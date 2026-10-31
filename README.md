# PaB-tree: Parallel B+-trees

[![docker](https://img.shields.io/github/actions/workflow/status/ucrparlay/pb-tree/docker-build.yml?branch=main&logo=docker&logoColor=white&label=docker&labelColor=0D1117)](https://github.com/ucrparlay/pb-tree/actions/workflows/docker-build.yml)
[![C++20, header-only](https://img.shields.io/badge/C%2B%2B-20%20%C2%B7%20header--only-0F80FF?logo=cplusplus&logoColor=white&labelColor=0D1117)](CMakeLists.txt)

PaB-tree is a parallel, in-memory B+-tree built for batch updates, which is:
- 🚀 Fast: up to 1.4× faster on construction and 3.6× faster on batch insertion than the strongest baseline, on billions of entries,
- 🔀 Highly parallel: batch updates have polylogarithmic span, and construction scales 75.6× on 112 cores (224 hyper-threads),
- 💾 Cache-efficient: `O(log_B n)` I/O per updated entry, the bound of a sequential B+-tree,
- 🎳 Complete: build, batch insert and delete, membership, range count and report, and a `std::set`-like interface,
- 🛠️ Easy to adapt: header-only C++20 on [ParlayLib](https://github.com/cmuparlay/parlaylib), with configurable leaf size, comparator, augmentation and allocator.

## Docs
- [Quick Start](docs/QUICK_START.md)
- [User Manual](docs/MANUAL.md)
- [Docker](docs/DOCKER_QUICK_REFERENCE.md)
- [Artifact Evaluation](docs/ARTIFACT_EVALUATION.md)

## Citation

## License

PaB-tree is MIT licensed — see [LICENSE](LICENSE).

Have a good day! ☀️
