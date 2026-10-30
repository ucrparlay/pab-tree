# Open issues after the refactor

Everything still wrong in `include/pab_tree/` as of 2026-08-17, after the
35-commit refactor. Each entry was re-checked against the current code, so the
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

## 1. No exception guarantee

The tree itself is RAII — copy/move constructor, copy/move assignment and
destructor are all there, so a `btree` owns and releases its nodes. What is
missing is the guarantee *during* an operation: nodes are raw pointers from
`parlay::type_allocator` with manual free, so a throwing allocation mid-insert
leaves the tree in an indeterminate state and leaks every node built so far.
Structural; not attempted.

---

## 2. Five TODO markers and one HACK

`b_tree.h:32,718` · `tree_node.h:178,369` · `points_op.hpp:53` ·
`node_op.hpp:126`

Design notes, not defects. Five ideas someone had — whether the ~25 `using`
re-exports in `btree` are needed, tuning when `flatten_rec` goes parallel,
picking the batch-split strategy from input skew, moving `update_aug` into the
aug type, and templating the alloc helpers.

The `HACK` at `node_op.hpp:126` is load-bearing and should stay. It marks the
`static_cast<int>` that keeps `return_node_n_arr[i] - 1` from wrapping when the
count is 0, since `buf_size_type` is unsigned. The code is right; the comment
is what keeps it right.

Cleared: the two `TODO:finish AT` on the interior constructors (every
allocation site calls `update_aug()` on the next line), a commented-out block
in `tree_node.h`, "this can be parallelized" in `box_op.h` (every caller passes
at most `k_buf_ub` entries), and an explicit-tag note in `b_batch_insert.hpp`.

Also dead, and left in place: the 3-arg `alloc_interior_node(nodes, split,
aug)` and the matching `btree_interior_node` constructor have no callers.

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

**~~No dataset-free test.~~ Fixed.** `tests/self_test.cpp` generates its own
keys from a seed and checks every operation against `std::map`. 14 tests, ~7
seconds, no data files. `unit_test` is unchanged and still reads
`/ssd1/zmen002/kv_pair`; both are registered with ctest.

Verified by fault injection rather than by passing: dropping an `update_aug`
on the delete path, an off-by-one in the leaf delete compaction, skipping a
covered child in `range_count`, and an iterator that fails to sort the leaf it
lands on are all caught. The last of those found a real hole in the test
itself -- `check_against` called `to_seq()` before iterating, and `to_seq`
sorts every leaf, which hid the bug. Iteration now runs first.

**R1 and R2 share one commit.** The rename was gated on its own first, but the
changes interleave across 43 files, so `3c2f28e` is not bisectable between
them.

**One existing assert was removed.** `assert(root_->size == 0)` in
`b_batch_delete.hpp`, which asserted exactly what the data-loss fix disproves
and fired on every tree the new branch recovers.
