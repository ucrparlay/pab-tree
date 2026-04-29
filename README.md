# PB-Tree: A fast in memory parallel B+-tree.

## Requirements

Necessary:

- CMake >= 3.15
- g++ with C++20 features support on Linux machines.
- We use [ParlayLib](https://github.com/cmuparlay/parlaylib) to support fork-join parallelism and some parallel primitives. It is provided as a submodule in our repository.

Optional:

- [jemalloc](https://github.com/jemalloc/jemalloc), slightly memory allocation improvement.
- [NUMA control](https://manpages.ubuntu.com/manpages/trusty/man8/numactl.8.html), improve the performance for parallelism.

## Getting Code

Try:

1. Clone the repository.

```{bash}
git clone https://github.com/ucrparlay/pb-tree
cd ParallelMultiwayOrderedSet
```

2. Initialize the submodule:

```bash
git submodule update --init --recursive
```

## Compilation

```bash
mkdir build && cd build
cmake -DDEBUG=OFF -DJEMA=OFF ..
make
```

## Generate data

```bash
./scripts/gen_graph.sh [path_for_data]
```


## Test 
To test a baseline with exp distribution, e.g., PMA, try:
```{bash}
./build/baselines -p [path_to_data]/exp/50000000_1/ins/100/1.base.in -d 1 -t 1 -r 4 -q 1 -s 0 -b 0.01 -T 3 -D exp
```

This will run the tree construction for 4 times, and batch insertion for 4 times, then the membership query.

Code Structure:
- Use -DDEBUG=ON in cmake to enable the debug. The extra compile commands for PMA is in the [CMakeList.txt](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/CMakeLists.txt#L131).
- The PMA is organized in a separate [header](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/external/baselines/pma.h). The [map_init](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/external/baselines/pma.h#L56) and [map_insert](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/external/baselines/pma.h#L75) will call the build and insertion in PMA. After each build/insert, the [delete_tree](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/external/baselines/pma.h#L115) will be used to reset the status of PMA. The input is std::pair<parlay::sequence<K>, and parlay::sequence<V>>, controlled by the [ParseInput](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/external/baselines/pma.h#L44) function.
- There is a [test_framework](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/tests/test_framework.h#L1175), with interface [BuildTree](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/tests/test_framework.h#L220) and [BatchInsert](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/tests/test_framework.h#L260) to call the API defined in the PMA header above. You can pass a [customized test function](https://github.com/ucrparlay/ParallelMultiwayOrderedSet/blob/main/tests/baselines.cpp#L9) as well. 
