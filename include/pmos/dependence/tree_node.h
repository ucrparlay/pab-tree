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
struct AllocEmptyLeafTag {};

struct Node {
  Node() : is_leaf{false}, size{0} {};
  Node(bool _is_leaf, size_t _size) : is_leaf{_is_leaf}, size{_size} {};

  bool is_leaf;
  size_t size;
};

template <typename TypeTrait, typename EleAssignTag = parlay::copy_assign_tag>
struct LeafNode : Node {
  using TT = TypeTrait;
  using E = typename TT::E;
  using EntrySeq = typename TT::EntrySeq;
  using AT = typename TT::LeafAug;
  using EleAssignTagType = EleAssignTag;

  // NOTE: default allocator
  LeafNode() : Node{true, static_cast<size_t>(0)}, aug(AT()) {}

  LeafNode(AT const& _aug) : Node{true, static_cast<size_t>(0)}, aug(_aug) {}

  // NOTE: alloc a leaf with default size
  template <typename Range>
  LeafNode(Range In, AllocNormalLeafTag)
      : Node{true, static_cast<size_t>(In.size())},
        elements(EntrySeq::uninitialized(TT::kBufUB)),
        aug(AT(In)) {
    assert(In.size() <= TT::kBufUB);
    std::ranges::for_each(In, [&, i = 0](auto&& x) mutable {
      parlay::assign_dispatch(elements[i++], x, EleAssignTag());
    });
  }

  // NOTE: alloc a leaf with specific size
  template <typename Range>
  LeafNode(Range In, size_t const alloc_size, AllocNormalLeafTag)
      : Node{true, static_cast<size_t>(In.size())},
        elements(EntrySeq::uninitialized(alloc_size)),
        aug(AT(In)) {
    assert(In.size() <= alloc_size);
    std::ranges::for_each(In, [&, i = 0](auto&& x) mutable {
      parlay::assign_dispatch(elements[i++], x, EleAssignTag());
    });
  }

  bool CapacityFull() const { return this->size >= TT::kBufUB; }

  EntrySeq GetEle() const { return elements.substr(0, this->size); }

  EntrySeq const& GetEleContainer() const { return this->elements; }

  EntrySeq& GetEleContainer() { return this->elements; }

  void InitializeEleSeq() {
    this->elements = EntrySeq::uninitialized(TT::kBufUB);
    return;
  }

  AT& GetAug() const { return aug; }

  // TODO: should put this one in the aug as well and don't use the monostate
  template <typename Range>
  auto UpdateAug(Range In) {
    return aug.UpdateAug(In);
  }

  auto ResetAug() { return aug.Reset(); }

  EntrySeq elements;
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
  return o;
}

// NOTE: Alloc a leaf
template <typename Range, typename Leaf>
static Leaf* AllocNormalLeafNode(Range In) {
  Leaf* o = parlay::type_allocator<Leaf>::alloc();
  new (o) Leaf(In, AllocNormalLeafTag());
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

// NOTE: multi-way node whose children # is fixed
template <typename TypeTrait>
struct MultiNode : Node {
  using BucketType = uint_fast8_t;
  using TT = TypeTrait;

  static consteval auto GetLevels() {
    typename TT::BufferSizeType a = 0;
    auto temp = TT::kBufUB;
    while (temp > 1) {
      temp >>= 1;  // divide by 2
      a++;
    }
    return a;
  }

  // using NodeArr = std::array<Node*, TT::kBufUB>;
  using NodeArr = parlay::sequence<Node*>;
  using NodeArrSlice = parlay::slice<Node**, Node**>;

  MultiNode(NodeArr const& _tree_nodes, TT::SplitSeq const& _split,
            TT::InteriorAug const& _aug)
      : Node{false,
             std::accumulate(
                 _tree_nodes.begin(), _tree_nodes.end(), static_cast<size_t>(0),
                 [](size_t acc, Node* n) -> size_t { return acc + n->size; })},
        tree_nodes(_tree_nodes),
        split(_split),
        aug(_aug) {}

  MultiNode(NodeArrSlice _tree_nodes_slice, TT::SplitSeqSlice _split_slice,
            TT::InteriorAug const& _aug)
      : Node{false,
             std::accumulate(
                 _tree_nodes_slice.begin(), _tree_nodes_slice.end(),
                 static_cast<size_t>(0),
                 [](size_t acc, Node* n) -> size_t { return acc + n->size; })},
        tree_nodes(parlay::to_sequence(_tree_nodes_slice)),
        split(parlay::to_sequence(_split_slice)),
        aug(_aug) {}

  TT::SplitSeq const& GetSplit() const { return split; }

  NodeArr tree_nodes;
  TT::SplitSeq split;
  TT::InteriorAug aug;
};

template <typename Interior, typename TT>
static Interior* AllocInteriorNode(typename Interior::NodeArr const& tree_nodes,
                                   typename TT::SplitSeq const& split) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(tree_nodes, split);
  return o;
}

template <typename Interior, typename TT>
static Interior* AllocInteriorNode(
    typename Interior::NodeArrSlice tree_nodes_slice,
    typename TT::SplitSeqSlice split_slice) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(tree_nodes_slice, split_slice);
  return o;
}

// TODO: maybe replace the NodeArr, ST, and AT with template parameters
template <typename Interior, typename TT>
static Interior* AllocInteriorNode(typename Interior::NodeArr const& tree_nodes,
                                   typename TT::SplitSeq const& split,
                                   typename TT::InteriorAug const& aug) {
  Interior* o = parlay::type_allocator<Interior>::alloc();
  new (o) Interior(tree_nodes, split, aug);
  return o;
}

template <typename Interior, typename TT>
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
// template <typename T>
// concept IsBinaryNode = CheckBinaryNode<T, pmos::BinaryNode>;

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
