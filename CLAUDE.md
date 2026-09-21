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
| `b_tree_impl/batch_op/b_batch_delete.hpp` | `batch_delete_`, `batch_delete_recursive`, `rebuild_from_flatten`, `consolidate_invalid_root` |
| `b_tree_impl/tree_op/node_op.hpp` | `generate_*_nodes`, `batch_insert_update_interior`, `batch_delete_update_interior_size`, `repair_rec`, `repair_node`, `balance_over_children`, `balance_over_leaves` |
| `b_tree_impl/tree_op/leaf_op.hpp` | `post_process_leaf_change`, `insert_points_2_leaf`, `delete_points_4_leaf`, `handling_leaf_split_overflow` |
| `b_tree_impl/tree_op/{flatten,points_op}.hpp` | `flatten_rec`; `partition_batch` and the two partitioners |
| `b_tree_impl/query_op/*.hpp` | membership, range count, range report, iterator |
| `b_tree_impl/validation.hpp` | `validate`, `verify_tree_property`, `check_height` |
| `tests/self_test.cpp` | dataset-free gate against `std::map` |
| `tests/{unit_test,b_test}.cpp`, `tests/test_framework.h` | data-driven tests, benchmark driver |

Build: CMake, `build/` exists; `cd build && make <target> -j`. `self_test`
and `unit_test` compile `-UNDEBUG` (asserts live); everything else is
`-O3 -march=native -DNDEBUG`. The CMake option `DEBUG` also compiles the
`#ifdef DEBUG` box-vs-split loops (O(subtree) per touched node); no test
target sets it, see the recipes under Tests.

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
  pre-existing; a nested-scope collision compiles and changes meaning) and
  a grep of string literals for the old names (an `EXPECT_DEATH` regex once
  matched an assert's text).

## Type system

`btree_type_trait<EleType, BufferSize = 64, LeafAug, InteriorAug, NodeType,
KeyCompare, NodeAllocType>`. `btree_trait<EleType, BufferSize, Options>`
names the rest: inherit `default_options` (`leaf_aug`, `interior_aug`,
`node`, `node_alloc`, `key_compare`; `void` means `std::less`, a plain alias
so the struct can be local to a function).

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

Comparator: every ordering decision goes through `TT::key_less`,
`key_equal`, `entry_less` (`a <= b` is `!key_less(b, a)`); `key_equal` is
one comparison for `std::less` or a comparator with static `equal`.
`SelfTest.ReverseComparator` (`std::greater`) is the only test of this;
five sites passed under `std::less` and failed under it, and a sixth hid
behind `#ifdef DEBUG`.

Allocator: every node comes from `TT::node_alloc::alloc<Node>()` and goes
back through `free<Node>(p)` (runs the destructor); must be thread-safe.
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
`get_node_reinsert_size` (an invalid interior reports its whole `size`),
`node_has_leaf_children` (reads child 0; never call it on a node with
`node_n == 0`, its slot 0 is a freed leaf).

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
`handling_leaf_split_overflow` (sorted leaf: `std::ranges::merge`);
`batch_insert_update_interior` has a fast path when no child split; a root
split goes through `assemble_interior_levels`. Scratch: `2n`
`node_split_pair`s, 32 B per input entry. Slots `2i`, `2i+1` belong to
input element i; that mapping is what makes the parallel sub-ranges
disjoint, so it cannot be shrunk.

`batch_delete_`: sort once; `batch_delete_recursive` marks bottom-up
(`batch_delete_update_interior_size` sets `size`, `valid`,
`contain_invalid_subtree`, `reinsert_size`; untouched interior children
get `reset_deletion_flag()`); `delete_points_4_leaf` compacts the leaf
and `post_process_leaf_change` decides `is_sorted`. Deleting an absent
key is a no-op. Then:

- root invalid and a leaf: return; `size == 0`: free the tree;
- root invalid with entries live: `consolidate_invalid_root`, else
  `rebuild_from_flatten` (flatten, free, sort, build) when its guard fails;
- root valid, interior, `size < k_buf_ub`: `rebuild_from_flatten`;
- else `repair_rec(root)`, then a root left with one child is collapsed.

**`repair_rec`** (2026-09-12, replaced flatten-and-reinsert): bottom-up.
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
`has_tomb`; `reinsert_size`/`contain_invalid_subtree` are still computed
and used only to prune the traversal.

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
`std::greater`; `expect::repair` asserts no leaf was allocated. Faults
caught: no seam repair (underfull-leaf assert), wrong concatenation
separator (box-vs-split assert), source not freed (leak check). 35/35 under
`-UNDEBUG` and `-DDEBUG -UNDEBUG`; ASAN/UBSan clean.

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
`assemble_interior_levels`. No entry leaves its leaf-parent, nothing is
sorted, scratch is a pointer and a key per leaf. Which path ran shows in
the allocator: consolidation allocates interiors and never a leaf. At 1e9
(`-t 128 -r 3`, ABBA): del 1% 6.69 → 6.02 s, 10% 2.52 → 1.96 s, small
ratios unchanged; 470 M entries in 0.020 s where the rebuild took 0.62 s.
The same bottom-up merge for the reinsert path is planned in
`docs/plan_structural_delete_2026-09.md`.

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
+3.5%). `get_entries` asserts `out.size() >= size()` at the top. An empty
or default tree answers every query empty. `flatten_rec<R, Granularity,
Sorted>`: `Sorted = true` sorts each leaf in place.

STL interface (`iterator.hpp`): `size empty max_size clear`, `contains
count find lower_bound upper_bound equal_range`, `begin/end/rbegin/rend`,
`to_seq`, `bulk_load insert erase`, `==`/`<=>`, `key_comp`. `const_iterator`
is three words and re-descends at leaf boundaries. Range overloads are
constrained with `requires std::ranges::range`; without it `erase(500u)`
binds to the template.

## Tests

| target | data | what |
|---|---|---|
| `self_test` | none, ~45 s | keys from a seed against `std::map`; leaf 2–256, five key shapes, every size where the shape changes; `--seed=N` reproduces a run exactly |
| `unit_test` | `/ssd1/zmen002/kv_pair` | real distributions at scale; skips without the data |

`check_against` validates, then iterates **before** `to_seq`/`get_entries`
(those sort every leaf and once hid an iterator bug). Special tests:
`ReverseComparator`; `CustomAllocator` (leak check, `live == 0`);
`Augmentation`; `DefaultHybridPredIsReproducible` (re-execs itself; an
in-process pair sees the same addresses and proves nothing);
`GetEntriesOutputSize` (`EXPECT_DEATH`, `death_test_style = threadsafe`);
`DeleteAbsentKeys`; `Consolidate*` and `DeleteChainTenPercent`
(deterministic collapse: sequential keys, keep every 8th — a random 10%
chain does not always collapse the root at small n).

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

## History

`docs/` is gitignored; its files are tracked with `-f`:
`code_review_2026-08.md`, `refactor_plan_2026-08.md`, `findings_register.md`,
`open_issues.md`, `plan_structural_delete_2026-09.md`, `harness_notes.md`.

The 2026-08 refactor renamed `pb_tree` → `pab_tree`, cut 4860 → 4004 lines
and ~90 → 12 public members, made `-Wall -Wextra` clean and the asserts
live in tests, and fixed seven stop-ship defects. One of them,
`batch_delete` freeing a tree whose root was merely underfull (`2a2d2ed`),
means every incremental-delete benchmark column recorded before 2026-08-17
timed a chain that had silently lost 10–50% of its batches; only
`logs/incremental_updates_new/0/{uni,norm,seg}/64.log` were rerun.
