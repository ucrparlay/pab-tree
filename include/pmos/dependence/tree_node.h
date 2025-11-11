#ifndef PMOS_DEPENDENCE_TREE_NODE_H_
#define PMOS_DEPENDENCE_TREE_NODE_H_

#include <parlay/slice.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <concepts>
#include <cstdint>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <variant>

#include "basic_point.h"
#include "comparator.h"
#include "dependence/concepts.h"
#include "parlay/utilities.h"

namespace pmos {

struct AllocNormalLeafTag {};
struct AllocDummyLeafTag {};
struct AllocEmptyLeafTag {};

struct Node {
  Node() : is_leaf{false}, size{0} {};
  Node(bool _is_leaf, size_t _size) : is_leaf{_is_leaf}, size{_size} {};

  bool is_leaf;
  size_t size;
};

template <typename TypeTrait, typename Range,
          typename EleAssignTag = parlay::move_assign_tag>
struct LeafNode : Node {
  using Ele = typename TypeTrait::EleType;
  using EleSeq = typename TypeTrait::EleSeq;
  using AT = typename TypeTrait::LeafAug;
  using TT = TypeTrait;

  // NOTE: default allocator
  LeafNode() : Node{true, static_cast<size_t>(0)}, is_dummy(false), aug(AT()) {}

  LeafNode(AT const& _aug)
      : Node{true, static_cast<size_t>(0)}, is_dummy(false), aug(_aug) {}

  // NOTE: alloc a leaf with default size
  LeafNode(Range In, AllocNormalLeafTag)
      : Node{true, static_cast<size_t>(In.size())},
        is_dummy(false),
        elements(EleSeq::uninitialized(TT::kBufUB)),
        aug(AT(In)) {
    assert(In.size() <= TT::kBufUB);
    std::ranges::for_each(In, [&, i = 0](auto&& x) mutable {
      parlay::assign_dispatch(elements[i++], x, EleAssignTag());
    });
  }

  // NOTE: alloc a leaf with specific size
  LeafNode(Range In, size_t const alloc_size, AllocNormalLeafTag)
      : Node{true, static_cast<size_t>(In.size())},
        is_dummy(false),
        elements(EleSeq::uninitialized(alloc_size)),
        aug(AT(In)) {
    assert(In.size() <= alloc_size);
    std::ranges::for_each(In, [&, i = 0](auto&& x) mutable {
      parlay::assign_dispatch(elements[i++], x, EleAssignTag());
    });
  }

  // NOTE: alloc a dummy leaf
  LeafNode(Range In, AllocDummyLeafTag)
      : Node{true, static_cast<size_t>(In.size())},
        is_dummy(true),
        elements(EleSeq::uninitialized(1)),
        aug(AT(In.cut(0, 1))) {
    parlay::assign_dispatch(elements[0], In[0], EleAssignTag());
  }

  bool CapacityFull() const { return this->size >= TT::kBufUB; }

  EleSeq const GetPoints() const { return elements.substr(0, this->size); }

  AT& GetAug() const { return aug; }

  AT const& GetSplit() const { return aug; }

  auto GetBox()
    requires(NodeHasNonTrivialAug<AT> && HasBox<AT>)
  {
    return aug.GetBox();
  }

  auto GetBox() const
    requires(NodeHasNonTrivialAug<AT> && HasBox<AT>)
  {
    return aug.GetBox();
  }

  // TODO: should put this one in the aug as well and don't use the monostate
  auto UpdateAug(Range In) { return aug.UpdateAug(In); }

  auto ResetAug() { return aug.Reset(); }

  bool is_dummy;
  EleSeq elements;
  AT aug;
};

template <typename NodeType, typename Range>
static auto GetAugByType(Range In) {
  if constexpr (NodeHasNonTrivialAug<typename NodeType::AT>) {
    return typename NodeType::AT(In);
  } else {
    return typename NodeType::AT();
  }
}

template <typename NodeType>
static auto GetAugByType(Node* l, Node* r) {
  if constexpr (NodeHasNonTrivialAug<typename NodeType::AT>) {
    return typename NodeType::AT(l, r);
  } else {
    return typename NodeType::AT();
  }
}

// NOTE:: Alloc a leaf with input IN and given size
// TODO: the input aug is no longer valid
template <typename Range, typename Leaf>
static Leaf* AllocFixSizeLeafNode(Range In, size_t const alloc_size) {
  Leaf* o = parlay::type_allocator<Leaf>::alloc();
  new (o) Leaf(In, alloc_size, AllocNormalLeafTag());
  assert(o->is_dummy == false);
  return o;
}

// NOTE: Alloc a leaf
template <typename Range, typename Leaf>
static Leaf* AllocNormalLeafNode(Range In) {
  Leaf* o = parlay::type_allocator<Leaf>::alloc();
  new (o) Leaf(In, AllocNormalLeafTag());
  assert(o->is_dummy == false);
  return o;
}

// NOTE: Alloc a empty Leaf
template <typename Range, typename Leaf>
static Leaf* AllocEmptyLeafNode() {
  Leaf* o = parlay::type_allocator<Leaf>::alloc();
  new (o) Leaf();
  return o;
}

// NOTE: Alloc a empty Leaf, but set the aug by hand
template <typename Range, typename Leaf>
static Leaf* AllocEmptyLeafNode(typename Leaf::AT const& aug) {
  Leaf* o = parlay::type_allocator<Leaf>::alloc();
  new (o) Leaf(aug);
  return o;
}

// NOTE: Alloc a dummy leaf
template <typename Range, typename Leaf>
static Leaf* AllocDummyLeafNode(Range In) {
  Leaf* o = parlay::type_allocator<Leaf>::alloc();
  new (o) Leaf(In, AllocDummyLeafTag());
  assert(o->is_dummy == true);
  assert(o->pts.size() == 1);
  return o;
}

template <typename Point, typename SplitType, typename AugType>
struct BinaryNode : Node {
  using PT = Point;
  using ST = SplitType;
  using AT = AugType;

  BinaryNode(Node* _left, Node* _right, const ST& _split, const AT& _aug)
      : Node{false, _left->size + _right->size},
        left(_left),
        right(_right),
        split(_split),
        aug(_aug) {}

  // NOTE: test whether we can fetch @depth levels from @T
  template <typename Interior>
  static inline bool TestDepth(Node* T, int cur_depth, int depth) {
    if (cur_depth == depth) {
      return true;
    }
    if (T->is_leaf) {
      return false;
    }
    auto TI = static_cast<Interior*>(T);
    return TestDepth<Interior>(TI->left, cur_depth + 1, depth) &&
           TestDepth<Interior>(TI->right, cur_depth + 1, depth);
  }

  ST const& GetSplit() const { return split; }

  Node* left;
  Node* right;
  ST split;
  AT aug;
};

// NOTE: multi-way node whose children # is fixed
template <typename TypeTrait, typename SplitType, typename AugType>
struct MultiNode : Node {
  using BucketType = uint_fast8_t;
  using TT = TypeTrait;
  using Ele = typename TT::EleType;
  using EleSeq = typename TT::EleSeq;
  using ST = SplitType;
  using AT = AugType;

  static consteval auto GetLevels() {
    typename TT::BufferSizeType a = 0;
    auto temp = TT::kBufUB;
    while (temp > 1) {
      temp >>= 1;  // divide by 2
      a++;
    }
    return a;
  }

  using NodeArr = std::array<Node*, TT::kBufUB>;

  MultiNode(NodeArr const& _tree_nodes, const ST& _split, const AT& _aug)
      : Node{false,
             std::accumulate(
                 _tree_nodes.begin(), _tree_nodes.end(), static_cast<size_t>(0),
                 [](size_t acc, Node* n) -> size_t { return acc + n->size; })},
        tree_nodes(_tree_nodes),
        split(_split),
        aug(_aug) {}

  ST const& GetSplit() const { return split; }

  NodeArr tree_nodes;
  ST split;
  AT aug;
};

template <typename Interior>
static Interior* AllocInteriorNode(Node* L, Node* R,
                                   typename Interior::ST const& split) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(L, R, split);
  return o;
}

template <typename Interior>
static Interior* AllocInteriorNode(Node* L, Node* R,
                                   typename Interior::ST const& split,
                                   typename Interior::AT const& aug) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(L, R, split, aug);
  return o;
}

template <typename Interior>
static Interior* AllocInteriorNode(typename Interior::NodeArr const& tree_nodes,
                                   typename Interior::ST const& split) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(tree_nodes, split);
  return o;
}

// TODO: maybe replace the NodeArr, ST, and AT with template parameters
template <typename Interior>
static Interior* AllocInteriorNode(typename Interior::NodeArr const& tree_nodes,
                                   typename Interior::ST const& split,
                                   typename Interior::AT const& aug) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(tree_nodes, split, aug);
  return o;
}

template <typename Interior>
static Interior* AllocInteriorNode() {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior();
  return o;
}

template <typename NodeType>
static void FreeNode(Node* T) {
  parlay::type_allocator<NodeType>::retire(static_cast<NodeType*>(T));
}

// Define some alias for concept check
template <typename T>
concept IsBinaryNode = CheckBinaryNode<T, pmos::BinaryNode>;

template <typename T>
concept IsMultiNode = CheckMultiNode<T, pmos::MultiNode>;

// TODO: may remove below from inner tree
template <typename T>
concept IsPointerToNode = CheckPointerToNode<T, Node>;

template <typename T, typename Point>
concept IsNodeBox = requires {
  requires IsPair<T> && IsPointerToNode<typename T::first_type> &&
               IsBox<typename T::second_type, Point>;
};

}  // namespace pmos

#endif  // PMOS_DEPENDENCE_TREE_NODE_H_
