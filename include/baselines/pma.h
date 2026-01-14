#pragma once

#include <cstdint>

#include "PMA/CPMA.hpp"
#include "PMA/internal/leaf.hpp"

namespace PMA {

template<class TypeTrait>
class PMA
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

  using PMA_Leaf = uncompressed_leaf<K, V>;
  using pmos_pma_settings = PMA_traits<PMA_Leaf, InPlace, 0, false, false>;
  static_assert(!PMA_Leaf::binary);
  CPMA<pmos_pma_settings> pma_tree_;

  template<class PT>
  void map_init(PT& P)
  {
    auto entries = parlay::map(P, [&](auto e) -> std::tuple<K, V> {
      {
        return { Entry::GetKey(e), Entry::GetValue(e) };
      }
    });
    // basically should return the insert num
    pma_tree_.insert_batch(entries.begin(), entries.size());
  }

  template<typename PT, typename M>
  void map_insert(PT& P)
  {
    auto entries = parlay::map(P, [&](auto e) -> std::tuple<K, V> {
      {
        return { Entry::GetKey(e), Entry::GetValue(e) };
      }
    });
    pma_tree_.insert_batch(entries.begin(), entries.size());
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
  void Build(Range In)
  {
    map_init(In);
  }

  void BatchInsert(EntrySlice In)
  {
    map_insert(In);
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
    this->pma_tree_.has(k);
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
    this->pma_tree_.~CPMA();
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
