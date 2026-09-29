# A new batch deletion algorithm.

This algrorithm is a slightly modification of the reinsertion algorithm in c937b45f. The difference is that we do not flatten the invalid tree and reinsertion, but to insert the valid subtrees of invalid subtrees into the correspodning positions of the new tree.

## Geneneral idea:
- First sieve the data to the corresponding leaf in parallel. 
- if a leaf is invalid, try to rebalance it with its siblings first.
- Merge back the whole tree and update the information of the new tree,
- traverse the trees again and for those invalid subtrees, collect all their valid subtrees.
- All such valid subtrees from all invalid subtrees are stored in an array and group by their height. Then we insert all nodes in same level as a batch into the corresponding positions of the new tree, using the similar algorithm in batch insertion (the only difference is now we will insert the pointers).
- The reinsertion starts from nodes in the leaf level (the entries in the underfull leaves) and goes up to upper levels, the reason to insert lower level first is that it may overflow to the upper level.

## Implementation idea:
- avoid unnecessary memory allocation such as parlay::tabulate during the recursion, pre-alloc the memory before the recursion (collect the valid subtrees) and reuse it.
- Group valid subtrees by height can be achieved using parlay::sort, or maybe we can use a more efficient way to do it, such as organize into corresponding position in array during the collection.



