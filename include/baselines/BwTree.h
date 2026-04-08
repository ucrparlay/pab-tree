#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "dependence/loggers.h"
#include "parlay/primitives.h"
#include "pmos/b_tree.h"

#include "../../external/BwTree/src/bwtree.h"

namespace BwTreeBaseline {

template<class TypeTrait>
class BwTreeWrapper
{
public:
  using TT = TypeTrait;

  using E = typename TT::E;
  using K = typename TT::K;
  using V = typename TT::V;
  using Entry = typename TT::Entry;

  using Num = typename TT::Num;
  using EntrySlice = typename TT::EntrySlice;
  using Split = typename TT::Split;
  using SplitSeq = typename TT::SplitSeq;

  using Leaf = void*;
  using Interior = void*;

#ifdef BWTREE_PELOTON
  using RawBwTree = peloton::index::BwTree<K, V, std::less<K>, std::equal_to<K>>;
#else
  using RawBwTree =
    wangziqi2013::bwtree::BwTree<K, V, std::less<K>, std::equal_to<K>>;
#endif

  using InputRangeT = std::pair<parlay::sequence<K>, parlay::sequence<V>>;
  using InputDeleteRangeT = parlay::sequence<K>;

  BwTreeWrapper()
    : tree_(std::make_shared<RawBwTree>(true))
  {
    tree_->UpdateThreadLocal(1);
    tree_->AssignGCID(0);
  }

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return { parlay::map(input_seq, [&](auto e) { return Entry::GetKey(e); }),
             parlay::map(input_seq, [&](auto e) { return Entry::GetVal(e); }) };
  }

  static auto ParseDeleteInput(TT::EntrySeq const& input_seq)
    -> InputDeleteRangeT
  {
    return parlay::map(input_seq,
                       [&](auto e) -> K { return Entry::GetKey(e); });
  }

  template<typename Range>
  void Build(Range&& In)
  {
    DeleteTree();

    auto const n = In.first.size();
    {
      std::lock_guard<std::mutex> lock(value_cache_mu_);
      for (size_t i = 0; i < n; ++i) {
        value_cache_[In.first[i]] = In.second[i];
      }
    }

    // BwTree does not expose a dedicated bulk-load API. Build by running
    // inserts concurrently instead of serial insertion.
    RunConcurrent(n, [&](size_t i) { tree_->Insert(In.first[i], In.second[i]); });
  }

  template<typename Range>
  void BatchInsert(Range&& In)
  {
    auto const n = In.first.size();
    {
      std::lock_guard<std::mutex> lock(value_cache_mu_);
      for (size_t i = 0; i < n; ++i) {
        value_cache_[In.first[i]] = In.second[i];
      }
    }

    RunConcurrent(n, [&](size_t i) { tree_->Insert(In.first[i], In.second[i]); });
  }

  template<typename Range>
  void BatchDelete(Range&& In)
  {
    std::vector<std::pair<K, V>> delete_pairs;
    delete_pairs.reserve(In.size());
    {
      std::lock_guard<std::mutex> lock(value_cache_mu_);
      for (size_t i = 0; i < In.size(); ++i) {
        auto it = value_cache_.find(In[i]);
        if (it != value_cache_.end()) {
          delete_pairs.emplace_back(it->first, it->second);
        }
      }
    }

    RunConcurrent(delete_pairs.size(), [&](size_t i) {
      tree_->Delete(delete_pairs[i].first, delete_pairs[i].second);
    });

    {
      std::lock_guard<std::mutex> lock(value_cache_mu_);
      for (auto const& [k, _] : delete_pairs) {
        value_cache_.erase(k);
      }
    }
  }

  auto Contain(K const k) const
  {
    pmos::MembershipLogger logger;
    std::vector<V> values;
    tree_->GetValue(k, values);
    return std::make_pair(!values.empty(), logger);
  }

  auto RangeCount(TT::Box const query_box)
    -> std::pair<size_t, pmos::RangeReportLogger>
  {
    pmos::RangeReportLogger logger;
    size_t count = 0;
    auto it = tree_->Begin(query_box.first);
    while (!it.IsEnd()) {
      auto const& kv = *it;
      if (kv.first >= query_box.second) {
        break;
      }
      ++count;
      ++it;
    }
    return { count, logger };
  }

  template<typename Range>
  auto RangeReport(TT::Box const query_box, Range&& out)
    -> std::pair<size_t, pmos::RangeReportLogger>
  {
    pmos::RangeReportLogger logger;
    size_t count = 0;
    auto it = tree_->Begin(query_box.first);
    while (!it.IsEnd()) {
      auto const& kv = *it;
      if (kv.first >= query_box.second) {
        break;
      }
      out[count].key = kv.first;
      out[count].val = kv.second;
      ++count;
      ++it;
    }
    return { count, logger };
  }

  void DeleteTree()
  {
    tree_.reset();
    tree_ = std::make_shared<RawBwTree>(true);
    tree_->UpdateThreadLocal(1);
    tree_->AssignGCID(0);

    std::lock_guard<std::mutex> lock(value_cache_mu_);
    value_cache_.clear();
  }

  auto GetSize() const
  {
    std::lock_guard<std::mutex> lock(value_cache_mu_);
    return value_cache_.size();
  }

  constexpr static char const* GetTreeName()
  {
    return "BwTree";
  }

  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }

private:
  template<typename Fn>
  void RunConcurrent(size_t n, Fn&& fn)
  {
    if (n == 0) {
      return;
    }

    auto const hw = std::thread::hardware_concurrency();
    auto const worker_num = std::min<size_t>(n, hw == 0 ? 1 : hw);

    tree_->UpdateThreadLocal(worker_num);

    std::vector<std::thread> workers;
    workers.reserve(worker_num);
    for (size_t tid = 0; tid < worker_num; ++tid) {
      workers.emplace_back([&, tid] {
        tree_->AssignGCID(static_cast<int>(tid));
        auto const start = (n * tid) / worker_num;
        auto const end = (n * (tid + 1)) / worker_num;
        for (size_t i = start; i < end; ++i) {
          fn(i);
        }
        tree_->UnregisterThread(static_cast<int>(tid));
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }

    tree_->UpdateThreadLocal(1);
    tree_->AssignGCID(0);
  }

  std::shared_ptr<RawBwTree> tree_;

  mutable std::mutex value_cache_mu_;
  std::map<K, V, std::less<K>> value_cache_;
};

} // namespace BwTreeBaseline
