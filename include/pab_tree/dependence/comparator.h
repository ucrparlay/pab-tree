#ifndef PABTREE_DEPENDENCE_COMPARATOR_H_
#define PABTREE_DEPENDENCE_COMPARATOR_H_

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>

namespace pab_tree
{
//*----------- double precision comparision ----------------
//* God made the integers, all else is the work of man.
//* -- Leopold Kronecker
template <typename T>
class numeric_comparator {
public:
	static bool gt(T const a, T const b)
	{
		if constexpr (std::is_floating_point_v<T>) {
			return a - b > eps;
		} else {
			return a > b;
		}
	}

	static bool lt(T const a, T const b)
	{
		if constexpr (std::is_floating_point_v<T>) {
			return a - b < -eps;
		} else {
			return a < b;
		}
	}

	static bool eq(T const a, T const b)
	{
		if constexpr (std::is_floating_point_v<T>) {
			return std::abs(a - b) < eps;
		} else {
			return a == b;
		}
	}

	static T min(T const a, T const b)
	{
		return lt(a, b) ? a : b;
	}

	static T max(T const a, T const b)
	{
		return gt(a, b) ? a : b;
	}

	static T abs(T const a)
	{
		return lt(a, 0) ? -a : a;
	}

	static T max_val()
	{
		return std::numeric_limits<T>::max();
	}

	static T min_val()
	{
		return std::numeric_limits<T>::lowest();
	}

private:
	static constexpr T eps = std::numeric_limits<T>::epsilon();
};

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_COMPARATOR_H_
