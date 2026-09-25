# Plan: graft delete (reinsert valid subtrees, not entries)

Status: implemented on branch `graft-delete` (2026-09-24); only sanity
timings so far (CLAUDE.md: within a few percent of repair on uni). Idea
from `docs/new_delete_alg.md`. Code:
`b_tree_impl/batch_op/b_batch_delete_graft.hpp`; trait option in
`type_trait.h`; `b_test_graft` target. Tests: `self_test` `Graft*` (46/46
overall at `-O1 -UNDEBUG`; `Graft*` under `-DDEBUG -UNDEBUG`; ASAN/UBSan
clean). Faults injected and caught: shortest-first (`order`), keep right
separator (random suites), no bound raise (`raised`), no edge update
(`edge`), wrong splice separator (`GraftTombOverflow`). Repair-path
codegen of `b_test` unchanged (0-line normalised diff).

Decisions (user, 2026-09-24):

- A separate path, chosen per tree with the trait option `delete_policy`
  (`repair_delete`, the default, or `graft_delete`). `repair_rec`,
  `consolidate_invalid_root` and `rebuild_from_flatten` stay; the two
  paths are compared later.
- An invalid root is handled by today's code (consolidate, else rebuild).
  The graft path starts where `repair_rec` starts: a valid root with an
  invalid node below.

## Idea

`c937b45`'s reinsert path, with two changes:

1. Before validity is decided, every leaf-parent balances its underfull
   leaves (`balance_over_leaves`).
2. The tomb T (the highest valid node with an invalid child) does not
   flatten its invalid subtrees. It cuts out their largest valid subtrees
   and grafts each back whole, as a child of a node one level above the
   subtree's height. Only entries of leftover underfull leaves go through
   batch insert.

## Constraints the doc leaves open

1. **Tallest height first.** Shortest-first breaks: X holds, in key order,
   leaf q, leaf-parent P, leaf v. The left neighbour's last leaf-parent L
   covers X's range, so q and v go into L; P then goes after L and v is
   stranded inside L. With tallest-first, every node at height h in round
   h is an original node (splits from rounds >= h happen at h+1 and up),
   so none straddles an item.
2. **A removed child's key range goes to its left neighbour.** Drop the
   child with its left separator. A node whose first child went reports
   its new lower bound; the parent raises the separator left of it, up to
   T. Each item then goes right after the child its lower bound routes to,
   with that bound as the new separator. Items left of all of T's content
   (T's first children went) go in front along T's leftmost path; the
   bound used there is updated after each round. The one item at the
   global left edge has no bound; it takes its smallest key.
   Today's compaction keeps the right separator instead, which sends the
   range right, or left for a last child.
3. **A valid subtree inside an invalid one may hold invalid nodes.** They
   are detached first and join T's pool; the subtree's bound may rise (2).
4. **T can overflow.** `c937b45`'s no-split argument covers entries. With
   subtrees, B = 4 (at most 16 children): T has 16 children; child X has 3
   full valid children and 13 invalid ones holding 3 full valid children
   each. Round 1: X's full left neighbour c takes the 3, 19 children,
   splits; T 15 + 2 = 16. Round 2: the 39 lower items land on the last four
   children of c's right half (full; 12, 12, 12, 3) -> 3 + 3 + 3 + 2 nodes,
   the half has 17 and splits; T 17. So T's children live in a growable
   list while its rounds run, and T's parent splices the result, up to the
   root (`assemble_interior_levels`).
5. **Offsets.** Each node stores how many items it emits, in
   `reinsert_size` (unread by the repair path).

## Phases

After today's sort, mark (`batch_delete_recursive`) and root checks:

- `graft_settle_rec`, flagged nodes, bottom-up: balance underfull leaves
  (total > 0, >= 2 leaves); recompute `valid`, `contain_invalid_subtree`
  and the item count: children's counts, plus the valid children of an
  invalid node (an invalid leaf-parent emits all its leaves). Validity can
  only rise, so a valid root stays valid. All-empty leaf-parents are left
  whole.
- `graft_tomb_rec`: down to the tombs; splice what children return; split
  when over `k_buf_ub`.
- `graft_detach_rec` (valid node: returns its raised bound) and
  `graft_decompose_rec` (invalid node): items (node, bound, height) in key
  order; underfull leaves as entry sources; invalid interiors freed.
- `graft_reinsert` at T: group by height (stable); tallest first,
  `graft_rec` into T's children (partition by bound with `partition_batch`;
  interior levels via `batch_insert_update_interior` /
  `handling_interior_split_overflow`; the target via `graft_into_node`);
  then the loose entries, sorted, through `batch_insert_recursive` on T's
  children. 2 scratch slots per item, as in insert. End: write back, or
  regroup with `generate_interior_nodes` and return.
- Collapse single-child roots (today's loop).

Unchanged: batch insert, the mark pass, `balance_over_leaves`.

## Invariants

- After settle, every leaf of a valid leaf-parent holds >= `k_buf_lb`.
- Detach keeps every valid node whole (its >= `k_buf_lb` valid children
  stay); items are valid subtrees.
- Grafting only adds children; splits make 2B..3B. No node changes height.
- Sizes: kept + items + loose entries = size after marking.

## Steps

0. Trait option; `batch_delete_` dispatches with `if constexpr`. The
   repair instantiation must be unchanged (codegen diff).
1. Settle, detach/decompose, graft, T resolution, upward splice.
2. Tests, against `std::map`: existing delete suites under graft; the
   q/P/v shape (fault: shortest-first); T's first children gone, at the
   global left edge and inside (fault: keep the right separator); a valid
   item whose first child went (fault: no raise); T overflow; std::greater,
   augmentation, leak, heap allocator; `-DDEBUG -UNDEBUG`, ASAN/UBSan.
3. Benchmark ABBA against HEAD at 1e9, uni/norm/seg x 4 ratios. `build/`
   is a DEBUG build: use a release build directory.

Later, optional: the invalid root through the same pool (assemble the
tallest group, graft the rest); one descent for all heights.
