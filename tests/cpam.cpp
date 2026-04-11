#include "test_framework.h"

#include "baselines/cpam.h"
#include "baselines/pam.h"

template<typename TypeTrait>
struct CpamWrapper
{
  using TreeType = typename CPAMTree::CpamRaw<TypeTrait>;
};
template<typename TypeTrait>
struct PamWrapper
{
  using TreeType = typename PAMTree::PamRaw<TypeTrait>;
};

template<typename Options, typename RunFunc>
void
ApplyCPAM(Options const& opts, RunFunc test_func)
{
#ifndef DYNAMIC_GRAPH
  using Wrapper = Wrapper<uint32_t, uint32_t>;
#else
  using Wrapper = Wrapper<std::pair<uint32_t, uint32_t>, uint32_t>;
#endif // DYNAMIC_GRAPH

  using K = Wrapper::K;
  using V = Wrapper::V;
  using E = Wrapper::E;
  // using EleType = Element<uint32_t, uint32_t>;
  using TypeTrait = pmos::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                         16,
                                         Wrapper::LeafAugEmpty,
                                         Wrapper::InteriorAugEmpty,
                                         8,
                                         pmos::ProtoNode>;
  if (opts.tree_type == 0) {
    // B-tree
  } else if (opts.tree_type == 1) {
    Run<CpamWrapper<TypeTrait>>(opts, test_func);
  } else if (opts.tree_type == 2) {
    Run<PamWrapper<TypeTrait>>(opts, test_func);
  }
}

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Options opts(params);

  auto test_func =
    []<class TreeDesc, typename TypeTrait, typename Options>(Options& opts) {};

  // Wrapper::ApplyBaselines(opts, test_func);
  // ApplyCPAM(opts, DefaultTestFunc);

#ifndef DYNAMIC_GRAPH
  ApplyCPAM(opts, DefaultTestFunc);
#else
  ApplyCPAM(opts, TestTemporalGraph);
#endif // DYNAMIC_GRAPH
  return 0;
}
