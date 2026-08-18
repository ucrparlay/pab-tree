#pragma once

#include <cstdint>

#include "PMA/CPMA.hpp"
#include "PMA/internal/leaf.hpp"
#include "StructOfArrays/multipointer.hpp"
#include "parlay/primitives.h"
#include "pab_tree/b_tree.h"

namespace SPMA {

template<class TT>
class SpmaWrapper
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
  using SpmaLeaf = uncompressed_leaf<key_type, mapped_type>;
  using PmosSpmaSettings = PMA_traits<SpmaLeaf, BNary, 17, false, false>;
  using SpmaPointer = SpmaLeaf::element_ptr_type;
  static_assert(!SpmaLeaf::binary);
  static_assert(PmosSpmaSettings::parallel);
  using SPMA = CPMA<PmosSpmaSettings>;
  std::shared_ptr<SPMA> pma_tree_;

  using input_range_type = std::pair<parlay::sequence<key_type>, parlay::sequence<mapped_type>>;

  static auto parse_input(type_trait::entry_seq const& input_seq) -> input_range_type
  {
    return { parlay::map(input_seq,
                         [&](auto e) { return entry_traits::get_key(e) + 1; }),
             parlay::map(input_seq, [&](auto e) { return entry_traits::get_val(e); }) };
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
  //
  // void batch_delete(Slice In) {
  //   if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
  //     return;
  //   }
  //   cpam_aug_map_ = map_delete(In, std::move(cpam_aug_map_));
  // }

  auto contain(key_type const k) const
  {
    return this->pma_tree_->has(k);
  }

  void delete_tree()
  {
    // auto tmp = std::move(this->pma_tree_);
    this->pma_tree_.reset();
    this->pma_tree_ = std::make_shared<SPMA>();
  }

  auto get_size() const
  {
    return this->pma_tree_.size();
  }

  constexpr static char const* get_tree_name()
  {
    return "SPMA";
  }

  constexpr static char const* check_has_box()
  {
    return "HasBox";
  }
};

} // namespace CPAMTree
