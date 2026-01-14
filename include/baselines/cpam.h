#pragma once

#include <cstdint>

#include "CPAM/include/cpam/cpam.h"
#include "pmos/b_tree.h"
// #include "geobase.h"
// #include "hilbert.h"

namespace CPAMTree {
using namespace std;
using namespace cpam;
// using namespace geobase;
using parlay::par_do;
using parlay::par_do_if;
using parlay::sequence;

template<class TypeTrait>
class CpamRaw
{
public:
  using TT = TypeTrait;
  // TODO: Do we really need to define these types again?

  using E = typename TT::E;
  using K = typename TT::K;
  using V = typename TT::V;
  using Entry = TT::Entry;

  using Num = typename TT::Num;
  using EntrySlice = typename TT::EntrySlice;
  using Split = typename TT::Split;
  using SplitSeq = typename TT::SplitSeq;

  // begin of CPAM_RAW definition
  using key_type = K;
  using val_type = V;
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

  using zmap = cpam::aug_map<entry, 32>;
  using par = std::tuple<typename entry::key_t, typename entry::val_t>;

  using Leaf = zmap::Tree::node;
  using Interior = zmap::Tree::node;

  using InputRangeT = parlay::sequence<par>;

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return parlay::map(input_seq, [&](auto e) -> par {
      return { Entry::GetKey(e), Entry::GetVal(e) };
    });
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

  template<typename Range>
  auto map_init(Range entries)
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
  template<typename Range, typename M>
  auto map_insert(Range P, M&& mmp)
  {
    return std::move(zmap::multi_insert(std::forward<M>(mmp), P));
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
  // int mbr_mbr_relation(Box const& a, Box const& b) {
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
  template<typename Range>
  void Build(Range&& In)
  {
    cpam_aug_map_ = std::move(map_init(std::forward<Range>(In)));
  }

  template<typename Range>
  void BatchInsert(Range&& In)
  {
    if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
      return Build(std::forward<Range>(In));
    }
    cpam_aug_map_ = map_insert(In, std::move(cpam_aug_map_));
  }
  //
  // void BatchDelete(Slice In) {
  //   if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
  //     return;
  //   }
  //   cpam_aug_map_ = map_delete(In, std::move(cpam_aug_map_));
  // }

  // template <typename Node, typename Range>
  // auto KNN(Node* T, Point const& q, psi::kBoundedQueue<Point, Range>& bq) {
  //   psi::KNNLogger logger;
  //   // this->knn(this->cpam_aug_map_, q, bq.max_size(), logger.vis_leaf_num);
  //   zmap::template knn<BT>(this->cpam_aug_map_, q, bq, logger);
  //   return logger;
  // }
  //

  auto Contain(K const k) const
  {
    pmos::RangeQueryLogger logger;
    auto res = this->cpam_aug_map_.contains(k);
    return std::make_pair(res, logger);
  }
  // auto RangeCount(Box const& q) {
  //   psi::RangeQueryLogger logger;
  //   auto size = this->range_count(this->cpam_aug_map_, q);
  //   return std::make_pair(size, logger);
  // }
  //
  // template <typename Range>
  // auto RangeQuery(Box const& q, Range&& Out) {
  //   psi::RangeQueryLogger logger;
  //   auto size = this->range_report(this->cpam_aug_map_, q, Out);
  //   return std::make_pair(size, logger);
  // }

  void DeleteTree()
  {
    cpam_aug_map_.clear();
  }

  [[nodiscard]] size_t GetSize() const
  {
    return cpam_aug_map_.size();
  }
  constexpr static char const* GetTreeName()
  {
    return "CPAM";
  }
  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }
  zmap cpam_aug_map_;
};

} // namespace CPAMTree
