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

// Tells the tree how to read your entry. The tree calls these directly --
// take the entry by reference or by value, whichever you prefer.
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

**Deleting a key that is not there does nothing.** `batch_delete` and
`erase(key)` skip keys the tree does not hold, like `std::set::erase`. The
batch may name absent keys, may be longer than the tree, and may repeat a
delete; only the keys actually present come out. `erase(key)` returns whether
it was there.

**`range_report`'s output must be large enough.** It writes
`range_count(box)` entries and does not check `out.size()`. A smaller `out`
overruns; debug builds assert.

**Batch updates need transient memory proportional to the batch.**
`batch_insert` allocates a sorted copy of the input plus `32 * n` bytes of
scratch — `2n` slots of `pair<Node*, key>`, one pair per input element per
side of a possible split. About 40 bytes per entry in flight, both released
when the call returns. At n = 1e9 that is roughly 40 GB, so size the batch to
the machine. `build` and `batch_delete` need only the sorted copy; the delete
path allocates scratch just when a rebalance has to reinsert, sized by the
reinserted entries rather than by the batch.

The `2n` is not slack that can be trimmed: slots `2i` and `2i+1` belong to
input element `i`, and that fixed mapping is what lets the parallel recursion
hand each child a provably disjoint sub-range without coordinating. Shrinking
the buffer would index out of bounds.

**The tree owns its nodes.** `get_root()` returns a non-owning observer.
Lifetime ends at `delete_tree()` or the destructor.

## Key ordering

`btree_type_trait`'s sixth parameter is the key comparator, defaulting to
`std::less<K>`. Every ordering decision in the tree goes through it — the
descent, the leaf searches, the batch partitioners, all the sorts, the range
boxes and `validate()`. Nothing compares keys with a raw operator.

```cpp
using descending = pab_tree::btree_type_trait<
        my_ele, 64,
        pab_tree::empty_leaf_aug, pab_tree::empty_interior_aug,
        pab_tree::proto_node,
        std::greater<uint64_t> >;      // the tree is ordered high to low
```

Iteration, `to_seq`, `lower_bound`/`upper_bound` and the range boxes all follow
the comparator, so a `range_count({a, b})` on a descending tree takes `a`
greater than `b`.

The comparator must be stateless and default-constructible; it is instantiated
where it is used rather than stored, so the tree stays 8 bytes. It costs
nothing: with `std::less` the generated code is instruction-for-instruction
what the hard-coded operators produced.

Equality is derived as `!less(a,b) && !less(b,a)`, which is two comparisons.
`std::less` is special-cased to one, and any comparator supplying a static
`equal(a, b)` gets the same treatment:

```cpp
struct my_cmp {
        bool operator()(K const &a, K const &b) const { return ...; }
        static bool equal(K const &a, K const &b) { return ...; }  // optional
};
```

One caveat: `validate()` derives the least and greatest key from the numeric
limits of `K`, so a comparator that leaves neither limit extreme (ordering by a
hash, say) needs those bounds supplied before `validate()` will agree with it.
Ordinary orders — `std::less`, `std::greater`, any monotone transform — are
fine.

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

## Node base

`btree_type_trait`'s fifth parameter is the common base of both node types,
defaulting to `pab_tree::proto_node`. It is constrained by the `node_base`
concept, so a type that will not work is rejected at your declaration rather
than deep inside a header:

```cpp
template <typename T>
concept node_base =
        std::constructible_from<T, bool, size_t> && requires(T t, size_t n) {
                { t.is_leaf } -> std::convertible_to<bool>;
                { t.size }    -> std::convertible_to<size_t>;
                t.size = n;
        };
```

`is_leaf` picks the downcast between leaf and interior; `size` is the subtree
entry count, which the range queries read without descending and which
rebalancing writes. Carry extra fields if you want them — they are present on
every node the tree builds:

```cpp
struct tagged_node {
        tagged_node() : is_leaf(false), size(0), tag(0) {}
        tagged_node(bool l, size_t s) : is_leaf(l), size(s), tag(0xAB) {}
        bool is_leaf;
        size_t size;
        unsigned tag;          // yours
};
```

A plain aggregate works too — C++20 parenthesized aggregate initialization
means `struct { bool is_leaf; size_t size; };` is constructible from
`(bool, size_t)` with no constructor written.

## Node allocation

`btree_type_trait`'s seventh parameter is the node allocator, defaulting to
`pab_tree::parlay_node_alloc`, which is `parlay::type_allocator` — a pool.
Every node the tree takes or releases goes through it; nothing names an
allocator directly.

```cpp
struct my_node_alloc {
        template <typename T> static T *alloc();       // raw storage
        template <typename T> static void free(T *p);  // after the dtor
};

using tt = pab_tree::btree_type_trait<
        my_ele, 64,
        pab_tree::empty_leaf_aug, pab_tree::empty_interior_aug,
        pab_tree::proto_node, std::less<uint64_t>,
        my_node_alloc>;
```

`alloc<T>` returns raw storage of the right size and alignment for `T`; the
tree placement-news into it. `free<T>` is called after the destructor has run.
Both must be callable from several threads at once, since the batch operations
build nodes in parallel. Keep it pooled: the tree allocates one node per leaf
and per interior, so a general-purpose allocator is visible immediately.

The indirection is free. Against the default the generated code is identical —
same instruction count, same instruction mix.

**The tree owns every node it allocates.** `get_root()` hands back a
non-owning observer; ownership ends at `clear()`/`delete_tree()` or the
destructor, which release every node through the same allocator.

## Tests

Two suites. Both are built with `-UNDEBUG`, so the library's internal asserts
are live, and both are registered with ctest.

```bash
ctest --test-dir build          # runs both
```

**`self_test` — needs no data.** Generates its own keys from a seed and
checks every operation against `std::map`: build, batch insert, batch delete,
membership, range count, range report, ordered iteration, the lookup family,
augmentation, and value semantics. Sweeps leaf sizes 2…256 and five key
shapes, and covers every size where the structure changes shape. About 7
seconds.

```bash
cmake --build build --target self_test
./build/self_test                    # fixed seed, printed on startup
./build/self_test --seed=12345       # reproduce a reported failure
./build/self_test --rounds=20        # more shapes per size, for fuzzing
```

It is deterministic: the same seed reproduces a run exactly. Every failure
prints the leaf size, input size, key shape and seed, plus the flag to rerun
just that case.

**`unit_test` — reads the benchmark datasets** under `/ssd1/zmen002/kv_pair`.
Without them every test skips, so a green run means nothing on a machine that
lacks the data. Keep it for checking against real distributions at scale.

```bash
cmake --build build --target unit_test
./build/unit_test --gtest_filter='*1M*'
```

## Benchmarks

`b_test` is the benchmark driver. `scripts/` holds the sweeps and the data
generators. See `CLAUDE.md` for the tag numbering and dataset layout.

## License

MIT, see `LICENSE`.
