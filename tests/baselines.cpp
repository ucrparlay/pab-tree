#include "test_framework.h"

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Wrapper::Options opts(params);

  auto test_func =
    []<class TreeDesc, typename TypeTrait, typename Options>(Options& opts) {
      using Tree = TreeDesc::TreeType;
      using TT = TypeTrait;
      using EntrySeq = TT::EntrySeq;
      EntrySeq wp;
      EntrySeq wi;

      using PmaLeaf = uncompressed_leaf<typename TT::K, typename TT::V>;
      using PmosPmaSettings = PMA_traits<PmaLeaf, InPlace, 0, false, false>;
      using PmaPointer = PmaLeaf::element_ptr_type;
      using PMA = CPMA<PmosPmaSettings>;

      read_points<typename TT::E>(opts.input_file_path.c_str(), wp, 0);

      parlay::internal::timer t("PMA Batch Insert Test");
      for (int i = 0; i < 4; i++) {
        auto P = Tree::ParseInput(wp);
        PmaPointer it(P.first.begin(), P.second.begin());
        PMA p;

        t.start();
        // parlay::sort_inplace(parlay::make_slice(P.first));
        // t.next("Sorted keys");
        p.insert_batch(it, P.first.size(), false);
        t.stop();
      }
      std::cout << "average insertion time for " << t.total_time() / 4
                << " over 4 rounds: ";

      // auto [base_path, batch_path] =
      // opts.ParseInsertBatchPath(opts.batch_ratio);
      // // std::cout << base_path << " " << batch_path << std::endl;
      // auto [n, d] = read_points<typename TT::E>(base_path.c_str(), wp, 0);
      // auto [n2, d2] = read_points<typename TT::E>(batch_path.c_str(), wi, n);

      // for (int i = 0; i < 4; i++) {
      //   auto WP = Tree::ParseInput(wp).first;
      //   auto WI = Tree::ParseInput(wi).first;
      //   PMA p;
      //   PmaPointer it(WP.begin());
      //   p.insert_batch(it, WP.size(), false);
      //   PmaPointer new_it(WI.begin());
      //   p.insert_batch(new_it, WI.size(), false);

      //   std::cout << "Round " << i << " done." << std::endl;
      // }

      std::cout << "Test finished." << std::endl;
    };

  Wrapper::ApplyBaselines(opts, test_func);
  // Wrapper::ApplyBaselines(opts, DefaultTestFunc);

  return 0;
}
