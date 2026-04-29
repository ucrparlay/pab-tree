#ifndef PBTREE_DEPENDENCE_LOGGERS_H
#define PBTREE_DEPENDENCE_LOGGERS_H

#include <cstddef>

namespace pb_tree {

struct MembershipLogger
{
  size_t vis_leaf_num = 0;
  size_t vis_interior_num = 0;
};

struct RangeReportLogger
{
  size_t vis_leaf_num = 0;
  size_t vis_interior_num = 0;
  size_t flatten_node_num = 0;
};

struct KNNLogger
{
  size_t vis_leaf_num = 0;
  size_t vis_interior_num = 0;
  size_t generate_box_num = 0;
  size_t check_box_num = 0;
  size_t skip_box_num = 0;
};

struct RangeQueryLogger
{
  size_t vis_leaf_num = 0;
  size_t vis_interior_num = 0;
  size_t generate_box_num = 0;
  size_t full_box_num = 0;
  size_t skip_box_num = 0;
};

} // namespace pb_tree

#endif // PBTREE_DEPENDENCE_LOGGERS_H_
