#include <vector>

#include "data_op.h"

parlay::sequence<double> ParseParam(std::string const &input,
									double defaultValue = 1.0)
{
	if (input.empty()) {
		return {};
	}
	parlay::sequence<double> result;
	size_t start = 0;
	size_t end = input.find(',');

	while (end != std::string::npos) {
		std::string token = input.substr(start, end - start);
		try {
			result.push_back(std::stod(token));
		} catch (...) {
			std::cerr << "Error parsing value: " << token << std::endl;
			return {};
		}
		start = end + 1;
		end = input.find(',', start);
	}

	// Handle the last token
	std::string token = input.substr(start);
	try {
		result.push_back(std::stod(token));
	} catch (...) {
		std::cerr << "Error parsing value: " << token << std::endl;
		return {};
	}

	return result.empty() ? parlay::sequence<double>{} : result;
}

int main(int argc, char *argv[])
{
	commandLine P(argc, argv,
				  "[-k {1,...,100}] [-d {2,3,5,7,9,10}] [-n <node num>] [-t "
				  "<parallelTag>] [-p <inFile>] [-r {1,...,5}] [-q {0,1}] [-i "
				  "<_insertFile>] [-s <kSummary>]");
	std::string path;
	int base_n, batch_n, val_n, file_n, dis_type, batch_op;
	std::string base_param, batch_param;

	path = P.getOptionValue("-path");
	base_n = P.getOptionIntValue("-base_n", 1'000'000);
	batch_n = P.getOptionIntValue("-batch_n", 100'000);
	val_n = P.getOptionIntValue("-val_n", 1);
	file_n = P.getOptionIntValue("-file_num", 2);
	// PARA: 0: unifrom, 1: exp, 2: norm, 3: dual norm
	dis_type = P.getOptionIntValue("-dis", 0);
	// PARA: 0: insert, 1: delete
	batch_op = P.getOptionIntValue("-batch_op", 0);
	// PARA: 0: text .in, 1: binary .bin
	int const binary = P.getOptionIntValue("-binary", 0);
	// PARA: uni: none
	// PARA: exp: 10
	// PARA: norm: 0.5, 0.2
	batch_param = P.getOptionValue("-batch_param");
	base_param = P.getOptionValue("-base_param");

	parlay::sequence<double> batch_param_seq = ParseParam(batch_param, 1.0);
	parlay::sequence<double> base_param_seq = ParseParam(base_param, 1.0);

	std::cout << "base_n: " << base_n << " batch_n: " << batch_n
			  << " pts_dim: " << val_n << " file_num: " << file_n
			  << " distribution_type: " << dis_type << " batch_param: ";
	for (auto const &p : batch_param_seq)
		std::cout << p << " ";
	std::cout << " base_param: ";
	for (auto const &p : base_param_seq)
		std::cout << p << " ";
	std::cout << std::endl;

	// NOTE: ../kdtree/
	path += std::string(*path.rbegin() == '/' ? "" : "/");
	std::filesystem::create_directory(path);

	// NOTE: ../kdtree/uni/
	path += std::string(dis_type == 0 ? "uni/" :
						dis_type == 1 ? "exp/" :
						dis_type == 2 ? "norm/" :
										"dual_norm/");
	std::filesystem::create_directory(path);

	// NOTE: ../kdtree/uni/1000000_1/
	path += toString(base_n) + "_" + toString(val_n) + "/";
	std::filesystem::create_directory(path);

	// NOTE: ../kdtree/uni/1000000_1/ins/
	path += std::string(batch_op == 0 ? "ins/" : "del/");
	std::filesystem::create_directory(path);

	// NOTE: ../kdtree/uni/1000000_1/ins/10/
	path += toString(base_n / batch_n) + "/";
	std::filesystem::create_directory(path);

	auto generate = [&]<typename E>(std::string const &new_path) {
		using entry_seq = parlay::sequence<E>;
		using K = typename E::K;
		std::cout << "Generating... " << std::endl;
		entry_seq base_seq, batch_seq;
		std::tie(base_seq, batch_seq) =
			dis_type == 0 ?
				Generator<E, UniformGenerator<>, UniformGenerator<>>::GenBatch(
					batch_op, base_n, batch_n, base_param_seq,
					batch_param_seq) :
			dis_type == 1 ?
				Generator<E, UniformGenerator<>,
						  ExponentialGenerator<>>::GenBatch(batch_op, base_n,
															batch_n,
															base_param_seq,
															batch_param_seq) :
			dis_type == 2 ?
				Generator<E, UniformGenerator<>, NormalGenerator<>>::GenBatch(
					batch_op, base_n, batch_n, base_param_seq,
					batch_param_seq) :
				Generator<E, NormalGenerator<>, NormalGenerator<>>::GenBatch(
					batch_op, base_n, batch_n, base_param_seq, batch_param_seq);
		std::cout << "Writing... " << std::endl;
		if (binary) {
			PrintToBinaryFile(new_path + ".base.bin", base_seq);
			PrintToBinaryFile(new_path + ".batch.bin", batch_seq);
		} else {
			PrintToFile(new_path + ".base.in", base_seq);
			PrintToFile(new_path + ".batch.in", batch_seq);
		}
	};

	for (int i = 0; i < file_n; i++) {
		std::string newpath = path + toString(i + 1);
		std::cout << newpath << std::endl;
		rand_seed = i;
		if (val_n == 1) {
			generate.operator()<Wrapper<Axis, std::tuple<Axis>>::Element<
				Axis, std::tuple<Axis>>>(newpath);
		}
		// else if (pts_dim == 3) {
		//   generate.operator()<psi::BasicPoint<Axis, 3>>(newpath);
		// } else if (pts_dim == 5) {
		//   generate.operator()<psi::BasicPoint<Axis, 5>>(newpath);
		// } else if (pts_dim == 7) {
		//   generate.operator()<psi::BasicPoint<Axis, 7>>(newpath);
		// } else if (pts_dim == 9) {
		//   generate.operator()<psi::BasicPoint<Axis, 9>>(newpath);
		// } else if (pts_dim == 12) {
		//   generate.operator()<psi::BasicPoint<Axis, 12>>(newpath);
		// } else if (pts_dim == 16) {
		//   generate.operator()<psi::BasicPoint<Axis, 16>>(newpath);
		// } else {
		//   throw std::runtime_error("Invalid dimension");
		// }
	}
	return 0;
}
