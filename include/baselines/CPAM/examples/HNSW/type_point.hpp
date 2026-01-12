#ifndef __TYPE_POINT_HPP__
#define __TYPE_POINT_HPP__

#include <cstdint>
#include <iterator>

#include "h5_ops.hpp"
#include "util/benchUtils.h"

struct fvec {
  uint32_t id;
  float* coord;
};

std::pair<parlay::sequence<fvec>, uint32_t> ps_from_SIFT(char const* file) {
  static auto to_fvec = [](size_t id, auto begin, auto end) {
    typedef
        typename std::iterator_traits<decltype(begin)>::value_type type_elem;
    if constexpr (std::is_same_v<decltype(begin),
                                 ptr_mapped<type_elem, ptr_mapped_src::DISK>>) {
      auto const *begin_raw = begin.get(), *end_raw = end.get();
      auto const n = std::distance(begin_raw, end_raw);

      // auto coord = std::make_unique<type_elem[]>(n);
      type_elem* coord = new type_elem[n];
      parlay::parallel_for(0, n,
                           [&](size_t i) { coord[i] = *(begin_raw + i); });

      fvec point;
      point.id = id;
      point.coord = std::move(coord);
      return point;
    }
  };

  return parse_vecs<float>(file, to_fvec);
}

std::pair<parlay::sequence<fvec>, uint32_t> ps_from_HDF5(char const* file,
                                                         char const* dir) {
  auto [buffer_ptr, bound] = read_array_from_HDF5<float>(file, dir);
  auto const dim = bound[1];
  auto* buffer = buffer_ptr.release();

  parlay::sequence<fvec> ps(bound[0]);
  parlay::parallel_for(0, bound[0],
                       [&](uint32_t i) { ps[i] = fvec{i, &buffer[i * dim]}; });
  return {ps, dim};
}
#endif  // __TYPE_POINT_HPP_
