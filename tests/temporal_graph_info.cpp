#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>

#include "common/temporal_graph_io.h"

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

  using EdgeRecord = std::tuple<uint64_t, uint64_t, IO::UpdateInfo, double>;
  const uint64_t total_updates = graph.total_updates();

  auto edges_by_timestamp = parlay::tabulate(total_updates, [&](size_t i) {
    const double ts = IO::data_type_is_integer(graph.timestamp_type())
                        ? static_cast<double>(graph.get_row_timestamp_integral(i))
                        : graph.get_row_timestamp_floating_point(i);
    return EdgeRecord{
      graph.get_row_src(i), graph.get_row_dest(i), graph.get_row_info(i), ts
    };
  });

  parlay::integer_sort_inplace(edges_by_timestamp,
                               [](const EdgeRecord& edge) -> uint64_t {
                                 return static_cast<uint64_t>(std::get<3>(edge));
                               });

  constexpr size_t kNumBatches = 10;
  using BatchSplit =
    std::pair<parlay::sequence<EdgeRecord>, parlay::sequence<EdgeRecord>>;
  parlay::sequence<BatchSplit> edge_batches(kNumBatches);
  for (size_t batch = 0; batch < kNumBatches; ++batch) {
    const size_t begin = (total_updates * batch) / kNumBatches;
    const size_t end = (total_updates * (batch + 1)) / kNumBatches;
    auto batch_edges = parlay::tabulate(end - begin, [&](size_t j) {
      return edges_by_timestamp[begin + j];
    });
    auto insert_edges = parlay::filter(batch_edges, [](const EdgeRecord& edge) {
      return std::get<2>(edge) == IO::UpdateInfo::INSERT;
    });
    auto delete_edges = parlay::filter(batch_edges, [](const EdgeRecord& edge) {
      return std::get<2>(edge) == IO::UpdateInfo::DELETE;
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
      out << std::get<0>(edge) << " " << std::get<1>(edge) << "\n";
    }
    return true;
  };

  std::cout << "Created " << edge_batches.size()
            << " timestamp-ordered batches\n";
  for (size_t batch = 0; batch < edge_batches.size(); ++batch) {
    const auto insert_path = output_dir / (std::to_string(batch) + "_insert.in");
    const auto delete_path = output_dir / (std::to_string(batch) + "_delete.in");
    if (!write_edges(edge_batches[batch].first, insert_path) ||
        !write_edges(edge_batches[batch].second, delete_path)) {
      return EXIT_FAILURE;
    }

    std::cout << "Batch " << batch << ": "
              << edge_batches[batch].first.size() << " inserts, "
              << edge_batches[batch].second.size() << " deletes\n";
  }

  return EXIT_SUCCESS;
}
