#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include <parlay/primitives.h>

// #include "absl/container/flat_hash_map.h"
// #include "absl/container/flat_hash_set.h"

namespace IO
{

class empty {};

enum class DataType : uint8_t {
	NONE = 0,
	UINT8 = 10,
	UINT16 = 11,
	UINT32 = 12,
	UINT64 = 13,
	INT8 = 20,
	INT16 = 21,
	INT32 = 22,
	INT64 = 23,
	FLOAT32 = 30,
	FLOAT64 = 31
};
template <DataType DT> struct CType;

template <> struct CType<DataType::NONE> {
	using type = empty;
};
template <> struct CType<DataType::UINT8> {
	using type = uint8_t;
};
template <> struct CType<DataType::UINT16> {
	using type = uint16_t;
};
template <> struct CType<DataType::UINT32> {
	using type = uint32_t;
};
template <> struct CType<DataType::UINT64> {
	using type = uint64_t;
};
template <> struct CType<DataType::INT8> {
	using type = int8_t;
};
template <> struct CType<DataType::INT16> {
	using type = int16_t;
};
template <> struct CType<DataType::INT32> {
	using type = int32_t;
};
template <> struct CType<DataType::INT64> {
	using type = int64_t;
};
template <> struct CType<DataType::FLOAT32> {
	using type = float;
};
template <> struct CType<DataType::FLOAT64> {
	using type = double;
};

inline std::string data_type_to_string(DataType wt)
{
	switch (wt) {
	case DataType::NONE:
		return "None";
	case DataType::UINT8:
		return "uint8_t";
	case DataType::UINT16:
		return "uint16_t";
	case DataType::UINT32:
		return "uint32_t";
	case DataType::UINT64:
		return "uint64_t";
	case DataType::INT8:
		return "int8_t";
	case DataType::INT16:
		return "int16_t";
	case DataType::INT32:
		return "int32_t";
	case DataType::INT64:
		return "int64_t";
	case DataType::FLOAT32:
		return "float";
	case DataType::FLOAT64:
		return "double";
	}
}
inline constexpr uint64_t data_type_to_size(DataType wt)
{
	switch (wt) {
	case DataType::NONE:
		return 0;
	case DataType::UINT8:
		return 1;
	case DataType::UINT16:
		return 2;
	case DataType::UINT32:
		return 4;
	case DataType::UINT64:
		return 8;
	case DataType::INT8:
		return 1;
	case DataType::INT16:
		return 2;
	case DataType::INT32:
		return 4;
	case DataType::INT64:
		return 8;
	case DataType::FLOAT32:
		return 4;
	case DataType::FLOAT64:
		return 8;
	}
}
constexpr bool data_type_is_integer(DataType wt)
{
	switch (wt) {
	case DataType::NONE:
	case DataType::UINT8:
	case DataType::UINT16:
	case DataType::UINT32:
	case DataType::UINT64:
	case DataType::INT8:
	case DataType::INT16:
	case DataType::INT32:
	case DataType::INT64:
		return true;
	case DataType::FLOAT32:
	case DataType::FLOAT64:
		return false;
	}
}

enum class UpdateInfo : uint8_t { INSERT = 0, DELETE = 1 };

inline std::string update_info_to_string(UpdateInfo t)
{
	switch (t) {
	case UpdateInfo::INSERT:
		return "INSERT";
	case UpdateInfo::DELETE:
		return "DELETE";
	}
}

struct GraphHeader {
	uint64_t num_vertices = -1;
	uint64_t num_unique_edges = -1;
	uint64_t total_updates = -1;
	bool if_signal_graph = false;
	DataType vertex_id_type = DataType::NONE;
	DataType timestamp_type = DataType::NONE;
	bool if_directed = false;
	DataType weight_type = DataType::NONE;

	GraphHeader()
	{
	}

	GraphHeader(const std::string &filename)
	{
		std::ifstream in(filename, std::ios::binary);
		if (!in) {
			std::cerr << "Could not open file";
			return;
		}

		in.read(reinterpret_cast<char *>(&num_vertices), sizeof(num_vertices));
		in.read(reinterpret_cast<char *>(&num_unique_edges),
				sizeof(num_unique_edges));
		in.read(reinterpret_cast<char *>(&total_updates),
				sizeof(total_updates));

		uint8_t signal;
		in.read(reinterpret_cast<char *>(&signal), sizeof(uint8_t));
		if_signal_graph = signal;

		uint8_t vertex_id, timestamp;
		in.read(reinterpret_cast<char *>(&vertex_id), sizeof(uint8_t));
		in.read(reinterpret_cast<char *>(&timestamp), sizeof(uint8_t));
		vertex_id_type = static_cast<DataType>(vertex_id);
		timestamp_type = static_cast<DataType>(timestamp);

		uint8_t directed;
		in.read(reinterpret_cast<char *>(&directed), sizeof(uint8_t));
		if_directed = directed;

		uint8_t weight;
		in.read(reinterpret_cast<char *>(&weight), sizeof(uint8_t));
		weight_type = static_cast<DataType>(weight);
		return;
	}
	void print() const
	{
		std::cout << "Number of vertices: " << num_vertices << "\n";
		std::cout << "Number of unique edges: " << num_unique_edges << "\n";
		std::cout << "Total updates: " << total_updates << "\n";
		std::cout << "Signal graph: " << (if_signal_graph ? "true" : "false")
				  << "\n";
		std::cout << "Vertex ID type: " << data_type_to_string(vertex_id_type)
				  << "\n";
		std::cout << "Timestamp type: " << data_type_to_string(timestamp_type)
				  << "\n";
		std::cout << "Directed: " << (if_directed ? "true" : "false") << "\n";
		std::cout << "Weight type: " << data_type_to_string(weight_type)
				  << "\n";
	}
	static constexpr uint64_t header_size_in_bytes = 32;
	// round up the header size to 32 bytes for future proofing and alignment
	// sizeof(uint64_t) * 3 + sizeof(bool) * 2 + sizeof(DataType) * 3;
};

class InputGraph {
	GraphHeader header = {};

	void *mapped = nullptr;
	bool valid;
	size_t filesize = -1;
	static constexpr uint64_t header_size = GraphHeader::header_size_in_bytes;
	static constexpr uint64_t src_offset = 0;

	uint64_t row_size;
	uint64_t dest_offset;
	uint64_t ts_offset;
	uint64_t weight_offset;
	uint64_t info_offset;

	uint64_t get_row_size()
	{
		uint64_t result = 0;
		result += 2 * data_type_to_size(header.vertex_id_type);
		result += data_type_to_size(header.timestamp_type);
		result += data_type_to_size(header.weight_type);
		if (!header.if_signal_graph) {
			result += sizeof(UpdateInfo);
		}
		return result;
	}

public:
	InputGraph(const std::string &filename)
	{
		header = GraphHeader(filename);
		row_size = get_row_size();
		dest_offset = src_offset + data_type_to_size(header.vertex_id_type);
		ts_offset = dest_offset + data_type_to_size(header.vertex_id_type);
		weight_offset = ts_offset + data_type_to_size(header.timestamp_type);
		info_offset = weight_offset + data_type_to_size(header.weight_type);

		int fd = open(filename.c_str(), O_RDONLY);
		if (fd == -1) {
			std::cerr << "issue opening file";
			valid = false;
			return;
		}

		struct stat st;
		if (fstat(fd, &st) == -1) {
			std::cerr << "issue fstating file";
			valid = false;
			return;
		}
		filesize = st.st_size;
		mapped = mmap(nullptr, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);

		if (mapped == MAP_FAILED) {
			std::cerr << "issue mmaping file";
			valid = false;
			return;
		}
	}
	template <typename type = void> uint64_t get_row_src(uint64_t i)
	{
		if constexpr (std::is_same_v<type, uint32_t>) {
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + src_offset);
		}

		switch (header.vertex_id_type) {
		case DataType::UINT8:
			return *reinterpret_cast<uint8_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + src_offset);
		case DataType::UINT16:
			return *reinterpret_cast<uint16_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + src_offset);
		case DataType::UINT32:
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + src_offset);
		case DataType::UINT64:
			return *reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + src_offset);
		case DataType::NONE:
		case DataType::INT8:
		case DataType::INT16:
		case DataType::INT32:
		case DataType::INT64:
		case DataType::FLOAT32:
		case DataType::FLOAT64:
			assert(false);
			return 0;
		}
	}
	template <typename type = void> uint64_t get_row_dest(uint64_t i)
	{
		if constexpr (std::is_same_v<type, uint32_t>) {
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + dest_offset);
		}

		switch (header.vertex_id_type) {
		case DataType::UINT8:
			return *reinterpret_cast<uint8_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + dest_offset);
		case DataType::UINT16:
			return *reinterpret_cast<uint16_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + dest_offset);
		case DataType::UINT32:
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + dest_offset);
		case DataType::UINT64:
			return *reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + dest_offset);
		case DataType::NONE:
		case DataType::INT8:
		case DataType::INT16:
		case DataType::INT32:
		case DataType::INT64:
		case DataType::FLOAT32:
		case DataType::FLOAT64:
			assert(false);
			return 0;
		}
	}
	template <typename type = void>
	int64_t get_row_timestamp_integral(uint64_t i)
	{
		if constexpr (std::is_same_v<type, uint32_t>) {
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		}

		switch (header.vertex_id_type) {
		case DataType::UINT8:
			return *reinterpret_cast<uint8_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::UINT16:
			return *reinterpret_cast<uint16_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		case DataType::UINT32:
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		case DataType::UINT64:
			return *reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		case DataType::INT8:
			return *reinterpret_cast<int8_t *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + ts_offset);
		case DataType::INT16:
			return *reinterpret_cast<int16_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::INT32:
			return *reinterpret_cast<int32_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::INT64:
			return *reinterpret_cast<int64_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::FLOAT32:
			return *reinterpret_cast<float *>(static_cast<char *>(mapped) +
											  header.header_size_in_bytes +
											  row_size * i + ts_offset);
		case DataType::FLOAT64:
			return *reinterpret_cast<double *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + ts_offset);
		case DataType::NONE:
			return i;
		}
	}
	double get_row_timestamp_floating_point(uint64_t i)
	{
		switch (header.vertex_id_type) {
		case DataType::UINT8:
			return *reinterpret_cast<uint8_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::UINT16:
			return *reinterpret_cast<uint16_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		case DataType::UINT32:
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		case DataType::UINT64:
			return *reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + ts_offset);
		case DataType::INT8:
			return *reinterpret_cast<int8_t *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + ts_offset);
		case DataType::INT16:
			return *reinterpret_cast<int16_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::INT32:
			return *reinterpret_cast<int32_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::INT64:
			return *reinterpret_cast<int64_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + ts_offset);
		case DataType::FLOAT32:
			return *reinterpret_cast<float *>(static_cast<char *>(mapped) +
											  header.header_size_in_bytes +
											  row_size * i + ts_offset);
		case DataType::FLOAT64:
			return *reinterpret_cast<double *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + ts_offset);
		case DataType::NONE:
			return i;
		}
	}
	int64_t get_row_weight_integral(uint64_t i)
	{
		switch (header.vertex_id_type) {
		case DataType::UINT8:
			return *reinterpret_cast<uint8_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::UINT16:
			return *reinterpret_cast<uint16_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + weight_offset);
		case DataType::UINT32:
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + weight_offset);
		case DataType::UINT64:
			return *reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + weight_offset);
		case DataType::INT8:
			return *reinterpret_cast<int8_t *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + weight_offset);
		case DataType::INT16:
			return *reinterpret_cast<int16_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::INT32:
			return *reinterpret_cast<int32_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::INT64:
			return *reinterpret_cast<int64_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::FLOAT32:
			return *reinterpret_cast<float *>(static_cast<char *>(mapped) +
											  header.header_size_in_bytes +
											  row_size * i + weight_offset);
		case DataType::FLOAT64:
			return *reinterpret_cast<double *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + weight_offset);
		case DataType::NONE:
			return 1;
		}
	}
	double get_row_weight_floating_point(uint64_t i)
	{
		switch (header.vertex_id_type) {
		case DataType::UINT8:
			return *reinterpret_cast<uint8_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::UINT16:
			return *reinterpret_cast<uint16_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + weight_offset);
		case DataType::UINT32:
			return *reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + weight_offset);
		case DataType::UINT64:
			return *reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) +
												 header.header_size_in_bytes +
												 row_size * i + weight_offset);
		case DataType::INT8:
			return *reinterpret_cast<int8_t *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + weight_offset);
		case DataType::INT16:
			return *reinterpret_cast<int16_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::INT32:
			return *reinterpret_cast<int32_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::INT64:
			return *reinterpret_cast<int64_t *>(static_cast<char *>(mapped) +
												header.header_size_in_bytes +
												row_size * i + weight_offset);
		case DataType::FLOAT32:
			return *reinterpret_cast<float *>(static_cast<char *>(mapped) +
											  header.header_size_in_bytes +
											  row_size * i + weight_offset);
		case DataType::FLOAT64:
			return *reinterpret_cast<double *>(static_cast<char *>(mapped) +
											   header.header_size_in_bytes +
											   row_size * i + weight_offset);
		case DataType::NONE:
			return 1;
		}
	}
	UpdateInfo get_row_info(uint64_t i)
	{
		if (header.if_signal_graph) {
			return UpdateInfo::INSERT;
		} else {
			return *reinterpret_cast<UpdateInfo *>(static_cast<char *>(mapped) +
												   header.header_size_in_bytes +
												   row_size * i + info_offset);
		}
	}
	~InputGraph()
	{
		munmap(mapped, filesize);
	}

	void print_summary() const
	{
		std::cout << "InputGraph summary:\n";
		header.print();
	}
	void print_rows()
	{
		// INFO: output format: src, dest, timestamp, weight, INSERT/DELETE
		for (uint64_t i = 0; i < header.total_updates; i++) {
			std::cout << get_row_src(i) << ", ";
			std::cout << get_row_dest(i) << ", ";
			if (data_type_is_integer(header.timestamp_type)) {
				std::cout << get_row_timestamp_integral(i) << ", ";
			} else {
				std::cout << get_row_timestamp_floating_point(i) << ", ";
			}
			if (data_type_is_integer(header.weight_type)) {
				std::cout << get_row_weight_integral(i) << ", ";
			} else {
				std::cout << get_row_weight_floating_point(i) << ", ";
			}
			std::cout << update_info_to_string(get_row_info(i)) << "\n";
		}
	}
	uint64_t num_vertices() const
	{
		return header.num_vertices;
	}
	uint64_t num_unique_edges() const
	{
		return header.num_unique_edges;
	}
	uint64_t total_updates() const
	{
		return header.total_updates;
	}
	bool if_signal_graph() const
	{
		return header.if_signal_graph;
	}
	DataType vertex_id_type() const
	{
		return header.vertex_id_type;
	}
	DataType timestamp_type() const
	{
		return header.timestamp_type;
	}
	bool if_directed() const
	{
		return header.if_directed;
	}
	DataType weight_type() const
	{
		return header.weight_type;
	}
};

template <typename node_type, typename timestamp_type>
static std::pair<
	parlay::sequence<std::tuple<node_type, node_type, timestamp_type>>,
	node_type>
get_unweighted_graph_edges_signal(const std::string &filename, bool shuffle,
								  int64_t max_edges_to_read)
{
	IO::InputGraph graph(filename);
	uint64_t total_updates = graph.total_updates();
	if (max_edges_to_read > 0 && max_edges_to_read < total_updates) {
		total_updates = max_edges_to_read;
	}
	parlay::sequence<std::tuple<node_type, node_type, timestamp_type>> edges =
		parlay::tabulate(total_updates, [&](size_t i) {
			return std::make_tuple<node_type, node_type, timestamp_type>(
				graph.get_row_src<node_type>(i),
				graph.get_row_dest<node_type>(i),
				graph.get_row_timestamp_integral<timestamp_type>(i));
		});
	parlay::integer_sort_inplace(
		edges, [](auto const &t1) { return std::get<2>(t1); });
	if (shuffle) {
		std::random_device rd;
		std::default_random_engine rng(rd());
		timestamp_type begin = std::get<2>(edges[0]);
		size_t i = 0;
		while (begin == 0 && i < total_updates) {
			begin = std::get<2>(edges[i]);
			i++;
		}
		std::uniform_int_distribution<timestamp_type> dist(
			std::get<2>(edges[1]), std::get<2>(edges.back()));
		for (uint64_t i = 0; i < total_updates; i++) {
			std::get<2>(edges[i]) = dist(rng);
		}
		parlay::integer_sort_inplace(
			edges, [](auto const &t1) { return std::get<2>(t1); });
	}
	return std::make_pair(edges, graph.num_vertices());
}

template <typename node_type, typename timestamp_type>
static parlay::sequence<
	std::tuple<node_type, node_type, UpdateInfo, timestamp_type>>
turn_signal_into_windowed(
	parlay::sequence<std::tuple<node_type, node_type, timestamp_type>>
		&signal_edges,
	uint64_t window_size)
{
	timestamp_type end_time = std::get<2>(signal_edges.back());
	auto pairs = parlay::delayed_map(
		signal_edges,
		[](std::tuple<node_type, node_type, timestamp_type> &edges) {
			return std::make_pair(
				std::make_pair(std::get<0>(edges), std::get<1>(edges)),
				std::get<2>(edges));
		});
	auto groups = parlay::group_by_key(pairs);
	auto output_2d =
		parlay::map(groups, [window_size, end_time](auto &key_sequence_pair) {
			auto &[key, sequence] = key_sequence_pair;
			auto [src, dest] = key;
			parlay::integer_sort_inplace(sequence);
			parlay::sequence<
				std::tuple<node_type, node_type, UpdateInfo, timestamp_type>>
				updates_for_edge;
			updates_for_edge.emplace_back(src, dest, UpdateInfo::INSERT,
										  sequence[0]);
			timestamp_type last_insert = sequence[0];
			for (size_t i = 1; i < sequence.size(); i++) {
				if (last_insert + window_size >= sequence[i]) {
					last_insert = sequence[i];
				} else {
					updates_for_edge.emplace_back(src, dest, UpdateInfo::DELETE,
												  last_insert + window_size);
					updates_for_edge.emplace_back(src, dest, UpdateInfo::INSERT,
												  sequence[i]);
					last_insert = sequence[i];
				}
			}
			if (last_insert + window_size < end_time)
				updates_for_edge.emplace_back(src, dest, UpdateInfo::DELETE,
											  last_insert + window_size);
			return updates_for_edge;
		});
	return parlay::integer_sort(parlay::flatten(output_2d),
								[](auto const &t1) { return std::get<3>(t1); });
}

template <typename node_type, typename timestamp_type>
static std::pair<parlay::sequence<std::tuple<node_type, node_type, UpdateInfo,
											 timestamp_type>>,
				 node_type>
get_unweighted_graph_edges(const std::string &filename, bool shuffle,
						   int64_t max_edges_to_read)
{
	IO::InputGraph graph(filename);
	uint64_t total_updates = graph.total_updates();
	if (max_edges_to_read > 0 && max_edges_to_read < total_updates) {
		total_updates = max_edges_to_read;
	}

	auto edges = parlay::tabulate(
		total_updates,
		[&](uint64_t i)
			-> std::tuple<node_type, node_type, UpdateInfo, timestamp_type> {
			if (graph.get_row_src<node_type>(i) <
				graph.get_row_dest<node_type>(i)) {
				return { graph.get_row_src<node_type>(i),
						 graph.get_row_dest<node_type>(i),
						 graph.get_row_info(i),
						 graph.get_row_timestamp_integral<timestamp_type>(i) };
			} else {
				return { graph.get_row_dest<node_type>(i),
						 graph.get_row_src<node_type>(i), graph.get_row_info(i),
						 graph.get_row_timestamp_integral<timestamp_type>(i) };
			}
		});

	edges = parlay::integer_sort(
		edges, [](auto const &t1) { return std::get<3>(t1); });

	parlay::random_generator gen;
	std::uniform_int_distribution<timestamp_type> dis(
		std::get<3>(edges[1]), std::get<3>(edges.back()));

	auto pairs = parlay::delayed_map(
		edges, [](std::tuple<node_type, node_type, UpdateInfo, timestamp_type>
					  &edges) {
			return std::make_pair(
				std::make_pair(std::get<0>(edges), std::get<1>(edges)),
				std::make_pair(std::get<3>(edges), std::get<2>(edges)));
		});
	auto groups = parlay::group_by_key(pairs);
	auto output_2d = parlay::tabulate(groups.size(), [&](size_t i) {
		auto r = gen[i];
		auto &[key, sequence] = groups[i];
		auto [src, dest] = key;
		parlay::integer_sort_inplace(
			sequence, [](auto const &a) { return std::get<0>(a); });
		parlay::sequence<
			std::tuple<node_type, node_type, UpdateInfo, timestamp_type>>
			updates_for_edge;
		UpdateInfo last_operation = UpdateInfo::DELETE;
		for (auto &[ts, op] : sequence) {
			if (op != last_operation) {
				updates_for_edge.emplace_back(src, dest, op, ts);
				last_operation = op;
			}
		}
		if (shuffle) {
			for (auto &[s, d, op, ts] : updates_for_edge) {
				ts = dis(r);
			}
			parlay::integer_sort_inplace(
				updates_for_edge, [](auto const &a) { return std::get<3>(a); });
			auto next_op = UpdateInfo::INSERT;
			for (auto &[s, d, op, ts] : updates_for_edge) {
				op = next_op;
				if (op == UpdateInfo::INSERT) {
					next_op = UpdateInfo::DELETE;
				} else {
					next_op = UpdateInfo::INSERT;
				}
			}
		}
		return updates_for_edge;
	});

	edges =
		parlay::integer_sort(parlay::flatten(output_2d),
							 [](auto const &t1) { return std::get<3>(t1); });
	return { edges, graph.num_vertices() };
}

} // namespace IO
