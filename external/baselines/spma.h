#pragma once

#include <cstdint>

#include "PMA/CPMA.hpp"
#include "PMA/internal/leaf.hpp"
#include "StructOfArrays/multipointer.hpp"
#include "dependence/loggers.h"
#include "parlay/primitives.h"
#include "pb_tree/b_tree.h"

namespace SPMA {

template<class TypeTrait>
class SpmaWrapper
{
public:
  using TT = TypeTrait;

  using E = typename TT::E;
  using K = typename TT::K;
  using V = typename TT::V;
  using Entry = TT::Entry;

  using Num = typename TT::Num;
  using EntrySlice = typename TT::EntrySlice;
  using Split = typename TT::Split;
  using SplitSeq = typename TT::SplitSeq;

  using Leaf = TT::Node*;
  using Interior = TT::Node*;

  // using PmaLeaf = uncompressed_leaf<K>;
  using SpmaLeaf = uncompressed_leaf<K, V>;
  using PmosSpmaSettings = PMA_traits<SpmaLeaf, BNary, 17, false, false>;
  using SpmaPointer = SpmaLeaf::element_ptr_type;
  static_assert(!SpmaLeaf::binary);
  static_assert(PmosSpmaSettings::parallel);
  using SPMA = CPMA<PmosSpmaSettings>;
  std::shared_ptr<SPMA> pma_tree_;

  using InputRangeT = std::pair<parlay::sequence<K>, parlay::sequence<V>>;

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return { parlay::map(input_seq,
                         [&](auto e) { return Entry::GetKey(e) + 1; }),
             parlay::map(input_seq, [&](auto e) { return Entry::GetVal(e); }) };
  }

  template<class PT>
  void map_init(PT P)
  {
    assert(parlay::all_of(P.first, [](auto x) { return x != 0; }));
    size_t n = P.first.size();
    SpmaPointer it(P.first.begin(), P.second.begin());
    pma_tree_->insert_batch(it, n, false);
  }

  template<typename PT>
  void map_insert(PT P)
  {
    assert(parlay::all_of(P.first, [](auto x) { return x != 0; }));
    size_t n = P.first.size();
    // parlay::sort_inplace(parlay::make_slice(P.first));
    // parlay::sort_inplace(parlay::make_slice(P.second));
    SpmaPointer it(P.first.begin(), P.second.begin());
    pma_tree_->insert_batch(it, n, false);
    // std::cout << "PMA insert done. size: " << pma_tree_.size() << std::endl;
  }

  // APIs
  template<typename Range>
  void Build(Range&& In)
  {
    map_init(std::forward<Range>(In));
  }

  template<typename Range>
  void BatchInsert(Range&& In)
  {
    map_insert(std::forward<Range>(In));
  }
  //
  // void BatchDelete(Slice In) {
  //   if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
  //     return;
  //   }
  //   cpam_aug_map_ = map_delete(In, std::move(cpam_aug_map_));
  // }

  auto Contain(K const k) const
  {
    pb_tree::MembershipLogger logger;
    auto res = this->pma_tree_->has(k);
    return std::make_pair(res, logger);
  }

  void DeleteTree()
  {
    // auto tmp = std::move(this->pma_tree_);
    this->pma_tree_.reset();
    this->pma_tree_ = std::make_shared<SPMA>();
  }

  auto GetSize() const
  {
    return this->pma_tree_.size();
  }

  constexpr static char const* GetTreeName()
  {
    return "SPMA";
  }

  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }
};

} // namespace CPAMTree
