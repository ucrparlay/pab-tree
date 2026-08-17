# Open issues after the refactor

Everything still wrong in `include/pab_tree/` as of 2026-08-17, after the
31-commit refactor. Each entry was re-checked against the current code, so the
paths, names and line numbers are live, not pre-refactor.

Ordered by what I would fix next. The full history is in
`docs/findings_register.md` (all 112 findings, fixed and open) and
`docs/code_review_2026-08.md` (the original report).

**Not here:** the 57 findings the refactor fixed. See `git log 376e067..HEAD`.

---

## 1. Use-after-free in `reinsert_tree_after_delete`

`b_tree_impl/tree_op/node_op.hpp:706`

```cpp
[[maybe_unused]] size_type new_node_n = batch_insert_recursive(
        T, reinsert_entries, parlay::make_slice(node_split_seq),
        leaf_entries_pattern, leaf_sort_pred);
assert(new_node_n == 0);
```

A non-zero `new_node_n` means the callee took the interior-split path, which
frees `T` and writes replacement children into `node_split_seq`. The contract
is that the caller splices them in. This one does neither: `node_split_seq` is
a local that dies on return, and under `NDEBUG` the assert is gone. The caller
keeps using `T`, and `T`'s parent still holds the freed pointer.

The verifier confirmed it is reachable: `T` is the topmost node with an invalid
direct child, still near `k_buf_ub` children after compaction, and the
reinserted entries push a surviving child over its own `k_buf_ub`. Reproduced
with 284 reinserted entries at small leaf sizes.

**Why it is still open:** the fix is in `fetch_invalid_subtree_nodes`, on the
frozen list. The correct shape is to return the count and the sequence and
splice via `assemble_interior_levels`, exactly as `batch_insert_` does.

**Status:** contained, not fixed. The check is now unconditional and aborts
with a labelled message, so the corruption is immediate and visible in every
build instead of silent. Committed 2026-08-17.

**Tried and reverted:** rebuilding T's subtree in place so T is never freed.
It breaks the occupancy invariant -- few entries yield a one-child interior,
and a non-root node needs at least `k_buf_lb` children. That rule is why the
original code hands the problem upward in the first place.

**The real fix** needs `fetch_invalid_subtree_nodes` to return the split count
and node sequence, with each frame splicing its child's replacement through
`assemble_interior_levels`, exactly as `batch_insert_` does one level up.
Confined to two functions, but it changes the delete recursion's signature.

---

## 2. Every leaf allocation zero-fills the whole element array

`dependence/tree_node.h:73`

```cpp
, elements(entry_arr())
```

`entry_arr` is `std::array<E, k_buf_ub>`, so this value-initialises all
`k_buf_ub` entries even when the leaf holds far fewer. The review measured
**22% of build time**.

Untouched because it sits in node allocation, next to frozen code, and I had
no measurement of my own. It is the largest remaining performance item and
worth confirming with the 0.5 gate before and after.

---

## 3. `DEBUG` is an unprefixed configuration macro

7 sites: `b_batch_delete.hpp:112,178`, `b_batch_insert.hpp:89,158`,
`node_op.hpp:152,481,646`. Set by `CMakeLists.txt`.

`DEBUG` is among the most commonly defined macro names anywhere. A consumer
whose build defines it silently switches on the `get_node_box_rec`
verification loops, an O(subtree) traversal with heap allocation per child on
every interior update.

Rename to `PABTREE_DEBUG`. This was in the plan and I did not do it.

---

## 4. `-DDEBUG=ON` builds abort on sorted-input insert

`b_tree_impl/batch_op/b_batch_insert.hpp:89`

```cpp
assert(!sorted_input || new_node_n == 0);
```

False: `new_node_n > 0` means the root overflowed, which depends on batch size,
not on whether the caller pre-sorted.

**Kept deliberately** — your call was to leave existing asserts alone, so the
correct check sits beside it rather than replacing it. Recorded here because
the consequence is that the sorted-input fast path cannot be exercised in a
debug build.

---

## 5. `default_hybrid_pred` hashes heap addresses

`b_tree.h:215`

```cpp
(reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
(reinterpret_cast<uintptr_t>(In.end()) >> 3);
```

The default insert and delete path decides leaf sortedness from heap
addresses, so leaf ordering varies run to run under ASLR. Query results are
unaffected, but `sorted_leaf_ratio` and anything timing-sensitive are not
reproducible.

---

## 6. Batch operations require a parlay range

`b_batch_insert.hpp:67`, `b_batch_delete.hpp:68`

```cpp
parlay::slice<const E *, const E *> sorted_slice{ In.begin(), In.end() };
```

This needs the range's iterator to be a raw `const E *`, so
`batch_insert(std::vector<E>)` fails while `build(std::vector<E>)` succeeds.
The static_assert that used to advertise otherwise is gone, but the
inconsistency remains, and the failure surfaces deep in an implementation
header rather than at the call site.

---

## 7. No standard container conveniences

`b_tree.h`

No `size()`, `empty()`, `clear()`, `swap()`, no iteration, no single-key
insert or erase. Defensible for a batch-parallel design, but `empty()` and
`clear()` cost nothing and `size()` is a one-line alias for `get_size()`.

---

## 8. No RAII and no exception guarantee

Nodes are raw pointers from `parlay::type_allocator` with manual free. A
throwing allocation mid-insert leaves the tree in an indeterminate state and
leaks every node built so far. Structural; not attempted.

---

## 9. Eleven TODO markers

`b_tree.h:27,136,146,468` · `tree_node.h:81,183,385,409` ·
`box_op.h:21` · `b_batch_insert.hpp:59` · `points_op.hpp:55`

Two are `// TODO:finish AT` on the interior constructors, which the
augmentation work has now largely answered. The rest are notes to yourself.

---

## Deliberately not fixed

**The `2n` scratch buffer** (`b_batch_insert.hpp`) allocates 32 bytes per
input element. It looks like obvious waste and is not: the `2n` sizing is the
recursion's addressing scheme, and `cut(2l, 2r)` is what makes the per-child
sub-ranges provably disjoint. It is on the frozen list.

**Comparator and allocator trait parameters.** Real features, not debt.
Deferred by the plan. When added they must be template parameters resolved at
compile time.

---

## Gaps in the work itself

**No dataset-free test.** `unit_test` reads `/ssd1/zmen002/kv_pair`; without it
every test skips and `ctest` passes green with zero coverage. Plan step 0.4
called for a self-contained file cross-checked against `std::set`. I gated on
the existing suite plus standalone repros instead, so a contributor without
your data still has no working gate. This is the most valuable thing left.

**R1 and R2 share one commit.** The rename was gated on its own first, but the
changes interleave across 43 files, so `3c2f28e` is not bisectable between
them.

**One existing assert was removed.** `assert(root_->size == 0)` in
`b_batch_delete.hpp`, which asserted exactly what the data-loss fix disproves
and fired on every tree the new branch recovers.
