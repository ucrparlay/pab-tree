#ifndef PMOS_B_TREE_H_
#define PMOS_B_TREE_H_

#include <cstdint>

#include "dependence/comparator.h"
#include "dependence/loggers.h"
#include "dependence/tree_node.h"
#include "dependence/type_trait.h"

namespace pmos {

// NOTE: Basetree
template <class TypeTrait, uint kBufferSize>
class BTree {
 public:
  using E = typename TypeTrait::EleType;
  using K = typename E::KeyType;
  using V = typename E::ValType;

  using Num = Num_Comparator<K>;
  using Slice = parlay::slice<E*, E*>;
  using EleSeq = parlay::sequence<E>;
  using EleIter = typename parlay::sequence<E>::iterator;
  using LeafAug = typename TypeTrait::LeafAug;
  using InteriorAug = typename TypeTrait::InteriorAug;

  // NOTE: Const variables
  // NOTE: uint32t handle up to 4e9 at least
  // WARN: bucket num should smaller than 1<<8 to handle type overflow
  using BucketType = typename TypeTrait::BucketType;
  static constexpr BucketType const kBuildDepthOnce =
      TypeTrait::kSkeletonHeight;
  static constexpr BucketType const kPivotNum = TypeTrait::kPivotNum;
  static constexpr BucketType const kBucketNum = TypeTrait::kBucketNum;
  static constexpr uint_fast8_t const kLog2Base = TypeTrait::kLog2Base;
  static constexpr uint_fast16_t const kBlockSize = TypeTrait::kBlockSize;

  // NOTE: tree structure
  static constexpr uint_fast8_t const kLeaveWrap = 32;
  static constexpr uint_fast8_t const kThinLeaveWrap = 24;
  static constexpr uint_fast8_t const kSlimLeaveWrap = 8;
  static constexpr uint_fast16_t const kSerialBuildCutoff = 1 << 10;

  using BufferSizeType =
      std::conditional_t<(kBufferSize > 255u), uint_fast16_t, uint_fast8_t>;
  static constexpr BufferSizeType kBuf = kBufferSize;
  static constexpr BufferSizeType kBufLB = kBufferSize;
  static constexpr BufferSizeType kBufUB = 4 * kBufferSize;

  // NOTE: array based inner tree for batch insertion and deletion
  template <typename Leaf, typename Interior>
  struct InnerTree;

  template <typename Range>
  void Build(Range&& In);

  void Build_(Slice In);

  // NOTE: build tree
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
  // template <IsMultiNode Interior, bool UpdateParFlag = true>
  // static inline void UpdateInterior(
  //     Node* T, typename Interior::NodeArr const& new_nodes);
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
  // template <typename Leaf>
  // static Node* InsertPoints2Leaf(Node* T, Slice In);
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
  // template <IsMultiNode Interior>
  // static inline BucketType RetriveTag(Point const& p, NodeTagSeq const&
  // tags);
  //
  // template <typename Interior>
  // static void SeievePoints(Slice A, Slice B, size_t const n,
  //                          NodeTagSeq const& tags,
  //                          parlay::sequence<BallsType>& sums,
  //                          BucketType const tags_num);
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
  // inline static bool ForceParallelRecursion(Interior const* T);
  //
  // constexpr virtual void DeleteTree() = 0;
  //
  // template <typename Leaf, typename Interior>
  // void DeleteTreeWrapper();
  //
  // template <typename Leaf, IsBinaryNode Interior, bool granularity = true>
  // static void DeleteTreeRecursive(Node* T);
  //
  // template <typename Leaf, IsMultiNode Interior, bool granularity = true>
  // static void DeleteTreeRecursive(Node* T);
  //
  // template <typename Leaf, IsDynamicNode Interior, bool granularity = true>
  // static void DeleteTreeRecursive(Node* T);
  //
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
  // // NOTE: utility
  // // TODO: better evaluate the parallel recursion function
  // template <typename Leaf, IsBinaryNode Interior, typename Range,
  //           bool granularity = true>
  // static void FlattenRec(Node* T, Range Out);
  //
  // template <typename Leaf, typename Interior, typename Range,
  //           bool granularity = true>
  // static void FlattenRec(Node* T, Range Out)
  //   requires(!IsBinaryNode<Interior>);
  //
  // template <typename Leaf, typename Range>
  // static void ExtractPointsInLeaf(Node* T, Range Out);
  //
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
  // // NOTE: validations
  // template <typename Leaf, typename Interior>
  // Box CheckBox(Node* T, Box const& box);
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
  // template <typename Leaf, typename Interior, typename SplitRule>
  // void Validate();
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
  // Node* GetRoot() { return this->root_; }
  //
  // size_t GetSize() { return this->root_ ? this->root_->size : 0; }
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

#endif  // PMOS_B_TREE_H_
