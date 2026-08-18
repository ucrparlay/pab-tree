# ParallelMultiwayOrderedSet — Project Knowledge

## Working conventions (for Claude)

- **Keep this file updated.** After any session where new files are read, new functions are understood, or new design decisions are uncovered, append or update the relevant section here. This file is the persistent knowledge store for this project and travels across machines.

- **Check `.claude/` for memory.** At the start of each session, also check `.claude/MEMORY.md` and the memory files under `.claude/projects/` for additional persistent context (user preferences, feedback, project decisions) that supplements this file.


- Don't need to run the unit_test after the updates. just make sure the compilation is passed.

## What this project is

A parallel, cache-oblivious B-tree (`pmos::BTree<TypeTrait>`) that supports parallel batch insert, batch delete, and range queries. It lives entirely in `include/pb_tree/`.

## Library review — `docs/code_review_2026-08.md` (2026-08-17)

Full review of `include/pb_tree/`: 89 findings (13 critical, 31 high). **Read it
before changing library code** — several of its findings are traps that current
tooling cannot catch.

> NOTE: `/docs` is gitignored (`.gitignore:7`), so that file is on disk but not
> committed. `git add -f docs/code_review_2026-08.md` to track it, or move it
> somewhere unignored. The summary below is the tracked copy.

The short version:

- **The safety net is inert.** Default build is `-DNDEBUG -w`, so all 152
  `assert()`s compile to nothing and every warning is off. `Validate()` returns
  `true` unconditionally (`validation.hpp:190`) — it discards
  `VerifyTreeProperty`/`CheckHeight`, which only assert. `kCheckSize` checks no
  sizes at all. Do not treat a green `unit_test` as evidence of anything.
- **Seven stop-ship defects**, three of which fire in normal use: deleting from
  a small tree frees live entries (`b_batch_delete.hpp:85`); duplicate keys are
  silently unreachable to every query (`node_op.hpp:31`); querying an empty or
  default-constructed tree segfaults (`membership.hpp:14`, `range_count.hpp:17`,
  `range_report.hpp:26`, `utility.hpp:24`). Also: copy ctor/assign do not
  compile (`node_op.hpp:685,691`), `BTree t(seq)` is ambiguous
  (`b_tree.h:77-87`), `Build()` leaks the old tree (`b_build_tree.hpp:34`), and
  `BatchDelete(..., sorted_input=true, ...)` deletes nothing
  (`b_batch_delete.hpp:80` passes `sorted_seq` where it built `sorted_slice`).
- **The delete/rebalance path never calls `UpdateAug`**, so any non-trivial
  augmentation is correct after `Build`/`BatchInsert` and silently wrong after
  a `BatchDelete` that rebalances. `InteriorAug` cannot be implemented at all —
  `Update()` gets no arguments and no access to children.
- **What was checked and is fine**: `BalanceOverLeaves`' in-place-write
  invariants, the fixed-array bounds arithmetic, parallel slice disjointness in
  the insert path, the branchless `BinarySearch`, and the unsigned-`BufSizeT`
  underflow class.

## Refactor status (2026-08-17) -- COMPLETE

Plan: `docs/refactor_plan_2026-08.md`. Review: `docs/code_review_2026-08.md`.

All phases done, 30 commits, each gated on `unit_test`.

| | before | after |
|---|---|---|
| library | 4860 lines / 24 files | 4004 / 21 |
| `sizeof(btree)` | 296 B | 8 B |
| public surface | ~90 members | 12 + aliases |
| warnings | off via `-w` | 0 with `-Wall -Wextra` |
| asserts in tests | 0 of 152 | all live |
| include roots | 2, three spellings | 1, headers self-contained |
| lines over 80 cols | -- | 0 |
| user boilerplate | ~45 lines | 13 |
| installable | no | `find_package(pab_tree)` |

**Defects fixed.** The review's seven stop-ship items, plus four it missed:

- `RunTest` built argv from `std::to_string(x).c_str()`, seven dangling
  pointers, so the suite ran with garbage flags and the membership check never
  executed.
- `verify_tree_property` read `split[0]` on single-child interiors, where no
  split key exists. This was the "kBuf<=8 structural violation"; the tree was
  fine.
- `tree_node.h` used `parlay::is_sorted` without including
  `parlay/primitives.h`, surviving only on a transitive include from the
  deleted `basic_point.h`.
- The build path never called `update_aug`, so a fresh tree had a zero
  augmentation.

**Where measurement overrode the plan.** `range_report` clamping cost +3.5%,
so the contract is documented and asserted instead. Node predicates went
`private` rather than into a `detail` namespace: all 72 call sites are
internal. Removing the loggers is performance-neutral (325.5 vs 327.8 ns), not
the win first claimed.

**Two asserts touched, both recorded.** The wrong one in `b_batch_insert.hpp`
was kept with the correct check added beside it. The one in
`b_batch_delete.hpp` asserting `root_->size == 0` was removed: it asserted
exactly what the data-loss fix disproves.

## Conventions (set 2026-08-17)

Plan: `docs/refactor_plan_2026-08.md`.

- **Rename**: `pb_tree` → `pab_tree`; class `BTree` → `pab_tree::btree`;
  directory `include/pab_tree/`; guards `PABTREE_*`; CMake `PMOS` targets →
  `pab_tree`. `tests/` and `scripts/` follow.
- **Naming**: everything is `lower_snake_case` except `CamelCase` template
  parameters and `UPPER_CASE` macros. That covers classes, functions, members,
  enum values, type aliases **and concepts**.
  - `static constexpr` constants take a **`k_` prefix**: `kBufUB` → `k_buf_ub`,
    `kSplitFactor` → `k_split_factor`.
  - **Enum values take no prefix**: `kSorted` → `sorted`. They are values of a
    type, not compile-time sizes.
  - **Template parameters take no prefix either**, even when constant —
    `kCheckSize` → `CheckSize`.
  - Type aliases take the STL name where one exists (`value_type`, `key_type`,
    `size_type`), else bare snake_case (`entry_seq`).
  - Nothing is `UPPER_CASE` unless it is a macro. This also avoids macro
    capture: `BLOCK_SIZE` is `#define`d in `<linux/fs.h>` and
    `<sys/mount.h>`, so an `UPPER_CASE` class member of that name would be
    rewritten by the preprocessor whatever its scope.
  - Never use leading-underscore + capital or double underscore; reserved to
    the implementation. Ctor params keep their leading `_`, members stay plain.
- **Macros must be `PABTREE_`-prefixed.** The library's own macros are all
  include guards and already are. But the library *reads* `DEBUG` at 9 sites
  (`CMakeLists.txt:87` sets it) — rename to `PABTREE_DEBUG`. `DEBUG` is one of
  the most commonly defined names anywhere, and a consumer whose build sets it
  silently enables the `GetNodeBoxRec` verification loops: an O(subtree)
  traversal with heap allocation per child per interior update. Tests-only
  macros (`MEMCHECK`, `DYNAMIC_GRAPH`, `OSM`, `MAC`, `CCP`, `SIEVE`, `LEAF<N>`)
  are lower priority; `MAC` and `SIEVE` are generic enough to be worth it.
  Kernel and STL naming agree on functions, variables, structs and macros
  (checked against `/usr/include/linux/*.h` and libstdc++ 15.1.1); they differ
  on enum values and type-alias suffixes, where the STL form above wins.
- **Format**: Linux kernel style, 8-wide tabs, 80 columns. **There is no
  `BasedOnStyle: Linux`** — verified with clang-format 22.1.3, the only values
  are LLVM/Google/Chromium/Mozilla/WebKit/Microsoft/GNU. The settings are
  spelled out in `.clang-format.kernel` at the repo root (staged, not active;
  R3 renames it into place). One deviation from the kernel file:
  `Standard: c++20`, not `Cpp03` — Cpp03 writes `vector<vector<int> >`.
  Measured: all 24 headers format cleanly, most get *shorter*, and of 49 lines
  over 80 columns only 2 are code (`validation.hpp:59` string literal,
  `comparator.h:90` trailing comment) — the rest are long comment blocks.
- **Order**: delete → rename → restyle → reformat, as separate commits.
  Reformat commit goes in `.git-blame-ignore-revs`.
- **Comments**: short and concise. One or two lines, never a paragraph. No
  jargon. State the fact; add detail only if it is very important. Same for
  commit messages.
- **Do not change**: the algorithm implementations, existing asserts (adding
  new ones is fine), the submodules, or `external/baselines/`.
- **Gate**: `unit_test` after each mechanical pass. It runs here — data is at
  `/ssd1/zmen002/kv_pair/`. Elsewhere every test skips and ctest passes green,
  so a green run only means something where the data exists.
- **Settled**: the tree is a **set** (distinct keys, debug assert only);
  loggers are deleted (`contain` → `bool`); deep copy is fixed, not deleted;
  the wrong assert at `b_batch_insert.hpp:86` stays with a correct one added
  beside it.

## Build

```bash
cd build && make <target> -j$(nproc)
# targets: unit_test, b_test, pma, cpam, ...
```

Build is CMake-based. The `build/` directory already exists; no need to re-run cmake.

### `INDEX_MICROBENCH` — MassTree/BwTree are out of `all` (added 2026-08-03)

`index_microbench_workload` (the concurrent-index driver: MassTree, BwTree,
ARTOLC, skiplists) is still **configured** on every run, but carries
`EXCLUDE_FROM_ALL` unless `-DINDEX_MICROBENCH=ON` is passed:

| | default | `-DINDEX_MICROBENCH=ON` |
|---|---|---|
| part of `all` (plain `make`) | no | yes |
| `make index_microbench_workload` | works | works |
| configure-time warning | printed | silent |

`EXCLUDE_FROM_ALL` rather than an `if()` around the whole block **on purpose**:
the 8 benchmark scripts name the target explicitly (`make "${solver}"`), so
gating its *existence* would break `scalability_concurrent.sh` and
`range_query_with_log.sh`. This way the change stays entirely inside
`CMakeLists.txt` and no script needs the flag. Verified in the generated
`CMakeFiles/Makefile2`: with ON there is an `all:
CMakeFiles/index_microbench_workload.dir/all` line, without it there is not,
while the target's own `.dir/all` rule is present either way. (`make -n all` at
the top level is *not* a usable check — it only prints the recursive
`$(MAKE) -f CMakeFiles/Makefile2 all`, never the sub-make's commands.)

Why it is out of `all`: the target hard-codes `-lpapi -ltcmalloc_minimal -ltbb
-latomic` as naked link names (no `find_library`, so a miss is a link-time
error, not a configure one), and `-include masstree/config.h` pulls in a
**checked-in Linux configure output** (`HAVE_MADV_HUGEPAGE`,
`HAVE_SYS_EPOLL_H`). PAPI has no macOS port, so the target cannot build on a
Mac at all — in `all`, a plain `make` there fails on work nobody asked for.
None of it is needed to build or test the tree itself.

Package list (also in the warning): PAPI (`libpapi-dev` / `papi-devel`),
gperftools (`libgoogle-perftools-dev` / `gperftools-devel`), oneTBB
(`libtbb-dev` / `tbb-devel`), libatomic, pthreads+libm. MassTree additionally
wants `external/index-microbench/masstree/config.h` (tracked in-tree; `git
ls-files` confirms it, so no autotools run is normally needed) or a prebuilt
`mtIndexAPI.a` beside it — absent that `.a`, CMake compiles the 17 masstree
`.cc`/`.c` sources directly and says so in a warning. That `mtIndexAPI.a`
warning and the `libatomic was not found` one (macOS only) fire on every
configure, exactly as they did before this change, since the target is still
configured.

**Formatting gotcha**: `message()` reflows any line starting at column 0, which
mangled the package table on the first attempt. Every line of that warning is
indented by ≥2 spaces on purpose — CMake leaves indented lines preformatted.

## Key source files

| Path | Role |
|------|------|
| `include/pb_tree/b_tree.h` | `BTree<TT>` class declaration + all type aliases |
| `include/pb_tree/dependence/type_trait.h` | `BTreeTypeTrait` template — defines all size constants |
| `include/pb_tree/dependence/tree_node.h` | `LeafNode`, `MultiWayNode` (interior) structs; alloc/free helpers |
| `include/pb_tree/b_tree_impl/tree_op/node_op.hpp` | `BalanceOverLeaves`, `GenerateLeafNodes`, `GenerateInteriorNodes`, `BatchInsertUpdateInterior`, etc. |
| `include/pb_tree/b_tree_impl/tree_op/leaf_op.hpp` | `InsertPoints2Leaf`, `DeletePoints4Leaf`, `HandlingLeafSplitOverflow` |
| `include/pb_tree/b_tree_impl/batch_op/b_batch_delete.hpp` | `BatchDelete`, `BatchDeleteRecursive`, `FetchInvalidSubtreeNodes` |
| `include/pb_tree/b_tree_impl/batch_op/b_batch_insert.hpp` | `BatchInsert`, `BatchInsertRecursive` |
| `include/pb_tree/b_tree_impl/batch_op/b_build_tree.hpp` | `Build`, `BuildRecursive` |
| `tests/b_test.cpp` | Main benchmark/test driver |
| `tests/unit_test.cpp` | Unit tests (uses GoogleTest) |
| `tests/wrapper.h` | Concrete element/augmentation types for tests |

All `b_tree_impl/**/*.hpp` files are `#include`d at the bottom of `b_tree.h`; they are not compiled separately.

## Two test suites (self_test added 2026-08-17)

| target | data | runtime | what it is |
|---|---|---|---|
| `self_test` | none | ~7 s | generates keys from a seed, checks against `std::map` |
| `unit_test` | `/ssd1/zmen002/kv_pair` | minutes | real distributions at scale, **skips** without the data |

Both are `-UNDEBUG` and both are ctest tests. `self_test` is the gate that
works anywhere; `unit_test` is unchanged.

`./self_test --seed=N` reproduces a run **exactly** — verified by hashing two
runs with the timing text stripped. Default seed is fixed (20260817), printed
on startup. A failure prints leaf size, input size, key shape and seed via
`SCOPED_TRACE`, plus the rerun flag.

Covers build / batch_insert / batch_delete / contain / range_count /
range_report / get_entries / to_seq / iteration / find / lower_bound /
upper_bound / equal_range / augmentation / copy-move-swap-clear, over leaf
sizes 2–256, five key shapes (uniform, clustered, sequential, sparse,
boundary), and every size where the structure changes shape (0,1,2,3,…,1025).

### It was validated by breaking the library, not by passing

| injected fault | caught? |
|---|---|
| drop an `update_aug` on the delete path | yes — `Augmentation` |
| off-by-one in the leaf delete compaction | yes — SIGABRT |
| `range_count` skips a covered middle child | yes — 20 of 14 tests |
| iterator skips `ensure_leaf_sorted` | yes, **after fixing the test** |
| `range_count` right edge `<` → `<=` | no — the mutant is *equivalent* |

Two things worth keeping:

- **The iterator fault initially passed.** `check_against` called `to_seq()`
  before iterating, and `to_seq` runs `flatten_rec<sorted=true>` which sorts
  every leaf — so iteration never saw an unsorted leaf. **Iteration must be
  checked before `to_seq`/`get_entries`.** Anything added to that function
  that sorts leaves has to go after the iteration block.
- **The `<`→`<=` mutant is not a coverage gap.** With `<=` the extra child
  pulled into the middle sum lies entirely below `second`, and the child then
  recursed into starts at `second` and contributes 0. Same answer, more work.

A no-op fault injection looks exactly like a coverage gap. The first
`range_count` attempt replaced `get_node_id_containing_key_exclusive`, a string
that does not occur in that file, so nothing changed and the suite "passed".
Confirm the mutant actually differs before concluding anything.

### `multi_way_node` needed `get_aug()`

`leaf_node` had `get_aug()`; the interior did not, so an interior augmentation
could be written but not read back. Added both overloads. `aug` was already
public (`multi_way_node` is a `struct`), so this is an accessor, not a change
in access.

## STL container interface (added 2026-08-17)

`btree` now carries the API common to TLX's four B+-tree containers and to
`std::set`, on top of the batch interface. `include/pab_tree/b_tree_impl/query_op/iterator.hpp`
holds the iteration half.

| Group | Members |
|---|---|
| types | `key_type` `mapped_type` `value_type` `difference_type` `size_type` `key_compare` `value_compare` `iterator` `const_iterator` `reverse_iterator` `const_reverse_iterator` |
| capacity | `size` `empty` `max_size` `clear` |
| lookup | `contains` `count` `find` `lower_bound` `upper_bound` `equal_range` |
| iteration | `begin` `end` `cbegin` `cend` `rbegin` `rend` `to_seq` `sort_leaves` |
| modifiers | `bulk_load` `insert(range)` `insert(entry)` `erase(range)` `erase(key)` `erase(iter)` `erase(first,last)` |
| compare | `operator==` `operator<=>` |
| observers | `key_comp` `value_comp` |

Not adopted: `get_allocator` (no allocator parameter yet), `insert(hint, x)`,
`erase_one` (a duplicate of `erase(key)` here, since keys are distinct),
`print`/`print_leaves`. `validate` is the existing spelling of TLX's `verify`.

### Iteration rests on one structural fact

Leaves are ordered **against each other** — the B+-tree invariant — but a
leaf's own entries are not necessarily sorted (`is_sorted`, the deliberate
performance trick that `sorted_leaf_ratio` measures). So ordered iteration
only needs the leaf currently under the cursor to be sorted, and
`ensure_leaf_sorted` does that on arrival. This is the same lazy-sort that
`extract_points_in_leaf<sorted=true>` already performed for `to_seq`.

**That write means iteration and lookup are not thread-safe by default.** Call
`sort_leaves()` first (one parallel pass, idempotent) and afterwards both only
read.

An earlier draft called `sort_leaves()` from inside `begin`/`find`/
`lower_bound`/`upper_bound`. That is O(n) *per lookup* — probing n keys became
O(n²) and the test hung. Per-leaf sorting is what makes lookups O(log n).

### `const_iterator` carries no path stack

Crossing a leaf boundary re-descends from the root via `next_leaf`/`prev_leaf`,
which track the deepest node that still had a child on the far side. That is
O(height) per leaf, so O(height / k_buf) amortised per entry, and it keeps the
iterator 3 words wide rather than holding an O(height) stack. Entries are
immutable (`iterator` is `const_iterator`, as in `std::set`) because moving one
would break the leaf holding it.

### Overload traps found while writing this

- `erase(K)` vs `erase(Range&&)`: `erase(500u)` bound to the **template**,
  because binding `unsigned&&` to an rvalue outranks `const unsigned&`, and
  that tiebreak is applied before the non-template preference. Both range
  overloads are now constrained with `requires std::ranges::range<...>`.
- `CHECK(t.validate<true, false>())` does not compile — the comma inside the
  template argument list splits the macro's arguments.

## Type system

The tree is parameterised by `TypeTrait = BTreeTypeTrait<EleType, kBufferSize, LeafAug, InteriorAug, kSkHeight, NodeType>`.

### Key size constants (all in `BTreeTypeTrait`)

```
kBuf   = kBufferSize          (minimum/target leaf size)
kBufLB = kBufferSize          (leaf validity lower bound: size < kBufLB → invalid)
kBufMD = 2 * kBufferSize      (average/split target)
kBufUB = 4 * kBufferSize      (max nodes per interior / max entries per leaf)
```

Typical values in tests: `kBuf` ∈ {16, 32, **64** (default), 128, 256, 512, 1024}.

### Type aliases used everywhere

```cpp
using SizeT    = uint_fast32_t;   // entry/size counts
using BufSizeT = <unsigned>;      // node/slot counts -- see WARNING below
using SizeBufUBArr    = std::array<SizeT,    kBufUB>;
using BufSizeBufUBArr = std::array<BufSizeT, kBufUB>;
using BooleanBufUBArr = std::array<bool,     kBufUB>;
using EntryArr = std::array<E, kBufUB>;   // fixed-size leaf element buffer
```

**WARNING — `BufSizeT` is UNSIGNED, not `int`.** This doc said `int` until
2026-08-17; that was stale. `type_trait.h:118-123` picks it by width:

```cpp
using BufSizeT = std::conditional_t<(kBufferSize * kBufUBFac > 65535U),
                   uint_fast32_t,
                   std::conditional_t<(kBufferSize * kBufUBFac > 255U),
                     uint_fast16_t, uint_fast8_t>>;
```

So `kBuf=64` → `kBufUB=256` → `uint_fast16_t`; `kBuf=32` → `kBufUB=128` →
`uint_fast8_t`. Consequences for anyone editing node code:

- A backward loop `for (BufSizeT i = n-1; i >= 0; --i)` **never terminates**.
  The library's one backward loop (`node_op.hpp:440`) is explicitly `int` and
  carries a `// WARN: signed` comment — keep it that way, and use `int` for any
  new one.
- Every `- 1` on a count must be guarded. Existing sites are
  (`GetSplitN()` returns `node_n == 0 ? 0 : node_n - 1`); match that pattern.
- `BinarySearch` returns `size_t`; assigning to `BufSizeT` narrows silently
  (the build passes `-w`, see below).

A 2026-08-17 review checked every `-1` site and backward loop in the library and
found no live underflow — but the guarding is by convention, not by type.

## Node types

### `LeafNode<TT>` (= `BTree::Leaf`)

- `bool is_leaf = true`
- `size_t size` — number of live elements
- `EntryArr elements` — fixed-size array of `E` (capacity = `kBufUB`)
- `bool is_sorted` — true when elements are in key order
- A leaf is **valid** iff `size >= kBufLB`

### `BTree::BTreeInteriorNode` (= `BTree::Interior`)

Inherits `MultiWayNode<TT>`:
- `BufSizeT node_n` — number of children (≤ kBufUB)
- `NodeArr tree_nodes` — array of child `Node*` (size kBufUB)
- `SplitArr split` — separator keys (size kBufUB; `node_n - 1` are live)
- `bool valid` — interior is valid iff `(node_n - invalid_children) >= kBufLB` (or > 0 for root)
- `SizeT invalid_subtree_num` — count of invalid subtrees below
- `SizeT reinsert_size` — entries that need re-insertion after deletion
- `ResetDeletionFlag()` — clears valid/invalid_subtree_num/reinsert_size

### Node validity helpers

```cpp
GetNodeInvalidStatus(T)     // leaf: size < kBufLB; interior: !valid
NodeIsValid(T)              // !GetNodeInvalidStatus(T)
NodeContainInvalidSubtree(T) // leaf: 0 or 1; interior: contain_invalid_subtree
GetNodeReinsertSize(T)      // leaf: size if invalid, else 0; interior: reinsert_size
NodeHasLeafChildren(T)      // true if first child is a leaf
SelfBalanceNode(T, tomb)    // NodeHasLeafChildren && tomb && NodeIsValid
```

## Bucket arithmetic

```cpp
GetBktNum(item_n)            = item_n / kBufMD        // target # of output buckets
GetBktStIdx(i, bkt_n, n)     = (n * i) / bkt_n        // start index of bucket i
GetBktEndIdx(i, bkt_n, n)    = (n * (i+1)) / bkt_n    // end index of bucket i
```

## Deletion path (high-level)

1. `BatchDelete` → sorts keys → `BatchDeleteRecursive` (marks invalid nodes, sets `reinsert_size`)
2. `FetchInvalidSubtreeNodes` — traverses tree, compacts invalid subtrees:
   - If `SelfBalanceNode` (leaf-level interior, tomb flag): calls `BalanceOverLeaves`
   - Otherwise recursively handles invalid subtrees, then calls `ReInsertTreeAfterDelete`

## `BalanceOverLeaves` — optimized implementation (node_op.hpp:425)

Called when an interior node's direct children are all leaves and some are invalid (underfull). Groups each invalid leaf with an adjacent valid pivot leaf and redistributes entries.

### Algorithm

**Backward pass** (right→left): assigns each node to its group pivot (the nearest valid node to its right). Simultaneously accumulates `group_sum[pivot]` = total entries in that group. Trailing invalid nodes (right of all valid nodes) are fixed up in a forward pass to belong to `rightmost_pivot_idx`.

**Main forward loop**: for each group `[i, j)`:
- **Case 1** (j == i+1, single valid node): copy pointer/split directly in-place → O(1)
- **Case 2** (sum < kBufUB, merge): copy all invalid leaves' entries into the valid leaf via a running pointer, free source leaves → O(sum)
- **Case 3** (distribute): gather all entries into `entry_buf`, `std::sort`, scatter back into buckets

### Key design decisions in the current optimized version

1. **`group_sum` precomputed in backward pass** — avoids `->size` pointer dereferences in the inner group-end-finding loop.
2. **Write directly to `TI->tree_nodes`/`TI->split`** — no intermediate `new_tree_nodes`/`new_splits` arrays, no final O(kBufUB) copy. Safe because invariant `tree_idx ≤ i` holds throughout (verified for all three cases including that outer-split reads from `TI->split[j-1]` never alias write targets).
3. **Lazy `entry_buf` allocation** — `typename TT::EntrySeq entry_buf` starts empty; allocated only on the first case-3 group. Common paths (cases 1 & 2) pay zero heap allocation cost.
4. **Running pointers in copy loops**:
   - Case 2 merge: `dst` pointer advances instead of re-calling `GetEleEndIter()` per iteration; final size computed as `dst - begin`.
   - Case 3 gather: `ep` pointer advances with each `std::copy_n` result.
   - Case 3 scatter: `sp` pointer advances; split key read as `Entry::GetKey(*sp)` right after advance, avoiding `GetBktStIdx` call.
5. **`ele_l` tracked across scatter iterations** — computes `bkt_size = ele_r - ele_l` without calling `GetBktStIdx` per bucket.
6. **`[[likely]]`/`[[unlikely]]` hints** — single-node case (most common) marked `[[likely]]`; entry_buf allocation marked `[[unlikely]]`.

### Correctness invariants for direct-write

At the start of group `[i, j)`: `tree_idx == split_idx` (non-rightmost groups keep them equal; rightmost group produces Δtree = Δsplit + 1 at the very end). And `tree_idx ≤ i`. This ensures all write positions are ≤ all read positions, making in-place compaction safe.

## Parallelism

Uses `parlay::parallel_for` for tree-level operations. `BalanceOverLeaves` itself is sequential (operates on a single interior node's children which is bounded by kBufUB).

## Range report implementation notes

- `RangeReport` lives in `include/pb_tree/b_tree_impl/query_op/range_report.hpp`.
- Boundary nodes (`l`, `r`) are still handled with recursive filtering against `[query_l, query_r)`.
- Fully covered middle subtrees now use `RangeReportFlattenRec` (const traversal) rather than generic `FlattenRec`:
  - leaf case performs a single `std::copy_n` into output,
  - interior case descends children left-to-right and appends contiguously.
- Sorted-leaf filtering path in `RangeReportLeaf` uses `BinarySearchKey` bounds plus one bulk `std::copy_n` instead of per-element assignment.
- Empty ranges (`query_box.first >= query_box.second`) return immediately.
- `FlattenRec` (`tree_op/flatten.hpp`) now precomputes per-child offsets in O(node_n) before `parallel_for`, removing prior O(node_n^2) prefix-sum work inside each task.

## Point-membership query path (`ContainRec`)

`Contain(k)` → `ContainRec(root, k)` in `query_op/membership.hpp`.

### Key functions on the hot path

| Function | Location | Role |
|---|---|---|
| `ContainRec` | `membership.hpp` | Iterative descent; follows child pointers until leaf |
| `GetNodeIdContainingKey(k)` | `tree_node.h:333` | Binary search on `split[]` → child index |
| `Leaf::Contain(k)` | `tree_node.h:115` | Binary search on `elements[]`; branches on `is_sorted` |
| `BinarySearch<8>` | `dependence/binary_search.h` | Core search primitive used by all the above |

### `kBinarySearchBase` — cache-line-sized linear scan base

Both `LeafNode` and `MultiWayNode` expose a `static constexpr BufSizeT kBinarySearchBase` that sets the linear-scan threshold for `BinarySearch<N>`. It is computed from the hardware cache line size at compile time using `std::hardware_constructive_interference_size` (from `<new>`, C++17):

```cpp
// LeafNode — searches E (entry = key + val)
static constexpr TT::BufSizeT kBinarySearchBase =
    std::hardware_constructive_interference_size / sizeof(E);

// MultiWayNode — searches K (split keys only)
static constexpr BufSizeT kBinarySearchBase =
    std::hardware_constructive_interference_size / sizeof(TT::K);
```

They differ because leaf search scans entries (`sizeof(E)`) while interior search scans bare split keys (`sizeof(K)`). Example with key=uint64+val=uint32 (E=16B, K=8B) on a 64B-line machine: leaf base=4, interior base=8.

`hardware_constructive_interference_size` (not `destructive`) is correct here: it gives the max size that fits within one cache line, which is exactly the linear-scan budget we want.

### Optimizations already applied

- **`ContainRec` is iterative** (not recursive): eliminates O(height) function-call overhead; the former tail-recursive form is now a `while (!T->is_leaf)` loop.
- **`BinarySearch` is branchless**: both the binary phase and the final linear scan are branchless.
  - Binary phase: `bool go = less(I[mid]); lo = go ? mid+1 : lo; hi = go ? hi : mid;` — compiles to `cmov`, no mispredictions.
  - Linear phase: `count += less(I[i]) ? 1 : 0` loop instead of early-exit — trades ≤ `_base/2` extra comparisons for zero branch mispredictions in the tail.
- These improvements benefit **all** `BinarySearch<8>` callers: `GetNodeIdContainingKey`, `GetNodeIdContainingKeyExclusive`, `Leaf::BinarySearchKey`, `Leaf::BinarySearchKeyExclusive`, and range-query paths.

### What `GetNodeIdContainingKey` does

Returns first `i` where `split[i] > k` (= the child subtree containing `k`). Implemented as `BinarySearch<8>(GetSplitSlice(), [&](K key){ return key <= k; })`. `GetSplitSlice()` returns a `parlay::slice` over `split[0..node_n-2]`.

### Leaf search note

`Leaf::Contain` branches on `is_sorted`. Leaves become unsorted after the merge case in `BalanceOverLeaves` (`is_sorted = false`); the unsorted fallback is an O(kBufUB) linear scan via `std::ranges::find_if`.

## Range query path (`RangeReport` / `RangeCount`)

Both live in `query_op/range_report.hpp` and `query_op/range_count.hpp`.

### Algorithm

At each interior node: find `l` = first child overlapping `[first, second)`, `r` = last such child. Visit boundary children `l` and `r` recursively; middle children `(l, r)` are fully covered.

### Optimizations applied

1. **Iterative descent** (same idea as `ContainRec`): the `while (!T->is_leaf)` loop avoids O(height) recursive call overhead when the query falls in a single child at each level (narrow queries).

2. **Narrowed search for `r`**: since `r ≥ l`, the binary search for `r` starts from index `l` in the split array instead of 0. This halves the search range in the common case.

3. **Same narrowing in leaf `r` search**: `RangeReportLeaf` and `RangeCountLeaf` search for the upper bound `r` starting from `l` rather than 0.

4. **Parallel flatten of middle children** (`RangeReportRec`): pre-computes each middle child's output offset using `nodes[i]->size`, then launches `parlay::parallel_for` over the middle children, each calling `FlattenRec<Range, true, false>` on its own non-overlapping output slice.

5. **Bug fix**: original code called `FlattenRec(nodes[i], offset, out, logger)` with 4 args but `FlattenRec` takes 2 — never caught because the template was never instantiated in unit_test. Fixed to call `FlattenRec<Range, true, false>(nodes[i], out.cut(start, end))`.

6. **Branchless count in unsorted leaf** (`RangeCountLeaf`): original code had a dead `cnt` variable and wrote directly to `offset++`. Replaced with `cnt += condition ? 1 : 0` (no early-exit branch), then `offset += cnt`.

### Key type note

`GetNodeIdContainingKeyExclusive(k)`: returns first i where `split[i] >= k` — used for the right boundary. If `split[i] == k`, tree_nodes[i] is still visited (elements in tree_nodes[i] have keys in `[split[i-1], split[i])`, all `< k = split[i]`, so they belong in the query range `[first, second)`).

### Template instantiation note

`RangeReportRec` and `RangeCountRec` are templated on `Range` and are never instantiated in `unit_test`. Always compile `b_test` to catch template body errors in these functions.

## Batch insert path (`BatchInsert_` / `BatchInsertRecursive`)

### `BatchInsert_` (`b_batch_insert.hpp`)

Entry point. Sorts input once via `parlay::sort`, then calls `BatchInsertRecursive` on the root. If the root splits (returns `new_node_n > 0`), assembles the new root via `AssembleInteriorLevels`.

### `BatchInsertRecursive` (`b_batch_insert.hpp`)

Recursive parallel descent. Input `In` is always a sorted slice (sorted once in `BatchInsert_` before descent — all recursive calls pass `In.cut(l, r)` which are subarrays of the sorted input).

- **Leaf case**: if fits (`size + n <= kBufUB`) → `InsertPoints2Leaf` in-place; else → `HandlingLeafSplitOverflow`.
- **Interior case**: partitions input across children using `SplitBatchBinarySearch` (when `n > kSplitFactor * split_n`) or `SplitBatchMerge`; recurses in parallel via `parlay::parallel_for` with granularity 1.
- `new_node_n` is accumulated in O(cnt) — iterating only touched children — starting from `GetNodeN()` and adding `split_n - 1` for children that split. `return_node_n_arr[i]` is always 0 or ≥2 (never 1), so this is correct.

### `BatchInsertUpdateInterior` (`node_op.hpp`)

Updates the interior node after recursive insert.

**Fast path** (common case): if `new_node_n == TI->GetNodeN()` (no child split), only recomputes `TI->size` by summing child sizes and calls `UpdateAug`. Avoids two O(kBufUB) `std::array` copies (~4KB) that would otherwise rebuild `tree_nodes`/`split` to identical values.

**Why the invariant holds**: `return_node_n_arr[i]` is always 0 or ≥2, because `GenerateLeafNodes` and `GenerateInteriorNodes` always produce ≥2 buckets (since input to overflow path has ≥ kBufUB elements, and `GetBktNum = item_n / kBufMD`, so ≥2). Therefore `new_node_n == GetNodeN()` iff all `return_node_n_arr[i] == 0`.

**Slow path** (some child split): rebuilds `new_nodes`/`new_splits` arrays in a temporary, then moves into `TI`. Cannot write directly in-place because the expansion ratio means write index overtakes read index.

### `HandlingLeafSplitOverflow` (`leaf_op.hpp`)

Handles leaf overflow. Allocates a merged entry buffer, then splits via `GenerateLeafNodes`.

**Sorted fast path**: when `TL->is_sorted` is true (after `Build` or after distribute in `BalanceOverLeaves`), uses `std::ranges::merge` O(n) instead of concatenate + `parlay::sort` O(n log n). Safe because `In` is always sorted.

### `AssembleInteriorLevels` (`b_build_tree.hpp`)

Shared helper used by both `BuildRecursive` and `BatchInsert_` (when root splits). Takes a flat (node, key) slice and iteratively calls `GenerateInteriorNodes` until the result fits in a single root. Allocates its own aux ping-pong buffers internally. Replaced the duplicated while-loop that existed in both call sites.

### Points-op helpers (`points_op.hpp`)

- `SplitBatchBinarySearch`: sequential binary searches narrowing the range, O(split_n · log(n/split_n)).
- `SplitBatchMerge`: single O(n + split_n) merge pass; used when `n <= kSplitFactor * split_n`.
- `SeievePoints`: parallel histogram-based scatter (used in `BatchInsertSieve` / `-DSIEVE` path only).

## Batch delete path (`BatchDelete_` / `BatchDeleteRecursive`)

### `BatchDelete_` (`b_batch_delete.hpp`)

Entry point. Sorts input keys once via `parlay::sort`, then calls `BatchDeleteRecursive` on the root. If the result is `invalid_flag` (tree emptied), calls `DeleteTree`. If any invalid subtrees remain, calls `FetchInvalidSubtreeNodes` with `has_tomb = true` to compact and reinsert.

### `BatchDeleteRecursive` (`b_batch_delete.hpp`)

Recursive parallel descent. `In` (sorted key slice) is partitioned across children using `SplitBatchBinarySearch` or `SplitBatchMerge` (same threshold as insert). Results `(invalid_flag, invalid_subtree_num, reinsert_size)` per child are collected, then `BatchDeleteUpdateInteriorSize` aggregates them into the current node's validity state.

**Precomputed l/r**: `touched_l[cnt]` / `touched_r[cnt]` are stored alongside `nodes_map[cnt]` to avoid recomputing child ranges inside the `parallel_for` lambda.

**Hoisted `NodeHasLeafChildren`**: whether children are interior nodes is computed once before the loop (same result for every child of a given interior node), avoiding a repeated function call per untouched child.

**Untouched children**: `invalid_subtree_flag[i] = false`, counts = 0 are set directly; if children are interior nodes, `ResetDeletionFlag()` is called so the reinsertion phase sees clean state.

### `DeletePoints4Leaf` (`leaf_op.hpp`)

`In` is always sorted (sorted once in `BatchDelete_` before descent). Replaces the original O(n × m) scan (linear search per key × leaf size) with:

1. If `!TL->is_sorted`: sort the leaf in-place O(m log m), set `is_sorted = true`.
2. Two-pointer O(m + n) compaction: walk sorted leaf and sorted `In` together, copying surviving elements to a write cursor.

Result: O(m + n) for already-sorted leaves, O(m log m + n) otherwise — vs O(n × m) previously. The result is always sorted, so `is_sorted` stays `true`.

### `BatchDeleteUpdateInteriorSize` (`node_op.hpp`)

Aggregates per-child validity info into the current interior node. Updates `TI->size`, `TI->valid`, `TI->invalid_subtree_num`, `TI->reinsert_size`. Called unconditionally from `BatchDeleteRecursive` (unlike insert, there is no fast path here because validity state always needs updating even when no children became invalid).

### `FetchInvalidSubtreeNodes` / `BalanceOverLeaves`

See `BalanceOverLeaves` section above — these are shared between delete and the general rebalancing path.

## Leaf sort policy (`PostProcessLeafChange` / `PseudoRandomGenPattern`)

### What it does

After any leaf mutation (insert, delete, balance-merge), `PostProcessLeafChange` is the single authority for setting `is_sorted`. It branches on `PatternType` (`b_tree_impl/tree_op/leaf_op.hpp`):

| Pattern | Action |
|---|---|
| `kSorted` | Always `std::sort` the leaf; `is_sorted = true` |
| `kUnsorted` | Skip sort; `is_sorted = false` |
| `kHybrid` | Call `pred(T, In.size())` — sort if returns `true`, mark unsorted if `false` |

### Call sites (all three delegate here)

- `InsertPoints2Leaf` — after appending batch entries to the leaf end
- `DeletePoints4Leaf` — after swap-to-end removal (leaf order scrambled)
- `BalanceOverLeaves` case B merge — after compacting invalid leaves into the pivot

### User-supplied hybrid predicate

The `kHybrid` decision function is a **template parameter `LeafSortPred`** threaded through the entire call stack — zero runtime overhead, fully inlinable. The predicate signature is:

```cpp
bool leaf_sort_pred(Node const* T, size_t batch_size);
```

Users pass a lambda (or any callable) at `BatchInsert` / `BatchDelete` call sites:

```cpp
tree.BatchInsert(data, kHybrid, /*sorted=*/false,
    [](auto const* T, size_t n) { return T->size + n > threshold; });
```

### Default: `DefaultHybridPred`

Defined as a nested struct in `BTree<TT>` (declared before `BatchInsert` in `b_tree.h` so it can serve as a default template argument). Computes a Knuth multiplicative hash of `(T->size + batch_sz)` and returns the low bit — effectively a ~50/50 coin flip with no global state:

```cpp
struct DefaultHybridPred {
    template<typename N>
    bool operator()(N const* T, size_t batch_sz) const noexcept {
        return (static_cast<size_t>(T->size) + batch_sz) * 2654435761ULL & 1;
    }
};
```

The no-pattern `BatchInsert(Range&&)` / `BatchDelete(Range&&)` overloads hardcode `kHybrid` and pass `DefaultHybridPred{}`.

### Threading chain

```
BatchInsert(In, pattern, sorted, pred)
  └─ BatchInsert_(In, pattern, sorted, pred)
       └─ BatchInsertRecursive(T, In, node_split_slice, pattern, pred)
            └─ InsertPoints2Leaf(T, In, pattern, pred)
                 └─ PostProcessLeafChange(T, In, pattern, pred)

BatchDelete(In, pattern, sorted, pred)
  └─ BatchDelete_(In, pattern, sorted, pred)
       ├─ BatchDeleteRecursive(T, In, pattern, pred)
       │    └─ DeletePoints4Leaf(T, In, pattern, pred)
       │         └─ PostProcessLeafChange(T, In, pattern, pred)
       └─ FetchInvalidSubtreeNodes(root, entries, tomb, pattern, pred)
            ├─ BalanceOverLeaves(TI, reinsert_size, pattern, pred)   [case B merge]
            │    └─ PostProcessLeafChange(dest, slice, pattern, pred)
            └─ ReInsertTreeAfterDelete(T, entries, pattern, pred)
                 └─ BatchInsertRecursive(…, pattern, pred)
```

### `PseudoRandomGenPattern` (kept but no longer on the hot path)

The original coin-flip helper (`b_tree.h:396`) that additionally XORs in address bits from `In.begin()`/`In.end()` to break per-call-site correlation. Still exists but is not called internally; the default is now `DefaultHybridPred`.

### Important: `DefaultHybridPred` must be declared before `BatchInsert` in the class body

C++ requires default template arguments to reference names already declared at that point. `DefaultHybridPred` is placed immediately before the `BatchInsert` declarations in `b_tree.h`.

## Concrete element type in tests

Default: `Wrapper<uint32_t, uint32_t>::Element` → key=uint32_t (4B), val=uint32_t (4B) → E is 8 bytes.
OSM variant: key=uint64_t.
Dynamic graph: key=tuple<uint32_t,uint32_t,uint32_t>.

## Benchmark dataset files (`/data/zmen002/kv_pair/...`)

Generated by `data_generator` (`tests/data_generate.cpp` + `tests/data_op.h`).

### Directory layout

`<root>/<dis>/<base_n>_<val_n>/<ins|del>/<base_n/batch_n>/<i>.{base,batch}.in`
e.g. `uni/100000000_1/del/10/1.base.in` = uniform, 100M base entries, 1 value
column, delete workload, batch = 10% of base, file #1 (rand_seed = 0).

### Text `.in` format

Header line `<N> <dim>` (dim = 1 key + #value columns), then N lines of
`<key> <val>`. Keys are **rank-discretized** distribution draws: real samples
from base+batch are sorted together and each key is its rank — distinct
integers preserving the distribution shape. Entries are then random-shuffled
and `val` = post-shuffle position (0,1,2,...). For delete workloads the base
is a dense permutation of `0..base_n-1` and the batch keys ⊆ base; for insert
workloads base and batch keys are disjoint, jointly covering
`0..base_n+batch_n-1`.

### Binary `.bin` format (`tests/binary_io.h` — single source of truth)

Little-endian: `uint64_t header[3] = {N, dim, width}` followed by `N*dim`
values of `width` bytes each, row-major (`key, val, ...`). `width` is 4 when
every value fits in uint32 (writers narrow automatically via
`WriteBinaryEntries`), else 8. `ReadBinaryEntries` widens back to uint64
transparently. All writers/readers go through these two helpers — **older
2-word-header `.bin` files are incompatible; regenerate them**. Producers:

- `data_to_binary -p <file.in> [-o <file.bin>]` — converts an existing text
  file (default output replaces `.in` with `.bin`).
- `scripts/convert_to_binary.sh <dir> [-f] [-d]` — recursively converts every
  `.in` under `<dir>`; skips up-to-date `.bin` twins unless `-f`; `-d` deletes
  each `.in` after its successful conversion (skipped files are kept).
- `data_generator ... -binary 1` — generates directly to `.base.bin`/
  `.batch.bin` via `PrintToBinaryFile` (`data_op.h`) instead of text.
- `temporal_graph_osm -p <graph> -o <out_dir> [-binary {0,1}]` — `-binary 1`
  writes `<i>_insert.bin`/`<i>_delete.bin` (`[N, 2, src, dest, ...]`) instead of
  `.in`. Uses the `commandLine` parser (`common/parse_command_line.h`): `-p`/`-o`
  are required (empty → usage), `-h`/`-help` print usage.
- `temporal_graph_signal -p <graph> -o <out_dir> [-binary {0,1}]
  [-group_by_year {0,1}]` — converts a *signal* graph (edge appearances, no
  insert/delete) into a single `0.in`/`0.bin` (`[N, 3, src, dest, timestamp,
  ...]`). Timestamps are Unix epoch seconds; `-group_by_year 1` (default) labels
  edges by calendar year `1..#batches`, `-group_by_year 0` renumbers distinct
  timestamps `1..#timestamps`. Same `commandLine` interface as above.
- `filter_csv_year <in.csv> <out>` — if `<out>` ends with `.bin`, writes
  binary (`[N, 2, code, time, ...]`) instead of text.

### Generator gotcha

`data_generator` **segfaults** if `-base_param`/`-batch_param` are omitted
(`std::string(nullptr)` from `getOptionValue`) — pass dummies (e.g. `1`) even
for uniform, which ignores them. `incre_data_generator` uses the
`getOptionValue(opt, default)` **string** overload instead and does not have
this bug.

### `incre_data_generator` (`tests/incre_data_generate.cpp`)

Datasets for the incremental-update tags (`1<<6` / `1<<7`), added 2026-07-30.

**The problem it solves.** `data_generator`'s `Generator::ToEntry`
random-shuffles the whole key set before writing, so slicing that file into
consecutive chunks — exactly what `BatchInsertByStep`/`BatchDeleteByStep` do —
gives every batch a *uniform* sample of the key space no matter what `-dis`
said. The distribution survives only globally, never per batch, so `exp` and
`uni` produce statistically identical incremental workloads.

**Construction.** Base keys are the dense uniform space `[0, base_n)` (key ==
rank; a rank-discretised base comes out dense anyway, see `Discreatize`). Then
repeatedly, over a shrinking pool of surviving keys:

```
draw batch_n positions from the requested distribution, mapped onto [0, |pool|)
  -- these are RANKS INTO THE POOL, not keys
batch <- pool entries at those ranks;  pool <- pool minus batch
```

Output = the batches concatenated **in generation order**, shuffled *within*
each batch but not across. So the harness's existing slicing picks the batches
up unchanged. Verified at 1e6/1e4 (`exp`, λ=10): batch mean walks 0.10 → 0.15 →
… → 0.99 of the key space with a tight p10–p90 band, i.e. a clustered hotspot
that migrates as the front is thinned. `norm` starts tight around 0.5 and
spreads outward as the middle is consumed; `uni` stays flat (correctly).

**Flags.** Same shape as `data_generator`: `-path -base_n -batch_n -val_n 1
-file_num -dis {0,1,2,3} -batch_op {0,1} -binary {0,1} -batch_param <csv>`,
plus `-validate {0,1}` (full distinctness check; the cheap count/sum/max
permutation check always runs). `-batch_param` is now a **fraction of the
surviving pool**: exp → λ (default 10); norm → mean,dev (0.5,0.2); dual_norm →
mean1,dev1,mean2,dev2 (0.25,0.05,0.75,0.05).

**`dual_norm` changed meaning.** In `data_generator` "dual" meant base *and*
batch normal. Here the base distribution is meaningless (the base is the whole
dense key space), so `-dis 3` is a **two-mode batch** instead — otherwise it
would be identical to `-dis 2`.

### `seg` (`-dis 4`) — a sliding segment

Each batch of `m` is drawn **uniformly from the leading `mult * m` surviving
keys** (`-batch_param mult`, default 2), then removed. So the hot region is a
fixed-width window of *ranks* that advances by exactly `m` per batch.

Implemented as `TargetDensity == 1.0` (flat, like `uni`) plus
`WindowSize == mult * want`. All of the shape lives in the window; nothing else
in the sampler changes.

**It has the one property `exp` cannot.** `exp`'s concentration is capped by
`lambda <= pool/batch` and its demand rises as the pool drains, so the shape
degrades over a run and needs per-ratio `lambda` tuning. `seg`'s window is
defined relative to `batch_n`, not to the pool, so **one parameter is equally
concentrated at every ratio and every point in the run** — no tuning, no
degradation, no capping.

Measured at 1e6 / 100 batches, `mult = 2`:

- max rank used is `19999` of a `20000` window in every batch — the draw never
  escapes the leading `2m`.
- rank spread `p10/p50/p90 = 2000/9950/18000` of 20000 — exactly uniform inside.
- key-space `p10..p90` width settles at **3.2%** of the key space and slides
  0.002 → 0.036 → 0.235 → 0.487 → 0.736 → 0.966.

The key span (~3.2m) is wider than the rank span (2m) because a key that
survives a draw stays in the window and can be re-exposed — survival decays
geometrically at `1/mult` per batch, so a thin tail trails behind.

`mult` is a clean knob: `1` is **exactly sequential** (batch b is literally keys
`[b*m, (b+1)*m)`), `2` gives a 3.2%-wide window, `16` a 31%-wide one.

Also the cheapest shape to generate: pool-work saving is `pool_n / (mult*m)`,
which at ratio 100 is already 25x and grows with the ratio.

**Writes one file, not a pair, and the stem is `.batch` for BOTH ops** (renamed
2026-07-31): `ins/<base_n/batch_n>/<i>.batch.<ext>` and
`del/<base_n/batch_n>/<i>.batch.<ext>`. There is no base/batch pair in these
datasets — the whole file *is* the batch stream, sliced into equal windows by
the harness — so the old `.base` stem on the insert side (inherited from what
tag `1<<6` happened to read) made two identical things look different. Both
tags now read `ParseXBatchPath(ratio).batch_path`.

This required `Options::ExtractPrefixes` to also strip `.batch` — it stripped
only `.bin`/`.in`/`.base`, so a `-p` pointing at a batch file left the prefix as
`<i>.batch` and every derived path became `<i>.batch.base.<ext>`. Verified: `-p`
on `ins/<x>/<i>.batch.bin` now resolves all four ratios on both sides, and
`tag=224` emits 36 tokens from `b_test`, masstree and bwtree alike. The change is
additive — the standard `/data/zmen002/kv_pair` datasets still have real
base/batch pairs and tags `1<<0`/`1<<1`/`1<<3` still read `base_path`.

The layout is otherwise identical to the standard datasets so
`ParseInsertBatchPath`/`ParseDeleteBatchPath` resolve it unchanged — which also
means **point `-path` at a fresh root** or it overwrites them.

**λ must track the batch ratio — there is a hard arithmetic ceiling.** The
exponential asks for `batch_n · λ / pool` of the ranks at the very front of the
pool ("demand"), and nothing can supply more than 100% of them, so

```
λ ≤ pool / batch_n   ==  the number of batches still to come
```

Demand rises through a run because the pool drains: `demand_b = demand_0 /
(1 − b/batch_num)`. So the **fraction of the run that is exactly on-shape is
`1 − demand_0`**. Past the ceiling the excess probability is capped at 1 and the
batch becomes the sharpest achievable rather than the requested shape (reported
as a NOTE with the count of capped rank-slots).

### Sampler — three generations, and why the last one won

`SelectBatchFlags` computes an inclusion probability per surviving rank and then
flips an independent weighted coin for each. Measured shape error and scatter at
batch 50/100, ratio 100 (scatter 1.0 = what a genuine random draw gives):

| sampler | λ=10 | λ=20 | λ=50 | scatter |
|---|---|---|---|---|
| guess-and-retry (v1) | −2.0% | −4.6% | −11.7% | 1.1 / 1.8 / 13.6 |
| running-total sweep (v2) | −0.0% | −0.0% | −0.0% | **0.007** |
| independent coins (v3, current) | −0.3% | −0.5% | −0.1% | **1.07 / 1.00 / 0.89** |

- **v1 was wrong, not just noisy.** Rejection sampling wastes every draw that
  lands on an already-taken rank, and in a crowded region nearly all of them do,
  so keys leaked out of the peak into the tail. Its inflated "scatter" at λ=50 is
  the systematic bias showing up in the chi-square, not real scatter.
- **v2 got the shape exactly right but killed the randomness** — picks spaced
  evenly along the cumulative curve by construction, ~150× less scatter than a
  real draw. Every stretch of the key range got almost precisely its predicted
  count, so there were no lucky hot spots, which is most of what makes a skewed
  workload hard on a tree.
- **v3 keeps v2's accuracy and restores v1's randomness.** Residual shape errors
  of ±0.5% are within one standard deviation of the per-batch count — they *are*
  the scatter, not bias. Also ~25% faster than v2 (no prefix scan).

Inclusion probabilities: `π_i = min(1, w_i/t)` where `w_i` is the unnormalised
target density at rank i and `t` solves `#{w_i ≥ t} + (Σ_{w_i<t} w_i)/t = want`.
Since `π ∝ w` for every uncapped rank, "capped" is exactly `w_i ≥ t` for a single
scalar `t`, so the solve is a scalar fixed-point iteration (`g(t)` is strictly
decreasing; the step always moves the right way), not a per-element search.

Independent coins give a count near `want`, not on it (±√want ≈ 0.03%), and the
file is sliced into equal windows downstream, so the count is repaired: drop
uniformly at random when over (uniform dropping scales every π by the same
factor, leaving the shape untouched), add uniformly at random from the unselected
when under. Both repairs are ≪ the natural scatter. The repair draws positions by
oversample-and-dedupe rather than materialising and shuffling a `pool_n`-sized
index array.

**Bug that v2 introduced and the permutation check caught** (worth remembering,
since the same shape of error can recur): under heavy capping the scalar solve
does not fully converge, the running total lands just under `want`, the rescale
goes slightly *above* 1, and a capped slot (π = 1) can then straddle two whole
numbers while the sweep can only ever pick that rank once. One lost pick leaves
an **uninitialised** output slot, and the file stops being a permutation. The
cheap sum/max check in `ValidatePermutation` is what surfaced it — keep it.

Other implementation notes: distribution objects are constructed inside the
lambda because `std::normal_distribution` caches a spare deviate (sharing one
across workers, as `data_op.h`'s generators do, is a data race). `parlay::filter`
must not be used with a side-effecting predicate — its predicate runs over a
delayed sequence and may be evaluated more than once per element.

### Per-ratio datasets — the harness now reloads per ratio

Tags `1<<6`/`1<<7` used to read *one* file and slice it four ways. As of
2026-07-30 the loops in `test_framework.h` hoist `incre_ratio_seq =
{1e-4, 1e-3, 1e-2, 1e-1}` above both blocks and call
`ParseInsertBatchPath(ratio)` / `ParseDeleteBatchPath(ratio)` **inside** the
loop, reloading `ins/<1/ratio>/` and `del/<1/ratio>/` each time. That matches
`incre_data_generator`'s ratio dir (`base_n / batch_n` == `1/ratio`), so each
ratio gets batches laid out for the step it slices at.

Consequences:

- **`-b` / `opts.batch_ratio` is now unused by tags `1<<6`/`1<<7`.** It only
  picks the file handed to `-p`, which tag `1<<5` builds its control from.
- 4 file reads per block instead of 1 (~64 GB of extra I/O per run at 1e9).
- Under the new root both sides are 1e9 entries; under the old root
  `del/<x>/*.batch.bin` is `base_n/x` entries, so the insert and delete blocks
  were not comparable at `-b != 1`. That asymmetry is gone.

**Silent-wrong-data hazard.** The old root `/data/zmen002/kv_pair` *also* has
`ins/{10,100,1000,10000}/`, so pointing the sweep there still runs to
completion — it just measures the globally-shuffled data. `incre_updates.sh`
now sets `data_prefix=/data/zmen002/kv_pair_incre` and `batch_ratio=0.01`
(so `-p` resolves to `ins/100/1.base.bin`); the old prefix is kept commented
one line below.

**The two halves are no longer interchangeable.** With globally-shuffled data
both halves of `wp` were uniform samples of the whole key space. With the new
data the first half is the low-key region (for `exp`) and the second the high-key
region, so in `incre_update_query`: membership `IND` probes only low keys and
`OOD` only high keys, and `GenRectangles` builds every range box out of
`entry_in_tree`, i.e. entirely inside the low half. Arguably what a skewed
workload should do, but the columns do not mean what they did.

### `scripts/gen_incre_data.sh`

Generates the whole sweep: 4 ratios x 4 distributions x 2 ops = 32 files x 8 GB
= **~256 GB** (the volume had 698 GB free on 2026-07-30). Ratios are generated
**cheapest first** (`10 100 1000 10000`) and existing non-empty outputs are
skipped unless `-f` is passed, so it is resumable.

**λ = `batch_ratio / 2`** → `{5, 50, 500, 5000}`. That puts demand at 0.5 at the
start of a run, so it reaches the ceiling of 1.0 exactly at the half-way point:
**every batch in the first half is exactly on-shape**, and the first half is what
builds the tree state the queries run against. Front-vs-back odds inside a batch
are `e^λ`, so even the widest batches (10% of the pool, λ=5) lean ~148x. The
earlier `batch_ratio / 10` was 5x flatter for no accuracy gain once the sampler
became exact. `norm`/`dual_norm` need no scaling — their mass sits in a band
~`2*dev` wide, so even the widest batch here stays under the ceiling.

### Front-window optimization (2026-07-31)

`WindowSize()` restricts each batch to the leading `win_n` ranks; only that
prefix is scanned, and **compaction shifts the window's survivors right against
a moving `lo` offset** so the untouched suffix never moves. Per-batch cost drops
from `O(pool_n)` to `O(win_n)`.

- `exp`: window is `40/lambda` of the pool (mass beyond is `e^-40`, and the
  expected picks lost are bounded by `lambda*want*e^-40` ≈ 1e-9), floored at
  `8*want` so a capped batch can still reach. **123.8x less pool-work** at
  lambda 5000; measured 1e8/10000 batches **~22 min → 18 s (77x)**.
- `seg`: window is `mult*want` by definition — 25x at ratio 100, more at finer
  ratios.
- `norm`/`dual_norm`: window is the whole pool (a centred window would work for
  them but is a different change), so they behave as before.
- `uni`: **closed form, no carve at all** (2026-07-31). For a flat density the
  batch-by-batch process is provably one global shuffle — cut a uniform random
  permutation of `[0, base_n)` into consecutive `batch_n` blocks and block 0 is
  a uniform random subset of the key space, block 1 a uniform random subset of
  what is left, and so on, which is exactly what the loop computes. `O(base_n)`
  instead of `O(base_n * batch_num / 2)`. Measured at 1e8: **9.3 s at every
  ratio** (was ~4.6 min at ratio 10000); at 1e9 it should drop 46 min → ~90 s.
  Verified empirically before the change: `uni` batch 0 at ratio 100 had
  p10/p50/p90 = 0.0999/0.5002/0.9000 of the key space, i.e. already a plain
  uniform subset, so the carve was producing nothing the shuffle does not.

  Corollary worth remembering: for `uni` this generator's output is
  distributionally identical to `data_generator`'s. The new generator only earns
  its keep on shapes that *have* per-batch structure.

**`parlay::random_shuffle` defaults to a FIXED seed.** Both the uniform closed
form and the per-batch within-batch shuffle now pass `parlay::random(seed)`
explicitly. Without it every `-file_num` instance of a `uni` dataset would have
come out byte-identical (verified: files 1 and 2 now differ).

**Bug this surfaced.** The count repair used to top up **uniformly** over the
free ranks. Harmless when the shape is gentle, badly wrong when it is steep: at
lambda 5000 it sprayed ~0.5% of the batch flat across the window and pushed the
largest key to **4x** where the exponential could plausibly reach (0.0078 vs an
expected max of 0.0018). Fixed by rejection-sampling the additions from the
shape itself (accept with probability `w/max(w)`). KS 0.0144 → **0.0073**, head
error −0.72% → **+0.09%**, max key 0.0078 → **0.0017**. Dedupe the *accepted*
set, not the raw candidate draw, or the repair costs more than the optimization
saves (41 s vs 18 s at 1e8).

### Rank-skew is not key-skew — what `exp` actually produces

The sampler shapes **ranks in the surviving pool**, and the pool is sorted
throughout (`pack` preserves order, the window is a prefix, survivors shift
right, the untouched suffix holds strictly larger keys). Confirmed empirically:
ranks computed as index-in-`sorted(pool)` match `exp(lambda)` to KS ≈ 0.005.

But once the front is thinned the survivors are no longer evenly spaced, so the
rank→key map goes nonlinear. Measured at lambda 50 (shape indicator
`(p50-p10)/(p90-p10)`; 0.268 = exponential, 0.500 = symmetric):

| batch | 0 | 5 | 25 | 50 | 90 |
|---|---|---|---|---|---|
| in rank space | 0.265 | 0.279 | 0.264 | 0.269 | 0.476 |
| **in key space** | **0.265** | **0.447** | **0.452** | **0.419** | **0.508** |

So each `exp` batch is a **tight migrating cluster in key space** (p10–p90 width
3–7% of the key space, centre walking 0.014 → 0.25 → 0.50 → 0.90), but the shape
*inside* the cluster is exponential only for batch 0 — afterwards it is roughly
symmetric. At batch 25 the leading 10% of the pool's key span holds just **0.04x**
the average survivor density, which is what bends the mapping.

If per-batch key-space exponentiality is required, feed `TargetDensity` the key
position `(pool[lo+i] - klo)/(khi - klo)` instead of `(i+0.5)/pool_n`; the window
would then need a binary search on keys and would come out smaller. Not done.
`seg` sidesteps the whole issue — its window is a rank count by definition.

**Generation cost is the real constraint, and it is worse than first estimated.**
Cost is linear in total pool-work `base_n * batch_num / 2`, measured at ~2.9 ns
per unit (n=1e7, 100/1000/10000 batches → 1.6 / 13.3 / ~145 s). Projected at
base_n=1e9, per file and per 8-file tier:

| ratio dir | per file | tier |
|---|---|---|
| 10 | 0.2 min | 0.03 h |
| 100 | 2.4 min | 0.3 h |
| 1000 | 24 min | 3.2 h |
| **10000** | **4.0 h** | **32 h** |

~36 h total, **90% of it in the finest tier**. Not yet implemented, but the clear
fix: at λ=5000 the density is negligible beyond ~0.6% of the pool, so 99.4% of
every pass is spent on ranks with essentially zero chance of selection.
Restricting the work to that front window, and compacting by shifting survivors
rightward against a moving start offset instead of rebuilding the whole pool,
would cut the finest tier by roughly 160x (≈15 min). Do this before generating at
1e9.

### `temporal_graph_signal` driver (`tests/temporal_graph_signal.cpp`)

Converts a **signal graph** — a stream of edge *appearances* `(src, dest,
timestamp)` with no INSERT/DELETE semantics — into one timestamp-labeled output
file (`0.in`/`0.bin`; rows `src dest timestamp`, binary `[N, 3, src, dest,
timestamp, ...]`). Pipeline:

1. **Read & sort** edges by raw timestamp (Unix epoch seconds, read as
   `double`).
2. **Deduplicate** `(src, dest)` keeping the earliest (smallest raw-timestamp)
   copy via `remove_duplicates_ordered` (input is timestamp-sorted, so the
   stable sort by `(src, dest)` keeps the earliest copy of each group).
3. **Re-label timestamps** on the deduplicated edges (the label becomes the
   output timestamp). Dedup leaves the result sorted by `(src, dest)`, so it is
   **re-sorted by raw timestamp first** to make group adjacency valid, then:
   - `-group_by_year 1` (default): convert each epoch second to its UTC calendar
     year via `gmtime_r` (`tm_year + 1900`), then label the distinct years
     `1..#batches` by ascending year — one batch per year. Years with no edges
     are skipped, so labels stay contiguous.
   - `-group_by_year 0`: renumber the distinct raw timestamps `1..#timestamps`
     by ascending value (each distinct timestamp = its own label). Ranks are
     computed over the **deduplicated** set.
   - Both share the same group-flag + `scan_inclusive` machinery; only the flag
     predicate differs (year change vs raw-timestamp change). The result
     `labels` sequence overwrites `deduped_edges[i].timestamp`.
4. **Shuffle within equal labels**: global `random_shuffle` then `stable_sort`
   on the label → uniform order within each label group, globally ordered across
   groups.
5. **Stats + write**: prints max vertex ids, max degree, average degree, and a
   post-dedup per-batch summary (`Batch N: M edges`, listed only in year mode
   since rank mode can have a huge number of labels; the aggregate distinct-label
   count prints in both modes).

Note the **dedup-before-relabel** order: relabeling operates on the final edge
set, so rank-mode labels (and the printed distinct counts) reflect deduped data,
not the raw stream.

CLI (`commandLine`, `common/parse_command_line.h`): `-p` input (required), `-o`
output dir (required), `-binary {0,1}`, `-group_by_year {0,1}`. Missing required
arg or `-h`/`-help` → usage.

## `ReadEntries` (`test_framework.h`) — the dataset loader

```cpp
template<typename E, int kInputType = 0>
auto ReadEntries(char const* iFile, parlay::sequence<E>& wp, size_t offset = 0)
  -> std::pair<size_t, int>;  // returns (N, dim)
```

`ReadEntries` is a **dispatch layer**: path ends in `.bin` →
`ReadEntriesBinary`; anything else → `ReadEntriesText`. No filesystem probing —
the given path is the file that gets read. `ParseInsertBatchPath`/
`ParseDeleteBatchPath` (`options.h`) derive base/batch paths that **inherit the
input file's extension** (`InputExtension()`), so a `.bin` input (`-p`) makes
all derived batch files load as binary too. `ExtractPrefixes` strips `.bin`,
`.in`, then `.base`.

**Debugging note**: `unit_test` redirects stdout during `RunTest`, so an
`abort()` from a missing data file used to die silently (no gtest summary).
`binary_io.h` errors go to **stderr** for this reason.

### `ReadEntriesText` (the original text parser)

- Reads whole file via `benchIO::readStringFromFile`, tokenizes in parallel via
  `stringToWords`, parses `N`/`dim` from the first two tokens, then a
  `parallel_for` fills `wp[i]` via `E::ReadKey`/`E::ReadVal` (`std::stoul`).
- **Overwrites** `wp` (`clear()` + `resize(N)`), never appends.
- Only ever reads 2 columns (`pts[i*dim]`, `pts[i*dim+1]`) regardless of dim.
- `kInputType` (enum `InputType` in `wrapper.h`): `kNormalInput`/`kOsmInput`
  read key+val from the two columns; `kDynamicGraphInput` packs two uint32
  columns into a uint64 key `(u<<32)|v` and sets `val = i + offset`.
- `offset` is only used by `kDynamicGraphInput`: callers thread a running
  `total_entries` across multiple files so vals stay globally unique.
- Call sites: `DefaultTestFunc` (base file from `-p`, then per-ratio
  base/batch pairs via `opts.ParseInsertBatchPath`/`ParseDeleteBatchPath`),
  `TestTemporalGraph` (OSM single file; dynamic graph 10×`{i}_insert.in`/
  `{i}_delete.in` pairs), `unit_test.cpp`.
- `ReadPointsSerial` (serial fallback) is used only under `-DMEMCHECK`.

### `ReadEntriesBinary`

Same interface/semantics, reads the `.bin` format via `ReadBinaryEntries`
(`binary_io.h`; handles 32/64-bit payload widths transparently). Assigns
`wp[i].key`/`wp[i].val` directly (no string parse); `kDynamicGraphInput`
packs `(u<<32)|v` and uses `offset` like the text reader. Aborts on
missing/truncated file or bad header.

### Known issue (pre-existing, 2026-06)

`stdset`/baseline targets currently fail to compile due to a `TestLeafRandom<TT>`
call signature mismatch in `TestLeafRandomness` — unrelated to the reader work
(verified by stashing). `b_test` builds fine.

## Baselines (`external/baselines/*.h` + `tests/<name>.cpp`)

Single-threaded / external reference implementations that conform to the same
test-framework interface as `BTree<TT>` (methods: `Build`, `BatchInsert`,
`BatchDelete`, `Contain`, `RangeCount`, `RangeReport`, `GetEntries`,
`DeleteTree`, `GetSize`, plus `ParseInput`/`ParseDeleteInput`/`KeyProbe` adapters
and the `Leaf`/`Interior` aliases). Each has a thin `tests/<name>.cpp` driver
(mirror of `tests/stdset.cpp`) and its own executable target in CMake.

| Baseline | Wrapper header | Backing store | Notes |
|---|---|---|---|
| StdSet | `stdset.h` | `std::set<E, KeyLess>` | serial loops; ordered by key only |
| TlxBTree | `tlxbtree.h` | `tlx::btree_set<E, KeyLess>` | serial loops; in-memory B+ tree |

- **TLX** is a git **submodule** at `external/tlx` (header-only btree). Added with
  `git submodule add -f` because `.gitignore` ignores all extensionless paths
  (`*` on line 2). Include dir `${PROJECT_SOURCE_DIR}/external/tlx` is added only
  to the `tlxbtree` target; header is `<tlx/container/btree_set.hpp>`.
- `tlxbtree.h` differs from `stdset.h` only in: include, `SetType`, and
  `Contain` uses `set_.exists(k)` (tlx) vs `set_.find(k) != end()` (std::set).
- **Build caveat**: baselines have **no `Validate()` method**, so they only
  compile on the `DefaultTestFunc` path. Under `-DOSM`/`-DDYNAMIC_GRAPH` the
  driver selects `TestTemporalGraph`, which calls `pkd.Validate()` →
  compile error. Build baselines with `OSM=OFF DYNAMIC_GRAPH=OFF` (the default).
- **Membership-query segfault fix** (`DefaultTestFunc`, `test_framework.h`): the
  `query_type & (1<<0)` (membership) branch sliced the membership probe set from
  `wi` (the insert/delete batch) in its `wi.size()==0` case. With `-t 0` (no
  insert/delete batch) `wi` is empty, so `wi.subseq(0, batch_size)` with
  `batch_size = wp.size()*kMemberQueryRatio (0.01) > 0` did an out-of-bounds copy
  → SIGSEGV inside parlay's parallel `sequence` construction (crash is in the
  framework, *before* any `Contain` call — affected every baseline equally, not
  TLX-specific). Fixed to slice `wp` (the keys actually built into the tree).

## Concurrent-index incremental sweep (`external/index-microbench/workload.cpp`)

Three tags, **numbered identically in `workload.cpp` and `tests/test_framework.h`**
(renumbered 2026-07-28 — anything older that says "tag 5 = insert" predates this):

| tag | block | b_test | concurrent |
|---|---|---|---|
| `1<<5` | static build, then query — the control | `BuildTree<…,kOutputRatio=2>` | `ConBuild` |
| `1<<6` | incremental insert | `BatchInsertByStep` | `ConInsertByStep` |
| `1<<7` | incremental delete | `BatchDeleteByStep` | `ConDeleteByStep` |

Driven by `scripts/incre_updates.sh` (`tag=2#11100000` = 224, all three).

All three end in the same query suite on a **half-filled** tree, factored into one
helper per side — `incre_update_query` (`test_framework.h`, a lambda beside
`membership_query`/`range_query`/`range_count`) and `ConQueryAfterStep`
(`workload.cpp`). Both take `(entry_in_tree, entry_not_in_tree)` and run: one
membership pass over `random_shuffle(append(both halves))` truncated to
`batch_size`, then `kRangeQueryType` range queries and `kRangeQueryType` range
counts, all on `entry_in_tree`. `batch_size` is `kMemberQueryRatio` of the
**whole** input, not of one half.

Tag `1<<5` being set also skips the shared primal build block (`if ((opts.tag &
(1<<5)) == 0)`) in both files, since that block builds from the same input.

### `ConInsertByStep` / `ConDeleteByStep`

Both slice the input into `1/ratio` batches over
`ratio_seq = {0.0001, 0.001, 0.01, 0.1}`, time the whole chain **once** (no
`rounds` loop — `-r` is ignored), then **rebuild** to reach a half state rather
than unwinding, because masstree's `remove` never reclaims (measured: 1e9 delete
grows RSS 98GB → 1491GB and dies).

The half state is `slice_num / 2` batches, and callers split the input at
`size() / 2` to decide which keys are in the index — these agree exactly only
when `step` divides `n` (true for every real dataset). Both sides use `size()/2`
so the two drivers classify keys identically.

### Timing vs. query state (tag `1<<5`)

The timed build covers the **whole** input; only the final untimed build is the
half one. `BuildTree`'s `kOutputRatio` template parameter does this (times
`Build(wp)`, ends with `Build(wp[0, n/kOutputRatio))`), and the tag `1<<5` block
in `workload.cpp` mirrors it with `ConBuild<1>(wp)` → drop → `ConBuild<0>(half)`.
This is what makes the control comparable to tags `1<<6`/`1<<7`, which likewise
time the full chain and then rebuild to half.

### `ConRunBatched` — why threads are spawned once

`threadinfo::make` mallocs 8KB, links into a never-pruned global `allthreads`,
and grabs a 2MB superpage pool. One `StartThreads` per batch would leak
`slice_num * num_thread` threadinfos (1e9 @ ratio 1e-4 → 2.24M ≈ 4.4TB, killing
the run via `pthread_create` EAGAIN). `ConRunBatched` spawns one thread per
worker, one threadinfo each, and walks all batches inside with a `std::barrier`
between them. Work split inside a batch is **proportional**
(`in.size()*tid/num_thread`), unlike `ConInsert`/`ConDelete`, which use
`key_per_thread*tid` and drop the remainder.

### Output format

**The suite was trimmed at some point after the 2026-07-28 notes below, and both
drivers were trimmed identically** — verify against the code before trusting any
column count here. As of 2026-07-31 `incre_update_query` (`test_framework.h`) and
`ConQueryAfterStep` (`workload.cpp`) each run:

- **one** membership pass — `MIX` only (both halves appended, shuffled, truncated
  to `batch_size`). The `IND` and `OOD` passes are commented out in both files.
- **one** range query, `rec_type = 1` only (the `for rec_type` loop is commented
  out in both).
- **one** range count, `rec_type = 1` only.

So per ratio it is **4 columns** = 1 timing + 1 membership + 1 range query + 1
range count, not 10. Whole-run totals: tag `1<<5` → 4 tokens; tags `1<<6`/`1<<7`
→ 4 ratios × 4 = 16 each, **plus** 1 primal build token when tag `1<<5` is not
also set. `tag=2#11100000` (all three, what `incre_updates.sh` runs) is
`4 + 16 + 16 = 36` tokens; a lone `1<<6` or `1<<7` is 17.

Every column is a real number for every index type as of 2026-07-31 — masstree's
range-count `-` placeholders are gone (see the `q_[0]` race fix below), and it
reported all range queries from 2026-07-28 (the `scan` stack-array fix).

**`merge_incre_updates.py` still expects the old 90-value layout** and has not
been updated for the trim; it will treat a current 36-value run as a short row
and pad it. Fix the parser before merging new logs.

The primal build is **always** run and always a real number, in both drivers —
`workload.cpp` used to skip it and print `-` when only the incremental bits were
set. Restoring it costs masstree a full 98GB / ~66s index at 1e9 that tags
`1<<6`/`1<<7` immediately release; it is paid only to keep that column
comparable against `b_test`.

### Merging the trimmed logs (`scripts/merge_incre_updates_new.py`)

Merges **both** log trees into one 36-value CSV at
`data/incremental_updates_new/<name>.csv`, and writes a concatenation of every
source log (with `>>> <path>` markers) beside it for reading by hand.

Row layout is `build | 3 queries | 8 x (update_time, 3 queries)` = **36 values**,
9 blocks of 4. The 3 queries are `MIX`, `RRM`, `RCM` — the driver dropped the
`IND`/`OOD` membership passes and the `RRS`/`RRL`/`RCS`/`RCL` box sizes, taking
the row from 90 to 36. Block order is unchanged: build, then ins then del at
0.01% / 0.1% / 1% / 10%. Field mapping was confirmed by magnitude against the
old 90-column CSV (`exp,BTree,64` gives RRM 0.0837 vs 0.0804, RCM 0.00215 vs
0.00205; MassTree's del timings track the old row within a few percent).

**The 36 columns are a strict subset of the 90**, so the old logs are *projected*
down rather than skipped — keep the timing plus MIX/RRM/RCM per block, drop the
other six. Projection is **by column name**, not index, so it cannot drift if
either layout changes. Only the 73-value pre-MIX layout is unprojectable (no MIX
column at all) and still skipped.

`kSources` decides which sweep supplies which distribution. **Order matters**:
sources are tried in turn and the first to supply a given
`(benchmark, tree, leaf_wrap)` wins, so a later entry only fills gaps.

| order | source | values | supplies |
|---|---|---|---|
| 1 | `logs/incremental_updates_new/` | 36 | `uni`, `norm`, `seg` |
| 2 | `logs/incremental_updates/` | 90 | `uni` — **backfill only** |

As of 2026-08-02 the tag=224 sweep is primary for all three distributions, and
the old 90-value sweep backfills `uni`: a `(tree, leaf_wrap)` it has that the new
sweep lacks is taken from there and projected onto the 36 columns by name. This
was requested explicitly; it is **not** the safe default — see the caution
below.

**The CSV reports `norm` as `skew` and `seg` as `adversary`** (`kBenchDisplay`).
This is a display mapping applied at row construction, *not* a rename of the
logs: the driver writes `Dis: norm` / `Dis: seg` from the generator's own `-dis`
values, so renaming directories and headers would be undone by the next
benchmark run. `kWantBench`, `kSources` and the dedupe key all stay in
generator names; only the emitted `benchmark` column changes.

For `norm`/`seg` there is no second source, so a tree with no run under
`logs/incremental_updates_new/<tree>/<dis>/` is **absent** from that
distribution. A distribution in `kWantBench` that supplies **no** rows at all is
warned about on stderr (`warn: no rows for …`), since it is otherwise invisible:
the CSV simply has no such row.

**The `uni` backfill mixes two sweeps inside one distribution, and the merger
says so.** `report_coverage()` prints every backfilled row, then a `WARN:` for
any `(benchmark, tree)` drawing from more than one sweep — the dangerous case,
because such a curve steps where the *source* changes rather than where the
parameter does. Concretely, if the new sweep's `uni` has only `64.log`, then
`uni/BTree` leaf 64 comes from the new sweep and 2..1024 from the old, so the
leaf-size curve has an artificial discontinuity at 64. Read those lines before
plotting; the real fix is rerunning the missing leaf sizes into the new sweep,
after which the backfill goes quiet on its own (no backfill lines, no `WARN`).

Whether a row was backfilled is **not** a CSV column — the schema is unchanged
so downstream consumers keep working — it exists only in the stderr report.

**`exp`, `seg_10` and `dual_norm` are dropped from the output**, their logs
still on disk. Adding a name back to `kWantBench` *and* `kSources` restores it.

`seg` and `seg_10` are the same sliding-segment shape at two window widths (the
generator's `-batch_param mult`), so they are separate distributions rather than
two runs of one — do not dedupe them. They differ per tree in *direction*, which
is how you can tell: ins_0.01%, `seg` → `seg_10`, CPAM 7.43 → 12.60 and PAM
8.40 → 12.66 (slower), but PMA 191.98 → 178.85 (faster).

**How far apart the two sweeps are** — the evidence behind the caution above.
They are not interchangeable, and the difference is **per tree, not a uniform
scale factor** — same tree, `exp` ins_0.01%, old → new:

| BTree | CPAM | PAM | PMA | MassTree | BwTree |
|---|---|---|---|---|---|
| 29.25 → 6.40 | 55.60 → 6.41 | 29.75 → 8.24 | 29.90 → **181.17** | 87.29 → 84.70 | 55.68 → 55.15 |

PMA got ~6x *slower* while MassTree and BwTree barely moved, so a backfilled row
sits at its own unrelated operating point next to rows measured on the new
sweep — which is why the `uni` backfill is reported per row rather than applied
silently. Rerunning the missing runs into `incremental_updates_new/` is the only
thing that actually removes the mismatch. (`exp` itself is no longer emitted —
the table is kept because it is the evidence for this.)

(An earlier revision of this note claimed a "~9x" factor between the sweeps.
That was derived by comparing `exp/BwTree`-old against `exp/BTree`-new — two
different trees — and is wrong. The table above is the same-tree comparison.)

**Every leaf size that was run is emitted** (`kBTreeLeafWraps = None`, changed
2026-08-02 — it was previously `kBTreeLeafWrap = 64`, a hard pin). BTree is the
only tree swept over leaf sizes, so a distribution holds one BTree row per leaf
size it was run at against 1 row for each other tree. Set
`kBTreeLeafWraps` to a collection — `{64}` reproduces the old 18-row output
exactly — to pin it back for a plot that wants one line per tree.

Because coverage is a property of the logs and not of the merger, the script
prints a `note:` per `(benchmark, tree)` for any tree that is swept *somewhere*,
including the distributions where it is not — so `adversary/BTree spans 1 leaf
size(s): 64` appears next to `uni/BTree spans 10`, rather than the gap being
silent.

**`leaf_wrap` must come from the header's `Buf:` field, not the filename.** The
new sweep names every file `64.log` after the BTree leaf size, so PAM, PMA and
MassTree runs sit in a `64.log` while reporting `Buf: 1`. A merger written
against the path would mislabel three of the six trees.

Expected state: the 3 × 6 grid, `uni`/`skew`/`adversary` × BTree, PAM, CPAM,
PMA, MassTree, BwTree, with BTree expanded over its leaf sizes — 18 rows when
pinned, more once the leaf sweep is included. How many depends on which
`logs/incremental_updates_new/<tree>/<dis>/<leaf_wrap>.log` files exist; read the
`note:`/`warn:` lines the merger prints rather than assuming the grid is full.
Every row should parse at exactly 36 values.

The row counts and values recorded below predate the 2026-08-02 switch to a
single source, when `uni` came from the old 90-value sweep — re-derive them from
a current run before quoting. (The old `uni/MassTree` `-` in every `RCM` column
was an artefact of those logs predating the `q_[0]` race fix; a `uni` run from
the current sweep should have real numbers there.)

`skew` lands between `uni` and `adversary`, as its centred band should — BTree
ins_0.01%: uni 29.35, skew 25.03, adversary 7.61.

**`adversary/MassTree` and `adversary/BwTree` are byte-identical to those trees'
`exp` logs** (ins_0.01% 84.7021 and 55.145, matching to every digit) — they were
copied from `exp` and relabelled, so they are placeholders, not `seg` runs. The
other four `adversary` rows and all six `skew` rows are genuine. Re-run those
two against the `seg` dataset before publishing, or the table reads as "MassTree
and BwTree are insensitive to distribution", which the other four trees
contradict.

**The `seg` in the CSV is `45f7a2c6`'s data**, restored on 2026-08-01 (BTree
ins_0.01% 7.61, CPAM 7.43, PAM 8.40, PMA 191.98). The set that had been there is
now `seg_10`, moved into `<tree>/seg_10/64.log`.

**Moving a log between distributions means editing its header, not just the
path.** The merger reads `benchmark` from the `Dis:` field; the directory is
never consulted. The `seg` → `seg_10` move needed
`sed 's/Dis: seg;/Dis: seg_10;/'` on each file — without it the moved logs would
still have come out labelled `seg` and collided with the restored ones, and
first-seen-wins would have silently produced a mix of the two.

BTree's `exp` logs in the new sweep now carry the full leaf-size sweep
(2..1024), not just 64 — though `exp` is not emitted (see `kWantBench`). The
`exp/BTree` values shifted slightly (build 1.3142 → 1.31545) when `64.log` was
rerun — expected, not a merge bug.

**A short row means a lost token, and it shifts every later column.** The
BwTree `exp` log arrived once at 35 values, missing `del_0.01%_MIX`; the parser
pads at the *end*, so `del_0.01%_MIX` read as 0.455 (a membership probe with a
range-report magnitude) and `del_0.01%_RCM` as 38.6 seconds. The count is the
cheap check — `sed -n '2,$p' <log> | tr -s ' \n' '\n' | grep -c .` should be 36
for every file before trusting a merge.

### `scripts/filter_incremental_updates.py`

Cuts a merged CSV down to 20 columns: `benchmark, tree, leaf_wrap`, the 8 update
timings, and the `build` and `1%` blocks' `MIX`/`RRM`/`RCM`. Defaults to the
36-value CSV above, writing `<name>_filtered.csv` next to it.

The kept columns are a subset of **both** layouts, so passing the old
`data/incremental_updates/incremental_updates.csv` explicitly still works
(verified). This script is the only place the 20-column list lives —
`merge_incre_updates_new.py` deliberately has no `--slim` flag, so the two
cannot drift apart.

### Merging the logs (`scripts/merge_incre_updates.py`)

Concatenates `logs/incremental_updates/<tree>/<dis>/<leaf_wrap>.log` (tree dirs
are the numeric `-T` values: 0=BTree, 1=CPAM, 2=PAM, 3=PMA, 5=MassTree,
6=BwTree) into one log, then writes `data/incremental_updates/<name>.csv` — one
row per run, 93 columns = `benchmark, tree, leaf_wrap` + the 90 values.

Column names are prefixed by their block (`build`, `ins_0.01%` … `del_10%`)
because the 9 query names repeat once per block; a bare repeated `IND` × 9 is
ambiguous to any consumer. Nothing reads this CSV yet — `merge_incre_updates.py`
is its only reference.

Three log realities the parser has to absorb:

- **`-` is a legal value**, not a parse error (masstree's 27 range-count cells in
  logs recorded before 2026-07-31). The pre-2026-07-29 version tested
  `float(tok)` on every token and therefore dropped every masstree row silently.
  Runs after the `q_[0]` race fix emit no `-` at all, but keep the handling for
  the older logs.
- **A row can be split across lines.** Masstree's allocator writes `madvise
  superpage: Cannot allocate memory` to stderr while the row is going to stdout,
  and `2>&1 | tee` splices it mid-row. The parser strips known noise phrases
  (`kNoisePhrases`) and accumulates values until the *next header*, rather than
  expecting one row per line. Verified against a saved noisy log.
- **A run can end short.** A killed process leaves a partial row (BwTree at 1e9
  uniform died mid-`del 1%`, 76 of 90 values). Those are padded to keep columns
  aligned and reported on stderr — the valid prefix is real data.

**Stale logs are dropped, deliberately.** A `2.log`/`32.log` from the older
73-value layout (`build + 8 × 9`, no MIX and no static query block) carries a
header *identical* to a current run's — `Buf` is the tree's internal block size,
not the filename — so padding it would emit a duplicate
`(benchmark, tree, leaf_wrap)` key holding shifted values. `kLegacyFieldNum`
catches exactly 73 values and skips with a note.

The 16 pre-MIX logs that existed (CPAM `32.log`, PAM/PMA/BwTree `2.log`, × 4
distributions) were `git mv`d to `logs/incremental_updates_old/<tree>/<dis>/` on
2026-07-29, keeping the `<tree>/<dis>/` structure because the bare filenames
(`2.log` under three different trees) collide if flattened. That folder is a
**sibling** of `logs/incremental_updates/`, deliberately outside
`concat_logs()`'s `logs/incremental_updates/*/*/*.log` glob. `kLegacyFieldNum`
stays as a net for future stragglers.

### Gotchas found 2026-07-27

- **Running the incremental blocks together used to yield only the insert half
  for masstree** — the insert block ran first in the same process, masstree
  returned nothing to the OS, and the `ConDeleteSweepFits` guard then saw a
  depleted `MemAvailable` and skipped the delete block. **Fixed**:
  `ReclaimMassTreeMemory` frees the memory between phases and the guard is gone,
  so one process now produces both halves (verified at 1e8 for masstree and
  bwtree). Splitting into one tag per process is no longer needed.
- **Ratio 0.0001 is noisy.** Observed 7.16–14.51s for the same insert chain
  across runs, while the other three ratios hold within a few percent — 10000
  batches means 10000 barrier syncs. These are single-shot (`-r` is ignored on
  this path), so average before trusting that column.
- **Insert/delete blocks read different files**: tag `1<<6` reads `base_path`
  into `wp`, tag `1<<7` reads `batch_path` into `wi`. Equal only when `-b 1`. At
  `-b 0.1` the delete sweep is 10x smaller, and since `batch_size` derives from
  `wp.size()` vs `wi.size()`, the membership columns differ 10x too — the two
  blocks are not comparable as logged.
- **Build config trap**: `main` dispatches to `ApplyConcurrentIndexDynamicGraph`
  when `DYNAMIC_GRAPH` or `OSM` is defined, and that function only handles tags
  3 and 4 — tags `1<<5`/`1<<6`/`1<<7` produce *no output at all*, silently.
  Rebuild with `-DDYNAMIC_GRAPH=OFF -DOSM=OFF` before running them by hand.
  `incre_updates.sh` is safe because its `run_cmake` passes those flags.
- **`BatchInsertByStep` divides by zero when `ratio * n < 1`** (SIGFPE, rc 136):
  `step` has no `max(…, 1)` guard, unlike `BatchDeleteByStep`. Only bites tiny
  smoke datasets — at n ≥ 1e8 the smallest ratio still gives `step = 10000`.

### Dataset note

Ratio-1 dirs (`ins/1`, `del/1`, i.e. batch == base) exist only for some
`<dis>/<n>` combinations — at 1e8 only `dual_norm` has them; `uni/100000000_1`
has just `10/100/1000/10000`. `-b 1` against a distribution lacking `1/` aborts
in `ReadEntries` before any work.

### `ConRangeQuery` dead-allocation fix (2026-07-27)

`ConRangeQuery` used to prefix-sum the per-query result sizes and allocate
`parlay::sequence<std::pair<keytype,uint64_t>> v(tot_size)` up front. Nothing
ever wrote to `v` — its only reader was the commented-out `range_report`
branch — but at 1e8 with rectangle type 2 that is 2.5e10 pairs = **372GiB**,
and bwtree died there with `std::bad_alloc`. Removed `v`, `offset`, and the
`parlay::scan`; `rec_num` is now unused (`(void)rec_num`).

Masstree never reached it only because `ConQueryAfterStep` skipped rectangle
type 2 for it — masstree had the *same* bug in its own `scan` (`index.h:829`),
`Str results[range]`, a stack array sized to the result count, ~8MB at 5e5
elements on an 8MB stack. Same shape, different memory. **Both are fixed now**
(see below); the range query skips are gone.

Both index types implement `scan(key, range)` as "walk `range` elements from
`key`" (masstree `get_next_n`, bwtree iterator walk), so the timings are
comparable. If the `range_report` branch is ever restored, note
`v.begin() + offset[start_index]` is wrong — it offsets by the thread's first
query index, not per-query, so queries within a thread would overlap.

### `GenRectangles` non-FixSize path never sets the lower bound

In `tests/test_framework.h:126-139` (the `FixSize == false` branch, which is
what `GenRectangles<TT, false, false>` — every concurrent-index call site —
uses), `range.first` is never assigned for types 1 and 2; `range.second` is
assigned twice instead. `std::pair`'s default ctor value-initializes, so
`range.first == 0`:

| type | intended | actual |
|---|---|---|
| 0 | `[1, n^0.25)` | `[1, n^0.25)` — correct |
| 1 | `[n^0.25, sqrt(n))` | `[0, sqrt(n))` |
| 2 | `[sqrt(n), 1e6)` | `[0, 1e6)` |

So the three "brackets" are nested rather than disjoint (compare the `FixSize`
branch, which correctly sets both bounds to `[1,100) [100,10000) [10000,1e6)`).
Large-bracket measurements therefore include arbitrarily small boxes and
understate large-range cost. Cross-tree comparisons stay valid — `srand(10)`
makes the box sequence identical for every tree at a given `(rec_num, type, n)`
— but the brackets do not measure what their names say.

### BwTree incremental results (1e8 uni, `-t 96 -b 0.1`)

Works end to end after the fix (bwtree was never subject to the old memory
guard, which only applied to masstree and has since been removed). Notable: rectangle-type-2 scans cost **17.0s in the delete block vs 10.5s in
the insert block** despite the delete-block tree holding 10x fewer live entries
(5e6 vs 5e7). Because type 2 caps at `[0,1e6)` for both and `srand(10)` fixes
the length sequence, both blocks walk an *identical* number of elements — so
that 1.6x is a real per-element slowdown, consistent with BwTree delta/tombstone
chains left behind by the bulk deletion.

### Masstree memory reclamation (2026-07-27)

**Where masstree's memory actually lives.** Not in the tree. `threadinfo::make`
(`masstree/kvthread.cc:98`) mallocs a *fresh* 8KB threadinfo with zeroed
`pool_[]` on every call, and `StartThreads`/`ConRunBatched` call it once per
thread per invocation. Pools are per-threadinfo, refilled from `posix_memalign`
superpages recorded in `pool_ptrs_`. So nothing an index frees is ever returned
to the OS or reused by a later index.

Consequence: **restoring `~mt_index` would reclaim nothing.** Its body is
commented out (`masstree/mtIndexAPI.hh:76`) and references a `ti_` that is not a
member, but even a correct `table_->destroy(ti)` would only push nodes onto a
freelist no subsequent build consults, because that build gets new threadinfos.
Don't spend time on it.

**What does work** — `threadinfo::release_all_pools()` (added to `kvthread.hh`
next to `deallocate_ti`): walks the global `allthreads` list, calls
`deallocate_ti()` on each (frees its superpages plus the `pool_ptrs_` buffer),
frees the threadinfo struct, and nulls `allthreads`. `deallocate_ti()` already
existed and had **no callers** — it is the only thing in the tree that releases
pool pages.

`ReclaimMassTreeMemory()` in `workload.cpp` wraps it and is called at six points
where the workers have joined and no index is live: inside
`ConInsertByStep`/`ConDeleteByStep` between the full and half builds, and after
each `delete idx` in tags 3, 5 and 6. All six are **outside** the timed regions
(each follows a `t.stop()`).

**Stop at the allocator — do not release to the OS.** An earlier version also
called `MallocExtension::instance()->ReleaseFreeMemory()` (this binary links
`tcmalloc_minimal`, CMakeLists.txt:246, which parks freed pages in its page
heap). That was only ever needed to raise `MemAvailable` for the guard, and the
guard is gone. Worse, it made the *next* build fault every page back in, and
that cost lands inside its timed region — ratio 0.0001 swung 7.16–14.51s with
it, and settled to 6.30–7.26s once removed. Leaving the pages with tcmalloc
still prevents growth, because tcmalloc reuses them for the next build.

**SAFETY**: `release_all_pools()` frees the pages masstree nodes live in and the
threadinfo objects themselves. Only call it with no masstree index live, no
`threadinfo*` held, and the Con* worker threads joined. Deliberately *not*
called in tags 0/1, where `primal_idx` stays live across ratios.

**The `ConDeleteSweepFits` guard is gone** (removed 2026-07-27). It multiplied
the input size by a hand-picked 1000 bytes/entry, compared that against
`MemAvailable`, and skipped the whole delete block when it did not fit — the
`fits` flag, the 36 `-` placeholders, and all. It was ~2.3x over (93.1GiB
demanded at 1e8 for a sweep peaking near 40GB) and its constant had been
calibrated against the leaking behaviour that `ReclaimMassTreeMemory` now
prevents. The delete sweep runs unconditionally.

### Tag 3 (batch update by size) fixes

- Removed the post-measurement `ConDelete` "reset" sweeps from both masstree
  branches. `idx` is freshly built each iteration and dropped on the next line,
  so emptying it bought nothing while costing ~1.4KB of unreclaimed RSS per
  entry — `ConDelete(append(wi, wp))` was ~1.4TB at 1e9, plus a full
  concatenated copy of the input. Both branches then became identical to the
  bwtree branch, so they are now one path.
- Tag 3 honours `opts.summary`: `1` keeps the in-process 13-ratio sweep, `0`
  runs the single `-b` ratio so the sweep can be driven one process per ratio.
- **Blocked on data**: tag 3's `ratio_seq` needs ratio dirs
  `{10000,5000,2000,1000,500,200,100,50,20,10,5,2,1}`; only
  `{1,10,100,1000,10000}` exist for any distribution, so it aborts on ratio 2.
  Regenerate with `gen_graph.sh`'s full `batch_ratio_seq` uncommented before
  trying to validate. `merge_batch_updates.py` expects one `Tree:` header
  followed by a single line of 26 values, which a per-process sweep would break.

### Masstree range query — `scan` stack array (fixed 2026-07-28)

`MassTreeIndex::scan` sized a stack array from the caller's argument:

```cpp
Str results[range];   // Str is 16B; range reaches ~5e5 on rec_type 2 => ~8MB
```

That overruns the 8MB thread stack, which is the crash `ConQueryAfterStep` and
the `range_query` lambdas were dodging with `if (index_type == TYPE_MASSTREE &&
rec_type == kRangeQueryType - 1)`. Replaced with a `thread_local
std::vector<Str>` grown on demand and reused (bounded ~16MB/thread by the 1e6
box cap), and **all three range query skips removed**.

Measured at 1e8 after the fix — rec2 is correct but expensive:

| | rec0 | rec1 | rec2 |
|---|---|---|---|
| insert block | 0.015 | 0.55 | **~76s** |
| delete block | 0.020 | 0.71 | **~105s** |

rec2 boxes hold ~141x the elements of rec1 and cost ~138x the time, i.e. time
tracks element count — evidence the scan is doing complete work, not bailing
early. Cost: ~12 min/run vs ~1 min before, since rec2 adds 75-105s per ratio
across 8 ratios. Masstree is ~7.5x slower per element than bwtree here (3.3e8
vs 2.5e9 elements/s) because its scanner writes a 16-byte `Str` per element.

### Masstree range count — shared `q_[0]` data race (fixed 2026-07-31)

**The old note here was wrong.** It claimed the count path "cannot crash" and was
merely slow, so the skip was cosmetic. It is not: with the skip removed, masstree
**SIGSEGVs inside tcmalloc** (`CentralFreeList::ReleaseListToSpans`) at n=1e6 on
16 threads, within a second. Reproduce with
`./build/index_microbench_workload -p <file.bin> -D uni -t 0 -q 4 -T 5 -r 1`.

**Root cause — one shared `query` object, not slowness.** `mt_index` holds a
single `query<row_type> q_[1]` member (`mtIndexAPI.hh`), and the old
`range_count` walked the range by calling `dynamic_get_next` once per key, which
runs its scan through that shared `q_[0]`. `query_json_scanner::visit_value`
(`kvrow.hh:405`) rewrites `q_.scankey_` — a **refcounted `lcdf::String` it
reallocates whenever the key no longer fits** — and `run_scan` clears/refills
`q_.f_`, a `std::vector`, on entry. Two threads scanning at once therefore free
the same buffer twice. `get_next_n` never had the problem because its scanner is
a plain stack local and it calls `table_->table().scan()` directly, bypassing
`q_` entirely — which is why range *query* worked and range *count* did not.

**The fix**: `mt_index::count_range` (`mtIndexAPI.hh`, next to `get_next_n`) —
a stack-local `count_scanner` whose `visit_value` compares the key bytes against
the upper bound and returns false to stop. `MassTreeIndex::range_count`
(`index.h`) byte-swaps both ends of the box and makes one `count_range` call.
This is thread safe *and* one tree descent per query instead of one per key,
with no Json allocation at all.

Correctness checked against `GenRectangles`' own oracle — the boxes are built as
`(wp[l].key, wp[r].key)` over the sorted deduplicated input, so `.second == r-l`
is the exact expected count. 50 000 boxes, 0 mismatches, at 8 and 32 threads.

**Both masstree range-count skips are gone** (`ConQueryAfterStep` and the
`range_count` lambda in `ApplyConcurrentIndex`), so masstree no longer emits `-`
in any column. Measured at n=1e6 / 64 threads, rec_type 1: masstree 0.064s vs
bwtree 0.096s — it is now the *faster* of the two on this column.

**`range_report` still has the identical race** and is marked `WARN` in
`index.h`. It is dead code (`ConRangeQuery` calls `scan()`, the branch that would
use it is commented out), but it drives the same `dynamic_get_next`. Rebuild it
on a stack-local scanner before ever calling it from more than one thread.

## Scalability sweep (`scripts/scalability.sh`)

Sweeps `PARLAY_NUM_THREADS` over `1 4 16 32 64 112 224` at n=1e9, pinning each
step with `taskset` (stride 4 at 1/4/16 threads, stride 2 at 32, then dense) plus
`numactl -i all` above 1 thread. `scalability_concurrent.sh` is the same sweep
without the taskset/numactl prefixes, for the concurrent indexes.

`round` is set **per thread count** — 1 round at 1 and 4 threads, 2 above — so a
1-thread run does not cost 4× a 224-thread one. Timings are per-round averages,
so they stay comparable across the sweep.

Config: `tag=2#0011` (batch insert 1<<0 + batch delete 1<<1), `query_type=0`,
`batch_ratio=0.01`, `dis_seq=("uni" "exp")` — norm/dual_norm are commented out.
`scalability.sh` currently has only MassTree and BwTree uncommented; the tree
side (0–3) is commented out but its logs are present from an earlier run.

### Log layout — the core count is only in the filename

`logs/scalability/<tree>/<dis>/<cores>.log`, one run each: a `Tree:` header then
**3 values — build, insert, delete** (the primal build always reports, then the
two tag bits; `query_type=0` means no query columns, unlike the incremental
sweep's 90).

The thread count appears **nowhere in the log body** — `Buf:` is the leaf size
(64 for BTree, 1 for the rest), not the core count. So `merge_scalability.py`
walks `<tree>/<dis>/<cores>.log` per file and takes cores from the stem;
concatenating first and parsing the flat file, the way
`merge_incre_updates.py` does, would silently lose the sweep's x axis. The
concatenated log is still written (with a `>>> <path>` marker per file) for
reading by hand.

### `merge_scalability.py`

Writes `data/scalability/<name>.csv`, **long/tidy**: one row per
`(benchmark, tree, cores)` = 9 columns, `build/insert/delete` plus
`build_su/insert_su/delete_su`, where `*_su` is self-relative speedup
`t(1 core) / t(n cores)`. Long rather than cores-as-columns because cores is the
x axis of every plot this feeds. Speedup cells are left **blank** when the 1-core
baseline for that `(benchmark, tree)` is missing or blank — never fabricated from
a partial baseline. Sorting is `bench_order`, `tree_order`, then cores
**numerically** (a lexical sort gives 1, 112, 16, 224…).

Shares the incremental merge's three robustness rules: `-` is a legal value,
values accumulate until the next header so a `madvise`-spliced row still parses,
and a short row is padded with a warning. A header with no timings at all (a run
that died before printing) is reported and skipped rather than emitted as an
empty row.

Current state: 84 rows = 6 trees × 2 distributions × 7 core counts, all clean.
One stray `logs/scalability/3/uni/.log` (empty filename stem, header only, no
timings) is skipped with a note — note that both bash's `*` and Python's `glob`
skip leading-dot names, so it is invisible to the concatenation but *is* seen by
the per-file walk, which uses `os.listdir`.
