#pragma once

#include <cstdint>
#include <utility>

#include "pb_tree/b_tree.h"
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

  constexpr static size_t kCpamBufferSize = 64;
  using zmap = cpam::aug_map<entry, kCpamBufferSize>;
  using par = std::tuple<typename entry::key_t, typename entry::val_t>;

  using Leaf = zmap::Tree::node;
  using Interior = zmap::Tree::node;

  using InputRangeT = parlay::sequence<par>;
  using InputDeleteRangeT = parlay::sequence<K>;

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return parlay::map(input_seq, [&](auto e) -> par {
      return { Entry::GetKey(e), Entry::GetVal(e) };
    });
  }

  static auto ParseDeleteInput(TT::EntrySeq const& input_seq)
    -> InputDeleteRangeT
  {
    return parlay::map(input_seq,
                       [&](auto e) -> K { return Entry::GetKey(e); });
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

  template<typename Range, typename M>
  auto map_delete(Range P, M&& mmp)
  {
    return std::move(zmap::multi_delete(std::forward<M>(mmp), P));
  }
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

  template<typename Range>
  void BatchDelete(Range&& In)
  {
    if (!this->cpam_aug_map_.root || !zmap::size(this->cpam_aug_map_.root)) {
      return;
    }
    cpam_aug_map_ = map_delete(In, std::move(cpam_aug_map_));
  }

  auto Contain(K const k) const
  {
    pb_tree::RangeQueryLogger logger;
    auto res = this->cpam_aug_map_.contains(k);
    return std::make_pair(res, logger);
  }

  int mbr_mbr_relation(TT::K cur_key, TT::Box const& box)
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
  //   auto f = [&](TT::K cur) { return mbr_mbr_relation(cur, query_mbr); };
  //
  //   int64_t ret = 0;
  //   zmap::range_count(tree, f, ret, logger);
  //   return ret;
  // }

  auto RangeCount(TT::Box const query_box)
    -> std::pair<size_t, pb_tree::RangeReportLogger>
  {
    // the bounding box should be [a, b)
    pb_tree::RangeReportLogger logger;
    // auto rt_size = this->RangeCountImpl(this->cpam_aug_map_, query_box,
    // logger);
    auto rt_tree =
      zmap::range(this->cpam_aug_map_, query_box.first, query_box.second);
    auto rt_size = zmap::size(rt_tree.root);
    return { rt_size, logger };
  }

  template<class T, class MBR, typename Out, typename Logger>
  auto RangeReportImpl(T& tree, MBR query_mbr, Out&& out, Logger& logger)
  {
    // auto ret = zmap::values(filter_range(tree, query_mbr, use_hilbert));
    auto f = [&](TT::K cur) { return mbr_mbr_relation(cur, query_mbr); };

    int64_t ret = 0;
    zmap::range_report_filter2(tree, f, ret, out, logger);
    return ret;
  }

  template<typename Range>
  auto RangeReport(TT::Box const query_box, Range&& out)
    -> std::pair<size_t, pb_tree::RangeReportLogger>
  {
    // the bounding box should be [a, b)
    pb_tree::RangeReportLogger logger;
    auto rt_size =
      this->RangeReportImpl(this->cpam_aug_map_, query_box, out, logger);
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
    return { rt_size, logger };
  }

  void DeleteTree()
  {
    cpam_aug_map_.clear();
    cpam_aug_map_.root = nullptr;
  }

  [[nodiscard]] size_t GetSize() const
  {
    return cpam_aug_map_.size();
  }
  constexpr static char const* GetTreeName()
  {
    return "CPAM";
  }
  constexpr static int GetLeafSize()
  {
    return kCpamBufferSize;
  }
  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }
  zmap cpam_aug_map_;
};

} // namespace CPAMTree
