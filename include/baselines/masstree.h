#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "dependence/loggers.h"
#include "parlay/primitives.h"
#include "pmos/b_tree.h"

// Masstree includes
#include "masstree.hh"
#include "masstree_tcursor.hh"
#include "masstree_insert.hh"
#include "masstree_get.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "kvthread.hh"

namespace MasstreeWrapper {

// Thread info required by Masstree
class SimpleThreadInfo {
public:
  threadinfo* ti_;
  
  SimpleThreadInfo() {
    ti_ = threadinfo::make(threadinfo::TI_MAIN, -1);
  }
  
  ~SimpleThreadInfo() {
    ti_->rcu_stop();
  }
  
  threadinfo* get() { return ti_; }
};

template<class TypeTrait>
class MasstreeWrapper
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

  using Leaf = void*;
  using Interior = void*;

  // Masstree specific types
  struct MasstreeParams : public Masstree::nodeparams<15, 15> {
    typedef V value_type;
    typedef Masstree::value_print<value_type> value_print_type;
    typedef ::threadinfo threadinfo_type;
  };
  
  using Table = Masstree::basic_table<MasstreeParams>;
  using Cursor = Masstree::tcursor<MasstreeParams>;
  using Str = Masstree::Str;
  
  std::unique_ptr<Table> table_;
  mutable SimpleThreadInfo thread_info_;

  using InputRangeT = std::pair<parlay::sequence<K>, parlay::sequence<V>>;
  using InputDeleteRangeT = parlay::sequence<K>;

  MasstreeWrapper() {
    table_ = std::make_unique<Table>();
    table_->initialize(*thread_info_.get());
  }

  ~MasstreeWrapper() {
    if (table_) {
      table_->destroy(*thread_info_.get());
    }
  }

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return { parlay::map(input_seq,
                         [&](auto e) { return Entry::GetKey(e); }),
             parlay::map(input_seq, [&](auto e) { return Entry::GetVal(e); }) };
  }

  static auto ParseDeleteInput(TT::EntrySeq const& input_seq)
    -> InputDeleteRangeT
  {
    return parlay::map(input_seq,
                       [&](auto e) -> K { return Entry::GetKey(e); });
  }

  // Convert key to Str for Masstree API
  inline Str key_to_str(K key) const {
    return Str(reinterpret_cast<const char*>(&key), sizeof(K));
  }

  template<class PT>
  void map_init(PT P)
  {
    size_t n = P.first.size();
    for (size_t i = 0; i < n; i++) {
      Cursor lp(*table_, key_to_str(P.first[i]));
      bool found = lp.find_insert(*thread_info_.get());
      if (!found) {
        lp.value() = P.second[i];
      }
      lp.finish(1, *thread_info_.get());
    }
  }

  template<typename PT>
  void map_insert(PT P)
  {
    size_t n = P.first.size();
    for (size_t i = 0; i < n; i++) {
      Cursor lp(*table_, key_to_str(P.first[i]));
      bool found = lp.find_insert(*thread_info_.get());
      if (!found) {
        lp.value() = P.second[i];
      }
      lp.finish(1, *thread_info_.get());
    }
  }

  template<typename PT>
  void map_delete(PT P)
  {
    size_t n = P.size();
    for (size_t i = 0; i < n; i++) {
      Cursor lp(*table_, key_to_str(P[i]));
      bool found = lp.find_locked(*thread_info_.get());
      if (found) {
        lp.finish(-1, *thread_info_.get());
      } else {
        lp.finish(0, *thread_info_.get());
      }
    }
  }

  // APIs
  template<typename Range>
  void Build(Range&& In)
  {
    map_init(std::forward<Range>(In));
  }

  template<typename Range>
  void BatchInsert(Range&& In)
  {
    map_insert(std::forward<Range>(In));
  }

  template<typename Range>
  void BatchDelete(Range In)
  {
    map_delete(std::forward<Range>(In));
  }

  auto Contain(K const k) const
  {
    pmos::MembershipLogger logger;
    V value;
    bool found = table_->get(key_to_str(k), value, *thread_info_.get());
    return std::make_pair(found, logger);
  }

  auto RangeCount(TT::Box const query_box)
    -> std::pair<size_t, pmos::RangeReportLogger>
  {
    pmos::RangeReportLogger logger;
    size_t count = 0;
    CountScannerHelper scanner(count, query_box.second);
    table_->scan(key_to_str(query_box.first), true, scanner, *thread_info_.get());
    return { count, logger };
  }

  template<typename Range>
  auto RangeReport(TT::Box const query_box, Range&& out)
    -> std::pair<size_t, pmos::RangeReportLogger>
  {
    pmos::RangeReportLogger logger;
    size_t count = 0;
    ReportScannerHelper<Range> scanner(count, query_box.second, out);
    table_->scan(key_to_str(query_box.first), true, scanner, *thread_info_.get());
    return { count, logger };
  }

  void DeleteTree()
  {
    if (table_) {
      table_->destroy(*thread_info_.get());
      table_ = std::make_unique<Table>();
      table_->initialize(*thread_info_.get());
    }
  }

  auto GetSize() const
  {
    // Masstree doesn't provide direct size query
    // Return 0 for now
    return 0;
  }

  constexpr static char const* GetTreeName()
  {
    return "Masstree";
  }

  constexpr static char const* CheckHasBox()
  {
    return "HasBox";
  }

private:
  // Scanner helper structs need to be defined outside the member functions
  struct CountScannerHelper {
    size_t& count;
    K end_key;
    CountScannerHelper(size_t& c, K ek) : count(c), end_key(ek) {}
    template <typename SS, typename KK>
    void visit_leaf(const SS&, const KK&, threadinfo&) {}
    bool visit_value(Str key_str, V, threadinfo&) {
      K key = *reinterpret_cast<const K*>(key_str.data());
      if (key >= end_key) return false;
      count++;
      return true;
    }
  };

  template<typename Range>
  struct ReportScannerHelper {
    size_t& count;
    K end_key;
    Range& output;
    ReportScannerHelper(size_t& c, K ek, Range& o) : count(c), end_key(ek), output(o) {}
    template <typename SS, typename KK>
    void visit_leaf(const SS&, const KK&, threadinfo&) {}
    bool visit_value(Str key_str, V value, threadinfo&) {
      K key = *reinterpret_cast<const K*>(key_str.data());
      if (key >= end_key) return false;
      output[count].key = key;
      output[count].val = value;
      count++;
      return true;
    }
  };
};

} // namespace MasstreeWrapper
