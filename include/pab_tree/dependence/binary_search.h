#ifndef PABTREE_BINARY_SEARCH_H_
#define PABTREE_BINARY_SEARCH_H_

#include <cstddef>

namespace pab_tree::detail
{

// Legacy linear-scan helpers (kept for callers outside the hot path).
template <typename Seq, typename F>
size_t linear_search(Seq const &I, typename Seq::value_type const &v,
					 const F &less)
{
	for (size_t i = 0; i < I.size(); i++) {
		if (!less(I[i], v)) {
			return i;
		}
	}
	return I.size();
}

template <typename Seq, typename F>
size_t linear_search(Seq const &I, const F &less)
{
	for (size_t i = 0; i < I.size(); i++) {
		if (!less(I[i])) {
			return i;
		}
	}
	return I.size();
}

// Return the index of the first element where !less(elem, v).
//
// Binary phase: branchless cmov-friendly ternaries narrow the range to
// ≤ BinarySearchBase elements without a single branch.
// Linear phase: branchless popcount-style scan of the remaining elements —
// counts how many still satisfy less() rather than early-exiting, eliminating
// branch mispredictions in the tail at the cost of a bounded number of extra
// comparisons (≤ BinarySearchBase/2 on average).
template <size_t BinarySearchBase, typename Seq, typename F>
size_t binary_search(Seq const &I, typename Seq::value_type const &v,
					 const F &less)
{
	size_t lo = 0, hi = I.size();
	while (hi - lo > BinarySearchBase) {
		size_t const mid = lo + ((hi - lo) >> 1);
		bool const go = less(I[mid], v);
		lo = go ? mid + 1 : lo;
		hi = go ? hi : mid;
	}
	size_t count = 0;
	for (size_t i = lo; i < hi; ++i) {
		count += less(I[i], v) ? 1 : 0;
	}
	return lo + count;
}

// Return the index of the first element where !less(elem).
template <size_t BinarySearchBase, typename Seq, typename F>
size_t binary_search(Seq const &I, const F &less)
{
	size_t lo = 0, hi = I.size();
	while (hi - lo > BinarySearchBase) {
		size_t const mid = lo + ((hi - lo) >> 1);
		bool const go = less(I[mid]);
		lo = go ? mid + 1 : lo;
		hi = go ? hi : mid;
	}
	size_t count = 0;
	for (size_t i = lo; i < hi; ++i) {
		count += less(I[i]) ? 1 : 0;
	}
	return lo + count;
}

} // namespace pab_tree::detail

#endif // PABTREE_BINARY_SEARCH_H_
