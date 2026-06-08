#include "test_framework.h"

#include "baselines/stdset.h"

template<typename TypeTrait>
struct StdSetWrapper
{
  using TreeType = typename StdSetTree::StdSetRaw<TypeTrait>;
};

template<typename Options, typename RunFunc>
void
ApplyStdSet(Options const& opts, RunFunc test_func)
{
#if defined(DYNAMIC_GRAPH)
  using Wrapper = Wrapper<uint64_t, uint32_t>;
#elif defined(OSM)
  using Wrapper = Wrapper<uint64_t, uint32_t>;
#else
  using Wrapper = Wrapper<uint32_t, uint32_t>;
#endif

  using K = Wrapper::K;
  using V = Wrapper::V;
  using E = Wrapper::E;
  using TypeTrait = pb_tree::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                            16,
                                            Wrapper::LeafAugEmpty,
                                            Wrapper::InteriorAugEmpty,
                                            8,
                                            pb_tree::ProtoNode>;
  Run<StdSetWrapper<TypeTrait>>(opts, test_func);
}

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Options opts(params);

  auto test_func =
    []<class TreeDesc, typename TypeTrait, typename Options>(Options& opts) {

    };

#if defined(DYNAMIC_GRAPH) || defined(OSM)
  ApplyStdSet(opts, TestTemporalGraph);
#else
  ApplyStdSet(opts, DefaultTestFunc);
#endif // DYNAMIC_GRAPH

  return 0;
}
