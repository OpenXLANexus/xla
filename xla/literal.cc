/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/literal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/casts.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "xla/index_util.h"
#include "xla/permutation_util.h"
#include "xla/primitive_util.h"
#include "xla/shape_util.h"
#include "xla/status_macros.h"
#include "xla/types.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/float8.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/mem.h"
#include "tsl/util/byte_swap_array.h"

namespace xla {
namespace {

using absl::StrCat;

constexpr bool kLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
// Literals can be used as DMA targets, which can require alignment. We
// force a tsl::Allocator::kAllocatorAlignment-byte minimum
// alignment.
constexpr int kMinimumAlignment = 64;

// Converts between little and big endian.
//
// Precondition: size % 2 == 0 (elements in the array are 16 bits long)
void ConvertEndianShort(std::string* bytes) {
  CHECK_EQ(bytes->size() % 2, 0);
  for (int64_t i = 0, end = bytes->size(); i < end; i += 2) {
    std::swap((*bytes)[i], (*bytes)[i + 1]);
  }
}

void ConvertEndianShort(char* bytes, int64_t size) {
  CHECK_EQ(size % 2, 0);
  for (int64_t i = 0; i < size; i += 2) {
    std::swap(bytes[i], bytes[i + 1]);
  }
}

std::string CompactOneline(const std::string& input) {
  std::string result;
  std::vector<std::string> v = absl::StrSplit(input, absl::ByAnyChar("\n "));
  bool first = true;
  // Concatenate elements in "v" with spaces separating them, but ignoring
  // empty entries.
  for (const auto& s : v) {
    if (s.empty()) {
      continue;
    }
    absl::StrAppend(&result, (first ? "" : " "), s);
    first = false;
  }
  return result;
}

bool LiteralProtoHasValues(const LiteralProto& proto) {
  return proto.preds_size() || !proto.s8s().empty() || !proto.u8s().empty() ||
         proto.s32s_size() || proto.s64s_size() || proto.u32s_size() ||
         proto.u64s_size() || proto.f32s_size() || proto.f64s_size() ||
         proto.c64s_size() || proto.c128s_size() ||
         proto.tuple_literals_size() || !proto.f16s().empty() ||
         !proto.bf16s().empty() || !proto.u16s().empty() ||
         !proto.s16s().empty() || !proto.f8e5m2s().empty() ||
         !proto.f8e4m3fns().empty();
}

// Lazy getter for the interned scalar shape in static storage. We reuse this
// shape pointer to when constructing scalar Literals, which can happen a lot
// when we are evaluating reduce-like ops in HloEvalutator, and copying the
// shape over and over again significantly slows down the evaluator.
template <PrimitiveType kType>
const Shape& ScalarShapeImpl() {
  static_assert(primitive_util::IsArrayType(kType),
                "Not a valid type for a scalar.");
  static const Shape* shape = [] {
    auto shape = new Shape(kType, {}, {}, {});
    shape->mutable_layout();
    return shape;
  }();
  return *shape;
}

const Shape& ScalarShape(PrimitiveType type) {
  return primitive_util::PrimitiveTypeSwitch<const Shape&>(
      [&](auto primitive_type_constant) -> const Shape& {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          return ScalarShapeImpl<primitive_type_constant>();
        }
        LOG(FATAL) << PrimitiveType_Name(primitive_type_constant())
                   << " element type cannot be a scalar type.";
      },
      type);
}

const Shape& NilShape() {
  static const Shape* shape = new Shape(TUPLE, {}, {}, {});
  return *shape;
}

// Returns the interned shape pointer in static storage if it's a scalar shape
// or nil shape.
const Shape* TryInternShape(const Shape& shape) {
  if (shape.IsTuple() && shape.tuple_shapes_size() == 0) {
    return &NilShape();
  }
  if (shape.IsArray() && shape.dimensions_size() == 0 && shape.is_static() &&
      shape.layout().tiles_size() == 0 && shape.layout().memory_space() == 0) {
    return &ScalarShape(shape.element_type());
  }
  return nullptr;
}

}  // namespace

LiteralBase::~LiteralBase() = default;

const Shape& LiteralBase::shape() const { return root_piece().subshape(); }

const char* LiteralBase::Piece::buffer() const {
  return std::visit(BufferVisitor{}, rep_);
}

const LiteralBase::Piece& LiteralBase::piece(
    const ShapeIndex& shape_index) const {
  Piece* piece = &const_cast<Piece&>(root_piece());
  for (const auto i : shape_index) {
    DCHECK_GE(i, 0);
    DCHECK_LT(i, piece->children_size());
    piece = &piece->child(i);
  }
  return *piece;
}

std::ostream& operator<<(std::ostream& out, const Literal& literal) {
  out << literal.ToString();
  return out;
}

MutableLiteralBase::StrideConfig::StrideConfig(
    const Shape& source_shape, const Shape& dest_shape,
    absl::Span<const int64_t> dimensions)
    : dimensions(dimensions),
      base(dimensions.size(), 0),
      step(dimensions.size(), 1) {
  if (!dimensions.empty()) {
    // Selects the shape with the largest minor dimension as the one upon
    // which to run the tight stride loop.
    if (dimensions[LayoutUtil::Minor(source_shape.layout(), 0)] >=
        dimensions[LayoutUtil::Minor(dest_shape.layout(), 0)]) {
      minor_dimension = LayoutUtil::Minor(source_shape.layout(), 0);
      dest_stride = IndexUtil::GetDimensionStride(dest_shape, minor_dimension);
    } else {
      minor_dimension = LayoutUtil::Minor(dest_shape.layout(), 0);
      source_stride =
          IndexUtil::GetDimensionStride(source_shape, minor_dimension);
    }
    minor_loop_size = dimensions[minor_dimension];
    step[minor_dimension] = minor_loop_size;
  }
}

Shape* MutableLiteralBase::mutable_shape_do_not_use() {
  const Shape* const_shape = shape_.get();
  Shape* shape = shape_.get_mutable(/*ensure_owned=*/true);
  if (shape != const_shape) {
    std::function<void(const Shape&, Piece*)> set_piece_shapes =
        [&set_piece_shapes](const Shape& shape, Piece* piece) {
          piece->set_subshape(&shape);
          if (shape.IsTuple()) {
            for (int i = 0; i < ShapeUtil::TupleElementCount(shape); ++i) {
              const Shape& subshape = shape.tuple_shapes(i);
              set_piece_shapes(subshape, &piece->child(i));
            }
          }
        };
    set_piece_shapes(*shape, &mutable_root_piece());
  }
  return shape;
}

Literal::Literal() : Literal(NilShape()) {}

Literal::Literal(const Shape& shape)
    : Literal(shape, /*allocate_arrays=*/true) {}

void Literal::SetPiece(const Shape& shape, Piece* piece, bool allocate_arrays,
                       ArrayValueState leaf_array_value_state) {
  if (shape.IsTuple()) {
    for (const Shape& subshape : shape.tuple_shapes()) {
      auto child_piece = Piece();
      child_piece.set_subshape(&subshape);

      SetPiece(subshape, &child_piece, allocate_arrays, leaf_array_value_state);

      piece->emplace_back(std::move(child_piece));
    }
  } else if (shape.IsArray()) {
    DCHECK(LayoutUtil::IsDenseArray(shape))
        << "literal array storage is currently only supported for dense "
           "arrays: "
        << shape;
    piece->set_array_value_state(leaf_array_value_state);
    if (leaf_array_value_state == LiteralBase::ArrayValueState::kKnown &&
        allocate_arrays) {
      piece->AllocateBuffers();
    }
  }
}

Literal::Literal(const Shape& shape, bool allocate_arrays,
                 ArrayValueState leaf_array_value_state)
    : MutableLiteralBase() {
  if (const Shape* intered_shape_ptr = TryInternShape(shape)) {
    shape_ = intered_shape_ptr;
  } else {
    shape_ = std::make_unique<Shape>(shape);
  }
  CHECK(leaf_array_value_state != ArrayValueState::kKnown ||
        LayoutUtil::HasLayout(*shape_));
  root_piece_.set_subshape(shape_.get());
  CHECK(&root_piece_.subshape() == shape_.get());

  SetPiece(*shape_, &root_piece_, allocate_arrays, leaf_array_value_state);
}

Literal::~Literal() { DeallocateBuffers(); }

void Literal::DeallocateBuffers() {
  root_piece_.ForEachMutableSubpiece(
      [&](const ShapeIndex& index, Piece* piece) {
        piece->DeallocateBuffers();
      });
}

Literal::Literal(Literal&& other) : MutableLiteralBase() {
  *this = std::move(other);
}

Literal& Literal::operator=(Literal&& other) {
  DCHECK(&other.root_piece_.subshape() == other.shape_.get());
  using std::swap;
  swap(shape_, other.shape_);
  swap(root_piece_, other.root_piece_);
  DCHECK(&root_piece_.subshape() == shape_.get());

  return *this;
}

Literal LiteralBase::CreateFromShape(const Shape& shape) {
  Literal literal(shape);
  literal.root_piece_.ForEachMutableSubpiece(
      [&](const ShapeIndex& index, Piece* piece) {
        if (piece->subshape().IsArray()) {
          memset(piece->untyped_data(), 0, piece->size_bytes_dense());
        }
      });
  return literal;
}

Literal LiteralBase::CreateFromShapeWithUnknownLeafArrays(const Shape& shape) {
  Literal literal(shape, /*allocate_arrays=*/false, ArrayValueState::kUnknown);
  return literal;
}

Literal LiteralBase::CreateFromShapeWithUndeterminedLeafArrays(
    const Shape& shape) {
  Literal literal(shape, /*allocate_arrays=*/false,
                  ArrayValueState::kUndetermined);
  return literal;
}

int32_t LiteralBase::GetDynamicSize(int64_t dim_index) const {
  return GetDynamicSize(dim_index, {});
}

int32_t LiteralBase::GetDynamicSize(int64_t dim_index,
                                    const ShapeIndex& shape_index) const {
  return piece(shape_index).GetDynamicSize(dim_index);
}

std::optional<int64_t> LiteralBase::GetFirstInteger() const {
  switch (shape().element_type()) {
    case U8:
      return GetFirstElement<uint8_t>();
    case U16:
      return GetFirstElement<uint16_t>();
    case U32:
      return GetFirstElement<uint32_t>();
    case U64: {
      int64_t v = GetFirstElement<uint64_t>();
      if (v < 0) {
        return std::nullopt;
      }
      return v;
    }
    case S8:
      return GetFirstElement<int8_t>();
    case S16:
      return GetFirstElement<int16_t>();
    case S32:
      return GetFirstElement<int32_t>();
    case S64:
      return GetFirstElement<int64_t>();
    default:
      return std::nullopt;
  }
}

template <typename NativeT>
Status MutableLiteralBase::CopySliceFromInternal(
    const LiteralBase& src_literal, absl::Span<const int64_t> src_base,
    absl::Span<const int64_t> dest_base, absl::Span<const int64_t> copy_size) {
  const int64_t src_base_size = src_base.size();
  const int64_t dest_base_size = dest_base.size();
  TF_RET_CHECK(LayoutUtil::IsDenseArray(shape()));
  TF_RET_CHECK(src_literal.shape().rank() == src_base_size);
  TF_RET_CHECK(shape().rank() == dest_base_size);

  auto linear_index = [](const Shape& shape,
                         absl::Span<const int64_t> multi_index) {
    return IndexUtil::MultidimensionalIndexToLinearIndex(shape, multi_index);
  };

  if (src_literal.shape().rank() == 0 || shape().rank() == 0) {
    // If any of the two shapes are scalars, we can just call the StridedCopy()
    // directly, and we know we will be copying only one value.
    TF_RET_CHECK(copy_size.empty());
    StridedCopy(data<NativeT>(), linear_index(shape(), dest_base), 0,
                src_literal.data<NativeT>(),
                linear_index(src_literal.shape(), src_base), 0, 1);
  } else if (!ShapeUtil::IsZeroElementArray(shape()) &&
             !ShapeUtil::IsZeroElementArray(src_literal.shape())) {
    // Perform copy if neither src nor dest has dimensions with zero element,
    // otherwise it's a no-op.
    TF_RET_CHECK(src_base.size() == dest_base.size());
    TF_RET_CHECK(src_base.size() == copy_size.size());

    // Scan the source from minor, stepping in copy size blocks, then within
    // the index enumeration functor, do a strided copy advancing source index
    // by one (walking through the minor dimension), and destination index by
    // proper stride size at the matching dimension.
    DimensionVector src_indexes(src_base.size(), 0);
    DimensionVector dest_indexes(dest_base.size(), 0);
    MutableLiteralBase::StrideConfig stride_config(src_literal.shape(), shape(),
                                                   copy_size);

    auto copy_proc = [&](absl::Span<const int64_t> indexes) {
      // Map from multi-dimensional index, to source index.
      std::transform(indexes.begin(), indexes.end(), src_base.begin(),
                     src_indexes.begin(), std::plus<int64_t>());
      // Map from multi-dimensional index, to destination index.
      std::transform(indexes.begin(), indexes.end(), dest_base.begin(),
                     dest_indexes.begin(), std::plus<int64_t>());

      int64_t src_index = linear_index(src_literal.shape(), src_indexes);
      int64_t dest_index = linear_index(shape(), dest_indexes);

      // `this->` is needed to workaround MSVC bug: #16882
      StridedCopy(this->data<NativeT>(), dest_index, stride_config.dest_stride,
                  src_literal.data<NativeT>(), src_index,
                  stride_config.source_stride, stride_config.minor_loop_size);
      return true;
    };

    ShapeUtil::ForEachIndex(src_literal.shape(), stride_config.base,
                            stride_config.dimensions, stride_config.step,
                            copy_proc);
  }
  return OkStatus();
}

Status MutableLiteralBase::CopyElementFrom(
    const LiteralSlice& src_literal, absl::Span<const int64_t> src_index,
    absl::Span<const int64_t> dest_index) {
  DCHECK(LayoutUtil::IsDenseArray(shape()));
  DCHECK_EQ(shape().element_type(), src_literal.shape().element_type());
  const int64_t src_linear_index =
      IndexUtil::MultidimensionalIndexToLinearIndex(src_literal.shape(),
                                                    src_index);
  const int64_t dest_linear_index =
      IndexUtil::MultidimensionalIndexToLinearIndex(shape(), dest_index);
  const int64_t primitive_size =
      ShapeUtil::ByteSizeOfPrimitiveType(shape().element_type());

  char* dest_address =
      static_cast<char*>(untyped_data()) + dest_linear_index * primitive_size;
  const char* source_address =
      static_cast<const char*>(src_literal.untyped_data()) +
      src_linear_index * primitive_size;
  if (dest_address != source_address) {
    memcpy(dest_address, source_address, primitive_size);
  }
  return OkStatus();
}

/* static */ StatusOr<Literal> MutableLiteralBase::CreateFromProto(
    const LiteralProto& proto, bool prohibit_empty_literal) {
  if (!proto.has_shape()) {
    return InvalidArgument("LiteralProto has no shape");
  }
  Shape shape(proto.shape());
  if (ShapeUtil::HasPrimitiveType(shape, OPAQUE_TYPE)) {
    return InvalidArgument(
        "Literal shape cannot include OPAQUE_TYPE sub-shape");
  }
  if (!LayoutUtil::HasLayout(shape)) {
    return InvalidArgument("LiteralProto has no layout");
  }
  if (LayoutUtil::IsSparseArray(shape)) {
    return Unimplemented("Sparse literals are not supported");
  }

  TF_RETURN_IF_ERROR(ShapeUtil::ValidateShapeWithOptionalLayout(shape));

  Literal literal(shape);

  TF_RETURN_IF_ERROR(literal.root_piece_.ForEachMutableSubpieceWithStatus(
      [&](const ShapeIndex& index, Piece* piece) {
        const LiteralProto* proto_element = &proto;
        for (int64_t i : index) {
          CHECK(i < proto_element->tuple_literals_size());
          proto_element = &proto_element->tuple_literals(i);
        }

        if (piece->subshape().IsTuple()) {
          if (proto_element->tuple_literals_size() !=
              ShapeUtil::TupleElementCount(piece->subshape())) {
            return InvalidArgument(
                "Expected %d tuple elements in LiteralProto, has %d",
                ShapeUtil::TupleElementCount(piece->subshape()),
                proto_element->tuple_literals_size());
          }
          return OkStatus();
        }
        if (piece->subshape().element_type() == TOKEN) {
          return OkStatus();
        }

        CHECK(piece->subshape().IsArray());

        // When prohibit_empty_literal is false (allowing literal with no
        // values), only copy from proto if the literal proto has values. This
        // mode is used for a learned cost model.
        if (prohibit_empty_literal || LiteralProtoHasValues(*proto_element)) {
          TF_RETURN_IF_ERROR(piece->CopyFromProto(*proto_element));
        }

        return OkStatus();
      }));

  return std::move(literal);
}

Literal Literal::SubLiteral(ShapeIndexView shape_index) {
  if (!shape_index.empty()) {
    auto decomposed = this->DecomposeTuple();
    return decomposed.at(shape_index.front())
        .SubLiteral(shape_index.subspan(1));
  } else {
    return std::move(*this);
  }
}

std::vector<Literal> Literal::DecomposeTuple() {
  CHECK(shape().IsTuple());
  std::vector<Literal> elements;
  const auto tuple_element_count = ShapeUtil::TupleElementCount(shape());
  elements.reserve(tuple_element_count);
  for (int i = 0; i < tuple_element_count; ++i) {
    elements.push_back(Literal(ShapeUtil::GetSubshape(shape(), {i}),
                               /*allocate_arrays=*/false));
    Literal& element = elements.back();
    element.root_piece_.ForEachMutableSubpiece(
        [&](const ShapeIndex& index, Piece* dest_piece) {
          if (dest_piece->subshape().IsTuple()) {
            return;
          }
          ShapeIndex src_index = {i};
          for (int64_t j : index) {
            src_index.push_back(j);
          }
          Piece& src_piece = piece(src_index);

          // Move the respective buffer over to the element Literal.
          dest_piece->MoveDataFrom(src_piece);
        });
  }
  // Set this literal to be nil-shaped.
  *this = Literal();
  return elements;
}

namespace {

// Copies the elements in 'src' to 'dest'. The shape and layout of the data in
// the array slices are indicated by dest_shape and src_shape respectively.
template <typename NativeT>
void CopyElementsBetween(absl::Span<NativeT> dest,
                         absl::Span<const NativeT> src, const Shape& dest_shape,
                         const Shape& src_shape) {
  CHECK(LayoutUtil::IsDenseArray(dest_shape));
  CHECK(LayoutUtil::IsDenseArray(src_shape));
  CHECK(ShapeUtil::Compatible(dest_shape, src_shape));
  if (ShapeUtil::IsZeroElementArray(dest_shape)) {
    return;
  }
  std::vector<int64_t> index(dest_shape.rank());
  do {
    dest[IndexUtil::MultidimensionalIndexToLinearIndex(dest_shape, index)] =
        src[IndexUtil::MultidimensionalIndexToLinearIndex(src_shape, index)];
  } while (IndexUtil::BumpIndices(dest_shape, absl::MakeSpan(index)));
}
}  // namespace

int32_t LiteralBase::Piece::GetDynamicSize(int64_t dim_index) const {
  CHECK(LayoutUtil::IsDenseArray(subshape()));
  if (!subshape_->is_dynamic_dimension(dim_index)) {
    // This is a static dimension, return size.
    return subshape_->dimensions(dim_index);
  }
  return dynamic_size_buffer()[dim_index];
}

void LiteralBase::Piece::SetDynamicSize(int64_t dim_index, int32_t size) {
  CHECK(LayoutUtil::IsDenseArray(subshape()));
  CHECK(subshape_->is_dynamic_dimension(dim_index));
  dynamic_size_buffer()[dim_index] = size;
}

void LiteralBase::Piece::AllocateBuffers() {
  const int64_t bytes = total_bytes_dense();
  if (bytes > kMaxInlinedBytes) {
    CHECK_EQ(buffer(), nullptr);
    rep_.emplace<DenseRep>();
    set_buffer(
        static_cast<char*>(tsl::port::AlignedMalloc(bytes, kMinimumAlignment)));
  } else {
    rep_.emplace<DenseInlinedRep>();
  }
}

void LiteralBase::Piece::DeallocateBuffers() {
  if (auto* array_rep = GetDenseRep()) {
    tsl::port::AlignedFree(array_rep->data);
    rep_.emplace<Uninitialized>();
  }
}

Status LiteralBase::Piece::CopyFrom(const LiteralBase::Piece& src,
                                    bool only_dynamic_bound) {
  CHECK(subshape_ != nullptr);
  CHECK(src.subshape_ != nullptr);
  CHECK(LayoutUtil::IsDenseArray(subshape()))
      << __func__ << " is only supported for dense arrays: " << subshape();
  CHECK(LayoutUtil::IsDenseArray(src.subshape()))
      << __func__ << " is only supported for dense arrays: " << src.subshape();
  if (src.array_value_state_ == ArrayValueState::kUnknown ||
      src.array_value_state_ == ArrayValueState::kUndetermined) {
    if (array_value_state_ == ArrayValueState::kKnown) {
      DeallocateBuffers();
    }
    array_value_state_ = src.array_value_state_;
    return OkStatus();
  } else {
    CHECK(src.array_value_state_ == ArrayValueState::kKnown);
    if (array_value_state_ == ArrayValueState::kUndetermined ||
        array_value_state_ == ArrayValueState::kUnknown) {
      AllocateBuffers();
    }
    array_value_state_ = src.array_value_state_;
  }

  if (ShapeUtil::Equal(subshape(), src.subshape())) {
    // If the layouts are equal it's faster just to memcpy.
    memcpy(buffer(), src.buffer(), src.size_bytes_dense());
  } else {
    std::vector<int64_t> origin(subshape().rank(), 0);
    switch (subshape().element_type()) {
#define COPY_ELEMENTS(XLA_T, NATIVE_T)                                      \
  case (XLA_T):                                                             \
    if (only_dynamic_bound) {                                               \
      CopyElementsWithDynamicBound<NATIVE_T>(src);                          \
    } else {                                                                \
      CopyElementsBetween<NATIVE_T>(data<NATIVE_T>(), src.data<NATIVE_T>(), \
                                    subshape(), src.subshape());            \
    }                                                                       \
    break;
      COPY_ELEMENTS(U8, uint8_t);
      COPY_ELEMENTS(U16, uint16_t);
      COPY_ELEMENTS(U32, uint32_t);
      COPY_ELEMENTS(U64, uint64_t);
      COPY_ELEMENTS(S8, int8_t);
      COPY_ELEMENTS(S16, int16_t);
      COPY_ELEMENTS(S32, int32_t);
      COPY_ELEMENTS(S64, int64_t);
      COPY_ELEMENTS(F8E5M2, tsl::float8_e5m2);
      COPY_ELEMENTS(F8E4M3FN, tsl::float8_e4m3fn);
      COPY_ELEMENTS(F16, half);
      COPY_ELEMENTS(BF16, bfloat16);
      COPY_ELEMENTS(F32, float);
      COPY_ELEMENTS(F64, double);
      COPY_ELEMENTS(C64, complex64);
      COPY_ELEMENTS(C128, complex128);
      COPY_ELEMENTS(PRED, bool);
#undef COPY_ELEMENTS
      default:
        return Unimplemented(
            "Copying a Literal object with element type %s is not implemented.",
            PrimitiveType_Name(subshape().element_type()));
    }
  }
  DCHECK_EQ(dynamic_size_buffer_bytes(), src.dynamic_size_buffer_bytes());
  if (subshape().is_dynamic() && src.subshape().is_dynamic()) {
    memcpy(dynamic_size_buffer(), src.dynamic_size_buffer(),
           src.dynamic_size_buffer_bytes());
  }
  return OkStatus();
}

void MutableLiteralBase::SetDynamicSize(int64_t dim_index, int32_t size) {
  return SetDynamicSize(dim_index, {}, size);
}

void MutableLiteralBase::SetDynamicSize(int64_t dim_index,
                                        const ShapeIndex& shape_index,
                                        int32_t size) {
  Shape* subshape =
      ShapeUtil::GetMutableSubshape(mutable_shape_do_not_use(), shape_index);
  CHECK(LayoutUtil::IsDenseArray(*subshape))
      << __func__ << " is only supported for dense arrays: " << *subshape;
  CHECK_GE(subshape->dimensions(dim_index), size);
  subshape->set_dynamic_dimension(dim_index, true);
  CHECK_EQ(&piece(shape_index).subshape(), subshape);

  piece(shape_index).SetDynamicSize(dim_index, size);
}

Status MutableLiteralBase::CopyFrom(const LiteralSlice& src_literal,
                                    const ShapeIndex& dest_shape_index,
                                    const ShapeIndex& src_shape_index,
                                    bool only_dynamic_bound) {
  const Shape& dest_subshape =
      ShapeUtil::GetSubshape(shape(), dest_shape_index);
  const Shape& src_subshape =
      ShapeUtil::GetSubshape(src_literal.shape(), src_shape_index);
  if (only_dynamic_bound) {
    auto bound_shape = dest_subshape.is_static() ? src_subshape : dest_subshape;
    auto compact_shape =
        dest_subshape.is_static() ? dest_subshape : src_subshape;
    CHECK(ShapeUtil::DynamicShapeIsCompatible(compact_shape, bound_shape))
        << compact_shape.ToString() << " vs " << bound_shape.ToString();
  } else {
    if (!ShapeUtil::Compatible(dest_subshape, src_subshape)) {
      return InvalidArgument(
          "Destination subshape incompatible with source subshape: %s vs %s",
          ShapeUtil::HumanString(dest_subshape),
          ShapeUtil::HumanString(src_subshape));
    }
  }
  return mutable_root_piece().ForEachMutableSubpieceWithStatus(
      [&](const ShapeIndex& index, Piece* piece) {
        if (!piece->subshape().IsArray()) {
          return OkStatus();
        }

        // Determine if this index is in the part of this literal that we want
        // to copy over from src_literal.
        bool in_subtree_to_copy = true;
        for (int i = 0; i < dest_shape_index.size(); ++i) {
          if (index[i] != dest_shape_index[i]) {
            in_subtree_to_copy = false;
            break;
          }
        }
        if (!in_subtree_to_copy) {
          return OkStatus();
        }
        // Construct the index of the corresponding piece in the source literal.
        ShapeIndex src_piece_index = src_shape_index;
        for (int64_t i = dest_shape_index.size(), end = index.size(); i < end;
             ++i) {
          src_piece_index.push_back(index[i]);
        }
        TF_RETURN_IF_ERROR(
            piece->CopyFrom(src_literal.piece(src_piece_index),
                            /*only_dynamic_bound=*/only_dynamic_bound));
        return OkStatus();
      });
}

Status Literal::MoveFrom(Literal&& src_literal,
                         const ShapeIndex& dest_shape_index) {
  const Shape& dest_subshape =
      ShapeUtil::GetSubshape(shape(), dest_shape_index);
  if (!ShapeUtil::Equal(dest_subshape, src_literal.shape())) {
    return InvalidArgument(
        "Destination subshape not equal to source shape: %s vs %s",
        ShapeUtil::HumanString(dest_subshape),
        ShapeUtil::HumanString(src_literal.shape()));
  }

  src_literal.root_piece_.ForEachMutableSubpiece(
      [&](const ShapeIndex& src_index, Piece* src_piece) {
        if (!src_piece->subshape().IsArray()) {
          return;
        }

        ShapeIndex dest_index = dest_shape_index;
        for (int64_t i : src_index) {
          dest_index.push_back(i);
        }
        Piece& dest_piece = piece(dest_index);
        dest_piece.DeallocateBuffers();
        dest_piece.MoveDataFrom(*src_piece);
      });

  src_literal.shape_ = MaybeOwningShapePtr(&NilShape());
  src_literal.root_piece_ = Piece();
  src_literal.root_piece_.set_subshape(src_literal.shape_.get());

  return OkStatus();
}

Status MutableLiteralBase::CopySliceFrom(const LiteralSlice& src_literal,
                                         absl::Span<const int64_t> src_base,
                                         absl::Span<const int64_t> dest_base,
                                         absl::Span<const int64_t> copy_size) {
  TF_RET_CHECK(shape().IsArray()) << ShapeUtil::HumanString(shape());
  TF_RET_CHECK(src_literal.shape().IsArray())
      << ShapeUtil::HumanString(src_literal.shape());
  TF_RET_CHECK(ShapeUtil::SameElementType(src_literal.shape(), shape()));

  return primitive_util::PrimitiveTypeSwitch<Status>(
      [&](auto primitive_type_constant) -> Status {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return CopySliceFromInternal<NativeT>(src_literal, src_base,
                                                dest_base, copy_size);
        }
        return Unimplemented(
            "Copying a slice from a Literal object with element type %d is not "
            "implemented.",
            shape().element_type());
      },
      shape().element_type());
}

void MutableLiteralBase::PopulateR1(const tsl::core::Bitmap& values) {
  CHECK(shape().IsArray());
  CHECK_EQ(shape().rank(), 1);
  CHECK_EQ(element_count(), values.bits());
  CHECK_EQ(shape().element_type(), PRED);
  for (int64_t i = 0; i < static_cast<int64_t>(values.bits()); ++i) {
    Set({i}, values.get(i));
  }
}

Literal LiteralBase::Relayout(const Layout& new_layout,
                              const ShapeIndex& shape_index) const {
  // Create new shape with 'new_layout' set at the given shape index.
  Shape new_shape = shape();
  Shape* subshape = ShapeUtil::GetMutableSubshape(&new_shape, shape_index);
  TF_CHECK_OK(LayoutUtil::ValidateLayoutForShape(new_layout, *subshape));
  *subshape->mutable_layout() = new_layout;
  Literal result(new_shape);
  TF_CHECK_OK(result.CopyFrom(*this));
  return result;
}

Literal LiteralBase::Relayout(const Shape& shape_with_layout) const {
  CHECK(ShapeUtil::Compatible(shape_with_layout, shape()))
      << "Given shape_with_layout " << ShapeUtil::HumanString(shape_with_layout)
      << " not compatible with literal shape "
      << ShapeUtil::HumanString(shape());
  Literal result = CreateFromShape(shape_with_layout);
  ShapeUtil::ForEachSubshape(
      result.shape(),
      [this, &result](const Shape& subshape, const ShapeIndex& index) {
        if (subshape.IsArray()) {
          TF_CHECK_OK(result.CopyFrom(*this,
                                      /*dest_shape_index=*/index,
                                      /*src_shape_index=*/index));
        }
      });
  return result;
}

Literal LiteralBase::ToBoundedDynamic(const Shape& bounded_shape) const {
  CHECK(bounded_shape.is_dynamic());
  Literal result(bounded_shape);
  ShapeUtil::ForEachSubshape(
      shape(), [&](const Shape& subshape, const ShapeIndex& index) {
        if (!subshape.IsArray()) {
          return;
        }
        for (int64_t i = 0; i < subshape.rank(); ++i) {
          if (bounded_shape.is_dynamic_dimension(i)) {
            result.SetDynamicSize(i, subshape.dimensions(i));
          }
        }
      });
  TF_CHECK_OK(result.CopyFrom(*this, {}, {}, /*only_dynamic_bound=*/true));

  return result;
}

Literal LiteralBase::ToStatic() const {
  // Create new shape with 'new_layout' set at the given shape index.
  Shape new_shape = shape();
  ShapeUtil::ForEachMutableSubshape(
      &new_shape, [this](Shape* subshape, const ShapeIndex& index) {
        if (!subshape->IsArray()) {
          return;
        }
        for (int64_t i = 0; i < subshape->rank(); ++i) {
          // GetDynamicSize has a 32-bit return type and may truncate static
          // dimensions, so make sure to skip.
          if (!subshape->is_dynamic_dimension(i)) continue;
          subshape->set_dynamic_dimension(i, false);
          subshape->set_dimensions(i, GetDynamicSize(i, index));
        }
      });
  Literal result(new_shape);
  TF_CHECK_OK(result.CopyFrom(*this, {}, {}, /*only_dynamic_bound=*/true));
  return result;
}

StatusOr<Literal> LiteralBase::Broadcast(
    const Shape& result_shape, absl::Span<const int64_t> dimensions) const {
  if (!shape().IsArray()) {
    return InvalidArgument("Broadcast only supports arrays.");
  }

  for (int64_t i = 0, end = dimensions.size(); i < end; i++) {
    TF_RET_CHECK(shape().dimensions(i) ==
                 result_shape.dimensions(dimensions[i]));
  }

  TF_RET_CHECK(result_shape.element_type() == shape().element_type());
  Literal result(result_shape);
  // scratch_source_index is temporary storage space for the computed index into
  // the input literal.  We put it here to avoid allocating an std::vector in
  // every iteration of ShapeUtil::ForEachIndex.
  std::vector<int64_t> scratch_source_index(shape().dimensions_size());

  char* dest_data = static_cast<char*>(result.untyped_data());
  const char* source_data = static_cast<const char*>(untyped_data());
  const int64_t primitive_size =
      ShapeUtil::ByteSizeOfPrimitiveType(shape().element_type());
  if (shape().is_dynamic()) {
    for (int64_t i = 0; i < dimensions.size(); ++i) {
      if (shape().is_dynamic_dimension(i)) {
        // Set any dynamic sizes in the new literal.
        int64_t dynamic_size = GetDynamicSize(i);
        result.SetDynamicSize(dimensions[i], dynamic_size);
      }
    }
  }

  ShapeUtil::ForEachIndex(
      result_shape, [&](absl::Span<const int64_t> output_index) {
        for (int64_t i = 0, end = dimensions.size(); i < end; ++i) {
          scratch_source_index[i] = output_index[dimensions[i]];
        }
        int64_t dest_index = IndexUtil::MultidimensionalIndexToLinearIndex(
            result_shape, output_index);
        int64_t source_index = IndexUtil::MultidimensionalIndexToLinearIndex(
            shape(), scratch_source_index);
        memcpy(dest_data + primitive_size * dest_index,
               source_data + primitive_size * source_index, primitive_size);
        return true;
      });

  return std::move(result);
}

StatusOr<Literal> LiteralBase::Reshape(
    absl::Span<const int64_t> dimensions) const {
  if (!LayoutUtil::IsDenseArray(shape())) {
    return InvalidArgument("Reshape is only supported for dense arrays.");
  }
  if (shape().is_dynamic()) {
    // TODO(b/243182930): We should consider supporting dynamic reshape.
    return Unimplemented("Dynamic reshape is not implemented.");
  }
  Literal output;
  if (!LayoutUtil::IsMonotonicWithDim0Major(shape().layout())) {
    output = Relayout(LayoutUtil::GetDefaultLayoutForRank(shape().rank()));
  } else {
    output = Clone();
  }
  // Because the layout is monotonic, we can simply reuse the same sequence of
  // values without changing their order.
  *output.mutable_shape_do_not_use() =
      ShapeUtil::MakeShape(shape().element_type(), dimensions);

  int64_t elements_before = ShapeUtil::ElementsIn(shape());
  int64_t elements_after = ShapeUtil::ElementsIn(output.shape());
  if (elements_before != elements_after) {
    return InvalidArgument(
        "Shapes before and after Literal::Reshape have different numbers "
        "of elements: %s vs %s.",
        ShapeUtil::HumanString(shape()),
        ShapeUtil::HumanString(output.shape()));
  }
  return std::move(output);
}

Literal LiteralBase::Transpose(absl::Span<const int64_t> permutation) const {
  CHECK(LayoutUtil::IsDenseArray(shape()))
      << __func__ << " is only supported for dense arrays: " << shape();
  CHECK(shape().rank() == permutation.size() && IsPermutation(permutation))
      << "Given permutation is not a permutation of dimension numbers";
  // To transpose the array, we just permute the dimensions and layout, and
  // do a straight memory copy of the raw data set.
  // This is considerably faster than iterating over every array element using
  // the EachCell<>() and Set<>() APIs.
  Shape permuted_shape = ShapeUtil::PermuteDimensions(permutation, shape());
  // Replace the layout with one affine to this shape, such that a
  // transpose operation can be performed by leaving the flat values
  // representation intact.
  // For example, consider the shape F32[11,8]{1,0} under a {1,0} permutation.
  // The shape with affine layout resulting from that operation will be
  // F32[8,11]{0,1}, since it leaves the original most minor (the 8 sized), the
  // most minor.
  //
  // Essentially, given MinMaj(Di) the position of the Di dimension within the
  // minor to major vector, and given T(Di) the index that the original Di
  // dimension has within the transposed array, a layout is affine if
  // MinMaj(Di) == TMinMaj(T(Di)), with TMinMaj() being the minor to major
  // vector of the affine layout.
  std::vector<int64_t> inverse_permutation = InversePermutation(permutation);
  CHECK(LayoutUtil::IsDenseArray(permuted_shape));
  Layout* layout = permuted_shape.mutable_layout();
  layout->clear_minor_to_major();
  for (auto index : LayoutUtil::MinorToMajor(shape())) {
    layout->add_minor_to_major(inverse_permutation[index]);
  }
  Literal new_literal(permuted_shape);
  if (shape().is_dynamic()) {
    for (int64_t i = 0; i < shape().rank(); i++) {
      if (shape().is_dynamic_dimension(i)) {
        // Set the dynamic size of any dynamic dimension in the transposed
        // literal.
        new_literal.SetDynamicSize(inverse_permutation[i], GetDynamicSize(i));
      }
    }
  }
  DCHECK_EQ(ShapeUtil::ByteSizeOf(new_literal.shape()),
            ShapeUtil::ByteSizeOf(shape()));
  std::memcpy(new_literal.untyped_data(), untyped_data(), size_bytes());
  return new_literal;
}

template <typename NativeT>
Literal LiteralBase::SliceInternal(
    const Shape& result_shape, absl::Span<const int64_t> start_indices) const {
  Literal result_literal(result_shape);
  DimensionVector new_indices(result_shape.rank());
  CHECK(result_literal
            .Populate<NativeT>([&](absl::Span<const int64_t> indices) {
              for (int64_t i = 0; i < result_shape.rank(); ++i) {
                new_indices[i] = indices[i] + start_indices[i];
              }
              return Get<NativeT>(new_indices);
            })
            .ok());
  for (int64_t dnum = 0; dnum < shape().rank(); ++dnum) {
    if (shape().is_dynamic_dimension(dnum)) {
      int64_t dynamic_size = GetDynamicSize(dnum) - start_indices[dnum];
      CHECK_GE(dynamic_size, 0) << GetDynamicSize(dnum);
      dynamic_size = std::min(dynamic_size, result_shape.dimensions(dnum));
      result_literal.SetDynamicSize(dnum, dynamic_size);
    }
  }
  return result_literal;
}

Literal LiteralBase::Slice(absl::Span<const int64_t> start_indices,
                           absl::Span<const int64_t> limit_indices) const {
  CHECK(shape().IsArray()) << "tuple is not supported for slice";

  DimensionVector result_dimensions;
  for (int64_t dnum = 0; dnum < shape().rank(); ++dnum) {
    CHECK_GE(start_indices[dnum], 0);
    CHECK_LE(limit_indices[dnum], shape().dimensions(dnum))
        << "dnum = " << dnum;
    int64_t dimension = limit_indices[dnum] - start_indices[dnum];
    CHECK_GE(dimension, 0) << "dnum = " << dnum;
    result_dimensions.push_back(dimension);
  }
  auto result_shape = ShapeUtil::MakeShapeWithDenseLayout(
      shape().element_type(), result_dimensions,
      LayoutUtil::MinorToMajor(shape()));
  ShapeUtil::CopyDynamicDimensions(&result_shape, shape());
  return primitive_util::PrimitiveTypeSwitch<Literal>(
      [&](auto primitive_type_constant) -> Literal {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return SliceInternal<NativeT>(result_shape, start_indices);
        }
        LOG(FATAL) << "not yet implemented: "
                   << PrimitiveType_Name(result_shape.element_type());
      },
      shape().element_type());
}

Literal LiteralBase::Clone() const {
  Literal result(shape());
  TF_CHECK_OK(result.CopyFrom(*this));
  return result;
}

std::unique_ptr<Literal> LiteralBase::CloneToUnique() const {
  auto result = std::make_unique<Literal>(shape());
  TF_CHECK_OK(result->CopyFrom(*this));
  return result;
}

bool LiteralBase::IsDetermined(const ShapeIndex& shape_index) const {
  return piece(shape_index).IsDetermined();
}

bool LiteralBase::IsKnown(const ShapeIndex& shape_index) const {
  return piece(shape_index).IsKnown();
}

std::string LiteralBase::GetAsString(absl::Span<const int64_t> multi_index,
                                     const ShapeIndex& shape_index) const {
  const Shape& subshape = ShapeUtil::GetSubshape(shape(), shape_index);
  CHECK(LayoutUtil::IsDenseArray(subshape));
  return primitive_util::PrimitiveTypeSwitch<std::string>(
      [&](auto primitive_type_constant) -> std::string {
        if constexpr (primitive_type_constant == PRED) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return Get<NativeT>(multi_index, shape_index) ? "true" : "false";
        }
        if constexpr (primitive_util::IsIntegralType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return StrCat(Get<NativeT>(multi_index, shape_index));
        }
        if constexpr (primitive_util::IsFloatingPointType(
                          primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return RoundTripFpToString(Get<NativeT>(multi_index, shape_index));
        }
        if constexpr (primitive_util::IsComplexType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          NativeT c = Get<NativeT>(multi_index, shape_index);
          return StrCat("(", RoundTripFpToString(c.real()), ", ",
                        RoundTripFpToString(c.imag()), ")");
        }
        LOG(FATAL) << PrimitiveType_Name(subshape.element_type());
      },
      subshape.element_type());
}

std::optional<int64_t> LiteralBase::GetIntegralAsS64(
    absl::Span<const int64_t> multi_index) const {
  CHECK(LayoutUtil::IsDenseArray(shape()));
  return primitive_util::PrimitiveTypeSwitch<std::optional<int64_t>>(
      [&](auto primitive_type_constant) -> std::optional<int64_t> {
        if constexpr (primitive_util::IsIntegralType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return static_cast<int64_t>(Get<NativeT>(multi_index));
        }
        return std::nullopt;
      },
      shape().element_type());
}

std::optional<double> LiteralBase::GetAsDouble(
    absl::Span<const int64_t> multi_index) const {
  CHECK(LayoutUtil::IsDenseArray(shape()));
  return primitive_util::PrimitiveTypeSwitch<std::optional<double>>(
      [&](auto primitive_type_constant) -> std::optional<double> {
        if constexpr (primitive_util::IsFloatingPointType(
                          primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return static_cast<double>(Get<NativeT>(multi_index));
        }
        return std::nullopt;
      },
      shape().element_type());
}

std::optional<complex128> LiteralBase::GetAsComplex128(
    absl::Span<const int64_t> multi_index) const {
  return primitive_util::PrimitiveTypeSwitch<std::optional<complex128>>(
      [&](auto primitive_type_constant) -> std::optional<complex128> {
        if constexpr (primitive_util::IsFloatingPointType(
                          primitive_type_constant) ||
                      primitive_type_constant == S8) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return {{static_cast<double>(Get<NativeT>(multi_index)), 0}};
        }
        if constexpr (primitive_util::IsComplexType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return {Get<NativeT>(multi_index)};
        }
        return std::nullopt;
      },
      shape().element_type());
}

Status MutableLiteralBase::SetIntegralAsS64(
    absl::Span<const int64_t> multi_index, int64_t value) {
  CHECK(LayoutUtil::IsDenseArray(shape()));
  return primitive_util::PrimitiveTypeSwitch<Status>(
      [&](auto primitive_type_constant) -> Status {
        if constexpr (primitive_util::IsIntegralType(primitive_type_constant) ||
                      primitive_type_constant == PRED) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          Set<NativeT>(multi_index, value);
          return OkStatus();
        }
        return FailedPrecondition("Array element type is not integral: %s",
                                  PrimitiveType_Name(shape().element_type()));
      },
      shape().element_type());
}

Status MutableLiteralBase::SetFromDouble(absl::Span<const int64_t> multi_index,
                                         double value) {
  CHECK(LayoutUtil::IsDenseArray(shape()));
  return primitive_util::PrimitiveTypeSwitch<Status>(
      [&](auto primitive_type_constant) -> Status {
        if constexpr (primitive_util::IsFloatingPointType(
                          primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          Set<NativeT>(multi_index, static_cast<NativeT>(value));
          return OkStatus();
        }
        return FailedPrecondition("Array element type is not floating: %s",
                                  PrimitiveType_Name(shape().element_type()));
      },
      shape().element_type());
}

namespace {

std::string ShapeToString(bool print_layout, const Shape& shape) {
  return print_layout ? ShapeUtil::HumanStringWithLayout(shape)
                      : ShapeUtil::HumanString(shape);
}

void ToStringHelper(const LiteralBase& literal, const ShapeIndex& shape_index,
                    bool print_shape, bool print_layout,
                    std::vector<std::string>* pieces);

void TupleToStringHelper(const LiteralBase& literal,
                         const ShapeIndex& shape_index, bool print_shape,
                         bool print_layout, std::vector<std::string>* pieces) {
  const Shape& subshape = ShapeUtil::GetSubshape(literal.shape(), shape_index);
  pieces->push_back("(\n");
  std::vector<std::string> tuple_pieces;
  const auto tuple_element_count = ShapeUtil::TupleElementCount(subshape);
  tuple_pieces.reserve(tuple_element_count);
  for (int i = 0; i < ShapeUtil::TupleElementCount(subshape); ++i) {
    ShapeIndex element_index = shape_index;
    element_index.push_back(i);
    std::vector<std::string> element_pieces;
    ToStringHelper(literal, element_index, print_shape, print_layout,
                   &element_pieces);
    tuple_pieces.push_back(absl::StrJoin(element_pieces, ""));
  }
  pieces->push_back(absl::StrJoin(tuple_pieces, ",\n"));
  pieces->push_back("\n)");
}

void DenseArrayToStringHelper(const LiteralBase& literal,
                              const ShapeIndex& shape_index, bool print_shape,
                              bool print_layout,
                              std::vector<std::string>* pieces) {
  const Shape& subshape = ShapeUtil::GetSubshape(literal.shape(), shape_index);
  int64_t rank = subshape.rank();

  std::function<void(absl::Span<const int64_t> dimensions,
                     std::vector<int64_t>*)>
      to_string_recursive = [&](absl::Span<const int64_t> dimensions,
                                std::vector<int64_t>* accum_indices) {
        // dimensions.size() decreases by 1 at each recursive call,
        // and accum_indices->size() increases by 1.
        // Their sum is equal to the rank of the tensor.
        CHECK_EQ(rank, dimensions.size() + accum_indices->size());

        auto brace_to_string = [&](std::string brace) -> std::string {
          // Handle 1D tensor
          if (rank == 1) {
            return brace;
          }
          // Handle the innermost tensor of a 2D+ tensor.
          if (dimensions.size() == 1 && brace == "{") {
            return StrCat("  ", brace, dimensions[0] <= 1 ? "" : " ");
          }
          if (dimensions.size() == 1 && brace == "}") {
            return StrCat(dimensions[0] <= 1 ? "" : " ", brace);
          }
          // Handle the non-innermost tensors of a 2D+ tensor.
          if (brace == "{") {
            const int64_t accum_indices_size = accum_indices->size();
            if (rank > 3 && !accum_indices->empty() &&
                accum_indices_size < rank) {
              int index = accum_indices->size() - 1;
              int value = accum_indices->back();
              return StrCat(brace, " /*i", index, "=", value, "*/\n");
            }
            return StrCat(brace, "\n");
          }
          return StrCat("\n", brace);
        };

        if (dimensions.empty()) {
          // Display predicates as 0s and 1s so that the string is more dense.
          std::string elem;
          if (subshape.element_type() == PRED && rank > 0) {
            elem = literal.Get<bool>(*accum_indices, shape_index) ? "1" : "0";
          } else {
            elem = literal.GetAsString(*accum_indices, shape_index);
          }
          pieces->push_back(elem);
        } else {
          pieces->push_back(brace_to_string("{"));
          for (int i = 0; i < dimensions[0]; ++i) {
            accum_indices->push_back(i);
            to_string_recursive(dimensions.subspan(1), accum_indices);
            accum_indices->pop_back();
            if (i < dimensions[0] - 1) {
              pieces->push_back(",");
              pieces->push_back(dimensions.size() > 1 ? "\n" : " ");
            }
          }
          pieces->push_back(brace_to_string("}"));
        }
      };

  if (print_shape) {
    pieces->push_back(ShapeToString(print_layout, subshape));
    if (subshape.is_dynamic()) {
      pieces->push_back("(");
      for (int64_t i = 0; i < subshape.dimensions_size(); ++i) {
        pieces->push_back(StrCat(literal.GetDynamicSize(i, shape_index)));
        if (i < subshape.dimensions_size() - 1) {
          pieces->push_back(",");
        }
      }
      pieces->push_back(")");
    }
    pieces->push_back(" ");
  }
  std::vector<int64_t> indices = {};
  std::vector<int64_t> dimensions;
  dimensions.reserve(subshape.rank());
  for (int64_t i = 0; i < subshape.rank(); ++i) {
    dimensions.push_back(literal.GetDynamicSize(i, shape_index));
  }
  to_string_recursive(dimensions, &indices);
}

void ToStringHelper(const LiteralBase& literal, const ShapeIndex& shape_index,
                    bool print_shape, bool print_layout,
                    std::vector<std::string>* pieces) {
  const Shape& subshape = ShapeUtil::GetSubshape(literal.shape(), shape_index);
  CHECK(LayoutUtil::HasLayout(literal.shape()));
  CHECK(LayoutUtil::HasLayout(subshape));
  if (subshape.IsTuple()) {
    TupleToStringHelper(literal, shape_index, print_shape, print_layout,
                        pieces);
  } else if (subshape.IsToken()) {
    pieces->push_back("token");
  } else {
    CHECK(LayoutUtil::IsDenseArray(subshape));
    if (literal.IsKnown(shape_index)) {
      DenseArrayToStringHelper(literal, shape_index, print_shape, print_layout,
                               pieces);
    } else {
      pieces->push_back(ShapeToString(print_layout, subshape));
      pieces->push_back(" ");
      if (literal.IsDetermined(shape_index)) {
        pieces->push_back("unknown");
      } else {
        pieces->push_back("undetermined");
      }
    }
  }
}

}  // namespace

std::string LiteralBase::ToString() const {
  std::vector<std::string> pieces;
  CHECK(LayoutUtil::HasLayout(this->shape()));
  ToStringHelper(*this, {}, /*print_shape=*/true,
                 /*print_layout=*/false, &pieces);
  return absl::StrJoin(pieces, "");
}

std::string LiteralBase::ToStringOneline() const {
  return CompactOneline(ToString());
}

std::string LiteralBase::ToStringWithoutShape() const {
  std::vector<std::string> pieces;
  CHECK(LayoutUtil::HasLayout(this->shape()));
  ToStringHelper(*this, {}, /*print_shape=*/false,
                 /*print_layout=*/false, &pieces);
  return absl::StrJoin(pieces, "");
}

std::string LiteralBase::ToStringWithoutShapeOneline() const {
  return CompactOneline(ToStringWithoutShape());
}

std::string LiteralBase::ToStringWithLayout() const {
  std::vector<std::string> pieces;
  CHECK(LayoutUtil::HasLayout(this->shape()));
  ToStringHelper(*this, {}, /*print_shape=*/true,
                 /*print_layout=*/true, &pieces);
  return absl::StrJoin(pieces, "");
}

std::string LiteralBase::ToStringWithLayoutOneline() const {
  return CompactOneline(ToStringWithLayout());
}

void LiteralBase::EachCellAsString(
    absl::FunctionRef<void(absl::Span<const int64_t> indices,
                           const std::string& value)>
        per_cell) const {
  if (ShapeUtil::IsZeroElementArray(shape())) {
    return;
  }
  std::vector<int64_t> indices = IndexUtil::LinearIndexToMultidimensionalIndex(
      shape(), /*linear_index=*/0);
  do {
    per_cell(indices, GetAsString(indices));
  } while (IndexUtil::BumpIndices(shape(), absl::MakeSpan(indices)));
}

namespace {
template <typename NativeSrcT, typename NativeDestT, typename ConverterType>
Literal ConvertBetweenNativeTypesWithConverter(const LiteralBase& src_literal,
                                               const ConverterType& converter) {
  CHECK(src_literal.shape().IsArray());
  Literal result_literal(ShapeUtil::ChangeElementType(
      src_literal.shape(),
      primitive_util::NativeToPrimitiveType<NativeDestT>()));
  auto src_data = src_literal.data<NativeSrcT>();
  auto dest_data = result_literal.template data<NativeDestT>();
  int64_t num_elements = src_literal.element_count();

  for (int64_t i = 0; i < num_elements; ++i) {
    dest_data[i] = converter(src_data[i]);
  }
  return result_literal;
}

template <typename NativeSrcT, typename NativeDestT>
Literal ConvertBetweenNativeTypes(const LiteralBase& src_literal) {
  auto converter = [](NativeSrcT src) {
    if constexpr (std::is_same_v<NativeSrcT, NativeDestT>) {
      return src;
    }
    // C++ [conv.bool]p1:
    //   A prvalue of arithmetic [...] type can be converted to a prvalue of
    //   type bool. A zero value [...] is converted to false; any other value is
    //   converted to true.
    // C++ [conv.fpint]p1:
    //   [...] The behavior is undefined if the truncated value cannot be
    //   represented in the destination type.
    //
    // Using static_cast to convert a float to an integral type other than bool
    // may be undefined if the value's magnitude is too large or it is a NaN.
    // Let's choose saturating arithmetic as it captures the spirit of infinity
    // and arbitrarily map NaN to zero.
    if constexpr (!std::is_same_v<NativeDestT, bool> &&
                  !std::numeric_limits<NativeSrcT>::is_integer &&
                  std::numeric_limits<NativeDestT>::is_integer) {
      if (src != src) {
        return NativeDestT{0};
      }
      if (src >=
          static_cast<NativeSrcT>(std::numeric_limits<NativeDestT>::max())) {
        return std::numeric_limits<NativeDestT>::max();
      }
      if (src <=
          static_cast<NativeSrcT>(std::numeric_limits<NativeDestT>::lowest())) {
        return std::numeric_limits<NativeDestT>::lowest();
      }
    }
    return static_cast<NativeDestT>(src);
  };
  return ConvertBetweenNativeTypesWithConverter<NativeSrcT, NativeDestT>(
      src_literal, converter);
}

template <typename NativeSrcT, typename NativeDestT>
Literal BitcastBetweenNativeTypes(const LiteralBase& src_literal) {
  if constexpr (sizeof(NativeSrcT) == sizeof(NativeDestT)) {
    auto converter = [](NativeSrcT src) {
      return Eigen::numext::bit_cast<NativeDestT>(src);
    };
    return ConvertBetweenNativeTypesWithConverter<NativeSrcT, NativeDestT>(
        src_literal, converter);
  }
  // This template specialization is here to make the compiler happy. bit_cast
  // has a static check that the types are the same size. This specialization
  // should never be used because the source and destination types are checked
  // for identical sizes higher up.
  LOG(FATAL) << "Invalid bitcast between types of different sizes.";
}

template <PrimitiveType primitive_src_type, PrimitiveType primitive_dest_type>
Literal ConvertIfTypesMatch(const LiteralBase& src_literal, bool bitcast) {
  CHECK_EQ(primitive_src_type, src_literal.shape().element_type());
  if (bitcast) {
    return BitcastBetweenNativeTypes<
        typename primitive_util::PrimitiveTypeToNative<
            primitive_src_type>::type,
        typename primitive_util::PrimitiveTypeToNative<
            primitive_dest_type>::type>(src_literal);
  } else {
    return ConvertBetweenNativeTypes<
        typename primitive_util::PrimitiveTypeToNative<
            primitive_src_type>::type,
        typename primitive_util::PrimitiveTypeToNative<
            primitive_dest_type>::type>(src_literal);
  }
}

template <PrimitiveType primitive_src_type>
StatusOr<Literal> ConvertIfDestTypeMatches(const LiteralBase& src_literal,
                                           PrimitiveType primitive_dest_type,
                                           bool bitcast) {
  return primitive_util::PrimitiveTypeSwitch<StatusOr<Literal>>(
      [&](auto primitive_type_constant) -> StatusOr<Literal> {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          if (!primitive_util::IsComplexType(primitive_type_constant) ||
              !bitcast) {
            return ConvertIfTypesMatch<primitive_src_type,
                                       primitive_type_constant>(src_literal,
                                                                bitcast);
          }
        }
        return Unimplemented(
            "Converting from type %s to type %s is not implemented.",
            PrimitiveType_Name(src_literal.shape().element_type()),
            PrimitiveType_Name(primitive_dest_type));
      },
      primitive_dest_type);
}

StatusOr<Literal> ConvertSwitch(const LiteralBase& literal,
                                PrimitiveType primitive_dest_type,
                                bool bitcast) {
  TF_RET_CHECK(LayoutUtil::IsDenseArray(literal.shape()));
  if (literal.shape().element_type() == primitive_dest_type) {
    return literal.Clone();
  }
  return primitive_util::PrimitiveTypeSwitch<StatusOr<Literal>>(
      [&](auto primitive_type_constant) -> StatusOr<Literal> {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant) &&
                      !primitive_util::IsComplexType(primitive_type_constant)) {
          return ConvertIfDestTypeMatches<primitive_type_constant>(
              literal, primitive_dest_type, bitcast);
        }
        return Unimplemented("%s from type %s to type %s is not implemented.",
                             (bitcast ? "Bitcast converting" : "Converting"),
                             PrimitiveType_Name(literal.shape().element_type()),
                             PrimitiveType_Name(primitive_dest_type));
      },
      literal.shape().element_type());
}

}  // namespace

StatusOr<Literal> LiteralBase::Convert(
    PrimitiveType primitive_dest_type) const {
  return ConvertSwitch(*this, primitive_dest_type, /*bitcast=*/false);
}

StatusOr<Literal> LiteralBase::BitcastConvert(const Shape& dest_shape) const {
  if (ShapeUtil::ByteSizeOf(dest_shape) != ShapeUtil::ByteSizeOf(shape())) {
    return InvalidArgument(
        "Can not bitcast-convert from shape %s to a shape of different size %s",
        shape().ToString(), dest_shape.ToString());
  }
  if (dest_shape.IsTuple() || shape().IsTuple()) {
    return InvalidArgument(
        "bitcast-convert is not valid for tuple shapes %s->%s",
        shape().ToString(), dest_shape.ToString());
  }
  if (shape().is_dynamic() || dest_shape.is_dynamic()) {
    return InvalidArgument(
        "bitcast-convert is not valid for dynamic shape %s->%s",
        shape().ToString(), dest_shape.ToString());
  }

  Literal out(dest_shape);
  std::memcpy(out.root_piece_.buffer(), root_piece().buffer(),
              root_piece().size_bytes_dense());

  // Perform the reshape on little endian encoding even on big endian machines.
  if (!kLittleEndian) {
    // Swap byte ordering as per the input data type.
    size_t input_elem_size =
        ShapeUtil::ByteSizeOfPrimitiveType(shape().element_type());
    TF_RETURN_IF_ERROR(tsl::ByteSwapArray(
        const_cast<char*>(out.root_piece().buffer()), input_elem_size,
        out.root_piece().size_bytes_dense() / input_elem_size));
    // Swap byte ordering as per the output data type.
    size_t output_elem_size =
        ShapeUtil::ByteSizeOfPrimitiveType(dest_shape.element_type());
    TF_RETURN_IF_ERROR(tsl::ByteSwapArray(
        const_cast<char*>(out.root_piece().buffer()), output_elem_size,
        out.root_piece().size_bytes_dense() / output_elem_size));
  }

  return out;
}

StatusOr<Literal> LiteralBase::ConvertToShape(const Shape& dest_shape) const {
  if (!dest_shape.IsTuple()) {
    return Convert(dest_shape.element_type());
  }
  std::vector<Literal> elements;
  const auto tuple_element_count = ShapeUtil::TupleElementCount(shape());
  elements.reserve(tuple_element_count);
  for (int i = 0; i < tuple_element_count; ++i) {
    auto element = LiteralSlice(*this, {i});
    TF_ASSIGN_OR_RETURN(
        auto new_element,
        element.ConvertToShape(ShapeUtil::GetSubshape(dest_shape, {i})));
    elements.push_back(std::move(new_element));
  }
  return MutableLiteralBase::MoveIntoTuple(absl::MakeSpan(elements));
}

/* static */ Literal MutableLiteralBase::MoveIntoTuple(
    absl::Span<Literal> elements) {
  std::vector<const Shape*> element_shapes;
  element_shapes.reserve(elements.size());
  for (const Literal& element : elements) {
    element_shapes.push_back(&element.shape());
  }
  Literal literal(ShapeUtil::MakeTupleShapeWithPtrs(element_shapes),
                  /*allocate_arrays=*/false);
  for (int i = 0, end = elements.size(); i < end; ++i) {
    TF_CHECK_OK(
        literal.MoveFrom(std::move(elements[i]), /*dest_shape_index=*/{i}));
  }
  return literal;
}

template <typename NativeT>
void LiteralBase::Piece::CopyElementsWithDynamicBound(
    const LiteralBase::Piece& src) {
  auto dest_shape = subshape();
  auto src_shape = src.subshape();

  // At least one shape has to be static as bound.
  CHECK(dest_shape.is_static() || src_shape.is_static());
  auto bound_shape = dest_shape.is_static() ? src_shape : dest_shape;
  if (ShapeUtil::IsZeroElementArray(dest_shape)) {
    return;
  }
  std::vector<int64_t> index(dest_shape.rank());
  do {
    bool out_of_bound = false;
    for (int64_t i = 0; i < index.size(); ++i) {
      // Do not copy elements beyond dynamic bound.
      if (index[i] >= GetDynamicSize(i) || index[i] >= src.GetDynamicSize(i)) {
        out_of_bound = true;
      }
    }
    if (out_of_bound) {
      continue;
    }
    data<NativeT>()[IndexUtil::MultidimensionalIndexToLinearIndex(dest_shape,
                                                                  index)] =
        src.data<NativeT>()[IndexUtil::MultidimensionalIndexToLinearIndex(
            src_shape, index)];
  } while (IndexUtil::BumpIndices(bound_shape, absl::MakeSpan(index)));
}

template <typename NativeT>
bool LiteralBase::Piece::EqualElementsInternal(
    const LiteralBase::Piece& other, std::vector<int64_t>* multi_index) const {
  if (multi_index->size() == subshape().rank()) {
    return (Get<NativeT>(*multi_index) == other.Get<NativeT>(*multi_index));
  }
  for (int64_t i = 0; i < GetDynamicSize(multi_index->size()); ++i) {
    multi_index->push_back(i);
    if (!EqualElementsInternal<NativeT>(other, multi_index)) {
      return false;
    }
    multi_index->pop_back();
  }
  return true;
}

bool LiteralBase::Piece::EqualDynamicSize(
    const LiteralBase::Piece& other) const {
  DCHECK(ShapeUtil::Compatible(subshape(), other.subshape()));
  if (subshape().is_static()) {
    return true;
  }

  for (int64_t i = 0; i < subshape().rank(); ++i) {
    if (GetDynamicSize(i) != other.GetDynamicSize(i)) {
      return false;
    }
  }
  return true;
}

bool LiteralBase::Piece::EqualElements(const LiteralBase::Piece& other) const {
  if (subshape().is_static() &&
      ShapeUtil::Equal(subshape(), other.subshape()) && subshape().IsArray()) {
    CHECK(LayoutUtil::IsDenseArray(subshape()))
        << __func__ << " is only supported for dense arrays: " << subshape();
    CHECK_EQ(size_bytes_dense(), other.size_bytes_dense());
    return memcmp(buffer(), other.buffer(), size_bytes_dense()) == 0;
  }

  std::vector<int64_t> multi_index;
  return primitive_util::PrimitiveTypeSwitch<bool>(
      [&](auto primitive_type_constant) -> bool {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return EqualElementsInternal<NativeT>(other, &multi_index);
        }
        LOG(FATAL)
            << "Unimplemented: LiteralBase::Piece::EqualElements for type "
            << PrimitiveType_Name(subshape().element_type());
      },
      subshape().element_type());
}

bool LiteralBase::operator==(const LiteralBase& other) const {
  // Checking the structure of tuple literals. Checks for dense arrays are
  // performed below.
  if (!ShapeUtil::EqualStructure(shape(), other.shape())) {
    return false;
  }

  return root_piece().ForEachSubpieceWithBool(
      [&](const ShapeIndex& index, const Piece& piece) {
        const Piece& other_piece = other.piece(index);
        const Shape& subshape = piece.subshape();
        const Shape& other_subshape = other_piece.subshape();
        if (subshape.element_type() != other_subshape.element_type()) {
          return false;
        }
        if (!piece.subshape().IsArray()) {
          return true;
        }
        if (subshape.rank() != other_subshape.rank()) {
          return false;
        }

        for (int64_t i = 0; i < subshape.rank(); ++i) {
          if (piece.GetDynamicSize(i) != other_piece.GetDynamicSize(i)) {
            return false;
          }
        }

        if (!piece.EqualElements(other_piece)) {
          return false;
        }
        return true;
      });
}

template <typename NativeT>
static bool EqualIncludingNan(NativeT a, NativeT b) {
  // msvc can't compile std::isnan(a) where `a` is uint8_t.  This is a bug
  // according to https://en.cppreference.com/w/cpp/numeric/math/isnan, but it's
  // easy to work around.
  return a == b || (std::isnan(static_cast<double>(a)) &&
                    std::isnan(static_cast<double>(b)));
}

template <typename T>
static bool EqualIncludingNan(std::complex<T> a, std::complex<T> b) {
  return EqualIncludingNan(a.real(), b.real()) &&
         EqualIncludingNan(a.imag(), b.imag());
}

template <typename NativeT>
static bool AllElementsEqualValue(absl::Span<const NativeT> data,
                                  NativeT value) {
  for (int64_t i = 0; i < data.size(); ++i) {
    if (!EqualIncludingNan(data[i], value)) {
      return false;
    }
  }
  return true;
}

bool Literal::Piece::IsAll(const Literal& scalar) const {
  CHECK(ShapeUtil::IsScalar(scalar.shape())) << scalar.shape().ToString();
  if (!subshape().IsArray()) {
    return false;
  }

  CHECK(LayoutUtil::IsDenseArray(subshape()))
      << __func__ << " is only supported for dense arrays: " << subshape();
  CHECK_EQ(subshape().element_type(), scalar.shape().element_type());
  return primitive_util::PrimitiveTypeSwitch<bool>(
      [&](auto primitive_type_constant) -> bool {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          using NativeT = typename primitive_util::PrimitiveTypeToNative<
              primitive_type_constant>::type;
          return AllElementsEqualValue(data<NativeT>(),
                                       scalar.GetFirstElement<NativeT>());
        }
        return false;
      },
      subshape().element_type());
}

bool LiteralBase::IsAll(const Literal& scalar) const {
  return root_piece().IsAll(scalar);
}

bool LiteralBase::IsAll(int8_t value) const {
  if (!shape().IsArray()) {
    return false;
  }
  PrimitiveType ty = shape().element_type();
  if (primitive_util::IsFloatingPointType(ty)) {
    return IsAllFloatImpl(value, /*round_value=*/false);
  }
  if (primitive_util::IsUnsignedIntegralType(ty) && value < 0) {
    return false;
  }
  Literal scalar(ShapeUtil::MakeScalarShape(ty));
  if (!primitive_util::PrimitiveTypeSwitch<bool>(
          [&](auto primitive_type_constant) -> bool {
            if constexpr (primitive_util::IsIntegralType(
                              primitive_type_constant) ||
                          primitive_type_constant == PRED) {
              using NativeT = typename primitive_util::PrimitiveTypeToNative<
                  primitive_type_constant>::type;
              if (value != static_cast<int8_t>(static_cast<NativeT>(value))) {
                return false;
              }
              scalar.Set<NativeT>({}, value);
              return true;
            }
            return false;
          },
          ty)) {
    return false;
  }
  return root_piece().IsAll(scalar);
}

bool LiteralBase::IsAllFloat(float value) const {
  return IsAllFloatImpl(value, /*round_value=*/true);
}

bool LiteralBase::IsAllFloatImpl(float value, bool round_value) const {
  if (!shape().IsArray()) {
    return false;
  }
  PrimitiveType ty = shape().element_type();
  Literal scalar(ShapeUtil::MakeScalarShape(ty));
  if (!primitive_util::PrimitiveTypeSwitch<bool>(
          [&](auto primitive_type_constant) -> bool {
            if constexpr (primitive_util::IsFloatingPointType(
                              primitive_type_constant)) {
              using NativeT = typename primitive_util::PrimitiveTypeToNative<
                  primitive_type_constant>::type;
              if (!round_value &&
                  !EqualIncludingNan(
                      value, static_cast<float>(static_cast<NativeT>(value)))) {
                return false;
              }
              scalar.Set<NativeT>({}, static_cast<NativeT>(value));
              return true;
            }
            return false;
          },
          ty)) {
    return false;
  }
  return root_piece().IsAll(scalar);
}

bool LiteralBase::IsAllComplex(complex64 value) const {
  if (!shape().IsArray()) {
    return false;
  }
  PrimitiveType ty = shape().element_type();
  Literal scalar(ShapeUtil::MakeScalarShape(ty));
  switch (ty) {
    case C64:
      scalar.Set<complex64>({}, value);
      break;
    case C128:
      scalar.Set<complex128>({}, value);
      break;
    default:
      return false;
  }
  return root_piece().IsAll(scalar);
}

bool LiteralBase::IsAllFirst() const {
  if (!shape().IsArray()) {
    return false;
  }

  // Empty shapes are not all the first element since there is no first element.
  if (ShapeUtil::IsZeroElementArray(shape())) {
    return false;
  }

  absl::InlinedVector<int64_t, 4> start_indices(/*n=*/shape().rank(), 0);
  absl::InlinedVector<int64_t, 4> end_indices(/*n=*/shape().rank(), 1);
  Literal first = Slice(start_indices, end_indices);
  return IsAll(first.Reshape({}).value());
}

bool LiteralBase::IsR1Iota() const {
  if (!shape().IsArray()) {
    return false;
  }

  CHECK(LayoutUtil::IsDenseArray(shape()))
      << __func__ << " is only supported for dense arrays: " << shape();

  if (shape().rank() != 1) {
    return false;
  }

  auto is_iota_at_idx = [&](const int64_t idx) {
    return primitive_util::PrimitiveTypeSwitch<bool>(
        [&](auto primitive_type_constant) -> bool {
          if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
            using NativeT = typename primitive_util::PrimitiveTypeToNative<
                primitive_type_constant>::type;
            if constexpr (primitive_util::IsIntegralType(
                              primitive_type_constant)) {
              return static_cast<int64_t>(Get<NativeT>({idx})) == idx;
            }
            if constexpr (primitive_util::IsFloatingPointType(
                              primitive_type_constant)) {
              return Get<NativeT>({idx}) == static_cast<NativeT>(idx);
            }
            if constexpr (primitive_util::IsComplexType(
                              primitive_type_constant)) {
              return Get<NativeT>({idx}) ==
                     NativeT(static_cast<typename NativeT::value_type>(idx),
                             static_cast<typename NativeT::value_type>(0.0));
            }
          }
          // pred, token, opaque, tuple, etc. are all not iota.
          return false;
        },
        shape().element_type());
  };

  const int64_t elements = ShapeUtil::ElementsIn(shape());
  for (int64_t idx = 0; idx < elements; ++idx) {
    if (!is_iota_at_idx(idx)) {
      return false;
    }
  }

  return true;
}

// Returns a stride if the literal is a strided iota, i.e., iota multiplied by a
// stride. Only applicable for integer iotas. Returns std::nullopt if the
// literal is not a strided iota.
std::optional<int64_t> LiteralBase::IsR1StridedIota() const {
  if (!shape().IsArray() || shape().rank() != 1) {
    return std::nullopt;
  }

  CHECK(LayoutUtil::IsDenseArray(shape()))
      << __func__ << " is only supported for dense arrays: " << shape();

  const int64_t elements = ShapeUtil::ElementsIn(shape());
  const PrimitiveType type = shape().element_type();
  if (elements <= 1 || !primitive_util::IsIntegralType(type)) {
    return std::nullopt;
  }

  auto get_element_at = [&](const int64_t idx) -> int64_t {
    return primitive_util::PrimitiveTypeSwitch<int64_t>(
        [&](auto primitive_type_constant) -> int64_t {
          if constexpr (primitive_util::IsIntegralType(
                            primitive_type_constant)) {
            using NativeT = typename primitive_util::PrimitiveTypeToNative<
                primitive_type_constant>::type;
            return static_cast<int64_t>(Get<NativeT>({idx}));
          }
          LOG(FATAL) << "unexpected type";
        },
        shape().element_type());
  };

  // Infer the stride as the second element (since first element is supposed
  // to be zero).
  int64_t stride = get_element_at(1);
  if (stride == 0) {
    return std::nullopt;
  }

  for (int64_t idx = 0; idx < elements; ++idx) {
    if (get_element_at(idx) != idx * stride) {
      return std::nullopt;
    }
  }

  return stride;
}

bool LiteralBase::IsZero(absl::Span<const int64_t> indices) const {
  CHECK(LayoutUtil::IsDenseArray(shape()))
      << __func__ << " is only supported for dense arrays: " << shape();
  switch (shape().element_type()) {
    case U8:
      return Get<uint8_t>(indices) == 0;
    case U16:
      return Get<uint16_t>(indices) == 0;
    case U32:
      return Get<uint32_t>(indices) == 0;
    case U64:
      return Get<uint64_t>(indices) == 0;
    case S8:
      return Get<int8_t>(indices) == 0;
    case S16:
      return Get<int16_t>(indices) == 0;
    case S32:
      return Get<int32_t>(indices) == 0;
    case S64:
      return Get<int64_t>(indices) == 0;
    case F32:
      return Get<float>(indices) == 0.0f;
    case F64:
      return Get<double>(indices) == 0.0;
    case C64:
      return Get<complex64>(indices) == complex64(0.0f, 0.0f);
    case C128:
      return Get<complex128>(indices) == complex128(0.0f, 0.0f);
    case F16:
      return Get<half>(indices) == static_cast<half>(0.0f);
    case BF16:
      return Get<bfloat16>(indices) == static_cast<bfloat16>(0.0f);
    case F8E5M2:
      return Get<tsl::float8_e5m2>(indices) ==
             static_cast<tsl::float8_e5m2>(0.0f);
    case F8E4M3FN:
      return Get<tsl::float8_e4m3fn>(indices) ==
             static_cast<tsl::float8_e4m3fn>(0.0f);
    case PRED:
      return Get<bool>(indices) == false;
    default:
      LOG(FATAL) << "Input literal must be an array.";
  }
}

namespace {

template <typename RepeatedFieldT, typename NativeT>
void CopyToRepeatedField(RepeatedFieldT* dest,
                         const absl::Span<const NativeT> src) {
  *dest = RepeatedFieldT(src.begin(), src.end());
}

}  // namespace

void LiteralBase::Piece::set_array_value_state(ArrayValueState state) {
  array_value_state_ = state;
}

LiteralBase::ArrayValueState LiteralBase::Piece::get_array_value_state() const {
  return array_value_state_;
}

void LiteralBase::Piece::WriteToProto(LiteralProto* proto) const {
  *proto->mutable_shape() = subshape().ToProto();
  switch (subshape().element_type()) {
    case PRED:
      CopyToRepeatedField(proto->mutable_preds(), data<bool>());
      break;
    case S8:
      proto->set_s8s(static_cast<const signed char*>(data<int8_t>().data()),
                     element_count());
      break;
    case U8:
      proto->set_u8s(static_cast<const unsigned char*>(data<uint8_t>().data()),
                     element_count());
      break;
    case U32:
      CopyToRepeatedField(proto->mutable_u32s(), data<uint32_t>());
      break;
    case U64:
      CopyToRepeatedField(proto->mutable_u64s(), data<uint64_t>());
      break;
    case S32:
      CopyToRepeatedField(proto->mutable_s32s(), data<int32_t>());
      break;
    case S64:
      CopyToRepeatedField(proto->mutable_s64s(), data<int64_t>());
      break;
    case U16:
      *proto->mutable_u16s() =
          std::string(reinterpret_cast<const char*>(data<uint16_t>().data()),
                      size_bytes_dense());
      if (!kLittleEndian) {
        ConvertEndianShort(proto->mutable_u16s());
      }
      break;
    case S16:
      *proto->mutable_s16s() =
          std::string(reinterpret_cast<const char*>(data<int16_t>().data()),
                      size_bytes_dense());
      if (!kLittleEndian) {
        ConvertEndianShort(proto->mutable_s16s());
      }
      break;
    case F16:
      *proto->mutable_f16s() =
          std::string(reinterpret_cast<const char*>(data<half>().data()),
                      size_bytes_dense());
      if (!kLittleEndian) {
        ConvertEndianShort(proto->mutable_f16s());
      }
      break;
    case BF16:
      *proto->mutable_bf16s() =
          std::string(reinterpret_cast<const char*>(data<bfloat16>().data()),
                      size_bytes_dense());
      if (!kLittleEndian) {
        ConvertEndianShort(proto->mutable_bf16s());
      }
      break;
    case F8E5M2:
      *proto->mutable_f8e5m2s() = std::string(
          reinterpret_cast<const char*>(data<tsl::float8_e5m2>().data()),
          size_bytes_dense());
      break;
    case F8E4M3FN:
      *proto->mutable_f8e4m3fns() = std::string(
          reinterpret_cast<const char*>(data<tsl::float8_e4m3fn>().data()),
          size_bytes_dense());
      break;
    case F32:
      CopyToRepeatedField(proto->mutable_f32s(), data<float>());
      break;
    case F64:
      CopyToRepeatedField(proto->mutable_f64s(), data<double>());
      break;
    case C64:
      for (complex64 value : data<complex64>()) {
        proto->add_c64s(value.real());
        proto->add_c64s(value.imag());
      }
      break;
    case C128:
      for (complex128 value : data<complex128>()) {
        proto->add_c128s(value.real());
        proto->add_c128s(value.imag());
      }
      break;
    case TUPLE:
    case TOKEN:
      // Nothing to do but assign the shape which is done above.
      return;
    default:
      // TODO(b/111551621): Support serializing more PrimitiveTypes.
      LOG(FATAL) << "Unhandled primitive type "
                 << PrimitiveType_Name(subshape().element_type());
  }
}

const void* LiteralBase::Piece::untyped_data() const {
  DCHECK(LayoutUtil::IsDenseArray(subshape()))
      << ShapeUtil::HumanString(subshape());
  return buffer();
}

void* LiteralBase::Piece::untyped_data() {
  DCHECK(LayoutUtil::IsDenseArray(subshape()))
      << ShapeUtil::HumanString(subshape());
  return buffer();
}

namespace {

template <typename RepeatedFieldT, typename NativeT>
Status CopyFromRepeatedField(absl::Span<NativeT> dest,
                             const RepeatedFieldT& src) {
  if (dest.size() != src.size()) {
    return InvalidArgument(
        "Expected %lu elements in LiteralProto repeated field, has %d",
        dest.size(), src.size());
  }
  std::copy(src.begin(), src.end(), dest.begin());
  return OkStatus();
}

}  // namespace

Status LiteralBase::Piece::CopyFromProto(const LiteralProto& proto) {
  // These conditions should have been checked in
  // MutableLiteralBase::CreateFromProto.
  TF_RET_CHECK(proto.has_shape());
  Shape shape(proto.shape());
  TF_RET_CHECK(LayoutUtil::HasLayout(shape));
  TF_RET_CHECK(ShapeUtil::Equal(shape, subshape()));

  switch (subshape().element_type()) {
    case PRED:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<bool>(), proto.preds()));
      break;
    case S8: {
      auto s8_data = data<int8_t>();
      TF_RET_CHECK(proto.s8s().size() == s8_data.size());
      std::copy(proto.s8s().begin(), proto.s8s().end(), s8_data.begin());
    } break;
    case U8: {
      auto u8_data = data<uint8_t>();
      TF_RET_CHECK(proto.u8s().size() == u8_data.size());
      std::copy(proto.u8s().begin(), proto.u8s().end(), u8_data.begin());
    } break;
    case S32:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<int32_t>(), proto.s32s()));
      break;
    case S64:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<int64_t>(), proto.s64s()));
      break;
    case U32:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<uint32_t>(), proto.u32s()));
      break;
    case U64:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<uint64_t>(), proto.u64s()));
      break;
    case S16: {
      const std::string& s(proto.s16s());
      TF_RET_CHECK(data<int16_t>().size() * sizeof(int16_t) == s.size());
      memcpy(untyped_data(), s.data(), s.size());
      if (!kLittleEndian) {
        ConvertEndianShort(reinterpret_cast<char*>(untyped_data()), s.size());
      }
    } break;
    case U16: {
      const std::string& s(proto.u16s());
      TF_RET_CHECK(data<uint16_t>().size() * sizeof(uint16_t) == s.size());
      memcpy(untyped_data(), s.data(), s.size());
      if (!kLittleEndian) {
        ConvertEndianShort(reinterpret_cast<char*>(untyped_data()), s.size());
      }
    } break;
    case F8E5M2: {
      const std::string& s(proto.f8e5m2s());
      TF_RET_CHECK(data<tsl::float8_e5m2>().size() * sizeof(tsl::float8_e5m2) ==
                   s.size());
      memcpy(untyped_data(), s.data(), s.size());
    } break;
    case F8E4M3FN: {
      const std::string& s(proto.f8e4m3fns());
      TF_RET_CHECK(data<tsl::float8_e4m3fn>().size() *
                       sizeof(tsl::float8_e4m3fn) ==
                   s.size());
      memcpy(untyped_data(), s.data(), s.size());
    } break;
    case F16: {
      const std::string& s(proto.f16s());
      TF_RET_CHECK(data<half>().size() * sizeof(half) == s.size());
      memcpy(untyped_data(), s.data(), s.size());
      if (!kLittleEndian) {
        ConvertEndianShort(reinterpret_cast<char*>(untyped_data()), s.size());
      }
    } break;

    case BF16: {
      const std::string& s(proto.bf16s());
      TF_RET_CHECK(data<bfloat16>().size() * sizeof(bfloat16) == s.size());
      memcpy(untyped_data(), s.data(), s.size());
      if (!kLittleEndian) {
        ConvertEndianShort(reinterpret_cast<char*>(untyped_data()), s.size());
      }
    } break;
    case F32:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<float>(), proto.f32s()));
      break;
    case F64:
      TF_RETURN_IF_ERROR(CopyFromRepeatedField(data<double>(), proto.f64s()));
      break;
    case C64: {
      auto complex_data = data<complex64>();
      TF_RET_CHECK(proto.c64s_size() == complex_data.size() * 2);
      for (int64_t i = 0; i < complex_data.size(); ++i) {
        complex_data[i] = complex64{proto.c64s(i * 2), proto.c64s(i * 2 + 1)};
      }
      break;
    }
    case C128: {
      auto complex_data = data<complex128>();
      const int64_t complex_data_size_doubled = complex_data.size() * 2;
      TF_RET_CHECK(proto.c128s_size() == complex_data_size_doubled);
      for (int64_t i = 0, end = complex_data.size(); i < end; ++i) {
        complex_data[i] =
            complex128{proto.c128s(i * 2), proto.c128s(i * 2 + 1)};
      }
      break;
    }
    case TUPLE:
      return InvalidArgument("Should not be called on tuple shapes: %s",
                             ShapeUtil::HumanString(subshape()));
    default:
      return InvalidArgument("Is called on unsupported shape: %s",
                             ShapeUtil::HumanString(subshape()));
  }
  return OkStatus();
}

bool LiteralBase::Piece::IsKnown() const {
  if (array_value_state_ != ArrayValueState::kKnown) {
    return false;
  }
  if (subshape().IsTuple()) {
    bool are_all_leaf_arrays_known = true;
    ForEachSubpiece([&are_all_leaf_arrays_known](const ShapeIndex& index,
                                                 const Piece& piece) {
      if (!piece.subshape().IsArray()) {
        return;
      }
      are_all_leaf_arrays_known &= piece.IsKnown();
    });
    return are_all_leaf_arrays_known;
  }
  return true;
}

bool LiteralBase::Piece::IsDetermined() const {
  if (array_value_state_ == ArrayValueState::kUndetermined) {
    return false;
  }
  if (subshape().IsTuple()) {
    bool are_all_leaf_arrays_determined = true;
    ForEachSubpiece([&are_all_leaf_arrays_determined](const ShapeIndex& index,
                                                      const Piece& piece) {
      if (!piece.subshape().IsArray()) {
        return;
      }
      are_all_leaf_arrays_determined &= piece.IsDetermined();
    });
    return are_all_leaf_arrays_determined;
  }
  return true;
}

LiteralProto LiteralBase::ToProto() const {
  LiteralProto proto;
  root_piece().ForEachSubpiece(
      [&](const ShapeIndex& index, const Piece& piece) {
        LiteralProto* proto_piece = &proto;
        for (int64_t i : index) {
          while (proto_piece->tuple_literals_size() <= i) {
            proto_piece->add_tuple_literals();
          }
          proto_piece = proto_piece->mutable_tuple_literals(i);
        }
        piece.WriteToProto(proto_piece);
      });

  return proto;
}

const void* LiteralBase::untyped_data(const ShapeIndex& shape_index) const {
  return piece(shape_index).untyped_data();
}

void* MutableLiteralBase::untyped_data(const ShapeIndex& shape_index) {
  return piece(shape_index).untyped_data();
}

int64_t LiteralBase::size_bytes(const ShapeIndex& shape_index) const {
  return piece(shape_index).size_bytes_dense();
}

std::string LiteralBase::GetR1U8AsString() const {
  CHECK(shape().IsArray());
  CHECK_EQ(shape().rank(), 1);
  CHECK_EQ(shape().element_type(), U8);
  return std::string(absl::bit_cast<const char*>(data<uint8_t>().data()),
                     ShapeUtil::ElementsIn(shape()));
}

void MutableBorrowingLiteral::CopyPieceSubtree(const Shape& shape,
                                               const Piece* src_piece,
                                               Piece* dest_piece) {
  DCHECK(ShapeUtil::Equal(src_piece->subshape(), dest_piece->subshape()))
      << "src_piece has shape: "
      << ShapeUtil::HumanString(src_piece->subshape())
      << "dest_piece has shape: "
      << ShapeUtil::HumanString(dest_piece->subshape());
  dest_piece->set_array_value_state(src_piece->get_array_value_state());
  if (shape.IsTuple()) {
    for (int i = 0; i < ShapeUtil::TupleElementCount(shape); ++i) {
      const Shape& subshape = shape.tuple_shapes(i);

      auto child_piece = Piece();
      child_piece.set_subshape(&subshape);

      CopyPieceSubtree(subshape, &src_piece->child(i), &child_piece);

      dest_piece->emplace_back(std::move(child_piece));
    }
  } else if (shape.IsArray()) {
    dest_piece->set_buffer(const_cast<char*>(src_piece->buffer()));
  }
}

MutableLiteralBase::~MutableLiteralBase() = default;

MutableBorrowingLiteral::MutableBorrowingLiteral(
    const MutableBorrowingLiteral& literal)
    : MutableLiteralBase() {
  shape_ = literal.shape_.Clone();
  CHECK(LayoutUtil::HasLayout(*shape_));

  root_piece_ = new Piece();
  root_piece_->set_subshape(shape_.get());

  CopyPieceSubtree(*shape_, &literal.root_piece(), root_piece_);
}

MutableBorrowingLiteral& MutableBorrowingLiteral::operator=(
    const MutableBorrowingLiteral& literal) {
  shape_ = literal.shape_.Clone();
  CHECK(LayoutUtil::HasLayout(*shape_));

  root_piece_ = new Piece();
  root_piece_->set_subshape(shape_.get());

  CopyPieceSubtree(*shape_, &literal.root_piece(), root_piece_);

  return *this;
}

MutableBorrowingLiteral::MutableBorrowingLiteral(MutableLiteralBase* literal)
    : MutableLiteralBase() {
  shape_ = literal->shape_.Clone();
  CHECK(LayoutUtil::HasLayout(*shape_));

  root_piece_ = new Piece();
  root_piece_->set_subshape(shape_.get());

  CopyPieceSubtree(*shape_, &literal->root_piece(), root_piece_);
}

MutableBorrowingLiteral::MutableBorrowingLiteral(
    MutableBorrowingLiteral literal, const ShapeIndex& view_root)
    : MutableLiteralBase() {
  shape_ = std::make_unique<Shape>(literal.piece(view_root).subshape());
  CHECK(LayoutUtil::HasLayout(*shape_));

  root_piece_ = new Piece();
  root_piece_->set_subshape(shape_.get());

  CopyPieceSubtree(*shape_, &literal.piece(view_root), root_piece_);
}

MutableBorrowingLiteral::MutableBorrowingLiteral(const char* src_buf_ptr,
                                                 const Shape& shape)
    : MutableLiteralBase() {
  shape_ = std::make_unique<Shape>(shape);
  CHECK(LayoutUtil::HasLayout(*shape_));
  CHECK(!shape_->IsTuple());

  root_piece_ = new Piece();
  root_piece_->set_subshape(shape_.get());
  root_piece_->set_buffer(const_cast<char*>(src_buf_ptr));
}

MutableBorrowingLiteral::MutableBorrowingLiteral(absl::Span<char*> src_buf_ptrs,
                                                 const Shape& shape)
    : MutableLiteralBase() {
  shape_ = std::make_unique<Shape>(shape);
  if (!shape_->IsTuple()) {
    CHECK_EQ(src_buf_ptrs.size(), 1);
    root_piece_ = new Piece();
    root_piece_->set_subshape(shape_.get());
    root_piece_->set_buffer(const_cast<char*>(src_buf_ptrs[0]));
  } else {
    CHECK(!ShapeUtil::IsNestedTuple(*shape_));
    CHECK_EQ(src_buf_ptrs.size(), ShapeUtil::TupleElementCount(*shape_));
    root_piece_ = new Piece();
    root_piece_->set_subshape(shape_.get());

    for (int i = 0; i < src_buf_ptrs.size(); ++i) {
      Piece child_piece;
      const auto& src_shape = shape_->tuple_shapes(i);
      CHECK(src_shape.IsArray());
      child_piece.set_subshape(&src_shape);
      child_piece.set_buffer(src_buf_ptrs[i]);
      root_piece_->emplace_back(std::move(child_piece));
    }
  }
}

MutableBorrowingLiteral::~MutableBorrowingLiteral() {
  if (root_piece_ != nullptr) {
    delete root_piece_;
  }
}

LiteralSlice::LiteralSlice(const LiteralBase& literal)
    : LiteralBase(), root_piece_(&literal.root_piece()) {}

LiteralSlice::LiteralSlice(const LiteralBase& literal,
                           const ShapeIndex& view_root)
    : LiteralBase(), root_piece_(&literal.piece(view_root)) {}

void BorrowingLiteral::BuildPieceSubtree(const Shape& shape, Piece* piece) {
  CHECK(shape.IsTuple());
  for (int i = 0; i < ShapeUtil::TupleElementCount(shape); ++i) {
    const Shape& subshape = shape.tuple_shapes(i);

    auto child_piece = Piece();
    child_piece.set_subshape(&subshape);

    if (subshape.IsTuple()) {
      BuildPieceSubtree(subshape, &child_piece);
    }

    piece->emplace_back(std::move(child_piece));
  }
}

BorrowingLiteral::BorrowingLiteral(const char* src_buf_ptr, const Shape& shape)
    : LiteralBase(), shape_(std::make_unique<Shape>(shape)) {
  CHECK(shape_->IsArray());
  CHECK(LayoutUtil::HasLayout(*shape_));

  root_piece_ = Piece();
  root_piece_.set_subshape(shape_.get());
  root_piece_.set_buffer(const_cast<char*>(src_buf_ptr));
}

BorrowingLiteral::BorrowingLiteral(absl::Span<const char* const> src_buf_ptrs,
                                   const Shape& shape)
    : LiteralBase(), shape_(std::make_unique<Shape>(shape)) {
  CHECK(shape_->IsTuple());
  CHECK(!ShapeUtil::IsNestedTuple(*shape_));
  CHECK_EQ(src_buf_ptrs.size(), ShapeUtil::TupleElementCount(*shape_));
  root_piece_ = Piece();
  root_piece_.set_subshape(shape_.get());
  BuildPieceSubtree(*shape_, &root_piece_);

  for (int i = 0, end = src_buf_ptrs.size(); i < end; ++i) {
    const auto& src_shape = shape_->tuple_shapes(i);
    CHECK(src_shape.IsArray());
    root_piece_.child(i).set_buffer(const_cast<char*>(src_buf_ptrs[i]));
  }
}

}  // namespace xla
