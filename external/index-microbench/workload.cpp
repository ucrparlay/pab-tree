#include "./papi_util.cpp"

// yum install papi papi-devel gperftools-libs gperftools-devel
#include "microbench.h"

#include "../../tests/test_framework.h"
#include "parlay/primitives.h"
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

thread_local long skiplist_steps = 0;
std::atomic<long> skiplist_total_steps;

// #define USE_TBB

#ifdef USE_TBB
#include "tbb/tbb.h"
#endif

// Enable this if you need pre-allocation utilization
// #define BWTREE_CONSOLIDATE_AFTER_INSERT

#ifdef BWTREE_CONSOLIDATE_AFTER_INSERT
#ifdef USE_TBB
#warning "Could not use TBB and BwTree consolidate together"
#endif
#endif

#ifdef BWTREE_COLLECT_STATISTICS
#ifdef USE_TBB
#warning "Could not use TBB and BwTree statistics together"
#endif
#endif

// Whether insert interleaves
// #define INTERLEAVED_INSERT

// Whether read operatoin miss will be counted
// #define COUNT_READ_MISS

typedef uint64_t keytype;
typedef std::less<uint64_t> keycomp;

static const uint64_t key_type = 0;
static const uint64_t value_type =
  1; // 0 = random pointers, 1 = pointers to keys

extern bool hyperthreading;

// Whether we only perform insert
static bool insert_only = false;

// We could set an upper bound of the number of loaded keys
static int64_t max_init_key = -1;

#include "util.h"

/*
 * MemUsage() - Reads memory usage from /proc file system
 */
size_t
MemUsage()
{
  FILE* fp = fopen("/proc/self/statm", "r");
  if (fp == nullptr) {
    fprintf(stderr, "Could not open /proc/self/statm to read memory usage\n");
    exit(1);
  }

  unsigned long unused;
  unsigned long rss;
  if (fscanf(fp,
             "%ld %ld %ld %ld %ld %ld %ld",
             &unused,
             &rss,
             &unused,
             &unused,
             &unused,
             &unused,
             &unused) != 7) {
    perror("");
    exit(1);
  }

  (void)unused;
  fclose(fp);

  return rss * (4096 / 1024); // in KiB (not kB)
}

//==============================================================
// LOAD
//==============================================================
inline void
load(int wl,
     int kt,
     int index_type,
     std::vector<keytype>& init_keys,
     std::vector<keytype>& keys,
     std::vector<uint64_t>& values,
     std::vector<int>& ranges,
     std::vector<int>& ops)
{
  std::string init_file;
  std::string txn_file;

  if (kt == RAND_KEY && wl == WORKLOAD_A) {
    init_file = "workloads/loada_zipf_int_100M.dat";
    txn_file = "workloads/txnsa_zipf_int_100M.dat";
  } else if (kt == RAND_KEY && wl == WORKLOAD_C) {
    init_file = "workloads/loadc_zipf_int_100M.dat";
    txn_file = "workloads/txnsc_zipf_int_100M.dat";
  } else if (kt == RAND_KEY && wl == WORKLOAD_E) {
    init_file = "workloads/loade_zipf_int_100M.dat";
    txn_file = "workloads/txnse_zipf_int_100M.dat";
  } else if (kt == MONO_KEY && wl == WORKLOAD_A) {
    init_file = "workloads/mono_inc_loada_zipf_int_100M.dat";
    txn_file = "workloads/mono_inc_txnsa_zipf_int_100M.dat";
  } else if (kt == MONO_KEY && wl == WORKLOAD_C) {
    init_file = "workloads/mono_inc_loadc_zipf_int_100M.dat";
    txn_file = "workloads/mono_inc_txnsc_zipf_int_100M.dat";
  } else if (kt == MONO_KEY && wl == WORKLOAD_E) {
    init_file = "workloads/mono_inc_loade_zipf_int_100M.dat";
    txn_file = "workloads/mono_inc_txnse_zipf_int_100M.dat";
  } else {
    fprintf(stderr, "Unknown workload type or key type: %d, %d\n", wl, kt);
    exit(1);
  }

  std::ifstream infile_load(init_file);

  std::string op;
  keytype key;
  int range;

  std::string insert("INSERT");
  std::string read("READ");
  std::string update("UPDATE");
  std::string scan("SCAN");

  int count = 0;
  while ((count < INIT_LIMIT) && infile_load.good()) {
    infile_load >> op >> key;
    if (op.compare(insert) != 0) {
      std::cout << "READING LOAD FILE FAIL!\n";
      return;
    }
    init_keys.push_back(key);
    count++;

    // If we have reached the max init key limit then just break
    if (max_init_key > 0 && count == max_init_key) {
      break;
    }
  }

  fprintf(stderr, "Loaded %d keys\n", count);

  count = 0;
  uint64_t value = 0;
  void* base_ptr = malloc(8);
  uint64_t base = (uint64_t)(base_ptr);
  free(base_ptr);

  keytype* init_keys_data = init_keys.data();

  if (value_type == 0) {
    while (count < INIT_LIMIT) {
      value = base + rand();
      values.push_back(value);
      count++;
    }
  } else {
    while (count < INIT_LIMIT) {
      values.push_back(reinterpret_cast<uint64_t>(init_keys_data + count));
      count++;
    }
  }

  // If we do not perform other transactions, we can skip txn file
  if (insert_only == true) {
    return;
  }

  // If we also execute transaction then open the
  // transacton file here
  std::ifstream infile_txn(txn_file);

  count = 0;
  while ((count < LIMIT) && infile_txn.good()) {
    infile_txn >> op >> key;
    if (op.compare(insert) == 0) {
      ops.push_back(OP_INSERT);
      keys.push_back(key);
      ranges.push_back(1);
    } else if (op.compare(read) == 0) {
      ops.push_back(OP_READ);
      keys.push_back(key);
    } else if (op.compare(update) == 0) {
      ops.push_back(OP_UPSERT);
      keys.push_back(key);
    } else if (op.compare(scan) == 0) {
      infile_txn >> range;
      ops.push_back(OP_SCAN);
      keys.push_back(key);
      ranges.push_back(range);
    } else {
      std::cout << "UNRECOGNIZED CMD!\n";
      return;
    }
    count++;
  }

  // Average and variation
  long avg = 0, var = 0;
  // If it is YSCB-E workload then we compute average and stdvar
  if (ranges.size() != 0) {
    for (int r : ranges) {
      avg += r;
    }

    avg /= (long)ranges.size();

    for (int r : ranges) {
      var += ((r - avg) * (r - avg));
    }

    var /= (long)ranges.size();

    fprintf(stderr, "YCSB-E scan Avg length: %ld; Variance: %ld\n", avg, var);
  }
}

//==============================================================
// EXEC
//==============================================================
inline void
exec(int wl,
     int index_type,
     int num_thread,
     std::vector<keytype>& init_keys,
     std::vector<keytype>& keys,
     std::vector<uint64_t>& values,
     std::vector<int>& ranges,
     std::vector<int>& ops)
{

  Index<keytype, keycomp>* idx =
    getInstance<keytype, keycomp>(index_type, key_type);

  // WRITE ONLY TEST-----------------
  int count = (int)init_keys.size();
  fprintf(stderr,
          "Populating the index with %d keys using %d threads\n",
          count,
          num_thread);
  double start_time = 0.0;
  double end_time = 0.0;

#ifdef USE_TBB
  tbb::task_scheduler_init init{ num_thread };

  std::atomic<int> next_thread_id;
  next_thread_id.store(0);

  auto func = [idx, &init_keys, &values, &next_thread_id](
                const tbb::blocked_range<size_t>& r) {
    size_t start_index = r.begin();
    size_t end_index = r.end();

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int thread_id = next_thread_id.fetch_add(1);
    idx->AssignGCID(thread_id);

    int gc_counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      idx->insert(init_keys[i], values[i], ti);
      gc_counter++;
      if (gc_counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    ti->rcu_quiesce();
    idx->UnregisterThread(thread_id);

    return;
  };

  idx->UpdateThreadLocal(num_thread);
  start_time = get_now();
  tbb::parallel_for(tbb::blocked_range<size_t>(0, count), func);
  end_time = get_now();
  idx->UpdateThreadLocal(1);
#else

  auto func = [idx, &init_keys, num_thread, &values, index_type](
                uint64_t thread_id, bool) {
    size_t total_num_key = init_keys.size();
    size_t key_per_thread = total_num_key / num_thread;
    size_t start_index = key_per_thread * thread_id;
    size_t end_index = start_index + key_per_thread;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int gc_counter = 0;
#ifdef INTERLEAVED_INSERT
    for (size_t i = thread_id; i < total_num_key; i += num_thread) {
#else
    for (size_t i = start_index; i < end_index; i++) {
#endif
      if (index_type == TYPE_SKIPLIST) {
        idx->insert(init_keys[start_index + end_index - 1 - i],
                    values[start_index + end_index - 1 - i],
                    ti);
      } else {
#ifdef BWTREE_USE_DELTA_UPDATE
        idx->insert(init_keys[i], values[i], ti);
#else
        idx->insert_bwtree_fast(init_keys[i], values[i]);
#endif
      }
      gc_counter++;
      if (gc_counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    ti->rcu_quiesce();

    return;
  };

  start_time = get_now();
  StartThreads(idx, num_thread, func, false);
  end_time = get_now();

  if (index_type == TYPE_SKIPLIST) {
    fprintf(stderr, "SkipList size = %lu\n", idx->GetIndexSize());
    fprintf(stderr,
            "Skiplist avg. steps = %f\n",
            (double)skiplist_total_steps / (double)init_keys.size());
  }

#endif

  double tput = count / (end_time - start_time) / 1000000; // Mops/sec

  std::cout << "\033[1;32m";
  std::cout << "insert " << tput << "\033[0m" << "\n";

  // Only execute consolidation if BwTree delta chain is used
#ifdef BWTREE_CONSOLIDATE_AFTER_INSERT
  fprintf(stderr, "Starting consolidating delta chain on each level\n");
  idx->AfterLoadCallback();
#endif

  // If the workload only executes load phase then we return here
  if (insert_only == true) {
    delete idx;
    return;
  }

  // READ/UPDATE/SCAN TEST----------------
  int txn_num = GetTxnCount(ops, index_type);
  uint64_t sum = 0;
  uint64_t s = 0;

  if (values.size() < keys.size()) {
    fprintf(stderr, "Values array too small\n");
    exit(1);
  }

  fprintf(stderr, "# of Txn: %d\n", txn_num);

  // This is used to count how many read misses we have found
  std::atomic<size_t> read_miss_counter{}, read_hit_counter{};
  read_miss_counter.store(0UL);
  read_hit_counter.store(0UL);

  auto func2 = [num_thread,
                idx,
                &read_miss_counter,
                &read_hit_counter,
                &keys,
                &values,
                &ranges,
                &ops](uint64_t thread_id, bool) {
    size_t total_num_op = ops.size();
    size_t op_per_thread = total_num_op / num_thread;
    size_t start_index = op_per_thread * thread_id;
    size_t end_index = start_index + op_per_thread;

    std::vector<uint64_t> v;
    v.reserve(10);

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      int op = ops[i];
      if (op == OP_INSERT) { // INSERT
        idx->insert(keys[i], values[i], ti);
      } else if (op == OP_READ) { // READ
        v.clear();

#ifdef BWTREE_USE_MAPPING_TABLE
        idx->find(keys[i], &v, ti);
#else
        idx->find_bwtree_fast(keys[i], &v);
#endif

        // If we count read misses then increment the
        // counter here if the vetor is empty
#ifdef COUNT_READ_MISS
        if (v.size() == 0UL) {
          read_miss_counter.fetch_add(1);
        } else {
          read_hit_counter.fetch_add(1);
        }
#endif
      } else if (op == OP_UPSERT) { // UPDATE
        idx->upsert(keys[i], reinterpret_cast<uint64_t>(&keys[i]), ti);
      } else if (op == OP_SCAN) { // SCAN
        idx->scan(keys[i], ranges[i], ti);
      }

      counter++;
      if (counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    // Perform GC after all operations
    ti->rcu_quiesce();

    return;
  };

  start_time = get_now();
  StartThreads(idx, num_thread, func2, false);
  end_time = get_now();

  // Print out how many reads have missed in the index (do not have a value)
#ifdef COUNT_READ_MISS
  fprintf(stderr,
          "  Read misses: %lu; Read hits: %lu\n",
          read_miss_counter.load(),
          read_hit_counter.load());
#endif

  tput = txn_num / (end_time - start_time) / 1000000; // Mops/sec

  std::cout << "sum = " << sum << "\n";
  std::cout << "\033[1;31m";

  if (wl == WORKLOAD_A) {
    std::cout << "read/update " << (tput + (sum - sum));
  } else if (wl == WORKLOAD_C) {
    std::cout << "read " << (tput + (sum - sum));
  } else if (wl == WORKLOAD_E) {
    std::cout << "insert/scan " << (tput + (sum - sum));
  } else {
    fprintf(stderr, "Unknown workload type: %d\n", wl);
    exit(1);
  }

  std::cout << "\033[0m" << "\n";

  if (index_type == TYPE_SKIPLIST) {
    fprintf(stderr, "SkipList size = %lu\n", idx->GetIndexSize());
    fprintf(stderr,
            "Skiplist avg. steps = %f\n",
            (double)skiplist_total_steps / (double)init_keys.size());
  }

  delete idx;

  return;
}

/*
 * run_rdtsc_benchmark() - This function runs the RDTSC benchmark which is a
 * high contention insert-only benchmark
 *
 * Note that key num is the total key num
 */
void
run_rdtsc_benchmark(int index_type, int thread_num, int key_num)
{
  Index<keytype, keycomp>* idx =
    getInstance<keytype, keycomp>(index_type, key_type);

  auto func = [idx, thread_num, key_num](uint64_t thread_id, bool) {
    size_t key_per_thread = key_num / thread_num;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    uint64_t* values = new uint64_t[key_per_thread];

    int gc_counter = 0;
    for (size_t i = 0; i < key_per_thread; i++) {
      // Note that RDTSC may return duplicated keys from different cores
      // to counter this we combine RDTSC with thread IDs to make it unique
      // The counter value on a single core is always unique, though
      uint64_t key = (Rdtsc() << 6) | thread_id;
      values[i] = key;
      // fprintf(stderr, "%lx\n", key);
      idx->insert(key, reinterpret_cast<uint64_t>(values + i), ti);
      gc_counter++;
      if (gc_counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    ti->rcu_quiesce();

    delete[] values;

    return;
  };

  double start_time = get_now();
  StartThreads(idx, thread_num, func, false);
  double end_time = get_now();

  // Only execute consolidation if BwTree delta chain is used
#ifdef BWTREE_CONSOLIDATE_AFTER_INSERT
  idx->AfterLoadCallback();
#endif

  double tput = key_num * 1.0 / (end_time - start_time) / 1000000; // Mops/sec
  std::cout << "insert " << tput << "\n";

  return;
}

template<typename TypeTrait>
struct MassTreeWrapper
{
  // using TreeType = typename CPAMTree::CpamRaw<TypeTrait>;
};
template<typename TypeTrait>
struct BwTreeWrapper
{
  // using TreeType = typename PAMTree::PamRaw<TypeTrait>;
};

template<int kPrint>
void
ConDelete(auto num_thread, auto idx, const auto& wp)
{
  auto func = [idx, &wp, num_thread](uint64_t thread_id, bool) {
    size_t total_num_key = wp.size();
    size_t key_per_thread = total_num_key / num_thread;
    size_t start_index = key_per_thread * thread_id;
    size_t end_index = start_index + key_per_thread;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int gc_counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      idx->remove(wp[i].GetKey(), wp[i].GetVal(), ti);
      gc_counter++;
      if (gc_counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    ti->rcu_quiesce();

    return;
  };

  parlay::internal::timer t;
  t.reset(), t.start();
  StartThreads(idx, num_thread, func, false);
  t.stop();
  if constexpr (kPrint) {
    std::cout << t.total_time() << " " << std::flush;
  }
}

template<int kPrint>
void
ConInsert(auto num_thread, auto idx, const auto& wp)
{
  auto func = [idx, &wp, num_thread](uint64_t thread_id, bool) {
    size_t total_num_key = wp.size();
    size_t key_per_thread = total_num_key / num_thread;
    size_t start_index = key_per_thread * thread_id;
    size_t end_index = start_index + key_per_thread;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int gc_counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      idx->insert(wp[i].GetKey(), wp[i].GetVal(), ti);
      gc_counter++;
      if (gc_counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    ti->rcu_quiesce();

    return;
  };

  parlay::internal::timer t;
  t.reset(), t.start();
  StartThreads(idx, num_thread, func, false);
  t.stop();
  if constexpr (kPrint) {
    std::cout << t.total_time() << " " << std::flush;
  }
}

template<int kPrint>
auto
ConBuild(auto num_thread, int index_type, const auto& wp)
{
  // instantiate the index
  Index<keytype, keycomp>* idx =
    getInstance<keytype, keycomp>(index_type, key_type);

  ConInsert<kPrint>(num_thread, idx, wp);

  return idx;
}

template<int kPrint = 1>
double
ConMembership(auto num_thread, auto idx, const auto& query_ele)
{
  auto func2 = [num_thread, idx, &query_ele](uint64_t thread_id, bool) {
    size_t total_num_op = query_ele.size();
    size_t op_per_thread = total_num_op / num_thread;
    size_t start_index = op_per_thread * thread_id;
    size_t end_index = start_index + op_per_thread;

    std::vector<uint64_t> v;
    v.reserve(10);

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      idx->find(query_ele[i], &v, ti);

      counter++;
      if (counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    // Perform GC after all operations
    ti->rcu_quiesce();

    return;
  };

  parlay::internal::timer t;
  t.reset(), t.start();
  StartThreads(idx, num_thread, func2, false);
  t.stop();
  if constexpr (kPrint) {
    std::cout << t.total_time() << " " << std::flush;
  }
  return t.total_time();
}

template<int kPrint = 1>
double
ConRangeCount(auto num_thread, auto idx, auto const& query_box_seq)
{
  auto func2 = [num_thread, idx, &query_box_seq](uint64_t thread_id, bool) {
    size_t total_num_op = query_box_seq.size();
    size_t op_per_thread = total_num_op / num_thread;
    size_t start_index = op_per_thread * thread_id;
    size_t end_index = start_index + op_per_thread;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      idx->range_count(query_box_seq[i].first, ti);

      counter++;
      if (counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    // Perform GC after all operations
    ti->rcu_quiesce();

    return;
  };

  parlay::internal::timer t;
  t.reset(), t.start();
  StartThreads(idx, num_thread, func2, false);
  t.stop();
  if constexpr (kPrint) {
    std::cout << t.total_time() << " " << std::flush;
  }
  return t.total_time();
}

template<int kPrint = 1>
double
ConRangeQuery(auto num_thread,
              auto idx,
              int index_type,
              int rec_num,
              auto const& query_box_seq)
{
  auto [offset, tot_size] = parlay::scan(
    parlay::tabulate(
      rec_num, [&](size_t i) -> size_t { return query_box_seq[i].second; }),
    parlay::addm<size_t>());
  offset.push_back(tot_size);
  parlay::sequence<std::pair<keytype, uint64_t>> v(tot_size);

  auto func2 = [num_thread, idx, index_type, &v, &offset, &query_box_seq](
                 uint64_t thread_id, bool) {
    size_t total_num_op = query_box_seq.size();
    size_t op_per_thread = total_num_op / num_thread;
    size_t start_index = op_per_thread * thread_id;
    size_t end_index = start_index + op_per_thread;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);

    int counter = 0;
    for (size_t i = start_index; i < end_index; i++) {
      // if (index_type == TYPE_MASSTREE) {
      auto box = query_box_seq[i].first;
      idx->scan(box.first, query_box_seq[i].second, ti);
      // } else if (index_type == TYPE_BWTREE) {
      //   idx->range_report(
      //     query_box_seq[i].first, v.begin() + offset[start_index], ti);
      // }

      counter++;
      if (counter % 4096 == 0) {
        ti->rcu_quiesce();
      }
    }

    // Perform GC after all operations
    ti->rcu_quiesce();

    return;
  };

  parlay::internal::timer t;
  t.reset(), t.start();
  StartThreads(idx, num_thread, func2, false);
  t.stop();
  if constexpr (kPrint) {
    std::cout << t.total_time() << " " << std::flush;
  }
  return t.total_time();
}

template<typename TT>
void
ConcurrentIdxOSM(auto num_thread,
                 auto primal_idx,
                 int index_type,
                 const parlay::sequence<typename TT::EntrySeq>& node_by_time,
                 const int sliding_window_len,
                 const size_t member_query_n)
{
  const int time_period_num = node_by_time.size();
  auto const& insert_entry = node_by_time;
  auto const& delete_entry = node_by_time;
  auto query_points = TT::KeySeq::uninitialized(member_query_n);
  auto step = member_query_n / time_period_num;
  for (int i = 0; i < time_period_num; i++) {
    parlay::parallel_for(0, step, [&](size_t j) {
      query_points[(i * step) + j] = TT::Entry::GetKey(node_by_time[i][j]);
    });
  }

  parlay::sequence<double> insert_time(time_period_num, 0.0);
  parlay::sequence<double> delete_time(time_period_num, 0.0);
  parlay::sequence<double> query_time(time_period_num, 0.0);
  parlay::sequence<size_t> tree_size(time_period_num, 0);

  size_t current_tree_size = 0;
  // running the osm
  parlay::internal::timer t;
  for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
    t.reset(), t.start();
    ConInsert<0>(num_thread, primal_idx, insert_entry[batch_id]);
    t.stop();
    insert_time[batch_id] += t.total_time();
    current_tree_size += node_by_time[batch_id].size();

    if (batch_id >= sliding_window_len) {
      t.reset(), t.start();
      ConDelete<0>(
        num_thread, primal_idx, delete_entry[batch_id - sliding_window_len]);
      t.stop();
      delete_time[batch_id] += t.total_time();
      current_tree_size -= node_by_time[batch_id - sliding_window_len].size();
    } else {
      delete_time[batch_id] = 0.0;
    }
    tree_size[batch_id] = current_tree_size;

    // NOTE: test membership query
    t.reset(), t.start();
    double time = ConMembership(num_thread, primal_idx, query_points);
    t.stop();
    query_time[batch_id] += time;
  }

  std::cout << std::right << std::setw(12) << "insert_n" << std::setw(12)
            << "delete_n" << std::setw(12) << "tree_n" << std::setw(14)
            << "insert_t(s)" << std::setw(14) << "delete_t(s)" << std::setw(14)
            << "query_t(s)" << std::endl;
  for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
    size_t const delete_n =
      batch_id >= sliding_window_len
        ? node_by_time[batch_id - sliding_window_len].size()
        : 0;
    std::cout << std::right << std::setw(12) << node_by_time[batch_id].size()
              << std::setw(12) << delete_n << std::setw(12)
              << tree_size[batch_id] << std::setw(14) << std::fixed
              << std::setprecision(6) << insert_time[batch_id] << std::setw(14)
              << delete_time[batch_id] << std::setw(14) << query_time[batch_id]
              << std::endl;
  }
}

template<typename TT>
void
ConcurrentIdxDynamicGraph(auto num_thread,
                          auto primal_idx,
                          int index_type,
                          const auto& insert_batch_seq,
                          const auto& delete_batch_seq,
                          const auto& box_seq,
                          const size_t member_query_n)
{
  auto time_period_num = insert_batch_seq.size();
  auto const& insert_entry = insert_batch_seq;
  auto const& delete_entry = delete_batch_seq;
  auto query_points = TT::KeySeq::uninitialized(member_query_n);
  auto step = member_query_n / time_period_num;
  if (member_query_n % time_period_num != 0) {
    std::cout << "the member query num should be divisible by time period num"
              << std::endl;
    return;
  }

  for (int i = 0; i < time_period_num; i++) {
    parlay::parallel_for(0, step, [&](size_t j) {
      query_points[(i * step) + j] = TT::Entry::GetKey(insert_batch_seq[i][j]);
    });
  }

  parlay::sequence<double> insert_time(time_period_num, 0.0);
  parlay::sequence<double> delete_time(time_period_num, 0.0);
  parlay::sequence<double> member_query_time(time_period_num, 0.0);
  parlay::sequence<double> range_query_time(time_period_num, 0.0);
  parlay::sequence<size_t> tree_size(time_period_num, 0);

  size_t current_tree_size = 0;
  // running the osm
  parlay::internal::timer t;
  double time = 0;
  for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
    t.reset(), t.start();
    ConInsert<0>(num_thread, primal_idx, insert_entry[batch_id]);
    t.stop();
    insert_time[batch_id] += t.total_time();
    current_tree_size += insert_entry[batch_id].size();

    t.reset(), t.start();
    ConDelete<0>(num_thread, primal_idx, delete_entry[batch_id]);
    t.stop();
    delete_time[batch_id] += t.total_time();
    current_tree_size -= delete_entry[batch_id].size();
    tree_size[batch_id] = current_tree_size;

    // NOTE: test membership query
    t.reset(), t.start();
    time = ConMembership<0>(num_thread, primal_idx, query_points);
    t.stop();
    member_query_time[batch_id] += time;

    t.reset(), t.start();
    time = ConRangeQuery<0>(
      num_thread, primal_idx, index_type, box_seq.size(), box_seq);
    t.stop();
    range_query_time[batch_id] += time;
  }

  std::cout << std::right << std::setw(12) << "insert_n" << std::setw(12)
            << "delete_n" << std::setw(12) << "tree_n" << std::setw(14)
            << "insert_t(s)" << std::setw(14) << "delete_t(s)" << std::setw(14)
            << "find_t(s)" << std::setw(14) << "range_t(s)" << std::endl;
  for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
    // size_t const delete_n =
    //   batch_id >= sliding_window_len
    //     ? node_by_time[batch_id - sliding_window_len].size()
    //     : 0;
    std::cout << std::right << std::setw(12)
              << insert_batch_seq[batch_id].size() << std::setw(12)
              << delete_batch_seq[batch_id].size() << std::setw(12)
              << tree_size[batch_id] << std::setw(14) << std::fixed
              << std::setprecision(6) << insert_time[batch_id] << std::setw(14)
              << delete_time[batch_id] << std::setw(14)
              << member_query_time[batch_id] << std::setw(14)
              << range_query_time[batch_id] << std::endl;
  }
}

template<typename Options>
void
ApplyConcurrentIndexDynamicGraph(Options const& opts)
{
  // opts.PrintOptionValue();

  using Wrapper = Wrapper<uint64_t, uint32_t>;
  using E = typename Wrapper::E;
  // using EleType = Element<uint32_t, uint32_t>;
  using TT = pb_tree::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                     16,
                                     Wrapper::LeafAugEmpty,
                                     Wrapper::InteriorAugEmpty,
                                     8,
                                     pb_tree::ProtoNode>;

#if defined(DYNAMIC_GRAPH)
  constexpr int kInputType = InputType::kDynamicGraphInput;
#elif defined(OSM)
  constexpr int kInputType = InputType::kOsmInput;
#else
  constexpr int kInputType = InputType::kNormalInput;
#endif

  using EntrySeq = typename TT::EntrySeq;

  int index_type = 0;
  if (opts.tree_type == 5) {
    index_type = TYPE_MASSTREE;
    PrintTreeParam("MassTree", 1);
  } else if (opts.tree_type == 6) {
    index_type = TYPE_BWTREE;
    PrintTreeParam("BwTree", 1);
  } else {
    std::cout << "Unsupported tree type: " << opts.tree_type << std::endl;
    return;
  }

  opts.PrintOptions();
  std::cout << "\n" << std::flush;

  int num_thread = parlay::num_workers();
  Index<keytype, keycomp>* idx =
    getInstance<keytype, keycomp>(index_type, key_type);

  // NOTE: real world sliding window
  if (opts.tag & (1 << 3)) {
    string osm_path = opts.input_file_path;
    EntrySeq osm_records;
    auto [input_num, input_dim] =
      ReadEntries<typename TT::E, kInputType>(osm_path.c_str(), osm_records);
    parlay::sort_inplace(osm_records, [&](auto const& a, auto const& b) {
      return TT::Entry::GetVal(a) < TT::Entry::GetVal(b);
    });
    const std::vector<int> files = { 2015, 2016, 2017, 2018, 2019,
                                     2020, 2021, 2022, 2023, 2024 };
    auto node_by_year = parlay::tabulate(files.size(), [&](size_t i) {
      return parlay::filter(osm_records, [&](auto const& record) {
        return TT::Entry::GetVal(record) == files[i];
      });
    });

    ConcurrentIdxOSM<TT>(num_thread,
                         idx,
                         index_type,
                         node_by_year,
                         kOsmWindowLen,
                         kMemberQueryOsmSize);
  }

  if (opts.tag & (1 << 4)) { // NOTE: temporal graph
    // string dynamic_graph_path =
    //   "/data/zmen002/geometry/osm_temporal_graph/north_america/";
    string dynamic_graph_path = opts.input_file_path;

    constexpr int kDynamicGraphBatchNum = 10;
    std::array<std::string, kDynamicGraphBatchNum> insert_files;
    std::array<std::string, kDynamicGraphBatchNum> delete_files;
    for (int i = 0; i < kDynamicGraphBatchNum; i++) {
      insert_files[i] = dynamic_graph_path + std::to_string(i) + "_insert.in";
      delete_files[i] = dynamic_graph_path + std::to_string(i) + "_delete.in";
    }
    parlay::sequence<EntrySeq> insert_batch_seq(kDynamicGraphBatchNum);
    parlay::sequence<EntrySeq> delete_batch_seq(kDynamicGraphBatchNum);
    size_t total_entries = 0;
    for (int i = 0; i < kDynamicGraphBatchNum; i++) {
      auto [n1, dim1] = ReadEntries<typename TT::E, kInputType>(
        insert_files[i].c_str(), insert_batch_seq[i], total_entries);
      total_entries += n1;

      auto [n2, dim2] = ReadEntries<typename TT::E, kInputType>(
        delete_files[i].c_str(), delete_batch_seq[i], total_entries);
      total_entries += n2;

      // std::cout << n1 << " " << n2 << std::endl;
    }

    // generate box
    auto random_wi = parlay::random_shuffle(insert_batch_seq[0]);
    auto box_seq = parlay::tabulate(kDynamicGraphRangeNum, [&](size_t i) {
      auto e = random_wi[i];
      uint64_t key = TT::Entry::GetKey(e);
      uint64_t u = key >> 32;
      auto box = std::make_pair(u << 32, (u + 1) << 32);
      return std::make_pair(box, 20);
    });
    ConcurrentIdxDynamicGraph<TT>(num_thread,
                                  idx,
                                  index_type,
                                  insert_batch_seq,
                                  delete_batch_seq,
                                  box_seq,
                                  kMemberQueryOsmSize);
  }

  std::cout << "\n" << std::flush;
}

template<typename Options>
void
ApplyConcurrentIndex(Options const& opts)
{
  // opts.PrintOptionValue();

  using Wrapper = Wrapper<uint32_t, uint32_t>;
  using E = typename Wrapper::E;
  // using EleType = Element<uint32_t, uint32_t>;
  using TT = pb_tree::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                     16,
                                     Wrapper::LeafAugEmpty,
                                     Wrapper::InteriorAugEmpty,
                                     8,
                                     pb_tree::ProtoNode>;

  using EntrySeq = TT::EntrySeq;

  int index_type = 0;
  if (opts.tree_type == 5) {
    index_type = TYPE_MASSTREE;
    PrintTreeParam("MassTree", 1);
  } else if (opts.tree_type == 6) {
    index_type = TYPE_BWTREE;
    PrintTreeParam("BwTree", 1);
  } else {
    std::cout << "Unsupported tree type: " << opts.tree_type << std::endl;
    return;
  }

  opts.PrintOptions();
  std::cout << "\n" << std::flush;

  int num_thread = parlay::num_workers();

  EntrySeq wp;
  EntrySeq wi;
  ReadEntries<typename TT::E>(opts.input_file_path.c_str(), wp);

  // NOTE: build
  auto* primal_idx = ConBuild<1>(num_thread, index_type, wp);
  // std::cout << "build" << std::endl;

  // NOTE: insert
  if (opts.tag & (1 << 0)) {
    if (opts.summary) {
      parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
      for (auto ratio : ratio_seq) {
        auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        if (index_type == TYPE_MASSTREE) { // WARN: masstree MLE for 1e9 input
                                           // if rebuild the whole index
          ConInsert<1>(num_thread, primal_idx, wi);
          ConDelete<0>(num_thread, primal_idx, wi);
        } else if (index_type == TYPE_BWTREE) {
          auto* idx = ConBuild<0>(num_thread, index_type, wp);
          ConInsert<1>(num_thread, idx, wi);
          delete idx;
        }
      }
    } else {
      auto [base_path, batch_path] =
        opts.ParseInsertBatchPath(opts.batch_ratio);
      // std::cout << base_path << " " << batch_path << std::endl;
      auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
      auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
      if (index_type == TYPE_MASSTREE) {
        ConInsert<1>(num_thread, primal_idx, wi);
        ConDelete<0>(num_thread, primal_idx, wi);
      } else if (index_type == TYPE_BWTREE) {
        auto* idx = ConBuild<0>(num_thread, index_type, wp);
        ConInsert<1>(num_thread, idx, wi);
        delete idx;
      }
    }
  }

  // NOTE: batch delete
  if (opts.tag & (1 << 1)) {
    if (opts.summary) {
      parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
      // parlay::sequence<double> const ratio_seq = { 0.01, 0.1 };
      for (auto ratio : ratio_seq) {
        // puts("------------------------------");
        auto [base_path, batch_path] = opts.ParseDeleteBatchPath(ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        if (index_type == TYPE_MASSTREE) {
          ConDelete<1>(num_thread, primal_idx, wi);
          ConInsert<0>(num_thread, primal_idx, wi);
        } else if (index_type == TYPE_BWTREE) {
          auto* idx = ConBuild<0>(num_thread, index_type, wp);
          ConDelete<1>(num_thread, idx, wi);
          delete idx;
        }
      }
    } else {
      auto [base_path, batch_path] =
        opts.ParseDeleteBatchPath(opts.batch_ratio);
      // std::cout << base_path << " " << batch_path << std::endl;
      auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
      auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
      if (index_type == TYPE_MASSTREE) {
        ConDelete<1>(num_thread, primal_idx, wi);
        ConInsert<0>(num_thread, primal_idx, wi);
      } else if (index_type == TYPE_BWTREE) {
        auto* idx = ConBuild<0>(num_thread, index_type, wp);
        ConDelete<1>(num_thread, idx, wi);
        delete idx;
      }
    }
  }

  // NOTE: Membership query
  if (opts.query_type & (1 << 0)) {
    auto batch_size = static_cast<size_t>(wp.size() * kMemberQueryRatio);
    // auto query_ele =
    //   wi.size() ? wi.subseq(0, batch_size) : wp.subseq(0, batch_size);
    auto query_ele = parlay::tabulate(batch_size, [&](size_t i) {
      return wi.size() ? wi[i].GetKey() : wp[i].GetKey();
    });
    ConMembership(num_thread, primal_idx, query_ele);
  }

  // PARA: lambda for range query
  auto range_query = [&](auto const& base_points) {
    for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
      if (index_type == TYPE_MASSTREE && rec_type == kRangeQueryType - 1) {
        // Skip the largest rectangle type for masstree since it segfaults
        std::cout << "- " << " ";
        continue;
      }
      auto [query_box_seq, max_size] =
        GenRectangles<TT, false, false>(kRangeQueryNum, rec_type, base_points);
      ConRangeQuery(
        num_thread, primal_idx, index_type, kRangeQueryNum, query_box_seq);
    }
  };

  // PARA: lambda for range count
  auto range_count = [&](auto const& base_points) {
    for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
      if (index_type == TYPE_MASSTREE) {
        // Skip the largest rectangle type for masstree since it segfaults
        std::cout << "- " << " ";
        continue;
      }
      auto [query_box_seq, max_size] =
        GenRectangles<TT, false, false>(kRangeQueryNum, rec_type, base_points);
      ConRangeCount(num_thread, primal_idx, query_box_seq);
    }
  };

  // NOTE: range query
  if (opts.query_type & (1 << 1)) {
    range_query(wp);
  }

  // NOTE: range count
  if (opts.query_type & (1 << 2)) {
    range_count(wp);
  }

  // NOTE: batch update by size
  if (opts.tag & (1 << 3)) {
    if (index_type == TYPE_MASSTREE) { // reset the status
      ConDelete<0>(num_thread, primal_idx, wp);
      delete primal_idx;
    } else {
      delete primal_idx;
    }

    parlay::sequence<double> const ratio_seq = { 0.0001, 0.0002, 0.0005, 0.001,
                                                 0.002,  0.005,  0.01,   0.02,
                                                 0.05,   0.1,    0.2,    0.5,
                                                 1.0 };
    for (auto ratio : ratio_seq) {
      auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
      // std::cout << base_path << " " << batch_path << std::endl;
      auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
      auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
      // BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      if (index_type == TYPE_MASSTREE) { // WARN: masstree MLE for 1e9 input
                                         // if rebuild the whole index
        auto* idx = ConBuild<0>(num_thread, index_type, wp);
        ConInsert<1>(num_thread, idx, wi);
        ConDelete<0>(num_thread, idx, parlay::append(wi, wp));
        delete idx;
      } else if (index_type == TYPE_BWTREE) {
        auto* idx = ConBuild<0>(num_thread, index_type, wp);
        ConInsert<1>(num_thread, idx, wi);
        delete idx;
      }
    }

    for (auto ratio : ratio_seq) {
      auto [base_path, batch_path] = opts.ParseDeleteBatchPath(ratio);
      // std::cout << base_path << " " << batch_path << std::endl;
      auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
      auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
      if (index_type == TYPE_MASSTREE) {
        auto* idx = ConBuild<0>(num_thread, index_type, wp);
        ConDelete<1>(num_thread, idx, wi);
        ConDelete<0>(num_thread, idx, wp);
        delete idx;
      } else if (index_type == TYPE_BWTREE) {
        auto* idx = ConBuild<0>(num_thread, index_type, wp);
        ConDelete<1>(num_thread, idx, wi);
        delete idx;
      }
    }
  }
  std::cout << "\n" << std::flush;
}

int
main(int argc, char* argv[])
{
  // Suppress tcmalloc large-allocation warnings for benchmark runs.
  setenv("TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD", "1099511627776", 0);

  commandLine params(argc, argv);
  Options opts(params);

#if defined(DYNAMIC_GRAPH) || defined(OSM)
  ApplyConcurrentIndexDynamicGraph(opts);
#else
  ApplyConcurrentIndex(opts);
#endif // DYNAMIC_GRAPH
  // ApplyConcurrentIndex(opts);

  return 0;
  // if (argc < 5) {
  //   std::cout << "Usage:\n";
  //   std::cout << "1. workload type: a, c, e, none\n";
  //   std::cout << "   \"none\" type means we just load the file and exit. \n"
  //                "This serves as the base line for microbenchamrks\n";
  //   std::cout << "2. key distribution: rand, mono\n";
  //   std::cout
  //       << "3. index type: bwtree skiplist masstree artolc btreeolc
  //       btreertm\n";
  //   std::cout << "4. number of threads (integer)\n";
  //   std::cout << "   --hyper: Whether to pin all threads on NUMA node 0\n";
  //   std::cout << "   --mem: Whether to monitor memory access\n";
  //   std::cout << "   --numa: Whether to monitor NUMA throughput\n";
  //   std::cout
  //       << "   --insert-only: Whether to only execute insert operations\n";
  //
  //   return 1;
  // }

  // Then read the workload type
  //   int wl;
  //   if (strcmp(argv[1], "a") == 0) {
  //     wl = WORKLOAD_A;
  //   } else if (strcmp(argv[1], "c") == 0) {
  //     wl = WORKLOAD_C;
  //   } else if (strcmp(argv[1], "e") == 0) {
  //     wl = WORKLOAD_E;
  //   } else {
  //     fprintf(stderr, "Unknown workload: %s\n", argv[1]);
  //     exit(1);
  //   }
  //
  //   // Then read key type
  //   int kt;
  //   if (strcmp(argv[2], "rand") == 0) {
  //     kt = RAND_KEY;
  //   } else if (strcmp(argv[2], "mono") == 0) {
  //     kt = MONO_KEY;
  //   } else if (strcmp(argv[2], "rdtsc") == 0) {
  //     kt = RDTSC_KEY;
  //   } else {
  //     fprintf(stderr, "Unknown key type: %s\n", argv[2]);
  //     exit(1);
  //   }
  //
  // int index_type;
  // if (strcmp(argv[3], "bwtree") == 0) {
  //   index_type = TYPE_BWTREE;
  // } else if (strcmp(argv[3], "masstree") == 0) {
  //   index_type = TYPE_MASSTREE;
  // } else if (strcmp(argv[3], "artolc") == 0) {
  //   index_type = TYPE_ARTOLC;
  // } else if (strcmp(argv[3], "btreeolc") == 0) {
  //   index_type = TYPE_BTREEOLC;
  // } else if (strcmp(argv[3], "skiplist") == 0) {
  //   index_type = TYPE_SKIPLIST;
  // } else if (strcmp(argv[3], "btreertm") == 0) {
  //   index_type = TYPE_BTREERTM;
  // } else if (strcmp(argv[3], "none") == 0)
  //   // This is a special type used for measuring base cost (i.e.
  //   // only loading the workload files but do not invoke the index)
  //   index_type = TYPE_NONE;
  // else {
  //   fprintf(stderr, "Unknown index type: %d\n", index_type);
  //   exit(1);
  // }
  //
  //   // Then read number of threads using command line
  //   int num_thread = atoi(argv[4]);
  //   if(num_thread < 1 || num_thread > 40) {
  //     fprintf(stderr, "Do not support %d threads\n", num_thread);
  //     exit(1);
  //   } else {
  //     fprintf(stderr, "Number of threads: %d\n", num_thread);
  //   }
  //
  //   // Then read all remianing arguments
  //   int repeat_counter = 1;
  //   char **argv_end = argv + argc;
  //   for(char **v = argv + 5;v != argv_end;v++) {
  //     if(strcmp(*v, "--hyper") == 0) {
  //       // Enable hyoerthreading for scheduling threads
  //       hyperthreading = true;
  //     } else if(strcmp(*v, "--mem") == 0) {
  //       // Enable memory bandwidth measurement
  //       memory_bandwidth = true;
  //     } else if(strcmp(*v, "--numa") == 0) {
  //       numa = true;
  //     } else if(strcmp(*v, "--insert-only") == 0) {
  //       insert_only = true;
  //     } else if(strcmp(*v, "--repeat") == 0) {
  //       // If we repeat, then exec() will be called for 5 times
  //       repeat_counter = 5;
  //     } else if(strcmp(*v, "--max-init-key") == 0) {
  //       max_init_key = atoll(*(v + 1));
  //       if(max_init_key <= 0) {
  //         fprintf(stderr, "Illegal maximum init keys: %ld\n", max_init_key);
  //         exit(1);
  //       }
  //
  //       // Ignore the next argument
  //       v++;
  //     } else {
  //       fprintf(stderr, "Unknown switch: %s\n", *v);
  //       exit(1);
  //     }
  //   }
  //
  //   if(max_init_key != -1) {
  //     fprintf(stderr, "Maximum init keys: %ld\n", max_init_key);
  //     fprintf(stderr, "  NOTE: Memory is not affected in this case\n");
  //   }
  //
  // #ifdef COUNT_READ_MISS
  //   fprintf(stderr, "  Counting read misses\n");
  // #endif
  //
  // #ifdef BWTREE_CONSOLIDATE_AFTER_INSERT
  //   fprintf(stderr, "  BwTree will considate after insert phase\n");
  // #endif
  //
  // #ifdef USE_TBB
  //   fprintf(stderr, "  Using Intel TBB to run concurrent tasks\n");
  // #endif
  //
  // #ifdef INTERLEAVED_INSERT
  //   fprintf(stderr, "  Interleaved insert\n");
  // #endif
  //
  // #ifdef BWTREE_COLLECT_STATISTICS
  //   fprintf(stderr, "  BwTree will collect statistics\n");
  // #endif
  //
  //   fprintf(stderr, "Leaf delta chain threshold: %d; Inner delta chain
  //   threshold: %d\n",
  //           LEAF_DELTA_CHAIN_LENGTH_THRESHOLD,
  //           INNER_DELTA_CHAIN_LENGTH_THRESHOLD);
  //
  // #ifndef BWTREE_USE_MAPPING_TABLE
  //   fprintf(stderr, "  BwTree does not use mapping table\n");
  //   if(wl != WORKLOAD_C) {
  //     fprintf(stderr, "Could only use workload C\n");
  //     exit(1);
  //   }
  //
  //   if(index_type != TYPE_BWTREE) {
  //     fprintf(stderr, "Could only use BwTree\n");
  //     exit(1);
  //   }
  // #endif
  //
  // #ifndef BWTREE_USE_CAS
  //   fprintf(stderr, "  BwTree does not use CAS\n");
  // #endif
  //
  // #ifndef BWTREE_USE_DELTA_UPDATE
  //   fprintf(stderr, "  BwTree does not use delta update\n");
  //   if(index_type != TYPE_BWTREE) {
  //     fprintf(stderr, "Could only use BwTree\n");
  //   }
  // #endif
  //
  // #ifdef USE_OLD_EPOCH
  //   fprintf(stderr, "  BwTree uses old epoch\n");
  // #endif
  //
  //   // If we do not interleave threads on two sockets then this will be
  //   printed if(hyperthreading == true) {
  //     fprintf(stderr, "  Hyperthreading for thread 10 - 19, 30 - 39\n");
  //   }
  //
  //   if(repeat_counter != 1) {
  //     fprintf(stderr, "  Repeat for %d times (NOTE: Memory number may not be
  //     correct)\n",
  //             repeat_counter);
  //   }
  //
  //   if(memory_bandwidth == true) {
  //     if(geteuid() != 0) {
  //       fprintf(stderr, "Please run the program as root in order to measure
  //       memory bandwidth\n"); exit(1);
  //     }
  //
  //     fprintf(stderr, "  Measuring memory bandwidth\n");
  //
  //     PCM_memory::InitMemoryMonitor();
  //   }
  //
  //   if(numa == true) {
  //     if(geteuid() != 0) {
  //       fprintf(stderr, "Please run the program as root in order to measure
  //       NUMA operations\n"); exit(1);
  //     }
  //
  //     fprintf(stderr, "  Measuring NUMA operations\n");
  //
  //     // Call init here to avoid calling it mutiple times
  //     PCM_NUMA::InitNUMAMonitor();
  //   }
  //
  //   if(insert_only == true) {
  //     fprintf(stderr, "Program will exit after insert operation\n");
  //   }
  //
  //
  //   fprintf(stderr, "  BTree element pair count: %lu\n",
  //           (uint64_t)btreeolc::BTreeLeaf<uint64_t, uint64_t>::maxEntries);
  //
  //   // If the key type is RDTSC we just run the special function
  // if (kt != RDTSC_KEY) {
  //   std::vector<keytype> init_keys;
  //   std::vector<keytype> keys;
  //   std::vector<uint64_t> values;
  //   std::vector<int> ranges;
  //   std::vector<int> ops; // INSERT = 0, READ = 1, UPDATE = 2
  //
  //   init_keys.reserve(50000000);
  //   keys.reserve(10000000);
  //   values.reserve(10000000);
  //   ranges.reserve(10000000);
  //   ops.reserve(10000000);
  //
  //   memset(&init_keys[0], 0x00, 50000000 * sizeof(keytype));
  //   memset(&keys[0], 0x00, 10000000 * sizeof(keytype));
  //   memset(&values[0], 0x00, 10000000 * sizeof(uint64_t));
  //   memset(&ranges[0], 0x00, 10000000 * sizeof(int));
  //   memset(&ops[0], 0x00, 10000000 * sizeof(int));
  //
  //   load(wl, kt, index_type, init_keys, keys, values, ranges, ops);
  //   printf("Finished loading workload file (mem = %lu)\n", MemUsage());
  //   if (index_type != TYPE_NONE) {
  //     // Then repeat executing the same workload
  //     while (repeat_counter > 0) {
  //       exec(wl, index_type, num_thread, init_keys, keys, values, ranges,
  //       ops); repeat_counter--; printf("Finished running benchmark (mem =
  //              % lu)\n ", MemUsage());
  //     }
  //   } else {
  //     fprintf(stderr, "Type None is selected - no execution phase\n");
  //   }
  // } else {
  //   fprintf(stderr, "Running RDTSC benchmark...\n");
  //   run_rdtsc_benchmark(index_type, num_thread, 50 * 1000 * 1000);
  // }
  //
  // exit_cleanup();
}
