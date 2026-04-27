#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>

#include "common/temporal_graph_io.h"
#include "parlay/primitives.h"

struct EdgeRecord
{
  uint64_t src;
  uint64_t dest;
  IO::UpdateInfo update_info;
  double timestamp;
};

int
main(int argc, char** argv)
{
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0]
              << " <temporal_graph_binary_file> <output_folder>\n";
    return EXIT_FAILURE;
  }

  const std::string filename = argv[1];
  const std::filesystem::path output_dir = argv[2];
  IO::InputGraph graph(filename);
  graph.print_summary();

  // using EdgeRecord = std::tuple<uint32_t, uint32_t, IO::UpdateInfo, double>;
  const uint64_t total_updates = graph.total_updates();

  auto edges_by_timestamp = parlay::tabulate(total_updates, [&](size_t i) {
    const double ts =
      IO::data_type_is_integer(graph.timestamp_type())
        ? static_cast<double>(graph.get_row_timestamp_integral(i))
        : graph.get_row_timestamp_floating_point(i);
    return EdgeRecord{ .src = graph.get_row_src(i),
                       .dest = graph.get_row_dest(i),
                       .update_info = graph.get_row_info(i),
                       .timestamp = ts };
  });

  parlay::sort_inplace(
    edges_by_timestamp,
    [](const EdgeRecord& a, const EdgeRecord& b) -> uint64_t {
      return a.timestamp < b.timestamp;
    });

  // Deduplicate inserts and deletes separately before batching.
  // Input is timestamp-sorted so the first occurrence of each (src, dest) has
  // the smallest timestamp. Stable-sort by (src, dest) preserves that order
  // within each group, then pack keeps only the first of each group.
  auto dedup_keep_earliest =
    [](parlay::sequence<EdgeRecord> edges) -> parlay::sequence<EdgeRecord> {
    parlay::stable_sort_inplace(
      edges, [](const EdgeRecord& a, const EdgeRecord& b) {
        return a.src < b.src || (a.src == b.src && a.dest < b.dest);
      });
    // NOTE: sort + keep = remove duplicates
    auto keep = parlay::tabulate(edges.size(), [&](size_t i) -> bool {
      return i == 0 || edges[i].src != edges[i - 1].src ||
             edges[i].dest != edges[i - 1].dest;
    });
    return parlay::pack(edges, keep);
  };

  auto ts_cmp = [](const EdgeRecord& a, const EdgeRecord& b) {
    return a.timestamp < b.timestamp;
  };
  auto all_inserts = dedup_keep_earliest(
    parlay::filter(edges_by_timestamp, [](const EdgeRecord& e) {
      return e.update_info == IO::UpdateInfo::INSERT;
    }));
  auto all_deletes = dedup_keep_earliest(
    parlay::filter(edges_by_timestamp, [](const EdgeRecord& e) {
      return e.update_info == IO::UpdateInfo::DELETE;
    }));
  auto deduped_edges = parlay::merge(all_inserts, all_deletes, ts_cmp);
  parlay::sort_inplace(deduped_edges, ts_cmp);

  std::cout << "Original edge: " << edges_by_timestamp.size() << std::endl;
  std::cout << "Deduplicated to " << deduped_edges.size() << " unique edges\n";
  auto* max_u = parlay::max_element(
    deduped_edges, [&](auto const& a, auto const& b) { return a.src < b.src; });
  auto* max_v =
    parlay::max_element(deduped_edges, [&](auto const& a, auto const& b) {
      return a.dest < b.dest;
    });
  std::cout << "max elements are: " << (*max_u).src << " " << (*max_v).dest
            << std::endl;

  const size_t deduped_total = deduped_edges.size();

  // generate batch
  constexpr size_t kNumBatches = 10;
  using BatchSplit =
    std::pair<parlay::sequence<EdgeRecord>, parlay::sequence<EdgeRecord>>;
  parlay::sequence<BatchSplit> edge_batches(kNumBatches);
  for (size_t batch = 0; batch < kNumBatches; ++batch) {
    const size_t begin = (deduped_total * batch) / kNumBatches;
    const size_t end = (deduped_total * (batch + 1)) / kNumBatches;
    auto batch_edges = parlay::tabulate(
      end - begin, [&](size_t j) { return deduped_edges[begin + j]; });
    auto insert_edges = parlay::filter(batch_edges, [](const EdgeRecord& edge) {
      return edge.update_info == IO::UpdateInfo::INSERT;
    });
    auto delete_edges = parlay::filter(batch_edges, [](const EdgeRecord& edge) {
      return edge.update_info == IO::UpdateInfo::DELETE;
    });
    edge_batches[batch] =
      BatchSplit{ std::move(insert_edges), std::move(delete_edges) };
  }

  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  if (ec) {
    std::cerr << "Failed to create output directory: " << output_dir
              << " error: " << ec.message() << "\n";
    return EXIT_FAILURE;
  }

  auto write_edges = [&](const parlay::sequence<EdgeRecord>& edges,
                         const std::filesystem::path& out_path) -> bool {
    std::ofstream out(out_path);
    if (!out) {
      std::cerr << "Failed to open output file: " << out_path << "\n";
      return false;
    }
    out << edges.size() << " 2\n";
    for (const auto& edge : edges) {
      out << edge.src << " " << edge.dest << "\n";
    }
    return true;
  };

  std::cout << "Created " << edge_batches.size()
            << " timestamp-ordered batches\n";
  for (size_t batch = 0; batch < edge_batches.size(); ++batch) {
    const auto insert_path =
      output_dir / (std::to_string(batch) + "_insert.in");
    const auto delete_path =
      output_dir / (std::to_string(batch) + "_delete.in");
    if (!write_edges(edge_batches[batch].first, insert_path) ||
        !write_edges(edge_batches[batch].second, delete_path)) {
      return EXIT_FAILURE;
    }

    std::cout << "Batch " << batch << ": " << edge_batches[batch].first.size()
              << " inserts, " << edge_batches[batch].second.size()
              << " deletes\n";
  }

  return EXIT_SUCCESS;
}
