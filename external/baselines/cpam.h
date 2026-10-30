#pragma once

#include <cstdint>
#include <utility>

#include "pab_tree/b_tree.h"
#include <cpam.h>
// #include "geobase.h"
// #include "hilbert.h"

namespace CPAMTree {
using namespace std;
using namespace cpam;
// using namespace geobase;
using parlay::par_do;
using parlay::par_do_if;
using parlay::sequence;

template<class TT>
class cpam_raw
{
public:
  using type_trait = TT;
  // TODO: Do we really need to define these types again?

  using entry_type = typename type_trait::entry_type;
  using key_type = typename type_trait::key_type;
  using mapped_type = typename type_trait::mapped_type;
  using entry_traits = type_trait::entry_traits;

  using num_comparator = typename type_trait::num_comparator;
  using entry_slice = typename type_trait::entry_slice;
  using split_type = typename type_trait::split_type;
  using split_seq = typename type_trait::split_seq;

  // begin of CPAM_RAW definition; key_type above is already what CPAM wants
  using val_type = mapped_type;
  using aug_type = size_t; // aug size of the tree

  //	CPAM entry
  struct entry
  {
    using key_t = key_type;
    using val_t = val_type;
    using aug_t = aug_type;

    static bool comp(key_t a, key_t b)
    {
      return a < b;
    }
    static aug_t get_empty()
    {
      return 0;
    }
    static aug_t from_entry([[maybe_unused]] key_t k, [[maybe_unused]] val_t v)
    {
      return 1;
    }
    static aug_t combine(aug_t a, aug_t b)
    {
      return a + b;
    }
  };

  constexpr static size_t kCpamBufferSize = 64;
  using zmap = cpam::aug_map<entry, kCpamBufferSize>;
  using par = std::tuple<typename entry::key_t, typename entry::val_t>;

  using leaf_type = zmap::Tree::node;
  using interior_type = zmap::Tree::node;

  using input_range_type = parlay::sequence<par>;
  using input_delete_range_type = parlay::sequence<key_type>;

  static auto parse_input(type_trait::entry_seq const& input_seq) -> input_range_type
  {
    return parlay::map(input_seq, [&](auto e) -> par {
      return { entry_traits::get_key(e), entry_traits::get_val(e) };
    });
  }

  static auto parse_delete_input(type_trait::entry_seq const& input_seq)
    -> input_delete_range_type
  {
    return parlay::map(input_seq,
                       [&](auto e) -> key_type { return entry_traits::get_key(e); });
  }
  // template <typename T>
  // auto knn(T& tree, Point const& query_point, auto k, auto& vis_leaf) {
  //   auto f = [&](auto cur_pt) {
  //     return BT::P2PDistanceSquare(cur_pt, query_point);
  //     // return point_point_sqrdis(cur_pt, query_point);
  //   };
  //
  //   auto f2 = [&](auto cur_mbr) {
  //     return BT::P2BMinDistanceSquare(query_point, cur_mbr);
  //     // return point_mbr_sqrdis(query_point, cur_mbr);
  //   };
  //
  //   std::priority_queue<nn_pair, vector<nn_pair>, nn_pair_cmp> nn_res;
  //   zmap::knn_filter(tree, f, f2, k, nn_res, vis_leaf);
  //   return nn_res.top().second;
  // }

  template<typename R>
  auto map_init(R entries)
  {
    zmap m1(entries);
    return std::move(m1);
  }

  // template <typename M>
  // auto map_diff(M& lhs, M& rhs) {
  //   auto add = zmap::values(zmap::map_difference(rhs, lhs));
  //   auto remove = zmap::values(zmap::map_difference(lhs, rhs));
  //   return make_tuple(add, remove);
  // }
  //
  template<typename R, typename M>
  auto map_insert(R P, M&& mmp)
  {
    return zmap::multi_insert(std::forward<M>(mmp), P);
  }

  template<typename R, typename M>
  auto map_delete(R P, M&& mmp)
  {
    return zmap::multi_delete(std::forward<M>(mmp), P);
  }
  //
  // int mbr_mbr_relation(box_type const& a, box_type const& b) {
  //   if (!BT::BoxIntersectBox(a, b)) {
  //     return -1;  // disjoint
  //   } else if (BT::WithinBox(a, b)) {
  //     return 1;
  //   } else {
  //     return 0;  // overlap
  //   }
  // }
  //

  // template <class T, class MBR>
  // auto range_count(T& zCPAM, MBR& query_mbr) {
  //   // auto f = [&](auto& cur) { return mbr_mbr_relation(cur, query_mbr); };
  //   // auto f2 = [&](auto& cur) { return point_in_mbr(cur, query_mbr); };
  //   auto f = [&](auto& cur_mbr) {
  //     return mbr_mbr_relation(cur_mbr, query_mbr);
  //   };
  //   auto f2 = [&](auto& cur_point) {
  //     return BT::WithinBox(cur_point, query_mbr);
  //   };
  //
  //   auto res = zmap::range_count_filter2(zCPAM, f, f2);
  //   return res;
  // }
  //

  // APIs
  template<typename R>
  void build(R&& In)
  {
    cpam_aug_map_ = std::move(map_init(std::forward<R>(In)));
  }

  template<typename R>
  void batch_insert(R&& In)
  {
    if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
      return build(std::forward<R>(In));
    }
    cpam_aug_map_ = map_insert(In, std::move(cpam_aug_map_));
  }

  template<typename R>
  void batch_delete(R&& In)
  {
    if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
      return;
    }
    cpam_aug_map_ = map_delete(In, std::move(cpam_aug_map_));
  }

  auto contain(key_type const k) const
  {
    return this->cpam_aug_map_.contains(k);
  }

  // CPAM's range walk reports visited nodes through a logger object; these
  // two counters are all it touches.
  struct visit_counter
  {
    size_t vis_leaf_num = 0;
    size_t vis_interior_num = 0;
  };

  int mbr_mbr_relation(type_trait::key_type cur_key, type_trait::box_type const& box)
  {
    if (cur_key < box.first) { // query box is to the right
      return -1;
    } else if (cur_key >= box.second) { // query box is to the left
      return 1;
    } else { // within the box
      return 0;
    }
  }

  // template<class T, class MBR, typename Logger>
  // auto RangeCountImpl(T& tree, MBR query_mbr, Logger& logger)
  // {
  //   // auto ret = zmap::values(filter_range(tree, query_mbr, use_hilbert));
  //   auto f = [&](TT::key_type cur) { return mbr_mbr_relation(cur, query_mbr); };
  //
  //   int64_t ret = 0;
  //   zmap::range_count(tree, f, ret, logger);
  //   return ret;
  // }

  auto range_count(type_trait::box_type const query_box) -> size_t
  {
    // the bounding box should be [a, b)
    // auto rt_size = this->RangeCountImpl(this->cpam_aug_map_, query_box,
    // logger);
    auto rt_tree =
      zmap::range(this->cpam_aug_map_, query_box.first, query_box.second);
    auto rt_size = zmap::size(rt_tree.root);
    return rt_size;
  }

  template<class T, class MBR, typename Out, typename Logger>
  auto range_report_impl(T& tree, MBR query_mbr, Out&& out, Logger& logger)
  {
    // auto ret = zmap::values(filter_range(tree, query_mbr, use_hilbert));
    auto f = [&](type_trait::key_type cur) { return mbr_mbr_relation(cur, query_mbr); };

    int64_t ret = 0;
    zmap::range_report_filter2(tree, f, ret, out, logger);
    return ret;
  }

  template<typename R>
  auto range_report(type_trait::box_type const query_box, R&& out) -> size_t
  {
    // the bounding box should be [a, b)
    visit_counter logger;
    auto rt_size =
      this->range_report_impl(this->cpam_aug_map_, query_box, out, logger);
    // auto rt_tree =
    //   zmap::range(this->cpam_aug_map_, query_box.first, query_box.second);
    // auto rt_size = zmap::size(rt_tree.root);
    // zmap::foreach_index(std::move(rt_tree), [&](auto& e, size_t i) {
    //   out[i] = { std::get<0>(e), std::get<1>(e) };
    // });
    // auto seq = zmap::entries(std::move(rt_tree));
    // parlay::parallel_for(0, seq.size(), [&](size_t i) {
    //   out[i] = { std::get<0>(seq[i]), std::get<1>(seq[i]) };
    // });
    return rt_size;
  }

  template<typename R>
  void get_entries(R& out)
  {
    zmap::foreach_index(this->cpam_aug_map_, [&](auto const& e, size_t i) {
      out[i] = { std::get<0>(e), std::get<1>(e) };
    });
  }

  void delete_tree()
  {
    cpam_aug_map_.clear();
    cpam_aug_map_.root = nullptr;
  }

  [[nodiscard]] size_t get_size() const
  {
    return cpam_aug_map_.size();
  }
  constexpr static char const* get_tree_name()
  {
    return "CPAM";
  }
  constexpr static int get_leaf_size()
  {
    return kCpamBufferSize;
  }
  constexpr static char const* check_has_box()
  {
    return "HasBox";
  }
  zmap cpam_aug_map_;
};

} // namespace CPAMTree
