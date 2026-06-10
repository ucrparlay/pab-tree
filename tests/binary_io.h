#pragma once

// Binary ".bin" dataset format (little-endian):
//   uint64_t header[3] = { N, dim, width }   width = bytes per value, 4 or 8
//   N*dim values of `width` bytes each, row-major (key, val...).
// Writers automatically narrow to 4-byte values when every value fits in
// uint32_t; readers widen back to uint64_t transparently.

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"

inline void
WriteBinaryEntries(std::string const& path,
                   uint64_t n,
                   uint64_t dim,
                   parlay::sequence<uint64_t> const& payload)
{
  assert(payload.size() == n * dim);
  uint64_t const max_v =
    payload.empty() ? 0 : parlay::reduce(payload, parlay::maxm<uint64_t>());
  uint64_t const width = max_v <= std::numeric_limits<uint32_t>::max()
                           ? sizeof(uint32_t)
                           : sizeof(uint64_t);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    throw std::runtime_error("Unable to open output file: " + path);
  }
  uint64_t const header[3] = { n, dim, width };
  out.write(reinterpret_cast<char const*>(header), sizeof(header));
  if (width == sizeof(uint32_t)) {
    auto narrowed = parlay::tabulate(payload.size(), [&](size_t i) {
      return static_cast<uint32_t>(payload[i]);
    });
    out.write(reinterpret_cast<char const*>(narrowed.data()),
              static_cast<std::streamsize>(narrowed.size() *
                                           sizeof(uint32_t)));
  } else {
    out.write(reinterpret_cast<char const*>(payload.data()),
              static_cast<std::streamsize>(payload.size() *
                                           sizeof(uint64_t)));
  }
  out.close();
}

// Returns (N, dim, values) with 4-byte payloads widened back to uint64_t.
inline auto
ReadBinaryEntries(char const* path)
  -> std::tuple<uint64_t, uint64_t, parlay::sequence<uint64_t>>
{
  std::ifstream fs(path, std::ios::binary);
  if (!fs.is_open()) {
    std::cout << "Unable to open file: " << path << std::endl;
    abort();
  }

  uint64_t header[3];
  fs.read(reinterpret_cast<char*>(header), sizeof(header));
  uint64_t const n = header[0];
  uint64_t const dim = header[1];
  uint64_t const width = header[2];
  if (!fs.good() || (width != sizeof(uint32_t) && width != sizeof(uint64_t))) {
    std::cout << "Bad binary header in " << path << " (width = " << width
              << "); regenerate the .bin file." << std::endl;
    abort();
  }

  auto const payload_bytes = static_cast<std::streamsize>(n * dim * width);
  parlay::sequence<uint64_t> data;
  if (width == sizeof(uint64_t)) {
    data = parlay::sequence<uint64_t>::uninitialized(n * dim);
    fs.read(reinterpret_cast<char*>(data.data()), payload_bytes);
  } else {
    auto narrowed = parlay::sequence<uint32_t>::uninitialized(n * dim);
    fs.read(reinterpret_cast<char*>(narrowed.data()), payload_bytes);
    data = parlay::tabulate(narrowed.size(), [&](size_t i) {
      return static_cast<uint64_t>(narrowed[i]);
    });
  }
  if (fs.gcount() != payload_bytes) {
    std::cout << "Truncated binary file: " << path << std::endl;
    abort();
  }
  return { n, dim, std::move(data) };
}
