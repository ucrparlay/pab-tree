#include "test_framework.h"
#include <cstdint>

// NOTE: Trees
template<typename TypeTrait>
struct BTreeWrapper
{
  using TreeType = typename pb_tree::BTree<TypeTrait>;
};

template<typename Options, typename RunFunc>
static void
ApplyOrthogonal(Options const& opts, RunFunc test_func)
{
  // using EleType = Element<uint32_t, uint32_t>;
#ifdef LEAF1
  static constexpr uint const kBuf = 1;
#elif defined(LEAF2)
  static constexpr uint const kBuf = 2;
#elif defined(LEAF4)
  static constexpr uint const kBuf = 4;
#elif defined(LEAF8)
  static constexpr uint const kBuf = 8;
#elif defined(LEAF16)
  static constexpr uint const kBuf = 16;
#elif defined(LEAF32)
  static constexpr uint const kBuf = 32;
#elif defined(LEAF64)
  static constexpr uint const kBuf = 64;
#elif defined(LEAF128)
  static constexpr uint const kBuf = 128;
#elif defined(LEAF256)
  static constexpr uint const kBuf = 256;
#elif defined(LEAF512)
  static constexpr uint const kBuf = 512;
#elif defined(LEAF1024)
  static constexpr uint const kBuf = 1024;
#else
  static constexpr uint const kBuf = 64; // Default
#endif // LEAF1

#if defined(DYNAMIC_GRAPH)
  using Wrapper = Wrapper<uint64_t, uint32_t>;
#elif defined(OSM)
  // using Wrapper = Wrapper<std::pair<uint64_t, uint64_t>, uint32_t>;
  using Wrapper = Wrapper<uint64_t, uint32_t>;
#else
  using Wrapper = Wrapper<uint32_t, uint32_t>;
#endif

  using K = Wrapper::K;
  using V = Wrapper::V;
  using E = Wrapper::E;
  using TypeTrait = pb_tree::BTreeTypeTrait<Wrapper::ElementWrapper<E>,
                                         kBuf,
                                         Wrapper::LeafAugEmpty,
                                         Wrapper::InteriorAugEmpty,
                                         8,
                                         pb_tree::ProtoNode>;
  if (opts.tree_type == 0) {
    Run<BTreeWrapper<TypeTrait>>(opts, test_func);
  }
}

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Options opts(params);

#if defined(DYNAMIC_GRAPH) || defined(OSM)
  ApplyOrthogonal(opts, TestTemporalGraph);
#else
  ApplyOrthogonal(opts, DefaultTestFunc);
#endif // DYNAMIC_GRAPH

  return EXIT_SUCCESS;
}
