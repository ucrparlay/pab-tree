# A new batch deletion algorithm.

This algrorithm is a slightly modification of the reinsertion algorithm in c937b45f. The difference is that we do not flatten the invalid tree and reinsertion, but to insert the valid subtrees of invalid subtrees into the correspodning positions of the new tree.

Geneneral idea:
- First sieve the data to the corresponding leaf in parallel. 
- if a leaf is invalid, try to rebalance it with its siblings first.
- Merge back the whole tree and update the information of the new tree,
- traverse the trees again and for those invalid subtrees, collect all their valid subtrees. All such valid subtrees are stored in an array and group by their height. Then we insert all nodes in same level as a batch into the corresponding positions of the new tree, and handle the overflow like in batch insertion. 
- For invalid leaves we just collect all their entries and call the batch insertion


