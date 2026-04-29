#include "test_framework.h"

#include "baselines/masstree.h"

template<typename TypeTrait>
struct MasstreeWrapperType
{
  using TreeType = typename MasstreeWrapper::MasstreeWrapper<TypeTrait>;
};

template<typename Options, typename RunFunc>
void
ApplyMasstree(Options const& opts, RunFunc test_func)
{
  using K = Wrapper::K;
  using V = Wrapper::V;
  using E = Wrapper::E;
  using TypeTrait = pb_tree::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                         16,
                                         Wrapper::LeafAugEmpty,
                                         Wrapper::InteriorAugEmpty,
                                         8,
                                         pb_tree::ProtoNode>;

  if (opts.tree_type == 5) {
    Run<MasstreeWrapperType<TypeTrait>>(opts, test_func);
  }
}

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Options opts(params);

  auto test_func =
    []<class TreeDesc, typename TypeTrait, typename Options>(Options& opts) {
      std::cout << "Masstree test finished." << std::endl;
    };

  ApplyMasstree(opts, DefaultTestFunc);

  return 0;
}
