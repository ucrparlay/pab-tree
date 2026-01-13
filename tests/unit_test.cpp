#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "test_framework.h"

namespace fs = std::filesystem;

class ParTreeCorrectTest : public ::testing::Test
{
public:
  static std::string file_filter;

protected:
  static constexpr char const* kDataPrefix = "/ssd1/zmen002/kv_pair";
  static constexpr int kTag = 0b0001; // 2#0001
  static constexpr double kBatchRatio = 0.01;
  static constexpr int kQueryType = 0b001; // 2#001
  static constexpr int kTreeType = 0;
  static constexpr int kDim = 1;
  static constexpr int kRounds = 2;
  static constexpr int kSummary = 0;
  static constexpr double kBatchQueryRatio = 0.1;

  std::vector<size_t> nodes{ 1000000, 5000000, 10000000, 50000000 };
  std::vector<std::string> distributions{ "uni", "exp", "norm", "dual_norm" };
  std::vector<std::string> batch_ops{ "ins" };

  template<typename TT, typename TreeDesc>
  void CheckCorrectness(Wrapper::Options opts)
  {
    using Tree = TreeDesc::TreeType;
    using EntrySeq = typename TT::EntrySeq;

    Tree tree;
    constexpr bool kTestTime = false;
    EntrySeq wp, wi;

    if (opts.tag & (1 << 0)) {
      auto [base_path, batch_path] =
        opts.ParseInsertBatchPath(opts.batch_ratio);
      auto [n, d] = read_points<typename TT::E>(base_path.c_str(), wp, 0);
      auto [n2, d2] = read_points<typename TT::E>(batch_path.c_str(), wi, n);

      BuildTree<TT, Tree, kTestTime, 2>(wp, opts.rounds, tree);
      tree.template Validate<typename Tree::Leaf, typename Tree::Interior>();

      BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, 2);
      ASSERT_EQ(tree.GetSize(), wp.size() + wi.size());
      tree.template Validate<typename Tree::Leaf, typename Tree::Interior>();
    }

    EntrySeq new_wp;
    if (opts.tag == (1 << 0)) {
      new_wp = wp;
      new_wp.append(wi);
    } else {
      new_wp = wp;
    }

    if (opts.query_type & (1 << 0)) {
      size_t batch_size = static_cast<size_t>(wp.size() * kBatchQueryRatio);
      auto query_ele = parlay::random_shuffle(new_wp).subseq(0, batch_size);
      auto res = parlay::sequence<bool>(query_ele.size(), false);
      Membership<TT>(tree, query_ele, opts.rounds, res);
      ASSERT_TRUE(parlay::all_of(res, [&](auto r) { return r == true; }));
    }

    tree.DeleteTree();
  }

  std::vector<std::string> FindBaseFiles(std::string const& files_path)
  {
    std::vector<std::string> base_files;
    if (!fs::exists(files_path)) {
      return base_files;
    }

    for (auto const& entry : fs::directory_iterator(files_path)) {
      if (entry.is_regular_file()) {
        std::string filename = entry.path().filename().string();
        if (filename.find(".base.in") != std::string::npos) {
          base_files.push_back(entry.path().string());
        }
      }
    }
    return base_files;
  }

  void RunTest(std::string const& file_path)
  {
    std::vector<char const*> argv = { "unit_test",
                                      "-p",
                                      file_path.c_str(),
                                      "-s",
                                      std::to_string(kSummary).c_str(),
                                      "-d",
                                      std::to_string(kDim).c_str(),
                                      "-t",
                                      std::to_string(kTag).c_str(),
                                      "-r",
                                      std::to_string(kRounds).c_str(),
                                      "-T",
                                      std::to_string(kTreeType).c_str(),
                                      "-q",
                                      std::to_string(kQueryType).c_str(),
                                      "-b",
                                      std::to_string(kBatchRatio).c_str() };

    // Print the command being executed
    std::cout << "Running: b_ccp";
    for (size_t i = 1; i < argv.size(); i++) {
      std::cout << " " << argv[i];
    }
    std::cout << std::flush << std::endl;

    std::vector<char*> argv_mutable;
    for (char const* arg : argv) {
      argv_mutable.push_back(const_cast<char*>(arg));
    }

    commandLine params(argv_mutable.size(), argv_mutable.data());
    Wrapper::Options opts(params);

    // Redirect stdout to suppress output from test execution
    std::streambuf* old_cout = std::cout.rdbuf();
    std::ostringstream null_stream;
    std::cout.rdbuf(null_stream.rdbuf());

    auto run = [&]<typename TreeWrapper, typename TypeTrait>(
                 Wrapper::Options opts) -> void {
      CheckCorrectness<TypeTrait, TreeWrapper>(opts);
    };

    Wrapper::ApplyOrthogonal(opts, run);

    // Restore stdout
    std::cout.rdbuf(old_cout);
  }

  void BTreeTest(std::string const& distribution, size_t node_size)
  {
    std::string files_path =
      std::string(kDataPrefix) + "/" + distribution + "/" +
      std::to_string(node_size) + "_" + std::to_string(kDim) + "/ins/" +
      std::to_string(static_cast<int>(kBatchRatio * 100));
    auto base_files = FindBaseFiles(files_path);

    if (base_files.empty()) {
      GTEST_SKIP() << "Data files not found at: " << files_path;
    }

    // Filter files if file_filter is specified
    if (!file_filter.empty()) {
      std::vector<std::string> filtered_files;
      for (auto const& file : base_files) {
        if (file.find(file_filter) != std::string::npos) {
          filtered_files.push_back(file);
        }
      }
      if (filtered_files.empty()) {
        GTEST_SKIP() << "No files match filter: " << file_filter;
      }
      base_files = filtered_files;
    }

    for (auto const& file : base_files) {
      SCOPED_TRACE("Testing file: " + file);
      EXPECT_NO_THROW(RunTest(file));
    }
  }
};

TEST_F(ParTreeCorrectTest, Uni1M)
{
  BTreeTest("uni", 1000000);
}

TEST_F(ParTreeCorrectTest, Uni5M)
{
  BTreeTest("uni", 5000000);
}

TEST_F(ParTreeCorrectTest, Uni10M)
{
  BTreeTest("uni", 10000000);
}

TEST_F(ParTreeCorrectTest, Uni50M)
{
  BTreeTest("uni", 50000000);
}

TEST_F(ParTreeCorrectTest, Exp1M)
{
  BTreeTest("exp", 1000000);
}

TEST_F(ParTreeCorrectTest, Exp5M)
{
  BTreeTest("exp", 5000000);
}

TEST_F(ParTreeCorrectTest, Exp10M)
{
  BTreeTest("exp", 10000000);
}

TEST_F(ParTreeCorrectTest, Exp50M)
{
  BTreeTest("exp", 50000000);
}

TEST_F(ParTreeCorrectTest, Norm1M)
{
  BTreeTest("norm", 1000000);
}

TEST_F(ParTreeCorrectTest, Norm5M)
{
  BTreeTest("norm", 5000000);
}

TEST_F(ParTreeCorrectTest, Norm10M)
{
  BTreeTest("norm", 10000000);
}

TEST_F(ParTreeCorrectTest, Norm50M)
{
  BTreeTest("norm", 50000000);
}

TEST_F(ParTreeCorrectTest, DualNorm10M)
{
  BTreeTest("dual_norm", 10000000);
}

TEST_F(ParTreeCorrectTest, DualNorm50M)
{
  BTreeTest("dual_norm", 50000000);
}

// Define the static member
std::string ParTreeCorrectTest::file_filter = "";

// ./unit_test --gtest_filter=ParTreeCorrectTest.Uni1M
// ./unit_test --gtest_filter=ParTreeCorrectTest.Uni1M --file_filter=1.base.in
int
main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);

  // Parse custom arguments
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--file_filter=", 0) == 0) {
      ParTreeCorrectTest::file_filter =
        arg.substr(14); // Length of "--file_filter="
      std::cout << "Filtering files containing: "
                << ParTreeCorrectTest::file_filter << std::endl;
    }
  }

  return RUN_ALL_TESTS();
}
