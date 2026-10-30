# Code review — `include/pb_tree/` (2026-08-17)

Scope: the library only — 24 headers, ~4,900 LOC. `external/`, `scripts/` and
`tests/` were excluded except where they change what the headers do.

Method: nine reviewers read the library in parallel along separate axes; one
worked empirically, writing a minimal client and compiling it by hand. Every
correctness claim was then handed to an adversarial verifier told to *refute*
it and to default to "refuted" when it could not positively confirm. 27
findings survived that gate, several with narrowed triggers. Refuted claims
were dropped — including an `unsigned`-underflow class that turned out to be
guarded, and several claims about the sieve path, which is dead code.

**89 findings: 13 critical, 31 high, 34 medium, 11 low.**

---

## Verdict

The parallel algorithms hold up. Randomized differential stress against
`std::set` passed clean, `BalanceOverLeaves`' in-place compaction invariants
were independently re-derived and confirmed, and the fixed-array bounds
arithmetic is provably safe.

What fails is everything a *library* needs on top of that: two of the five
special members do not compile, routine deletion can free live data, and the
entire safety net — 152 asserts, `Validate()`, and the test suite — is inert in
the build people actually produce.

---

## Stop-ship defects

Seven distinct problems, deduplicated from the 13 critical findings.

### 1. Deleting from a small tree frees live data — `b_batch_delete.hpp:85`

*Fires in practice.* The code treats an invalid interior root as an empty one.
It isn't: `BatchDeleteUpdateInteriorSize` marks the root invalid when *every*
child is invalid, and a leaf is invalid at any `size < kBufLB` — including
sizes 1..63. There is no relation to `size == 0`.

```cpp
if (invalid_flag) {                    // root is invalid
  if (this->root_->is_leaf) { return; }
  assert(this->root_->size == 0);      // <-- compiled out under NDEBUG
  this->DeleteTreeRec<false>(this->root_);
  this->root_ = nullptr;
}
```

At `kBufferSize=64`: build 300 entries, delete keys 0–99 and 150–269. Both
leaves fall under 64, the root goes invalid, and **80 live entries are freed** —
`GetSize()` returns 0 with no diagnostic. A randomized stress deleting ≤1/8 of
live keys per round hit this within ~100 iterations. Reachable at any tree
height, not just fan-out 2.

**Fix**: test `root_->size == 0` rather than the validity flag; otherwise fall
through to the flatten-and-rebuild recovery that already exists twelve lines
below for the `root_->size < kBufUB` case. Factor both into one
`RebuildFromFlatten()`.

### 2. Duplicate keys are silently unreachable — `node_op.hpp:31`

*Fires in practice.* `GenerateLeafNodes` takes each separator as the key of the
first element of the next bucket. When equal keys straddle a bucket boundary,
the left child holds an element whose key *equals* `split[i]` — but
`GetNodeIdContainingKey` returns the first `i` with `split[i] > k`, so those
copies are never visited again.

Measured at `kBufferSize=32`, 100,000 entries over 100 distinct keys:

```
RangeCount([0,100))  = 99040   expect 100000
RangeCount([50,51))  = 40      expect 1000
```

Via `BatchInsert`: 1,000 copies of key 7 into a 1,000-key tree gives
`RangeCount([7,8))` = **9**. Nothing in the library documents that keys must be
distinct — grepping for "duplicate", "distinct", "unique" returns nothing.

**Fix**: decide which structure this is. As a *set*, document the precondition
and add a strict-comparator `assert` after the sort in `Build_`/`BatchInsert_`
(O(n) on already-sorted input). To *support* duplicates, advance the bucket
boundary past each run of equal keys before emitting the separator.

### 3. Every query on an empty tree segfaults — `membership.hpp:14` et al.

*Fires in practice.* `Contain` hands `root_` straight to `ContainIter`, which
dereferences it on its first line. `RangeCount` (`range_count.hpp:17`),
`RangeReport` (`range_report.hpp:26`) and `GetEntries` (`utility.hpp:24`) have
the identical shape. `GetSize()` *does* guard, and so do `BatchInsert`,
`BatchDelete` and `DeleteTree` — the inconsistency is inside one class.

Two reproductions on a stock `-O2 -DNDEBUG` build:

- `Tree t; t.Contain(5);` → SIGSEGV
- build 1,000 → delete all 1,000 → `GetSize()` prints 0 → `t.Contain(1)` → SIGSEGV

Delete-everything-then-query is ordinary application state.

**Fix**: four early-outs. `RangeReport` already has the right shape at line 19
for the degenerate-box case — extend that condition with `|| root_ == nullptr`.

### 4. Copy constructor and copy assignment do not compile — `node_op.hpp:685,691`

`BTree` advertises a full rule of five and routes both copy operations through
`CopyTreeRecursive`, whose body is never instantiated by anything in the repo
and contains two hard errors:

```cpp
return AllocNormalLeafNode<TT::EntrySlice, Leaf>(TL->GetEle());
// error: dependent-name 'TT::EntrySlice' is parsed as a non-type,
//        but instantiation yields a type            -- missing `typename`

return AllocInteriorNode(parlay::make_slice(new_nodes), TI->GetSplitSlice());
// error: no matching function -- <Interior, TT> are both in non-deduced
//        positions and must be given explicitly
```

The verifier found a third defect: `GetEle()` returns an `EntrySeq`, not an
`EntrySlice`, so adding `typename` alone still doesn't fix line 685.

**Fix**: `AllocNormalLeafNode<typename TT::EntrySeq, Leaf>(...)` and
`AllocInteriorNode<Interior, TT>(...)`; make it `static`, take `Node const*`.
If deep copy isn't wanted, `= delete` both — shipping silently-uncompilable
copy operations is the one option that isn't defensible.

### 5. `BTree t(seq);` is ambiguous — `b_tree.h:77-87`

Two unconstrained, non-`explicit` single-argument constructor templates sit
back to back: `BTree(Range in)` and `BTree(Range const& in)`. For any argument
category they are equally good matches, so every call is ambiguous — verified
for lvalue, const lvalue and rvalue `parlay::sequence`.

Being unconstrained they also outrank the copy constructor for a non-const
`BTree` lvalue (the greedy-forwarding-constructor hijack). Practical
consequence: default-construct-then-`Build` is currently the *only* way to make
a populated tree.

**Fix**: delete the by-value overload; constrain and mark the survivor:

```cpp
template<typename Range>
  requires (!std::same_as<std::remove_cvref_t<Range>, BTree>)
explicit BTree(Range&& in) { this->Build(std::forward<Range>(in)); }
```

### 6. `Build()` on a populated tree leaks the old one — `b_build_tree.hpp:34`

`Build_` overwrites `root_` without releasing what was there. Measured: 50
successive `Build` calls on a 200,000-entry sequence grew RSS linearly from
18 MB to 182 MB (~3.3 MB per rebuild), and parlay prints "un-freed blocks
obtained from block_allocator" at exit.

That `BatchInsert_` calls `DeleteTree()` before *its* `Build_`
(`b_batch_insert.hpp:65`) shows this is an oversight. Because the memory sits
in parlay's pool allocator, no leak checker attributes it to the library.

**Fix**: one line — `this->DeleteTree();` at the top of `Build_` (a no-op when
`root_` is null), then drop the now-redundant call in `BatchInsert_`.

### 7. `BatchDelete(..., sorted_input=true, ...)` deletes nothing — `b_batch_delete.hpp:80`

The function computes a correct `sorted_slice` for both branches, then passes
the wrong variable:

```cpp
parlay::slice<const K*, const K*> sorted_slice{ In.begin(), In.end() };
typename TT::KeySeq sorted_seq;
if (!sorted_input) {
  sorted_seq   = parlay::sort(In);
  sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
}
BatchDeleteRecursive(this->root_,
                     parlay::make_slice(sorted_seq),   // <-- should be sorted_slice
```

With `sorted_input=true`, `sorted_seq` is default-constructed and empty; the
recursion takes its size-0 early-out. `sorted_slice` is dead on every path.
`BatchInsert_` has the identical prologue and gets it right.

Verifier's narrowing: **no in-repo caller passes `true`**, so this is latent
rather than currently firing — it detonates for the first user who takes the
documented fast path.

**Fix**: pass `sorted_slice`. Requires retyping `BatchDeleteRecursive`'s
parameter to the already-defined `TT::ConstKeySlice` and propagating const
through `DeletePoints4Leaf`, `SplitBatchBinarySearch`, `SplitBatchMerge` — all
of which only read.

---

## Four themes

### I. The safety net is disconnected at every layer

This is the finding that matters most: it explains why the defects above were
found by review rather than by the build. Four mechanisms that look like
verification are all inert simultaneously.

| Mechanism | Expected | Actual |
|---|---|---|
| 152 `assert()` | Catch invariant violations | Default build is `-DNDEBUG` — every one compiles to nothing |
| `Validate()` | Return false on a corrupt tree | `return true` unconditionally; discards `VerifyTreeProperty`/`CheckHeight`, which only assert |
| `kCheckSize` | Verify `size` consistency | No size check exists in `validation.hpp` at all — the parameter name is aspirational |
| `-Wall -Wextra` | Flag the obvious | `add_compile_options(-w)` — all warnings off, `-Wall` commented out above it |

The `kCheckSize` gap has teeth. `RangeReportIter` derives **disjoint parallel
output slices** from `child->size` (`off[k+1] = off[k] + nodes[l+1+k]->size`,
then `out.cut(off[k], off[k+1])` inside a `parallel_for`). A stale `size` gives
two workers overlapping slices and a race. Confirmed: corrupting `root->size`
and a child's `size` by hand still yields `Validate<true,false>() == 1` in a
*debug* build. For anyone adding a new mutating operation, a stale `size` is the
most likely mistake and the validator is blind to it.

Fixing this is the highest-leverage change available and is mostly deletion and
wiring, not design. Defects 1, 4, 5 and 7 would all have been caught by a single
dataset-free test file.

### II. There is no boundary between API and implementation

`b_tree.h` declares **91 member functions under a single `public:`**. The one
`private:` (line 631) covers data members only. Roughly 18 are the user-facing
contract; the other ~73 are recursion internals — `BatchInsertRecursive`,
`HandlingInteriorSplitOverflow`, `SplitBatchMerge`, `PartitionBinarySearchRec`,
`GenerateLeafNodes`, …

Two costs: a newcomer cannot tell which 18 to call, and every one of those 73
signatures is now formally a promised API — changing `BatchInsertRecursive`'s
parameter list becomes a breaking change.

Same missing boundary in the observers: `GetSize()`, `GetEntries()`,
`Validate()`, `SortedLeafRatio()` and `GetRoot()` are all non-const, so a
`const BTree&` can be passed around but not asked anything — while `Contain`,
`RangeCount` and `RangeReport` *are* correctly const. And 288 of the object's
296 bytes are dead instrumentation (4 parlay timers, 4 doubles, 2 ints, 3
atomics) that nothing reads.

### III. The extension points are nominal

`BTreeTypeTrait` takes six parameters, which reads as six ways to adapt the
tree. Two of them vary.

| Parameter | State |
|---|---|
| `EleType` | Real. Contract undocumented but discoverable. |
| `kBufferSize` | Real, well exercised across 2–1024. |
| `LeafAugType` | Works, but goes stale after any delete. |
| `InteriorAugType` | **Cannot be implemented.** `Update()` receives no arguments and the aug has no back-pointer, so it cannot see its children. |
| `NodeType` | Vestigial — `Leaf`/`Interior` hardcoded at `b_tree.h:204-205`. |
| `kSkHeightVal` | Feeds only the sieve path, commented out of the include list. |

Augmentation is the headline extension point and has three compounding
problems:

1. The interior aug cannot be computed at all.
2. The **entire delete/rebalance path never calls `UpdateAug`** — not in
   `handle_merge`, not in `handle_distribute`, not in `BalanceOverLeaves` after
   it rewrites `tree_nodes`/`split`/`node_n`, not in `FetchInvalidSubtreeNodes`
   after it recomputes `size` by hand. So even a leaf-only augmentation is
   correct after `Build`/`BatchInsert` and silently wrong after any
   `BatchDelete` that rebalances.
3. The required interface is nowhere stated. Actual contract:
   `{Update(), static ForceParallel(size_t,size_t)}` for interior,
   `{default ctor, ctor(Range), UpdateAug(Range)}` for leaf. Meanwhile the node
   forwards `SetParallelFlag`/`ResetParallelFlag`/`GetParallelFlagIniStatus`
   (nothing calls them) and the only example defines `Create`, a two-arg
   `Update`, and `Reset` (nothing calls those either).

`concepts.h` should have caught this. It defines 14 concepts; exactly one
(`IsPBTree`) is used anywhere, and only from `tests/`. None constrains an
extension point, so a wrong augmentation gives a six-frame instantiation
backtrace instead of a named requirement. It also still contains
`FindVar<T>()` returning `*reinterpret_cast<T*>(nullptr)`.

Two more walls: key comparison is hardcoded as `<`/`<=`/`==` on `K` across ~15
sites, so no custom ordering is possible; and `PostProcessLeafChange` sorts
leaves with `E::operator<` while every other ordering site goes through
`Entry::GetKey` — an unwritten requirement that an element's `operator<` agree
with its key order. Violate it and a leaf is marked `is_sorted = true` ordered
by the wrong field, after which `Leaf::Contain`'s binary search returns false
for keys that are present.

### IV. Header hygiene blocks reuse before the first line compiles

The library needs **two** include roots. Internal includes are spelled three
mutually incompatible ways — `"pb_tree/b_tree.h"` (7 uses),
`"dependence/type_trait.h"` (10), `"../../b_tree.h"` (7) — so the obvious
single-root compile dies immediately:

```
$ g++ -std=c++20 -Iinclude -Iexternal/parlaylib/include first_use.cpp
include/pb_tree/dependence/box_op.h:4:10: fatal error:
    dependence/type_trait.h: No such file or directory
```

`CMakeLists.txt` papers over this by exporting both roots — which also dumps
`comparator.h`, `concepts.h`, `binary_search.h`, `loggers.h` into the
consumer's global include namespace, where they shadow the user's own files.

Related: `b_tree.h` includes **no parlay header** despite using
`parlay::sequence`, `parlay::slice`, `parlay::map` and four
`parlay::internal::timer`s in its class body. It works only because
`dependence/basic_point.h` happens to pull them in — which means the ~300 lines
of otherwise-dead `BasicPoint`/`AugPoint` cannot be deleted without a
mysterious build break. `box_op.h:5` has `#include "utility"`, a quoted include
of a standard header.

About **700 lines (15% of the library) are unreachable**: the sieve subsystem
(header commented out at `b_tree.h:656`, but `BatchInsertSieve` still declared
and its support functions still compiled into every TU), `basic_point.h`, and
the declared-but-undefined `SplitLeaf` / `InsertToLeafNoOverflow`.
`b_tree.h:60-61` references `TT::kPivotNum` and `TT::kBucketNum`, both commented
out of the trait — it compiles only because static member initializers are
instantiated lazily. Four public identifiers are misspelled: `SeievePoints`,
`RetriveTag`, plus `Seieve`/`Retrive` variants.

Finally, `b_tree.h:15` injects a generic `std::pair` streaming operator into
namespace `pb_tree`. Because `Contain` and `RangeCount` return pairs whose
second type lives in `pb_tree`, ADL finds it, and any user with their own pair
printer gets an ambiguity error.

---

## What's genuinely good

- **The core algorithms survived adversarial review.** 250-iteration randomized
  differential stress against `std::set` at `kBuf` 2/4/8 with all internal
  asserts enabled passed clean; 600 iterations at 8/16/64 in release.
- **`BalanceOverLeaves`' in-place compaction is correct.** The `tree_idx <= i`
  and `tree_idx == split_idx` invariants, plus the non-obvious `bkt_n <= j - i`
  bound that makes the distribute case safe, were independently re-derived and
  confirmed for all three cases including the rightmost group, a leading
  invalid node, and trailing invalid nodes. No leaf is freed while still
  reachable; entries are always copied before the source is freed.
- **Fixed-array bounds arithmetic is provably safe.** The `new_node_n <= kBufUB`
  guard fires before any `NodeArr` is touched; the worst case (255 of 256
  children splitting) routes to the heap-backed overflow path.
- **The unsigned-`BufSizeT` hazard was checked and is not present.** The one
  backward loop is explicitly `int` with a `// WARN: signed` comment; every
  `- 1` site is guarded.
- **Parallel output ranges are provably disjoint** in the insert path; the
  reinsert prefix scan is computed sequentially before the `parallel_for`.
- **`LeafSortPred` as a template parameter is a genuinely good design** — zero
  overhead, fully inlinable, and the one extension point that *is* constrained
  (`std::predicate<LeafSortPred, Node*, Range>`).
- **The cache-line-derived `kBinarySearchBase`**, computed separately for leaves
  (`sizeof(E)`) and interiors (`sizeof(K)`), is careful work, and the branchless
  binary search was verified correct at every edge case.

---

## Suggested order of work

Sequenced so each step makes the next safer, rather than by severity.

1. **Turn the safety net back on.** Drop `-w`; make `Validate()` return a real
   verdict and add the subtree-size and null-child checks; add one dataset-free
   test file covering copy/move, the sorted-input flag, both leaf-sort patterns,
   and empty-tree queries. Do this first — it is what catches the rest.
2. **Fix the seven stop-ship defects.** Five are one-to-three-line changes.
   Defects 1 and 2 need a decision, not just an edit.
3. **Decide the duplicate-key contract and write it down.** Set or multiset.
   Either is fine; silence is not, and it currently costs correctness.
4. **Draw the public boundary.** One `private:` before `Build_`; move the shared
   static node predicates into `pb_tree::detail`. Delete the ~700 dead lines and
   the four misspelled identifiers now, while nothing external depends on them.
5. **Normalise includes to one root.** Rewrite every internal include as
   `"pb_tree/…"`, then remove the second root from `PMOS_INCLUDE_DIR` so
   regressions fail the build. Give `b_tree.h` its own parlay includes, which
   also unblocks deleting `basic_point.h`.
6. **Make the extension points real or remove them.** Give
   `InteriorAug::Update` the node's children; add the missing `UpdateAug` calls
   on the delete path; express the two augmentation contracts and the element
   contract as concepts attached to the trait parameters. Drop `NodeType` and
   `kSkHeightVal` if they aren't going to work.
7. **Then write the README the library deserves.** A worked example, the
   element-type and augmentation contracts, the duplicate-key rule, who owns
   `Node*`, and how `RangeReport`'s output buffer must be sized. Not before —
   several of those answers change in steps 3 and 6.
