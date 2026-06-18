#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "binary_io.h"
#include "common/parse_command_line.h"
#include "common/temporal_graph_io.h"
#include "parlay/internal/group_by.h"
#include "parlay/primitives.h"
#include "parlay/random.h"

// A signal graph stores only edge *appearances* (src, dest, timestamp); it
// carries no INSERT/DELETE update_info. This driver:
//   1. reads and parses the edges,
//   2. removes duplicate edges (same (src, dest), keeping the earliest one),
//   3. re-labels timestamps: by default groups edges by calendar year (raw
//      timestamps are Unix epoch seconds), labeling distinct years 1..#batches
//      by ascending year; with group_by_year=0 it instead renumbers the
//      distinct timestamps 1..#timestamps by ascending value,
//   4. sorts by the new label, randomly shuffling edges that share a label,
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
  commandLine P(
    argc,
    argv,
    "-p <signal_graph_binary_file> -o <output_folder> [-binary {0,1}] "
    "[-group_by_year {0,1}]\n"
    "  -binary 1 writes 0.bin (uint64 stream [N, 3, src, dest, timestamp, "
    "...]) instead of text 0.in\n"
    "  -group_by_year 0 renumbers the distinct timestamps 1..#timestamps by "
    "ascending value instead of grouping edges by calendar year (default 1)");

  const std::string filename = P.getOptionValue("-p", "");
  const std::filesystem::path output_dir = P.getOptionValue("-o", "");
  if (filename.empty() || output_dir.empty())
    P.badArgument();
  const bool binary = P.getOptionIntValue("-binary", 0) != 0;
  const bool group_by_year = P.getOptionIntValue("-group_by_year", 1) != 0;
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

  // NOTE: print several edges with their raw timestamps (before relabeling) so
  // the original timestamp format/scale can be inspected.
  std::cout << "Edges before timestamp relabeling (raw timestamps):\n";
  std::cout << std::setprecision(17);
  for (int i = 0; i < 5 && i < edges.size(); i++) {
    std::cout << "Edge " << i << ": src " << edges[i].src << ", dest "
              << edges[i].dest << ", timestamp ";
    if (ts_integral)
      std::cout << static_cast<uint64_t>(edges[i].timestamp) << "\n";
    else
      std::cout << edges[i].timestamp << "\n";
  }

  // 2. Remove duplicate edges, keeping the earliest occurrence of each
  // (src, dest). remove_duplicates_ordered stable-sorts by the given (src,
  // dest) comparator, then keeps the first of each equal group. Since edges is
  // timestamp-sorted, the stable sort leaves the smallest-timestamp copy first
  // within each group, so the earliest occurrence is the one kept.
  auto deduped_edges = parlay::remove_duplicates_ordered(
    edges, [](const EdgeRecord& a, const EdgeRecord& b) {
      return a.src < b.src || (a.src == b.src && a.dest < b.dest);
    });
  const size_t deduped_total = deduped_edges.size();

  // 3. Re-label timestamps on the deduplicated edges. dedup left them sorted by
  // (src, dest), so re-sort by raw timestamp first: then equal (or same-group)
  // values are contiguous, and an inclusive scan of the group-start flags gives
  // each edge the 1-based index of its group, which becomes its new timestamp.
  parlay::sort_inplace(deduped_edges, ts_cmp);
  parlay::sequence<size_t> labels;
  if (group_by_year) {
    // Group edges by calendar year: the raw timestamps are Unix epoch seconds,
    // so convert each to its UTC calendar year and label the distinct years
    // 1..#batches by ascending year, so every batch corresponds to one year.
    auto edge_year = parlay::tabulate(deduped_total, [&](size_t i) -> int {
      const std::time_t t =
        static_cast<std::time_t>(deduped_edges[i].timestamp);
      std::tm tm_buf;
      gmtime_r(&t, &tm_buf);
      return tm_buf.tm_year + 1900;
    });
    auto flags = parlay::tabulate(deduped_total, [&](size_t i) -> size_t {
      return (i == 0 || edge_year[i] != edge_year[i - 1]) ? 1 : 0;
    });
    labels = parlay::scan_inclusive(flags);
    if (deduped_total > 0)
      std::cout << "Year batches: " << labels.back() << " (years "
                << edge_year.front() << ".." << edge_year.back() << ")\n";
  } else {
    // Renumber the distinct timestamps 1..#timestamps by ascending value,
    // using the rank itself as the new timestamp.
    auto flags = parlay::tabulate(deduped_total, [&](size_t i) -> size_t {
      return (i == 0 ||
              deduped_edges[i].timestamp != deduped_edges[i - 1].timestamp)
               ? 1
               : 0;
    });
    labels = parlay::scan_inclusive(flags);
    if (deduped_total > 0)
      std::cout << "Distinct timestamps: " << labels.back() << "\n";
  }
  parlay::parallel_for(0, deduped_total, [&](size_t i) {
    deduped_edges[i].timestamp = static_cast<double>(labels[i]);
  });

  // 4. Sort by the new label, randomly shuffling edges that share a label. A
  // global random_shuffle followed by a stable sort on the label leaves a
  // uniformly random order within each label group while keeping labels
  // globally ordered.
  deduped_edges = parlay::random_shuffle(deduped_edges);
  parlay::stable_sort_inplace(deduped_edges, ts_cmp);

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

    // Per-batch summary: count edges per label with histogram_by_key. The
    // number of distinct labels is the histogram size; the per-batch breakdown
    // (sorted by label) is only printed when grouping by year, since rank mode
    // can have far too many distinct timestamps to list.
    auto batch_hist =
      parlay::histogram_by_key(parlay::map(deduped_edges, [](auto& e) {
        return static_cast<uint64_t>(e.timestamp);
      }));
    if (group_by_year) {
      parlay::sort_inplace(batch_hist, [](auto const& a, auto const& b) {
        return a.first < b.first;
      });
      for (auto const& [batch, count] : batch_hist)
        std::cout << "  Batch " << batch << ": " << count << " edges\n";
    }
    std::cout << (group_by_year ? "Distinct years (batches) after dedup: "
                                : "Distinct timestamps after dedup: ")
              << batch_hist.size() << "\n";
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
