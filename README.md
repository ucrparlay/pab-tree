# PaB-Tree: A fast in-memory parallel B+-tree.

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
