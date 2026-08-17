# Open issues after the refactor

Everything still wrong in `include/pab_tree/` as of 2026-08-17, after the
31-commit refactor. Each entry was re-checked against the current code, so the
paths, names and line numbers are live, not pre-refactor.

Ordered by what I would fix next. The full history is in
`docs/findings_register.md` (all 112 findings, fixed and open) and
`docs/code_review_2026-08.md` (the original report).

**Not here:** everything the refactor fixed. See `git log 376e067..HEAD` and
`docs/findings_register.md`.

**Resolved after the fact:** the reported use-after-free in
`reinsert_tree_after_delete` was not one. `assert(new_node_n == 0)` states a
real invariant: the reinserted entries came out of `T`'s own subtree, which
already held them in at most `k_buf_ub` children, so putting them back cannot
overflow `T`. Measured against it — a reinsert batch of 13,626 entries at
`k_buf_ub = 8`, 1,700x capacity — and `T` never overflowed, at any leaf size.
The check is now unconditional rather than assert-only, so the invariant is
enforced in release too, but it is a guard, not a fix for a live defect.

---

## 1. Batch operations require a parlay range

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

## 2. No standard container conveniences

`b_tree.h`

No `size()`, `empty()`, `clear()`, `swap()`, no iteration, no single-key
insert or erase. Defensible for a batch-parallel design, but `empty()` and
`clear()` cost nothing and `size()` is a one-line alias for `get_size()`.

---

## 3. No RAII and no exception guarantee

Nodes are raw pointers from `parlay::type_allocator` with manual free. A
throwing allocation mid-insert leaves the tree in an indeterminate state and
leaks every node built so far. Structural; not attempted.

---

## 4. Eleven TODO markers

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
