#ifndef PMOS_B_TREE_H_
#define PMOS_B_TREE_H_

#include <cstdint>

#include "dependence/box_op.h"
#include "dependence/comparator.h"
#include "dependence/loggers.h"
#include "dependence/tree_node.h"
#include "dependence/type_trait.h"

namespace pmos {
// Template definition for the operator
template<typename T1, typename T2>
auto
operator<<(std::ostream& os, std::pair<T1, T2> const& p) -> std::ostream&
{
  os << "(" << p.first << ", " << p.second << ")";
  return os;
}

// NOTE: Basetree
template<class TypeTrait>
class BTree
{
public:
  using TT = TypeTrait;
  // TODO: Do we really need to define these types again?

  using E = typename TT::E;
  using K = typename TT::K;
  using V = typename TT::V;
  using Entry = TT::Entry;

  using Node = TT::Node;
  using Num = typename TT::Num;
  using EntrySlice = typename TT::EntrySlice;
  using Split = typename TT::Split;
  using NodeSplitSeqPair =
    std::pair<typename TT::NodeSeq, typename TT::SplitSeq>;

  using NodeSplitPair = std::pair<Node*, Split>;
  using NodeSplitPairSeq = parlay::sequence<NodeSplitPair>;
  using NodeSplitPairSlice = parlay::slice<NodeSplitPair*, NodeSplitPair*>;
  using NodeSeqList = parlay::sequence<typename TT::NodeSeq>;
  using SplitSeqList = parlay::sequence<typename TT::SplitSeq>;

  using LeafAug = typename TT::LeafAug;
  using InteriorAug = typename TT::InteriorAug;

  using InputRangeT = TT::EntrySeq;
  using InputDeleteRangeT = TT::KeySeq;

  // NOTE: Const variables
  // NOTE: uint32t handle up to 4e9 at least
  // WARN: bucket num should smaller than 1<<8 to handle type overflow
  using BucketType = typename TT::BucketType;
  static constexpr BucketType const kBuildDepthOnce = TT::kSkeletonHeight;
  static constexpr BucketType const kPivotNum = TT::kPivotNum;
  static constexpr BucketType const kBucketNum = TT::kBucketNum;
  static constexpr uint_fast8_t const kLog2Base = TT::kLog2Base;
  static constexpr uint_fast16_t const kBlockSize = TT::kBlockSize;

  using SizeBufUBArr = typename TT::SizeBufUBArr;
  using BufSizeBufUBArr = typename TT::BufSizeBufUBArr;
  using BooleanBufUBArr = typename TT::BooleanBufUBArr;
  using BufSizeT = TT::BufSizeT;
  using SizeT = TT::SizeT;

  // NOTE: tree structure
  static constexpr uint_fast16_t const kSerialBuildCutoff = 512;

  // NOTE: constructor and destructor
  BTree() = default;

  template<typename Range>
  BTree(Range in)
  {
    this->Build(in);
  }

  template<typename Range>
  BTree(Range const& in)
  {
    this->Build(in);
  }

  // copy constructor
  // copy @other
  BTree(const BTree& other)
    : root_(other.root_ ? this->CopyTreeRecursive(other.root_) : nullptr)
  {
  }

  // copy assignment operator
  // copy @other, will launch a deep copy
  BTree& operator=(const BTree& other)
  {
    if (this == &other) {
      return *this;
    }

    this->DeleteTree(); // delete current tree
    this->root_ = other.root_ ? this->CopyTreeRecursive(other.root_) : nullptr;
    return *this;
  }

  // move constructor
  BTree(BTree&& other) noexcept
    : root_(other.root_)
  {
    other.root_ = nullptr;
  }

  // move assignment operator
  BTree& operator=(BTree&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }

    this->DeleteTree();        // delete current tree
    this->root_ = other.root_; // steal
    other.root_ = nullptr;

    return *this;
  }

  // destructor
  ~BTree()
  {
    this->DeleteTree();
  }

  // PARA: interior node type
  struct BTreeInteriorNode : MultiWayNode<TT>
  {
    using TT = TT;
    using BaseNode = MultiWayNode<TT>;

    BTreeInteriorNode(TT::NodeSeq const& _tree_nodes,
                      TT::SplitSeq const& _split)
      : BaseNode(_tree_nodes, _split, typename TT::InteriorAug())
      , valid(true)
      , invalid_subtree_num(0)
    {
    } // TODO:finish AT

    BTreeInteriorNode(TT::NodeSlice _tree_nodes_slice,
                      TT::SplitSlice _split_slice)
      : BaseNode(_tree_nodes_slice, _split_slice, typename TT::InteriorAug())
      , valid(true)
      , invalid_subtree_num(0)
    {
    } // TODO:finish AT

    BTreeInteriorNode(TT::NodeSeq const& _tree_nodes,
                      TT::SplitSeq const& _split,
                      TT::InteriorAug const& _aug)
      : BaseNode(_tree_nodes, _split, _aug)
      , valid(true)
      , invalid_subtree_num(0)
    {
    }

    auto UpdateAug()
    {
      return this->aug.Update();
    }

    void SetParallelFlag(bool const flag)
    {
      this->aug.SetParallelFlag(flag);
    }

    void ResetParallelFlag()
    {
      this->aug.ResetParallelFlag();
    }

    auto GetParallelFlagIniStatus() -> bool
    {
      return this->aug.GetParallelFlagIniStatus();
    }

    [[nodiscard]] auto ForceParallel() const -> bool
    {
      return this->aug.ForceParallel(this->size, kSerialBuildCutoff);
    }

    void ResetDeletionFlag()
    {
      this->valid = true;
      this->invalid_subtree_num = 0;
      this->reinsert_size = 0;
    }

    bool valid;
    SizeT invalid_subtree_num;
    SizeT reinsert_size;
  };

  using Leaf = LeafNode<TT, parlay::move_assign_tag>;
  using Interior = BTreeInteriorNode;

  // NOTE: helper function to decide the bucket number and range
  static SizeT GetBktNum(SizeT const item_n)
  {
    return item_n / TT::kBufMD;
  }

  static SizeT GetBktStIdx(SizeT const i, SizeT const bkt_n, SizeT const n)
  {
    return (static_cast<size_t>(n) * i) / bkt_n;
  };

  static SizeT GetBktEndIdx(SizeT const i, SizeT const bkt_n, SizeT const n)
  {
    return (static_cast<size_t>(n) * (i + 1)) / bkt_n;
  };

  // PARA: return whether a node is invalid
  static bool NodeHasLeafChildren(const Node* T)
  {
    return T->is_leaf ? false
                      : static_cast<const Interior*>(T)->tree_nodes[0]->is_leaf;
  }

  static bool SelfBalanceNode(const Node* T, const bool tomb)
  {
    return NodeHasLeafChildren(T) && tomb && NodeIsValid(T);
  }

  static bool GetNodeInvalidStatus(const Node* T)
  {
    return T->is_leaf ? (T->size < TT::kBufLB)
                      : (!static_cast<const Interior*>(T)->valid);
  }

  static bool NodeIsValid(const Node* T)
  {
    return !GetNodeInvalidStatus(T);
  }

  static SizeT GetNodeInvalidSubtreeNum(const Node* T)
  {
    return T->is_leaf ? (T->size < TT::kBufLB ? 1 : 0)
                      : static_cast<const Interior*>(T)->invalid_subtree_num;
  }

  static SizeT GetNodeReinsertSize(const Node* T)
  {
    return T->is_leaf ? (T->size < TT::kBufLB ? T->size : 0)
                      : static_cast<const Interior*>(T)->reinsert_size;
  }

  // NOTE: build tree
  template<typename Range>
  void Build(Range&& In);

  template<typename Range>
  void Build_(Range In);

  template<typename Range>
  Node* BuildRecursive(Range In);

  Node* AssembleInteriorLevels(TT::NodeSlice nodes, TT::SplitSlice keys);

  template<typename Range>
  void BatchInsert(Range&& In);

  template<typename Range>
  void BatchInsert_(Range In, bool sorted = false);

  template<typename Range>
  void BatchInsertSieve(Range In);

  template<typename Range>
  SizeT BatchInsertRecursive(Node* T,
                             Range In,
                             NodeSplitPairSlice node_split_slice);

  template<typename Range>
  SizeT BatchInsertSieveRecursive(Node* T,
                                  Range In,
                                  Range Out,
                                  NodeSplitPairSlice node_split_slice);

  template<typename Range>
  void BatchDelete(Range&& In);

  template<typename Range>
  void BatchDelete_(Range In);

  // Returns (invalid_flag, needs_fetch, reinsert_entry_num).
  // needs_fetch: any subtree needs FetchInvalidSubtreeNodes (not stored in
  // node).
  std::tuple<bool, bool, SizeT> BatchDeleteRecursive(Node* T, TT::KeySlice In);

  // Remove the invalid subtree and put it in flatten_node_slice
  // Needs to compact the tree_nodes if find a invalid subtree and update the
  // interior node size and valid flag after the compaction
  // Return the total number entries in the tree that needs to rebuild
  void FetchInvalidSubtreeNodes(Node* T,
                                EntrySlice reinsert_entries,
                                bool has_tomb

  );

  std::pair<Node*, Node*> SplitLeaf(const Entry* a,
                                    TT::BufSizeT n,
                                    const Entry* b,
                                    TT::BufSizeT m);

  std::pair<Node*, Node*> InsertToLeafNoOverflow(Node* T, EntrySlice In);

  void ReInsertTreeAfterDelete(Node* T, EntrySlice reinsert_entries);

  void PruneOutEmptyTree(Node* T);

  void BalanceOverLeaves(Node* T, const SizeT reinsert_size);

  static Node* BatchInsertUpdateInterior(Node* T,
                                         NodeSplitPairSlice node_split_slice,
                                         SizeBufUBArr const& split_pos_arr,
                                         SizeT new_node_n,
                                         SizeBufUBArr const& return_node_n_arr);

  std::tuple<bool, SizeT, SizeT> BatchDeleteUpdateInteriorSize(
    Node* T,
    SizeBufUBArr const& invalid_subtree_num_arr,
    BooleanBufUBArr const& invalid_subtree_flag_arr,
    SizeBufUBArr const& reinsert_entries_num_arr);

  // PARA: return bucket_num
  static SizeT GenerateLeafNodes(TT::EntrySlice key_in,
                                 TT::SplitSlice key_out,
                                 TT::NodeSlice node_out);

  static SizeT GenerateLeafNodes(TT::EntrySlice key_in,
                                 NodeSplitPairSlice node_split_out);

  // PARA: return bucket_num
  template<typename KeyFunc, typename NodeFunc>
  struct GenNodesOutputHandler
  {
    KeyFunc set_key;
    NodeFunc set_node;
  };

  template<typename OutputHandler>
  static SizeT GenerateLeafNodes(TT::EntrySlice key_in,
                                 OutputHandler&& output_handler);

  template<typename OutputHandler>
  static SizeT GenerateInteriorNodes(TT::NodeSlice node_in,
                                     TT::SplitSlice key_in,
                                     OutputHandler&& output_handler);
  // NOTE: batch insert
  template<typename Range>
  static Node* InsertPoints2Leaf(Node* T, Range In);

  static void PartitionBinarySearchRec(auto split_seq_s,
                                       // parlay::sequence<int>& split_pos_seq,
                                       auto& split_pos_seq,
                                       EntrySlice entry_seq_s,
                                       size_t split_offset,
                                       size_t In_offset);

  template<typename Range>
  static SizeT HandlingLeafSplitOverflow(Node* T,
                                         Range In,
                                         NodeSplitPairSlice node_split_slice);

  static SizeT HandlingInteriorSplitOverflow(
    Node* T,
    NodeSplitPairSlice node_split_slice,
    SizeBufUBArr const& split_pos_arr,
    SizeT new_node_n,
    SizeBufUBArr const& return_node_n_arr);

  static TT::SizeT DeletePoints4Leaf(Node* T, TT::KeySlice In);

  // template <typename Leaf, typename RT>
  // static RT DiffPoints4Leaf(Node* T, Slice In);
  //

  template<typename Range>
  static void SplitBatchBinarySearch(SizeBufUBArr& split_pos_arr,
                                     Range In,
                                     TT::SplitSliceConst split_slice);

  template<typename Range>
  static void SplitBatchSieve(SizeBufUBArr& split_pos_arr,
                              Range In,
                              TT::SplitSliceConst split_slice);

  template<typename Range>
  static void SplitBatchMerge(SizeBufUBArr& split_pos_arr,
                              Range In,
                              TT::SplitSliceConst split_slice);

  template<typename T>
  static inline BufSizeT RetriveTag(T e, TT::SplitSliceConst split_slice);

  template<typename Range>
  static TT::BallSeq SeievePoints(Range A,
                                  Range B,
                                  TT::SplitSliceConst split_seq);
  // NOTE: delete tree
  // template <SupportsForceParallel Interior, bool granularity>
  template<bool granularity>
  inline static bool ForceParallelRecursion(Interior const* T);

  Node* CopyTreeRecursive(Node* T);

  void DeleteTree();

  template<bool granularity = true>
  static void DeleteTreeRec(Node* T);
  // NOTE: utility
  // TODO: better evaluate the parallel recursion function
  template<typename Range, bool granularity = true, bool sorted = false>
  static void FlattenRec(Node* T, Range Out);

  template<typename Range, bool sorted = false>
  static void ExtractPointsInLeaf(Node* T, Range Out);

  // NOTE: validations

  static TT::Box GetNodeBoxRec(Node const* T);

  static TT::Box GetNodeSliceBox(TT::NodeSlice node_slice);

  static TT::Box GetNodeSliceBox(TT::ConstNodeSlice node_slice);

  // static TT::Box GetNodeBox(Node const* T);

  template<bool kCheckSize = true>
  TT::Box VerifyTreeProperty(Node const* T);

  bool VerifyUnoverlappedTree(Node const* T, TT::NodeSlice node_slice);

  TT::HeightT CheckHeight(Node const* T);

  bool CheckContainNode(Node const* T, Node const* P);

  template<typename Leaf, typename Interior>
  int GetHeight(Node const* T);

  template<typename Leaf, typename Interior>
  int GetTreeNodesNum(Node const* T);

  template<bool kCheckSize = true, bool kVerbose = true>
  bool Validate(Node const* T = nullptr);

  std::tuple<bool, bool, typename TT::SizeT> CheckTreeAugAfterDelete(Node* T);

  auto Contain(K const& k) const -> std::pair<bool, MembershipLogger>;

  bool ContainIter(Node const* T, K const& k) const;

  bool ContainRec(Node const* T, K const& k) const;

  auto RangeCount(TT::Box query_box) const
    -> std::pair<SizeT, RangeReportLogger>;

  void RangeCountLeaf(Node const* T,
                      TT::Box const& query_box,
                      SizeT& offset) const;

  void RangeCountRec(Node const* T,
                     TT::Box const& query_box,
                     SizeT& offset,
                     RangeReportLogger& logger) const;

  template<typename Range>
  auto RangeReport(TT::Box query_box, Range&& out) const
    -> std::pair<SizeT, RangeReportLogger>;

  template<typename Range>
  void RangeReportLeaf(Node const* T,
                       TT::Box const& query_box,
                       SizeT& offset,
                       Range out) const;

  template<typename Range>
  void RangeReportIter(Node const* T,
                       TT::Box const& query_box,
                       SizeT& offset,
                       Range out,
                       RangeReportLogger& logger) const;

  template<typename Range>
  void RangeReportRec(Node const* T,
                      TT::Box const& query_box,
                      SizeT& offset,
                      Range out,
                      RangeReportLogger& logger) const;

  template<typename Range>
  void RangeReportFlattenRec(Node const* T,
                             SizeT& offset,
                             Range out,
                             RangeReportLogger& logger) const;

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return input_seq;
  }

  static auto ParseDeleteInput(TT::EntrySeq const& input_seq)
    -> InputDeleteRangeT
  {
    return parlay::map(input_seq, [&](auto e) { return Entry::GetKey(e); });
  }

  Node* GetRoot()
  {
    return this->root_;
  }

  size_t GetSize()
  {
    return this->root_ ? this->root_->size : 0;
  }

  constexpr static char const* GetTreeName()
  {
    return "BTree";
  }

  constexpr static int GetLeafSize()
  {
    return TT::kBuf;
  }

private:
  Node* root_ = nullptr;
  parlay::internal::timer insert_leaf_timer;
  parlay::internal::timer lower_bound_timer;
  parlay::internal::timer merge_timer;
  parlay::internal::timer recurse_timer;

  double insert_leaf_time = 0.0;
  double lower_bound_time = 0.0;
  double merge_time = 0.0;
  double recurse_time = 0.0;

  int vis_leaf_num = 0;
  int vis_inter_num = 0;

  std::atomic<size_t> max_reinsert_batch_size = 0;
};

} // namespace pmos

#include "b_tree_impl/b_box_op.hpp"
#include "b_tree_impl/batch_op/b_batch_delete.hpp"
#include "b_tree_impl/batch_op/b_batch_insert.hpp"
// #include "b_tree_impl/batch_op/b_batch_insert_sieve.hpp"
#include "b_tree_impl/batch_op/b_build_tree.hpp"
#include "b_tree_impl/batch_op/b_delete_tree.hpp"
#include "b_tree_impl/points_op.hpp"
#include "b_tree_impl/query_op/membership.hpp"
#include "b_tree_impl/query_op/range_count.hpp"
#include "b_tree_impl/query_op/range_report.hpp"
#include "b_tree_impl/tree_op/flatten.hpp"
#include "b_tree_impl/tree_op/leaf_op.hpp"
#include "b_tree_impl/tree_op/node_op.hpp"
#include "b_tree_impl/validation.hpp"

#endif // PMOS_B_TREE_H_
