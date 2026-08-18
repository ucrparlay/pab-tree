# Findings register — still open

16 open library findings: 9 medium, 7 low. No high ones remain. 13 are from
the 2026-08-17 review, 3 were found since.

The review raised 89 library findings. The 73 not listed here were either
fixed -- `git log 376e067..HEAD` names each defect -- or reviewed and dropped
as not worth doing. This file's own history distinguishes the two.

`docs/open_issues.md` is the short, ranked version. This one carries the
reviewer's full detail and suggested fix for each.

Paths are as reviewed (`include/pb_tree/`, old identifier names); the library
is now `include/pab_tree/` with STL-style names, so line numbers predate the
rename. `VERIFIED` means an adversarial verifier could not refute the claim.

## Closed since the last revision (2026-08-17)

| finding | closed by |
|---|---|
| batch ops reject `std::vector` (high) | `96aba15` — contiguous ranges borrowed, others materialised |
| no `size`/`empty`/`swap`/`clear`, no iteration, no single-key ops (medium) | `fc51224` — the STL container interface |
| no RAII (part of the exception-safety finding) | already done; the entry is reduced to exception safety alone |
| `ctest` cannot pass without the author's datasets (high, outside) | `dd43327` — `self_test` |
| no edge-case coverage below 1e6 (medium, outside) | `dd43327` — sizes 0…1025, leaf sizes 2…256 |
| both `DefaultHybridPred` findings (medium) | **refuted by measurement**, not fixed — see below |
| `NodeType` unconstrained, `EleAssignTag` dead (medium) | `node_base` concept; dead parameter removed |
| no allocator customization point (high) | `NodeAllocType` trait parameter, codegen-identical |
| key comparison hardcoded to `operator<` (high) | `KeyCompare` trait parameter, verified codegen-neutral |
| no comparator customization point (medium) | same change; the two were one defect |
| plus 8 more outside the library, listed in that section |  |

**The two `DefaultHybridPred` findings were closed as wrong, not as fixed.**
"ASLR-nondeterministic" is false: the leaf-sortedness pattern is identical
over 6 runs under full ASLR, under `setarch -R`, and at 1/2/8/32 threads. "Bit
3 of `begin` is zero so the address terms achieve nothing" is also false:
`bit3(begin)` is 1 in 1596 of 3124 calls and the terms flip the result in 1534
of them. Both missed that allocation bases are 16-byte aligned, which makes
bit 3 of an address the offset's bit 3 — deterministic, but not zero. The one
true part, that the comment described something the code does not do, is
fixed. The code is unchanged.

Three findings were **added**: an interior augmentation cannot read its
children's augmentations (medium), the dead `std::monostate` clause (low), and
the unreachable 3-argument interior allocator (low).

## MEDIUM — 9

### The augmentation concepts are declared but never applied
`include/pab_tree/dependence/concepts.h`:39,46 · extensibility
**Status: open — found 2026-08-17 while constraining NodeType**

`leaf_augmentation` and `interior_augmentation` have existed since the
refactor and nothing references them. A concept that is never applied
enforces nothing, so a malformed augmentation still fails deep inside a
header rather than at the user's declaration.

They cannot be attached to the trait as written. Both are parameterised on a
`Slice` type, and the library calls the leaf augmentation's constructor and
`update_aug` with more than one range type — `entry_slice` at the two
`update_aug` call sites, and whatever range `build` holds at the constructor.
Pinning the concept to a single slice type would reject valid augmentations.

Partly mitigated: `btree_type_trait` now `static_assert`s the requirements
that do not depend on a range type — both augmentations default-constructible,
and `interior_aug::force_parallel(size_t, size_t) -> bool`.

*Suggested fix:* restate both concepts over a generic range
(`template <typename A, typename R> concept leaf_augmentation = ... requires(A a, R r)`)
and apply them at the call sites in `leaf_node::update_aug` and
`btree_interior_node::update_aug`, where the range type is known, rather than
on the trait.

### No reusable traversal skeleton: every batch op re-implements partition-recurse-reassemble
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:127 · extensibility
**Status: structural**

`BatchInsertRecursive` (`b_batch_insert.hpp:127-192`) and `BatchDeleteRecursive` (`b_batch_delete.hpp:165-229`) share ~40 near-identical lines: the `static_cast<Interior*>(T)`, the identical partitioner selection

```cpp
if (In.size() > TT::kSplitFactor * TI->GetSplitN()) {
  SplitBatchBinarySearch(split_pos_seq, In, TI->GetSplitSlice());
} else {
  SplitBatchMerge(split_pos_seq, In, TI->GetSplitSlice());
}
```

the `#ifdef DEBUG` range assertion, the touched-children loop computing `l`/`r` from `split_pos_seq` and filling `nodes_map[cnt++]`, and the `parlay::parallel_for(0, cnt, ..., 1)` dispatch. Only the per-child payload and the aggregation differ. The key-vs-entry difference is handled

*Suggested fix:* Extract the shared skeleton as `template<typename Range, typename ChildFn> static BufSizeT ForEachTouchedChild(Interior* TI, Range In, SizeBufUBArr& split_pos_arr, ChildFn&& fn)` in `points_op.hpp`, where `fn(BufSizeT child_idx, BufSizeT slot, SizeT l, SizeT r)` runs inside the `parallel_for`; have both recursions call it. Replace the duplicated `if constexpr` with one `static K KeyOf(auto const&

### BatchInsert reserves 2 pairs (32 bytes) of scratch per input element regardless of splits
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:77 · performance
**Status: NOT TO BE FIXED - the 2n addressing is load-bearing**

```cpp
NodeSplitPairSeq node_split_seq =
  NodeSplitPairSeq::uninitialized(2 * sorted_slice.size());
```
`NodeSplitPair` is `std::pair<Node*, Split>` — 16 bytes with padding for a 32-bit key — so this reserves `32 * n` bytes for every batch. At the benchmarked n = 1e9 that is a **32 GB** allocation, on top of the sorted copy. The 2n factor is genuinely the worst case (a leaf at capacity receiving a single element needs 2 slots, so the ratio really is 2 at n=1), but it is the worst case for a *pathological* batch: in the common case where few or no nodes split, essentially all of it is untouched. `ReInsertTreeAfterDelete` (node_op.hpp:669) allocates the same 2n scratch a second time during re

*Suggested fix:* Bound the scratch by what can actually be produced rather than by input size. The number of nodes any subtree can hand back is `(subtree_size + batch_size) / kBufMD + 1`; sizing `node_split_seq` as `2 * min(n, root_->size / TT::kBufMD + n / TT::kBufMD + 2)` covers every split path while collapsing the common case. If that analysis is not worth doing, at least document the `32*n` bytes of scratch i

### The branchless binary search is hand-inlined four times with a hardcoded base of 8
`include/pab_tree/b_tree_impl/query_op/range_report.hpp`:98 · standards
**Status: open**

`BinarySearch<_binary_search_base>` (binary_search.h:41-76) exists precisely as the shared primitive, and the node types expose a tuned `kBinarySearchBase` derived from `std::hardware_constructive_interference_size` (tree_node.h:57-64 and :276-283). Yet the right-boundary search is copy-pasted inline four times with a literal cutoff: `while (hi - lo > 8)` at range_report.hpp:98 (interior splits) and :46 (leaf entries), and range_count.hpp:79 and :34. The bodies are identical modulo the accessor. The hardcoded 8 also throws away the cache-line tuning the rest of the library goes to some trouble to compute (for `kBuf=64` with 8-byte entries the leaf base is 8, but for 16-byte entries it is 4).

*Suggested fix:* Replace all four inlined loops with the primitive, adding an offset-taking overload so the narrowing is preserved: `template<size_t base, typename Seq, typename F> size_t BinarySearchFrom(Seq const& I, size_t lo, F const& less)` in `binary_search.h`, then call `TI->GetSplitSlice()`-based `BinarySearchFrom<Interior::kBinarySearchBase>(split_slice, l, [&](K key){ return key < query_box.second; })` i

### Deleting a key that is not in the tree aborts in Debug builds; the precondition is undocumented
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:140 · api-usability
**Status: undocumented precondition**

```cpp
assert(T->size >= In.size());
assert(parlay::all_of(In, [&](auto a) {
  return std::find_if(TL->elements.begin(), TL->elements.begin() + TL->size,
                      [&](auto const e) { return Entry::GetKey(e) == a; }) !=
         TL->elements.begin() + TL->size;
}));
```

plus `assert(new_node->size + In.size() == old_size)` at b_batch_delete.hpp:155. `BatchDelete`'s declaration (b_tree.h:319-325) documents nothing, and neither does README.md. Reproduced: build 1000 keys 0..999, `BatchDelete` of keys 5000..5099, compiled without `-DNDEBUG` -> `Assertion 'parlay::all_of(In, ...)' failed`, abort. In Release the same call is harmless (verified: size stays 1000) because `DeletePoints4

*Suggested fix:* Decide and document the contract. If absent keys are meant to be allowed (the Release behaviour, and the more useful semantics), delete the two preconditions at leaf_op.hpp:140-146 and relax b_batch_delete.hpp:155 to `assert(new_node->size + In.size() >= old_size)`; then state in the `BatchDelete` doc comment that keys not present are ignored and duplicates are idempotent, and add a unit test cove

### BatchDelete asserts every key is present; deleting an absent key aborts in non-NDEBUG builds
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:141 · correctness · VERIFIED
**Status: same as above**

`DeletePoints4Leaf` asserts that every key in the batch exists in the target leaf:

```cpp
// leaf_op.hpp:140-145
assert(T->size >= In.size());
assert(parlay::all_of(In, [&](auto a) {
  return std::find_if(TL->elements.begin(), TL->elements.begin() + TL->size,
                      [&](auto const e) { return Entry::GetKey(e) == a; }) !=
         TL->elements.begin() + TL->size;
```

Deleting keys that are not in the tree — an entirely ordinary operation for a set — aborts. My probe built 1000 entries with even keys and deleted `{1,3,5,7}`:

```
probe: leaf_op.hpp:141: ... Assertion `parlay::all_of(In, [&](auto a) { ... }) ' failed.
Aborted (core dumped)
```

The same program compiled `-O2 -D

> Verifier narrowed: Two corrections, neither of which changes the verdict:

(a) It is not one assertion but three that fire on an absent key, so a user cannot suppress it with a single-line patch:
  - `leaf_op.hpp:141-146` — the membership `all_of` (the one my probe hit first);
  - `leaf_op.hpp:164-166` — `assert(end =

*Suggested fix:* Remove the second assertion at leaf_op.hpp:141-145 (the implementation already handles absent keys correctly via the two-pointer compaction), and document on the `BatchDelete` declaration in b_tree.h:319-325 that keys not present are ignored. Keep `assert(T->size >= In.size())` only if that is genuinely required — my probe suggests it is not, since deleting 3 absent keys from a leaf is legal.

### GetEntries returns unsorted output, has no size check, and its ordering contract is undocumented
`include/pab_tree/b_tree_impl/utility.hpp`:22 · api-usability
**Status: reduced — `to_seq()` added, `get_entries` still unchecked**

`to_seq()` (fc51224) returns the entries in key order as a new sequence,
which is the ordered, allocating accessor this asked for, and the README
documents that `get_entries` is unordered. Still open: `get_entries(Out)`
does not check `Out.size() >= size()`.

`GetEntries(Range& Out)` calls `FlattenRec<TT::EntrySlice, false, /*sorted=*/false>` over `Out.begin() .. Out.begin() + root_->size`. With `sorted == false`, `ExtractPointsInLeaf` (leaf_op.hpp:51-62) copies each leaf verbatim, so any leaf with `is_sorted == false` contributes its entries in arbitrary order. It also silently assumes `Out.size() >= GetSize()` with no check (only a debug `assert(Out.size() >= TL->size)` per leaf, leaf_op.hpp:60), and dereferences a null root (see the null-root finding). The same applies to `RangeReport`'s output, whose order is 'sorted across leaves, arbitrary within an unsorted leaf'.

*Suggested fix:* Either make it sorted — `GetEntries` should call `FlattenRec<..., /*sorted=*/true>` (which sorts each leaf in place and is already used by the delete path) — or rename/document it, e.g. `void GetEntriesUnordered(Range& Out)` plus a `GetEntriesSorted`. Whichever is chosen, add the precondition `Out.size() >= GetSize()` to the declaration in `b_tree.h:505-506` and check it, and document `RangeReport

### Element type needs three wrapper layers, and the library's own layer forces a by-value copy on every key access
`include/pab_tree/dependence/type_trait.h`:21 · api-usability
**Status: open**

A user must write an element struct *and* a static-accessor wrapper (`tests/wrapper.h:130`: "NOTE: user needs to add a layer to expose API to b_tree"), and the library then wraps that again in `EleWrapper`, which adds nothing but signature changes:

```cpp
static auto GetKey(E const e)      { return EleType::GetKey(e); }
static auto GetVal(E const e)      { return EleType::GetVal(e); }
static E ToEntry(K const& k, V const& v) { return EleType::ToEntry(k, v); }
```

Note `E const e` — by value. The test wrapper takes `E const&` (`wrapper.h:139`), so the copy is imposed by the library layer, on the hottest path there is: `Entry::GetKey` appears 42 times across the impl, inside every binary-sea

*Suggested fix:* Delete `EleWrapper` and set `using Entry = EleType;` directly in `TypeTrait` (line 48 already comments that `Entry` is 'abstract type to access E'), or at minimum change the forwarders to `static K GetKey(E const& e)` / `static V GetVal(E const& e)`. Replace the class with a documented concept: `template<typename T> concept EntryType = requires(typename T::E const& e) { typename T::E; typename T::

### Trait parameters are positional and undocumented; kSkHeightVal has no discoverable meaning
`include/pab_tree/dependence/type_trait.h`:86 · api-usability
**Status: partly - README documents them now**

The only way to instantiate the tree is:

```cpp
template<typename EleType, uint kBufferSize, typename LeafAugType,
         typename InteriorAugType, uint_fast8_t kSkHeightVal, typename NodeType>
class BTreeTypeTrait
```

Six positional parameters, no defaults, no comments on the declaration, no documentation. Three of them are effectively unguessable:

- **`EleType`** is not the element. It is an access layer that must expose `E`, `K`, `V` and `static GetKey(E)`/`GetVal(E)`. I only learned this by finding `EleWrapper<EleType>` at type_trait.h:93 and `Wrapper::ElementWrapper<E>` at tests/b_test.cpp:54. Passing the element struct itself — the obvious first guess — produces 204 lines of error

*Suggested fix:* Give the declaration defaults and a doc comment: `template<typename EleType, uint kBufferSize = 64, typename LeafAugType = EmptyLeafAug, typename InteriorAugType = EmptyInteriorAug, uint_fast8_t kSkHeightVal = 8, typename NodeType = ProtoNode>`, so the common instantiation is `BTreeTypeTrait<MyEle>`. Rename `EleType` to `EntryAccessor` (or add `template<typename A> concept EntryAccessor = requires

## LOW — 7

### LeafSortPred contract is undocumented, inconsistent between insert and delete, and CLAUDE.md states it wrong
`include/pab_tree/b_tree.h`:271 · docs
**Status: undocumented**

`DefaultHybridPred::operator()(InputNode const* T, Range In)` takes a *range*, but the same predicate object is invoked with two different range types: `PostProcessLeafChange` is reached from `InsertPoints2Leaf` with an `EntrySlice` (leaf_op.hpp:87) and from `DeletePoints4Leaf` with a `TT::KeySlice` (leaf_op.hpp:173). A user-supplied predicate must therefore be a template accepting both, but the `requires std::predicate<LeafSortPred, Node*, Range>` clauses on `BatchInsert_`/`BatchDelete_` only check one of them, so a monomorphic predicate passes the constraint on `BatchInsert` and then fails to compile inside `BatchDelete`. Nothing in the headers documents the signature; `CLAUDE.md` document

*Suggested fix:* Pin the contract down. Document above `DefaultHybridPred` in b_tree.h: `// Callable as: template<class N, class R> bool operator()(N const* leaf, R batch) const; invoked with EntrySlice on insert and KeySlice on delete.` Tighten both requires-clauses to check both instantiations — `requires std::predicate<LeafSortPred, Node*, typename TT::ConstEntrySlice> && std::predicate<LeafSortPred, Node*, typ

### BuildRecursive assigns this->root_ as a hidden side effect and is not recursive
`include/pab_tree/b_tree_impl/batch_op/b_build_tree.hpp`:103 · api-usability
**Status: open**

```cpp
this->root_ =
  AssembleInteriorLevels(node_out.cut(0, bkt_n), key_out.cut(0, bkt_n - 1));
return this->root_;
```
The function both mutates `root_` and returns the same pointer; its only caller then assigns `root_` again (`Build_`, line 37). The two early-return branches (`In.size() == 0` and `In.size() < kBufUB`) do *not* set `root_`, so whether the member is touched depends on the input size. The function is also named `...Recursive` but contains no recursive call — the level-by-level loop lives in `AssembleInteriorLevels`.

*Suggested fix:* Make it a pure factory and rename it: `static Node* BuildSubtree(Range In)`, with the two `this->root_` writes at lines 103-105 replaced by `return AssembleInteriorLevels(...)`. Update the declaration at b_tree.h:267 and the single call site at b_build_tree.hpp:37.

### BalanceOverLeaves indexes group_sum with a kBufUB sentinel when no child is valid
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:435 · correctness
**Status: open - frozen code**

```cpp
BufSizeT rpidx = TT::kBufUB;
BufSizeT pivot = TT::kBufUB;
for (int i = TI->GetNodeN() - 1; i >= 0; --i) { ... }
assert(rpidx != TT::kBufUB);
```

If no child is valid, `rpidx` and every `tree_group[i]` stay at the sentinel `TT::kBufUB`. The dispatch loop then executes `const SizeT sum = group_sum[g];` (node_op.hpp:588) with `g == TT::kBufUB`, but `group_sum` is `SizeBufUBArr` = `std::array<SizeT, kBufUB>` — a one-past-the-end read of a stack array. The `assert` on line 456 is the only guard, and it is compiled out in the default build (`-DNDEBUG`, CMakeLists.txt:96).

I traced the reachability and it is currently blocked: `BalanceOverLeaves` is called only from the `SelfBalanceNode(T,

*Suggested fix:* Make the sentinel unrepresentable as an index and fail loudly. Use `static constexpr BufSizeT kNoPivot = std::numeric_limits<BufSizeT>::max();` for `rpidx`/`pivot`, and open `BalanceOverLeaves` with an unconditional guard rather than an assert: `if (TI->GetNodeN() == 0) return;` followed by, after the lambda, `if (rightmost_pivot_idx == kNoPivot) { /* caller violated the NodeIsValid precondition *

### GetEntries requires a caller-sized output and dereferences root_ unconditionally
`include/pab_tree/b_tree_impl/utility.hpp`:24 · api-usability
**Status: reduced — null guard added, `to_seq()` is the allocating overload**

The null guard is in (`utility.hpp`), and `to_seq()` is the
`auto get_entries() const -> entry_seq` this suggested. Still open: the
`Out.size()` precondition is documented but not checked.

```cpp
// utility.hpp:19-25
template<typename Range>
void BTree<TT>::GetEntries(Range& Out)
{
  this->FlattenRec<typename TT::EntrySlice, false, false>(
    this->root_,
    parlay::make_slice(Out.begin(), Out.begin() + this->root_->size));
}
```

Two issues. First, `this->root_->size` is dereferenced with no null check, so `GetEntries` on a default-constructed tree segfaults (same family as the Contain/RangeCount/RangeReport finding). Second, the signature `void GetEntries(Range& Out)` gives no hint that `Out` must already have at least `GetSize()` elements — it takes a non-const reference, which reads like it will resize. It does not; it slices `Out.begin()` to `Out.begin() + size` and `Fl

*Suggested fix:* Add the null guard (`if (this->root_ == nullptr) return;`) and either resize in the function (`Out.resize(this->GetSize());` before the slice, since `Range&` is already non-const) or, better, add an allocating overload `auto GetEntries() const -> typename TT::EntrySeq` and document the existing one as requiring `Out.size() >= GetSize()`. Also rename or document the `sorted` template argument, whic

### `node_has_non_trivial_aug`'s `std::monostate` clause is dead
`include/pab_tree/dependence/concepts.h`:15 · dead-code
**Status: open — found 2026-08-17**

```cpp
template <typename T>
concept node_has_non_trivial_aug =
	(!std::is_empty_v<T>) && (!std::same_as<T, std::monostate>);
```

`std::monostate` is an empty type, so `!std::is_empty_v<T>` already rejects it
and the second conjunct can never change the result. Nothing in the library
uses `std::monostate` as an augmentation — the defaults are `empty_leaf_aug`
and `empty_interior_aug` (`node_base.h`), both empty structs. The clause and
the "don't use the monostate" half of the TODO at `tree_node.h`:178 are
leftovers from an earlier design.

*Suggested fix:* drop the clause and the `<variant>` include it needs.

### The 3-argument interior allocator and constructor have no callers
`include/pab_tree/dependence/tree_node.h`:371 · dead-code
**Status: open — found 2026-08-17**

`alloc_interior_node(nodes, split, aug)` and the matching
`btree_interior_node(nodes, split, aug)` constructor take an explicit
augmentation. All three allocation sites (`b_build_tree.hpp`:80,
`node_op.hpp`:69, `node_op.hpp`:699) use the 2-argument form and then call
`node->update_aug()`, so neither is reachable.

Left in place deliberately: it is plausible API for someone constructing a
node with a precomputed augmentation. Delete it if that never materialises.

### `uint` is not a standard C++ type but appears in BTreeTypeTrait's template parameter list
`include/pab_tree/dependence/type_trait.h`:36 · standards
**Status: cosmetic**

Both trait templates take their leaf size as `uint`:

```cpp
template<typename EleType,
         uint kBufferSize,          // line 36 (TypeTrait) and line 87 (BTreeTypeTrait)
         ...
  static uint const kBufMDFac = 2;  // line 112
  static uint const kBufUBFac = 4;  // line 113
```

`uint` is a POSIX/glibc typedef from `<sys/types.h>`, not a C++ keyword or a standard-library name. type_trait.h includes `<cstdint>`, `<type_traits>`, `<utility>` and three project/parlay headers — none of which is documented to provide `uint`; it arrives transitively and happens to work on glibc/libstdc++. It is unavailable on MSVC and not guaranteed on libc++.

Because `kBufferSize` is a template *parame

*Suggested fix:* Change both template parameter declarations to `std::uint32_t kBufferSize` and both factors to `static constexpr std::uint32_t kBufMDFac = 2;` / `kBufUBFac = 4;` (also making them `constexpr` rather than `const`, matching `kBuf`/`kBufLB`/`kBufMD`/`kBufUB` on lines 124-127). Add `#include <array>` to type_trait.h.

