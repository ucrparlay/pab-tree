#include "test_framework.h"

#include "baselines/BwTree.h"

namespace wangziqi2013::bwtree {
bool print_flag = false;
thread_local int BwTreeBase::gc_id = -1;
std::atomic<size_t> BwTreeBase::total_thread_num{ 0UL };
} // namespace wangziqi2013::bwtree

template<typename TypeTrait>
struct BwTreeWrapperType
{
  using TreeType = typename BwTreeBaseline::BwTreeWrapper<TypeTrait>;
};

template<typename Options, typename RunFunc>
void
ApplyBwTree(Options const& opts, RunFunc test_func)
{
  using K = Wrapper::K;
  using V = Wrapper::V;
  using E = Wrapper::E;
  using TypeTrait = pmos::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                         16,
                                         Wrapper::LeafAugEmpty,
                                         Wrapper::InteriorAugEmpty,
                                         8,
                                         pmos::ProtoNode>;

  if (opts.tree_type == 6) {
    Run<BwTreeWrapperType<TypeTrait>>(opts, test_func);
  }
}

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Options opts(params);

  auto test_func =
    []<class TreeDesc, typename TypeTrait, typename Options>(Options& opts) {
      std::cout << "BwTree test finished." << std::endl;
    };

  ApplyBwTree(opts, DefaultTestFunc);

  return 0;
}
