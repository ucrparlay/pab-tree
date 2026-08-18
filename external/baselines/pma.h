#pragma once

#include <cstdint>

#include "PMA/CPMA.hpp"
#include "PMA/internal/leaf.hpp"
#include "StructOfArrays/multipointer.hpp"
#include "parlay/primitives.h"
#include "pab_tree/b_tree.h"

namespace PMA {

template<class TT>
class PmaWrapper
{
public:
  using type_trait = TT;

  using entry_type = typename type_trait::entry_type;
  using key_type = typename type_trait::key_type;
  using mapped_type = typename type_trait::mapped_type;
  using entry_traits = type_trait::entry_traits;

  using num_comparator = typename type_trait::num_comparator;
  using entry_slice = typename type_trait::entry_slice;
  using split_type = typename type_trait::split_type;
  using split_seq = typename type_trait::split_seq;

  using leaf_type = type_trait::node_type*;
  using interior_type = type_trait::node_type*;

  // using pma_leaf = uncompressed_leaf<key_type>;
  using pma_leaf = uncompressed_leaf<key_type, mapped_type>;
  // TODO: try
  // https://github.com/wheatman/Packed-Memory-Array/blob/69edd1831174682a1f702375542a8244e8faeccc/include/PMA/CPMA.hpp#L80
  using pmos_pma_settings = PMA_traits<pma_leaf, Eytzinger, 0, false, false>;
  using pma_pointer = pma_leaf::element_ptr_type;
  // static_assert(!pma_leaf::binary);
  // static_assert(pmos_pma_settings::parallel);
  using PMA = CPMA<pmos_pma_settings>;
  std::shared_ptr<PMA> pma_tree_;

  using input_range_type = std::pair<parlay::sequence<key_type>, parlay::sequence<mapped_type>>;
  using input_delete_range_type = parlay::sequence<key_type>;

  static auto parse_input(type_trait::entry_seq const& input_seq) -> input_range_type
  {
    return { parlay::map(input_seq,
                         [&](auto e) { return entry_traits::get_key(e) + 1; }),
             parlay::map(input_seq, [&](auto e) { return entry_traits::get_val(e); }) };
  }

  static auto parse_delete_input(type_trait::entry_seq const& input_seq)
    -> input_delete_range_type
  {
    return parlay::map(input_seq,
                       [&](auto e) -> key_type { return entry_traits::get_key(e) + 1; });
  }

  template<class PT>
  void map_init(PT P)
  {
    assert(parlay::all_of(P.first, [](auto x) { return x != 0; }));
    size_t n = P.first.size();
    auto range = parlay::tabulate(
      n, [&](size_t i) { return std::make_tuple(P.first[i], P.second[i]); });
    pma_tree_ = std::make_shared<PMA>(range);
  }

  template<typename PT>
  void map_insert(PT P)
  {
    if (this->pma_tree_ == nullptr || this->pma_tree_->size() == 0) {
      map_init(P);
      return;
    }
    assert(parlay::all_of(P.first, [](auto x) { return x != 0; }));
    size_t n = P.first.size();
    // parlay::sort_inplace(parlay::make_slice(P.first));
    // parlay::sort_inplace(parlay::make_slice(P.second));
    pma_pointer it(P.first.begin(), P.second.begin());
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
  template<typename R>
  void build(R&& In)
  {
    map_init(std::forward<R>(In));
  }

  template<typename R>
  void batch_insert(R&& In)
  {
    map_insert(std::forward<R>(In));
  }

  template<typename R>
  void batch_delete(R In)
  {
    map_delete(std::forward<R>(In));
  }

  auto contain(key_type const k) const
  {
    return this->pma_tree_->has(k);
  }

  auto range_count(type_trait::box_type const query_box) -> size_t
  {
    // the bounding box should be [a, b)
    auto cnt = 0;
    auto f = [&](auto ele) { cnt++; };
    auto rt_size =
      this->pma_tree_->map_range(f, query_box.first, query_box.second);
    return rt_size;
  }

  template<typename R>
  auto range_report(type_trait::box_type const query_box, R&& out) -> size_t
  {
    // the bounding box should be [a, b)
    auto cnt = 0;
    auto f = [&](auto ele) {
      if constexpr (std::is_same_v<decltype(ele), typename type_trait::key_type>) {
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
    return rt_size;
  }

  template<typename R>
  void get_entries(R& out)
  {
    auto n_leaves = this->pma_tree_->total_leaves();
    // Count exact per-leaf element counts using the same iteration logic as
    // the write pass, so prefix-sum offsets match what is actually written.
    parlay::sequence<uint64_t> counts(n_leaves);
    parlay::parallel_for(0, n_leaves, [&](uint64_t i) {
      uint64_t cnt = 0;
      this->pma_tree_->get_leaf(i).template map<true>([&cnt](auto) { cnt++; });
      counts[i] = cnt;
    });
    auto [offsets, total] = parlay::scan(counts);
    parlay::parallel_for(0, n_leaves, [&](uint64_t i) {
      uint64_t pos = offsets[i];
      this->pma_tree_->get_leaf(i).template map<true>([&](auto ele) {
        if constexpr (std::is_same_v<decltype(ele), typename type_trait::key_type>) {
          out[pos++] = ele;
        } else {
          out[pos].key = std::get<0>(ele);
          out[pos].val = std::get<1>(ele);
          pos++;
        }
      });
    });
  }

  void delete_tree()
  {
    // auto tmp = std::move(this->pma_tree_);
    this->pma_tree_.reset();
    this->pma_tree_ = std::make_shared<PMA>();
  }

  auto get_size() const
  {
    return this->pma_tree_->size();
  }

  constexpr static int get_leaf_size()
  {
    return 1;
  }
  constexpr static char const* get_tree_name()
  {
    return "PMA";
  }

  constexpr static char const* check_has_box()
  {
    return "HasBox";
  }
};

} // namespace CPAMTree
