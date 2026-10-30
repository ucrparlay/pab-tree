# pab_tree — project knowledge

This file covers the tree. Harness, datasets, baselines and the
concurrent-index sweep live in `docs/harness_notes.md` (moved 2026-09-12).

## Working conventions (for Claude)

- Human-facing text (comments, commit messages, replies): as few words as
  possible. State the fact; no jargon, no paragraphs.
- Keep this file updated; it is the persistent knowledge store. Also read
  the `.claude/` memory files at session start.
- No need to run `unit_test` after edits; compilation must pass.
- **Never run a bare `git stash pop`** here (two long-lived `autostash`
  entries). To A/B against HEAD: `git archive <sha> | tar -x` into a scratch
  dir, symlink `external/{parlaylib,googletest,libmorton,fast-cpp-csv-parser,
  tlx}`, overlay the changed files.
- **zsh does not word-split unquoted variables** (`$FILES`, `set -- $pair`);
  a `rm -rf $S/src/$2` once ran with `$2` empty. Multi-step shell goes in a
  `#!/bin/bash` script; guard `rm` paths with `${v:?}`.
- Stop everything else before timing, even at nice 19: four single-core
  compiles slowed a 224-thread run 17% from batch 0.
- `/` (70 GB; holds `/tmp` and the scratchpad) fills up on this shared
  box: on 2026-10-14 it hit 0 bytes free and every compile died writing
  its temporaries. For big compiles set `TMPDIR` to a dir under `build/`
  (on `/home`).

## What it is

A parallel, cache-oblivious B+-tree, `pab_tree::btree<TT>`, header-only in
`include/pab_tree/`: parallel batch insert, batch delete, membership, range
count/report, plus a `std::set`-like interface. Keys are distinct (a set).

| path (under `include/pab_tree/`) | role |
|---|---|
| `b_tree.h` | class, aliases, node predicates; `#include`s every `b_tree_impl/**/*.hpp` at the bottom |
| `dependence/type_trait.h` | `btree_type_trait`, `btree_trait`, size constants, `default_options`, `plain_entry_adaptor` |
| `dependence/tree_node.h` | `leaf_node`, `multi_way_node`, alloc/free helpers |
| `dependence/{concepts,node_base,binary_search}.h` | concepts; `proto_node`, `parlay_node_alloc`; branchless `binary_search<Base>` |
| `b_tree_impl/batch_op/b_build_tree.hpp` | `build_`, `build_recursive`, `assemble_interior_levels` |
| `b_tree_impl/batch_op/b_batch_insert.hpp` | `batch_insert_`, `batch_insert_recursive` |
| `b_tree_impl/batch_op/b_serial_insert.hpp` | `serial_insert_`, `serial_insert_rec`, `serial_insert_leaf`, `serial_split_leaf`, `serial_split_interior` |
| `b_tree_impl/batch_op/b_serial_delete.hpp` | `serial_delete_`, `serial_delete_rec`, `serial_delete_leaf`, `serial_rebalance`, `serial_balance_leaves`, `serial_balance_interiors` |
| `b_tree_impl/batch_op/b_batch_delete.hpp` | `batch_delete_`, `batch_delete_recursive`, `rebuild_from_flatten`, `consolidate_invalid_root` |
| `b_tree_impl/batch_op/b_batch_delete_graft.hpp` | the fix-up below a valid root: `graft_repair`, `graft_all_rec`, `graft_detach_rec`, `graft_decompose_rec`, `graft_compact`, `graft_rec` / `graft_into_node` (left-edge rounds) |
| `b_tree_impl/tree_op/node_op.hpp` | `generate_*_nodes`, `batch_insert_update_interior`, `handling_interior_split_overflow`, `batch_delete_update_interior_size`, `batch_delete_update_leaf_parent`, `balance_over_leaves` |
| `b_tree_impl/tree_op/leaf_op.hpp` | `post_process_leaf_change`, `insert_points_2_leaf`, `delete_points_4_leaf`, `handling_leaf_split_overflow` |
| `b_tree_impl/tree_op/flatten.hpp`, `b_tree_impl/points_op.hpp` | `flatten_rec`; `partition_batch` and the two partitioners |
| `b_tree_impl/query_op/*.hpp` | membership, range count, range report, iterator |
| `b_tree_impl/validation.hpp` | `validate`, `verify_tree_property`, `check_height` |
| `tests/self_test.cpp` | dataset-free gate against `std::map` |
| `tests/{unit_test,b_test}.cpp`, `tests/test_framework.h` | data-driven tests, benchmark driver |

Build: CMake, `build/` exists; `cd build && make <target> -j`. `self_test`
and `unit_test` compile `-UNDEBUG` (asserts live); everything else is
`-O3 -march=native -DNDEBUG`. The CMake option `DEBUG=ON` makes every
target, benchmarks included, a `-g -DDEBUG` Debug build: no `-O`, asserts
live, plus the `#ifdef DEBUG` box-vs-split loops (O(subtree) per touched
node). `build/` was configured that way on 2026-09-21; the 2026-09-29
sweep left it `DEBUG=OFF JEMA=ON LEAF=64` (`incre_updates.sh` wipes and
reconfigures it). Check `grep DEBUG build/CMakeCache.txt` before timing.
The recipes under Tests give `-DDEBUG` at `-O1` for one binary.

## Conventions

- Names: `lower_snake_case` everywhere; `CamelCase` template parameters
  (`TT`, `R`, `LeafSortPred`); `k_` prefix on `static constexpr`
  constants; enum values and template parameters take no prefix; STL alias
  names where one exists (`key_type`, `size_type`; `value_type` is the
  entry, `mapped_type` the value). Nothing `UPPER_CASE` but macros, all
  `PABTREE_`-prefixed except the read of `DEBUG`.
- Format: Linux kernel style, tabs 4 wide, 80 columns measured at tab 4,
  `/* */` comments only, one blank line after every declaration in
  `b_tree.h`. Format with `/opt/opencilk-3/bin/clang-format` (19); the
  mason 18 on `PATH` rejects `.clang-format`.
- Do not change the algorithms, existing asserts (adding is fine) or the
  submodules unless asked. `external/baselines/` algorithms are frozen.
- Prove "no behaviour change" by codegen, not timing: build both sides in
  the same build dir and diff the normalised disassembly:

  ```bash
  objdump -d --no-show-raw-insn X | grep -E '^\s+[0-9a-f]+:' \
    | sed -E 's/^\s*[0-9a-f]+:\s*//; s/<[^>]*>/<S>/g; s/0x[0-9a-f]+/HEX/g; s/\b[0-9a-f]{4,}\b/ADDR/g'
  ```

  Section hashes differ on relocations alone; a 5-character string change
  moves 11 707 lines, so a 0-line diff is a strong statement. Timing on
  this shared box has a noise floor of several percent and drifts within a
  session; a 3-round `-r 3` at 1e9 repeats within 0.05 s, a 2 ms benchmark
  does not.
- After a rename: `-Wshadow` over `self_test` (baseline 3 warnings, all
  pre-existing; a plain `-I` `-fsyntax-only` compile shows 31, all in
  parlay/gtest headers; a nested-scope collision compiles and changes
  meaning) and
  a grep of string literals for the old names (an `EXPECT_DEATH` regex once
  matched an assert's text).

## Type system

`btree_type_trait<EleType, BufferSize = 64, LeafAug, InteriorAug, NodeType,
KeyCompare, NodeAllocType>`. `btree_trait<EleType, BufferSize, Options>`
names the rest: inherit `default_options` (`leaf_aug`, `interior_aug`,
`node`, `node_alloc`, `key_compare`; `void` means `std::less`, a plain alias
so the struct can be local to a function; a local struct cannot hold a
static member, so options are type aliases only).

```
k_buf_lb = B    leaf valid iff size >= B; interior valid iff >= B valid children (root: >= 1)
k_buf_md = 2B   build/split target for leaves and interiors
k_buf_ub = 4B   leaf capacity, max children, every std::array size
```

`buf_size_type` is **unsigned**: `uint_fast8_t` for B <= 63, else 64-bit
(`uint_fast16_t` is 8 bytes here). `n - 1` wraps at 0: backward loops are
`for (i = n; i-- > 0;)` and every `- 1` is guarded (`get_split_n()` is 0
for `node_n == 0`). `size_type` is `uint_fast32_t`.

Entry: a struct with public `key`/`val` (`plain_entry_adaptor` derives the
types), or an adaptor with `entry_type/key_type/mapped_type` and static
`get_key/get_val/to_entry`; `entry_traits_of<E>` picks. A `conditional_t`
over a constrained alias hard-errors even on the untaken branch, so the
constraint is a `static_assert` in the class body.

Comparator: structural ordering decisions go through `TT::key_less`,
`key_equal`, `entry_less` (`a <= b` is `!key_less(b, a)`); `key_equal` is
one comparison for `std::less` or a comparator with static `equal`.
`SelfTest.ReverseComparator` (`std::greater`) is the only test of this;
five sites passed under `std::less` and failed under it, and a sixth hid
behind `#ifdef DEBUG`. Remaining exceptions: unsorted range scans and
`operator<=>` use native key operators; see the read-through findings below.

Allocator: every node comes from `TT::node_alloc::alloc<Node>()` and goes
back through `free<Node>(p)`; must be thread-safe. The allocator's `free`
must run the destructor: `free_node` only delegates, and the default
Parlay `retire` destroys the node. README and `node_base.h` incorrectly
say the destructor has already run before the allocator receives it.
`free_node<NodeType>` with the wrong type is silently wrong.

Augmentation: `leaf_aug(range)`, `leaf_aug::update_aug(range)`,
`interior_aug::update(children, splits)`, `force_parallel(size, cutoff)`,
`reset()`. Concepts are checked at the call sites that pass the range, so
a leaf aug with no `update_aug` compiles until something inserts. Both
node types have `get_aug()`.

Node base: the `node_base` concept wants `is_leaf`, an assignable `size`,
and construction from `(bool, size_t)`; a plain aggregate satisfies it.

## Nodes

`leaf_node`: `is_leaf`, `size`, `elements` (`std::array<entry, k_buf_ub>`,
tail deliberately uninitialised), `aug`, `is_sorted`.
`btree_interior_node : multi_way_node`: `node_n`, `tree_nodes[k_buf_ub]`,
`split[k_buf_ub]` (`node_n - 1` live), `aug`, `size` (sum of children, set
by the constructor), and the deletion flags `valid`,
`contain_invalid_subtree`, `reinsert_size` (constructor and
`reset_deletion_flag()` give true/0/0).

Separators: `split[i]` is a closed lower bound of child i+1 and a strict
upper bound of child i; strictly increasing. Build emits the first key of
the right bucket; a deletion may remove that key and the bound stays valid.
`get_node_id_containing_key(k)` = first i with `split[i] > k`;
`_exclusive` = first i with `split[i] >= k` (right edge of a half-open box).

Predicates (`b_tree.h`): `get_node_invalid_status` (leaf `size < B`,
interior `!valid`), `node_is_valid`, `node_contain_invalid_subtree`,
`get_node_reinsert_size` (items for the tomb: a leaf 1 if underfull, an
interior its `reinsert_size`), `node_has_leaf_children` (reads child 0;
never call it on a node with `node_n == 0`, its slot 0 is a freed leaf).

Buckets: `get_bkt_num(n) = n / k_buf_md`, `get_bkt_st_idx(i) = n*i/bkt_n`,
`get_bkt_end_idx(i) = n*(i+1)/bkt_n`; sizes land in `[k_buf_md, 3B]`, so
`generate_*_nodes` makes at least 2 from `>= k_buf_ub` items.

`k_binary_search_base` = cache line / `sizeof(entry)` (leaf) or
`sizeof(key)` (interior); `binary_search<Base>` is branchless in both
phases. The four hand-inlined right-boundary searches in
`range_count`/`range_report` stay inlined: a `slice` version cost +35–54%.

## Build, insert, delete

`build_`: `delete_tree()`, `parlay::sort`, assert distinct keys;
`build_recursive`: `< k_buf_ub` entries → one leaf, else
`generate_leaf_nodes` + `assemble_interior_levels` (repeats
`generate_interior_nodes` while `>= k_buf_ub` nodes remain, then one root
with `[2, k_buf_ub)` children; writes into the caller's node/key buffers
from the second level on).

`batch_insert_`: sort once; `batch_insert_recursive` partitions with
`partition_batch` (binary search when `n > k_split_factor(8) * split_n`,
else merge; `[[gnu::always_inline]]`, or it costs 9 calls), recurses with
grain 1; leaf fits → `insert_points_2_leaf`, else
`handling_leaf_split_overflow` (sorted leaf: `parlay::merge`, `fbb03b1`;
redd puts up to 66 M keys of a batch past the last key, into one leaf, and
this cut its insert chain 29%);
`batch_insert_update_interior` has a fast path when no child split; a root
split goes through `assemble_interior_levels`. Scratch: `2n`
`node_split_pair`s, 32 B per input entry. Slots `2i`, `2i+1` belong to
input element i; that mapping is what makes the parallel sub-ranges
disjoint, so it cannot be shrunk.

**`serial_insert`** (2026-10-14, user: one entry at a time, same interface
as `batch_insert`, own file). Input order, one descent per entry; no sort
(`sorted_input` unused), no parlay, no scratch. Null root or an interior
root of size 0: grows from an empty leaf (`batch_insert_` builds instead).
Leaf with room: append, `update_aug`, then the pattern decides as in
`post_process_leaf_change` (hybrid asks after the append); a sorted leaf
kept sorted takes the entry by shift, an unsorted one is `std::sort`ed.
Full leaf + 1: sorted, cut at `n / 2` as `generate_leaf_nodes` cuts (old
leaf keeps `2B`, new leaf `2B + 1`, separator = its first key). Interior:
`size += 1` on the way up; a split child's right half goes in after it; a
full node cuts at `n / 2` as `generate_interior_nodes` (new sibling `2B +
1`, merged separator `2B - 1` goes up). Root split: new root over both. So
it builds node for node what `batch_insert` of one-entry batches builds.
`serial_split_interior` is `[[gnu::noinline]]`: its merged arrays (3 KB at
leaf 64) would sit in every recursion frame (136 B without).

The default hybrid predicate on one-entry slices is leaf-size parity (the
address terms cancel; 8-byte entries sort iff the new size is odd, 16-byte
iff even, checked), so half the inserts `std::sort` their leaf.

Sanity timing, one run, not ABBA (leaf 64, 1e7 built, 1e6 random, best of
3), ns/entry, all threads / `PARLAY_NUM_THREADS=1`: `batch_insert` 3.2 /
155; `serial_insert` sorted 496 / 449, unsorted 158 / 138, hybrid 977 /
934, sorted on a key-sorted batch 132 / 98; one-entry `batch_insert` loop
(old `insert(entry)`) 3331 / 2857; tlx `insert` 562 / 550. Sorted pays
dependent misses in the leaf's binary search; unsorted touches only the
leaf's end. Serial code ran ~10% slower with parlay's workers alive.

**`serial_delete`** (2026-10-14, user asked: classical or re-insert;
classical chosen). `batch_delete`'s interface, one key at a time in input
order, one descent per key; no sort, no parlay, no node allocated. Absent
key: no-op; null root: return; an emptied root leaf stays, as in
`batch_delete`. Leaf: binary search if sorted, else a scan; a sorted leaf
the pattern may keep sorted closes the gap by shift, else the last entry
fills it; then the pattern decides as in `post_process_leaf_change`
(hybrid asks after the removal). Interior: `size -= 1`; a child left
underfull (leaf `size < B`, interior `node_n < B`) pairs with its larger
sibling, the left one on a tie. Together `< 2B`: the right one merges into
the left (an interior's separator comes down). Else both even out at
`n / 2`, the cut `generate_*_nodes` makes: a leaf giver is sorted first
and the separator becomes the right leaf's first key; interiors rotate the
separator through the parent. Merging and evening out keep `is_sorted`
exact and do not ask the pattern. The root collapses while it has one
child. Needs `B >= 2` (`static_assert`): at B = 1 an underfull interior
has no child left. On a one-key slice of 4-byte keys the default hybrid
predicate's address terms do not cancel (as they do for 8- and 16-byte
entries): bit 2 of the slice address flips the answer, so two call sites
can leave different `is_sorted` (read from the code).

Why not the graft/re-insert fix-up: one key leaves at most one underfull
node per level, all on its path, and a sibling fixes each in O(B). The
pool and counting sort exist to fix many subtrees at once, in parallel.
That path run one key at a time is one-key `batch_delete`, which is what
`erase(key)` did before 2026-10-14.

Sanity timing, one run, not ABBA (leaf 64, 1e7 built, random keys, best
of 3), ns/key, all threads / `PARLAY_NUM_THREADS=1`. 1e6 deleted:
`batch_delete` 4.1 / 169; `serial_delete` sorted 659 / 459, unsorted
495 / 345, hybrid 608 / 573, sorted on a key-sorted batch 131 / 100;
one-key `batch_delete` loop (old `erase(key)`) 1843 / 1758; tlx
`erase` 392 / 402. 6e6 deleted (more merging): `batch_delete` 2.0 / 115;
sorted 413 / 402, unsorted 345 / 341, hybrid 507 / 514, key-sorted 86 /
78; tlx 392 / 400. At 1e6 the serial calls ran up to 43% slower with
parlay's workers alive (cause not checked); at 6e6 within 10%.

One at a time against tlx and std::set (2026-10-14, same session, 1e7
random entries in input order, insert / delete s): 1e8 tree, ours hybrid
12.0 / 10.3, sorted 9.8 / 10.1, unsorted 3.1 / 8.5, tlx 11.7 / 11.8,
std::set 26.2 / 25.6; 1e9 tree, hybrid 13.3-15.5 / 14.0, sorted 13.8 /
14.1, unsorted 7.0 / 12.3, tlx 19.3 / 18.8, std::set 39.8 / 41.9.
`logs/summary_serial` is a 1e8 tree and its 1% column is 1e6 entries, not
1e7 (user read it as 1e7 at 1e9). Details: `docs/harness_notes.md`.

`batch_delete_`: sort once; `batch_delete_recursive` marks bottom-up. A
leaf-parent first balances its underfull leaves, then takes its flags from
what is left (`batch_delete_update_leaf_parent` -> `balance_over_leaves`);
an interior with interior children takes them from its children
(`batch_delete_update_interior_size`). Flags: `size`, `valid`,
`contain_invalid_subtree`, `reinsert_size` (inputs for the tomb, see Graft);
untouched interior children get `reset_deletion_flag()`. Bookkeeping reads
the **touched** children only (2026-09-28, this branch): a node's `size`
drops by what they lost (sizes read before the recursion), and a
leaf-parent looks for underfull leaves among its touched leaves, because
between operations every leaf below a leaf-parent holds `>= k_buf_lb`
(`#ifdef DEBUG` checks both against a full scan). Reading every child was
a cache miss per child: at 1e9 uni 0.01% the chain went 76.2 -> 40.6 s on
`dev`'s code and 84.3 -> 39.5 s here (probe copies, `-t 128 -r 1`); seg and
10% are unchanged. `dev` still reads every child.
`delete_points_4_leaf` compacts the leaf (linear find per key, swap to the
end: O(keys × leaf size), sorted leaf or not) and
`post_process_leaf_change` decides `is_sorted`. Deleting an absent key is a
no-op. Then:

- root invalid and a leaf: return; `size == 0`: free the tree;
- root invalid with entries live: `consolidate_invalid_root`, else
  `rebuild_from_flatten` (flatten, free, sort, build) when its guard fails;
- root valid, interior, invalid node below, `size < k_buf_ub`:
  `rebuild_from_flatten`;
- else `graft_repair` (Graft, below). After every delete a root left with
  one child is collapsed; leaf balancing alone can leave one.

**`repair_rec`** (2026-09-12, replaced flatten-and-reinsert; **on `dev`
only**: this branch removed it, with `repair_node` and
`balance_over_children`, for the graft below). Bottom-up.
Children flagged `contain_invalid_subtree` are repaired first, in parallel
(grain 1), then `repair_node`: a leaf-parent merges its underfull leaves
(`balance_over_leaves`; all-empty leaves are freed, a lone underfull leaf
is left for the parent); an interior runs `balance_over_children` — drop
empty children, group the thin ones (`node_n < k_buf_lb`) with the nearest
valid sibling to the right exactly as phase 1 of `balance_over_leaves`,
then per group: concatenate the child arrays into the last member
(separator between two members = the parent's split between them), or,
above `k_buf_ub` children, re-bucket the group's children with
`generate_interior_nodes`. Only pointers move; entries move only inside
`balance_over_leaves`. A node that absorbed siblings is repaired again
(`repair_node(dest)`) because a thin sibling may have carried a thin child
of its own; the cascade runs along that seam only. Everything strictly
below a repaired node is valid; the node itself may be thin for its parent,
and the root's thinness is resolved by collapsing single-child roots.
Height never changes below the root. No reinsert, no `2n` scratch, no
`has_tomb`. `contain_invalid_subtree` prunes the traversal;
`reinsert_size` is still computed but read only by one assert and the
uncalled `check_tree_aug_after_delete`. `balance_over_leaves`'s
`reinsert_size` argument is the caller's underfull-leaf sum, not the field.

Serial repair work (2026-09-21): with `V` nodes before repair and capacity
parameter `B`, the release/default-augmentation bound is
`O(V * B * log(B + 1))`, including local leaf sorting; structural work is
`O(V * B)`. Charge group changes to `Phi = node_count + underfull_count`,
using actual occupancy, not deletion flags. A changed group of `r >= 2`
nodes contains at least `r - 1` underfull nodes; merging or redistributing
decreases Phi by at least `r - 1`, even allowing its parent to become
underfull. Thus total group sizes and recursive repair callbacks are
`O(V)`, without an extra height factor. A group's movement/sorting costs
`O(r * B * log(B + 1))`. For an initially valid tree with `B >= 2` and
`N` entries before deletion, `V = O(1 + N/B)`: worst-case repair is
`O((N + B) * log(B + 1))`, linear in N for fixed B. This excludes deletion
batch sorting and the separate invalid-root rebuild path.

Measured at 1e9, user's command (`-t 128 -r 3`, `-p
kv_pair_incre/<dis>/1000000000_1/ins/100/1.batch.bin -D <dis>`), ABBA x2
against the consolidation commit `c937b45`, repeats within 0.08 s:

| dis | build | 0.01% | 0.1% | 1% | 10% |
|---|---|---|---|---|---|
| uni | `c937b45` | 35.15 | 14.54 | 5.98 | 1.96 |
| uni | repair | **32.61** | **13.09** | **5.55** | **1.81** |
| norm | `c937b45` | 28.62 | 13.60 | 5.48 | 2.64 |
| norm | repair | **24.43** | **10.77** | **3.79** | **1.77** |
| seg | `c937b45` | 10.59 | 4.67 | 3.13 | 2.67 |
| seg | repair | **7.63** | **2.54** | **1.59** | **1.38** |

uni −7..10%, norm −15..33%, seg −28..49%: the more a batch concentrates,
the more whole subtrees go thin or empty, which the old path flattened and
reinserted. Wall time of the repair phase per chain (probe, `-r 1`): uni
4.0/2.8/0.9/1.25 → 1.1/0.57/0.21/0.07 s; norm 5.9/4.7/3.1/1.1 →
1.2/0.54/0.20/0.10 s; seg 7.4/4.0/3.0/2.4 → 2.8/0.40/0.16/0.11 s. The old
path reinserted 0.35–1.4 B entries per chain, more than the tree holds:
the same entries went round wave after wave. `size()` equals the expected
count after every batch, all three distributions, both builds.

Still open: on `norm` the consolidation guard fails twice (a leaf-parent
left with 1..`B`-1 entries) and the full rebuild runs — 0.1%: batch 997,
2 M entries, 4 ms; 10%: batch 7, 200 M entries, **0.29 s of a 1.8 s
chain**. Letting `repair_rec` take the invalid root too (the no-valid-child
merge at every level, then the single-child collapse) would remove the
guard and that rebuild.

Tests: `Repair*` in `self_test` — five shapes read off the live tree with
`leaf_parents_of` (thin, seam, overflow, empties, chain) at leaf 2/8/64,
plus augmentation, leak (allocs == frees per node type), heap allocator and
`std::greater`. The names are from `repair_rec`; here they run the graft,
and `expect::graft` asserts no leaf was allocated.

**Why the root collapses.** Build leaves hold `2B`; uniform deletion puts
all of them at `B` once half the tree is gone. A batch of >= 1% pushes every
leaf under `B` in one pass, invalidity reaches the root in that same
recursion, and no repair runs. Small batches cross a few leaves per batch,
get repaired, and the tree narrows instead; the root then fails only at
the height floor (a 5-level tree cannot keep one root child valid below
~`B^4` entries at minimum occupancy, ~100 M at 1e9 in practice).

**`consolidate_invalid_root`** (2026-09-12). Guard, read-only, before any
mutation: `root->size >= k_buf_ub` and every leaf-parent holds 0 or `>= B`
entries (summed from the leaves, not the flags). Then (1) parallel
recursion: a leaf-parent with an underfull leaf runs `balance_over_leaves`
— with **no** valid leaf it takes the new phase-1 branch, one group pivoted
on the last child (was an `assert`); all-empty leaves are freed (`node_n =
0`); (2) `collect_leaves_rec`: leaves in order plus the separator before
each, from the stored splits (child 0 inherits, child j gets
`split[j-1]`), freeing each old interior after its join; (3)
`assemble_interior_levels`. No entry leaves its leaf-parent; local leaf
balancing may sort, but there is no global sort. Scratch is a pointer and
a key per leaf. Which path ran shows in
the allocator: consolidation allocates interiors and never a leaf. At 1e9
(`-t 128 -r 3`, ABBA): del 1% 6.69 → 6.02 s, 10% 2.52 → 1.96 s, small
ratios unchanged; 470 M entries in 0.020 s where the rebuild took 0.62 s.
The same bottom-up merge for the reinsert path is planned in
`docs/plan_structural_delete_2026-09.md`. Here the mark pass has balanced
every leaf-parent already, so step (1) only frees all-empty ones.

**`balance_over_leaves`**, sequential per node: a backward pass gives each
child the nearest valid child to its right as pivot and sums the group;
trailing invalid children join the rightmost pivot. Per group: one node →
slide; `sum < k_buf_ub` → copy into the group's last node, free the rest,
`post_process_leaf_change`; else gather into `entry_buf` (`reinsert_size
+ k_buf_ub`), `std::sort`, scatter into the first `sum / k_buf_md` leaves
of the group (no allocation), splits = first key of each next bucket, free
the surplus. Writes go straight into the node's arrays; `tree_idx <= i`
throughout is what makes that safe. Epilogue: `node_n`, `update_aug`,
`reset_deletion_flag`; `size` is not recomputed.

**Graft** (2026-09-24/28, branch `graft-delete`; idea
`docs/new_delete_alg.md`, plan `docs/plan_graft_delete_2026-09.md`). The
fix-up below a valid root on this branch; compare against `dev`, which
keeps `repair_rec`. `c937b45`'s reinsert, but whole valid subtrees go back
instead of flattened entries. History: `00e8c80` had a `delete_policy`
option, a separate settle pass and per-tomb rounds with allocations in
the recursion; the settle pass went into the mark, then (user,
2026-09-25) the per-tomb rounds became one global pool and one descent
(`529ef36`, which still sorted all inputs). 2026-09-28 (user: use
`dev_delete`'s implementation, optimise it): restructured after
`dev_delete` (`9e34f95`, the user's March version: box and height
augmentation, a separate `MarkValidSubtrees` pass, per-height
`BatchInsertNodesRecursive` tallest first, entries last by `BatchInsert_`,
scratch of n slots although a target can emit more, asserts the root never
splits, allocates at every target node). Kept from it: preallocated
collection from per-node counts, entries collected already sorted,
batch-insert routing. Not kept: the augmentation (bounds from separators do
the same) and the extra passes.

The mark pass leaves `reinsert_size` = inputs a subtree hands over:
children's counts, plus the valid children of an invalid node; an invalid
leaf-parent hands every leaf, or, when its leaves merged into one underfull
leaf, that leaf's entries; an all-empty one is left whole. Validity is
decided after leaf balancing. Then `graft_repair`:

1. `graft_detach_rec(root)` writes every invalid subtree into one pool
   (SoA: keys, nodes, entries, heights), preallocated from the root's
   count, **in key order**: an item is (node, lower bound, height), a
   loose entry has no node (height `k_entry_source`), sorted per leaf on
   the way. Structural order is key order except that a valid item whose
   bound rose must follow what was cut from below that bound;
   `graft_decompose_rec` moves it there (usually 0 steps). Kept nodes are
   compacted (left-absorb, raised separators); the root's raised bound is
   the global left edge. `graft_detach_rec` visits valid nodes retained in
   the tree or pool; it removes their invalid children, compacts them
   (size from the children it visited, not a re-read of every child) and
   returns a raised first bound. `graft_decompose_rec` frees an invalid
   node and emits its valid children as items (after detaching their
   invalid descendants).
2. `parlay::counting_sort` of the positions by height (stable, O(n))
   gives `hpos`/`hoff`: group h = the height-h items, increasing. No
   comparison sort. One scratch of 2 `node_split_pair`s per input.
3. Inputs left of all content (keys < edge; rare): tallest-first rounds
   along the leftmost path (`graft_rec` / `graft_into_node` with the edge,
   moved after each round), then their entries.
4. Everything else in one descent, `graft_all_rec`. A node with no items
   one level below it routes like batch insert (`partition_batch`, touched
   children in parallel; no child split: only `size` changes). A node that
   takes items goes to `graft_join`: each item joins right after the child
   its bound routes to; every shorter input routes by that merged list, so
   it can land inside a just-joined item; the node is rebuilt once after
   its children return, and a split hands its parts up through the
   scratch. Sizes move by what the touched children gained; untouched
   children are not read. Buffers are on the stack; the heap only when a
   node takes more than `k_buf_ub` children.

A whole item at input position p is consumed at its parent: the previous
child's input range ends at p, and the grafted child's starts at p + 1.
Thus only shorter inputs descend; at height 1 only entries reach leaves.
Each child's scratch slice uses its disjoint input range.

Why not one batch per height: bottom-up rounds strand keys (removed X
holds leaf q, leaf-parent P, leaf v: q and v go into the left
neighbour's last leaf-parent before P exists, so v ends up left of P),
and rounds in either order rebuild a height-L node in every round below
L. The single descent places same-level items before shorter ones route
and rebuilds each node once.

Rules, each with a hand-built test: same-level items first (`order`); a
dropped child's range goes to its **left** neighbour, and a node whose
first child went reports its new bound so the parent raises that
separator (`graft_compact`; `raised`); inputs left of all content are
prepended, edge moved after each round (`edge`). `inner` checks that a
raised bound below the root routes lower items into its left sibling in
the single descent. A node that
lost a child can overflow when the rest comes back (`GraftTombOverflow`,
B = 4: root 16 children -> 17, height 4 -> 5), so `c937b45`'s no-split
argument does not carry over. `loose`/`loose_edge` (and `loose` under the
`unsorted` policy): a leaf-parent left with one underfull leaf, whose
entries must land inside the grafted leaf-parent before it. Faults
injected and caught (2026-09-25): shortest-first edge rounds and no raise
(`GraftHandShapes`); keep right separator, no edge update, wrong separator
between a split child's parts (`RepairShapes`). 2026-09-28, the
restructure, 10 of 10 caught: no reorder of a raised item, size not grown
(route, join, join delta), wrong separator after a split, item placed one
child late, the heap path, loose count 1, no sort of an unsorted loose
leaf, left-edge entries dropped; the loose shapes were added because the
count fault survived. The `#ifdef DEBUG` `validate<false>()` before the fix-up
is gone: after the mark, a leaf-parent can be thin until grafted;
`graft_repair` validates the result under `DEBUG`. 38/38 at `-O1
-UNDEBUG`; delete tests under `-DDEBUG -UNDEBUG` and ASAN/UBSan.

Timings, 2026-09-28, 1e9 `b_test -t 128 -r 1` (chain s; seg one run,
uni/norm A B B A mean of 2, repeats within 1%), probe copies in
`build/scratch_graft/probe` (phase timers on stderr; `summarize.py`):

| chain s | seg 0.01 / 0.1 / 1 / 10 % | uni | norm |
|---|---|---|---|
| `dev` repair | 6.98 / 4.09 / 3.70 / 3.59 | 76.47 / 30.91 / 13.97 / 4.81 | 55.09 / 24.81 / 9.36 / 4.27 |
| graft `529ef36` | 6.73 / 4.47 / 3.78 / 3.63 | | |
| graft, restructured | 6.58 / 4.46 / 3.75 / 3.64 | 84.28 / 31.31 / 13.91 / 4.78 | 58.96 / 24.98 / 9.36 / 4.36 |
| per-height rounds | 6.57 / 4.37 / 3.70 / 3.61 | | |
| `dev` + touched-only | 7.04 / 4.10 / 3.70 / 3.66 | 40.46 / 26.96 / 13.76 / 4.80 | 36.18 / 22.59 / 9.20 / 4.31 |
| graft + touched-only (this tree) | 6.59 / 4.44 / 3.74 / 3.65 | 39.45 / 26.73 / 13.58 / 4.76 | 35.55 / 22.30 / 9.09 / 4.31 |

Phases (timed chain, 0.01%): the batch sort is ~3.3-3.7 s everywhere (45%
of seg). Fix-up: repair 1.99 / 2.05 / 1.97 s (seg/uni/norm), graft 0.23 /
0.16 / 0.39 (`529ef36`: 0.42 on seg, input prep 0.16 -> 0.02). The mark
pass is the rest: uni 70.7 s on `dev` (92% of the chain), 34.1 with
touched-only reads; the graft's mark is +1.4..1.5 s over `dev`'s because
leaf balancing lives there (in `repair_rec` on `dev`; same CPU per perf).
With equal bookkeeping the graft wins 1-2.5% on uni/norm and 6% on seg
0.01%, loses 8% on seg 0.1% (balancing on the mark's critical path; seg
halves every leaf in its window). `dev_delete`-style per-height rounds
equal the single descent within noise. A run takes 8-14 min, mostly the
five 8 GB reads and the dedup sort in `read_entries`; each chain runs twice
(`time_loop` warm-up), the timed one is the second `[probe]` line.
Older numbers: `00e8c80` at 1e9 uni, repair 70.2 / 28.9 / 13.3 / 4.72 s,
graft 78.9 / 30.4 / 13.9 / 4.78 s (single runs); at 1e8 all three within
2% (the mark pass dominates).

Sweep (`incre_updates.sh`: tag 224, `-r 3`, leaf 64, `numactl -i all`,
JEMA), delete chain s. Graft is `92886ba` (2026-09-29), repair is `dev`'s
`1bf5576` (2026-09-22), CPAM from the graft session.
`scripts/compare_incre_delete.py` writes them with insert, queries and
ratios to `incre_updates_new_graft_vs_repair.csv`:

| dis | run | 0.01% | 0.1% | 1% | 10% |
|---|---|---|---|---|---|
| uni | repair | 33.66 | 13.38 | 5.61 | 1.80 |
| uni | graft | **19.60** | **11.49** | 5.40 | 1.77 |
| uni | CPAM | 54.37 | 30.89 | 7.03 | 1.58 |
| norm | repair | 28.75 | 12.44 | 4.00 | 1.81 |
| norm | graft | **19.23** | **9.96** | 3.70 | 1.82 |
| norm | CPAM | 48.04 | 21.12 | 4.43 | 1.36 |
| seg | repair | 8.72 | 3.14 | 1.91 | 1.43 |
| seg | graft | 7.14 | 2.60 | 1.68 | 1.36 |
| seg | CPAM | 4.96 | 2.13 | 1.35 | 1.13 |

The repair session ran slow on norm and seg: its insert chain (same code
as the graft run) is 22% / 8% over on norm 0.01% / 0.1% and 9% / 10% on
seg 0.01% / 1%, and its delete is up to 18% (norm) and 23% (seg) over the
2026-09-21 ABBA numbers above; uni agrees within 4% on both. Against the
ABBA repair numbers the graft run is 1.66 / 1.14 / 1.03 / 1.02x on uni,
1.27 / 1.08 / 1.02 / 0.97x on norm, 1.07 / 0.98 / 0.95 / 1.01x on seg: a
small-batch gain on uni and norm (the touched-only reads), nothing on seg.
CPAM's update chains repeat within 2% between the sessions (4.5% worst,
seg ins 0.01%) apart from one 512 s outlier, uni del 0.01% at `1bf5576`.
Against CPAM the graft run wins up to 1% batches on uni and norm
(insert 1.4-2.0x, delete 1.2-2.8x), loses 10% deletes (0.89x, 0.75x) and
all of seg (insert 0.76-0.98x, delete 0.69-0.83x).

Sweep and probe numbers do not compare: same repair code, uni 0.01%, 33.7 s
in the sweep and 76.5 s in the probe (cause not checked). The sweep's
`run_cmake` runs `rm -rf *` in `build/`; the probe copies are gone.

Real data (`b0d964e`, 2026-10-20, against `1bf5576`'s repair; CPAM drift
within 4%): delete chain osm 0.84x, ref 0.92x, sem 0.94x, redd 0.98x.
Tables and harness caveats: `docs/harness_notes.md`.

## Leaf sort policy

`post_process_leaf_change` is the one place `is_sorted` is decided:
`sorted` sorts, `unsorted` marks false, `hybrid` asks `leaf_sort_pred(node,
in)`. `default_hybrid_pred` hashes `size + in.size()` with address bits
of the slice — deterministic under ASLR (allocations are 16-byte aligned,
so bit 3 is an in-buffer offset), and the address terms flip half the
calls. A lookup on an unsorted leaf is a linear scan; `sort_leaves()` is
the idempotent parallel fix. Iteration sorts the leaf under the cursor on
arrival, so iteration and lookups are not thread-safe until `sort_leaves()`
has run.

## Queries and iteration

`contain`: iterative descent, `get_node_id_containing_key`, then leaf
binary search or scan. `range_count`/`range_report`: at each node find `l`
and `r` (the `r` search starts at `l`), recurse on the two boundary
children; middle children are fully covered, and `range_report` flattens
them in parallel into precomputed offsets. `range_report`'s output must
hold `range_count(box)` entries (asserted, not clamped: clamping cost
+3.5%). `get_entries` asserts `out.size() >= size()` at the top and does
not sort leaves (`flatten_rec` uses `Sorted = false`). An empty
or default tree answers every query empty. `flatten_rec<R, Granularity,
Sorted>`: `Sorted = true` sorts each leaf in place.

STL interface (`iterator.hpp`): `size empty max_size clear`, `contains
count find lower_bound upper_bound equal_range`, `begin/end/rbegin/rend`,
`to_seq`, `bulk_load insert erase`, `==`/`<=>`, `key_comp`. `const_iterator`
is three words and re-descends at leaf boundaries. Range overloads are
constrained with `requires std::ranges::range`; without it `erase(500u)`
binds to the template. `insert(entry)`, `erase(key)` and `erase(iterator)`
run `serial_insert`/`serial_delete` (user, 2026-10-14; were one-entry
batch calls); `erase(key)` reads `size()` before and after for its return
value. At `k_buf_lb` 1, which `serial_delete` rejects, `erase` stays on
`contain` + `batch_delete` (`if constexpr`). Loops of 1e6 calls on 1e7
(leaf 64, best of 3, A B B A, repeats within 1%), ns/call, one thread /
all: `erase(key)` 2018 / 2109 -> 621 / 645, `insert(entry)` 2159 / 2177
-> 889 / 881. Batch vs serial by batch size, 1 to 1e8 at 1e9: `b_test -t
256` (`docs/harness_notes.md`).

## Tests

| target | data | what |
|---|---|---|
| `self_test` | none, ~45 s | keys from a seed against `std::map`; leaf 2–256, five key shapes, every size where the shape changes; `--seed=N` reproduces a run exactly |
| `unit_test` | `/ssd1/zmen002/kv_pair` | uni/exp/norm/dual_norm at 1M–50M, every `*.base.bin`: build, insert the 1% batch, delete that batch, membership and range queries against the input; one config (leaf from `-DLEAF=`, default 32); checks are mostly plain `assert` (abort, no gtest report); `--file_filter=`, `--no_redirect`; skips without the data |

`check_against` validates, then iterates **before** `to_seq`/`get_entries`
(`to_seq` sorts every leaf and once hid an iterator bug; `get_entries`
does not sort). Special tests:
`ReverseComparator`; `CustomAllocator` (leak check, `live == 0`);
`Augmentation`; `DefaultHybridPredIsReproducible` (re-execs itself; an
in-process pair sees the same addresses and proves nothing);
`GetEntriesOutputSize` (`EXPECT_DEATH`, `death_test_style = threadsafe`);
`DeleteAbsentKeys`; `Consolidate*` and `DeleteChainTenPercent`
(deterministic collapse: sequential keys, keep every 8th — a random 10%
chain does not always collapse the root at small n). `GraftHandShapes`,
`GraftTombOverflow`, `GraftHeapAllocator`: shapes built node by node
(`shape_spec`, `shape_builder`, `install` copies a built root into the
tree's root node); `leaf_keeping(n, k)` deletes all but a leaf's last k
keys, and `hand_shape`/`delete_and_check` take a leaf policy (default
`hybrid`). `SerialInsert*` (9, end of file): `MatchesBatch` compares
node by node with per-entry `batch_insert` (three patterns, hybrid on a
size-only predicate); `FromEmpty` runs unsorted, hybrid, sorted calls with
only `validate()` between (iteration sorts leaves), three key orders, and
leaf 256 at 600 K ascending to split the root; `Shapes` is
`run_scenario<KBuf, true>`; `Augmentation` grows in both key orders and
checks after each of 3000 one-entry calls, because a revisit refreshes a
stale augmentation; plus churn, `std::greater`, leak, heap allocator, an
empty interior root. 26 mutants of `b_serial_insert.hpp`, all caught.
`SerialDelete*` (10): `HandShapes` pins 17 shapes at leaf 4 (leaf borrows
left/right, evens out, merges on a tie / first / last leaf, unsorted
givers, the interior cases, a node at the minimum, root collapse, a
cascade to the root) and asserts no node is allocated; `ToEmpty` empties
with unsorted, hybrid, sorted calls at leaf 2/8/64, three key orders, plus
leaf 2 at 200 K and leaf 256 at 600 K; `Shapes` is `run_scenario<KBuf,
SerialInsert, SerialDelete>`; plus absent keys, leaf patterns, churn mixed
with batch ops, augmentation, `std::greater`, leak, heap allocator.
`SerialNonContiguousInput` feeds a `std::deque` to both serial calls (a
copy that drops the last element fails it, in either call). 49 mutants of
`b_serial_delete.hpp`, all caught; the policy ones (which sibling, when to
merge, where to cut) only by `HandShapes`. 58/58 in the CMake build;
`Serial*` 20/20 under ASAN/UBSan, 19/19 (before the deque test) under
`-DDEBUG`. `SerialSingleElementOps`: `erase(key)`, `insert(entry)`,
`erase(iterator)` leave the shapes `serial_delete`/`serial_insert` leave
(`describe`, not `same_structure`: see the predicate note above);
`...LeafOne` runs the `k_buf_lb` 1 fallback. Routing mutants: 4 of 5
caught; `insert(entry)` back on `batch_insert` survives, as it builds the
same nodes. 60/60 in the CMake build.

Validate a new test by injecting a fault and watching it fail. An
equivalent mutant (`<` → `<=` on a covered edge) and a replacement that
matched nothing both look like coverage gaps.

Builds no CMake target makes:

```bash
# the #ifdef DEBUG box checks, asserts live
g++ -std=c++20 -O1 -pthread -DDEBUG -UNDEBUG -Iinclude -Iexternal/parlaylib/include \
  -Iexternal/googletest/googletest/include tests/self_test.cpp \
  build/lib/libgtest_main.a build/lib/libgtest.a -o st_debug
# ASAN/UBSan: gcc-toolset-15 has no libasan, and libgtest.a is not PIC
/opt/opencilk-3/bin/clang++ -std=c++20 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -fno-pie -no-pie -pthread -UNDEBUG <same -I and libs> -o st_asan
ASAN_OPTIONS=detect_leaks=0 ./st_asan --gtest_filter='SelfTest.Consolidate*'
```

`ConsolidateHeapAllocator` runs on plain new/delete because parlay's pool
recycles freed nodes and hides a use-after-free.

## Settled decisions

- The tree is a set; duplicates are a debug assert.
- Loggers are gone: `contain` → `bool`, `range_*` → `size_t`.
- These stay, each measured: the `2n` insert scratch; the four inlined
  searches; the duplicated parallel dispatch in the two recursions (only
  `partition_batch` is shared); `default_hybrid_pred`.
- The `interior_aug`/`leaf_aug` concepts check at call sites, not in the
  trait, which cannot name the range type.
- Leaf sizes above 256 are not in `self_test`; a leaf-1024 probe under
  `-DDEBUG -UNDEBUG` passed once the probe stopped reinserting survivors
  (the set contract).

## Read-through findings (2026-09-21)

Two failures reproduced with a temporary `-DNDEBUG` probe, leaf size 2;
no implementation changes:

- Reverse-comparator range queries on unsorted leaves use native `>=`/`<`
  (`range_count.hpp`, `range_report.hpp`). Build keys 9,7, insert 5 with
  the explicit unsorted policy: range [9,4) counts/reports 0 instead of 3.
  `sort_leaves()` restores both results to 3. `ReverseComparator` currently
  iterates before checking ranges, so it misses this state.
- `copy_tree_recursive` passes each leaf's current order to the normal
  leaf constructor, which assumes sorted input and sets `is_sorted=true`.
  Build 10,20, insert 1 with the unsorted policy, then copy: the original
  contains 1, but the release-build copy does not. Debug builds can assert
  in the constructor. Both copy construction and copy assignment use this
  helper.

2026-09-24, insert/delete read-through, no code change:

- No caller: `partition_binary_search_rec` (`points_op.hpp`),
  `check_tree_aug_after_delete` (`validation.hpp`).
- Stale comments from the reinsert era: `b_batch_delete.hpp` above
  `batch_delete_recursive` ("needs_fetch", `fetch_invalid_subtree_nodes`)
  and at the untouched-child reset ("the reinsertion phase"); `b_tree.h`
  says `needs_repair` is "not stored in node" (it is
  `contain_invalid_subtree`).

## History

`docs/` is gitignored; its files are tracked with `-f`:
`code_review_2026-08.md`, `refactor_plan_2026-08.md`, `findings_register.md`,
`open_issues.md`, `plan_structural_delete_2026-09.md`, `harness_notes.md`,
`new_delete_alg.md`.

The 2026-08 refactor renamed `pb_tree` → `pab_tree`, cut 4860 → 4004 lines
and ~90 → 12 public members, made `-Wall -Wextra` clean and the asserts
live in tests, and fixed seven stop-ship defects. One of them,
`batch_delete` freeing a tree whose root was merely underfull (`2a2d2ed`),
means every incremental-delete benchmark column recorded before 2026-08-17
timed a chain that had silently lost 10–50% of its batches; only
`logs/incremental_updates_new/0/{uni,norm,seg}/64.log` were rerun.
