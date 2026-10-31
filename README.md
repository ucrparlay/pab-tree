# PaB-tree: Parallel B+-trees with Batch Updates

[![docker](https://img.shields.io/github/actions/workflow/status/ucrparlay/pab-tree/docker-build.yml?branch=main&logo=docker&logoColor=white&label=docker&labelColor=0D1117)](https://github.com/ucrparlay/pab-tree/actions/workflows/docker-build.yml)
[![C++20, header-only](https://img.shields.io/badge/C%2B%2B-20%20%C2%B7%20header--only-0F80FF?logo=cplusplus&logoColor=white&labelColor=0D1117)](CMakeLists.txt)

PaB-tree is a parallel, in-memory B+-tree built for batch updates, which is:
- 🚀 **Blazingly Fast**: faster than all concurrent baselines and even faster than the SOTA parallel BST.
- 🔀 **Highly parallel and scalable**: scales to hundreds of cores and billions of inputs.
- 💾 **Theoretical guaranteed**: the first parallel b+-tree with optimal time and I/O bounds.
- 🎳 **Complete**: build, batch updates, various queries; all you need is here.
- 🛠️ **Easy to adapt**: header-only C++20 on [ParlayLib](https://github.com/cmuparlay/parlaylib), with configurable parameters.

## Docs
- [Quick Start](docs/QUICK_START.md)
- [User Manual](docs/MANUAL.md)
- [Docker](docs/DOCKER_QUICK_REFERENCE.md)
- [Artifact Evaluation](docs/ARTIFACT_EVALUATION.md)

## Citation

## License

PaB-tree is MIT licensed — see [LICENSE](LICENSE).

Have a good day! ☀️
