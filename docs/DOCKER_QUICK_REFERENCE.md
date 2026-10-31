# Docker Quick Reference

## Quick Start - Using the Prebuilt Image (Recommended)

```bash
# 1. Pull the image
./docker-run.sh pull

# 2. Open a shell in the container (requires --data-path)
./docker-run.sh run --data-path /mnt/large-disk/pab-tree

# 3. Or run every experiment (--perf for Fig. 7)
./docker-run.sh full --data-path /mnt/large-disk/pab-tree --perf
```

The image holds the toolchain and the source, but no binaries. Everything is
compiled inside the container, on the machine that runs it, with
`-march=native`, so the numbers are those of that machine.

## Alternative - Build Locally

If you want to change the code or cannot reach the prebuilt image:

```bash
./docker-run.sh build
./docker-run.sh run --data-path /mnt/large-disk/pab-tree
```

The image copies the checkout as it is, so initialize the submodules first
(`git submodule update --init --recursive`).

## Commands

| Command | What it does |
|---|---|
| `build` | Build the image from this checkout |
| `pull` | Pull the prebuilt image |
| `run` | Start a shell in the container |
| `full` | Run every experiment (`scripts_ae/run.sh`) |
| `shell` | Open another shell in the running container |
| `stop` | Stop the running container |
| `clean` | Remove the results in `scripts_ae/` and the image |

## Options

For `run` and `full`:

| Option | Description | Default |
|---|---|---|
| `--data-path PATH` | Host directory for the datasets, mounted at `/data` (required) | - |
| `--input-size N` | Entries per synthetic dataset | `1000000000` |
| `--no-download` | Do not download the real-world datasets | download |
| `--perf` | Allow hardware counters (Fig. 7); runs as root | off |
| `--with-masstree-bwtree` | Also run Mass-tree and Bw-tree (Fig. 4, 5, 9) | off |
| `--cpus N` | Limit the CPUs | all |
| `--memory SIZE` | Limit the memory, e.g. `512g` | none |

`PAB_TREE_IMAGE=name ./docker-run.sh ...` uses another image.

## What Is in the Image

| Component | Version |
|---|---|
| Base | Rocky Linux 8.10 |
| Compiler | GCC 15.2.1 and binutils 2.44 (Red Hat's `gcc-toolset-15`) |
| Build | CMake 3.26, GNU Make |
| Libraries | jemalloc; tcmalloc, PAPI, TBB and libatomic for Mass-tree and Bw-tree |
| Scripts | Python 3.12 (standard library only); R 4.5 with dplyr, tidyr, ggplot2, scales, ggsci, gtable, latex2exp, ggpubr and zoo; the Linux Libertine font |
| Tools | numactl, perf, GNU time, bc |

## Manual Docker Commands

What `docker-run.sh run` does, without the helper:

```bash
docker pull ghcr.io/ucrparlay/pab-tree:latest

docker run -it --rm \
  --cap-add=SYS_NICE \
  -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v /mnt/large-disk/pab-tree:/data \
  -v "$PWD/scripts_ae/data:/workspace/pab-tree/scripts_ae/data" \
  -v "$PWD/scripts_ae/logs:/workspace/pab-tree/scripts_ae/logs" \
  -v "$PWD/scripts_ae/plots:/workspace/pab-tree/scripts_ae/plots" \
  ghcr.io/ucrparlay/pab-tree:latest
```

`--cap-add=SYS_NICE` lets `numactl -i all` interleave memory across sockets;
`-u` keeps the results yours. Create the three `scripts_ae/` directories
first, or Docker creates them as root.

## Results Location

Relative to the repository root:

| Type | Location |
|---|---|
| Datasets | the host path given to `--data-path` |
| Summarized results (csv) | `scripts_ae/data/` |
| Logs | `scripts_ae/logs/` |
| Figures (pdf) and Fig. 4 (csv) | `scripts_ae/plots/` |

## Hardware Counters (Fig. 7)

Reading CPU counters with `perf` inside a container takes more than the
default privileges. `--perf` runs the container as root with `SYS_ADMIN` and
without the seccomp filter; when it exits, `docker-run.sh` hands
`scripts_ae/` back to you. Files it creates under `--data-path` stay root's.

```bash
./docker-run.sh run --data-path /mnt/large-disk/pab-tree --perf
```

## Using Docker Compose

```bash
cp .env.example .env    # set DATA_PATH, AE_UID (id -u), AE_GID (id -g)
mkdir -p scripts_ae/data scripts_ae/logs scripts_ae/plots
docker compose run --rm pab-tree-ae
```

Compose does not download the real-world datasets; `docker-run.sh` does.

## Cleanup

```bash
# Stop the container, remove scripts_ae/{data,logs,plots} and the image
./docker-run.sh clean
```

Datasets under `--data-path` are left alone; delete them by hand.

## Troubleshooting

**`numactl: ... Operation not permitted`**
The container lacks `SYS_NICE`. `docker-run.sh` adds it; add
`--cap-add=SYS_NICE` to a hand-written `docker run`.

**`perf` reports `<not supported>` or permission denied**
Rerun with `--perf`. In a virtual machine that does not expose the CPU's
counters, Fig. 7 cannot run.

**Files in `scripts_ae/` belong to root**
A container ran as root: a hand-written `docker run` without `-u`, or a
`--perf` run that was killed. `./docker-run.sh clean` removes them.

**Out of memory**
Lower `--input-size`.

**Changes to the code do not show up**
The image holds a copy of the checkout. Rebuild it: `./docker-run.sh build`.
