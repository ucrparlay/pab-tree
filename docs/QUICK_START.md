# Quick Start

## With Docker
```bash
./docker-run.sh pull                        # or: ./docker-run.sh build
./docker-run.sh run --data-path /your/data/path
```

Inside the container, build and test:
```bash
cmake -B build .
cmake --build build -j
ctest --test-dir build
```

See the [Docker manual](DOCKER_QUICK_REFERENCE.md) for complete details.

## Manual Setup
### Requirements

Necessary:

- Linux, `cmake` >= 3.15
- A C++20 compiler (GCC or Clang). Tested with GCC 15.2.1
  (`gcc-toolset-15`) on Rocky Linux 8.10, as in the Docker image.
- ParlayLib and the other dependencies, provided as submodules

Optional:

- [jemalloc](https://github.com/jemalloc/jemalloc) (`-DJEMA=ON`, the
  default). Without it, configure with `-DJEMA=OFF`; with `ON` the configure
  step requires it.
- [numactl](https://manpages.ubuntu.com/manpages/trusty/man8/numactl.8.html),
  which the benchmark scripts use to interleave memory across sockets.
- tcmalloc (gperftools), PAPI, TBB and libatomic, for the Mass-tree and
  Bw-tree baselines (target `index_microbench_workload`).

## Getting Code

1. Clone the repository.

```bash
git clone https://github.com/ucrparlay/pab-tree.git
cd pab-tree
```

2. Initialize the submodules:

```bash
git submodule update --init --recursive
```

3. Build (`-DJEMA=OFF` unless jemalloc is installed):

```bash
cmake -B build -DJEMA=OFF .
cmake --build build -j
```

4. Check it works:

```bash
ctest --test-dir build      # self_test needs no data; unit_test skips without its datasets
```

For more detailed usage, see the [manual](MANUAL.md).
