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
for uniform, which ignores them.

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

## Cache-miss profiling (`tests/perf_control.h` + `scripts/perf_cache.sh`)

Measures hardware counters (cache misses etc.) over **one call** of `Build`,
`BatchInsert` or `BatchDelete`, rather than over the whole process. Without
gating, ~73% of the counts are setup: reading the input file, the dedup check in
`ReadEntries`, and the untimed setup `Build` that precedes every
`BatchInsert`/`BatchDelete` (measured at 1e6: 1.19B instructions ungated vs
321M for the build region alone).

### Mechanism

`perf` is launched with `-D -1` (start with events disabled) and
`--control fd:<ctl>,<ack>` over two fifos the script opens read-write. The
binary writes `enable`/`disable` on the control fd and drains the `ack\n` reply.
Verified present in this box's perf (4.18.0-553 / RHEL8) — both
`perf stat --control` and `perf record --control` support it.

- `PerfControl` (`tests/perf_control.h`) holds the two fds + the armed region
  name; every entry point is a **no-op when either fd is 0**, so ordinary
  timing runs are byte-for-byte unaffected.
- `PerfRegion("name")` is a scope guard placed **inside the timed lambda of
  `time_loop`** — that placement is what excludes the untimed `initf`
  (`pkd.Build`) and `endf` (`pkd.DeleteTree`) phases.
- Three regions exist: `"build"` (`BuildTree`), `"insert"` (`BatchInsert`),
  `"delete"` (`BatchDelete`).

### CLI flags (`options.h`)

| Flag | Meaning |
|---|---|
| `-pcf` | perf control fd (0 = not under perf) |
| `-pcaf` | perf ack fd |
| `-pcr` | which region to count: `build`/`insert`/`delete`; empty = all |

**`-pcr` is not optional in practice.** A `-t 1` run executes the unconditional
top-of-function `BuildTree` *and* `BatchInsert`, both guarded; perf accumulates
over every enabled interval, so without `-pcr` it reports build+insert summed.

### Two hard requirements on the run

- **`-r 1`.** `time_loop` enters the timed region `rounds` times *plus* at least
  one warmup round (`loop_late = 0.01` when `rounds > 1`), and perf sums them
  all. `Run()` prints a stderr warning if `-r != 1` under perf.
- **`-s 0`.** `-s 1` sweeps 4 batch ratios through the same region.

`-q 0` too, otherwise the `query_type != 0 && tag != 0` block runs another
*counted* `BuildTree` (it passes `kTestTime = true`).

### Event budget on this box (Xeon Silver 4314, Icelake-SP)

`cycles,instructions,cache-references,cache-misses,LLC-load-misses,L1-dcache-load-misses`
fits exactly: 2 fixed + 4 general-purpose counters, all reported at
`100.00%` enabled. A 7th event starts multiplexing and every count silently
becomes an extrapolation — check field 5 of the `-x,` output before trusting a
changed `EVENTS`.

### Serial vs parallel

`THREADS=1` is the meaningful setting for cache-obliviousness work. At
`THREADS=64` idle parlay workers spinning in `scheduler::get_own_job` are
counted too: the same 1e5-element insert measured 57.6M instructions serial vs
785M at 64 threads (13.6x), and cache misses 109K vs 247K (2.3x).

### `MODE=record` sample rate

`perf record` at its default period gives ~55 cache-miss samples for a ~20ms
region — useless for ranking functions. The script passes `-F 20000`
(`FREQ`), which raised it to 269; widen the region (larger `n` or `-b 1`)
until `# Samples` is in the thousands. Release builds have **no `-g`**
(CMakeLists.txt:90 is `-O3 -march=native`), so `perf report` resolves symbol
names from `.symtab` but gives no line numbers, and `-O3` inlining folds the
small leaf helpers into their callers.

### Measured at 1e8 uni, 64 threads, `-b 0.1` (misses per element)

| region | elements touched | cache-misses | per element | miss rate |
|---|---|---|---|---|
| build | 1e8 | 102.0M | 1.02 | 51.8% |
| insert | 1e7 | 20.0M | 2.00 | 60.7% |
| delete | 1e7 | 17.2M | 1.72 | 56.9% |

Insert/delete cost ~2x the misses per element that build does — build streams,
insert/delete chase scattered leaves.

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

Per ratio: 8 columns = 1 timing + 1 membership + 3 range query + 3 range count
(`kRangeQueryType = 3`). Masstree emits `-` for all 3 range counts, so column
count stays fixed. It reports all 3 range queries as of 2026-07-28 — see the
`scan` stack-array fix below.

Whole-run totals (verified 2026-07-28 at n=2000, both drivers, all index types):
tag `1<<5` → 8 tokens; tags `1<<6` and `1<<7` → 1 primal build + 4 ratios × 8 =
33 tokens each.

The primal build is **always** run and always a real number, in both drivers —
`workload.cpp` used to skip it and print `-` when only the incremental bits were
set. Restoring it costs masstree a full 98GB / ~66s index at 1e9 that tags
`1<<6`/`1<<7` immediately release; it is paid only to keep that column
comparable against `b_test`.

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

**Range count is still skipped for masstree, and it is a different problem.**
`MassTreeIndex::range_count` is cursor-based and cannot crash, but
`dynamic_get_next` (`mtIndexAPI.hh:198`) descends from the root *and builds a
fresh `Json` request for every key* — rec2 would mean ~2.5e10 descents plus
2.5e10 Json allocations. It would run for hours, not fail. Making that column
work needs a counting scanner built on `get_next_n`'s callback, not a flag flip.
(The old comment there claimed "segfaults", copy-pasted from the range query
skip; it was never true for the count path.)
