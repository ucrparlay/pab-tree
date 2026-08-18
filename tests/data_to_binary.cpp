/*
 * Convert a text ".in" dataset (produced by data_generate.cpp) into a raw
 * binary ".bin" file for fast loading.
 *
 * Text format:   "<N> <dim>\n" header, then N lines of dim decimal integers
 *                (key, then dim-1 value columns).
 * Binary format: see binary_io.h — header [N, dim, width] followed by N*dim
 *                values, narrowed to 32-bit when the data range fits.
 *
 * Usage: data_to_binary -p <input.in> [-o <output.bin>]
 *        -o defaults to the input path with ".in" replaced by ".bin".
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "binary_io.h"
#include "common/IO.h"
#include "common/parse_command_line.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"

/*
 * Sample usage:
 *   ./data_to_binary -p /data/zmen002/kv_pair/uni/100000000_1/del/10/1.base.in
 *     -> writes /data/zmen002/kv_pair/uni/100000000_1/del/10/1.base.bin
 *   ./data_to_binary -p /data/zmen002/kv_pair/uni/100000000_1/del/10/1.batch.in
 *   \
 *                    -o /tmp/1.batch.bin
 */
int main(int argc, char *argv[])
{
	commandLine cmd(argc, argv, "[-p <inFile>] [-o <outFile>]");
	std::string in_path = cmd.getOptionValue("-p", "");
	std::string out_path = cmd.getOptionValue("-o", "");

	if (in_path.empty()) {
		std::cerr << "Missing input file (-p)." << std::endl;
		return 1;
	}
	if (out_path.empty()) {
		out_path = in_path;
		size_t pos = out_path.rfind(".in");
		if (pos == out_path.size() - 3) {
			out_path.replace(pos, 3, ".bin");
		} else {
			out_path += ".bin";
		}
	}

	std::cout << "Reading " << in_path << std::endl;
	parlay::sequence<char> chars = benchIO::readStringFromFile(in_path.c_str());
	parlay::sequence<char *> words = benchIO::stringToWords(chars);

	uint64_t const n = std::stoull(words[0]);
	uint64_t const dim = std::stoull(words[1]);
	if (words.size() - 2 != n * dim) {
		std::cerr << "Token count mismatch: header says " << n << " x " << dim
				  << " = " << n * dim << ", file has " << words.size() - 2
				  << " tokens." << std::endl;
		return 1;
	}

	std::cout << "Parsing " << n << " entries with dim " << dim << std::endl;
	auto payload = parlay::tabulate(n * dim, [&](size_t i) -> uint64_t {
		return std::strtoull(words[2 + i], nullptr, 10);
	});

	std::cout << "Writing " << out_path << std::endl;
	write_binary_entries(out_path, n, dim, payload);

	std::cout << "Done." << std::endl;
	return 0;
}
