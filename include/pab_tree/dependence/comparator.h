#ifndef PABTREE_DEPENDENCE_COMPARATOR_H_
#define PABTREE_DEPENDENCE_COMPARATOR_H_

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>

namespace pab_tree {
//*----------- double precision comparision ----------------
//* God made the integers, all else is the work of man.
//* -- Leopold Kronecker
template<typename T>
class Num_Comparator
{
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

  static bool geq(T const a, T const b)
  {
    return !lt(a, b);
  }

  static bool leq(T const a, T const b)
  {
    return !gt(a, b);
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

  static bool is_zero(T const a)
  {
    return eq(a, static_cast<T>(0));
  }

  static T divide_two_ceil(T const a)
  {
    if constexpr (std::is_floating_point_v<T>) {
      return std::ceil(a / 2.0);
    } else {
      return (a + 1) / 2;
    }
  }

  static T integer_log2_upper(T const a)
  {
    if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(std::ceil(std::log2(a)));
    } else {
      if (a == 0)
        return 0; // BUG: this is a bug, should return -1
      T r = 0, b = a;
      while (b >>= 1)
        r++;
      return r + static_cast<T>((1 << r) != a);
    }
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
