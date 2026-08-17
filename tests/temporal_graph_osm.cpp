#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>

#include "binary_io.h"
#include "common/parse_command_line.h"
#include "common/temporal_graph_io.h"
#include "parlay/internal/group_by.h"
#include "parlay/primitives.h"

struct EdgeRecord {
	uint64_t src;
	uint64_t dest;
	IO::UpdateInfo update_info;
	double timestamp;
};

int main(int argc, char **argv)
{
	commandLine P(
		argc, argv,
		"-p <temporal_graph_binary_file> -o <output_folder> [-binary {0,1}] "
		"[-group_by_year {0,1}]\n"
		"  -binary 1 writes <i>_insert.bin/<i>_delete.bin (uint64 stream [N, 2, "
		"src, dest, ...]) instead of text .in\n"
		"  -group_by_year 1 makes one batch per calendar year (raw timestamps are "
		"Unix epoch seconds) instead of 10 equal-size timestamp-ordered batches");

	const std::string filename = P.getOptionValue("-p", "");
	const std::filesystem::path output_dir = P.getOptionValue("-o", "");
	if (filename.empty() || output_dir.empty())
		P.badArgument();
	const bool binary = P.getOptionIntValue("-binary", 0) != 0;
	const bool group_by_year = P.getOptionIntValue("-group_by_year", 0) != 0;
	IO::InputGraph graph(filename);
	graph.print_summary();

	// using EdgeRecord = std::tuple<uint32_t, uint32_t, IO::UpdateInfo, double>;
	const uint64_t total_updates = graph.total_updates();

	auto edges_by_timestamp = parlay::tabulate(total_updates, [&](size_t i) {
		const double ts =
			IO::data_type_is_integer(graph.timestamp_type()) ?
				static_cast<double>(graph.get_row_timestamp_integral(i)) :
				graph.get_row_timestamp_floating_point(i);
		return EdgeRecord{ .src = graph.get_row_src(i),
						   .dest = graph.get_row_dest(i),
						   .update_info = graph.get_row_info(i),
						   .timestamp = ts };
	});

	parlay::sort_inplace(
		edges_by_timestamp,
		[](const EdgeRecord &a, const EdgeRecord &b) -> uint64_t {
			return a.timestamp < b.timestamp;
		});

	// Deduplicate inserts and deletes separately before batching.
	// Input is timestamp-sorted so the first occurrence of each (src, dest) has
	// the smallest timestamp. Stable-sort by (src, dest) preserves that order
	// within each group, then pack keeps only the first of each group.
	auto dedup_keep_earliest =
		[](parlay::sequence<EdgeRecord> edges) -> parlay::sequence<EdgeRecord> {
		parlay::stable_sort_inplace(
			edges, [](const EdgeRecord &a, const EdgeRecord &b) {
				return a.src < b.src || (a.src == b.src && a.dest < b.dest);
			});
		// NOTE: sort + keep = remove duplicates
		auto keep = parlay::tabulate(edges.size(), [&](size_t i) -> bool {
			return i == 0 || edges[i].src != edges[i - 1].src ||
				   edges[i].dest != edges[i - 1].dest;
		});
		return parlay::pack(edges, keep);
	};

	auto ts_cmp = [](const EdgeRecord &a, const EdgeRecord &b) {
		return a.timestamp < b.timestamp;
	};
	auto all_inserts = dedup_keep_earliest(
		parlay::filter(edges_by_timestamp, [](const EdgeRecord &e) {
			return e.update_info == IO::UpdateInfo::INSERT;
		}));
	auto all_deletes = dedup_keep_earliest(
		parlay::filter(edges_by_timestamp, [](const EdgeRecord &e) {
			return e.update_info == IO::UpdateInfo::DELETE;
		}));
	auto deduped_edges = parlay::merge(all_inserts, all_deletes, ts_cmp);
	parlay::sort_inplace(deduped_edges, ts_cmp);

	std::cout << "Original edge: " << edges_by_timestamp.size() << std::endl;
	std::cout << "Deduplicated to " << deduped_edges.size()
			  << " unique edges\n";
	auto *max_u =
		parlay::max_element(deduped_edges, [&](auto const &a, auto const &b) {
			return a.src < b.src;
		});
	auto *max_v =
		parlay::max_element(deduped_edges, [&](auto const &a, auto const &b) {
			return a.dest < b.dest;
		});
	std::cout << "max elements are: " << (*max_u).src << " " << (*max_v).dest
			  << std::endl;

	const size_t deduped_total = deduped_edges.size();

	auto kv_num = parlay::histogram_by_key(
		parlay::map(all_inserts, [&](auto &e) { return e.src; }));
	auto *max_degree_vertex =
		parlay::max_element(kv_num, [&](auto const &a, auto const &b) {
			return a.second < b.second;
		});
	std::cout << "max degree: " << (*max_degree_vertex).second << std::endl;

	// generate batch
	using BatchSplit =
		std::pair<parlay::sequence<EdgeRecord>, parlay::sequence<EdgeRecord>>;
	// Slice deduped_edges[begin, end) and split it into its insert/delete edges.
	auto make_batch = [&](size_t begin, size_t end) -> BatchSplit {
		auto batch_edges = parlay::tabulate(
			end - begin, [&](size_t j) { return deduped_edges[begin + j]; });
		auto insert_edges =
			parlay::filter(batch_edges, [](const EdgeRecord &edge) {
				return edge.update_info == IO::UpdateInfo::INSERT;
			});
		auto delete_edges =
			parlay::filter(batch_edges, [](const EdgeRecord &edge) {
				return edge.update_info == IO::UpdateInfo::DELETE;
			});
		return BatchSplit{ std::move(insert_edges), std::move(delete_edges) };
	};

	parlay::sequence<BatchSplit> edge_batches;
	// When grouping by year, batch_years[b] records the calendar year of batch b
	// (for logging only; the year-relabeled timestamp is not written to output,
	// which carries just src/dest).
	parlay::sequence<int> batch_years;
	if (group_by_year) {
		// deduped_edges is timestamp-sorted, so edges of the same calendar year are
		// contiguous. Convert each Unix-epoch-seconds timestamp to its UTC year,
		// then make one batch per distinct year (ascending). The batch index keeps
		// the existing 0-based file-naming style; batch_years maps it to the year.
		auto edge_year = parlay::tabulate(deduped_total, [&](size_t i) -> int {
			const std::time_t t =
				static_cast<std::time_t>(deduped_edges[i].timestamp);
			std::tm tm_buf;
			gmtime_r(&t, &tm_buf);
			return tm_buf.tm_year + 1900;
		});
		auto idx = parlay::tabulate(deduped_total,
									[](size_t i) -> size_t { return i; });
		auto starts = parlay::filter(idx, [&](size_t i) {
			return i == 0 || edge_year[i] != edge_year[i - 1];
		});
		const size_t num_year_batches = starts.size();
		edge_batches = parlay::sequence<BatchSplit>(num_year_batches);
		batch_years = parlay::sequence<int>(num_year_batches);
		for (size_t b = 0; b < num_year_batches; ++b) {
			const size_t begin = starts[b];
			const size_t end =
				(b + 1 < num_year_batches) ? starts[b + 1] : deduped_total;
			edge_batches[b] = make_batch(begin, end);
			batch_years[b] = edge_year[begin];
		}
	} else {
		constexpr size_t kNumBatches = 10;
		edge_batches = parlay::sequence<BatchSplit>(kNumBatches);
		for (size_t batch = 0; batch < kNumBatches; ++batch) {
			const size_t begin = (deduped_total * batch) / kNumBatches;
			const size_t end = (deduped_total * (batch + 1)) / kNumBatches;
			edge_batches[batch] = make_batch(begin, end);
		}
	}

	std::error_code ec;
	std::filesystem::create_directories(output_dir, ec);
	if (ec) {
		std::cerr << "Failed to create output directory: " << output_dir
				  << " error: " << ec.message() << "\n";
		return EXIT_FAILURE;
	}

	auto write_edges = [&](const parlay::sequence<EdgeRecord> &edges,
						   const std::filesystem::path &out_path) -> bool {
		if (binary) {
			parlay::sequence<uint64_t> payload(edges.size() * 2);
			parlay::parallel_for(0, edges.size(), [&](size_t i) {
				payload[i * 2] = edges[i].src;
				payload[(i * 2) + 1] = edges[i].dest;
			});
			WriteBinaryEntries(out_path.string(), edges.size(), 2, payload);
			return true;
		}
		std::ofstream out(out_path);
		if (!out) {
			std::cerr << "Failed to open output file: " << out_path << "\n";
			return false;
		}
		out << edges.size() << " 2\n";
		for (const auto &edge : edges) {
			out << edge.src << " " << edge.dest << "\n";
		}
		return true;
	};

	// return 0;

	std::cout << "Created " << edge_batches.size() << " "
			  << (group_by_year ? "per-year" : "timestamp-ordered")
			  << " batches\n";
	for (size_t batch = 0; batch < edge_batches.size(); ++batch) {
		const auto insert_path =
			output_dir /
			(std::to_string(batch) + (binary ? "_insert.bin" : "_insert.in"));
		const auto delete_path =
			output_dir /
			(std::to_string(batch) + (binary ? "_delete.bin" : "_delete.in"));
		if (!write_edges(edge_batches[batch].first, insert_path) ||
			!write_edges(edge_batches[batch].second, delete_path)) {
			return EXIT_FAILURE;
		}

		std::cout << "Batch " << batch;
		if (group_by_year)
			std::cout << " (year " << batch_years[batch] << ")";
		std::cout << ": " << edge_batches[batch].first.size() << " inserts, "
				  << edge_batches[batch].second.size() << " deletes\n";
	}

	return EXIT_SUCCESS;
}
