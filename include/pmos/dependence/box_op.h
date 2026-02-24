#ifndef PMOS_DEPENDENCE_BOX_OP_HPP_
#define PMOS_DEPENDENCE_BOX_OP_HPP_

#include "dependence/type_trait.h"
#include "utility"

namespace pmos {
template<typename TT>
TT::Box
GetEmptyBox()
{
  return typename TT::Box(TT::Num::MaxVal(), TT::Num::MinVal());
}

template<typename TT>
TT::Box
GetBox(typename TT::Box const& a, typename TT::Box const& b)
{
  return typename TT::Box(TT::Num::Min(a.first, b.first),
                          TT::Num::Max(a.second, b.second));
}

// TODO: this can be parallelized (needs to define the min/max of a box)
template<typename TT>
TT::Box
GetBox(typename TT::ConstEntrySlice ele_slice)
{
  auto box = std::make_pair(TT::Entry::GetKey(ele_slice[0]),
                            TT::Entry::GetKey(ele_slice[0]));
  for (typename TT::BufSizeT i = 1; i < ele_slice.size(); i++) {
    auto key = TT::Entry::GetKey(ele_slice[i]);
    box.first = TT::Num::Min(box.first, key);
    box.second = TT::Num::Max(box.second, key);
  }
  return box;
}

template<typename TT>
TT::Box
GetBox(typename TT::EntrySeq const& ele_seq)
{
  return GetBox<TT>(parlay::make_slice(ele_seq));
}

template<typename TT>
TT::Box
GetBox(typename TT::BoxSeq const& box_seq)
{
  auto box = box_seq[0];
  for (int i = 1; i < box_seq.size(); i++) {
    box.first = TT::Num::Min(box.first, box_seq[i].first);
    box.second = TT::Num::Max(box.second, box_seq[i].second);
  }
  return box;
}
} // namespace pmos

#endif // PMOS_DEPENDENCE_BOX_OP_HPP_
