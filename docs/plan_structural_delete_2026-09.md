# Plan: structural rebalance for batch delete (bottom-up sibling merge)

Status: implemented 2026-09-12 as `repair_rec` / `repair_node` /
`balance_over_children` (`node_op.hpp`); `fetch_invalid_subtree_nodes`,
`reinsert_tree_after_delete` and `prune_out_empty_tree` are gone. Written
as a separate function rather than templating `balance_over_leaves`, so the
leaf path is untouched. The invalid-root consolidation
(`consolidate_invalid_root`, 2026-09-12) stays as the degenerate case, since
it regenerates the levels with build's fan-out.

Result (2026-09-20, 1e9, `-t 128 -r 3`, ABBA x2 vs `c937b45`): 0.01%
35.15 → 32.61 s, 0.1% 14.54 → 13.09 s, 1% 5.98 → 5.55 s, 10% 1.96 →
1.81 s. Step 0's answer: the repair phase was 4.0 / 2.8 / 0.9 / 1.25 s of
wall time per chain and is now 1.1 / 0.57 / 0.21 / 0.07 s; the old path
reinserted up to 1.27 B entries per chain.

Skewed data gains more (2026-09-21, same protocol): `norm` 28.62 / 13.60 /
5.48 / 2.64 → 24.43 / 10.77 / 3.79 / 1.77 s; `seg` 10.59 / 4.67 / 3.13 /
2.67 → 7.63 / 2.54 / 1.59 / 1.38 s. Sizes correct after every batch.

Next candidate: on `norm` the consolidation guard still fails twice and
falls back to the full rebuild (200 M entries, 0.29 s of the 1.8 s 10%
chain). `repair_rec` could take the invalid root as well and retire both
the guard and `rebuild_from_flatten` on that path.

## Context

After `batch_delete_recursive` has marked nodes, `fetch_invalid_subtree_nodes`
repairs the tree from the nearest valid ancestor down:

- a valid leaf-parent merges its underfull leaves in place
  (`balance_over_leaves`, `node_op.hpp:432`);
- a valid higher node drops its invalid children, **flattens their whole
  subtrees into entries** (`node_op.hpp:298`), frees them, and batch-inserts
  the entries into the surviving siblings (`reinsert_tree_after_delete`,
  `node_op.hpp:702`).

The second step costs a full entry copy per invalid subtree, a partition and
descent per reinsert, leaf splits and sorts on the way in, and a `2n`
node/split scratch buffer. Most of those entries sit in leaves that were
still fine; only the structure above them was thin.

Measured share (per-batch probe, 1e9, `-t 128`): everything under
`fetch_invalid_subtree_nodes` is 10% of the 0.01% chain, 16% at 0.1%, 4% at
1%, 8% at 10%. The flatten-and-reinsert part of that is **not measured
separately**. That is step 0.

## Idea

Repair bottom-up, one level at a time, merging *pointers* between siblings
and moving entries only at the leaf level:

1. **Leaves.** Every leaf-parent runs `balance_over_leaves` on its own
   array, including leaf-parents with no valid leaf (the fallback added in
   2026-09). Afterwards every leaf is valid; some leaf-parents have too few.
2. **Each interior level, going up.** Under one parent, group the thin
   children with the nearest valid sibling to their right (trailing thin
   ones with the rightmost valid), exactly `balance_over_leaves` phase 1 on
   child pointers. Per group: concatenate the child arrays into the pivot
   (`pivot.tree_nodes = A.tree_nodes ++ B.tree_nodes ...`, separators
   `A.split ++ [parent split between A and B] ++ B.split`), or, if the
   total exceeds `k_buf_ub`, re-bucket the concatenated (nodes, keys) list
   with `generate_interior_nodes` and give the parent the new nodes. Thin
   sources are freed.
3. **Seam fix.** A merged array may now hold a thin node that came from a
   source with no valid child (a thin chain). It sits next to valid
   neighbours for the first time, so run the same grouping on that array,
   one level down, along the seam only: pointer moves, and at the bottom
   one `balance_over_leaves` where an underfull leaf meets real leaves.
4. **Stop** at the nearest valid ancestor: it has `k_buf_lb` valid children
   to absorb into, so every level below it ends valid. No valid ancestor
   means the root is invalid: `consolidate_invalid_root` (regenerate the
   levels).

Heights never change: a node only ever absorbs siblings at its own level.
Entries move once, only out of underfull leaves; no flatten, no batch
insert, no `2n` scratch.

## What changes

- `balance_over_leaves`' phase 1 and dispatch become a level-agnostic
  template, `balance_over_children<NodeKind>`: leaves move entries, interiors
  move `tree_nodes`/`split` slices. Cases: single (slide), merge (append
  arrays into the pivot, inherit the outer split), distribute
  (`generate_interior_nodes` on the group's flat list; reuse the group's
  node objects where possible, free the surplus).
- `fetch_invalid_subtree_nodes`' generic branch (`node_op.hpp:336-430`):
  replace "compact, flatten, `reinsert_tree_after_delete`" with "recurse into
  children first, then `balance_over_children` on this node, then the seam
  fix for absorbed thin chains". `has_tomb`/`next_tomb` go away: there is no
  ancestor reinsert any more, so every leaf-parent balances itself.
- `reinsert_tree_after_delete`, `flatten_rec<…, Sorted=true>` on this path,
  and the `2n` `node_split_pair_seq` in the delete path are deleted.
- `batch_delete_update_interior_size` is unchanged: flags still drive which
  nodes are visited; the sizes are recomputed on the way up.

## Parallel structure

Same fork-join as today: `parallel_for` over children (grain 1 when the
subtree is large, `k_buf_ub` otherwise), then the node's own merge after the
join. A node's arrays are written only by the task owning its parent, after
the child tasks joined; seam fixes run inside that task. No locks. Work
O(entries in underfull leaves + nodes touched); span O(height · k_buf_ub²).

## Invariants to prove and test

- After level ℓ, every node at level ℓ is valid: merge gives
  `>= k_buf_lb` children (pivot alone had that many), distribute gives
  `[k_buf_md, 3·k_buf_lb]`.
- A parent's fan-out never exceeds what it had: one thin child removed, at
  most one node added per split. So overflow never propagates (the argument
  `open_issues.md:14-21` makes for the reinsert).
- Separators: concatenation keeps them strictly increasing; the outer split
  between two merged siblings is the parent's stored one.
- Uniform depth: no node changes level.
- Augmentation: `update_aug` on every node whose array changed.

## Steps

0. **Instrument first.** Split `fetch_invalid_subtree_nodes` time into
   leaf balance / flatten / reinsert with the session's `probe_patch.py`
   pattern; run the four ratios at 1e9. Go on only if flatten+reinsert is a
   worthwhile share.
1. Extract `balance_over_children` from `balance_over_leaves`; keep the
   leaf instantiation byte-for-byte equivalent (self_test, then the codegen
   diff recipe in CLAUDE.md).
2. Interior instantiation + the seam fix; wire into
   `fetch_invalid_subtree_nodes`; delete the reinsert path.
3. Tests, all against `std::map`: thin chains of every height (build
   sequential keys, delete a contiguous run that empties k whole subtrees
   plus every key not divisible by 8 in the neighbours); tiny groups that
   need the seam fix (leaf-parents left with 1..k_buf_lb-1 entries); overflow
   splits (sum > k_buf_ub); `std::greater`; augmentation; allocator
   leak/double-free; the 10% chain; `-DDEBUG -UNDEBUG` and ASAN builds.
4. Benchmark ABBA against HEAD at 1e9, all four ratios, plus `unit_test`
   data (`-b 0.01`, `-t 3`).

## Risks

- `balance_over_leaves` is the most invariant-dense function in the tree
  (direct-write compaction, `tree_idx <= i`); templating it is the risky
  edit. Keep the leaf instantiation identical and prove it by codegen.
- Distribute at interior levels changes node identities under the parent;
  the parent's `size`/aug must be recomputed after, as
  `batch_insert_update_interior` does.
- Gain is bounded by the reinsert share; if step 0 says under ~5%, stop.
