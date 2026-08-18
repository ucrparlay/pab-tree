# Findings register — still open

26 open library findings: 3 high, 16 medium, 7 low. 23 of them are from the
2026-08-17 review, 3 were found since. The review raised 89 library findings
in all, so 66 are now fixed and were removed from this file; they are
recoverable from `git log 376e067..HEAD`, which names each defect, and from
this file's own history.

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
| plus 8 more outside the library, listed in that section |  |

Three findings were **added**: an interior augmentation cannot read its
children's augmentations (medium), the dead `std::monostate` clause (low), and
the unreachable 3-argument interior allocator (low).

## HIGH — 3

### Bogus DEBUG assertion aborts any sorted-input BatchInsert that splits the root
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:86 · correctness
**Status: KEPT by your decision; correct check added beside it**

```cpp
#ifdef DEBUG
  assert(!sorted_input || new_node_n == 0);
#endif // DEBUG
```
This asserts that when the caller says the input is already sorted, the root never splits. There is no such relationship — `new_node_n > 0` means the root overflowed, which depends only on batch size, not on whether the caller pre-sorted. Confirmed: kBufferSize=32, a 200-entry tree, then `BatchInsert(b, kSorted, /*sorted_input=*/true)` with 200 000 sorted entries, compiled `-DDEBUG`:
```
Assertion `!sorted_input || new_node_n == 0' failed.
```
The same call with `sorted_input=false` succeeds. The `-DDEBUG` build is what `CMakeLists.txt:85-87` produces with `-DDEBUG=ON`.

*Suggested fix:* Delete the assertion. If the intent was to check that pre-sorted input really is sorted, replace it with the check that actually says that, placed before the descent:
```cpp
assert(!sorted_input || parlay::is_sorted(sorted_slice,
         [](E const& a, E const& b){ return Entry::GetKey(a) < Entry::GetKey(b); }));
```
and add a sorted-input case to `tests/unit_test.cpp`.

### Key comparison is hardcoded to operator< on K and operator< on E; no comparator can be supplied
`include/pab_tree/dependence/tree_node.h`:112 · extensibility
**Status: deferred (4.7) - a feature, not debt**

`TypeTrait` exposes `using Num = Num_Comparator<K>` (`type_trait.h:51`) but `Num` is used only by `box_op.h` (min/max/limits for validation boxes) — never for ordering. Every ordering decision is a raw operator:

- `tree_node.h:112,122` `Entry::GetKey(e) < k` / `<= k` (leaf binary search)
- `tree_node.h:131,136` `== k` (leaf membership)
- `tree_node.h:357,371` `key <= k` / `key < k` (interior descent)
- `tree_node.h:382-386` `ValidateKeyInNode`
- `points_op.hpp:128,130,190,196` (both batch partitioners)
- `range_report.hpp:48,54` and `range_count.hpp:36,42`
- `node_op.hpp:406,554` and `leaf_op.hpp:109,144,153`
- `leaf_op.hpp:31,37,56` `std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter())` a

*Suggested fix:* Add a seventh trait parameter `typename KeyCompare = std::less<K>` to `TypeTrait`/`BTreeTypeTrait`, expose it as `TT::KeyLess`, and route every site above through it (`TT::KeyLess{}(a,b)` for `<`, `!less(a,b) && !less(b,a)` for `==`). Replace the element sorts with key-projected sorts — `parlay::sort(In, [](E const& a, E const& b){ return TT::KeyLess{}(Entry::GetKey(a), Entry::GetKey(b)); })` — wh

### No allocator customization point: parlay::type_allocator is hard-coded, and Node* ownership is documented nowhere
`include/pab_tree/dependence/tree_node.h`:229 · extensibility
**Status: deferred (4.8) - a feature, not debt**

Every node allocation and deallocation names one concrete allocator, in eight places in tree_node.h with no indirection:

```cpp
  Leaf* o = parlay::type_allocator<Leaf>::alloc();   // 229, 239, 249
  new (o) Leaf(In, AllocNormalLeafTag());
  ...
  Interior* o = parlay::type_allocator<Interior>::alloc();  // 400, 410, 422, 431
  ...
  parlay::type_allocator<NodeType>::retire(static_cast<NodeType*>(T));  // 440
```

`BTreeTypeTrait` (type_trait.h:86-148) parameterises the element type, leaf size, two augmentation types, skeleton height and node base type — but not the allocator. There is no `Alloc` template parameter, no `TT::NodeAllocator` alias, and no allocator hook on `BTree`. A user want

*Suggested fix:* Add an allocator to the trait: `using NodeAlloc = NodeAllocType;` on `BTreeTypeTrait` defaulting to a `ParlayTypeAllocator` adaptor exposing `template<class T> static T* Alloc()` / `static void Free(T*)`, and route all eight sites through `TT::NodeAlloc`. Drop the unused `Range` parameter from `AllocEmptyLeafNode`, delete the `AllocInteriorNode<Interior,TT>()` overload. Add a README section statin

## MEDIUM — 16

### NodeType is a near-vestigial knob: Leaf and Interior are hardcoded
`include/pab_tree/b_tree.h`:204 · extensibility
**Status: partly - defaulted to proto_node, not removed**

`NodeType` is the sixth trait parameter and is bound to `pb_tree::ProtoNode` at every one of the six instantiation sites in the repo (`tests/b_test.cpp:59`, `unit_test.cpp:64`, `cpam.cpp:38`, `pma.cpp:40`, `stdset.cpp:31`, `tlxbtree.cpp:31`). What it actually controls is only the common base of the two node structs (`LeafNode : TypeTrait::Node`, `MultiWayNode : TypeTrait::Node`), and the base must expose a `(bool is_leaf, size_t size)` constructor plus public `is_leaf`/`size` fields, because `b_tree.h:224-257` (`NodeHasLeafChildren`, `GetNodeInvalidStatus`, `NodeContainInvalidSubtree`, `GetNodeReinsertSize`) and every `static_cast<Interior*>(T)` in the impl depend on exactly that shape. The

*Suggested fix:* Either drop the parameter and hardcode `ProtoNode` (documenting `Leaf`/`Interior` as fixed), or make it real: add `template<typename T> concept NodeBase = std::constructible_from<T, bool, size_t> && requires(T t){ {t.is_leaf} -> std::convertible_to<bool>; {t.size} -> std::convertible_to<size_t>; };` and constrain `NodeType` with it. Remove the unused `EleAssignTag` parameter from `LeafNode` or use

### DefaultHybridPred's address hashing does not do what its comment claims, and leaf sort state leaks into RangeReport ordering
`include/pab_tree/b_tree.h`:271 · api-usability
**Status: same as above**

```cpp
auto const v = (static_cast<size_t>(T->size) + In.size()) * 2654435761ULL ^
               (reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
               (reinterpret_cast<uintptr_t>(In.end()) >> 3);
return v & 1;
```

Only bit 0 of `v` is used, so the address terms contribute only bit 3 of `begin` and bit 3 of `end`. For the 16-byte-aligned allocations parlay hands out, bit 3 of `begin` is zero and bit 3 of `end` degenerates to a parity of the slice length — so the comment's "XOR in low address bits from begin/end to break correlation across call sites" achieves essentially nothing. I verified the outcome is in fact stable: the same binary run six times produced an identical `Sorted

*Suggested fix:* Drop the address terms from `DefaultHybridPred` — keep `return ((static_cast<size_t>(T->size) + batch_sz) * 2654435761ULL) >> 63;` (use a high bit, not bit 0, of the multiplicative hash) — which removes the `reinterpret_cast` and makes the behaviour obviously deterministic. Change the predicate contract to `bool(Node const* T, size_t batch_size)` to match the documentation and to remove the per-ca

### DefaultHybridPred hashes heap addresses, making the default insert/delete path ASLR-nondeterministic
`include/pab_tree/b_tree.h`:279 · testing
**Status: insert/delete leaf ordering is ASLR-dependent**

```cpp
struct DefaultHybridPred {
  template<typename InputNode, typename Range>
  bool operator()(InputNode const* T, Range In) const noexcept {
    auto const v = (static_cast<size_t>(T->size) + In.size()) * 2654435761ULL ^
                   (reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
                   (reinterpret_cast<uintptr_t>(In.end()) >> 3);
    return v & 1;
  }
};
```

The parameterless overloads use it: `BatchInsert(Range&& In)` (b_batch_insert.hpp:27-29) hardcodes `kHybrid` + `DefaultHybridPred{}`, and `BatchDelete` does the same. So *every* default-path update decides per leaf whether to sort based on heap addresses, which vary run to run under ASLR. Consequently `Leaf::i

*Suggested fix:* Remove the address terms from `DefaultHybridPred` so the decision is a pure function of `(T->size, In.size())`:

```cpp
return ((static_cast<size_t>(T->size) + In.size()) * 2654435761ULL) & 1;
```

(The address XOR was there to decorrelate call sites; a second multiply-shift round on the size sum achieves that without nondeterminism.) Then add a regression test that runs the same batch sequence tw

### DEBUG is an unprefixed configuration macro that changes template bodies — ODR hazard and a stdout-printing surprise
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:111 · standards
**Status: PABTREE_DEBUG rename planned, not done**

The library reads exactly one user-settable macro, `DEBUG`, at five sites (b_batch_delete.hpp:111,174; b_batch_insert.hpp:85,143; node_op.hpp:140,464,609), and all of them sit inside the bodies of `BTree<TT>` member templates. Two consequences.

**ODR.** Because these are templates in a header-only library, TU A compiled with `-DDEBUG` and TU B without it emit *different definitions of the same entity*. The linker keeps one arbitrarily. Nothing warns. CMakeLists.txt:97 makes this easy to hit: `if(DEBUG) add_compile_options(-g -march=native -DDEBUG)` applies to the whole project, so a mixed build (this library in a DEBUG consumer linked against a Release-built one) silently gets whichever bod

*Suggested fix:* Rename the macro to a namespaced one — `PBTREE_DEBUG_CHECKS` — at all seven sites and in CMakeLists.txt:97 (`add_compile_options(-DPBTREE_DEBUG_CHECKS)`). Remove the `puts(">>> pass tree aug after delete")` entirely, or route it through a caller-supplied logger. Document `PBTREE_DEBUG_CHECKS` in README.md, stating explicitly that all TUs linked together must agree on it. Note that `SIEVE`, `MEMCHE

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

### No comparator customization point: sorts use E::operator< while navigation uses Entry::GetKey, with no check they agree
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:72 · extensibility
**Status: deferred (4.7) - a feature, not debt**

The tree's structural invariants are all expressed in terms of `Entry::GetKey` — `MultiWayNode::GetNodeIdContainingKey` (tree_node.h:352), `LeafNode::BinarySearchKey` (106), `RangeCountLeaf`, `RangeReportLeaf`, `GetBox`. But every sort in the library uses the *element's* default `operator<` with no comparator:

```
b_batch_insert.hpp:72   sorted_seq = parlay::sort(In);
b_batch_delete.hpp:74   sorted_seq = parlay::sort(In);
b_build_tree.hpp:36     auto sort_seq = parlay::sort(In);
leaf_op.hpp:31,37,56    std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter());
leaf_op.hpp:117         entry_seq = parlay::sort(entry_seq);
node_op.hpp:551         std::sort(entry_buf.begin(), ...);
```

So an exten

*Suggested fix:* Sort by key explicitly everywhere: `parlay::sort(In, [](E const& a, E const& b){ return Entry::GetKey(a) < Entry::GetKey(b); })` and the equivalent for the four `std::sort` calls, which removes the dependency on `E::operator<` entirely. Alternatively add a `using KeyLess = KeyLessType;` to `BTreeTypeTrait` (defaulting to `std::less<K>`) and thread it through all nine sort sites plus `BinarySearchK

### AssembleInteriorLevels silently overwrites the caller's nodes/keys buffers
`include/pab_tree/b_tree_impl/batch_op/b_build_tree.hpp`:43 · extensibility
**Status: open**

`AssembleInteriorLevels(TT::NodeSlice nodes, TT::SplitSlice keys)` allocates one auxiliary buffer pair and then ping-pongs:
```cpp
auto swap_key_node = [&](SizeT bkt_n) {
  key_in  = key_in.cut(0, bkt_n - 1);   key_out = key_out.cut(0, bkt_n - 1);
  node_in = node_in.cut(0, bkt_n);      node_out = node_out.cut(0, bkt_n);
  std::ranges::swap(key_in, key_out);   std::ranges::swap(node_in, node_out);
};
```
After the first swap, `node_out`/`key_out` alias the *caller's* `nodes`/`keys`, so from the second iteration onward the function writes over its own input. Both current callers happen to pass throwaway scratch (`node_seq`/`key_seq` in `BuildRecursive`, `parlay::tabulate` results in `BatchIns

*Suggested fix:* Either document and rename to make ownership explicit — `Node* AssembleInteriorLevelsInPlace(TT::NodeSlice scratch_nodes, TT::SplitSlice scratch_keys)` with a comment stating both slices are clobbered — or take ownership by value: `Node* AssembleInteriorLevels(TT::NodeSeq nodes, TT::SplitSeq keys)` and have the two callers `std::move` their sequences in. The second is cheaper to reason about and c

### The branchless binary search is hand-inlined four times with a hardcoded base of 8
`include/pab_tree/b_tree_impl/query_op/range_report.hpp`:98 · standards
**Status: open**

`BinarySearch<_binary_search_base>` (binary_search.h:41-76) exists precisely as the shared primitive, and the node types expose a tuned `kBinarySearchBase` derived from `std::hardware_constructive_interference_size` (tree_node.h:57-64 and :276-283). Yet the right-boundary search is copy-pasted inline four times with a literal cutoff: `while (hi - lo > 8)` at range_report.hpp:98 (interior splits) and :46 (leaf entries), and range_count.hpp:79 and :34. The bodies are identical modulo the accessor. The hardcoded 8 also throws away the cache-line tuning the rest of the library goes to some trouble to compute (for `kBuf=64` with 8-byte entries the leaf base is 8, but for 16-byte entries it is 4).

*Suggested fix:* Replace all four inlined loops with the primitive, adding an offset-taking overload so the narrowing is preserved: `template<size_t base, typename Seq, typename F> size_t BinarySearchFrom(Seq const& I, size_t lo, F const& less)` in `binary_search.h`, then call `TI->GetSplitSlice()`-based `BinarySearchFrom<Interior::kBinarySearchBase>(split_slice, l, [&](K key){ return key < query_box.second; })` i

### No exception guarantee: a throwing allocation mid-insert leaves the tree double-freeable
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:111 · standards
**Status: reduced — the RAII half is done, exception safety is not**

The tree itself is now RAII: copy/move constructor, copy/move assignment
and destructor all exist, so a `btree` owns and releases its nodes. What
remains is the guarantee *during* an operation, described below.

Every node is a raw `Node*` from `parlay::type_allocator<...>::alloc()` + placement new, released only by explicit `FreeNode<T>` calls; there is no owning handle anywhere. The insert path frees before it has a replacement:
```cpp
if (TL->is_sorted) {
  std::ranges::merge(..., entry_seq.begin(), ...);
  FreeNode<Leaf>(T);                       // line 111 — parent still points at T
} else {
  ...
  FreeNode<Leaf>(T);                       // line 116
  entry_seq = parlay::sort(entry_seq);     // may throw std::bad_alloc
}
SizeT rt_bkt_n = GenerateLeafNodes(...);   // allocates; may throw
```
If `parlay::sort` or `GenerateLeafNodes` throws, `T` has already been retired while the parent's `tree

*Suggested fix:* Two concrete steps. (1) Reorder so no node is freed before its replacement exists: in `HandlingLeafSplitOverflow` move `FreeNode<Leaf>(T)` to after the `GenerateLeafNodes` call, and likewise move `FreeNode<Interior>(T)` in `HandlingInteriorSplitOverflow` to after `GenerateInteriorNodes` returns. (2) State the guarantee in the class comment on `BTree` in `b_tree.h` — e.g. "no-throw element and augm

### An interior augmentation cannot read its children's augmentations
`include/pab_tree/dependence/node_base.h`:41 · api-usability
**Status: open — found 2026-08-17 while writing self_test**

`interior_aug::update(nodes, splits)` receives the children as `Node *`, and
`proto_node` exposes only `is_leaf` and `size`. Reading a child's own
augmentation needs the concrete `Leaf`/`Interior` types, which are members of
`btree<TT>` and so are not nameable where the augmentation is defined — the
trait takes the augmentation as a parameter, so the dependency is circular.

Consequence: an interior augmentation can aggregate anything reachable from
`proto_node` (subtree entry counts, child counts, the split keys), but the
usual case — sum/min/max of the children's augmentations — is not expressible.
`tests/self_test.cpp` uses `subtree_size_aug`, which sums `n->size`, precisely
because that is what the signature allows.

This is the residue of the review's "InteriorAug cannot be implemented at all".
The refactor fixed the missing `update_aug` calls and passes real arguments, so
an interior augmentation now works and stays current; what remains is that it
cannot see below one level.

*Suggested fix:* pass the children as a slice of a CRTP or tag type that
exposes `get_aug()`, or template `update` on the child type so the aug can be
written generically:
```cpp
template <typename Child, typename Splits>
void update(parlay::slice<Child *const *, Child *const *> nodes, Splits const &);
```

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

## Outside the library — 13

Excluded from the review's scope; listed so nothing is lost. 10 were closed
by the refactor and the test work and have been removed; each was re-checked
against the current tree first.

- **[high]** `.gitmodules` — `fast-cpp-csv-parser` submodule uses a `git@github.com:` SSH URL, breaking the README's own clone step
- **[high]** `CMakeLists.txt` — Default configure fails without jemalloc: `option(JEMA "" ON)` + `pkg_check_modules(JEMALLOC REQUIRED)`
- **[high]** `CMakeLists.txt` — CMakeLists mixes the library with the benchmark harness: 12 research drivers always built, no BUILD_TESTING/BUILD_BENCHMARKS
- **[high]** `CMakeLists.txt` — No CI, no sanitizer or coverage configuration, and the strict .clang-tidy is never executed
- **[high]** `tests/test_framework.h` — b_test's query path has its correctness comparison commented out, so the driver that actually runs validates nothing
- **[medium]** `tests/unit_test.cpp` — unit_test still needs `/ssd1/zmen002/kv_pair` and silently skips without it; `self_test` now supplies the data-free coverage, so a green `ctest` means something again
- **[medium]** `CMakeLists.txt` — `-march=native` and forced CMAKE_BUILD_TYPE make the build non-portable and un-overridable
- **[medium]** `CMakeLists.txt` — Configure step executes a python script that writes `.clangd` into the source tree from a hardcoded `build/` path
- **[medium]** `CMakeLists.txt` — No release engineering: unused `VERSION 0.1`, no semver tag, no CHANGELOG, no version header
- **[medium]** `scripts/gen_graph.sh` — Benchmark scripts and drivers hardcode the author's data volumes; no configurable data root
- **[medium]** `tests/test_framework.h` — No sanitizer, race-detector or fuzz configuration; the one memory-check build (-DMEMCHECK=ON) does not compile
- **[low]** `CMakeLists.txt` — Dead `CMAKE_MODULE_PATH` entry pointing at a nonexistent `cmake/` directory
- **[low]** `tests/unit_test.cpp` — unit_test's only failure vehicle is EXPECT_NO_THROW, which cannot observe the failures the library produces