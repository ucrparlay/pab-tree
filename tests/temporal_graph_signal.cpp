#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "binary_io.h"
#include "common/temporal_graph_io.h"
#include "parlay/internal/group_by.h"
#include "parlay/primitives.h"
#include "parlay/random.h"

// A signal graph stores only edge *appearances* (src, dest, timestamp); it
// carries no INSERT/DELETE update_info. This driver:
//   1. reads and parses the edges,
//   2. re-labels each distinct timestamp 1..K by ascending rank,
//   3. removes duplicate edges (same (src, dest), keeping the earliest one),
//   4. sorts by timestamp, randomly shuffling edges that share a timestamp,
//   5. writes all edges to 0.in (text) or 0.bin (binary, [N, 3, src, dest,
//      timestamp, ...]). Each output row is "src dest timestamp".

struct EdgeRecord
{
  uint64_t src;
  uint64_t dest;
  double timestamp;
};

int
main(int argc, char** argv)
{
  // temporal_graph_signal <signal_graph_binary_file> <output_folder> [binary=0]
  // [equal_timestamp=0]

  if (argc < 3 || argc > 5) {
    std::cerr << "Usage: " << argv[0]
              << " <signal_graph_binary_file> <output_folder> [binary=0] "
                 "[equal_timestamp=0]\n"
              << "  binary=1 writes <i>.bin (uint64 stream [N, 3, src, dest, "
                 "timestamp, ...]) instead of text <i>.in\n"
              << "  equal_timestamp=1 divides the edges into 10 equal batches "
                 "labeled 1..10 instead of ranking distinct timestamps\n";
    return EXIT_FAILURE;
  }

  const std::string filename = argv[1];
  const std::filesystem::path output_dir = argv[2];
  const bool binary = argc >= 4 && std::atoi(argv[3]) != 0;
  const bool equal_timestamp = argc == 5 && std::atoi(argv[4]) != 0;
  IO::InputGraph graph(filename);
  graph.print_summary();

  // 1. Read and parse the edges.
  const bool ts_integral = IO::data_type_is_integer(graph.timestamp_type());
  const uint64_t total_updates = graph.total_updates();
  auto edges = parlay::tabulate(total_updates, [&](size_t i) {
    const double ts =
      IO::data_type_is_integer(graph.timestamp_type())
        ? static_cast<double>(graph.get_row_timestamp_integral(i))
        : graph.get_row_timestamp_floating_point(i);
    return EdgeRecord{ .src = graph.get_row_src(i),
                       .dest = graph.get_row_dest(i),
                       .timestamp = ts };
  });

  auto ts_cmp = [](const EdgeRecord& a, const EdgeRecord& b) {
    return a.timestamp < b.timestamp;
  };
  parlay::sort_inplace(edges, ts_cmp);

  // 2. Re-label timestamps. Two modes:
  if (equal_timestamp) {
    // Equal-timestamp mode: edges is timestamp-sorted, so split it into 10
    // equal contiguous batches by position and label each batch 1..10. Edge i
    // falls in batch (i * 10) / n (0-based), labeled (i * 10) / n + 1.
    constexpr size_t kNumBatches = 10;
    const size_t n = edges.size();
    parlay::parallel_for(0, n, [&](size_t i) {
      edges[i].timestamp = static_cast<double>((i * kNumBatches) / n + 1);
    });
    std::cout << "Equal-timestamp mode: divided " << n << " edges into "
              << kNumBatches << " batches labeled 1.." << kNumBatches << "\n";
  } else {
    // Rank mode: count distinct timestamps and re-label them 1..K by ascending
    // rank. edges is timestamp-sorted, so each distinct value starts a new
    // group. An inclusive scan of the group-start flags gives each edge the
    // 1-based rank of its timestamp, which becomes its new timestamp.
    auto ts_new_flags = parlay::tabulate(edges.size(), [&](size_t i) -> size_t {
      return (i == 0 || edges[i].timestamp != edges[i - 1].timestamp) ? 1 : 0;
    });
    auto ts_ranks = parlay::scan_inclusive(ts_new_flags);
    const size_t num_distinct_ts = edges.empty() ? 0 : ts_ranks.back();
    std::cout << "Distinct timestamps: " << num_distinct_ts << "\n";
    parlay::parallel_for(0, edges.size(), [&](size_t i) {
      edges[i].timestamp = static_cast<double>(ts_ranks[i]);
    });
  }

  for (int i = 0; i < 5 && i < edges.size(); i++) {
    std::cout << "Edge " << i << ": src " << edges[i].src << ", dest "
              << edges[i].dest << ", timestamp " << edges[i].timestamp << "\n";
  }

  // 3. Remove duplicate edges, keeping the earliest occurrence of each
  // (src, dest). remove_duplicates_ordered stable-sorts by the given (src,
  // dest) comparator, then keeps the first of each equal group. Since edges is
  // timestamp-sorted, the stable sort leaves the smallest-timestamp copy first
  // within each group, so the earliest occurrence is the one kept.
  auto deduped_edges = parlay::remove_duplicates_ordered(
    edges, [](const EdgeRecord& a, const EdgeRecord& b) {
      return a.src < b.src || (a.src == b.src && a.dest < b.dest);
    });

  // 4. Sort by timestamp, randomly shuffling edges that share a timestamp. A
  // global random_shuffle followed by a stable sort on timestamp leaves a
  // uniformly random order within each equal-timestamp group while keeping the
  // timestamps globally ordered.
  deduped_edges = parlay::random_shuffle(deduped_edges);
  parlay::stable_sort_inplace(deduped_edges, ts_cmp);

  const size_t deduped_total = deduped_edges.size();
  std::cout << "Original edges: " << total_updates << "\n";
  std::cout << "Deduplicated to " << deduped_total << " unique edges\n";
  if (deduped_total > 0) {
    auto* max_u =
      parlay::max_element(deduped_edges, [](auto const& a, auto const& b) {
        return a.src < b.src;
      });
    auto* max_v =
      parlay::max_element(deduped_edges, [](auto const& a, auto const& b) {
        return a.dest < b.dest;
      });
    std::cout << "Max vertex ids: src " << max_u->src << ", dest "
              << max_v->dest << "\n";
    auto kv_num = parlay::histogram_by_key(
      parlay::map(deduped_edges, [](auto& e) { return e.src; }));
    auto* max_degree_vertex = parlay::max_element(
      kv_num, [](auto const& a, auto const& b) { return a.second < b.second; });
    std::cout << "Max degree: " << max_degree_vertex->second << "\n";
    // Average out-degree over vertices that appear as a source: total edges
    // divided by the number of distinct source vertices.
    std::cout << "Average degree: "
              << static_cast<double>(deduped_total) / kv_num.size() << "\n";
  }

  // 5. Write all edges to a single file. Each row is "src dest timestamp".
  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  if (ec) {
    std::cerr << "Failed to create output directory: " << output_dir
              << " error: " << ec.message() << "\n";
    return EXIT_FAILURE;
  }

  auto write_edges = [&](const parlay::sequence<EdgeRecord>& batch_edges,
                         const std::filesystem::path& out_path) -> bool {
    if (binary) {
      parlay::sequence<uint64_t> payload(batch_edges.size() * 3);
      parlay::parallel_for(0, batch_edges.size(), [&](size_t i) {
        payload[i * 3] = batch_edges[i].src;
        payload[(i * 3) + 1] = batch_edges[i].dest;
        payload[(i * 3) + 2] = static_cast<uint64_t>(batch_edges[i].timestamp);
      });
      WriteBinaryEntries(out_path.string(), batch_edges.size(), 3, payload);
      return true;
    }
    std::ofstream out(out_path);
    if (!out) {
      std::cerr << "Failed to open output file: " << out_path << "\n";
      return false;
    }
    out << std::setprecision(17);
    out << batch_edges.size() << " 3\n";
    for (const auto& edge : batch_edges) {
      out << edge.src << " " << edge.dest << " ";
      if (ts_integral)
        out << static_cast<uint64_t>(edge.timestamp) << "\n";
      else
        out << edge.timestamp << "\n";
    }
    return true;
  };

  const auto out_path = output_dir / (binary ? "0.bin" : "0.in");
  if (!write_edges(deduped_edges, out_path)) {
    return EXIT_FAILURE;
  }
  std::cout << "Wrote " << deduped_total << " edges to " << out_path << "\n";

  return EXIT_SUCCESS;
}
