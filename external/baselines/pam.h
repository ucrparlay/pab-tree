#pragma once

#include <cstdint>

#include "pab_tree/b_tree.h"
#include <pam.h>
// #include "geobase.h"
// #include "hilbert.h"

namespace PAMTree {
using namespace std;
using namespace pam;
// using namespace geobase;
using parlay::par_do;
using parlay::par_do_if;
using parlay::sequence;

template<class TT>
class PamRaw
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

  // begin of CPAM_RAW definition; key_type above is already what PAM wants
  using val_type = mapped_type;
  using aug_type = size_t; // aug size of the tree

  //	CPAM entry
  struct entry
  {
    using key_t = key_type;
    using val_t = val_type;
    using aug_t = aug_type;

    static inline bool comp(key_t a, key_t b)
    {
      return a < b;
    }
    static aug_t get_empty()
    {
      return 0;
    }
    static aug_t from_entry(key_t k, val_t v)
    {
      return 1;
    }
    static aug_t combine(aug_t a, aug_t b)
    {
      return a + b;
    }
  };

  using zmap = aug_map<entry>;
  using par = std::pair<typename entry::key_t, typename entry::val_t>;

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
    return std::move(zmap::multi_insert(std::forward<M>(mmp), P));
  }

  template<typename R, typename M>
  auto map_delete(R P, M&& mmp)
  {
    return std::move(zmap::multi_delete(std::forward<M>(mmp), P));
  }
  //
  // template <typename PT, typename M>
  // auto map_delete(PT P, M&& mmp) {
  //   size_t n = P.size();
  //
  //   parlay::parallel_for(
  //       0, n, [&](int i) { P[i].aug.code = SplitRule::Encode(P[i]); });
  //   parlay::sequence<par> delete_pts(n);
  //   // parlay::sequence<pair<unsigned long long, long long> > delete_pts(n);
  //   parlay::parallel_for(0, n, [&](int i) {
  //     delete_pts[i] = {{P[i].aug.code, P[i].aug.id}, P[i]};
  //     // delete_pts[i] = {P[i]->morton_id, P[i]->id};
  //     // insert_pts[i] = {P[i]->id, P[i]};
  //   });
  //
  //   return std::move(zmap::multi_delete(std::forward<M>(mmp), delete_pts));
  // }
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
  // template <class T, class MBR, typename Out>
  // auto range_report(T& tree, MBR query_mbr, Out&& out) {
  //   // auto ret = zmap::values(filter_range(tree, query_mbr, use_hilbert));
  //   auto f = [&](auto cur) { return mbr_mbr_relation(cur, query_mbr); };
  //
  //   int64_t ret = 0;
  //   // zmap::range_report_filter(tree, f, ret, out);
  //   zmap::range_report_filter2(tree, f, ret, out);
  //   return ret;
  // }

  // APIs
  template<typename R>
  void build(R&& In)
  {
    pam_aug_map_ = std::move(map_init(std::forward<R>(In)));
  }

  template<typename R>
  void batch_insert(R&& In)
  {
    if (!this->pam_aug_map_.root || !zmap::size(this->pam_aug_map_.root)) {
      return build(std::forward<R>(In));
    }
    pam_aug_map_ = map_insert(std::forward<R>(In), std::move(pam_aug_map_));
  }

  template<typename R>
  void batch_delete(R&& In)
  {
    if (!this->pam_aug_map_.root || !zmap::size(this->pam_aug_map_.root)) {
      return;
    }
    pam_aug_map_ = map_delete(std::forward<R>(In), std::move(pam_aug_map_));
  }

  // template <typename node_type, typename R>
  // auto KNN(node_type* T, Point const& q, psi::kBoundedQueue<Point, R>& bq) {
  //   psi::KNNLogger logger;
  //   // this->knn(this->cpam_aug_map_, q, bq.max_size(), logger.vis_leaf_num);
  //   zmap::template knn<BT>(this->cpam_aug_map_, q, bq, logger);
  //   return logger;
  // }
  //
  // auto range_count(box_type const& q) {
  //   psi::RangeQueryLogger logger;
  //   auto size = this->range_count(this->cpam_aug_map_, q);
  //   return std::make_pair(size, logger);
  // }
  //
  // template <typename R>
  // auto range_query(box_type const& q, R&& out) {
  //   psi::RangeQueryLogger logger;
  //   auto size = this->range_report(this->cpam_aug_map_, q, out);
  //   return std::make_pair(size, logger);
  // }

  auto contain(key_type const k) const
  {
    return this->pam_aug_map_.contains(k);
  }

  // template<class T, class MBR>
  // auto RangeCountImpl(T& tree, MBR query_mbr)
  // {
  //   // auto ret = zmap::values(filter_range(tree, query_mbr, use_hilbert));
  //   auto f = [&](type_trait::key_type cur_key) {
  //     if (cur_key < query_mbr.first) { // query box is to the right
  //       return -1;
  //     } else if (cur_key >= query_mbr.second) { // query box is to the left
  //       return 1;
  //     } else { // within the box
  //       return 0;
  //     }
  //   };
  //
  //   int64_t ret = 0;
  //   zmap::range_count(tree, f, ret);
  //   return ret;
  // }

  auto range_count(type_trait::box_type const query_box) -> size_t
  {
    // the bounding box should be [a, b)
    // auto rt_size = this->RangeCountImpl(this->pam_aug_map_, query_box);

    auto rt_tree =
      zmap::range(this->pam_aug_map_, query_box.first, query_box.second);
    auto rt_size = zmap::size(rt_tree.root);
    return rt_size;
  }

  template<class T, class MBR, typename Out>
  auto range_report_impl(T& tree, MBR query_mbr, Out&& out)
  {
    // auto ret = zmap::values(filter_range(tree, query_mbr, use_hilbert));
    auto f = [&](type_trait::key_type cur_key) {
      if (cur_key < query_mbr.first) { // query box is to the right
        return -1;
      } else if (cur_key >= query_mbr.second) { // query box is to the left
        return 1;
      } else { // within the box
        return 0;
      }
    };

    int64_t ret = 0;
    zmap::range_report_filter2(tree, f, ret, out);
    return ret;
  }

  template<typename R>
  auto range_report(type_trait::box_type const query_box, R&& out) -> size_t
  {
    // the bounding box should be [a, b)
    auto rt_size = this->range_report_impl(this->pam_aug_map_, query_box, out);
    return rt_size;
  }

  template<typename R>
  void get_entries(R& out)
  {
    zmap::foreach_index(this->pam_aug_map_, [&](auto const& e, size_t i) {
      out[i] = { std::get<0>(e), std::get<1>(e) };
    });
  }

  void delete_tree()
  {
    pam_aug_map_.clear();
  }

  size_t get_size() const
  {
    return pam_aug_map_.size();
  }
  constexpr static char const* get_tree_name()
  {
    return "PAM";
  }
  constexpr static int get_leaf_size()
  {
    return 1;
  }
  constexpr static char const* check_has_box()
  {
    return "HasBox";
  }
  zmap pam_aug_map_;
};

} // namespace PAMTree
