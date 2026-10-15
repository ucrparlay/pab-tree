# One at a time: ours vs tlx vs std::set

Insert the first 1e7 entries of a uniform batch one at a time, in input
order, into a built tree; then delete the same keys one at a time. Leaf
64, `numactl -i all`, quiet machine, 2026-10-14. Seconds, mean of 2 runs;
both runs where they differ a lot. Ours links jemalloc; tlx and std::set
do not, as in the harness. Raw output: `uni_1e9.out`, `uni_1e8.out`.

## 1e9 tree (`kv_pair/uni/1000000000_1/ins/1`)

| structure | insert | delete |
|---|---|---|
| ours, default leaf policy | 13.3 | 14.0 |
| tlx | 19.3 | 18.8 |
| std::set | 39.8 | 41.9 |

## 1e8 tree (`kv_pair/uni/100000000_1/ins/10`)

| structure | insert | delete |
|---|---|---|
| ours, default leaf policy | 12.0 | 10.3 |
| tlx | 11.7 | 11.8 |
| std::set | 26.2 | 25.6 |

- 1e9: ours about 1.3× faster than tlx, about 3× faster than std::set.
  1e8: ours level with tlx, 2.2–2.5× faster than std::set.
- The ~3 s (std::set) and ~1 s (tlx) in `scripts/logs/summary_serial` are
  1e6 entries into a 1e8 tree. Insert / delete there: std::set 2.9 / 2.5 s,
  tlx 1.14 / 1.13 s; here: std::set 2.6 / 2.5 s, tlx 1.15 / 1.13 s, ours
  default 1.0 / 0.94 s.
- std::set at 1e9 was built from sorted input to save time, which helps it
  if anything. tlx was built by the harness's insert loop (26 min).
- Ours default: the second insert run was slower (15.5 vs 13.3 s); cause
  not checked.
- Unsorted leaves skip the search inside the leaf: inserts 2–4× faster,
  deletes about 1.2× faster; lookups then scan the leaf.
