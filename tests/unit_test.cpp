#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "parlay/primitives.h"
#include "test_framework.h"

namespace fs = std::filesystem;

template <typename TT>
struct btree_wrapper {
	using tree_type = typename pab_tree::btree<TT>;
};

template <typename Options, typename RunFunc>
static void check_orthogonal(Options const &opts, RunFunc test_func)
{
	/* using key_type = std::pair<uint32_t, uint32_t>; */
	using wrapper = wrapper<uint32_t, uint32_t>;
	using key_type [[maybe_unused]] = wrapper::key_type;
	using mapped_type [[maybe_unused]] = wrapper::mapped_type;
	using entry_type = wrapper::entry_type;
	/* using EleType = Element<uint32_t, uint32_t>; */
#ifdef LEAF1
	static constexpr uint const k_buf = 1;
#elif defined(LEAF2)
	static constexpr uint const k_buf = 2;
#elif defined(LEAF4)
	static constexpr uint const k_buf = 4;
#elif defined(LEAF8)
	static constexpr uint const k_buf = 8;
#elif defined(LEAF16)
	static constexpr uint const k_buf = 16;
#elif defined(LEAF32)
	static constexpr uint const k_buf = 32;
#elif defined(LEAF64)
	static constexpr uint const k_buf = 64;
#elif defined(LEAF128)
	static constexpr uint const k_buf = 128;
#elif defined(LEAF256)
	static constexpr uint const k_buf = 256;
#elif defined(LEAF512)
	static constexpr uint const k_buf = 512;
#elif defined(LEAF1024)
	static constexpr uint const k_buf = 1024;
#else
	static constexpr uint const k_buf = 32; /* Default */
#endif /* LEAF1 */

	using TT = pab_tree::btree_type_trait<
		wrapper::element_wrapper<entry_type>, k_buf, wrapper::leaf_aug_empty,
		wrapper::interior_aug_empty, pab_tree::proto_node>;
	if (opts.tree_type == 0) {
		Run<btree_wrapper<TT>>(opts, test_func);
	}
}

class ParTreeCorrectTest : public ::testing::Test {
public:
	static std::string file_filter;
	static bool redirect_output;

protected:
#ifndef MAC
	static constexpr char const *k_data_prefix = "/ssd1/zmen002/kv_pair";
#else
	static constexpr char const *k_data_prefix =
		"/Users/ziyang/code/ParallelMultiwayOrderedSet/benchmarks";
#endif /* !  #ifndef MAC */
	static constexpr int k_tag = 0b0011; /* 2#0001 */
	/* static constexpr int k_tag = 0b0000; // 2#0001 */
	static constexpr double k_batch_ratio = 0.01;
	static constexpr int k_query_type = 0b011; /* 2#001 */
	static constexpr int k_range_query_type = 3;
	/* static constexpr int k_range_query_type = 2; */
	static constexpr int k_range_query_num = 1000;
	static constexpr int k_tree_type = 0;
	static constexpr int k_dim = 1;
	static constexpr int k_rounds = 2;
	static constexpr int k_summary = 0;
	static constexpr double k_membership_query_ratio = 0.1;

	std::vector<size_t> nodes{ 1000000, 5000000, 10000000, 50000000 };
	std::vector<std::string> distributions{ "uni", "exp", "norm", "dual_norm" };
	std::vector<std::string> batch_ops{ "ins" };

	template <typename TT, typename TreeDesc>
	static void check_correctness(Options opts)
	{
		using Tree = TreeDesc::tree_type;
		using entry_seq = typename TT::entry_seq;

		Tree tree;
		constexpr bool k_test_time = false;
		entry_seq wp;
		entry_seq wi;
		auto [base_path, batch_path] =
			opts.parse_insert_batch_path(opts.batch_ratio);
		[[maybe_unused]] auto [n, d] =
			read_entries<typename TT::entry_type>(base_path.c_str(), wp);
		[[maybe_unused]] auto [n2, d2] =
			read_entries<typename TT::entry_type>(batch_path.c_str(), wi);
		build_tree<TT, Tree, k_test_time, 2>(wp, opts.rounds, tree);
		if (redirect_output) {
			ASSERT_TRUE((tree.template validate<true, false>()));
		} else {
			ASSERT_TRUE(tree.validate());
		}

		{
			entry_seq tmp(wp.size());
			tree.get_entries(tmp);
			assert(parlay::sort(tmp) == parlay::sort(wp));
		}

		if (opts.tag & (1 << 0)) {
			batch_insert<TT, Tree, k_test_time>(tree, wp, wi, 2);
			ASSERT_EQ(tree.get_size(), wp.size() + wi.size());
			if (redirect_output) {
				ASSERT_TRUE((tree.template validate<true, false>()));
			} else {
				ASSERT_TRUE(tree.validate());
			}

			{
				entry_seq tmp(wp.size() + wi.size());
				tree.get_entries(tmp);
				auto nwp = parlay::sort(parlay::append(wp, wi));
				tmp = parlay::sort(tmp);
				assert(tmp == nwp);
			}
		}

		if (opts.tag & (1 << 1)) {
			/* WARN: must happen after the insertion */
			batch_delete<TT, Tree, k_test_time>(tree, wp, wi, 2);
			assert(tree.get_size() == wp.size());
			if (redirect_output) {
				ASSERT_TRUE((tree.template validate<true, false>()));
			} else {
				ASSERT_TRUE(tree.validate());
			}

			entry_seq tmp(wp.size());
			tree.get_entries(tmp);
			assert(parlay::sort(tmp) == parlay::sort(wp));
		}

		entry_seq new_wp;
		if (opts.tag == (1 << 0)) {
			new_wp = wp;
			new_wp.append(wi);
		} else {
			new_wp = wp;
		}

		if (opts.query_type & (1 << 0)) {
			/* this part needs to correct for all parts */
			auto batch_size =
				static_cast<size_t>(wp.size() * k_membership_query_ratio);
			auto query_ele =
				parlay::random_shuffle(new_wp).subseq(0, batch_size);
			auto res = parlay::sequence<bool>(query_ele.size(), false);
			membership<TT>(tree, query_ele, opts.rounds, res);
			assert(parlay::all_of(res, [&](auto r) { return r == true; }));
			if (opts.tag & (1 << 1)) { /* specifically for delete */
				query_ele = wi;
				auto res = parlay::sequence<bool>(query_ele.size(), true);
				membership<TT>(tree, query_ele, opts.rounds, res);
				assert(parlay::all_of(res, [&](auto r) { return r == false; }));
			}
		}

		if (opts.query_type & (1 << 1)) {
			for (int rec_type = 0; rec_type < k_range_query_type; rec_type++) {
				range_query<TT>(new_wp, tree, k_rounds, k_range_query_num,
								rec_type);
			}
		}

		if (opts.query_type & (1 << 2)) {
			for (int rec_type = 0; rec_type < k_range_query_type; rec_type++) {
				range_count<TT>(new_wp, tree, k_rounds, k_range_query_num,
								rec_type);
			}
		}

		tree.delete_tree();
	}

	static auto find_base_files(std::string const &files_path)
		-> std::vector<std::string>
	{
		std::vector<std::string> base_files;
		if (!fs::exists(files_path)) {
			return base_files;
		}

		for (auto const &entry : fs::directory_iterator(files_path)) {
			if (entry.is_regular_file()) {
				std::string filename = entry.path().filename().string();
				if (filename.find(".base.bin") != std::string::npos) {
					base_files.push_back(entry.path().string());
				}
			}
		}
		return base_files;
	}

	static void run_test(std::string const &file_path,
						 std::string const &distribution)
	{
		/*
		 * These must outlive argv: std::to_string returns a temporary, so calling
		 * .c_str() on it inline leaves every pointer dangling.
		 */
		std::string const s_summary = std::to_string(k_summary);
		std::string const s_dim = std::to_string(k_dim);
		std::string const s_tag = std::to_string(k_tag);
		std::string const s_rounds = std::to_string(k_rounds);
		std::string const s_tree_type = std::to_string(k_tree_type);
		std::string const s_query_type = std::to_string(k_query_type);
		std::string const s_batch_ratio = std::to_string(k_batch_ratio);

		std::vector<char const *> argv = {
			"unit_test",		   "-p", file_path.c_str(),	   "-s",
			s_summary.c_str(),	   "-d", s_dim.c_str(),		   "-t",
			s_tag.c_str(),		   "-r", s_rounds.c_str(),	   "-T",
			s_tree_type.c_str(),   "-q", s_query_type.c_str(), "-b",
			s_batch_ratio.c_str(), "-D", distribution.c_str()
		};

		/* Print the command being executed */
		std::cout << "Running: unit_test";
		for (size_t i = 1; i < argv.size(); i++) {
			std::cout << " " << argv[i];
		}
		std::cout << std::flush << "\n";

		std::vector<char *> argv_mutable;
		for (char const *arg : argv) {
			argv_mutable.push_back(const_cast<char *>(arg));
		}

		commandLine params(argv_mutable.size(), argv_mutable.data());
		Options opts(params);

		/*
		 * Redirect stdout to suppress output from test execution (if enabled)
		 */
		std::streambuf *old_cout = nullptr;
		std::ostringstream null_stream;
		if (redirect_output) {
			old_cout = std::cout.rdbuf();
			std::cout.rdbuf(null_stream.rdbuf());
		}

		auto run =
			[&]<typename TreeWrapper, typename TT>(Options opts) -> void {
			check_correctness<TT, TreeWrapper>(opts);
		};

		check_orthogonal(opts, run);

		/* Restore stdout (if it was redirected) */
		if (old_cout) {
			std::cout.rdbuf(old_cout);
		}
	}

	static void btree_test(std::string const &distribution, size_t node_size)
	{
		std::string files_path =
			std::string(k_data_prefix) + "/" + distribution + "/" +
			std::to_string(node_size) + "_" + std::to_string(k_dim) + "/ins/" +
			std::to_string(static_cast<int>(1 / k_batch_ratio));
		auto base_files = find_base_files(files_path);

		if (base_files.empty()) {
			GTEST_SKIP() << "Data files not found at: " << files_path;
		}

		/* Filter files if file_filter is specified */
		if (!file_filter.empty()) {
			std::vector<std::string> filtered_files;
			for (auto const &file : base_files) {
				if (file.find(file_filter) != std::string::npos) {
					filtered_files.push_back(file);
				}
			}
			if (filtered_files.empty()) {
				GTEST_SKIP() << "No files match filter: " << file_filter;
			}
			base_files = filtered_files;
		}

		for (auto const &file : base_files) {
			SCOPED_TRACE("Testing file: " + file);
			EXPECT_NO_THROW(run_test(file, distribution));
		}
	}
};

TEST_F(ParTreeCorrectTest, Uni1M)
{
	btree_test("uni", 1000000);
}

TEST_F(ParTreeCorrectTest, Uni5M)
{
	btree_test("uni", 5000000);
}

TEST_F(ParTreeCorrectTest, Uni10M)
{
	btree_test("uni", 10000000);
}

TEST_F(ParTreeCorrectTest, Uni50M)
{
	btree_test("uni", 50000000);
}

TEST_F(ParTreeCorrectTest, Exp1M)
{
	btree_test("exp", 1000000);
}

TEST_F(ParTreeCorrectTest, Exp5M)
{
	btree_test("exp", 5000000);
}

TEST_F(ParTreeCorrectTest, Exp10M)
{
	btree_test("exp", 10000000);
}

TEST_F(ParTreeCorrectTest, Exp50M)
{
	btree_test("exp", 50000000);
}

TEST_F(ParTreeCorrectTest, Norm1M)
{
	btree_test("norm", 1000000);
}

TEST_F(ParTreeCorrectTest, Norm5M)
{
	btree_test("norm", 5000000);
}

TEST_F(ParTreeCorrectTest, Norm10M)
{
	btree_test("norm", 10000000);
}

TEST_F(ParTreeCorrectTest, Norm50M)
{
	btree_test("norm", 50000000);
}

TEST_F(ParTreeCorrectTest, DualNorm1M)
{
	btree_test("dual_norm", 1000000);
}

TEST_F(ParTreeCorrectTest, DualNorm5M)
{
	btree_test("dual_norm", 5000000);
}

TEST_F(ParTreeCorrectTest, DualNorm10M)
{
	btree_test("dual_norm", 10000000);
}

TEST_F(ParTreeCorrectTest, DualNorm50M)
{
	btree_test("dual_norm", 50000000);
}

/* Define the static members */
std::string ParTreeCorrectTest::file_filter = "";
bool ParTreeCorrectTest::redirect_output = true;

/*
 * ./unit_test --gtest_filter=ParTreeCorrectTest.Uni1M
 * ./unit_test --gtest_filter=ParTreeCorrectTest.Uni1M --file_filter=1.base.bin
 * ./unit_test --gtest_filter=ParTreeCorrectTest.Uni1M --no_redirect
 */
int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);

	/* Parse custom arguments */
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg.starts_with("--file_filter=")) {
			ParTreeCorrectTest::file_filter =
				arg.substr(14); /* Length of "--file_filter=" */
			std::cout << "Filtering files containing: "
					  << ParTreeCorrectTest::file_filter << std::endl;
		} else if (arg == "--no_redirect") {
			ParTreeCorrectTest::redirect_output = false;
			std::cout << "Output redirection disabled" << std::endl;
		}
	}

	return RUN_ALL_TESTS();
}
