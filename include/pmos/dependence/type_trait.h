#ifndef PMOS_DEPENDENCE_TYPE_TRAIT_H_
#define PMOS_DEPENDENCE_TYPE_TRAIT_H_

#include <cstdint>
#include <type_traits>
#include <utility>

#include "comparator.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"

namespace pmos {

template <typename EleType, typename LeafAugType, typename InteriorAugType,
          uint_fast8_t kSkHeightVal>
class TypeTrait {
 public:
  using E = EleType;
  using K = typename E::KeyType;
  using V = typename E::ValType;
  static constexpr uint_fast8_t const kSkeletonHeight = kSkHeightVal;

  using Num = Num_Comparator<K>;
  using Slice = parlay::slice<E*, E*>;
  using ConstSlice = parlay::slice<E const*, E const*>;
  using EleSeq = parlay::sequence<E>;
  using ConstEleSeq = parlay::sequence<E> const;
  using EleIter = typename parlay::sequence<E>::iterator;

  using LeafAug = LeafAugType;
  using InteriorAug = InteriorAugType;

  // NOTE: block param in point sieving
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

}  // namespace pmos

#endif  // PMOS_DEPENDENCE_TYPE_TRAIT_H_
