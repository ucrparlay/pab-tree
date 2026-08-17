# PaB-Tree

A parallel, cache-oblivious in-memory B+-tree. Header-only, C++20.

Supports parallel batch insert, batch delete, build, membership, range count
and range report. Parallelism comes from
[ParlayLib](https://github.com/cmuparlay/parlaylib).

## Requirements

- CMake >= 3.15
- A C++20 compiler (GCC or Clang) on Linux
- ParlayLib, provided as a submodule

Optional: [jemalloc](https://github.com/jemalloc/jemalloc) (`-DJEMA=ON`,
the default) and [numactl](https://manpages.ubuntu.com/manpages/trusty/man8/numactl.8.html).

## Build

```bash
git submodule update --init --recursive
cmake -B build -DJEMA=OFF .
cmake --build build -j
```

`-DJEMA=OFF` unless jemalloc is installed; the default is `ON` and the
configure step then requires it.

## Use it

```bash
cmake --install build --prefix /your/prefix
```

```cmake
find_package(pab_tree REQUIRED)
target_link_libraries(your_app PRIVATE pab_tree::pab_tree)
```

## Example

Everything a user writes:

```cpp
#include "pab_tree/b_tree.h"

struct entry {
        uint64_t key, val;
};

// Tells the tree how to read your entry.
struct my_ele {
        using E = entry;
        using K = uint64_t;
        using V = uint64_t;
        static K get_key(E const &e) { return e.key; }
        static V get_val(E const &e) { return e.val; }
        static E to_entry(K const &k, V const &v) { return E{ k, v }; }
};

using tt = pab_tree::btree_type_trait<my_ele, 64>;   // 64 = leaf size
using tree = pab_tree::btree<tt>;

int main()
{
        parlay::sequence<entry> data(1000);
        for (uint64_t i = 0; i < 1000; i++)
                data[i] = entry{ i, i };

        tree t;
        t.build(data);

        bool found = t.contain(500);
        size_t n = t.range_count({ 100, 200 });

        parlay::sequence<entry> out(n);
        t.range_report({ 100, 200 }, parlay::make_slice(out));

        parlay::sequence<uint64_t> del{ 1, 2, 3 };
        t.batch_delete(del);
}
```

## API

The batch calls are the ones worth using; everything is built to run a whole
batch in parallel.

| Call | Notes |
|---|---|
| `build(range)` | Replaces any existing contents |
| `batch_insert(range)` | Entries |
| `batch_delete(range)` | Keys, not entries |
| `contain(key) -> bool` | |
| `range_count(box) -> size_t` | `box` is half-open, `[first, second)` |
| `range_report(box, out) -> size_t` | See the sizing rule below |
| `get_entries(out)` | Every entry; `out` must hold `get_size()`. Not in key order |
| `get_size()`, `validate()`, `delete_tree()` | |

Any random-access range works for `build`, `batch_insert` and `batch_delete` —
`std::vector`, `std::array`, `std::span`, `parlay::sequence`. Contiguous ones
are read in place.

All queries are `const`. `validate()` is `[[nodiscard]]` and returns whether
the tree's invariants hold.

### Standard container interface

The names `std::set` and `tlx::btree_set` use also work:

| Group | Members |
|---|---|
| capacity | `size` `empty` `max_size` `clear` |
| lookup | `contains` `count` `find` `lower_bound` `upper_bound` `equal_range` |
| iteration | `begin` `end` `cbegin` `cend` `rbegin` `rend` `to_seq` |
| modifiers | `bulk_load` `insert` `erase` (range, single entry/key, or iterator) |
| compare | `==` `!=` `<` `<=` `>` `>=` |
| observers | `key_comp` `value_comp` |
| types | `key_type` `mapped_type` `value_type` `size_type` `iterator` … |

```cpp
tree t(data);                        // build from any range
for (auto const &e : t) { … }        // key order
auto it = t.lower_bound(100);
t.erase(it, t.upper_bound(200));     // one batch_delete underneath
if (t.contains(500)) { … }
```

Single-element `insert`/`erase` go through the batch path, so a loop over them
is far slower than one batch call. `insert` and `erase` take a range when you
give them one.

**Iteration writes to the tree.** Leaves hold their entries unsorted on
purpose, so the leaf under the cursor gets sorted as the iterator reaches it.
That makes iteration and lookup unsafe to run from several threads at once.
Call `sort_leaves()` first — one parallel pass, idempotent — and afterwards
both only read.

## Rules

**Keys must be distinct.** This is a set. Duplicates put entries on the wrong
side of a split key where no descent reaches them, so they become invisible to
`contain` and the range queries while `get_size()` still counts them. Debug
builds assert on it.

**`range_report`'s output must be large enough.** It writes
`range_count(box)` entries and does not check `out.size()`. A smaller `out`
overruns; debug builds assert.

**The tree owns its nodes.** `get_root()` returns a non-owning observer.
Lifetime ends at `delete_tree()` or the destructor.

## Augmentation

`btree_type_trait` takes two optional augmentation types. They default to
empty, and an empty augmentation compiles away entirely.

```cpp
struct sum_leaf_aug {
        uint64_t sum = 0;
        sum_leaf_aug() = default;
        template <typename Slice> sum_leaf_aug(Slice in) { update_aug(in); }
        template <typename Slice> void update_aug(Slice in)
        {
                sum = 0;
                for (auto const &e : in)
                        sum += e.key;
        }
        void reset() { sum = 0; }
};

struct sum_interior_aug {
        uint64_t sum = 0;
        sum_interior_aug() = default;
        template <typename Nodes, typename Splits>
        void update(Nodes const &nodes, Splits const &);   // sum the children
        static bool force_parallel(size_t, size_t) { return true; }
        void reset() { sum = 0; }
};

using tt = pab_tree::btree_type_trait<my_ele, 64,
                                      sum_leaf_aug, sum_interior_aug>;
```

The contracts are concepts in `dependence/concepts.h`, so a type that does not
satisfy one fails at your declaration naming the missing requirement.

## Tests

```bash
cmake --build build --target unit_test
ctest --test-dir build
```

`unit_test` reads datasets under `/ssd1/zmen002/kv_pair`. Without them every
test skips, so a green run means nothing on a machine that lacks the data.
It is built with `-UNDEBUG` so the library's internal asserts are live.

## Benchmarks

`b_test` is the benchmark driver. `scripts/` holds the sweeps and the data
generators. See `CLAUDE.md` for the tag numbering and dataset layout.

## License

MIT, see `LICENSE`.
