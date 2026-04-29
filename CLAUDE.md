# ParallelMultiwayOrderedSet — Project Knowledge

## Working conventions (for Claude)

- **Keep this file updated.** After any session where new files are read, new functions are understood, or new design decisions are uncovered, append or update the relevant section here. This file is the persistent knowledge store for this project and travels across machines.

- **Check `.claude/` for memory.** At the start of each session, also check `.claude/MEMORY.md` and the memory files under `.claude/projects/` for additional persistent context (user preferences, feedback, project decisions) that supplements this file.


- Don't need to run the unit_test after the updates. just make sure the compilation is passed.

## What this project is

A parallel, cache-oblivious B-tree (`pmos::BTree<TypeTrait>`) that supports parallel batch insert, batch delete, and range queries. It lives entirely in `include/pb_tree/`.

## Build

```bash
cd build && make <target> -j$(nproc)
# targets: unit_test, b_test, pma, cpam, ...
```

Build is CMake-based. The `build/` directory already exists; no need to re-run cmake.

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
using BufSizeT = int;             // node/slot counts (signed! backward loops use i >= 0)
using SizeBufUBArr    = std::array<SizeT,    kBufUB>;
using BufSizeBufUBArr = std::array<BufSizeT, kBufUB>;
using BooleanBufUBArr = std::array<bool,     kBufUB>;
using EntryArr = std::array<E, kBufUB>;   // fixed-size leaf element buffer
```

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

## Concrete element type in tests

Default: `Wrapper<uint32_t, uint32_t>::Element` → key=uint32_t (4B), val=uint32_t (4B) → E is 8 bytes.
OSM variant: key=uint64_t.
Dynamic graph: key=tuple<uint32_t,uint32_t,uint32_t>.
