# Plan: graft delete (reinsert valid subtrees, not entries)

Status: implemented on branch `graft-delete`. Idea from
`docs/new_delete_alg.md`. Code: `b_tree_impl/batch_op/b_batch_delete_graft.hpp`,
and the mark pass in `b_batch_delete.hpp` / `node_op.hpp`
(`batch_delete_update_leaf_parent`). Tests: `self_test` (the delete suites
now run the graft) plus `GraftHandShapes`, `GraftTombOverflow`,
`GraftHeapAllocator`. Faults injected and caught (2026-09-24):
shortest-first (`order`), keep right separator (random suites), no bound
raise (`raised`), no edge update (`edge`), wrong splice separator
(`GraftTombOverflow`). 2026-09-28 restructure: 10 of 10 caught (see
CLAUDE.md); the `loose` shapes cover loose entries.

Decisions (user):

- 2026-09-24: an invalid root is handled by today's code (consolidate,
  else rebuild). The graft starts where `repair_rec` started: a valid root
  with an invalid node below.
- 2026-09-24, then revised 2026-09-25: first a separate path behind a
  `delete_policy` trait option (`00e8c80`). On this branch the option and
  `repair_rec` / `repair_node` / `balance_over_children` are gone; compare
  against `dev`. The separate settle pass is folded into the mark pass, so
  the graft needs no extra traversal to get its flags.
- 2026-09-25 (user, updated `docs/new_delete_alg.md`): no reinsertion per
  tomb and no allocation inside the recursion. One global pool of items,
  preallocated before the traversal, put back in one pass. Per-height
  rounds were considered: bottom-up strands keys (q, P, v) and both orders
  rebuild upper nodes once per round, so the pass is a single descent that
  places same-level items before shorter ones route and rebuilds each node
  once.
- 2026-09-28 (user): "use the implementation in `dev_delete` and optimise
  it". `dev_delete` (`9e34f95`): box and height augmentation, a separate
  `MarkValidSubtrees` pass, `FetchValidSubtreeAndInvalidLeaves` into
  preallocated arrays (entries already sorted), `parlay::sort` of the
  wrappers by (height desc, box mid), per-height `BatchInsertNodesRecursive`
  tallest first, then `BatchInsert_` of the entries. Its defects: n scratch
  slots (a target node can emit more parts than it got items), asserts the
  root never splits, allocates `node_seq`/`split_seq` at every target
  node, leaf bounds from a scan when unsorted. Taken: preallocated
  collection from per-node counts, entries collected sorted, batch-insert
  routing. Changed: inputs written in key order so nothing is sorted, one
  descent, sizes by delta. The per-height rounds are kept as a probe
  variant for timing (`build/scratch_graft/probe`, `rounds`).

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

1. **Same-level items before shorter ones.** Shortest-first breaks: X
   holds, in key order, leaf q, leaf-parent P, leaf v. The left
   neighbour's last leaf-parent L covers X's range, so q and v go into L;
   P then goes after L and v is stranded inside L. The single descent
   merges a node's own-level items into its child list before routing
   anything shorter, so v routes into P. (In per-height rounds this means
   tallest first; the left-edge rounds still work that way.)
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
   the half has 17 and splits; T 17. In the single descent T is just a
   node on the way down: its split goes up through the scratch like any
   other, up to the root (`assemble_interior_levels`).
5. **Offsets.** Each node stores how many items it emits, in
   `reinsert_size`, set by the mark pass.

## Phases

- Mark (`batch_delete_recursive`), bottom-up: a leaf-parent balances its
  underfull leaves (total > 0, >= 2 leaves), then sets `valid`,
  `contain_invalid_subtree` and the item count from what is left
  (`batch_delete_update_leaf_parent`); an interior takes them from its
  children: their counts, plus its valid children when it is invalid
  (`batch_delete_update_interior_size`). All-empty leaf-parents are left
  whole.
- Root checks as before; the invalid root goes to consolidate / rebuild.
- `graft_repair`, once per delete:
  1. `graft_detach_rec(root)`: every invalid subtree into one pool sized by
     the root's count; kept nodes compacted (left-absorb, raised bounds);
     the root's raised bound is the global left edge.
  2. The pool is already in key order: items plus the loose entries of
     underfull leaves (a raised item is moved behind what was cut below
     its bound). Per-height position lists by a stable counting sort; one
     scratch of 2 slots per input.
  3. Inputs left of all content (rare): tallest-first rounds along the
     leftmost path with the edge (`graft_rec`), then their entries.
  4. `graft_all_rec` from the root: a node without items one level below
     routes as batch insert; `graft_join` merges items into a node's child
     list, shorter inputs route by the merged list, children in parallel,
     node rebuilt once; sizes by delta; overflow goes up through the
     scratch; a root split adds levels (`assemble_interior_levels`).
- Collapse single-child roots, now after every delete.

Unchanged: batch insert, `balance_over_leaves`.

## Invariants

- After the mark, every leaf of a valid leaf-parent holds >= `k_buf_lb`.
- Detach keeps every valid node whole (its >= `k_buf_lb` valid children
  stay); items are valid subtrees.
- Grafting only adds children; splits make 2B..3B. No node changes height.
- Sizes: kept + items + loose entries = size after marking.
- The pool is strictly increasing in key (asserted).
- Between operations every interior has default deletion flags: the detach
  resets every node it keeps, invalid nodes are freed, new nodes start
  clean. Checked 2026-09-28 by an assert-only copy (walk the tree after
  every delete) over the delete tests.

## Steps

0. (Done, then dropped) trait option beside the repair path.
1. Mark-pass balancing and counts, detach/decompose into one pool, the
   single descent.
2. Tests, against `std::map`: existing delete suites; the
   q/P/v shape (fault: shortest-first); T's first children gone, at the
   global left edge and inside (fault: keep the right separator); a valid
   item whose first child went (fault: no raise); T overflow; std::greater,
   augmentation, leak, heap allocator; `-DDEBUG -UNDEBUG`, ASAN/UBSan.
3. Benchmark ABBA against `dev` at 1e9, uni/norm/seg x 4 ratios. `build/`
   is a DEBUG build: use a release build directory.

## Results (2026-09-28, 1e9, `b_test -t 128`)

Full table in CLAUDE.md (Graft). In short, chain seconds at 0.01%:

| | seg | uni | norm |
|---|---|---|---|
| `dev` repair | 6.98 | 76.47 | 55.09 |
| `dev` + touched-only mark | 7.04 | 40.46 | 36.18 |
| graft + touched-only mark (this branch) | 6.59 | 39.45 | 35.55 |

- The graft's fix-up is 5-13x cheaper than `repair_rec` (0.16-0.39 s vs
  ~2 s per chain), but the mark pass is 80-90% of a uni/norm chain, so
  the algorithm moves the total by 1-2.5% (6% on seg; -8% on seg 0.1%).
- The mark pass read every child's size at each touched node; reading
  only the touched ones halves uni 0.01% on both branches. `dev` does not
  have it yet: port it before comparing the two fix-ups.
- Per-height rounds (`dev_delete`'s order) match the single descent.

Next, by measured cost: the batch sort (3.3-3.7 s per 0.01% chain; keys
are u32, a radix sort would do); leaf balancing (half of it `std::sort` of
a merged leaf); balancing on the mark's critical path (seg 0.1%);
`batch_insert_update_interior` still sums every child.

Later, optional: the invalid root through the same pool (assemble the
tallest group, graft the rest).
