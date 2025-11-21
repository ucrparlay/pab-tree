#ifndef PMOS_DEPENDENCE_TYPE_TRAIT_H_
#define PMOS_DEPENDENCE_TYPE_TRAIT_H_

#include <cstdint>
#include <type_traits>
#include <utility>

#include "comparator.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"

namespace pmos {

template <class EleType>
struct EleWrapper : EleType {
  using KeyType = typename EleType::KeyType;
  using ValType = typename EleType::ValType;
  // using Entry = std::tuple<KeyType, ValType>;
  using Entry = EleType::Entry;

  static inline auto GetKey(Entry const e) { return EleType::GetKey(e); }
  static inline auto GetVal(Entry const e) { return EleType::GetVal(e); }
  static inline Entry ToEntry(KeyType const& k, ValType const& v) {
    return EleType::ToEntry(k, v);
  };
};

template <typename EleType, uint kBufferSize, typename LeafAugType,
          typename InteriorAugType, uint_fast8_t kSkHeightVal>
class TypeTrait {
 public:
  // NOTE: element type
  using E = EleType::Entry;
  using K = typename E::KeyType;
  using V = typename E::ValType;

  // NOTE: general type
  using Num = Num_Comparator<K>;
  using Slice = parlay::slice<E*, E*>;
  using ConstSlice = parlay::slice<E const*, E const*>;
  using EleSeq = parlay::sequence<E>;
  using ConstEleSeq = parlay::sequence<E> const;
  using EleIter = typename parlay::sequence<E>::iterator;
  using Box = std::pair<K, K>;
  using BoxSeq = parlay::sequence<Box>;

  using LeafAug = LeafAugType;
  using InteriorAug = InteriorAugType;

  // NOTE: block param in point sieving
  static constexpr uint_fast8_t const kSkeletonHeight = kSkHeightVal;
  using BucketType =
      std::conditional_t<(kSkeletonHeight > 7), uint_fast16_t, uint_fast8_t>;
  using BallsType = uint_fast32_t;
  static constexpr BucketType const kPivotNum = (1 << kSkeletonHeight) - 1;
  static constexpr BucketType const kBucketNum = 1 << kSkeletonHeight;
  static constexpr uint_fast8_t const kLog2Base = 10;
  static constexpr uint_fast16_t const kBlockSize = 1 << kLog2Base;

  using BucketSeq = parlay::sequence<BucketType>;
  using BallSeq = parlay::sequence<BallsType>;
};

template <typename EleType, uint kBufferSize, typename LeafAugType,
          typename InteriorAugType, uint_fast8_t kSkHeightVal>
class BTreeTypeTrait
    : public TypeTrait<EleWrapper<EleType>, kBufferSize, LeafAugType,
                       InteriorAugType, kSkHeightVal> {
 public:
  using BT = TypeTrait<EleWrapper<EleType>, kBufferSize, LeafAugType,
                       InteriorAugType, kSkHeightVal>;
  using BufferSizeType =
      std::conditional_t<(kBufferSize > 255u), uint_fast16_t, uint_fast8_t>;
  static constexpr BufferSizeType kBuf = kBufferSize;
  static constexpr BufferSizeType kBufLB = kBufferSize;
  static constexpr BufferSizeType kBufMD = 2 * kBufferSize;
  static constexpr BufferSizeType kBufUB = 4 * kBufferSize;

  using Split = BT::K;
  using SplitSeq = parlay::sequence<Split>;
  using SplitSeqSlice = parlay::slice<Split*, Split*>;
};

}  // namespace pmos

#endif  // PMOS_DEPENDENCE_TYPE_TRAIT_H_
