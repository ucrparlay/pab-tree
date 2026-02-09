#ifndef PMOS_DEPENDENCE_TYPE_TRAIT_H_
#define PMOS_DEPENDENCE_TYPE_TRAIT_H_

#include <cstdint>
#include <type_traits>
#include <utility>

#include "comparator.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"

namespace pmos {

template<class EleType>
struct EleWrapper : EleType
{
  using E = typename EleType::E;
  using K = typename EleType::K;
  using V = typename EleType::V;
  // using Entry = std::tuple<KeyType, ValType>;

  static auto GetKey(E const e)
  {
    return EleType::GetKey(e);
  }
  static auto GetVal(E const e)
  {
    return EleType::GetVal(e);
  }
  static E ToEntry(K const& k, V const& v)
  {
    return EleType::ToEntry(k, v);
  };
};

template<typename EleType,
         uint kBufferSize,
         typename LeafAugType,
         typename InteriorAugType,
         uint_fast8_t kSkHeightVal,
         typename NodeType>
class TypeTrait
{
public:
  // NOTE: element type
  using E = EleType::E;
  using K = typename EleType::K;
  using V = typename EleType::V;
  using Entry = EleType; // NOTE: abstract type to access E

  // NOTE: general type
  using Num = Num_Comparator<K>;

  using EntrySeq = parlay::sequence<E>;
  using EntrySlice = parlay::slice<E*, E*>;
  using ConstEntrySlice = parlay::slice<E const*, E const*>;
  using ConstEntrySeq = parlay::sequence<E> const;
  using EntryIter = typename parlay::sequence<E>::iterator;

  using KeySeq = parlay::sequence<K>;
  using ConstKeySeq = parlay::sequence<K> const;
  using KeySlice = parlay::slice<K*, K*>;
  using ConstKeySlice = parlay::slice<K const*, K const*>;
  using KeyIter = typename parlay::sequence<K>::iterator;

  using Box = std::pair<K, K>;
  using BoxSeq = parlay::sequence<Box>;

  using LeafAug = LeafAugType;
  using InteriorAug = InteriorAugType;

  using Node = NodeType;
  using NodeSeq = parlay::sequence<Node*>;
  using NodeSlice = parlay::slice<Node**, Node**>;
};

template<typename EleType,
         uint kBufferSize,
         typename LeafAugType,
         typename InteriorAugType,
         uint_fast8_t kSkHeightVal,
         typename NodeType>
class BTreeTypeTrait
  : public TypeTrait<EleWrapper<EleType>,
                     kBufferSize,
                     LeafAugType,
                     InteriorAugType,
                     kSkHeightVal,
                     NodeType>
{
public:
  using BT = TypeTrait<EleWrapper<EleType>,
                       kBufferSize,
                       LeafAugType,
                       InteriorAugType,
                       kSkHeightVal,
                       NodeType>;
  using Split = BT::K;
  using SplitSeq = parlay::sequence<Split>;
  using SplitSlice = parlay::slice<Split*, Split*>;
  using SplitSliceConst = parlay::slice<Split const*, Split const*>;

  static uint const kBufMDFac = 2;
  static uint const kBufUBFac = 4;

  using SizeT = uint_fast32_t;
  using BufSizeT =
    std::conditional_t<(kBufferSize * kBufUBFac > 65535U),
                       uint_fast32_t,
                       std::conditional_t<(kBufferSize * kBufUBFac > 255U),
                                          uint_fast16_t,
                                          uint_fast8_t>>;
  static constexpr BufSizeT kBuf = kBufferSize;
  static constexpr BufSizeT kBufLB = kBufferSize;
  static constexpr BufSizeT kBufMD = kBufMDFac * kBufferSize;
  static constexpr BufSizeT kBufUB = kBufUBFac * kBufferSize;

  using SizeBufUBArr = std::array<SizeT, kBufUB>;
  using BufSizeBufUBArr = std::array<BufSizeT, kBufUB>;
  using BooleanBufUBArr = std::array<bool, kBufUB>;

  // NOTE: block param in point sieving
  static constexpr uint_fast8_t const kSkeletonHeight = kSkHeightVal;
  // using BucketType =
  //   std::conditional_t<(kSkeletonHeight > 7), uint_fast16_t, uint_fast8_t>;
  using BucketType = BufSizeT; // WARN: Assume sieve one node at a time
  // static constexpr BucketType const kPivotNum = (1 << kSkeletonHeight) - 1;
  // static constexpr BucketType const kBucketNum = 1 << kSkeletonHeight;
  static constexpr uint_fast8_t const kLog2Base = 10;
  static constexpr uint_fast16_t const kBlockSize = 1 << kLog2Base;
  static constexpr uint_fast8_t const kSplitFactor = 8;

  using BucketSeq = parlay::sequence<BucketType>;
  using BallSeq = parlay::sequence<SizeT>;
};

} // namespace pmos

#endif // PMOS_DEPENDENCE_TYPE_TRAIT_H_
