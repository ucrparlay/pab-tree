# PMOS: A library for Parallel Multiway Ordered Sets

## Requirements

Necessary:

- CMake >= 3.15
- g++ or clang++ with C++20 features support (Tested with g++14 and clang-19) on Linux machines.
- We use [ParlayLib](https://github.com/cmuparlay/parlaylib) to support fork-join parallelism and some parallel primitives. It is provided as a submodule in our repository.

Optional:

- [jemalloc](https://github.com/jemalloc/jemalloc), slightly memory allocation improvement.
- [NUMA control](https://manpages.ubuntu.com/manpages/trusty/man8/numactl.8.html), improve the performance for parallelism.

## Getting Code

Try:

1. Clone the repository.

```{bash}
git clone https://github.com/ucrparlay/ParallelMultiwayOrderedSet
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


