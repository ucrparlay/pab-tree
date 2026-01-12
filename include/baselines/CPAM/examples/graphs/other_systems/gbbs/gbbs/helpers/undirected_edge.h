#pragma once

#include <ostream>
#include <utility>

#include "gbbs/macros.h"

namespace gbbs {

class UndirectedEdge;

}  // namespace gbbs

namespace std {

template <>
struct hash<gbbs::UndirectedEdge> {
  size_t operator()(gbbs::UndirectedEdge const& edge) const;
};

}  // namespace std

namespace gbbs {

class UndirectedEdge;

// Represents an unweighted, undirected edge in a graph.
// This has the property that `UndirectedEdge({u, v}) == Undirected({v, u})`, so
// it can be used for, e.g., storing and retrieving edges from a hash table
// without worrying about the direction of the edge.
class UndirectedEdge {
 public:
  UndirectedEdge(uintE u, uintE v);
  explicit UndirectedEdge(std::pair<uintE, uintE> const& edge);

  bool operator==(UndirectedEdge const& other) const;
  bool operator!=(UndirectedEdge const& other) const;
  bool operator<(UndirectedEdge const& other) const;

  // Returns the two vertices that the edge connects.
  std::pair<uintE, uintE> const& endpoints() const;

 private:
  friend size_t std::hash<UndirectedEdge>::operator()(
      UndirectedEdge const&) const;

  std::pair<uintE, uintE> edge_;
};

std::ostream& operator<<(std::ostream& os, UndirectedEdge const&);

}  // namespace gbbs
