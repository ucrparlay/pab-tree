#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../external/fast-cpp-csv-parser/csv.h"
#include "binary_io.h"
#include "data_op.h"
#include "libmorton/morton.h"
#include "libmorton/morton3D.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"

namespace
{
struct Record {
	using CodeType = uint64_t;

	bool operator<(const Record &a) const
	{
		return code < a.code;
	}
	bool operator==(const Record &a) const
	{
		return code == a.code;
	}

	CodeType code;
	uint32_t time;
};
}

int main(int argc, char **argv)
{
	if (argc < 3 || argc > 6) {
		std::cerr
			<< "Usage: " << argv[0]
			<< " <input_csv> <output_csv> [start_year=2014] [end_year=2024] "
			   "[year_column=ts]\n"
			<< "Example:\n  " << argv[0]
			<< " /data/zmen002/geometry/osm_by_year/africa-latest.osm.pbf.csv "
			   "/tmp/africa-2014-2024.csv\n"
			<< "If <output_csv> ends with \".bin\", the output is written in "
			   "binary form (uint64 stream [N, 2, code, time, ...]).\n";
		return EXIT_FAILURE;
	}

	const std::string input_path = argv[1];
	const std::string output_path = argv[2];
	const int start_year = 2014;
	const int end_year = 2024;

	std::ifstream input(input_path);
	if (!input) {
		std::cerr << "Failed to open input CSV: " << input_path << "\n";
		return EXIT_FAILURE;
	}

	io::CSVReader<4> in(input_path);
	in.read_header(io::ignore_extra_column, "id", "ts", "lon", "lat");
	uint64_t id = 0;
	std::string ts;
	double lon = 0.0;
	double lat = 0.0;
	parlay::sequence<Record> record_seq;
	size_t read_rows = 0;
	size_t kept_rows = 0;
	while (in.read_row(id, ts, lon, lat)) {
		read_rows++;
		if (ts.substr(0, 4) < std::to_string(start_year) ||
		    ts.substr(0, 4) > std::to_string(end_year)) {
			continue;
		}
		auto lon_int = static_cast<uint32_t>(lon * 1e7);
		auto lat_int = static_cast<uint32_t>(lat * 1e7);
		record_seq.emplace_back(Record{
			.code = libmorton::morton2D_64_encode(lon_int, lat_int),
			.time = static_cast<uint32_t>(
				std::stoul(ts.substr(0, 4))) });
		kept_rows++;
	}

	std::cout << "Finished reading CSV. Total rows: " << read_rows
		  << ", Kept rows: " << kept_rows << "\n";

	// NOTE: remove duplicates
	record_seq = parlay::sort(record_seq.cut(0, kept_rows));
	record_seq = parlay::remove_duplicates_ordered(record_seq);
	// check
	auto z_code_seq = parlay::integer_sort(
		parlay::tabulate(record_seq.size(),
				 [&](size_t i) { return record_seq[i].code; }));
	parlay::parallel_for(1, z_code_seq.size(), [&](size_t i) {
		if (z_code_seq[i] == z_code_seq[i - 1]) {
			std::cerr << "Duplicate code found: " << z_code_seq[i]
				  << "\n";
		}
	});
	// resort by time
	record_seq =
		parlay::sort(record_seq, [](const Record &a, const Record &b) {
			return a.time == b.time ? a.code < b.code :
						  a.time < b.time;
		});
	std::cout << "Finished removing duplicates. Unique rows: "
		  << record_seq.size() << "\n";

	// NOTE: write to file; ".bin" output suffix selects the binary form
	if (std::string_view(output_path).ends_with(".bin")) {
		parlay::sequence<uint64_t> payload(record_seq.size() * 2);
		parlay::parallel_for(0, record_seq.size(), [&](size_t i) {
			payload[i * 2] = record_seq[i].code;
			payload[(i * 2) + 1] = record_seq[i].time;
		});
		WriteBinaryEntries(output_path, record_seq.size(), 2, payload);
	} else {
		auto header = toString(record_seq.size()) + " " + toString(2);
		auto record_str =
			parlay::tabulate(record_seq.size(), [&](size_t i) {
				std::string str;
				str += toString(record_seq[i].code) + " ";
				str += toString(record_seq[i].time);
				parlay::sequence<char> cstr(str.begin(),
							    str.end());
				return cstr;
			});
		writeSeqToFile(header, record_str, output_path.c_str());
	}

	std::cout << "Input: " << input_path << "\n"
		  << "Output: " << output_path << "\n"
		  << "Year range: [" << start_year << ", " << end_year << "]\n"
		  << "Rows read: " << read_rows << "\n"
		  << "Rows kept: " << record_seq.size() << "\n";

	return EXIT_SUCCESS;
}
