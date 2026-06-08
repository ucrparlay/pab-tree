#pragma once

#include <cstdint>
#include <set>
#include <utility>

#include "pb_tree/b_tree.h"

namespace StdSetTree {
using parlay::sequence;

// A baseline index backed by std::set (a balanced binary search tree).
// Batch insert / batch delete are implemented as plain serial loops over the
// input, so this serves as a single-threaded ordered-set reference point for
// the parallel B-tree benchmarks.
template<class TypeTrait>
class StdSetRaw
{
public:
  using TT = TypeTrait;

  using E = typename TT::E;
  using K = typename TT::K;
  using V = typename TT::V;
  using Entry = typename TT::Entry;

  // Order elements by key only, so the set behaves as an ordered map on keys.
  struct KeyLess
  {
    bool operator()(E const& a, E const& b) const
    {
      return Entry::GetKey(a) < Entry::GetKey(b);
    }
  };

  using SetType = std::set<E, KeyLess>;

  // The framework declares `Tree::Leaf` / `Tree::Interior` aliases (used only
  // for size reporting in some paths); std::set has no such notion, so map both
  // to the element type to satisfy the interface.
  using Leaf = E;
  using Interior = E;

  // The framework feeds whatever ParseInput / ParseDeleteInput return straight
  // back into Build / BatchInsert / BatchDelete, so we just keep the element
  // (resp. key) sequences as-is.
  using InputRangeT = typename TT::EntrySeq;
  using InputDeleteRangeT = typename TT::KeySeq;

  static auto ParseInput(typename TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return input_seq;
  }

  static auto ParseDeleteInput(typename TT::EntrySeq const& input_seq)
    -> InputDeleteRangeT
  {
    return parlay::map(input_seq,
                       [&](auto e) -> K { return Entry::GetKey(e); });
  }

  // Build a probe element carrying only a key, for ordered lookups/erases.
  static E KeyProbe(K const k)
  {
    return Entry::ToEntry(k, V{});
  }

  // APIs --------------------------------------------------------------------

  template<typename Range>
  void Build(Range&& In)
  {
    set_.clear();
    for (auto const& e : In) {
      set_.insert(e);
    }
  }

  template<typename Range, typename... Args>
  void BatchInsert(Range&& In, Args&&...)
  {
    for (auto const& e : In) {
      set_.insert(e);
    }
  }

  template<typename Range, typename... Args>
  void BatchDelete(Range&& In, Args&&...)
  {
    for (auto const& k : In) {
      set_.erase(KeyProbe(k));
    }
  }

  auto Contain(K const k) const
  {
    pb_tree::RangeQueryLogger logger;
    bool res = set_.find(KeyProbe(k)) != set_.end();
    return std::make_pair(res, logger);
  }

  auto RangeCount(typename TT::Box const query_box) const
    -> std::pair<size_t, pb_tree::RangeReportLogger>
  {
    // half-open range [first, second)
    pb_tree::RangeReportLogger logger;
    auto lo = set_.lower_bound(KeyProbe(query_box.first));
    auto hi = set_.lower_bound(KeyProbe(query_box.second));
    size_t cnt = static_cast<size_t>(std::distance(lo, hi));
    return { cnt, logger };
  }

  template<typename Range>
  auto RangeReport(typename TT::Box const query_box, Range&& out) const
    -> std::pair<size_t, pb_tree::RangeReportLogger>
  {
    // half-open range [first, second)
    pb_tree::RangeReportLogger logger;
    auto lo = set_.lower_bound(KeyProbe(query_box.first));
    auto hi = set_.lower_bound(KeyProbe(query_box.second));
    size_t i = 0;
    for (auto it = lo; it != hi; ++it) {
      out[i++] = *it;
    }
    return { i, logger };
  }

  template<typename Range>
  void GetEntries(Range& out)
  {
    size_t i = 0;
    for (auto const& e : set_) {
      out[i++] = e;
    }
  }

  void DeleteTree()
  {
    set_.clear();
  }

  [[nodiscard]] size_t GetSize() const
  {
    return set_.size();
  }
  constexpr static char const* GetTreeName()
  {
    return "StdSet";
  }
  constexpr static int GetLeafSize()
  {
    return 1;
  }
  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }

  SetType set_;
};

} // namespace StdSetTree
