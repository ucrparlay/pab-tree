#ifndef PMOS_BINARY_SEARCH_H_
#define PMOS_BINARY_SEARCH_H_

#include <cstddef>

namespace pmos {

// the following parameter can be tuned
template<typename Seq, typename F>
size_t
LinearSearch(Seq const& I, typename Seq::value_type const& v, const F& less)
{
  for (size_t i = 0; i < I.size(); i++)
    if (!less(I[i], v))
      return i;
  return I.size();
}

// return index to first key greater or equal to v
template<size_t _binary_search_base, typename Seq, typename F>
size_t
BinarySearch(Seq const& I, typename Seq::value_type const& v, const F& less)
{
  size_t start = 0;
  size_t end = I.size();
  while (end - start > _binary_search_base) {
    size_t mid = (end + start) / 2;
    if (!less(I[mid], v)) {
      end = mid;
    } else {
      start = mid + 1;
    }
  }
  return start + pmos::LinearSearch(make_slice(I).cut(start, end), v, less);
}

template<typename Seq, typename F>
size_t
LinearSearch(Seq const& I, const F& less)
{
  for (size_t i = 0; i < I.size(); i++)
    if (!less(I[i]))
      return i;
  return I.size();
}

// return index to first key where less is false
template<size_t _binary_search_base, typename Seq, typename F>
size_t
BinarySearch(Seq const& I, const F& less)
{
  size_t start = 0;
  size_t end = I.size();
  while (end - start > _binary_search_base) {
    size_t mid = (end + start) / 2;
    if (!less(I[mid])) {
      end = mid;
    } else {
      start = mid + 1;
    }
  }
  return start + pmos::LinearSearch(make_slice(I).cut(start, end), less);
}

} // namespace pmos

#endif // PMOS_BINARY_SEARCH_H_
