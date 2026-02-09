#ifndef PMOS_B_TREE_H_
#define PMOS_B_TREE_H_

#include <cstdint>

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
    {
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

    auto UpdateAug()
    {
      return this->aug.Update();
    }

    bool valid;
    SizeT invalid_subtree_num;
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
    return (n * i) / bkt_n;
  };

  static SizeT GetBktEndIdx(SizeT const i, SizeT const bkt_n, SizeT const n)
  {
    return (n * (i + 1)) / bkt_n;
  };

  static SizeT GetNodeInvalidSubtreeNum(Node* T)
  {
    return T->is_leaf ? T->size < TT::kBufLB ? 1 : 0
                      : static_cast<Interior*>(T)->invalid_subtree_num;
  }

  // NOTE: build tree
  template<typename Range>
  void Build(Range&& In);

  void Build_(EntrySlice In);

  Node* BuildRecursive(EntrySlice In);

  template<typename Range>
  void BatchInsert(Range&& In);

  void BatchInsert_(EntrySlice In);

  void BatchInsertSieve(EntrySlice In);

  SizeT BatchInsertRecursive(Node* T,
                             EntrySlice In,
                             NodeSplitPairSlice node_split_slice);

  SizeT BatchInsertSieveRecursive(Node* T,
                                  EntrySlice In,
                                  EntrySlice Out,
                                  NodeSplitPairSlice node_split_slice);

  template<typename Range>
  void BatchDelete(Range&& In);

  void BatchDelete_(EntrySlice In);

  // PARA: return whether T and all its subtree is valid
  // Only needs to ensure the tree size is correct
  SizeT BatchDeleteRecursive(Node* T, EntrySlice In);

  // Remove the invalid subtree and put it in flatten_node_slice
  // Needs to compact the tree_nodes if find a invalid subtree and update the
  // interior node size and valid flag after the compaction
  // Return the total number entries in the tree that needs to rebuild
  SizeT FetchInvalidSubtreeNodes(Node* T,
                                 typename TT::NodeSlice flatten_node_slice);

  // void BatchInsertRecursive(Node* T, EntrySlice In);

  // static inline BucketType FindBucket(Point const& p,
  //                                     HyperPlaneSeq const& pivots);
  //
  // template <IsBinaryNode Interior, bool UpdateParFlag = true>
  // static inline void UpdateInterior(Node* T, Node* L, Node* R);
  //
  // template <IsBinaryNode Interior, bool UpdateParFlag = true>
  // static inline void UpdateInterior(Node* T, NodeBox const& L,
  //                                   NodeBox const& R);
  //
  static Node* BatchInsertUpdateInterior(Node* T,
                                         NodeSplitPairSlice node_split_slice,
                                         SizeBufUBArr const& split_pos_arr,
                                         SizeT new_node_n,
                                         SizeBufUBArr const& return_node_n_arr);

  static SizeT BatchDeleteUpdateInteriorSize(
    Node* T,
    SizeBufUBArr invalid_subtree_num_arr);

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
  //
  // template <typename Leaf, typename Interior, bool granularity = true>
  // static void PrepareRebuild(Node* T, Slice In, Points& wx, Points& wo);
  //
  // template <typename Leaf, typename Interior, bool granularity = true>
  // static void PrepareRebuild(Node* T, Points& wx, Points& wo);
  //
  // static void Partition(Slice A, Slice B, size_t const n,
  //                       HyperPlaneSeq const& pivots,
  //                       parlay::sequence<BallsType>& sums);
  //
  // NOTE: batch insert

  static Node* InsertPoints2Leaf(Node* T, EntrySlice In);

  static void PartitionBinarySearchRec(auto split_seq_s,
                                       // parlay::sequence<int>& split_pos_seq,
                                       auto& split_pos_seq,
                                       EntrySlice entry_seq_s,
                                       size_t split_offset,
                                       size_t In_offset);

  static SizeT HandlingLeafSplitOverflow(Node* T,
                                         EntrySlice In,
                                         NodeSplitPairSlice node_split_slice);

  static SizeT HandlingInteriorSplitOverflow(
    Node* T,
    NodeSplitPairSlice node_split_slice,
    SizeBufUBArr const& split_pos_arr,
    SizeT new_node_n,
    SizeBufUBArr const& return_node_n_arr);

  static TT::SizeT DeletePoints4Leaf(Node* T, EntrySlice In);

  // template <typename Leaf, typename RT>
  // static RT DiffPoints4Leaf(Node* T, Slice In);
  //

  static void SplitBatchBinarySearch(SizeBufUBArr& split_pos_arr,
                                     EntrySlice In,
                                     TT::SplitSliceConst split_slice);

  static void SplitBatchSieve(SizeBufUBArr& split_pos_arr,
                              EntrySlice In,
                              TT::SplitSliceConst split_slice);

  static void SplitBatchMerge(SizeBufUBArr& split_pos_arr,
                              EntrySlice In,
                              TT::SplitSliceConst split_slice);

  static inline BucketType RetriveTag(E e, TT::SplitSliceConst split_slice);

  static TT::BallSeq SeievePoints(EntrySlice A,
                                  EntrySlice B,
                                  TT::SplitSliceConst split_seq);
  //
  // template <typename Leaf, typename Interior, typename PrepareFunc,
  //           typename... Args>
  // Node* RebuildWithInsert(Node* T, PrepareFunc prepare_func, Slice In,
  //                         Args&&... args);
  //
  // template <typename Leaf, typename Interior, bool granularity,
  //           typename... Args>
  // Node* RebuildSingleTree(Node* T, Args&&... args);
  //
  // template <typename Leaf, IsBinaryNode Interior, bool granularity,
  //           typename PrepareFunc, typename... Args>
  // Node* RebuildTreeRecursive(Node* T, PrepareFunc&& prepare_func,
  //                            bool const allow_rebuild, Args&&... args);
  //
  // template <typename Leaf, IsMultiNode Interior, bool granularity,
  //           typename PrepareFunc, typename... Args>
  // Node* RebuildTreeRecursive(Node* T, PrepareFunc&& prepare_func,
  //                            bool const allow_rebuild, Args&&... args);
  //
  // template <typename Leaf, IsBinaryNode Interior, typename Base>
  // static Base BuildInnerTree(BucketType idx, HyperPlaneSeq const& pivots,
  //                            parlay::sequence<Base> const& tree_nodes);
  //
  // template <IsMultiNode Interior>
  // static Node* BuildInnerTree(BucketType idx, HyperPlaneSeq const& pivots,
  //                             parlay::sequence<Node*> const& tree_nodes);
  //
  // static PointsIter SerialPartition(Slice In, DimsType d);
  //
  // // NOTE: delete tree
  // template <SupportsForceParallel Interior, bool granularity>
  template<bool granularity>
  inline static bool ForceParallelRecursion(Interior const* T);

  Node* CopyTreeRecursive(Node* T);

  void DeleteTree();

  template<bool granularity = true>
  static void DeleteTreeRec(Node* T);

  // // NOTE: KNN query stuffs
  // static inline DisType P2PDistanceSquare(Point const& p, Point const& q);
  //
  // static inline DisType P2BMinDistanceSquare(Point const& p, Box const& a);
  //
  // static inline DisType P2BMaxDistanceSquare(Point const& p, Box const& a);
  //
  // static inline double P2CMinDistance(Point const& p, Point const& center,
  //                                     DisType const r);
  //
  // template <typename CircleType>
  // static inline double P2CMinDistance(Point const& p, CircleType const&
  // cl);
  //
  // static inline DisType InterruptibleDistance(Point const& p, Point const&
  // q,
  //                                             DisType up);
  //
  // // NOTE: get the split for a node
  // template <typename Leaf, typename Interior>
  // static inline typename Interior::ST const& GetSplit(Node const* node)
  //   requires std::same_as<typename BTree::Box, typename Interior::ST>;
  //
  // // NOTE: searech knn in the leaf
  // template <typename Leaf, typename Range>
  // static void KNNLeaf(Node* T, Point const& q, kBoundedQueue<Point, Range>&
  // bq);
  //
  // // NOTE: search knn in the binary node
  // template <typename Leaf, IsBinaryNode Interior, typename Range>
  // static void KNNBinary(Node* T, Point const& q,
  //                       kBoundedQueue<Point, Range>& bq, Box const&
  //                       node_box, KNNLogger& logger)
  //   requires std::same_as<typename Interior::ST, HyperPlane>;
  //
  // template <typename Leaf, IsBinaryNode Interior, typename Range>
  // static void KNNBinaryBox(Node* T, Point const& q,
  //                          kBoundedQueue<Point, Range>& bq, KNNLogger&
  //                          logger);
  //
  // // NOTE: search knn in the expanded multi node
  // template <typename Leaf, IsMultiNode Interior, typename Range>
  // static void KNNMultiExpand(Node* T, Point const& q, DimsType dim,
  //                            BucketType idx, kBoundedQueue<Point, Range>&
  //                            bq, Box const& node_box, KNNLogger& logger);
  //
  // // NOTE: search knn in the expanded multi node
  // template <typename Leaf, IsMultiNode Interior, typename Range>
  // static void KNNMultiExpandBox(Node* T, Point const& q, DimsType dim,
  //                               BucketType idx, kBoundedQueue<Point,
  //                               Range>& bq, KNNLogger& logger);
  //
  // // NOTE: search knn in the multi node
  // template <typename Leaf, IsMultiNode Interior, typename Range>
  // static void KNNMulti(Node* T, Point const& q, kBoundedQueue<Point,
  // Range>& bq,
  //                      KNNLogger& logger);
  //
  // // NOTE: search knn in the mix of binary and multi node
  // template <typename Leaf, IsBinaryNode BN, IsMultiNode MN, typename Range>
  // static void KNNMix(Node* T, Point const& q, DimsType dim, BucketType idx,
  //                    kBoundedQueue<Point, Range>& bq, Box const& node_box,
  //                    KNNLogger& logger);
  //
  // // NOTE: search knn in the cover node
  // // TODO: change type of interior
  // template <typename Leaf, IsDynamicNode Interior, typename Range>
  // static void KNNCover(Node* T, Point const& q, kBoundedQueue<Point,
  // Range>& bq,
  //                      KNNLogger& logger);
  //
  // // NOTE: range count stuffs
  // template <typename Leaf>
  // static size_t RangeCountRectangleLeaf(Node* T, Box const& query_box);
  //
  // template <typename Leaf, IsBinaryNode Interior>
  // static size_t RangeCountRectangle(Node* T, Box const& query_box,
  //                                   Box const& node_box,
  //                                   RangeQueryLogger& logger);
  //
  // template <typename Leaf, IsMultiNode Interior>
  // static size_t RangeCountRectangle(Node* T, Box const& query_box,
  //                                   Box const& node_box, DimsType dim,
  //                                   BucketType idx, RangeQueryLogger&
  //                                   logger);
  //
  // // template <typename Leaf, IsBinaryNode Interior>
  // // static size_t RangeCountRadius(Node* T, NormalCircle const& cl,
  // //                                Box const& node_box);
  //
  // // NOTE: range query stuffs
  // template <typename Leaf, typename Range>
  // static void RangeQueryLeaf(Node* T, Range Out, size_t& s,
  //                            Box const& query_box);
  //
  // template <typename Leaf, IsBinaryNode Interior, typename Range>
  // static void RangeQuerySerialRecursive(Node* T, Range Out, size_t& s,
  //                                       Box const& query_box,
  //                                       Box const& node_box,
  //                                       RangeQueryLogger& logger);
  //
  // // template <typename Leaf, IsMultiNode Interior>
  // // static size_t RangeCountRadius(Node* T, NormalCircle const& cl,
  // //                                Box const& node_box);
  //
  // // NOTE: range query stuffs
  // template <typename Leaf, IsMultiNode Interior, typename Range>
  // static void RangeQuerySerialRecursive(Node* T, Range Out, size_t& s,
  //                                       Box const& query_box,
  //                                       Box const& node_box, DimsType dim,
  //                                       BucketType idx,
  //                                       RangeQueryLogger& logger);
  //
  // NOTE: utility
  // TODO: better evaluate the parallel recursion function
  template<typename Range, bool granularity = true>
  static void FlattenRec(Node* T, Range Out);

  template<typename Range>
  static void ExtractPointsInLeaf(Node* T, Range Out);

  // template <typename Leaf, IsMultiNode Interior, typename Range,
  //           bool granularity = true>
  // static void PartialFlatten(Node* T, Range Out, BucketType idx);
  //
  // // NOTE: expand a multi node into a binary node
  // template <IsBinaryNode BN, IsMultiNode MN>
  // static Node* ExpandMultiNode(typename MN::ST const& split, BucketType
  // idx,
  //                              BucketType deep,
  //                              parlay::sequence<Node*> const& tree_nodes);
  //
  // // NOTE: expand a multi-way tree into a binary tree
  // template <IsBinaryNode BN, IsMultiNode MN>
  // static Node* Expand2Binary(Node* T)
  //   requires std::same_as<typename BN::ST, typename MN::ST::value_type>;
  //
  // // NOTE: compress several level of bianry node into a multi node
  // template <IsBinaryNode BN, IsMultiNode MN>
  // static void CompressBinaryNode(Node* bn_proto,
  //                                typename MN::NodeArr& tree_nodes,
  //                                typename MN::ST& split, BucketType idx);
  //
  // // NOTE: compress a binary tree into a multi-way tree
  // template <IsBinaryNode BN, IsMultiNode MN>
  // static Node* Compress2Multi(Node* T)
  //   requires std::same_as<typename BN::ST, typename MN::ST::value_type>;
  //
  // NOTE: validations
  static TT::Box GetBox(TT::Box const& a, TT::Box const& b);

  static TT::Box GetBox(TT::EntrySeq const& ele_seq);

  static TT::Box GetBox(TT::BoxSeq const& box_seq);

  static TT::Box GetBox(Node const* T);

  TT::Box GetEmptyBox();

  template<typename Leaf, typename Interior>
  TT::Box VerifyTreeProperty(Node const* T);

  template<typename Leaf, typename Interior>
  int CheckHeight(Node const* T);

  template<typename Leaf, typename Interior>
  int GetHeight(Node const* T);

  template<typename Leaf, typename Interior>
  int GetTreeNodesNum(Node const* T);
  //
  // template <typename Leaf, typename Interior>
  // Points CheckCover(Node* T,
  //                   typename Interior::CircleType const&
  //                   level_cover_circle);
  //
  // template <typename Leaf, typename Interior>
  // static size_t CheckSize(Node* T);
  //
  // template <typename Leaf, typename Interior>
  // void CheckTreeSameSequential(Node* T, int dim);
  //
  template<typename Leaf, typename Interior>
  void Validate();

  auto Contain(K k) const -> std::pair<bool, MembershipLogger>;

  bool ContainRec(Node const* T, K k) const;
  //
  // template <typename Leaf, typename Interior>
  // size_t GetTreeHeight();
  //
  // template <typename Leaf, typename Interior>
  // size_t GetMaxTreeDepth(Node* T, size_t deep);
  //
  // template <typename Leaf, typename Interior>
  // double GetAveTreeHeight();
  //
  // template <typename Leaf, typename Interior>
  // size_t CountTreeNodesNum(Node* T);
  //
  // template <typename Leaf, typename Interior>
  // void CountTreeHeights(Node* T, size_t deep, size_t& idx,
  //                       parlay::sequence<size_t>& heights);
  //
  // // NOTE: param interfaces
  // void SetRoot(Node* root) { this->root_ = root; }
  //

  static auto ParseInput(TT::EntrySeq const& input_seq) -> InputRangeT
  {
    return input_seq;
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
};

} // namespace pmos

#include "b_tree_impl/batch_op/b_batch_delete.hpp"
#include "b_tree_impl/batch_op/b_batch_insert.hpp"
#include "b_tree_impl/batch_op/b_build_tree.hpp"
#include "b_tree_impl/batch_op/b_delete_tree.hpp"
#include "b_tree_impl/box_op.hpp"
#include "b_tree_impl/points_op.hpp"
#include "b_tree_impl/query_op/membership.hpp"
#include "b_tree_impl/tree_op/flatten.hpp"
#include "b_tree_impl/tree_op/leaf_op.hpp"
#include "b_tree_impl/tree_op/node_op.hpp"
#include "b_tree_impl/validation.hpp"

#endif // PMOS_B_TREE_H_
