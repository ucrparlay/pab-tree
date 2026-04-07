#pragma once

#include <cstdint>

#include "PMA/CPMA.hpp"
#include "PMA/internal/leaf.hpp"
#include "StructOfArrays/multipointer.hpp"
#include "dependence/loggers.h"
#include "parlay/primitives.h"
#include "pmos/b_tree.h"

namespace PMA {

template<class TypeTrait>
class PmaWrapper
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
  using PmaLeaf = uncompressed_leaf<K, V>;
  using PmosPmaSettings = PMA_traits<PmaLeaf, InPlace, 0, false, false>;
  using PmaPointer = PmaLeaf::element_ptr_type;
  // static_assert(!PmaLeaf::binary);
  // static_assert(PmosPmaSettings::parallel);
  using PMA = CPMA<PmosPmaSettings>;
  std::shared_ptr<PMA> pma_tree_;

  using InputRangeT = std::pair<parlay::sequence<K>, parlay::sequence<V>>;
  using InputDeleteRangeT = parlay::sequence<K>;

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return { parlay::map(input_seq,
                         [&](auto e) { return Entry::GetKey(e) + 1; }),
             parlay::map(input_seq, [&](auto e) { return Entry::GetVal(e); }) };
  }

  static auto ParseDeleteInput(TT::EntrySeq const& input_seq)
    -> InputDeleteRangeT
  {
    return parlay::map(input_seq,
                       [&](auto e) -> K { return Entry::GetKey(e); });
  }

  template<class PT>
  void map_init(PT P)
  {
    assert(parlay::all_of(P.first, [](auto x) { return x != 0; }));
    size_t n = P.first.size();
    PmaPointer it(P.first.begin(), P.second.begin());
    pma_tree_->insert_batch(it, n, false);
  }

  template<typename PT>
  void map_insert(PT P)
  {
    assert(parlay::all_of(P.first, [](auto x) { return x != 0; }));
    size_t n = P.first.size();
    // parlay::sort_inplace(parlay::make_slice(P.first));
    // parlay::sort_inplace(parlay::make_slice(P.second));
    PmaPointer it(P.first.begin(), P.second.begin());
    pma_tree_->insert_batch(it, n, false);
    // std::cout << "PMA insert done. size: " << pma_tree_.size() << std::endl;
  }

  template<typename PT>
  void map_delete(PT P)
  {
    assert(parlay::all_of(P, [](auto x) { return x != 0; }));
    size_t n = P.size();
    pma_tree_->remove_batch(P.data(), n, false);
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

  template<typename Range>
  void BatchDelete(Range In)
  {
    map_delete(std::forward<Range>(In));
  }

  auto Contain(K const k) const
  {
    pmos::MembershipLogger logger;
    auto res = this->pma_tree_->has(k);
    return std::make_pair(res, logger);
  }

  template<typename Range>
  auto RangeReport(TT::Box const query_box,
                   Range&& out) -> std::pair<size_t, pmos::RangeReportLogger>
  {
    // the bounding box should be [a, b)
    pmos::RangeReportLogger logger;

    auto cnt = 0;
    auto f = [&](auto ele) {
      if constexpr (std::is_same_v<decltype(ele), typename TT::K>) {
        // Binary case: ele is just the key
        out[cnt++] = ele;
      } else {
        // Key-value case: ele is a tuple
        out[cnt].key = std::get<0>(ele);
        out[cnt].val = std::get<1>(ele);
        cnt++;
      }
    };
    auto rt_size =
      this->pma_tree_->map_range(f, query_box.first, query_box.second);
    return { rt_size, logger };
  }

  void DeleteTree()
  {
    // auto tmp = std::move(this->pma_tree_);
    this->pma_tree_.reset();
    this->pma_tree_ = std::make_shared<PMA>();
  }

  auto GetSize() const
  {
    return this->pma_tree_.size();
  }

  constexpr static char const* GetTreeName()
  {
    return "PMA";
  }

  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }
};

} // namespace CPAMTree
