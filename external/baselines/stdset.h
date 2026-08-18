#pragma once

#include <cstdint>
#include <set>
#include <utility>

#include "pab_tree/b_tree.h"

namespace StdSetTree {
using parlay::sequence;

// A baseline index backed by std::set (a balanced binary search tree).
// Batch insert / batch delete are implemented as plain serial loops over the
// input, so this serves as a single-threaded ordered-set reference point for
// the parallel B-tree benchmarks.
template<class TT>
class stdset_raw
{
public:
  using type_trait = TT;

  using entry_type = typename type_trait::entry_type;
  using key_type = typename type_trait::key_type;
  using mapped_type = typename type_trait::mapped_type;
  using entry_traits = typename type_trait::entry_traits;

  // Order elements by key only, so the set behaves as an ordered map on keys.
  struct key_less
  {
    bool operator()(entry_type const& a, entry_type const& b) const
    {
      return entry_traits::get_key(a) < entry_traits::get_key(b);
    }
  };

  using set_type = std::set<entry_type, key_less>;

  // The framework declares `Tree::leaf_type` / `Tree::interior_type` aliases (used only
  // for size reporting in some paths); std::set has no such notion, so map both
  // to the element type to satisfy the interface.
  using leaf_type = entry_type;
  using interior_type = entry_type;

  // The framework feeds whatever parse_input / parse_delete_input return straight
  // back into build / batch_insert / batch_delete, so we just keep the element
  // (resp. key) sequences as-is.
  using input_range_type = typename type_trait::entry_seq;
  using input_delete_range_type = typename type_trait::key_seq;

  static auto parse_input(typename type_trait::entry_seq const& input_seq) -> input_range_type
  {
    return input_seq;
  }

  static auto parse_delete_input(typename type_trait::entry_seq const& input_seq)
    -> input_delete_range_type
  {
    return parlay::map(input_seq,
                       [&](auto e) -> key_type { return entry_traits::get_key(e); });
  }

  // build a probe element carrying only a key, for ordered lookups/erases.
  static entry_type key_probe(key_type const k)
  {
    return entry_traits::to_entry(k, mapped_type{});
  }

  // APIs --------------------------------------------------------------------

  template<typename R>
  void build(R&& In)
  {
    set_.clear();
    for (auto const& e : In) {
      set_.insert(e);
    }
  }

  template<typename R, typename... Args>
  void batch_insert(R&& In, Args&&...)
  {
    for (auto const& e : In) {
      set_.insert(e);
    }
  }

  template<typename R, typename... Args>
  void batch_delete(R&& In, Args&&...)
  {
    for (auto const& k : In) {
      set_.erase(key_probe(k));
    }
  }

  auto contain(key_type const k) const
  {
    return set_.find(key_probe(k)) != set_.end();
  }

  auto range_count(typename type_trait::box_type const query_box) const -> size_t
  {
    // half-open range [first, second)
    auto lo = set_.lower_bound(key_probe(query_box.first));
    auto hi = set_.lower_bound(key_probe(query_box.second));
    size_t cnt = static_cast<size_t>(std::distance(lo, hi));
    return cnt;
  }

  template<typename R>
  auto range_report(typename type_trait::box_type const query_box, R&& out) const -> size_t
  {
    // half-open range [first, second)
    auto lo = set_.lower_bound(key_probe(query_box.first));
    auto hi = set_.lower_bound(key_probe(query_box.second));
    size_t i = 0;
    for (auto it = lo; it != hi; ++it) {
      out[i++] = *it;
    }
    return i;
  }

  template<typename R>
  void get_entries(R& out)
  {
    size_t i = 0;
    for (auto const& e : set_) {
      out[i++] = e;
    }
  }

  void delete_tree()
  {
    set_.clear();
  }

  [[nodiscard]] size_t get_size() const
  {
    return set_.size();
  }
  constexpr static char const* get_tree_name()
  {
    return "StdSet";
  }
  constexpr static int get_leaf_size()
  {
    return 1;
  }
  constexpr static char const* check_has_box()
  {
    return "HasBox";
  }

  set_type set_;
};

} // namespace StdSetTree
