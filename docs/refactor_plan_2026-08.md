# Refactor plan — pab_tree

Companion to `docs/code_review_2026-08.md` (2026-08-17). Scope: `include/pb_tree/`,
plus the mechanical passes over `tests/`, `CMakeLists.txt` and `scripts/`.

> `/docs` is gitignored (`.gitignore:7`). `git add -f docs/*.md` to track these.

## Goals, in priority order

1. High performance — the overriding constraint
2. Easy to use
3. Easy to extend
4. Good readability
5. Good standard

## Ground rules

- The parallel algorithms do not change. See *Frozen list*.
- Existing asserts are not edited. New asserts are fine.
- Submodules are not touched. `external/baselines/` is not refactored.
- `unit_test` is the correctness gate after each mechanical pass. Its datasets
  are present on this machine at `/ssd1/zmen002/kv_pair/`.
- Comments stay short and factual. No blocks, no jargon.

## Settled decisions

| # | Decision |
|---|---|
| Name | `pab_tree::btree` |
| Naming | STL style: `lower_snake_case` for everything except `CamelCase` template parameters and `MACRO_NAME` macros. No `k` prefix. Concepts are snake_case. |
| Members | Ctor params keep the leading `_`; members stay plain. `root_` → `root` for consistency. |
| Format | The Linux kernel's own `.clang-format`, verbatim (8-wide tabs, 80 columns) |
| Scope | Rename + reformat `include/` and `tests/`; rename CMake `PMOS` targets; `scripts/` follows |
| Duplicates | **Set.** Keys must be distinct; debug-only assert, zero release cost |
| Bad assert | `b_batch_insert.hpp:86` stays; the correct check is added beside it |
| Loggers | Deleted entirely. `contain` → `bool`, `range_count` → `size_t` |
| Copy | Deep copy fixed, not deleted |

Defaults taken without asking — say if any is wrong: drop the vestigial
`NodeType` and `kSkHeightVal` trait parameters; defer comparator and allocator
parameters; add `.git-blame-ignore-revs` for the reformat commit.

---

## What the performance audit found

Four candidates for algorithm-neutral waste were investigated. Three were
dismissed:

| Candidate | Verdict |
|---|---|
| `NodeSplitPairSeq::uninitialized(2 * In.size())` — 32 B scratch per input element | **Load-bearing.** The `2n` sizing is the recursion's addressing scheme: each child gets `cut(2l, 2r)` matching its input range `[l,r)`, which is what makes the sub-ranges disjoint. It is `uninitialized`, so untouched pages never fault in. |
| `GetNodeBoxRec` — recursive subtree traversal, heap alloc and `parallel_for` per level | Release-clean. All three sites are `#ifdef DEBUG`. It is why `-DDEBUG=ON` builds are slow. |
| `LeafNode::GetEle()` returning `EntrySeq` by value | Release-clean, but a trap once asserts are on. `validation.hpp:21-22` calls it twice just to read `.size()`. |
| Dead instrumentation — 4 timers, 4 doubles, 2 ints, 3 atomics | Per-tree, not per-operation. Delete for size, not speed. |

There is no significant speedup sitting in the library. The performance job is
**not to regress**. Every step below is tagged: **[=]** neutral ·
**[+]** small win · **[!]** measure.

## Frozen list

- The `2n` `node_split_slice` addressing and its `cut(2l, 2r)` partition
- `BalanceOverLeaves`' in-place compaction and its `tree_idx <= i`,
  `tree_idx == split_idx`, `bkt_n <= j - i` invariants
- `BatchInsertUpdateInterior`'s `new_node_n == GetNodeN()` fast path
- `HandlingLeafSplitOverflow`'s sorted `std::ranges::merge` path
- The branchless `BinarySearch` and the cache-line-derived `kBinarySearchBase`
- The `SplitBatchBinarySearch` / `SplitBatchMerge` threshold
- `parallel_for` granularity choices
- The leaf-sort policy and `DefaultHybridPred`

If a step appears to need one of these, re-scope the step.

---

## Phase order

Deletions first, then renames, then formatting, then new code. Reformatting
before deleting churns lines that are about to disappear; renaming after
formatting causes a second full-file diff.

| Phase | Theme | Effort | Perf | Breaking |
|---|---|---|---|---|
| 0 | Safety net + perf baseline | ~2 d | [=] | no |
| 1 | Seven stop-ship defects | ~2 d | [=] | minor |
| 2 | API boundary | ~1.5 d | [+] | yes |
| 3 | Dead weight + one include root | ~1 d | [=] | yes |
| R1 | Rename to `pab_tree` | ~0.5 d | [=] | yes |
| R2 | Identifiers to STL style | ~1 d | [=] | yes |
| R3 | Reformat to kernel style | ~0.5 d | [=] | no |
| 4 | Extension points | ~3–4 d | [!] | yes |
| 5 | Readability | ~1 d | [=] | no |
| 6 | Consumability + docs | ~1.5 d | [=] | no |

---

## Phase 0 — Safety net and performance baseline

### 0.1 Warnings back on — ~1 h · [=]

Remove `add_compile_options(-w)` (`CMakeLists.txt:23`), restore `-Wall -Wextra`.
Measured fallout: 10 warnings, 4 classes, all in the library.

| Site | Warning | Fix |
|---|---|---|
| `tree_node.h:67`, `:83` | `-Wreorder` ×6 | Mem-init lists order `is_sorted` before `aug`; declarations say `elements; aug; is_sorted`. Reorder the lists. |
| `validation.hpp:45` | `-Wunused-variable` | `auto node` feeds only the commented-out assert on line 47. |
| `node_op.hpp:279` | `-Wunused-variable` | `auto size = T->size;` is dead. |
| `box_op.h:53` | `-Wsign-compare` | `int` vs parlay size type. |
| `b_tree.h:139` | `-Wchanges-meaning` | `using TT = TT;` is redundant. |

### 0.2 Asserts live in the test build — ~5 min · [=]

```cmake
target_compile_options(unit_test PRIVATE -UNDEBUG)
```

Verified: `-DNDEBUG -UNDEBUG` leaves asserts on.

Use `-UNDEBUG`, not `-DDEBUG=ON`. `DEBUG` additionally enables loops that call
`GetNodeBoxRec` once per child on every interior update — a recursive subtree
traversal with heap allocation and a `parallel_for` at every level. That is why
debug builds are unusable.

### 0.3 `Validate()` returns a real verdict — ~4 h · [=]

Today it is `return true` unconditionally (`validation.hpp:190`).

1. Propagate failure from `VerifyTreeProperty`; mark `[[nodiscard]]`.
2. Add the check `kCheckSize` implies but never performs:
   `TI->size == Σ child->size`. Highest-value single check here —
   `RangeReportIter` derives disjoint parallel output slices from
   `child->size`, so a stale value is a data race nothing detects.
3. Add null-child and `GetSplitN() == GetNodeN()-1` checks.
4. Replace `GetEle()` with `GetEleSlice()` at `validation.hpp:21, 22, 25, 28`
   and `leaf_op.hpp:75`. `GetEle()` heap-allocates and copies a whole leaf;
   `GetEleSlice()` already exists and is free. Without this, 0.2 makes the test
   build much slower.

Rule check: step 4 edits assert *arguments*, not assert semantics. If you want
the no-edit rule read strictly, skip it and accept the slower test build.

### 0.4 Dataset-free test file — ~6 h · [=]

`tests/unit_test.cpp` reads `/ssd1/zmen002/kv_pair`, which exists here but not
on a fresh clone, where all 16 tests skip and ctest passes green.

Add `tests/unit_test_api.cpp`: self-contained, generated input, cross-checked
against `std::set`. Cover copy/move and self-assign; `Build` twice on one
instance; `sorted_input` both ways asserting identical entries; all three
leaf-sort patterns; empty-tree queries; delete-everything-then-query;
`RangeCount == RangeReport().size()` over random boxes; `kBufferSize` 2, 4, 8,
64.

**Acceptance: this file must fail on current `HEAD`.**

### 0.5 Performance gate — ~4 h

Datasets: `/ssd1/zmen002/kv_pair/{uni,exp,norm,dual_norm}/{1e6..1e8}_1/`.

```bash
./build/b_test -p /ssd1/zmen002/kv_pair/uni/1000000_1/ins/100/1.base.bin \
               -d 1 -T 0 -t 3 -q 7 -r 3 -b 0.01 -s 0 -D uni
```

Run at two leaf sizes (`-DLEAF=64`, `-DLEAF=32`) and two distributions (`uni`,
`exp`). Record to `scripts/logs/refactor_gate/<phase>/`.

Establish the noise band by running the baseline 5× first. Do not gate on the
ratio-1e-4 column — CLAUDE.md documents it swinging 7.16–14.51 s for the same
chain. Confirm on `100000000_1` at phase boundaries only.

---

## Phase 1 — Stop-ship defects

All neutral: cold-path guards, a missing free, a corrected variable,
compile-time fixes.

- **1.1 Null-root guards — ~15 min · [=].** `membership.hpp:14`,
  `range_count.hpp:17`, `range_report.hpp:26` (extend the degenerate-box guard
  at line 19), `utility.hpp:24`. One predicted branch per query call, not per
  node.
- **1.2 `Build_` leak — ~5 min · [+].** `b_build_tree.hpp:34`: add
  `this->DeleteTree();` first, drop the redundant call at
  `b_batch_insert.hpp:65`. Verify RSS flat across 50 rebuilds; currently
  +3.3 MB each.
- **1.3 Constructor ambiguity — ~30 min · [=].** `b_tree.h:77-87`: delete the
  by-value overload, constrain the survivor with
  `requires (!std::same_as<std::remove_cvref_t<Range>, btree>)`, mark
  `explicit`. It is currently uncallable, so nothing depends on it.
- **1.4 Fix the deep copy — ~1 h · [=].** `node_op.hpp:685, 691`: add
  `typename`, use `EntrySeq` (what `GetEle()` returns, not `EntrySlice`), give
  `AllocInteriorNode<Interior, TT>` explicit arguments. Make it static, take
  `Node const*`. Add a test that copies, mutates the original, and checks the
  copy.
- **1.5 `BatchDelete_` sorted input — ~2 h · [=].** `b_batch_delete.hpp:80`
  passes `sorted_seq` (empty when `sorted_input==true`) where it built
  `sorted_slice`. Retype `BatchDeleteRecursive`'s parameter to the existing
  `TT::ConstKeySlice` and propagate const through `DeletePoints4Leaf`,
  `SplitBatchBinarySearch`, `SplitBatchMerge` — all read-only.
  Also add, beside the existing assert at `b_batch_insert.hpp:86`, the check
  the flag actually promises:
  `assert(!sorted_input || parlay::is_sorted(sorted_slice, key_less));`
  The old assert stays, so sorted-input inserts still abort under `-DDEBUG=ON`.
- **1.6 Root-invalid vs root-empty — ~3 h · [=].** `b_batch_delete.hpp:85`, the
  live data-loss bug. Test `root->size == 0` instead of the validity flag;
  otherwise fall through to the flatten-and-rebuild recovery twelve lines below.
  Factor both into a private `rebuild_from_flatten()`. Verify: 300 entries at
  `kBuf=64`, delete keys 0–99 and 150–269, expect size 80 (currently 0).
- **1.7 Duplicate keys — ~1 h · [=].** The tree is a set. Document the
  precondition on `Build`/`BatchInsert`; add a debug assert after the sort using
  a **strict** comparator, which catches duplicates in O(n) on already-sorted
  input. `GenerateLeafNodes` is not touched, so the build hot path is unchanged.

---

## Phase 2 — API boundary

Breaking; land as one release. Net performance slightly positive.

- **2.1 Move tests off `GetRoot()` — ~1 h · [=].** They dump the tree via
  `GetRoot()` + `FlattenRec`; `GetEntries()` is the supported path.
  Prerequisite for 2.2.
- **2.2 Insert `private:` — ~2 h · [=].** Before `Build_` (`b_tree.h:264`).
  Tests touch only 13 members plus `ParseInput`, `ParseDeleteInput`,
  `FlattenRec` and the `Leaf`/`Interior` aliases, so ~73 internals go private
  with no churn beyond 2.1.
- **2.3 Shared statics to `pab_tree::detail` — ~2 h · [=].** `NodeIsValid`,
  `GetNodeInvalidStatus`, `NodeHasLeafChildren`, `GetBktNum`, `GetBktStIdx`,
  `GetBktEndIdx` are node predicates. Keep them `static constexpr`/`inline`.
- **2.4 Const-correct the observers — ~1 h · [=].** `GetSize`, `GetEntries`,
  `Validate`, `SortedLeafRatio`, `GetRoot` are non-const while the queries
  already are. Add `Node const* GetRoot() const`.
- **2.5 Delete dead instrumentation — ~15 min · [+].** `b_tree.h:633-648`,
  288 of 296 bytes.
- **2.6 Delete the loggers — ~3 h · [+].** Remove `dependence/loggers.h` and
  all four types. `Contain` returns `bool`, `RangeCount` returns `SizeT`,
  `RangeReport` returns the count. Nothing ever populated them, and `Contain`
  currently builds and returns a 16-byte struct per call. Measure with 0.5's
  membership column.
- **2.7 `RangeReport` output contract — ~1 h · [=].** It never consults
  `out.size()` — verified writing 496 entries past a 4-slot slice. Clamp once
  per leaf and report truncation, or add an allocating overload. Do not add a
  per-element check inside the copy loop.

---

## Phase 3 — Dead weight and one include root

~700 lines unreachable. Runtime-neutral, compile-time win. **3.1 before 3.3** —
`basic_point.h` is currently the accidental source of `b_tree.h`'s parlay
includes.

- **3.1 Include what you use — ~1 h.** `b_tree.h` uses `parlay::sequence`,
  `parlay::slice`, `parlay::map` and four timers while including no parlay
  header. Add those plus `<atomic>`, `<ostream>`, `<utility>`, `<tuple>`;
  `<cassert>` to the 11 headers calling `assert`; fix `box_op.h:5`'s
  `#include "utility"` → `<utility>`.
- **3.2 One include root — ~2 h.** Three conventions today:
  `"pb_tree/b_tree.h"` (7), `"dependence/type_trait.h"` (10),
  `"../../b_tree.h"` (7). Rewrite all as `"pab_tree/…"`, then remove
  `include/pb_tree` from `PMOS_INCLUDE_DIR` so regressions fail the build.
  Merge this with R1, which rewrites the same lines.
- **3.3 Delete `basic_point.h` — ~15 min.** ~300 lines, unblocked by 3.1.
- **3.4 Delete the sieve subsystem — ~1 h.** `b_batch_insert_sieve.hpp` (already
  commented out at `b_tree.h:656`), the `BatchInsertSieve` and
  `BatchInsertSieveRecursive` declarations, `points_op.hpp` 44–115 and 135–178,
  and the dead constants `kPivotNum`, `kBucketNum` (both reference trait members
  that are commented out), `kBuildDepthOnce`, `kLog2Base`, `kBlockSize`. Retires
  the misspelled `SeievePoints`/`RetriveTag` without a rename.
- **3.5 Other dead declarations — ~15 min.** `SplitLeaf`,
  `InsertToLeafNoOverflow` (declared, never defined), `PseudoRandomGenPattern`
  (duplicate of `DefaultHybridPred`), `ContainRec` (superseded by `ContainIter`).
  Also in `comparator.h`: `Geq`, `Leq`, `IsZero`, `DivideTwoCeil` and
  `integer_log2_upper` have no callers — their only occurrence is their own
  definition. `integer_log2_upper` carries a `// BUG: this is a bug, should
  return -1` comment on its `a == 0` branch; deleting the function settles it
  (returning -1 would wrap for unsigned `T`, which is presumably why it was
  left).
- **3.6 Remove the ADL `operator<<` — ~15 min.** `b_tree.h:15` injects a generic
  `std::pair` printer into the namespace. Deleting the loggers (2.6) removes the
  query-result collision, but delete the operator too.
- **3.7 Header self-containment check — ~1 h.** Compile each header standalone
  with one `-I`.

---

## Phase R — Rename, restyle, reformat

Three commits, in this order, each gated by `unit_test`. Keep them mechanical:
no behaviour changes ride along, so a failing test means the pass was wrong.

### R1 — `pb_tree` → `pab_tree` — ~0.5 d

- `include/pb_tree/` → `include/pab_tree/` (`git mv`, so history follows)
- namespace `pb_tree` → `pab_tree`
- guards `PBTREE_*` → `PABTREE_*`
- `IsPBTree` → `is_pab_tree` (R2 does the case), `PBTreeTag` likewise

**Macro hygiene.** Audited: the library defines exactly 24 macros and every one
is an include guard, already `PBTREE_`-prefixed. Two are malformed and the
rewrite fixes them — `loggers.h` is missing the trailing underscore
(`PBTREE_DEPENDENCE_LOGGERS_H`), and `b_batch_delete.hpp` has mixed case
(`..._B_Batch_DELETE_HPP_`). `box_op.h` is a `.h` with an `_HPP_` guard.

The library reads exactly one project macro: **`DEBUG`, at 9 sites**. Rename it
to `PABTREE_DEBUG` and update `CMakeLists.txt:87`. This is the one that matters
— `DEBUG` is among the most commonly defined macros anywhere, and a consumer
whose build sets it silently switches on the `GetNodeBoxRec` verification loops,
which are an O(subtree) traversal with heap allocation per child per interior
update. That is a severe, silent slowdown triggered by a name collision.

Everything else in the project's macro vocabulary is tests-only: `MEMCHECK`
(1 site), `DYNAMIC_GRAPH` (11), `OSM` (11), `MAC` (2), `CCP` (1), `SIEVE`,
`LEAF<N>`. Prefixing those is optional and lower priority, but `MAC` and
`SIEVE` are generic enough to be worth it if `tests/` is being touched anyway.

Use `PABTREE_`, not `BTREE_`, to match the guards and the namespace.

No `BLOCK_SIZE` rename is needed: it is not a macro in this codebase.
`kBlockSize` is a `static constexpr` member, it becomes `k_block_size` under the
lower-case rule so it cannot be captured, and 3.4 deletes it as dead sieve code.
- CMake: `project(PMOS)` → `project(pab_tree)`, `add_library(PMOS INTERFACE)` →
  `add_library(pab_tree INTERFACE)`, `PMOS_INCLUDE_DIR` →
  `PAB_TREE_INCLUDE_DIR`, `PMOS_SOURCE_DIR` → `pab_tree_SOURCE_DIR`
- `tests/` call sites follow
- `scripts/` follow — they name CMake targets

Fold 3.2 in here: both rewrite every include line.

### R2 — Identifiers — ~1 d

Kernel and STL naming agree on functions, variables, structs and macros — all
`lower_snake_case`, macros `UPPER_CASE` — verified against
`/usr/include/linux/*.h` and libstdc++ 15.1.1. They diverge in three places;
the scheme below is the settled resolution.

| Category | Rule | Example |
|---|---|---|
| classes, functions, members | `lower_snake_case` | `batch_insert`, `btree_interior_node` |
| constants (`static constexpr`) | `k_` + `lower_snake_case` | `kBufUB` → `k_buf_ub`, `kSplitFactor` → `k_split_factor` |
| enum values | `lower_snake_case`, no prefix | `kSorted` → `sorted` |
| type aliases | STL name where one exists, else bare `lower_snake_case` | `E` → `value_type`, `K` → `key_type`, `SizeT` → `size_type`, `EntrySeq` → `entry_seq` |
| concepts | `lower_snake_case` | `IsPBTree` → `is_pab_tree` |
| template parameters | `CamelCase` | `granularity` → `Granularity` |
| macros | `UPPER_CASE` | unchanged |

Everything except template parameters and macros is lower case. Nothing in the
library should be `UPPER_CASE` unless it is a macro. The `k_` prefix marks
compile-time sizes only; enum values are values of a type and stay bare.

Specifics:

- `BTree` → `btree`, `BTreeInteriorNode` → `btree_interior_node`
- `BatchInsert` → `batch_insert`, `GetSize` → `get_size`

All 24 `k`-prefixed names in the library today, classified — the whole of the
constant rename:

| Today | Becomes | Why |
|---|---|---|
| `kBuf` `kBufLB` `kBufMD` `kBufUB` | `k_buf` `k_buf_lb` `k_buf_md` `k_buf_ub` | `static constexpr` |
| `kBufMDFac` `kBufUBFac` | `k_buf_md_fac` `k_buf_ub_fac` | `static constexpr` |
| `kSplitFactor` `kBinarySearchBase` | `k_split_factor` `k_binary_search_base` | `static constexpr` |
| `kSkeletonHeight` `kSerialBuildCutoff` | `k_skeleton_height` `k_serial_build_cutoff` | `static constexpr` |
| `kEnableGranularity` `kSortReinsertEntries` | `k_enable_granularity` `k_sort_reinsert_entries` | local `constexpr bool` |
| `kSorted` `kUnsorted` `kHybrid` | `sorted` `unsorted` `hybrid` | enum values — no prefix |
| `kBufferSize` `kSkHeightVal` | `BufferSize` `SkHeightVal` | template parameters |
| `kCheckSize` `kVerbose` | `CheckSize` `Verbose` | template parameters |
| `kBlockSize` `kBucketNum` `kBuildDepthOnce` `kLog2Base` `kPivotNum` | *deleted* | dead sieve constants, removed in 3.4 |

Five of the 24 need no rename because 3.4 deletes them — another reason Phase 3
runs before Phase R.
- only five template parameters are non-compliant: `granularity` →
  `Granularity`, `sorted` → `Sorted`, `kCheckSize` → `CheckSize`,
  `kVerbose` → `Verbose`, `_binary_search_base` → `BinarySearchBase`.
  Template parameters take **no** `k_` prefix even when they are constants —
  `kCheckSize` is a template parameter, so it becomes `CheckSize`, not
  `k_check_size`. Note `k_binary_search_base` (a `static constexpr` member of
  `leaf_node`) and `BinarySearchBase` (the template parameter of
  `binary_search`) are different things and diverge here on purpose.
- `root_` → `root`; ctor params keep their leading underscore
- locals `T`, `TI`, `TL` → `t`, `ti`, `tl`, which also suits kernel style's
  preference for short local names

Keeping constants lower case also sidesteps a real hazard: a class member named
in `UPPER_CASE` is rewritten by any macro of the same name, whatever its scope.
`BLOCK_SIZE` is `#define`d in `/usr/include/linux/fs.h` and
`/usr/include/sys/mount.h`, so `kBlockSize` → `BLOCK_SIZE` would have broken on
any TU that pulled either in. As `k_block_size` it cannot.

No kernel-style subsystem prefix (`btree_insert`). C needs one because it has no
namespaces; `pab_tree::btree::batch_insert` already carries it.

Prefer `clang-rename` or `sed` per symbol over one global pass, and build after
each group. `kBuf` names appear in CLAUDE.md, `scripts/` and log formats —
update those together.

### R3 — Reformat — ~0.5 d

**`BasedOnStyle: Linux` does not exist.** Verified with clang-format 22.1.3:
the only accepted values are LLVM, Google, Chromium, Mozilla, WebKit, Microsoft,
GNU. Kernel style therefore means spelling the settings out.

The config is staged at **`.clang-format.kernel`** (repo root, not active).
Activate in this commit:

```sh
mv .clang-format.kernel .clang-format
clang-format -i $(find include tests -name '*.h' -o -name '*.hpp' -o -name '*.cpp')
```

One deliberate deviation from the kernel file: `Standard: c++20`, not `Cpp03`.
Cpp03 writes `vector<vector<int> >` with a space, which is wrong for a codebase
full of nested templates. Every other key is the kernel's, and all of them are
accepted by clang-format 22 (a bogus key errors, so a clean run means accepted).

Measured on the current library before renaming:

| File | now | kernel | max width | >80 |
|---|---|---|---|---|
| `type_trait.h` | 152 | 132 | 84 | 1 |
| `tree_node.h` | 462 | 454 | 83 | 1 |
| `b_tree.h` | 669 | 641 | 94 | 7 |
| `node_op.hpp` | 696 | 711 | 89 | 18 |

All 24 headers format cleanly. Kernel style makes most files **shorter**, not
longer — the current Mozilla config breaks more aggressively. The earlier worry
that 80 columns would wrap template signatures hard was wrong.

Across the whole library there are 49 lines over 80 columns, of which **47 are
comments and 2 are code**:

- `validation.hpp:59` — a long string literal at 7 tabs of nesting;
  `BreakStringLiterals: false` will not split it. Shorten the message.
- `comparator.h:90` — a trailing comment. Moot: `integer_log2_upper` is dead
  (its only occurrence is its own definition), so 3.5 deletes it.

So the format overflow is almost entirely the long comment blocks that the
comment rule already calls for removing. Phase 5 and R3 fix the same lines —
do Phase 5's comment triage first if you want a clean 80-column result, or
accept 47 long comment lines and let Phase 5 clear them later.

Record the commit SHA in `.git-blame-ignore-revs` and set
`git config blame.ignoreRevsFile .git-blame-ignore-revs`.

---

## Phase 4 — Extension points

The only phase with real performance risk.

Steps 4.1 and 4.2 add `update_aug` calls to the delete path. Every benchmark
here uses the empty augmentation, so those calls must compile to nothing:

```cpp
if constexpr (node_has_non_trivial_aug<typename TT::interior_aug>)
        ti->update_aug();
```

`node_has_non_trivial_aug` already exists at `concepts.h:41`, unused. Run 0.5's
gate before and after and require unchanged timings for the empty-aug case.

- **4.1 Give the interior aug its children — ~1 d · [!]** `Update()` takes no
  arguments and the aug has no back-pointer, so a subtree-sum aug cannot be
  written. Forward `aug.update(get_node_slice(), get_split_slice())` — the
  signature `tests/wrapper.h:53` already declares and nothing calls. Pass a
  computed aug at each allocation site instead of default-constructing.
- **4.2 Add the missing `update_aug` calls — ~4 h · [!]** The delete path omits
  them entirely, so even a leaf-only aug is wrong after a rebalancing delete.
  Six sites: `handle_merge`, `handle_distribute`, `balance_over_leaves`
  (`node_op.hpp:606`), `prune_out_empty_tree` (`:656`),
  `fetch_invalid_subtree_nodes` (`:400`), `batch_delete_update_interior_size`
  (`:251`). All `if constexpr`-gated.
- **4.3 Three real concepts — ~4 h · [=].** `concepts.h` has 14, one used, none
  constraining an extension point. Replace with `entry_type`,
  `leaf_augmentation`, `interior_augmentation`, attached to the trait
  parameters. Delete `find_var` and its `*reinterpret_cast<T*>(nullptr)`.
- **4.4 Default augmentations — ~2 h · [=].** Ship `pab_tree::empty_leaf_aug`
  and `empty_interior_aug` as default template arguments so the common case is
  `btree_type_trait<my_ele, 64>`. Removes ~14 of the ~45 boilerplate lines a
  first-time user writes. Biggest ease-of-use win available, free at runtime.
- **4.5 Sort by key, not `E::operator<` — ~1 h · [!]** `PostProcessLeafChange`
  (`leaf_op.hpp:31`) and `ExtractPointsInLeaf` (`:56`) sort with the element's
  `operator<` while every other ordering site uses `Entry::GetKey`. That is an
  unwritten requirement that the two agree; violate it and a leaf is marked
  sorted but ordered by the wrong field, after which `Contain` misses present
  keys. Inlined lambda should be identical codegen — measure.
- **4.6 Drop `NodeType` and `kSkHeightVal` — ~2 h · [=].** `NodeType` is
  vestigial (`Leaf`/`Interior` hardcoded at `b_tree.h:204-205`); `kSkHeightVal`
  fed only the sieve path deleted in 3.4.
- **4.7 / 4.8 Comparator and allocator — deferred · [!]** Key comparison is
  hardcoded across ~15 sites; allocation names `parlay::type_allocator` in 8.
  Both are features, not debt. When added they must be template parameters
  resolved at compile time.

---

## Phase 5 — Readability

After the deletions, so nothing polished is about to be removed. Cosmetic only.

- **5.1** Resolve the 136 commented-out code lines: restore or delete.
- **5.2** Triage the markers — 50 `NOTE`, 14 `TODO`, 5 `WARN`, 1 `HACK`. `WARN`
  and `HACK` mark real hazards (`b_batch_insert.hpp:122`'s signed-loop `HACK`
  matters); keep those, shorten the rest or drop them.
- **5.3** R2 settles `typename TT::X` vs `TT::X` — 18 and 56 occurrences today.
  The bare form is illegal in template argument lists, which is exactly the bug
  in 1.4.
- **5.4** One line at the top of the main header: `.h` is interface, `.hpp` is
  member-template definitions included at the bottom and never on their own.
- **5.5** One line on the trailing-underscore inner forms (`build_`,
  `batch_insert_`), which become private in 2.2.

---

## Phase 6 — Consumability and docs

- **6.1 Installable — ~4 h.** There is an interface library with an
  `INSTALL_INTERFACE`, but no `install()` rule, no exported config, and
  `CMAKE_INSTALL_INCLUDEDIR` is never defined, so it expands to nothing.
  Depends on 3.2 / R1.
- **6.2 README — ~4 h.** Last, because 1.7, 2.6 and Phase 4 change its answers.
  Needs a worked example (~45 lines today, ~25 after 4.4), the element and
  augmentation contracts, the distinct-key rule, node ownership (`get_root()`
  is non-owning; lifetime ends at `delete_tree()`/destructor), and how to size
  `range_report`'s output.
- **6.3 Augmentation example test — ~2 h.** A `sum_aug` asserting
  `root->aug.sum == Σ get_entries()` after build, insert and delete. Stops
  4.1–4.2 regressing and documents 6.2.

---

## If you only have a week

Phases 0 and 1 (~4 days). The tree stops losing data, stops segfaulting on
empty, compiles its own API, has tests that run on a fresh clone, and has a
performance baseline on disk.

Forced to three: **0.2** (asserts live, 5 minutes, unblocks 152 dormant
checks), **0.5** (the perf gate — without it nothing else is safe to merge),
**1.6** (stop freeing live data).

## What not to do

- Don't fix defects before Phase 0 — you won't be able to tell whether the fix
  worked.
- Don't touch the frozen list. The `2n` scratch buffer in particular looks like
  obvious waste and is not.
- Don't use `-DDEBUG=ON` as the test configuration. Use `-UNDEBUG`.
- Don't add `update_aug` calls ungated — without `if constexpr` Phase 4 taxes
  every benchmark here.
- Don't rename `SeievePoints`/`RetriveTag` — 3.4 deletes their owner.
- Don't combine R1, R2 and R3 into one commit, and don't let a behaviour change
  ride along with any of them.
- Don't start Phase 4 before Phase 2 — every intermediate signature is an API
  promise while ~73 internals are public.
