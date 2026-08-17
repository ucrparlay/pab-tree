# Findings register — full list

Every finding from the 2026-08-17 review, including the medium and low ones
the report did not detail. 112 total: 89 in the library, 23 elsewhere.

Paths are as reviewed (`include/pb_tree/`); the library is now
`include/pab_tree/` and identifiers have been restyled, so line numbers are
pre-refactor. `VERIFIED` means an adversarial verifier could not refute it.

Status is my assessment of what the refactor did, not a separate audit.

## CRITICAL — 13

### `BTree t(seq);` does not compile — BTree(Range) and BTree(Range const&) are ambiguous
`include/pab_tree/b_tree.h`:77 · api-usability
**Status: fixed**

Two unconstrained template constructors are declared back to back:

```cpp
  template<typename Range>
  BTree(Range in)          // line 78
  {
    this->Build(in);
  }

  template<typename Range>
  BTree(Range const& in)   // line 84
  {
    this->Build(in);
  }
```

For any lvalue argument both deduce `Range` to the same type and are exact matches, so overload resolution is ambiguous. Verified by compiling `Tree t(s);` with `s` a `parlay::sequence<E>`:

```
error: call of overloaded 'BTree(parlay::sequence<...>&)' is ambiguous
note: there are 5 candidates
note: candidate 1: BTree(const Range&)   b_tree.h:84
note: candidate 2: BTree(Range)         b_tree.h:78
note: candidate 3: BTree(BTree&

*Fix:* Delete the `BTree(Range in)` overload at b_tree.h:78 and constrain the survivor so it cannot hijack copy/move:

```cpp
template<typename Range>
  requires parlay::is_random_access_range_v<std::remove_cvref_t<Range>> &&
           (!std::same_as<std::remove_cvref_t<Range>, BTree>)
explicit BTree(Range&& in) { this->Build(std::forward<Range>(in)); }
```

Add a compile-only test (`Tree t(seq);`) to t

### BTree(Range) constructor is ambiguous for every argument category — the range constructor is unusable
`include/pab_tree/b_tree.h`:77 · api-usability
**Status: fixed**

Two unconstrained, non-`explicit` single-argument constructor templates are declared back to back:

```cpp
  template<typename Range>
  BTree(Range in) { this->Build(in); }

  template<typename Range>
  BTree(Range const& in) { this->Build(in); }
```

For any argument these two deduce to the same parameter type modulo by-value vs by-const-ref, which overload resolution cannot rank. Verified by compiling all three cases against `parlay::sequence<W::E>`:

- `parlay::sequence<W::E> s(10); Tree a(s);` → `call of overloaded 'BTree(parlay::sequence<...>&)' is ambiguous`
- `const parlay::sequence<W::E> s(10); Tree a(s);` → ambiguous
- `Tree a(parlay::sequence<W::E>(10));` → ambiguous

So the constr

*Fix:* Delete the by-value overload and constrain and mark the survivor: `template<typename Range> requires parlay::is_random_access_range_v<std::remove_cvref_t<Range>> explicit BTree(Range&& in) { this->Build(std::forward<Range>(in)); }`. The `requires` clause also stops it from hijacking copy construction. Add a test that constructs from an lvalue sequence, a const lvalue and an rvalue.

### BatchDelete with sorted_input=true silently deletes nothing
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:80 · correctness · VERIFIED
**Status: fixed**

`BatchDelete_` builds `sorted_slice` for the already-sorted case but then hands `sorted_seq` — which is left empty when `sorted_input == true` — to the recursion:

```cpp
parlay::slice<const K*, const K*> sorted_slice{ In.begin(), In.end() };
typename TT::KeySeq sorted_seq;
if (!sorted_input) {
  sorted_seq = parlay::sort(In);
  sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
}

auto [invalid_flag, invalid_subtree_num, reinsert_entry_num] =
  BatchDeleteRecursive(this->root_,
                       parlay::make_slice(sorted_seq),   // <-- not sorted_slice
```

`BatchDeleteRecursive` sees `In.size() == 0` and returns immediately. The sibling `BatchInsert_` (`b_batch

> Verifier narrowed: Real bug, but two qualifications on the claim as written.

(a) Trigger is narrower than "the documented fast path": nothing in the repo ever reaches it. Every in-repo call site passes false — `BatchDelete(Range&&)` hardcodes `constexpr bool sorted_input = false` (b_batch_delete.hpp:25), and `tests/test_framework.h:416` and `:570` both declare `cons

*Fix:* Change the call to `BatchDeleteRecursive(this->root_, sorted_slice, leaf_entries_pattern, leaf_sort_pred)`, and add a regression test in `tests/unit_test.cpp` that runs each of `BatchInsert`/`BatchDelete` with `sorted_input` both true and false and compares `GetSize()`/`GetEntries()`.

### BatchDelete_ ignores sorted_input=true and silently deletes nothing
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:78 · correctness · VERIFIED
**Status: fixed**

The function builds a `sorted_slice` that is correct in both branches, then never uses it:

```cpp
parlay::slice<const K*, const K*> sorted_slice{ In.begin(), In.end() };
typename TT::KeySeq sorted_seq;
if (!sorted_input) {
  sorted_seq = parlay::sort(In);
  sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
}

auto [invalid_flag, invalid_subtree_num, reinsert_entry_num] =
  BatchDeleteRecursive(this->root_,
                       parlay::make_slice(sorted_seq),   // <-- not sorted_slice
                       ...);
```

When `sorted_input == true` the `if` body never runs, so `sorted_seq` is a default-constructed empty `KeySeq`. `BatchDeleteRecursive` receives a size

> Verifier narrowed: The defect is real and the diagnosis is exact, but two qualifications:

(a) Severity is "latent critical", not "live critical". It is unreachable through both no-flag overloads — `BatchInsert/BatchDelete(Range&&)` hardcode `constexpr bool sorted_input = false` (b_batch_delete.hpp:26) — and no in-repo caller passes true (tests/test_framework.h:416 a

*Fix:* Pass the slice that was computed: change line 80 to `sorted_slice`. Because `sorted_slice` is `parlay::slice<const K*, const K*>` while `BatchDeleteRecursive` takes `TT::KeySlice` (= `slice<K*,K*>`), retype `BatchDeleteRecursive`'s parameter to `TT::ConstKeySlice` (already defined in type_trait.h:62) and propagate that through `DeletePoints4Leaf`, `SplitBatchBinarySearch` and `SplitBatchMerge`, wh

### BatchDelete destroys the whole tree when every root child is underfull but non-empty
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:85 · correctness · VERIFIED
**Status: fixed**

```cpp
if (invalid_flag) { // NOTE: root is invalid
  if (this->root_->is_leaf) { return; }
  assert(this->root_->size == 0);
  constexpr bool kEnableGranularity = false;
  this->DeleteTreeRec<kEnableGranularity>(this->root_);
  this->root_ = nullptr;
  return;
}
```

The code assumes `invalid_flag` on an interior root means the tree is empty. It does not. `BatchDeleteUpdateInteriorSize` (node_op.hpp:252-254) sets, for the root, `TI->valid = (GetNodeN() - invalid_tree_n) > 0`, so the root is "invalid" exactly when *every* child is invalid — and a leaf child is invalid at `size < kBufLB`, which includes any size in `[1, kBufLB)`. There is no relation to `size == 0`.

Concrete repro (kBufferSi

> Verifier narrowed: The claim is accurate as written. One refinement to the trigger description: because an interior child is itself invalid when it has fewer than kBufLB valid children (node_op.hpp:253), the "all root children invalid" condition is reachable at any tree height, not only at fan-out 2 — the 2-leaf case is just the smallest witness. Fix: replace the `in

*Fix:* Test the actual entry count, not the validity flag. Replace the block with:
```cpp
if (invalid_flag && !this->root_->is_leaf) {
  if (this->root_->size == 0) {
    this->DeleteTreeRec<false>(this->root_);
    this->root_ = nullptr;
    return;
  }
  // root underfull but non-empty: flatten and rebuild
  auto tree_entries = TT::EntrySeq::uninitialized(this->root_->size);
  FlattenRec(this->root_, p

### Build() on a non-empty tree leaks the entire existing tree
`include/pab_tree/b_tree_impl/batch_op/b_build_tree.hpp`:34 · correctness · VERIFIED
**Status: fixed**

`Build_` overwrites `root_` with no attempt to release what was there:

```cpp
template<typename TT>
template<typename Range>
void
BTree<TT>::Build_(Range In)
{
  auto sort_seq = parlay::sort(In);
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));   // old tree orphaned
  assert(this->root_ != nullptr);
}
```

Verified at runtime — six consecutive `Build(s)` calls on a 2M-entry sequence with no `DeleteTree()` between them:

```
build #0  size=2000000  rss=236260 KB
build #1  size=2000000  rss=272344 KB
...
build #5  size=2000000  rss=407416 KB
There are un-freed blocks obtained from block_allocator.
```

~34 MB leaked per rebuild, and parlay's own allocator reports the leak at exi

> Verifier narrowed: Directionally correct but severity is overstated. This is a deterministic resource leak in a public API entry point, not a critical defect: no memory corruption, no crash, no incorrect query results, and no in-repo caller triggers it (every Build site in tests/test_framework.h and the constructors at b_tree.h:78,84 either starts from root_==nullptr

*Fix:* Add the guard at the top of `Build_`:

```cpp
BTree<TT>::Build_(Range In)
{
  this->DeleteTree();          // idempotent; no-op when root_ == nullptr
  auto sort_seq = parlay::sort(In);
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
}
```

Also remove the redundant `this->root_ = ...` assignment inside `BuildRecursive` (b_build_tree.hpp:103-105) — a `Recursive` helper writing the tr

### Build() leaks the entire existing tree; measured 3.3 MB lost per rebuild
`include/pab_tree/b_tree_impl/batch_op/b_build_tree.hpp`:34 · correctness · VERIFIED
**Status: fixed**

`Build_` overwrites `root_` without releasing the tree that was there:
```cpp
template<typename TT> template<typename Range>
void BTree<TT>::Build_(Range In)
{
  auto sort_seq = parlay::sort(In);
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));   // old root_ dropped
  assert(this->root_ != nullptr);
}
```
Every node of the previous tree is orphaned. `parlay::type_allocator` never reclaims them, so they are gone for the lifetime of the process. Note `BatchInsert_` (b_batch_insert.hpp:64-67) *does* call `this->DeleteTree()` before its `Build_`, which shows the omission here is an oversight, not a design choice.

Measured: 50 successive `Build(s)` calls on a 200 000-entry sequence

> Verifier narrowed: Accurate as to mechanism and location; two refinements. (a) Trigger is narrower than "Build() leaks the entire existing tree" reads: it leaks only when `Build` is invoked on an instance whose `root_` is already non-null. Default/converting construction (`root_ = nullptr`, b_tree.h:632) and the `Build_` calls reached through `BatchInsert_` (b_batch_

*Fix:* Make `Build_` idempotent by releasing first:
```cpp
template<typename TT> template<typename Range>
void BTree<TT>::Build_(Range In)
{
  this->DeleteTree();            // no-op when root_ == nullptr
  auto sort_seq = parlay::sort(In);
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
  assert(this->root_ != nullptr);
}
```
Then drop the now-redundant `this->DeleteTree()` at b_batch_inse

### Every query entry point dereferences a null root_: empty tree = SIGSEGV
`include/pab_tree/b_tree_impl/query_op/membership.hpp`:14 · correctness · VERIFIED
**Status: fixed**

`Contain` does `return { ContainIter(this->root_, k), logger };` and `ContainIter` starts with `while (!T->is_leaf)` — no null check. The same holds for `RangeCount` (`range_count.hpp:17` `RangeCountRec(this->root_, ...)`), `RangeReport` (`range_report.hpp:25-26`, whose only guard is the inverted-box test), `GetEntries` (`utility.hpp:24`, `this->root_->size`) and `Validate` (`validation.hpp:171-180`, `T = this->root_` then `VerifyTreeProperty(T)`). `root_ == nullptr` is a perfectly ordinary state: the default constructor leaves it null (`b_tree.h:632`), `DeleteTree()` sets it null, and `BatchDelete_` sets it null when the batch empties the tree (`b_batch_delete.hpp:92`). Note the write paths

> Verifier narrowed: Accurate as stated, with one narrowing of the second trigger: `BatchDelete_` only nulls root_ when the root is an interior node — `b_batch_delete.hpp:86-88` returns early (`if (this->root_->is_leaf) return;`) leaving a non-null empty leaf, so a tree small enough to have a leaf root survives "delete everything" without crashing. The default-construc

*Fix:* Add an early return at each public entry point: in `Contain` return `{false, MembershipLogger{}}` when `root_ == nullptr`; in `RangeCount`/`RangeReport` return `{0, RangeReportLogger{}}`; in `GetEntries` return without touching `Out`; in `Validate` return `true` for a null root. Prefer a single private helper `bool Empty() const noexcept { return root_ == nullptr || root_->size == 0; }` used by al

### Contain / RangeCount / RangeReport segfault on an empty tree (null root_)
`include/pab_tree/b_tree_impl/query_op/membership.hpp`:14 · correctness · VERIFIED
**Status: fixed**

`Contain` passes `this->root_` straight to `ContainIter`, which dereferences it on the first line: `while (!T->is_leaf)`. There is no null check anywhere on the path.

```cpp
// membership.hpp:11-15
BTree<TT>::Contain(K const& k) const -> std::pair<bool, MembershipLogger>
{
  MembershipLogger logger;
  return { ContainIter(this->root_, k), logger };
}
```

`RangeCount` (range_count.hpp:17, `RangeCountRec(this->root_, ...)`) and `RangeReport` (range_report.hpp:26, `RangeReportIter(this->root_, ...)`) have the identical shape. `GetSize()` (b_tree.h:614) *does* guard with `this->root_ ? ... : 0`, so the inconsistency is within one class.

I reproduced this twice on a stock build (`g++ -std=c++2

*Fix:* Add an early-out on `root_ == nullptr` at the three public entry points. In `membership.hpp`: `if (this->root_ == nullptr) [[unlikely]] return { false, MembershipLogger{} };` before the `ContainIter` call. In `range_count.hpp::RangeCount` and `range_report.hpp::RangeReport`: `if (this->root_ == nullptr) [[unlikely]] return { 0, RangeReportLogger{} };` (RangeReport already has exactly this shape fo

### Copy constructor and copy assignment do not compile — CopyTreeRecursive has two hard errors
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:681 · correctness · VERIFIED
**Status: fixed**

`BTree` declares a full rule of five, with the copy half routed through `CopyTreeRecursive`:

```cpp
  BTree(const BTree& other)                                    // b_tree.h:91
    : root_(other.root_ ? this->CopyTreeRecursive(other.root_) : nullptr) {}
```

But the function body never compiles:

```cpp
template<typename TT>
TT::Node*
BTree<TT>::CopyTreeRecursive(Node* T)
{
  if (T->is_leaf) {
    auto const TL = static_cast<Leaf const*>(T);
    return AllocNormalLeafNode<TT::EntrySlice, Leaf>(TL->GetEle());   // line 685
  }
  auto const TI = static_cast<Interior const*>(T);
  auto new_nodes = parlay::tabulate(TI->GetNodeN(), [&](size_t i) {
    return CopyTreeRecursive(TI->tree_nodes[i])

> Verifier narrowed: The claim is correct at both cited lines but understates the fix. Three defects, not two, and neither suggested one-line repair is sufficient:
1. `node_op.hpp:685` — needs `typename` AND the wrong alias: `GetEle()` returns `EntrySeq`, so the call must be `AllocNormalLeafNode<typename TT::EntrySeq, Leaf>(TL->GetEle())`, not `<typename TT::EntrySlice

*Fix:* Fix both lines: `AllocNormalLeafNode<typename TT::EntrySlice, Leaf>(TL->GetEle())` and `AllocInteriorNode<Interior, TT>(parlay::make_slice(new_nodes), TI->GetSplitSlice())`. `CopyTreeRecursive` should also be `static` and take `Node const* T` (it does not touch `this`). Then add `Tree u(t); ASSERT_EQ(u.GetSize(), t.GetSize());` to unit_test.cpp so the copy path is instantiated. If deep copy is not

### Duplicate keys silently make elements unreachable by point and range queries
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:31 · correctness · VERIFIED
**Status: fixed**

`GenerateLeafNodes` picks each separator as the key of the *first element of the next bucket*:

```cpp
output_handler.set_key(i, Entry::GetKey(In[GetBktEndIdx(i, bkt_n, item_n)]));
```

Bucket `i` spans `[st, end)` and gets split key `In[end].key`. If `In[end-1].key == In[end].key`, child `i` holds an element whose key equals `split[i]`, which violates the invariant `GetNodeIdContainingKey` relies on (`tree_node.h:352-357` returns the first `i` with `split[i] > k`). Those copies are then never visited. Nothing in the library documents that keys must be distinct, and there is no `assert`, `static_assert` or dedup step on the `Build`/`BatchInsert` path. Reproduced with a stock instantiation (`

> Verifier narrowed: Directionally and quantitatively correct; one narrowing. On the Build / BatchInsert-only path, point membership is NOT broken: `Contain` returns only a bool, and since `split[i]` is by construction the key of the first element of child i+1, the reachable child always still holds a copy of that key (measured: 0 `Contain` misses over 4096 entries wit

*Fix:* Either (a) enforce distinctness: document `BTree` as a *set*, and add a debug-mode check in `Build_`/`BatchInsert_` (`assert(parlay::is_sorted_unique(...))` after the sort) plus an explicit sentence in README; or (b) support duplicates: change `GenerateLeafNodes` to advance the bucket boundary past a run of equal keys before emitting the separator — e.g. compute `end' = first index >= end with In[

### Duplicate keys silently corrupt the tree; Build/BatchInsert lose entries with no error
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:31 · correctness · VERIFIED
**Status: fixed**

`GenerateLeafNodes` picks the separator for bucket i as the *first* key of the next bucket:
```cpp
output_handler.set_key(i, Entry::GetKey(In[GetBktEndIdx(i, bkt_n, item_n)]));
...
SizeT ele_r = (i == bkt_n - 1) ? item_n : GetBktEndIdx(i, bkt_n, item_n);
```
The tree invariant that every descent relies on is strict: `GetNodeIdContainingKey(k)` returns the first `i` with `split[i] > k` (tree_node.h:352), and `VerifyTreeProperty` asserts `box.second < TI->split[i]` (validation.hpp:50). When equal keys straddle a bucket boundary, the left bucket's max key *equals* `split[i]`, so every copy of that key living left of the boundary becomes unreachable to any descent. Nothing sorts duplicates into

> Verifier narrowed: Accurate as written, with three small refinements. (1) The debug-build claim is conditional: `Build`'s own asserts do NOT fire — `assert(parlay::is_sorted(key_out, a<b))` at `b_build_tree.hpp:100` passes for equal split keys since `is_sorted` with a strict `<` tolerates ties. The abort only happens if the user explicitly calls `Validate()`, whose `

*Fix:* Two things, both needed. (1) State and enforce the precondition: document on `Build`/`BatchInsert` in `b_tree.h` that keys must be distinct, and add a debug-only check in `Build_`/`BatchInsert_` — `assert(parlay::is_sorted(s, [](auto a, auto b){ return Entry::GetKey(a) < Entry::GetKey(b); }))` with a *strict* comparator, which detects duplicates on the already-sorted sequence in O(n). (2) Better,

### BTree's copy constructor and copy assignment do not compile — CopyTreeRecursive has two hard errors
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:685 · correctness · VERIFIED
**Status: fixed**

`BTree` declares a full rule-of-five (b_tree.h:91-134) including a copy ctor and copy assignment that both call `CopyTreeRecursive`. That function is never instantiated by any test, and it contains two errors:

```cpp
  if (T->is_leaf) {
    auto const TL = static_cast<Leaf const*>(T);
    return AllocNormalLeafNode<TT::EntrySlice, Leaf>(TL->GetEle());   // line 685
  }
  ...
  return AllocInteriorNode(parlay::make_slice(new_nodes), TI->GetSplitSlice()); // line 691
```

Line 685: `TT::EntrySlice` appears in a *template-argument list*. C++20's P0634 relaxation of `typename` does not extend to template argument lists, so this is ill-formed; GCC 15 reports `dependent-name 'TT::EntrySlice' is p

> Verifier narrowed: Directionally correct and the two quoted GCC errors are exact, but two refinements. (a) Severity: this is a hard compile error on an advertised public API (rule of five) that no test covers — a real usability/standard defect — but it cannot corrupt data or crash anything; nothing that compiles today is affected. "Major/high" fits better than "criti

*Fix:* Fix both lines: `return AllocNormalLeafNode<typename TT::EntrySeq, Leaf>(TL->GetEle());` (note `GetEle()` returns an `EntrySeq`, not an `EntrySlice`) and `return AllocInteriorNode<Interior, TT>(parlay::make_slice(new_nodes), TI->GetSplitSlice());`. Add a unit test that copies a non-trivial tree and compares `GetEntries` output. If copying is not meant to be supported, delete the copy ctor and copy

## HIGH — 31

### 91 public member functions, 0 private ones: the whole recursion machinery is part of the public contract
`include/pab_tree/b_tree.h`:27 · api-usability
**Status: fixed**

The class body has one `public:` at line 27 and one `private:` at line 631 covering only data members. Every one of the 91 declared member functions is public. Of those, the user-facing set is roughly 18 — `Build`, `BatchInsert` (x2), `BatchDelete` (x2), `Contain`, `RangeCount`, `RangeReport`, `GetEntries`, `GetSize`, `DeleteTree`, `Validate`, `SortedLeafRatio`, `ParseInput`, `ParseDeleteInput`, `GetTreeName`, `GetLeafSize`, plus the special members. The remaining ~73 are pure implementation: `BuildRecursive` (267), `AssembleInteriorLevels` (269), `BatchInsertRecursive` (306), `BatchDeleteRecursive` (338), `FetchInvalidSubtreeNodes` (349), `ReInsertTreeAfterDelete` (363), `BalanceOverLeaves`

*Fix:* Insert a `private:` before `Build_` (b_tree.h:264) and keep only the ~18 user-facing entries above it, adding `friend` declarations only where the free-function helpers in `b_tree_impl/` need access. The static node predicates the impl headers share (`NodeIsValid`, `GetNodeInvalidStatus`, `NodeHasLeafChildren`, `GetBktNum`/`GetBktStIdx`/`GetBktEndIdx`) belong in a `namespace pb_tree::detail` in `t

### A `const BTree&` cannot report its size, its contents, or be validated — the observers are non-const
`include/pab_tree/b_tree.h`:607 · api-usability
**Status: fixed**

The accessors that logically read the tree are declared non-const:

```cpp
  Node* GetRoot()                       { return this->root_; }          // 607
  size_t GetSize()                      { return this->root_ ? this->root_->size : 0; }  // 612
  template<typename Range> void GetEntries(Range& Out);                  // 506
  template<bool kCheckSize = true, bool kVerbose = true>
  bool Validate(Node const* T = nullptr);                                // 537
  double SortedLeafRatio(Node const* T = nullptr);                       // 544
  TT::HeightT CheckHeight(Node const* T);                                // 526
  template<typename Leaf, typename Interior> int GetHeight(Node const*);

*Fix:* Mark all of the above `const` — none needs to mutate. `CheckHeight`, `GetHeight`, `GetTreeNodesNum`, `CheckContainNode`, `VerifyUnoverlappedTree` should additionally be `static` (they use `this` for nothing). Add `size_t GetSize() const`, `bool Empty() const { return GetSize() == 0; }`, and `template<typename Range> void GetEntries(Range&& Out) const`. Replace `Node* GetRoot()` with `Node const* G

### InteriorAug cannot be implemented: Update() gets no children and aug is never recomputed after rebalance
`include/pab_tree/b_tree.h`:167 · extensibility
**Status: not addressed**

The only way an interior augmentation is ever refreshed is

```cpp
auto UpdateAug() { return this->aug.Update(); }
```

called from `node_op.hpp:96` and `node_op.hpp:159`. `Update()` takes no arguments and the aug object has no back-pointer to its node, so it cannot see `tree_nodes`, `split`, or the children's augs. Meanwhile every interior allocation constructs the aug by default — `AllocInteriorNode<Interior,TT>(node_in.cut(...), key_in.cut(...))` (`node_op.hpp:63-65`) reaches `BTreeInteriorNode(_tree_nodes_slice, _split_slice)` at `b_tree.h:150-156`, which passes `typename TT::InteriorAug()`. The three-argument constructor that does accept an aug (`b_tree.h:158`) has no caller. Finally, `

*Fix:* Change the interior hook to `void UpdateAug()` on the node forwarding the node's own state: `this->aug.Update(this->GetNodeSlice(), this->GetSplitSlice())`, matching the `Update(TreeNodes const&, SplitSeq const&)` signature `tests/wrapper.h:53` already declares (and nothing calls). Pass a computed aug at every `AllocInteriorNode` site instead of default-constructing, and add `UpdateAug` calls at t

### Augmentation contract is undocumented and contradicted by both the library and the only example
`include/pab_tree/b_tree.h`:172 · docs
**Status: fixed**

`BTreeInteriorNode` declares five aug-forwarding members:

```cpp
void SetParallelFlag(bool const flag)   { this->aug.SetParallelFlag(flag); }
void ResetParallelFlag()                { this->aug.ResetParallelFlag(); }
auto GetParallelFlagIniStatus() -> bool { return this->aug.GetParallelFlagIniStatus(); }
```

None of these three is called anywhere in the library, and the only augmentation in the repo (`tests/wrapper.h:37-65 InteriorAugEmpty`) does not define them — it compiles only because member functions of a class template are instantiated lazily. Conversely `InteriorAugEmpty` defines `Create(TreeNodes const&)`, `Update(TreeNodes const&, SplitSeq const&)` and `Reset()`, none of which the

*Fix:* Define and enforce the contracts in `concepts.h`, e.g. `template<typename A, typename NodeSlice, typename SplitSlice> concept InteriorAugmentation = requires(A a, NodeSlice n, SplitSlice s) { a.Update(n, s); { A::ForceParallel(size_t{}, size_t{}) } -> std::same_as<bool>; };` and `template<typename A, typename Range> concept LeafAugmentation = requires(A a, Range r) { A{}; A{r}; a.UpdateAug(r); };`

### BTree references trait members that do not exist (kPivotNum/kBucketNum) and declares an operation whose definition is not included
`include/pab_tree/b_tree.h`:60 · correctness · VERIFIED
**Status: not addressed**

```cpp
static constexpr BucketType const kPivotNum = TT::kPivotNum;
static constexpr BucketType const kBucketNum = TT::kBucketNum;
```

Both members are commented out in the trait (`type_trait.h:140-141`). This compiles today only because a class template's static-data-member initializers are not instantiated until used; any user or extender who writes `BTree<TT>::kBucketNum` gets a hard error inside the library. Similarly `BatchInsertSieve` and `BatchInsertSieveRecursive` are declared at `b_tree.h:302-316` but `b_tree_impl/batch_op/b_batch_insert_sieve.hpp` is commented out of the include list at `b_tree.h:656`, so calling either is an undefined-reference at link time. `SplitBatchSieve`, `S

> Verifier narrowed: Directionally correct and factually exact, but severity is medium rather than high, and the scope is slightly wider than stated. It is a dead-code/public-surface hygiene defect, not a correctness bug: every failure mode is a hard compile or link diagnostic on first use (never silent misbehaviour), and it only bites an extender who touches the sieve

*Fix:* Either restore the two `static constexpr` lines in `BTreeTypeTrait` and re-enable the sieve include, or delete `kPivotNum`/`kBucketNum` from `b_tree.h:60-61`, delete the `BatchInsertSieve`/`BatchInsertSieveRecursive` declarations, and move `b_batch_insert_sieve.hpp` plus `SplitBatchSieve`/`SeievePoints`/`RetriveTag` out of the shipped headers.

### pb_tree::operator<< for std::pair leaks via ADL and makes a user's own pair printer ambiguous
`include/pab_tree/b_tree.h`:15 · standards
**Status: fixed**

b_tree.h opens by injecting a completely generic `std::pair` streaming operator into `namespace pb_tree`:

```cpp
namespace pb_tree {
template<typename T1, typename T2>
auto operator<<(std::ostream& os, std::pair<T1, T2> const& p) -> std::ostream&
{ os << "(" << p.first << ", " << p.second << ")"; return os; }
```

This is not inert. `Contain` returns `std::pair<bool, MembershipLogger>` and `RangeCount` returns `std::pair<SizeT, RangeReportLogger>` (b_tree.h:548, 554) — `MembershipLogger` and `RangeReportLogger` live in `pb_tree` (loggers.h:8, 14), so `pb_tree` *is* an associated namespace of those pairs and ADL finds this operator. Verified: a TU defining the ordinary user-side generic pair

*Fix:* Delete the operator. If a printable form of the query results is wanted, give the logger types their own `operator<<` overloads in `pb_tree` (`std::ostream& operator<<(std::ostream&, MembershipLogger const&)` in loggers.h), which is ADL-correct and cannot collide with a user's pair printer. If the operator is only used for internal debugging, move it into an internal `pb_tree::detail` namespace an

### b_tree.h does not compile against a single include root, and includes no parlay header despite using parlay in its class body
`include/pab_tree/b_tree.h`:4 · build-packaging
**Status: fixed**

Two related problems.

(a) The library needs **two** include roots. `dependence/box_op.h:4` and `dependence/tree_node.h:20-21` spell their siblings as `#include "dependence/type_trait.h"` / `"dependence/binary_search.h"`, and `b_tree_impl/validation.hpp:9` uses `#include "b_tree.h"` — all of which resolve only if `include/pb_tree/` is itself on the search path, in addition to `include/`. Verified by compiling `#include "pb_tree/<header>"` with only `-Iinclude`:

```
OK   dependence/{loggers,binary_search,concepts,comparator,type_trait,basic_point}.h
FAIL dependence/box_op.h   : dependence/type_trait.h: No such file or directory
FAIL dependence/tree_node.h: dependence/binary_search.h: No such

*Fix:* Rewrite every intra-library include as a path from the single root `include/`: `#include "pb_tree/dependence/type_trait.h"` etc. (there are currently four different spellings of `b_tree.h` alone across the 15 impl headers — `"pb_tree/b_tree.h"`, `"../b_tree.h"`, `"../../b_tree.h"`, `"b_tree.h"`). Then set `PMOS_INCLUDE_DIR` to just `${PMOS_SOURCE_DIR}/include`. Add to b_tree.h the headers it actua

### ~700 dead lines shipped: the entire sieve subsystem plus BasicPoint/AugPoint, with four misspelled public identifiers
`include/pab_tree/b_tree.h`:302 · dead-code
**Status: fixed**

Three separate bodies of unreachable code, ~15% of the library:

**1. Sieve subsystem (~420 lines).** `b_tree_impl/batch_op/b_batch_insert_sieve.hpp` (303 lines) is commented out of b_tree.h's include list (line 656: `// #include "b_tree_impl/batch_op/b_batch_insert_sieve.hpp"`), yet the file ships and the class still declares `BatchInsertSieve` (302) and `BatchInsertSieveRecursive` (312) — declarations with no reachable definition, so calling either is an error at instantiation. Their support functions *are* still compiled into every TU via points_op.hpp but are called only from the excluded file: `RetriveTag` (points_op.hpp:51), `SeievePoints` (68), `SplitBatchSieve` (141) — lines 44-115 a

*Fix:* Delete `b_batch_insert_sieve.hpp`, the `BatchInsertSieve`/`BatchInsertSieveRecursive`/`SplitBatchSieve`/`SeievePoints`/`RetriveTag` declarations from b_tree.h, and points_op.hpp lines 44-115 and 135-178. Delete `SplitLeaf`, `InsertToLeafNoOverflow`, `PseudoRandomGenPattern` and `ContainRec`. Delete `dependence/basic_point.h` and its include in tree_node.h *after* giving b_tree.h and tree_node.h th

### No test for copy/move construction and assignment, GetEntries, SortedLeafRatio, or the kSorted/kUnsorted patterns
`include/pab_tree/b_tree.h`:91 · testing
**Status: fixed**

Exhaustive audit of the public API against every call site under `tests/`:

| API | tested? |
|---|---|
| `Build` | yes (`BuildTree`, test_framework.h:174) |
| `BatchInsert(Range&&)` | yes |
| `BatchDelete(Range&&)` | yes |
| `Contain` | yes (`Membership`, test_framework.h:822) |
| `RangeCount` / `RangeReport` | yes (oracle in `GenRectangles`) |
| `DeleteTree` | yes (indirectly) |
| `GetSize` | yes (unit_test.cpp:128) |
| **`BTree(const BTree&)`** (b_tree.h:91) | **no call site anywhere** |
| **`operator=(const BTree&)`** (b_tree.h:98) | **no call site anywhere** |
| **`BTree(BTree&&)`** (b_tree.h:110) | **no call site anywhere** |
| **`operator=(BTree&&)`** (b_tree.h:117) | **no call site a

*Fix:* Add one test file, `tests/unit_test_api.cpp`, self-contained (no dataset), covering:

```cpp
TEST(BTreeValue, DeepCopyIsIndependent) {
  Tree a(input); Tree b = a;                    // copy ctor
  a.BatchDelete(half_of_input);
  EXPECT_EQ(b.GetSize(), input.size());          // b unaffected
  EXPECT_TRUE(b.Validate());
  EntrySeq ea(b.GetSize()); b.GetEntries(ea);
  EXPECT_EQ(parlay::sort(ea), pa

### BatchDelete_ uses the wrong sequence for sorted input, making sorted-input deletes a silent no-op
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:79 · correctness
**Status: not addressed**

Outside this review's dimension but the exact mirror of the (correct) insert code, so reported here. `BatchDelete_` computes `sorted_slice` and then ignores it:
```cpp
parlay::slice<const K*, const K*> sorted_slice{ In.begin(), In.end() };
typename TT::KeySeq sorted_seq;
if (!sorted_input) {
  sorted_seq = parlay::sort(In);
  sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
}

auto [invalid_flag, invalid_subtree_num, reinsert_entry_num] =
  BatchDeleteRecursive(this->root_,
                       parlay::make_slice(sorted_seq),   // <-- should be sorted_slice
                       ...);
```
Compare `BatchInsert_` at b_batch_insert.hpp:79-83, which correctly passes

*Fix:* Change the argument at b_batch_delete.hpp:79 from `parlay::make_slice(sorted_seq)` to `sorted_slice`, matching `BatchInsert_`. Add a `sorted_input=true` delete case to `tests/unit_test.cpp` — neither the insert nor the delete sorted-input path is currently exercised anywhere.

### BatchDelete with sorted_input=true is a silent no-op, so queries then return deleted keys
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:80 · correctness
**Status: fixed**

`BatchDelete_` builds `sorted_slice` from `In` and only fills `sorted_seq` when `!sorted_input` (lines 71-76), but then passes `parlay::make_slice(sorted_seq)` — not `sorted_slice` — to `BatchDeleteRecursive` (line 80). When the caller says the input is already sorted, `sorted_seq` is an empty sequence, and `BatchDeleteRecursive` returns immediately on `In.size() == 0` (line 140). Contrast `BatchInsert_`, which correctly passes `sorted_slice` (b_batch_insert.hpp:80).

*Fix:* Change line 80 to pass `sorted_slice` (and drop the now-unused `parlay::make_slice(sorted_seq)`), matching `BatchInsert_`. Add a unit test that deletes the same key set twice, once with `sorted_input=false` and once with `true`, and asserts identical `GetSize()`/`GetEntries()`.

### BatchInsert/BatchDelete reject std::vector despite static_asserts promising any random-access range
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:69 · api-usability
**Status: not addressed**

`BatchInsert` opens with `static_assert(parlay::is_random_access_range_v<Range>)` and two more permissive checks, then the body does:
```cpp
parlay::slice<const E*, const E*> sorted_slice{ In.begin(), In.end() };
```
This silently requires the range's iterator to be a raw `const E*`. Verified: `t.BatchInsert(std::vector<E>)` fails with
```
b_batch_insert.hpp:69: error: no matching function for call to
  'parlay::slice<const E*, const E*>::slice(<brace-enclosed initializer list>)'
  note: no known conversion for argument 1 from '__gnu_cxx::__normal_iterator<E*, std::vector<E>>' to 'const E*'
```
while `t.Build(std::vector<E>)` and `t.BatchInsert(parlay::sequence<E>)` both compile fine. `Batch

*Fix:* Materialise instead of reinterpreting. Replace lines 69-74 with:
```cpp
typename TT::EntrySeq sorted_seq;
if (!sorted_input) { sorted_seq = parlay::sort(In); }
else                { sorted_seq = parlay::to_sequence(In); }
auto sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
```
(the `sorted_input` branch costs one O(n) copy, which is what the pointer aliasing was avoidin

### Bogus DEBUG assertion aborts any sorted-input BatchInsert that splits the root
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:86 · correctness
**Status: not addressed**

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

*Fix:* Delete the assertion. If the intent was to check that pre-sorted input really is sorted, replace it with the check that actually says that, placed before the descent:
```cpp
assert(!sorted_input || parlay::is_sorted(sorted_slice,
         [](E const& a, E const& b){ return Entry::GetKey(a) < Entry::GetKey(b); }));
```
and add a sorted-input case to `tests/unit_test.cpp`.

### Every query on an empty or default-constructed tree dereferences a null root and segfaults
`include/pab_tree/b_tree_impl/query_op/membership.hpp`:11 · correctness · VERIFIED
**Status: fixed**

`root_` is `nullptr` after `BTree()` and after `DeleteTree()`, and no query checks it:

```cpp
auto BTree<TT>::Contain(K const& k) const -> std::pair<bool, MembershipLogger>
{
  MembershipLogger logger;
  return { ContainIter(this->root_, k), logger };   // ContainIter does while (!T->is_leaf)
}
```

The same holds for `RangeCount` (range_count.hpp:17, `RangeCountRec(this->root_, ...)`), `RangeReport` (range_report.hpp:25-26) and `GetEntries` (utility.hpp:22-24, which reads `this->root_->size`). Verified:

```cpp
Tree t;
printf("empty GetSize=%zu\n", t.GetSize());   // prints 0 — GetSize *is* guarded
auto r = t.Contain(5u);                        // SIGSEGV (exit 139)
```

`GetSize()` at b_t

> Verifier narrowed: The claim is accurate; two small refinements. (a) `RangeReport` is not unconditionally fatal: `range_report.hpp:19-21` returns early when `query_box.first >= query_box.second`, so a degenerate/empty box happens to survive an empty tree. Any non-degenerate box still faults at `range_report.hpp:83`. (b) The reachability is understated, not overstated

*Fix:* Add an early return to each of the four public entry points, e.g.

```cpp
auto BTree<TT>::Contain(K const& k) const -> std::pair<bool, MembershipLogger>
{
  if (this->root_ == nullptr) [[unlikely]] return { false, MembershipLogger{} };
  ...
}
```

and `if (this->root_ == nullptr) return { 0, RangeReportLogger{} };` in `RangeCount`/`RangeReport`, `if (this->root_ == nullptr) return;` in `GetEntrie

### Contain/RangeCount/RangeReport/GetEntries dereference a null root after a delete empties the tree
`include/pab_tree/b_tree_impl/query_op/membership.hpp`:14 · correctness · VERIFIED
**Status: fixed**

`BatchDelete_` legitimately sets `this->root_ = nullptr` (b_batch_delete.hpp:92), and `DeleteTree()` does the same (b_delete_tree.hpp:19). Every read-side entry point then dereferences it unguarded:

- `membership.hpp:14` — `return { ContainIter(this->root_, k), logger };`, and `ContainIter` starts with `while (!T->is_leaf)`.
- `utility.hpp:23` — `parlay::make_slice(Out.begin(), Out.begin() + this->root_->size)`.
- `range_count.hpp:17` — `RangeCountRec(this->root_, ...)`.
- `range_report.hpp:24` — `RangeReportIter(this->root_, ...)`.

Only `GetSize()` (b_tree.h:614) is null-safe. Reproduced: after the delete described in the previous finding, `t.Contain(220)` segfaults (exit 139).

The state

> Verifier narrowed: The finding is real but the trigger is broader than stated, not narrower: no delete is required at all. `root_` is initialized to `nullptr` at b_tree.h:632, so a default-constructed `BTree<TT>` on which `Contain`/`RangeCount`/`RangeReport`/`GetEntries` is called before any `Build` segfaults identically (verified, exit 139) -- as does calling any of

*Fix:* Pick one representation of empty and enforce it. Either (a) never null `root_` — have `BatchDelete_` install `AllocEmptyLeafNode<EntrySlice, Leaf>()` instead of `nullptr`, matching the leaf-root case and matching what `BuildRecursive` already does for an empty input (b_build_tree.hpp:80-82); or (b) add an early return to each query: `if (this->root_ == nullptr) return {false, MembershipLogger{}};`

### Contain / RangeCount / RangeReport / GetEntries dereference a null root on an empty tree (SIGSEGV)
`include/pab_tree/b_tree_impl/query_op/membership.hpp`:22 · correctness · VERIFIED
**Status: fixed**

`root_` is `nullptr` for a default-constructed tree (b_tree.h:632) and after `DeleteTree()`, but no query entry point checks it:

```cpp
BTree<TT>::Contain(K const& k) const -> std::pair<bool, MembershipLogger>
{ ... return { ContainIter(this->root_, k), logger }; }

bool BTree<TT>::ContainIter(Node const* T, K const& k) const
{ while (!T->is_leaf) { ... } }        // T == nullptr -> deref
```

Same pattern in `RangeCount` (range_count.hpp:17, `RangeCountRec(this->root_, ...)`), `RangeReport` (range_report.hpp:27, only the empty-*box* case is guarded, not the empty-*tree* case), and `GetEntries` (utility.hpp:24, `this->root_->size`). Only `DeleteTree` (b_delete_tree.hpp:15) has a null check.

> Verifier narrowed: Two small precisions to the claim's wording, neither of which changes its substance or severity: (a) `Contain` dispatches to `ContainIter` (membership.hpp:14 → deref at membership.hpp:24), not `ContainRec` — `ContainRec` (membership.hpp:36) has the same defect but is never called from anywhere in include/ or tests/, so it is dead code; (b) `RangeRe

*Fix:* Add early returns to all four entry points, e.g. in `Contain`: `if (this->root_ == nullptr) [[unlikely]] return { false, MembershipLogger{} };`; in `RangeCount`: `if (this->root_ == nullptr) return { 0, RangeReportLogger{} };`; likewise `RangeReport`; and in `GetEntries`: `if (this->root_ == nullptr) return;`. Add a unit test that runs the whole query suite against a default-constructed tree.

### RangeReport has no bounds check and no documented contract for sizing `out`
`include/pab_tree/b_tree_impl/query_op/range_report.hpp`:58 · correctness · VERIFIED
**Status: fixed**

`RangeReport(TT::Box, Range&& out)` writes through `out` with no capacity check anywhere: `std::copy_n(TL->elements.begin() + l, r - l, out.begin() + offset)` (line 58), `out[offset++] = TL->elements[i]` in the unsorted leaf path (line 67), and `out.cut(off[k], off[k+1])` for the parallel middle-child flatten (line 142). `parlay::slice::cut`/`operator[]` do no release-mode checking, and `FlattenRec`'s only guard is `assert(T->size == Out.size())` (flatten.hpp:33), compiled out under NDEBUG. Nothing in the header states the required size, and the only way for a caller to know it is to run `RangeCount` first — a second full traversal.

> Verifier narrowed: Directionally correct, but two details are overstated and severity is better read as medium (API-contract/documentation defect) than high (library-internal memory-safety bug):

(a) It is a violated *precondition*, not a bug the library can hit on its own — all five in-repo call sites (tests/test_framework.h:727, 921, 980, 1289, 1512) pre-size `Out`

*Fix:* Document the precondition on the declaration in `b_tree.h:566-568` (`out.size() >= RangeCount(query_box).first`), and enforce it: keep a running `out.size()` check in `RangeReportIter`/`RangeReportLeaf` and either truncate-and-report or `throw std::out_of_range`. Better, add a convenience overload `auto RangeReport(TT::Box) const -> std::pair<typename TT::EntrySeq, RangeReportLogger>` that sizes t

### RangeReport writes past the end of the caller's output range with no bounds check
`include/pab_tree/b_tree_impl/query_op/range_report.hpp`:57 · correctness · VERIFIED
**Status: fixed**

`RangeReport(box, out)` never consults `out.size()`. The leaf path blindly does `std::copy_n(TL->elements.begin() + l, r - l, out.begin() + offset)` (range_report.hpp:57) and `RangeReportFlattenRec` appends the same way.

I proved the overflow with a canary: a 600-slot `parlay::sequence` filled with `0xDEADBEEF`, handed to `RangeReport` as a slice of only the **first 4** elements, over a query whose true answer is 500:

```
out slice size = 4
RangeReport returned 500
canary slots clobbered BEYOND the 4-slot out slice: 496
```

So it wrote 496 entries (7.9 KB at 16 B/entry) outside the range the caller passed, silently, with exit code 0. Had the sequence actually been 4 elements this is a hea

> Verifier narrowed: Correct but overstated in two respects.

(a) Severity is "unchecked/undocumented precondition", not "library writes out of bounds on its own". This is a caller-fills-buffer API in the shape of `std::copy` into an output iterator: the write is out of range only when the caller violates a precondition. The library does provide the means to satisfy it

*Fix:* Make the contract explicit and safe. Minimum: `assert(out.size() >= <count>)` is not enough since the count is not known up front — instead clamp. Change `RangeReportLeaf`/`RangeReportFlattenRec` to take the output capacity and stop copying at it, and have `RangeReport` return `{written, logger}` where `written <= out.size()`; add a second return field or a `bool truncated`. Alternatively add an a

### PostProcessLeafChange sorts leaves with E::operator<, not the key comparator used everywhere else
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:31 · extensibility
**Status: fixed**

Two of the three branches sort with the element's default `operator<`:

```cpp
if (leaf_entries_pattern == TT::LeafEntriesPattern::kSorted) {
  std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter());
  TL->is_sorted = true;
} else if (... kHybrid) {
  if (leaf_sort_pred(T, In)) {
    std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter());
    TL->is_sorted = true;
```

`ExtractPointsInLeaf` does the same (leaf_op.hpp:56). Every other ordering site in the library goes through `Entry::GetKey`: `HandlingLeafSplitOverflow`'s merge comparator (leaf_op.hpp:108-110), `handle_distribute`'s `std::sort` (node_op.hpp:551-555), `LeafNode`'s construction assert (tree_node.h:90-91), and `BinarySearchKey` (t

*Fix:* Sort by key explicitly and drop the hidden requirement: in both `PostProcessLeafChange` branches and in `ExtractPointsInLeaf`, use `std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter(), [](E const& a, E const& b){ return Entry::GetKey(a) < Entry::GetKey(b); });`. Do the same for the `parlay::sort(In)` calls in `Build_`, `BatchInsert_` and `BatchDelete_`. If keeping `operator<` is preferred for spe

### BTree's copy constructor and copy assignment do not compile (CopyTreeRecursive is broken)
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:685 · correctness · VERIFIED
**Status: fixed**

`CopyTreeRecursive` — the only body reachable from `BTree(const BTree&)` (b_tree.h:91) and `operator=(const BTree&)` (b_tree.h:98) — has two hard errors, so it has clearly never been instantiated:
```cpp
return AllocNormalLeafNode<TT::EntrySlice, Leaf>(TL->GetEle());          // line 685
...
return AllocInteriorNode(parlay::make_slice(new_nodes), TI->GetSplitSlice()); // line 691
```
Compiling `BTree<TT> u(t);` gives:
```
node_op.hpp:685: error: dependent-name 'BTree<TypeTrait>::TT::EntrySlice' is parsed as a non-type, but instantiation yields a type
node_op.hpp:691: error: no matching function for call to
  'AllocInteriorNode(parlay::slice<ProtoNode**, ProtoNode**>, parlay::slice<const unsi

> Verifier narrowed: Accurate as stated; two narrowings worth recording. (a) It is a hard compile error only on instantiation — the current build is unaffected because nothing in `include/` or `tests/` ever copies a `BTree` — so this cannot silently produce wrong results today, only break the moment a user or extender copies a tree (directly, by value-passing, or via a

*Fix:* Fix and cover it. Change node_op.hpp:685 to `AllocNormalLeafNode<typename TT::ConstEntrySlice, Leaf>(TL->GetEleSlice())` (avoiding the `EntrySeq` copy and the sortedness assert by copying `elements[0..size)` directly), and line 691 to `AllocInteriorNode<Interior, TT>(parlay::make_slice(new_nodes), TI->GetSplitSlice())` with a `SplitSliceConst`-taking `AllocInteriorNode` overload added in tree_node

### ReInsertTreeAfterDelete discards a non-zero split result: use-after-free of the reinserted node in release builds
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:669 · correctness · VERIFIED
**Status: not addressed**

```cpp
NodeSplitPairSeq node_split_seq =
  NodeSplitPairSeq::uninitialized(2 * reinsert_entries.size());
SizeT new_node_n = BatchInsertRecursive(T, reinsert_entries, parlay::make_slice(node_split_seq), ...);
assert(new_node_n == 0);
```
A non-zero `new_node_n` means `BatchInsertRecursive` took the `HandlingInteriorSplitOverflow` path, which at node_op.hpp:217 does `FreeNode<Interior>(T)` and writes `new_node_n` replacement children into `node_split_seq`. The contract is that the *caller* must splice those children in. `ReInsertTreeAfterDelete` does neither: `node_split_seq` is a local that dies on return, and under NDEBUG the `assert` vanishes. The caller — `FetchInvalidSubtreeNodes` at node

> Verifier narrowed: Mechanism is exactly as claimed; the trigger description is overstated/mis-scaled. The bug does NOT need an invalid interior child holding ~100 000 entries. What it needs is: (a) T is the topmost node with an invalid direct child (so `reinsert_flag && has_tomb`, node_op.hpp:401), (b) after the compaction loop at node_op.hpp:344-357 T still has clos

*Fix:* Make the contract explicit rather than asserted. Either (a) have `ReInsertTreeAfterDelete` return `SizeT` and the `NodeSplitPairSeq`, and have `FetchInvalidSubtreeNodes` splice via `AssembleInteriorLevels` exactly as `BatchInsert_` does at b_batch_insert.hpp:89-97; or (b) split the reinsert into chunks small enough that `T` provably cannot overflow — but (a) is the correct fix. At minimum promote

### The whole delete/rebalance path never calls UpdateAug, so any non-trivial augmentation goes stale
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:524 · extensibility
**Status: fixed**

The insert path maintains augmentations at every mutation: `BatchInsertUpdateInterior` calls `TI->UpdateAug()` on both its fast path (node_op.hpp:96) and its slow path (node_op.hpp:159), and `InsertPoints2Leaf` calls `TL->UpdateAug(...)` (leaf_op.hpp:84). The delete path calls it nowhere except `DeletePoints4Leaf`:

- `BalanceOverLeaves` / `handle_merge` (node_op.hpp:524-530) sets `dest->size` and hands off to `PostProcessLeafChange`, which only sorts — the merged leaf's `aug` still reflects its pre-merge contents.
- `BalanceOverLeaves` / `handle_distribute` (node_op.hpp:566-569) writes `leaf->size = bkt_size; leaf->is_sorted = true;` with no `leaf->UpdateAug(...)`; every leaf in a redistrib

*Fix:* Add the calls: `dest->UpdateAug(parlay::make_slice(dest->GetEleBeginIter(), dest->GetEleEndIter()))` after the size update in `handle_merge`; `leaf->UpdateAug(leaf->GetEleSlice())` after `leaf->size = bkt_size` in `handle_distribute`; `TI->UpdateAug()` immediately after `TI->node_n = tree_idx` in `BalanceOverLeaves` (node_op.hpp:606), after `TI->node_n = tree_node_idx` in `PruneOutEmptyTree` (node

### ReInsertTreeAfterDelete asserts away a root split; in Release that is a use-after-free plus lost nodes
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:671 · correctness · VERIFIED
**Status: not addressed**

```cpp
NodeSplitPairSeq node_split_seq =
  NodeSplitPairSeq::uninitialized(2 * reinsert_entries.size());
SizeT new_node_n = BatchInsertRecursive(T, reinsert_entries,
                                        parlay::make_slice(node_split_seq),
                                        leaf_entries_pattern, leaf_sort_pred);
assert(new_node_n == 0);
```

`BatchInsertRecursive` returns non-zero when the node it was called on overflows past `kBufUB` children (b_batch_insert.hpp:195-203). In that case it has already called `HandlingInteriorSplitOverflow`, which does `FreeNode<Interior>(T)` (node_op.hpp:217) and writes the replacement nodes into `node_split_slice`. `ReInsertTreeAfterDelete` returns `v

> Verifier narrowed: The code and the failure mode are exactly as described (assert-only handling of a return the callee is designed to produce; FreeNode<Interior>(T) at node_op.hpp:217; UAF at node_op.hpp:414 plus a dangling parent tree_nodes[i], silent under NDEBUG which the default Release build sets). The reachability argument in the report is wrong, however, and t

*Fix:* Make the split a handled outcome, not an assertion. Change the signature to `template<typename LeafSortPred> std::pair<SizeT, NodeSplitPairSeq> ReInsertTreeAfterDelete(...)` (or have it return the new root pointer), and in `FetchInvalidSubtreeNodes` at node_op.hpp:409 replace `T`'s slot in its parent — or, since `FetchInvalidSubtreeNodes` has no parent handle, hoist the reinsertion so the caller t

### BTree copy constructor and copy assignment do not compile
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:691 · correctness
**Status: fixed**

`BTree(const BTree&)` (b_tree.h:91) and `operator=(const BTree&)` (b_tree.h:98) both call `CopyTreeRecursive`, whose body is ill-formed on instantiation: `return AllocInteriorNode(parlay::make_slice(new_nodes), TI->GetSplitSlice());` cannot deduce the explicit template parameters `Interior`/`TT` of `AllocInteriorNode` (tree_node.h:396-413), and line 685 `return AllocNormalLeafNode<TT::EntrySlice, Leaf>(TL->GetEle());` needs `typename TT::EntrySlice`. Verified errors: "dependent-name 'pb_tree::BTree<TypeTrait>::TT::EntrySlice' is parsed as a non-type, but instantiation yields a type" (node_op.hpp:685) and "no matching function for call to 'AllocInteriorNode(parlay::slice<ProtoNode**,ProtoNode

*Fix:* Fix the call to `AllocInteriorNode<Interior, TT>(parlay::make_slice(new_nodes), TI->GetSplitSlice())` and `AllocNormalLeafNode<typename TT::EntrySlice, Leaf>(...)`, and preserve leaf state in the copy: allocate the leaf and then `new_leaf->is_sorted = TL->is_sorted;` (or sort the entries before constructing). Add a unit test that copies a tree built with `kHybrid` and compares `Contain`/`RangeCoun

### Validate() is assert-only, so it always returns true in Release builds
`include/pab_tree/b_tree_impl/validation.hpp`:180 · correctness · VERIFIED
**Status: fixed**

`Validate` calls `VerifyTreeProperty<kCheckSize>(T)` then `CheckHeight(T)` and ends with a hardcoded `return true;`. Every actual check inside is an `assert` — leaf occupancy (`validation.hpp:21`), leaf sortedness (`:24`), interior fanout (`:34`), split ordering (`:38`), the child-box-vs-split checks (`:50-62`), height uniformity (`:123`). CMakeLists.txt:96 sets `CMAKE_BUILD_TYPE Release`, which appends `-DNDEBUG`, so in the configuration this project ships and benchmarks with, `Validate()` traverses the whole tree (allocating a `parlay::sequence` per interior node) and cannot report anything.

> Verifier narrowed: Directionally correct; two narrowings. (a) "cannot report anything" is slightly too strong: the middle-child box-vs-split check at validation.hpp:54-61 prints a `std::cout` diagnostic outside the assert, so it is still visible under NDEBUG — but `Validate` still returns an unconditional `true` (validation.hpp:190), so the verdict is vacuous. (b) Th

*Fix:* Make the checks return `bool` instead of asserting: change `VerifyTreeProperty` to `std::pair<TT::Box,bool>` (or take a `bool& ok`), have `CheckHeight` return `std::optional<HeightT>`, and have `Validate` `return ok;`. Keep the `assert`s as an additional debug-time aid if desired, but the return value must reflect the checks in all build types. Also make `Validate`/`VerifyTreeProperty`/`CheckHeigh

### Validate() never checks subtree sizes, the field RangeCount and RangeReport depend on
`include/pab_tree/b_tree_impl/validation.hpp`:31 · correctness · VERIFIED
**Status: fixed**

`VerifyTreeProperty` checks boxes, splits and occupancy but never that `TI->size == sum(child->size)` — no size check exists in `validation.hpp` at all. Yet the query paths trust `size` completely: `RangeCountRec` adds fully-covered children with `offset += TI->tree_nodes[i]->size;` (range_count.hpp:106) without visiting them, and `RangeReportIter` derives the *disjoint parallel output slices* from it: `off[k+1] = off[k] + nodes[l+1+k]->size;` then `out.cut(off[k], off[k+1])` inside a `parlay::parallel_for` (range_report.hpp:134-144). If one child's `size` is too small, two workers get overlapping slices and race. The only size assertion in the codebase is `assert(T->size == Out.size())` in

> Verifier narrowed: `Validate`/`VerifyTreeProperty` (include/pb_tree/b_tree_impl/validation.hpp:16-67, 169-191) never verify `TI->size == sum(child->size)`, even though the template parameter is named `kCheckSize` (b_tree.h:536) and defaults to true — it only checks occupancy bounds. `RangeCountRec` (range_count.hpp:106) and `RangeReportIter` (range_report.hpp:134) bo

*Fix:* Extend `VerifyTreeProperty` to return the subtree size alongside the box and check `TI->size == accumulate(child sizes)` and `TL->size == TL->GetEle().size()`; add a null-child check and an optional visited-set/`GetTreeNodesNum`-vs-node-count check for cycles. Add an aug check gated on a `kCheckAug` template parameter so augmentation extenders get coverage too. Fix or delete the misleading comment

### Headers need TWO include roots; `-Iinclude` alone fails on the first compile
`include/pab_tree/dependence/box_op.h`:4 · build-packaging
**Status: fixed**

My very first compile attempt, using the obvious single include root, died immediately:

```
$ g++ -std=c++20 -O1 -pthread -Iinclude -Iexternal/parlaylib/include first_use.cpp
In file included from include/pb_tree/b_tree.h:7,
                 from first_use.cpp:6:
include/pb_tree/dependence/box_op.h:4:10: fatal error: dependence/type_trait.h: No such file or directory
    4 | #include "dependence/type_trait.h"
```

The library uses three mutually incompatible include conventions internally (counted across `include/pb_tree/`):
- `"pb_tree/b_tree.h"` (7 uses) — needs `-Iinclude`
- `"dependence/type_trait.h"`, `"comparator.h"`, `"concepts.h"`, `"basic_point.h"` (10 uses) — needs `-Iinclude/pb_t

*Fix:* Normalise every internal include to be rooted at `include/`, i.e. `#include "pb_tree/dependence/type_trait.h"`, `#include "pb_tree/dependence/comparator.h"`, `#include "pb_tree/b_tree.h"` — no relative `../..` forms. Then drop `"${PMOS_SOURCE_DIR}/include/pb_tree"` from `PMOS_INCLUDE_DIR` in CMakeLists.txt:120 so the single root is the only one and regressions are caught by the build. Change `#inc

### 13 of 14 concepts in concepts.h are dead; FindVar<T>() dereferences a null pointer
`include/pab_tree/dependence/concepts.h`:63 · dead-code
**Status: fixed**

Grepping `include/` for every name declared in `concepts.h` finds no use of `IsPointer`, `CheckPointerToNode`, `IsPair`, `IsBox`, `NodeHasNonTrivialAug`, `ContainsType`, `FindVar`, `CallableWithArg`, `InvokeWithOptionalArg`, `CheckBinaryNode`, `CheckMultiNode`, `CheckDynamicNode`, `IsAugPoint`, `HasBox` or `SupportsForceParallel` outside comments (`tree_node.h:445-457`, `b_tree.h:490`). The single live concept is `IsPBTree`, used only in `tests/test_framework.h:379,392,469,482`. The `FindVar` terminal overload is

```cpp
template<typename T>
constexpr T& FindVar()
{
  static_assert(!std::same_as<T, T>, "Type not found in parameter pack");
  return *reinterpret_cast<T*>(nullptr);
}
```

It is

*Fix:* Delete the unused concepts and `FindVar`/`InvokeWithOptionalArg`/the `BasicPoint`-era `CheckBinaryNode`/`CheckDynamicNode`/`IsAugPoint`/`HasBox`, and replace them with the three concepts the library actually needs: `EntryType` (see the element-contract finding), `LeafAugmentation`, `InteriorAugmentation` — each attached to its `TypeTrait` template parameter with `requires`.

### Key comparison is hardcoded to operator< on K and operator< on E; no comparator can be supplied
`include/pab_tree/dependence/tree_node.h`:112 · extensibility
**Status: fixed**

`TypeTrait` exposes `using Num = Num_Comparator<K>` (`type_trait.h:51`) but `Num` is used only by `box_op.h` (min/max/limits for validation boxes) — never for ordering. Every ordering decision is a raw operator:

- `tree_node.h:112,122` `Entry::GetKey(e) < k` / `<= k` (leaf binary search)
- `tree_node.h:131,136` `== k` (leaf membership)
- `tree_node.h:357,371` `key <= k` / `key < k` (interior descent)
- `tree_node.h:382-386` `ValidateKeyInNode`
- `points_op.hpp:128,130,190,196` (both batch partitioners)
- `range_report.hpp:48,54` and `range_count.hpp:36,42`
- `node_op.hpp:406,554` and `leaf_op.hpp:109,144,153`
- `leaf_op.hpp:31,37,56` `std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter())` a

*Fix:* Add a seventh trait parameter `typename KeyCompare = std::less<K>` to `TypeTrait`/`BTreeTypeTrait`, expose it as `TT::KeyLess`, and route every site above through it (`TT::KeyLess{}(a,b)` for `<`, `!less(a,b) && !less(b,a)` for `==`). Replace the element sorts with key-projected sorts — `parlay::sort(In, [](E const& a, E const& b){ return TT::KeyLess{}(Entry::GetKey(a), Entry::GetKey(b)); })` — wh

### No allocator customization point: parlay::type_allocator is hard-coded, and Node* ownership is documented nowhere
`include/pab_tree/dependence/tree_node.h`:229 · extensibility
**Status: deferred (4.8)**

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

*Fix:* Add an allocator to the trait: `using NodeAlloc = NodeAllocType;` on `BTreeTypeTrait` defaulting to a `ParlayTypeAllocator` adaptor exposing `template<class T> static T* Alloc()` / `static void Free(T*)`, and route all eight sites through `TT::NodeAlloc`. Drop the unused `Range` parameter from `AllocEmptyLeafNode`, delete the `AllocInteriorNode<Interior,TT>()` overload. Add a README section statin

### LeafAug/InteriorAug interface is undocumented, unconstrained, and mandatory even for an unaugmented tree
`include/pab_tree/dependence/type_trait.h`:68 · api-usability
**Status: fixed**

`BTreeTypeTrait` takes `LeafAugType` and `InteriorAugType` (type_trait.h:68-69) and forwards them with no constraint. There is a `concepts.h` in the same directory, but no concept in it constrains an augmentation.

I started with the honest minimum, `struct LeafAug {}; struct InteriorAug {};`. Build, Contain, RangeCount and RangeReport all compiled and ran fine with those — the requirement only appears once `BatchInsert`/`BatchDelete` are instantiated, at which point 4 errors fire from deep inside instantiation stacks:

```
tree_node.h:193: error: '...::LeafAug' {aka 'struct LeafAug'} has no member named 'UpdateAug'
  193 |     return aug.UpdateAug(In);
... error: too many initializers for '

*Fix:* Two changes. (1) Define and apply concepts in `dependence/concepts.h`, e.g. `template<typename A, typename Slice> concept LeafAugmentation = std::default_initializable<A> && std::constructible_from<A, Slice> && requires(A a, Slice s) { a.UpdateAug(s); };` and `template<typename A> concept InteriorAugmentation = std::default_initializable<A> && requires(A a, size_t n) { a.Update(); { A::ForceParall

## MEDIUM — 34

### 288 of 296 bytes of BTree are dead instrumentation; 4 timers start a clock on every construction
`include/pab_tree/b_tree.h`:631 · dead-code
**Status: fixed**

The entire private section is:

```cpp
private:
  Node* root_ = nullptr;
  parlay::internal::timer insert_leaf_timer;
  parlay::internal::timer lower_bound_timer;
  parlay::internal::timer merge_timer;
  parlay::internal::timer recurse_timer;

  double insert_leaf_time = 0.0;
  double lower_bound_time = 0.0;
  double merge_time = 0.0;
  double recurse_time = 0.0;

  int vis_leaf_num = 0;
  int vis_inter_num = 0;

  std::atomic<size_t> max_reinsert_batch_size = 0;
  std::atomic<size_t> sys_reinsert_size = 0;
  std::atomic<size_t> sys_balance_leaf_size = 0;
};
```

Grepping the whole of `include/` and `tests/` for all fourteen names yields hits only in commented-out lines: node_op.hpp:425 `//

*Fix:* Delete lines 633-648 outright, along with the four commented-out references in node_op.hpp and b_batch_delete.hpp. If per-operation profiling is wanted later, it belongs behind the same `Logger` template-parameter mechanism proposed for the queries, not in the object's layout. After removal `BTree` has one member and `BTree(BTree&&) noexcept`, `operator=(BTree&&)`, and a `void Swap(BTree&) noexcep

### Trailing-underscore overloads Build_/BatchInsert_/BatchDelete_ are public and undocumented, and carry the constraints that belong on the public overload
`include/pab_tree/b_tree.h`:295 · api-usability
**Status: fixed**

Three pairs of near-identical names sit side by side in the public section:

```cpp
  template<typename Range> void Build(Range&& In);        // 261
  template<typename Range> void Build_(Range In);         // 264

  template<typename Range, typename LeafSortPred = DefaultHybridPred>
  void BatchInsert(Range&& In, const TT::PatternType, const bool,
                   LeafSortPred const& = DefaultHybridPred{});          // 290

  template<typename Range, typename LeafSortPred>
    requires std::predicate<LeafSortPred, Node*, Range>
  void BatchInsert_(Range In, const TT::PatternType, const bool,
                    LeafSortPred const&);                               // 297
```

There is no co

*Fix:* Move `Build_`, `BatchInsert_`, `BatchDelete_` below `private:` (they are pure slice-taking implementations) and hoist the constraints onto the public overloads:

```cpp
template<typename Range, typename LeafSortPred = DefaultHybridPred>
  requires std::predicate<LeafSortPred,
                          Node*,
                          parlay::slice<E const*, E const*>>
void BatchInsert(Range&& In,

### Public constants BTree::kPivotNum and BTree::kBucketNum name trait members that do not exist
`include/pab_tree/b_tree.h`:60 · correctness · VERIFIED
**Status: fixed**

```cpp
  static constexpr BucketType const kBuildDepthOnce = TT::kSkeletonHeight;
  static constexpr BucketType const kPivotNum = TT::kPivotNum;    // line 60
  static constexpr BucketType const kBucketNum = TT::kBucketNum;  // line 61
```

But in `BTreeTypeTrait` those two members are commented out:

```cpp
  // static constexpr BucketType const kPivotNum = (1 << kSkeletonHeight) - 1;
  // static constexpr BucketType const kBucketNum = 1 << kSkeletonHeight;
```
(dependence/type_trait.h:140-141)

Because a static data member initializer in a class template is only instantiated on use, this compiles today. Verified that any use is a hard error:

```
b_tree.h:60:53: error: 'kPivotNum' is not a

> Verifier narrowed: Claim stands, with two adjustments. Broader than stated: the failure does not require naming the constants — an explicit instantiation (`template class pb_tree::BTree<TT>;`) also hard-errors at b_tree.h:60-61, since that instantiates static data member initializers. Narrower in impact: no shipped code path or test reads these constants, so there is

*Fix:* Delete lines 60-61 from b_tree.h. If sieve-based building is intended to return (the `SIEVE` code path shares these names), restore the two `static constexpr` definitions in `BTreeTypeTrait` at type_trait.h:140-141 instead, so the aliases resolve. Do not leave the aliases pointing at commented-out members.

### No size()/empty()/swap()/clear(), no iteration, no single-key ops, and no documented extension contract
`include/pab_tree/b_tree.h`:596 · api-usability
**Status: not addressed**

The container-shaped vocabulary an ordered-set user reaches for is entirely absent — `grep -nE '\b(begin|end|size|empty|swap|insert|erase|find|clear)\s*\(' include/pb_tree/b_tree.h` matches only the hash lambdas at lines 279-281 and 418-420. What exists instead is `GetSize()` (non-const, see the const-correctness finding), `DeleteTree()` as the de facto `clear()`, and `GetEntries(Range&)` as the only way to read contents — which requires the caller to pre-size a buffer from `GetSize()`, cannot be called on a `const` tree, and crashes on an empty one.

Which absences are defensible: no `begin()/end()`, no `operator[]`, no `insert(k)`/`erase(k)`. This is a batch-parallel bulk structure; per-el

*Fix:* Add the four cheap members — `size_t size() const`, `bool empty() const`, `void clear() { DeleteTree(); }`, `void Swap(BTree&) noexcept` — keeping `GetSize`/`DeleteTree` as aliases so the benchmark harness is unaffected. Turn the augmentation contract into two concepts in `dependence/concepts.h` and constrain the class head:

```cpp
template<typename A, typename Slice>
concept LeafAugmentation = r

### NodeType is a near-vestigial knob: Leaf and Interior are hardcoded
`include/pab_tree/b_tree.h`:204 · extensibility
**Status: not addressed**

`NodeType` is the sixth trait parameter and is bound to `pb_tree::ProtoNode` at every one of the six instantiation sites in the repo (`tests/b_test.cpp:59`, `unit_test.cpp:64`, `cpam.cpp:38`, `pma.cpp:40`, `stdset.cpp:31`, `tlxbtree.cpp:31`). What it actually controls is only the common base of the two node structs (`LeafNode : TypeTrait::Node`, `MultiWayNode : TypeTrait::Node`), and the base must expose a `(bool is_leaf, size_t size)` constructor plus public `is_leaf`/`size` fields, because `b_tree.h:224-257` (`NodeHasLeafChildren`, `GetNodeInvalidStatus`, `NodeContainInvalidSubtree`, `GetNodeReinsertSize`) and every `static_cast<Interior*>(T)` in the impl depend on exactly that shape. The

*Fix:* Either drop the parameter and hardcode `ProtoNode` (documenting `Leaf`/`Interior` as fixed), or make it real: add `template<typename T> concept NodeBase = std::constructible_from<T, bool, size_t> && requires(T t){ {t.is_leaf} -> std::convertible_to<bool>; {t.size} -> std::convertible_to<size_t>; };` and constrain `NodeType` with it. Remove the unused `EleAssignTag` parameter from `LeafNode` or use

### DefaultHybridPred's address hashing does not do what its comment claims, and leaf sort state leaks into RangeReport ordering
`include/pab_tree/b_tree.h`:271 · api-usability
**Status: fixed**

```cpp
auto const v = (static_cast<size_t>(T->size) + In.size()) * 2654435761ULL ^
               (reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
               (reinterpret_cast<uintptr_t>(In.end()) >> 3);
return v & 1;
```

Only bit 0 of `v` is used, so the address terms contribute only bit 3 of `begin` and bit 3 of `end`. For the 16-byte-aligned allocations parlay hands out, bit 3 of `begin` is zero and bit 3 of `end` degenerates to a parity of the slice length — so the comment's "XOR in low address bits from begin/end to break correlation across call sites" achieves essentially nothing. I verified the outcome is in fact stable: the same binary run six times produced an identical `Sorted

*Fix:* Drop the address terms from `DefaultHybridPred` — keep `return ((static_cast<size_t>(T->size) + batch_sz) * 2654435761ULL) >> 63;` (use a high bit, not bit 0, of the multiplicative hash) — which removes the `reinterpret_cast` and makes the behaviour obviously deterministic. Change the predicate contract to `bool(Node const* T, size_t batch_size)` to match the documentation and to remove the per-ca

### 143 public members with a single private section: no interface/implementation boundary on BTree
`include/pab_tree/b_tree.h`:631 · api-usability
**Status: fixed**

`class BTree` has one `private:` (line 631), holding `root_` and eight instrumentation members. Everything from line 27 to line 630 — roughly 143 declarations — is public. That includes every internal recursion (`BatchInsertRecursive`, `BatchDeleteRecursive`, `FetchInvalidSubtreeNodes`, `BalanceOverLeaves`, `ReInsertTreeAfterDelete`, `PruneOutEmptyTree`, `DeleteTreeRec`, `CopyTreeRecursive`), every node-plumbing helper (`GenerateLeafNodes`, `GenerateInteriorNodes`, `BatchInsertUpdateInterior`, `HandlingLeafSplitOverflow`, `SplitBatchMerge`, `PostProcessLeafChange`), every bucket-arithmetic helper (`GetBktNum`, `GetBktStIdx`, `GetBktEndIdx`), and the underscore-suffixed private-by-convention

*Fix:* Move the ~130 internal members behind `private:` and keep only the 12 entry points public. Where tests need internals, grant access with a single `template<typename> friend class BTreeTestAccess;`. Move the free helpers in `b_tree_impl/` and the node allocation helpers in tree_node.h into `namespace pb_tree::detail`. Delete the timer/counter/atomic instrumentation members (nothing reads them) — th

### const-correctness gaps: GetSize/GetRoot/Validate are non-const, and [[nodiscard]] is on 2 of ~30 pure observers
`include/pab_tree/b_tree.h`:612 · api-usability
**Status: fixed**

`GetSize` and `GetRoot` mutate nothing but are declared non-const:

```cpp
  Node* GetRoot() { return this->root_; }          // 607
  size_t GetSize() { return this->root_ ? this->root_->size : 0; }   // 612
```

so `void f(BTree<TT> const& t) { t.GetSize(); }` does not compile — you cannot ask a `const BTree&` how big it is. `Validate` (537), `SortedLeafRatio` (544), `VerifyTreeProperty` (522), `CheckHeight` (526) and `GetEntries` (506) are likewise non-const while taking `Node const*` arguments, i.e. they are declared const-unfriendly against their own evidence. Meanwhile `Contain` (548), `RangeCount` (554) and `RangeReport` (567) *are* const, so the class is internally inconsistent about

*Fix:* Make `GetSize`, `GetRoot` (returning `Node const*`, with a separate non-const overload if needed), `Validate`, `SortedLeafRatio`, `VerifyTreeProperty`, `CheckHeight` and `GetEntries` const member functions: `[[nodiscard]] size_t GetSize() const noexcept;`. Add `[[nodiscard]]` to all of those plus `Contain`, `RangeCount`, `RangeReport`, `CountSortedLeaves`, `LeafNode::Contain`, `LeafNode::CapacityF

### BTreeInteriorNode::reinsert_size is left indeterminate by all three constructors
`include/pab_tree/b_tree.h`:201 · correctness · VERIFIED
**Status: fixed**

`BTreeInteriorNode` declares three members and three constructors (b_tree.h:142-165, 199-201):

```cpp
    BTreeInteriorNode(TT::NodeSeq const& _tree_nodes, TT::SplitSeq const& _split)
      : BaseNode(_tree_nodes, _split, typename TT::InteriorAug())
      , valid(true)
      , contain_invalid_subtree(0)
    {}                                    // reinsert_size not initialised
    ...
    bool valid;
    bool contain_invalid_subtree;
    SizeT reinsert_size;                  // no default member initialiser
```

All three constructors initialise `valid` and `contain_invalid_subtree` and none initialises `reinsert_size`, which has no default member initialiser either — so a freshly allocated

> Verifier narrowed: Narrow to a low-severity hygiene/extensibility issue, not a live defect. Correct statement: `BTreeInteriorNode::reinsert_size` (include/pb_tree/b_tree.h:201) is left indeterminate by all three constructors (b_tree.h:142-165) while its two siblings `valid`/`contain_invalid_subtree` are initialised, so freshly allocated (and type_allocator-recycled)

*Fix:* Give all three members default member initialisers and delete them from the mem-init lists: `bool valid = true; bool contain_invalid_subtree = false; SizeT reinsert_size = 0;`. `contain_invalid_subtree` is a `bool` currently initialised from the integer literal `0` — make that `false` while you are there. The `ResetDeletionFlag()` fixup in b_batch_delete.hpp:207 can stay as a correctness measure b

### DefaultHybridPred hashes heap addresses, making the default insert/delete path ASLR-nondeterministic
`include/pab_tree/b_tree.h`:279 · testing
**Status: not addressed**

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

*Fix:* Remove the address terms from `DefaultHybridPred` so the decision is a pure function of `(T->size, In.size())`:

```cpp
return ((static_cast<size_t>(T->size) + In.size()) * 2654435761ULL) & 1;
```

(The address XOR was there to decorrelate call sites; a second multiply-shift round on the size sum achieves that without nondeterminism.) Then add a regression test that runs the same batch sequence tw

### BTree::kPivotNum and BTree::kBucketNum reference trait members that no longer exist
`include/pab_tree/b_tree.h`:60 · dead-code
**Status: not addressed**

b_tree.h:60-61 declares two public constants from the trait:

```cpp
static constexpr BucketType const kPivotNum = TT::kPivotNum;
static constexpr BucketType const kBucketNum = TT::kBucketNum;
```

But both trait members are commented out in type_trait.h:140-141:

```cpp
// static constexpr BucketType const kPivotNum = (1 << kSkeletonHeight) - 1;
// static constexpr BucketType const kBucketNum = 1 << kSkeletonHeight;
```

Because a static data member's initialiser is only instantiated when the member is odr-used, this compiles silently until someone touches it. I confirmed the failure:

```
$ ... std::cout << Tree::kBucketNum;
include/pb_tree/b_tree.h:61:54: error: 'kBucketNum' is not a memb

*Fix:* Delete b_tree.h:60-61 outright, since nothing in the compiled paths uses them (`b_batch_insert_sieve.hpp` is commented out of the include list at b_tree.h:656). If the sieve path is intended to come back, restore the two definitions in type_trait.h:140-141 instead so the declarations are valid.

### `using TT = TT;` at b_tree.h:139 emits -Wchanges-meaning in every user translation unit
`include/pab_tree/b_tree.h`:139 · standards
**Status: not addressed**

`BTreeInteriorNode` opens with a self-referential alias:

```cpp
// b_tree.h:137-140
struct BTreeInteriorNode : MultiWayNode<TT>
{
  using TT = TT;
```

GCC 15 warns on this for every TU that includes the header — 9 lines of noise on every single compile of my program, including the ones that otherwise succeeded:

```
include/pb_tree/b_tree.h:139:11: warning: declaration of 'using ...::BTreeInteriorNode::TT = ...' changes meaning of 'TT' [-Wchanges-meaning]
  139 |     using TT = TT;
      |           ^~
include/pb_tree/b_tree.h:139:16: note: used here to mean '...::TT = TypeTrait'
include/pb_tree/b_tree.h:28:9: note: declared here
   28 |   using TT = TypeTrait;
```

[basic.scope.class] mak

*Fix:* Delete line 139. `TT` from the enclosing `BTree<TypeTrait>` is already in scope inside `BTreeInteriorNode`; the only other user of that alias is `using BaseNode = MultiWayNode<TT>;` on the next line, which resolves identically without it. Separately, replace the global `add_compile_options(-w)` at CMakeLists.txt:23 with a warning set for library headers, or this class of defect will keep accumulat

### Query API returns std::pair<result, Logger>, forcing .first at every call site
`include/pab_tree/b_tree.h`:548 · api-usability
**Status: fixed**

All three query entry points bundle a profiling logger into the return value:

```cpp
// b_tree.h:548, 554, 567
auto Contain(K const& k) const -> std::pair<bool, MembershipLogger>;
auto RangeCount(TT::Box query_box) const -> std::pair<SizeT, RangeReportLogger>;
template<typename Range>
auto RangeReport(TT::Box query_box, Range&& out) const -> std::pair<SizeT, RangeReportLogger>;
```

So every membership test in user code reads `if (tree.Contain(k).first)`. `if (tree.Contain(k))` does not compile, and — worse for a reader — a `std::pair<bool, Logger>` is *not* contextually convertible to bool, so at least it fails loudly rather than silently being always-true.

The logger is not documented an

*Fix:* Make the logger opt-in rather than mandatory: keep the current functions under logging names (`ContainLogged`, `RangeCountLogged`, `RangeReportLogged`) and add the plain forms `bool Contain(K const& k) const`, `SizeT RangeCount(TT::Box) const`, `template<typename Range> SizeT RangeReport(TT::Box, Range&&) const` that discard the logger. Alternatively gate the logger on a template parameter, `templ

### DEBUG is an unprefixed configuration macro that changes template bodies — ODR hazard and a stdout-printing surprise
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:111 · standards
**Status: not addressed**

The library reads exactly one user-settable macro, `DEBUG`, at five sites (b_batch_delete.hpp:111,174; b_batch_insert.hpp:85,143; node_op.hpp:140,464,609), and all of them sit inside the bodies of `BTree<TT>` member templates. Two consequences.

**ODR.** Because these are templates in a header-only library, TU A compiled with `-DDEBUG` and TU B without it emit *different definitions of the same entity*. The linker keeps one arbitrarily. Nothing warns. CMakeLists.txt:97 makes this easy to hit: `if(DEBUG) add_compile_options(-g -march=native -DDEBUG)` applies to the whole project, so a mixed build (this library in a DEBUG consumer linked against a Release-built one) silently gets whichever bod

*Fix:* Rename the macro to a namespaced one — `PBTREE_DEBUG_CHECKS` — at all seven sites and in CMakeLists.txt:97 (`add_compile_options(-DPBTREE_DEBUG_CHECKS)`). Remove the `puts(">>> pass tree aug after delete")` entirely, or route it through a caller-supplied logger. Document `PBTREE_DEBUG_CHECKS` in README.md, stating explicitly that all TUs linked together must agree on it. Note that `SIEVE`, `MEMCHE

### No reusable traversal skeleton: every batch op re-implements partition-recurse-reassemble
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:127 · extensibility
**Status: not addressed**

`BatchInsertRecursive` (`b_batch_insert.hpp:127-192`) and `BatchDeleteRecursive` (`b_batch_delete.hpp:165-229`) share ~40 near-identical lines: the `static_cast<Interior*>(T)`, the identical partitioner selection

```cpp
if (In.size() > TT::kSplitFactor * TI->GetSplitN()) {
  SplitBatchBinarySearch(split_pos_seq, In, TI->GetSplitSlice());
} else {
  SplitBatchMerge(split_pos_seq, In, TI->GetSplitSlice());
}
```

the `#ifdef DEBUG` range assertion, the touched-children loop computing `l`/`r` from `split_pos_seq` and filling `nodes_map[cnt++]`, and the `parlay::parallel_for(0, cnt, ..., 1)` dispatch. Only the per-child payload and the aggregation differ. The key-vs-entry difference is handled

*Fix:* Extract the shared skeleton as `template<typename Range, typename ChildFn> static BufSizeT ForEachTouchedChild(Interior* TI, Range In, SizeBufUBArr& split_pos_arr, ChildFn&& fn)` in `points_op.hpp`, where `fn(BufSizeT child_idx, BufSizeT slot, SizeT l, SizeT r)` runs inside the `parallel_for`; have both recursions call it. Replace the duplicated `if constexpr` with one `static K KeyOf(auto const&

### BatchInsert reserves 2 pairs (32 bytes) of scratch per input element regardless of splits
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:77 · performance
**Status: not addressed**

```cpp
NodeSplitPairSeq node_split_seq =
  NodeSplitPairSeq::uninitialized(2 * sorted_slice.size());
```
`NodeSplitPair` is `std::pair<Node*, Split>` — 16 bytes with padding for a 32-bit key — so this reserves `32 * n` bytes for every batch. At the benchmarked n = 1e9 that is a **32 GB** allocation, on top of the sorted copy. The 2n factor is genuinely the worst case (a leaf at capacity receiving a single element needs 2 slots, so the ratio really is 2 at n=1), but it is the worst case for a *pathological* batch: in the common case where few or no nodes split, essentially all of it is untouched. `ReInsertTreeAfterDelete` (node_op.hpp:669) allocates the same 2n scratch a second time during re

*Fix:* Bound the scratch by what can actually be produced rather than by input size. The number of nodes any subtree can hand back is `(subtree_size + batch_size) / kBufMD + 1`; sizing `node_split_seq` as `2 * min(n, root_->size / TT::kBufMD + n / TT::kBufMD + 2)` covers every split path while collapsing the common case. If that analysis is not worth doing, at least document the `32*n` bytes of scratch i

### No comparator customization point: sorts use E::operator< while navigation uses Entry::GetKey, with no check they agree
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:72 · extensibility
**Status: fixed**

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

*Fix:* Sort by key explicitly everywhere: `parlay::sort(In, [](E const& a, E const& b){ return Entry::GetKey(a) < Entry::GetKey(b); })` and the equivalent for the four `std::sort` calls, which removes the dependency on `E::operator<` entirely. Alternatively add a `using KeyLess = KeyLessType;` to `BTreeTypeTrait` (defaulting to `std::less<K>`) and thread it through all nine sort sites plus `BinarySearchK

### Duplicate keys are silently stored as duplicates; the tree is a multiset, and nothing says so
`include/pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp`:29 · docs
**Status: fixed**

Neither `Build` nor `BatchInsert` deduplicates, and neither documents that. I built from 1000 entries covering only 500 distinct keys (each key twice) and then inserted a key that was already present:

```
dup build size=1000 (input 1000, distinct keys 500)
  count[0,500) = 1000
  after inserting existing key 42: size=1001 count[42,43)=3
  after deleting key 42 once: size=1000 count[42,43)=2
```

So the structure is a multiset: `Build` keeps both copies of every key, `BatchInsert` of an existing key adds a third rather than replacing or being a no-op, and `BatchDelete` of that key removes exactly one copy, not all of them. Every one of those three behaviours is a defensible design choice, an

*Fix:* State the semantics on the class in b_tree.h:25 and in the README: "BTree is a multiset over keys: Build and BatchInsert do not deduplicate, BatchInsert of an existing key stores an additional entry, and BatchDelete of a key removes one entry per occurrence in the batch. Deduplicate input with parlay::unique if set semantics are required." If set semantics are actually intended, add the dedupe in

### AssembleInteriorLevels silently overwrites the caller's nodes/keys buffers
`include/pab_tree/b_tree_impl/batch_op/b_build_tree.hpp`:43 · extensibility
**Status: not addressed**

`AssembleInteriorLevels(TT::NodeSlice nodes, TT::SplitSlice keys)` allocates one auxiliary buffer pair and then ping-pongs:
```cpp
auto swap_key_node = [&](SizeT bkt_n) {
  key_in  = key_in.cut(0, bkt_n - 1);   key_out = key_out.cut(0, bkt_n - 1);
  node_in = node_in.cut(0, bkt_n);      node_out = node_out.cut(0, bkt_n);
  std::ranges::swap(key_in, key_out);   std::ranges::swap(node_in, node_out);
};
```
After the first swap, `node_out`/`key_out` alias the *caller's* `nodes`/`keys`, so from the second iteration onward the function writes over its own input. Both current callers happen to pass throwaway scratch (`node_seq`/`key_seq` in `BuildRecursive`, `parlay::tabulate` results in `BatchIns

*Fix:* Either document and rename to make ownership explicit — `Node* AssembleInteriorLevelsInPlace(TT::NodeSlice scratch_nodes, TT::SplitSlice scratch_keys)` with a comment stating both slices are clobbered — or take ownership by value: `Node* AssembleInteriorLevels(TT::NodeSeq nodes, TT::SplitSeq keys)` and have the two callers `std::move` their sequences in. The second is cheaper to reason about and c

### Query return types hard-code an instrumentation struct; Contain's copy is never even populated
`include/pab_tree/b_tree_impl/query_op/membership.hpp`:11 · api-usability
**Status: not addressed**

Every query returns a pair whose second element is a counter struct:

```cpp
auto BTree<TT>::Contain(K const& k) const -> std::pair<bool, MembershipLogger>
{
  MembershipLogger logger;                       // default-constructed {0, 0}
  return { ContainIter(this->root_, k), logger }; // ContainIter takes no logger
}
```

`MembershipLogger` (loggers.h:8-12) is `{vis_leaf_num, vis_interior_num}`, both `= 0`. `ContainIter` never receives it, so `Contain` always returns `{result, {0, 0}}` — 16 bytes of guaranteed zeros that every caller must destructure. The library's own harness knows this: tests/test_framework.h:844-845 has the reads commented out (`// vis_leaf[i] = logger.vis_leaf_num;`).

*Fix:* Make the logger a defaulted template parameter and return the plain value:

```cpp
struct NoOpLogger {                       // in loggers.h
  constexpr void VisitLeaf() const noexcept {}
  constexpr void VisitInterior() const noexcept {}
  constexpr void FlattenNode() const noexcept {}
};

template<typename Logger = NoOpLogger>
bool Contain(K const& k, Logger&& logger = {}) const;

template<typen

### The branchless binary search is hand-inlined four times with a hardcoded base of 8
`include/pab_tree/b_tree_impl/query_op/range_report.hpp`:98 · standards
**Status: not addressed**

`BinarySearch<_binary_search_base>` (binary_search.h:41-76) exists precisely as the shared primitive, and the node types expose a tuned `kBinarySearchBase` derived from `std::hardware_constructive_interference_size` (tree_node.h:57-64 and :276-283). Yet the right-boundary search is copy-pasted inline four times with a literal cutoff: `while (hi - lo > 8)` at range_report.hpp:98 (interior splits) and :46 (leaf entries), and range_count.hpp:79 and :34. The bodies are identical modulo the accessor. The hardcoded 8 also throws away the cache-line tuning the rest of the library goes to some trouble to compute (for `kBuf=64` with 8-byte entries the leaf base is 8, but for 16-byte entries it is 4).

*Fix:* Replace all four inlined loops with the primitive, adding an offset-taking overload so the narrowing is preserved: `template<size_t base, typename Seq, typename F> size_t BinarySearchFrom(Seq const& I, size_t lo, F const& less)` in `binary_search.h`, then call `TI->GetSplitSlice()`-based `BinarySearchFrom<Interior::kBinarySearchBase>(split_slice, l, [&](K key){ return key < query_box.second; })` i

### No RAII and no exception guarantee: a throwing allocation mid-insert leaves the tree double-freeable
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:111 · standards
**Status: not addressed**

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

*Fix:* Two concrete steps. (1) Reorder so no node is freed before its replacement exists: in `HandlingLeafSplitOverflow` move `FreeNode<Leaf>(T)` to after the `GenerateLeafNodes` call, and likewise move `FreeNode<Interior>(T)` in `HandlingInteriorSplitOverflow` to after `GenerateInteriorNodes` returns. (2) State the guarantee in the class comment on `BTree` in `b_tree.h` — e.g. "no-throw element and augm

### Deleting a key that is not in the tree aborts in Debug builds; the precondition is undocumented
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:140 · api-usability
**Status: not addressed**

```cpp
assert(T->size >= In.size());
assert(parlay::all_of(In, [&](auto a) {
  return std::find_if(TL->elements.begin(), TL->elements.begin() + TL->size,
                      [&](auto const e) { return Entry::GetKey(e) == a; }) !=
         TL->elements.begin() + TL->size;
}));
```

plus `assert(new_node->size + In.size() == old_size)` at b_batch_delete.hpp:155. `BatchDelete`'s declaration (b_tree.h:319-325) documents nothing, and neither does README.md. Reproduced: build 1000 keys 0..999, `BatchDelete` of keys 5000..5099, compiled without `-DNDEBUG` -> `Assertion 'parlay::all_of(In, ...)' failed`, abort. In Release the same call is harmless (verified: size stays 1000) because `DeletePoints4

*Fix:* Decide and document the contract. If absent keys are meant to be allowed (the Release behaviour, and the more useful semantics), delete the two preconditions at leaf_op.hpp:140-146 and relax b_batch_delete.hpp:155 to `assert(new_node->size + In.size() >= old_size)`; then state in the `BatchDelete` doc comment that keys not present are ignored and duplicates are idempotent, and add a unit test cove

### Leaf sortedness is decided by E::operator< but every query uses Entry::GetKey — an unguarded extension trap
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:31 · extensibility
**Status: fixed**

`PostProcessLeafChange` sets `is_sorted = true` after `std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter())` (lines 31 and 37) — the default `operator<` on the element type `E`. `ExtractPointsInLeaf` does the same when `sorted == true` (leaf_op.hpp:56). But every query path orders by the trait: `Leaf::BinarySearchKey` uses `Entry::GetKey(e) < k` (tree_node.h:112), `RangeCountLeaf`/`RangeReportLeaf` use `Entry::GetKey(...)`, and `GetBox` uses `TT::Entry::GetKey`. Nothing in `concepts.h` or `BTreeTypeTrait` requires `E` to have an `operator<` at all, let alone one consistent with `Entry::GetKey`; it works today only because `tests/wrapper.h:107` happens to define `operator<` as `this->key < r

*Fix:* Sort through the trait everywhere: `std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter(), [](E const& a, E const& b){ return Entry::GetKey(a) < Entry::GetKey(b); })` in `PostProcessLeafChange` and `ExtractPointsInLeaf`. Add a concept, e.g. `template<class T> concept EntryTrait = requires(typename T::E e){ { T::GetKey(e) } -> std::totally_ordered; };`, and `static_assert` it in `BTreeTypeTrait`. Re

### BatchDelete asserts every key is present; deleting an absent key aborts in non-NDEBUG builds
`include/pab_tree/b_tree_impl/tree_op/leaf_op.hpp`:141 · correctness · VERIFIED
**Status: not addressed**

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
  - `leaf_op.hpp:164-166` — `assert(end == begin || std::cmp_equal(std::distance(begin, end

*Fix:* Remove the second assertion at leaf_op.hpp:141-145 (the implementation already handles absent keys correctly via the two-pointer compaction), and document on the `BatchDelete` declaration in b_tree.h:319-325 that keys not present are ignored. Keep `assert(T->size >= In.size())` only if that is genuinely required — my probe suggests it is not, since deleting 3 absent keys from a leaf is legal.

### GetEntries returns unsorted output, has no size check, and its ordering contract is undocumented
`include/pab_tree/b_tree_impl/utility.hpp`:22 · api-usability
**Status: not addressed**

`GetEntries(Range& Out)` calls `FlattenRec<TT::EntrySlice, false, /*sorted=*/false>` over `Out.begin() .. Out.begin() + root_->size`. With `sorted == false`, `ExtractPointsInLeaf` (leaf_op.hpp:51-62) copies each leaf verbatim, so any leaf with `is_sorted == false` contributes its entries in arbitrary order. It also silently assumes `Out.size() >= GetSize()` with no check (only a debug `assert(Out.size() >= TL->size)` per leaf, leaf_op.hpp:60), and dereferences a null root (see the null-root finding). The same applies to `RangeReport`'s output, whose order is 'sorted across leaves, arbitrary within an unsorted leaf'.

*Fix:* Either make it sorted — `GetEntries` should call `FlattenRec<..., /*sorted=*/true>` (which sorts each leaf in place and is already used by the delete path) — or rename/document it, e.g. `void GetEntriesUnordered(Range& Out)` plus a `GetEntriesSorted`. Whichever is chosen, add the precondition `Out.size() >= GetSize()` to the declaration in `b_tree.h:505-506` and check it, and document `RangeReport

### Validate() is assertion-only, so it returns true on a corrupt tree in release builds
`include/pab_tree/b_tree_impl/validation.hpp`:50 · testing
**Status: fixed**

`VerifyTreeProperty` reports every structural violation through `assert`, and the failure path even prints before asserting:
```cpp
if (box.first < TI->split[i - 1] || box.second >= TI->split[i]) {
  std::cout << "Validation failed at interior node split check." << std::endl;
  std::cout << "Box: [" << box.first << ", " << box.second << "], Splits: [" ...;
  assert(false);
}
```
Under `-DNDEBUG` the `assert` is gone but the `std::cout` is not, so `Validate()` prints "Validation failed..." and then returns `true`. Observed directly in the duplicate-key repro:
```
Validation failed at interior node split check.
Box: [7, 7], Splits: [7, 7]
...
dup batch: size=2000 (expect 2000) valid=1        <

*Fix:* Make `VerifyTreeProperty` return a status instead of asserting. Change its signature to `std::pair<TT::Box, bool> VerifyTreeProperty(Node const*)`, replace each `assert(cond)` with `ok &= cond;`, combine children's `ok` with a `parlay::reduce`, and have `Validate<kCheckSize, kVerbose>` return that flag (keeping the `assert(ok)` in debug builds on top). Route the diagnostic text to `std::cerr` guar

### LeafNode::GetAug() does not compile — the only leaf-augmentation accessor is broken
`include/pab_tree/dependence/tree_node.h`:184 · correctness · VERIFIED
**Status: fixed**

```cpp
AT& GetAug() const
{
  return aug;
}
```

Inside a `const` member function `aug` is a `const AT` lvalue, so binding it to `AT&` is ill-formed. It survives in the repo only because nothing instantiates it. Confirmed by instantiating: `error: binding reference of type 'AT&' {aka 'LA&'} to 'const AT' discards qualifiers` at tree_node.h:186.

> Verifier narrowed: Directionally correct but worth narrowing on two points. (1) Nothing in the library or tests instantiates it, so no build is affected — the breakage is latent and only surfaces for an extender who calls it. (2) There is a working workaround the claim omits: `LeafNode` is a `struct` with no access specifiers anywhere in its body (tree_node.h:42-204)

*Fix:* Replace with the conventional pair: `AT const& GetAug() const { return aug; }` and `AT& GetAug() { return aug; }`, and add a compile-time exercise (an augmentation test in `tests/unit_test.cpp` that reads a leaf aug back) so the accessor stays instantiated.

### Every leaf allocation zero-fills the full kBufUB element array: 22% of Build time
`include/pab_tree/dependence/tree_node.h`:85 · performance
**Status: not addressed**

```cpp
template<typename Range>
LeafNode(Range In, AllocNormalLeafTag)
  : Node{ true, static_cast<size_t>(In.size()) }
  , elements(EntryArr())        // <-- value-initialises all kBufUB entries
  , is_sorted{ true }
  , aug(AT(In))
{
  std::ranges::copy_n(In.begin(), In.size(), elements.begin());
}
```
`EntryArr` is `std::array<E, kBufUB>`; `elements(EntryArr())` value-initialises the whole thing (a memset of `kBufUB * sizeof(E)` bytes) and the very next statement overwrites the live prefix. At the default kBufferSize=64 with an 8-byte element that is 2048 bytes memset per leaf, of which only ~1024 carry data. Building 1e9 entries creates ~7.8M leaves ≈ 16 GB of pointless zeroing.

Measure

*Fix:* Drop the initialiser so `elements` is default-initialised (no-op for trivial `E`) and copy the prefix in the body:
```cpp
LeafNode(Range In, AllocNormalLeafTag)
  : Node{ true, static_cast<size_t>(In.size()) }
  , aug(AT(In))
  , is_sorted{ true }
{ std::ranges::copy_n(In.begin(), In.size(), elements.begin()); }
```
If `E` is ever a non-trivial type this needs `std::uninitialized_copy_n` plus a de

### LeafNode::GetAug() const returns AT& — ill-formed on instantiation
`include/pab_tree/dependence/tree_node.h`:184 · correctness · VERIFIED
**Status: fixed**

```cpp
  AT& GetAug() const
  {
    return aug;
  }
```

In a const member function `aug` is `AT const`, and binding it to `AT&` drops the qualifier. Because `LeafNode` is a class template this is only diagnosed on instantiation, and nothing in the library or tests calls it, so it has never been compiled. Verified: a TU calling `L.GetAug()` fails with `binding reference of type 'AT&' to 'const AT' discards qualifiers` at tree_node.h:186.

This is the single accessor an augmentation-writing extender would reach for first, since `aug` is otherwise reachable only as a public data member. `MultiWayNode` has no `GetAug` at all — `BTreeInteriorNode` reaches its augmentation as `this->aug` directly

> Verifier narrowed: Confirmed as written, with severity narrowed from medium to low. `include/pb_tree/dependence/tree_node.h:184` (`AT& GetAug() const { return aug; }`) is ill-formed on instantiation — reproduced with gcc as "binding reference of type 'AT&' to 'const AT' discards qualifiers" at tree_node.h:186 — but it is never instantiated by the library or any test

*Fix:* Provide the conventional pair and drop the broken one: `[[nodiscard]] AT const& GetAug() const noexcept { return aug; }` and `[[nodiscard]] AT& GetAug() noexcept { return aug; }`. Add the same pair to `MultiWayNode` and route `BTreeInteriorNode`'s five `this->aug` uses through it, so both node types expose one documented accessor and `aug` can become non-public.

### kSkHeightVal (5th trait parameter) only feeds a type used by an operation that is not compiled in
`include/pab_tree/dependence/type_trait.h`:136 · dead-code
**Status: not addressed**

`kSkeletonHeight` is used for exactly one thing:

```cpp
static constexpr uint_fast8_t const kSkeletonHeight = kSkHeightVal;
using BucketType = std::conditional_t<(kSkeletonHeight > 7), uint_fast16_t, uint_fast8_t>;
```

`BucketType` is used by `SeievePoints`/`SplitBatchSieve` in `points_op.hpp:68-108`, which are called only from `b_batch_insert_sieve.hpp` — the header that is commented out of `b_tree.h:656`. Every call site in the repo passes the magic value `8`, with no comment saying what it means. `kBuildDepthOnce` (`b_tree.h:59`), the only other consumer, has no uses either.

*Fix:* Remove `kSkHeightVal` from `TypeTrait`/`BTreeTypeTrait` (making the trait five parameters) along with `kSkeletonHeight`/`BucketType`/`kBuildDepthOnce`, and update the six instantiation sites; or, if the sieve path is intended to return, give the parameter a default (`uint_fast8_t kSkHeightVal = 8`) and a doc comment stating it is the sieve skeleton height and is inert unless `SIEVE` is enabled.

### Element type needs three wrapper layers, and the library's own layer forces a by-value copy on every key access
`include/pab_tree/dependence/type_trait.h`:21 · api-usability
**Status: not addressed**

A user must write an element struct *and* a static-accessor wrapper (`tests/wrapper.h:130`: "NOTE: user needs to add a layer to expose API to b_tree"), and the library then wraps that again in `EleWrapper`, which adds nothing but signature changes:

```cpp
static auto GetKey(E const e)      { return EleType::GetKey(e); }
static auto GetVal(E const e)      { return EleType::GetVal(e); }
static E ToEntry(K const& k, V const& v) { return EleType::ToEntry(k, v); }
```

Note `E const e` — by value. The test wrapper takes `E const&` (`wrapper.h:139`), so the copy is imposed by the library layer, on the hottest path there is: `Entry::GetKey` appears 42 times across the impl, inside every binary-sea

*Fix:* Delete `EleWrapper` and set `using Entry = EleType;` directly in `TypeTrait` (line 48 already comments that `Entry` is 'abstract type to access E'), or at minimum change the forwarders to `static K GetKey(E const& e)` / `static V GetVal(E const& e)`. Replace the class with a documented concept: `template<typename T> concept EntryType = requires(typename T::E const& e) { typename T::E; typename T::

### Trait parameters are positional and undocumented; kSkHeightVal has no discoverable meaning
`include/pab_tree/dependence/type_trait.h`:86 · api-usability
**Status: not addressed**

The only way to instantiate the tree is:

```cpp
template<typename EleType, uint kBufferSize, typename LeafAugType,
         typename InteriorAugType, uint_fast8_t kSkHeightVal, typename NodeType>
class BTreeTypeTrait
```

Six positional parameters, no defaults, no comments on the declaration, no documentation. Three of them are effectively unguessable:

- **`EleType`** is not the element. It is an access layer that must expose `E`, `K`, `V` and `static GetKey(E)`/`GetVal(E)`. I only learned this by finding `EleWrapper<EleType>` at type_trait.h:93 and `Wrapper::ElementWrapper<E>` at tests/b_test.cpp:54. Passing the element struct itself — the obvious first guess — produces 204 lines of error

*Fix:* Give the declaration defaults and a doc comment: `template<typename EleType, uint kBufferSize = 64, typename LeafAugType = EmptyLeafAug, typename InteriorAugType = EmptyInteriorAug, uint_fast8_t kSkHeightVal = 8, typename NodeType = ProtoNode>`, so the common instantiation is `BTreeTypeTrait<MyEle>`. Rename `EleType` to `EntryAccessor` (or add `template<typename A> concept EntryAccessor = requires

### A wrong EleType produces 204 lines with 4 of 6 errors inside parlaylib, while BatchDelete gets it right
`include/pab_tree/dependence/type_trait.h`:21 · api-usability
**Status: not addressed**

I made the single most likely trait mistake — passing the element struct as parameter 1 instead of an accessor layer (the struct did have `K`, `V`, `E` and member `GetKey()`/`GetVal()`, just not the *static* free-standing form `EleWrapper` forwards to). Result: 204 lines of stderr, 6 errors, only the first of which is in pb_tree code:

```
type_trait.h:23:27: error: no matching function for call to 'pb_tree::EleWrapper<MyEntry>::GetKey(const ...::E&)'
parlaylib/include/parlay/primitives.h:899:22: error: static assertion failed
parlaylib/include/parlay/internal/delayed/../../delayed_sequence.h:61:22: error: static assertion failed
parlaylib/include/parlay/utilities.h:348:24: error: static ass

*Fix:* Add a concept beside the existing ones in `dependence/concepts.h` and apply it in `type_trait.h`: `template<typename A> concept EntryAccessor = requires(typename A::E const& e) { typename A::E; typename A::K; typename A::V; { A::GetKey(e) } -> std::same_as<typename A::K>; { A::GetVal(e) } -> std::same_as<typename A::V>; } && std::totally_ordered<typename A::E>;` then constrain `EleWrapper` and `BT

## LOW — 11

### `using TT = TT;` in BTreeInteriorNode emits -Wchanges-meaning in every translation unit
`include/pab_tree/b_tree.h`:139 · standards
**Status: not addressed**

```cpp
struct BTreeInteriorNode : MultiWayNode<TT>
{
  using TT = TT;
```

Every TU that includes `pb_tree/b_tree.h` under GCC 14 gets:

`warning: declaration of 'using pb_tree::BTree<TypeTrait>::BTreeInteriorNode::TT = ...' changes meaning of 'TT' [-Wchanges-meaning]`, with two notes. Reproduced on a one-line TU. The alias is redundant — `TT` is already visible from the enclosing `BTree` (`b_tree.h:28`) — and it is consumed once, at `b_delete_tree.hpp:42` / `flatten.hpp:70` as `Interior::TT::kBufUB`, which resolves identically without it.

*Fix:* Delete the `using TT = TT;` line at `b_tree.h:139`; `Interior::TT` continues to resolve through the enclosing `BTree<TypeTrait>::TT`. If an explicit member alias is wanted, name it differently: `using TypeTraitT = TT;` and update `b_delete_tree.hpp:42` and `flatten.hpp:70`.

### LeafSortPred contract is undocumented, inconsistent between insert and delete, and CLAUDE.md states it wrong
`include/pab_tree/b_tree.h`:271 · docs
**Status: not addressed**

`DefaultHybridPred::operator()(InputNode const* T, Range In)` takes a *range*, but the same predicate object is invoked with two different range types: `PostProcessLeafChange` is reached from `InsertPoints2Leaf` with an `EntrySlice` (leaf_op.hpp:87) and from `DeletePoints4Leaf` with a `TT::KeySlice` (leaf_op.hpp:173). A user-supplied predicate must therefore be a template accepting both, but the `requires std::predicate<LeafSortPred, Node*, Range>` clauses on `BatchInsert_`/`BatchDelete_` only check one of them, so a monomorphic predicate passes the constraint on `BatchInsert` and then fails to compile inside `BatchDelete`. Nothing in the headers documents the signature; `CLAUDE.md` document

*Fix:* Pin the contract down. Document above `DefaultHybridPred` in b_tree.h: `// Callable as: template<class N, class R> bool operator()(N const* leaf, R batch) const; invoked with EntrySlice on insert and KeySlice on delete.` Tighten both requires-clauses to check both instantiations — `requires std::predicate<LeafSortPred, Node*, typename TT::ConstEntrySlice> && std::predicate<LeafSortPred, Node*, typ

### Positive: minimal boilerplate is ~40 lines and the program worked correctly on the 3rd compile
`include/pab_tree/b_tree.h`:25 · api-usability
**Status: not addressed**

Reporting the good side honestly, since several of the above findings sound worse in isolation than the experience was.

Once the two include roots were sorted out, I reached a working program in **3 compile attempts** (fail: include path; fail: 4 missing aug members; pass), which is very good for a template library of this weight. The 4 aug errors were 107 lines total and every one named the missing member in plain English. Compile time was 17 s at `-O3 -march=native` for a 75-line program — slow but tolerable.

Everything behaved as expected at runtime, first try:
```
size after build = 1000
contain(500) = 1
contain(501) = 0
count[100,200) = 50      // keys 0,2,4,...; [first,second) half-o

*Fix:* Commit the program above (or an equivalent) as `examples/minimal.cpp` with its own CMake target, and add it to CI. It exercises Build, Contain, RangeCount, RangeReport, BatchInsert and BatchDelete in 30 lines, would have caught the two-include-root problem the day it appeared, and doubles as the README example that is currently missing.

### Include-guard macros are inconsistently derived — one has mixed case, two disagree with the file extension
`include/pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp`:1 · standards
**Status: not addressed**

All 24 headers do have guards and no two collide, so the important property holds. The derivation rule is not applied consistently, though:

- `b_batch_delete.hpp:1-2` — `PBTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_`, mixed case in the middle (`B_Batch_DELETE`), unlike all 23 others.
- `dependence/box_op.h:1` — `PBTREE_DEPENDENCE_BOX_OP_HPP_` on a `.h` file.
- `tree_op/leaf_op.hpp:1` — `PBTREE_B_TREE_IMPL_LEAF_OP_HPP_`, missing the `TREE_OP_` path component that its three siblings (`node_op.hpp`, `flatten.hpp`) include.
- `dependence/binary_search.h:1` — `PBTREE_BINARY_SEARCH_H_`, missing `DEPENDENCE_`.
- `dependence/loggers.h` — guard is `PBTREE_DEPENDENCE_LOGGERS_H` but the closing comm

*Fix:* Adopt one rule — `PBTREE_<PATH_FROM_INCLUDE_ROOT>_<EXT>_` — and regenerate all 24 guards from it (`PBTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_HPP_`, `PBTREE_DEPENDENCE_BOX_OP_H_`, `PBTREE_B_TREE_IMPL_TREE_OP_LEAF_OP_HPP_`, `PBTREE_DEPENDENCE_BINARY_SEARCH_H_`), and make the `#endif` comment match. Or switch all 24 to `#pragma once`, which every supported compiler accepts and which `tests/wrapper.h

### BuildRecursive assigns this->root_ as a hidden side effect and is not recursive
`include/pab_tree/b_tree_impl/batch_op/b_build_tree.hpp`:103 · api-usability
**Status: not addressed**

```cpp
this->root_ =
  AssembleInteriorLevels(node_out.cut(0, bkt_n), key_out.cut(0, bkt_n - 1));
return this->root_;
```
The function both mutates `root_` and returns the same pointer; its only caller then assigns `root_` again (`Build_`, line 37). The two early-return branches (`In.size() == 0` and `In.size() < kBufUB`) do *not* set `root_`, so whether the member is touched depends on the input size. The function is also named `...Recursive` but contains no recursive call — the level-by-level loop lives in `AssembleInteriorLevels`.

*Fix:* Make it a pure factory and rename it: `static Node* BuildSubtree(Range In)`, with the two `this->root_` writes at lines 103-105 replaced by `return AssembleInteriorLevels(...)`. Update the declaration at b_tree.h:267 and the single call site at b_build_tree.hpp:37.

### RangeCount lacks the empty/inverted-box guard that RangeReport has
`include/pab_tree/b_tree_impl/query_op/range_count.hpp`:12 · api-usability
**Status: fixed**

`RangeReport` opens with `if (query_box.first >= query_box.second) [[unlikely]] { return { 0, RangeReportLogger{} }; }` (range_report.hpp:19-21). `RangeCount` has no such guard and walks from the root to a leaf before returning 0. The result is still correct — for `second <= first` the narrowed right-boundary search always yields `r == l` (because `split[l] > first >= second`), so the descent collapses to a single child and the leaf filters count nothing; verified empirically: `{500,500}` and `{900,500}` both give count 0 and report 0 (probe mode 4). But the two entry points behave differently for the same degenerate input, and neither documents what an inverted box means.

*Fix:* Add the same early return to `RangeCount`: `if (query_box.first >= query_box.second) [[unlikely]] { return { 0, RangeReportLogger{} }; }`, and state the half-open `[first, second)` convention plus the empty-box behaviour in a comment on both declarations in `b_tree.h:554` and `:566`.

### Dead query entry points: ContainRec, RangeReportRec, RangeReportFlattenRec, and an always-empty MembershipLogger
`include/pab_tree/b_tree_impl/query_op/range_report.hpp`:185 · dead-code
**Status: fixed**

`RangeReportRec` (line 185) and `RangeReportFlattenRec` (line 162) are public members that nothing calls — `RangeReport` uses `RangeReportIter`. `ContainRec` (membership.hpp:36) is likewise superseded by `ContainIter`. `RangeReportRec` is not merely redundant: it emits the left boundary, then the right boundary, then the middle subtrees (lines 219-227), so its output order differs from `RangeReportIter`'s, and its middle-child loop is serial. This is the same function that previously carried a 4-arg `FlattenRec` call that never compiled because it was never instantiated (per CLAUDE.md). Separately, `Contain` returns `std::pair<bool, MembershipLogger>` but the logger is default-constructed an

*Fix:* Delete `RangeReportRec`, `RangeReportFlattenRec` and `ContainRec` along with their declarations (b_tree.h:552, :583-594). Either populate `MembershipLogger` in `ContainIter` (`logger.vis_interior_num++` per level, `vis_leaf_num++` at the leaf — it must then take the logger by reference) or change `Contain` to `bool Contain(K const&) const` and keep the instrumented form as a separate `ContainLogge

### BalanceOverLeaves indexes group_sum with a kBufUB sentinel when no child is valid
`include/pab_tree/b_tree_impl/tree_op/node_op.hpp`:435 · correctness
**Status: not addressed**

```cpp
BufSizeT rpidx = TT::kBufUB;
BufSizeT pivot = TT::kBufUB;
for (int i = TI->GetNodeN() - 1; i >= 0; --i) { ... }
assert(rpidx != TT::kBufUB);
```

If no child is valid, `rpidx` and every `tree_group[i]` stay at the sentinel `TT::kBufUB`. The dispatch loop then executes `const SizeT sum = group_sum[g];` (node_op.hpp:588) with `g == TT::kBufUB`, but `group_sum` is `SizeBufUBArr` = `std::array<SizeT, kBufUB>` — a one-past-the-end read of a stack array. The `assert` on line 456 is the only guard, and it is compiled out in the default build (`-DNDEBUG`, CMakeLists.txt:96).

I traced the reachability and it is currently blocked: `BalanceOverLeaves` is called only from the `SelfBalanceNode(T,

*Fix:* Make the sentinel unrepresentable as an index and fail loudly. Use `static constexpr BufSizeT kNoPivot = std::numeric_limits<BufSizeT>::max();` for `rpidx`/`pivot`, and open `BalanceOverLeaves` with an unconditional guard rather than an assert: `if (TI->GetNodeN() == 0) return;` followed by, after the lambda, `if (rightmost_pivot_idx == kNoPivot) { /* caller violated the NodeIsValid precondition *

### GetEntries requires a caller-sized output and dereferences root_ unconditionally
`include/pab_tree/b_tree_impl/utility.hpp`:24 · api-usability
**Status: not addressed**

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

*Fix:* Add the null guard (`if (this->root_ == nullptr) return;`) and either resize in the function (`Out.resize(this->GetSize());` before the slice, since `Range&` is already non-const) or, better, add an allocating overload `auto GetEntries() const -> typename TT::EntrySeq` and document the existing one as requiring `Out.size() >= GetSize()`. Also rename or document the `sorted` template argument, whic

### ValidateKeyInNode and VerifyTreeProperty read split[0] when the node has a single child
`include/pab_tree/dependence/tree_node.h`:382 · correctness
**Status: fixed**

`ValidateKeyInNode` is `node_idx == 0 ? k < this->split.front() : (...)`. When `node_n == 1` (so `GetSplitN() == 0` and there is no live split key), `node_idx == 0` takes the first branch and reads `split[0]`, which is an indeterminate value in the default-constructed `SplitArr`. This function is called from every query descent's assertions — membership.hpp:28, range_report.hpp:112-114, range_count.hpp:93-95. `VerifyTreeProperty` has the same shape at validation.hpp:50 (`if (i == 0) assert(box.second < TI->split[0]);`), which for a one-child node compares against garbage. A one-child interior node is reachable: `FetchInvalidSubtreeNodes` compacts `TI->node_n = tree_node_idx` (node_op.hpp:355

*Fix:* Guard the degenerate case: `bool ValidateKeyInNode(BufSizeT node_idx, K k) const { if (GetSplitN() == 0) return node_idx == 0; ... }`, and in `VerifyTreeProperty` skip the split comparisons when `TI->GetNodeN() == 1` (the `BalanceOverLeaves` debug block at node_op.hpp:612 already does exactly this with `if (TI->GetNodeN() == 1) continue;`).

### `uint` is not a standard C++ type but appears in BTreeTypeTrait's template parameter list
`include/pab_tree/dependence/type_trait.h`:36 · standards
**Status: not addressed**

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

*Fix:* Change both template parameter declarations to `std::uint32_t kBufferSize` and both factors to `static constexpr std::uint32_t kBufMDFac = 2;` / `kBufUBFac = 4;` (also making them `constexpr` rather than `const`, matching `kBuf`/`kBufLB`/`kBufMD`/`kBufUB` on lines 124-127). Add `#include <array>` to type_trait.h.

## Outside include/pab_tree — 23

Excluded from the report per the scope you set; listed for completeness.

- **[critical]** `CMakeLists.txt` — Library is not installable or findable: no install(), no export, no Config.cmake, no namespaced alias
- **[critical]** `CMakeLists.txt` — All correctness checks are assert(), compiled out by -DNDEBUG in the default build
- **[critical]** `tests/unit_test.cpp` — unit_test executes zero tree code: argv built from dangling c_str() of temporaries
- **[high]** `.gitmodules` — `fast-cpp-csv-parser` submodule uses a `git@github.com:` SSH URL, breaking the README's own clone step
- **[high]** `CMakeLists.txt` — Default configure fails without jemalloc: `option(JEMA "" ON)` + `pkg_check_modules(JEMALLOC REQUIRED)`
- **[high]** `CMakeLists.txt` — CMakeLists mixes the library with the benchmark harness: 12 research drivers always built, no BUILD_TESTING/BUILD_BENCHMARKS
- **[high]** `CMakeLists.txt` — No CI, no sanitizer or coverage configuration, and the strict .clang-tidy is never executed
- **[high]** `README.md` — README has zero usage code, no API overview, and its one runnable command is broken
- **[high]** `README.md` — README documents requirements and build, but nothing about actually using the library
- **[high]** `tests/test_framework.h` — b_test's query path has its correctness comparison commented out, so the driver that actually runs validates nothing
- **[high]** `tests/unit_test.cpp` — The only CTest test hardcodes `/ssd1/zmen002/kv_pair`, so `ctest` cannot pass on any other machine
- **[high]** `tests/unit_test.cpp` — unit_test requires a hard-coded absolute dataset path and silently skips (reported as pass) without it
- **[medium]** `CMakeLists.txt` — CMakeLists.txt sets -w, hiding real warnings including a -Wreorder in both LeafNode constructors
- **[medium]** `CMakeLists.txt` — `-march=native` and forced CMAKE_BUILD_TYPE make the build non-portable and un-overridable
- **[medium]** `CMakeLists.txt` — Configure step executes a python script that writes `.clangd` into the source tree from a hardcoded `build/` path
- **[medium]** `CMakeLists.txt` — No release engineering: unused `VERSION 0.1`, no semver tag, no CHANGELOG, no version header
- **[medium]** `README.md` — No documentation of the extension points anywhere: no API reference, no usage example
- **[medium]** `README.md` — README has no API documentation at all — nothing describes how to instantiate BTreeTypeTrait or write an augmentation
- **[medium]** `scripts/gen_graph.sh` — Benchmark scripts and drivers hardcode the author's data volumes; no configurable data root
- **[medium]** `tests/test_framework.h` — No sanitizer, race-detector or fuzz configuration; the one memory-check build (-DMEMCHECK=ON) does not compile
- **[medium]** `tests/unit_test.cpp` — No edge-case coverage: smallest tested input is 1e6; empty tree, single element, delete-everything, duplicates and small kBufferSize are untested
- **[low]** `CMakeLists.txt` — Dead `CMAKE_MODULE_PATH` entry pointing at a nonexistent `cmake/` directory
- **[low]** `tests/unit_test.cpp` — unit_test's only failure vehicle is EXPECT_NO_THROW, which cannot observe the failures the library produces