#ifndef PMOS_B_TREE_H_
#define PMOS_B_TREE_H_

#include <cstdint>

#include "dependence/comparator.h"
#include "dependence/loggers.h"
#include "dependence/tree_node.h"
#include "dependence/type_trait.h"

namespace pmos {
// Template definition for the operator
template <typename T1, typename T2>
std::ostream& operator<<(std::ostream& os, std::pair<T1, T2> const& p) {
  os << "(" << p.first << ", " << p.second << ")";
  return os;
}

// NOTE: Basetree
template <class TypeTrait>
class BTree {
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
  using NodeSeqList = parlay::sequence<typename TT::NodeSeq>;
  using SplitSeqList = parlay::sequence<typename TT::SplitSeq>;

  using LeafAug = typename TT::LeafAug;
  using InteriorAug = typename TT::InteriorAug;

  // NOTE: Const variables
  // NOTE: uint32t handle up to 4e9 at least
  // WARN: bucket num should smaller than 1<<8 to handle type overflow
  using BucketType = typename TT::BucketType;
  static constexpr BucketType const kBuildDepthOnce = TT::kSkeletonHeight;
  static constexpr BucketType const kPivotNum = TT::kPivotNum;
  static constexpr BucketType const kBucketNum = TT::kBucketNum;
  static constexpr uint_fast8_t const kLog2Base = TT::kLog2Base;
  static constexpr uint_fast16_t const kBlockSize = TT::kBlockSize;

  using BufferSizeType = TT::BufferSizeType;

  // NOTE: tree structure
  static constexpr uint_fast16_t const kSerialBuildCutoff = 1 << 10;

  struct BTreeInteriorNode : MultiNode<TT> {
    using TT = TT;
    using BaseNode = MultiNode<TT>;
    using BTreeNodeArr = typename BaseNode::NodeArr;
    using BTreeNodeArrSlice = typename BaseNode::NodeArrSlice;

    BTreeInteriorNode(BTreeNodeArr const& _tree_nodes,
                      TT::SplitSeq const& _split)
        : BaseNode(_tree_nodes, _split, typename TT::InteriorAug()) {
    }  // TODO:finish AT

    BTreeInteriorNode(BTreeNodeArrSlice _tree_nodes_slice,
                      TT::SplitSlice _split_slice)
        : BaseNode(_tree_nodes_slice, _split_slice,
                   typename TT::InteriorAug()) {}  // TODO:finish AT

    BTreeInteriorNode(BTreeNodeArr const& _tree_nodes,
                      TT::SplitSeq const& _split, TT::InteriorAug const& _aug)
        : BaseNode(_tree_nodes, _split, _aug) {}

    inline void SetParallelFlag(bool const flag) {
      this->aug.SetParallelFlag(flag);
    }

    inline void ResetParallelFlag() { this->aug.ResetParallelFlag(); }

    inline bool GetParallelFlagIniStatus() {
      return this->aug.GetParallelFlagIniStatus();
    }

    inline bool ForceParallel() const {
      return this->aug.ForceParallel(this->size, kSerialBuildCutoff);
    }

    auto UpdateAug(BTreeNodeArr const& tree_nodes, TT::SplitSeq const& splits) {
      return this->aug.Update(tree_nodes, splits);
    }
  };

  using Leaf = LeafNode<TT, parlay::move_assign_tag>;
  using Interior = BTreeInteriorNode;

  // NOTE: array based inner tree for batch insertion and deletion
  template <typename Leaf, typename Interior>
  struct InnerTree;

  static size_t GetBktNum(size_t const item_n) { return item_n / TT::kBufMD; }

  static size_t GetBktStIdx(size_t const i, size_t const bkt_n,
                            size_t const n) {
    return (n * i) / bkt_n;
  };

  static size_t GetBktEndIdx(size_t const i, size_t const bkt_n,
                             size_t const n) {
    return (n * (i + 1)) / bkt_n;
  };

  // NOTE: build tree
  template <typename Range>
  void Build(Range&& In);

  void Build_(EntrySlice In);

  Node* BuildRecursive(EntrySlice In);

  template <typename Range>
  void BatchInsert(Range&& In);

  void BatchInsert_(EntrySlice In);

  NodeSplitSeqPair BatchInsertRecursive(Node* T, EntrySlice In, EntrySlice Out);

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
  static inline Node* UpdateInterior(Node* T, NodeSeqList const& node_seq_list,
                                     SplitSeqList const& split_seq_list,
                                     size_t new_node_n);

  static size_t GenerateLeafNodes(TT::EntrySlice key_in, TT::SplitSlice key_out,
                                  TT::NodeSlice node_out);

  static size_t GenerateInteriorNodes(TT::NodeSlice node_in,
                                      TT::NodeSlice node_out,
                                      TT::SplitSlice key_in,
                                      TT::SplitSlice key_out);
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
  // // NOTE: batch insert
  static Node* InsertPoints2Leaf(Node* T, EntrySlice In);

  static NodeSplitSeqPair HandlingLeafSplitOverflow(Node* T, EntrySlice In);

  static NodeSplitSeqPair HandlingInteriorSplitOverflow(
      Node* T, NodeSeqList const& node_seq_list,
      SplitSeqList const& split_seq_list);

  //
  // template <typename Leaf, typename RT>
  // static RT DeletePoints4Leaf(Node* T, Slice In);
  //
  // template <typename Leaf, typename RT>
  // static RT DiffPoints4Leaf(Node* T, Slice In);
  //
  // template <IsBinaryNode Interior>
  // static inline BucketType RetriveTag(Point const& p, NodeTagSeq const&
  // tags);
  //
  static inline BucketType RetriveTag(E const e, TT::SplitSeq const& split_seq);
  //
  static TT::BallSeq SeievePoints(EntrySlice A, EntrySlice B,
                                  TT::SplitSeq const& split_seq);
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
  template <bool granularity>
  inline static bool ForceParallelRecursion(Interior const* T);

  void DeleteTree();

  template <bool granularity = true>
  static void DeleteTreeRecursive(Node* T);

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
  // static inline double P2CMinDistance(Point const& p, CircleType const& cl);
  //
  // static inline DisType InterruptibleDistance(Point const& p, Point const& q,
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
  //                       kBoundedQueue<Point, Range>& bq, Box const& node_box,
  //                       KNNLogger& logger)
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
  //                            BucketType idx, kBoundedQueue<Point, Range>& bq,
  //                            Box const& node_box, KNNLogger& logger);
  //
  // // NOTE: search knn in the expanded multi node
  // template <typename Leaf, IsMultiNode Interior, typename Range>
  // static void KNNMultiExpandBox(Node* T, Point const& q, DimsType dim,
  //                               BucketType idx, kBoundedQueue<Point, Range>&
  //                               bq, KNNLogger& logger);
  //
  // // NOTE: search knn in the multi node
  // template <typename Leaf, IsMultiNode Interior, typename Range>
  // static void KNNMulti(Node* T, Point const& q, kBoundedQueue<Point, Range>&
  // bq,
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
  // static void KNNCover(Node* T, Point const& q, kBoundedQueue<Point, Range>&
  // bq,
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
  template <typename Range, bool granularity = true>
  static void FlattenRec(Node* T, Range Out);

  template <typename Range>
  static void ExtractPointsInLeaf(Node* T, Range Out);

  // template <typename Leaf, IsMultiNode Interior, typename Range,
  //           bool granularity = true>
  // static void PartialFlatten(Node* T, Range Out, BucketType idx);
  //
  // // NOTE: expand a multi node into a binary node
  // template <IsBinaryNode BN, IsMultiNode MN>
  // static Node* ExpandMultiNode(typename MN::ST const& split, BucketType idx,
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

  static TT::Box GetBox(Node* T);

  TT::Box GetEmptyBox();

  template <typename Leaf, typename Interior>
  TT::Box VerifyTreeProperty(Node const* T);

  template <typename Leaf, typename Interior>
  int CheckHeight(Node const* T);
  //
  // template <typename Leaf, typename Interior>
  // Points CheckCover(Node* T,
  //                   typename Interior::CircleType const& level_cover_circle);
  //
  // template <typename Leaf, typename Interior>
  // static size_t CheckSize(Node* T);
  //
  // template <typename Leaf, typename Interior>
  // void CheckTreeSameSequential(Node* T, int dim);
  //
  template <typename Leaf, typename Interior>
  void Validate();
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
  Node* GetRoot() { return this->root_; }

  size_t GetSize() { return this->root_ ? this->root_->size : 0; }
  //
  // Box GetRootBox() { return this->tree_box_; }
  //
  // consteval static auto GetBuildDepthOnce() {
  //   return static_cast<int>(kBuildDepthOnce);
  // }

 protected:
  Node* root_ = nullptr;
};

}  // namespace pmos

#include "b_tree_impl/batch_op/b_batch_insert.hpp"
#include "b_tree_impl/batch_op/b_build_tree.hpp"
#include "b_tree_impl/batch_op/b_delete_tree.hpp"
#include "b_tree_impl/box_op.hpp"
#include "b_tree_impl/points_op.hpp"
#include "b_tree_impl/tree_op/flatten.hpp"
#include "b_tree_impl/tree_op/leaf_op.hpp"
#include "b_tree_impl/tree_op/node_op.hpp"
#include "b_tree_impl/validation.hpp"

#endif  // PMOS_B_TREE_H_
