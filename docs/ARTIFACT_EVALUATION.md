# Artifact Evaluation

The artifact for *Parallel B+-trees: Theory and Practice* (PPoPP 2027).

## Hardware Requirements

The paper's numbers were measured on four Intel Xeon Platinum 8176 CPUs (112
cores, 224 hyper-threads) with 1.47 TB RAM, compiled with GCC 15.2.0 at `-O3`.

- **Memory**: to be measured; the experiments build trees of 10^9 entries.
- **CPU**: all available cores. Fig. 9 sweeps the thread count.
- **Disk**: about 330 GB at 10^9 entries (see [Datasets](#datasets)).
- **Docker**, and permission to run containers.
- **Hardware counters** for Fig. 7: `perf` must be able to read them in a
  container (`--perf` below). Most virtual machines cannot.

**Before timing anything:** stop other jobs on the machine. Background load
slows parallel runs far more than the CPU share it takes.

## Getting Started (about 30 minutes)

Every synthetic experiment, at 10^6 entries instead of 10^9. The numbers are
too small to compare with the paper; this checks that everything builds and
runs.

```bash
./docker-run.sh pull        # or: ./docker-run.sh build
./docker-run.sh run --data-path /mnt/large-disk/pab-tree --input-size 1000000 --perf
# in the container:
cd scripts_ae && ./run.sh synthetic
```

Expected: `scripts_ae/plots/` holds `fig4_synthetic.csv`, `fig7_perf_cache.pdf`
and `fig9_scalability.pdf`, and the run ends with `done`. Without `--perf`,
Fig. 7 is skipped and listed in `scripts_ae/logs/failed.txt`.

## Full Evaluation

```bash
./docker-run.sh full --data-path /mnt/large-disk/pab-tree --perf
```

This runs `scripts_ae/run.sh` at 10^9 entries: it generates each synthetic
dataset just before the experiments that read it, deletes it afterwards, runs
every experiment, merges the logs into `scripts_ae/data/` and writes
`scripts_ae/plots/`: each figure as a PDF, Fig. 4 as a csv table. A step that
fails is listed in `scripts_ae/logs/failed.txt` and the rest go on;
`scripts_ae/logs/env.txt` records the machine, compiler and commit. Run it
inside `tmux` or `screen`: it takes many hours.

The experiments are those of the paper's body, Fig. 4 to 9; Fig. 1 and the
appendix (Fig. 10, Fig. 11, Table 1) are not part of the artifact.

Mass-tree and Bw-tree (Fig. 4, 5 and 9) are left out by default: they are
the slowest trees by far, and Bw-tree crashes now and then (a crashed run is
retried twice). Add `--with-masstree-bwtree` to run them too.

One figure at a time, in the container:

```bash
cd scripts_ae
./run.sh fig9 merge plot
```

| Paper | `run.sh` step | Script | Output in `scripts_ae/plots/` |
|---|---|---|---|
| Fig. 4 | `fig4` | `ae_run_incre_updates.sh` | `fig4_synthetic.csv` |
| Fig. 5 | `fig5` | `ae_run_osm.sh` | `fig5_osm.pdf` |
| Fig. 6 | `fig6` | `ae_run_temporal_graph.sh` | `fig6_temporal_graph.pdf` |
| Fig. 7 | `fig7` (needs `--perf`) | `ae_run_perf.sh` | `fig7_perf_cache.pdf` |
| Fig. 8 | `fig8` | `ae_run_randomness.sh` | `fig8_randomness.pdf` |
| Fig. 9 | `fig9` | `ae_run_scalability.sh` | `fig9_scalability.pdf` |

`./run.sh synthetic` runs Fig. 4, 7 and 9; `./run.sh real` runs Fig. 5, 6
and 8, which need the real-world datasets.

**Reading the results.** Absolute times depend on the machine; compare the
shape of each figure and the order of the trees with the paper. The titles,
labels and legends are the paper's. Fig. 4 is a table, in seconds. Fig. 5's
overflow and underflow shares come from numbers the paper's scripts hold.

## Parameters

`docker-run.sh`:

- `--data-path PATH` - Host directory for the datasets (required)
- `--input-size N` - Entries per synthetic dataset (default: 10^9)
- `--no-download` - Do not download the real-world datasets
- `--perf` - Allow hardware counters, for Fig. 7; the container runs as
  root
- `--with-masstree-bwtree` - Also run Mass-tree and Bw-tree
- `--cpus NUM`, `--memory SIZE` - Limit the container; usually left unset

`run.sh` takes the steps, and `--with-masstree-bwtree` as well. It and the
`ae_*.sh` scripts read these from the environment:

| Variable | Meaning | Default |
|---|---|---|
| `INPUT_SIZE` | Entries per synthetic dataset | `1000000000` |
| `ROUNDS` | Timed runs per measurement, after one warm-up | `3` |
| `THREADS` | Thread counts of Fig. 9 | 1, 4, 16, 32, 64 up to the cores, the cores, all hyper-threads |
| `WITH_MASSTREE_BWTREE` | `1` also runs Mass-tree and Bw-tree | `0` |
| `KEEP_DATA` | `1` keeps the generated datasets | delete |
| `RETRIES` | Reruns of a run that fails | `2` |

See the [Docker reference](DOCKER_QUICK_REFERENCE.md) for the rest.

## Datasets

`scripts_ae/ae_data_generate.sh` generates the synthetic data under
`<data-path>/synthetic`, with distinct 32-bit keys and values:

| Paper | Name | Each batch is drawn |
|---|---|---|
| Uniform | `uni` | uniformly from the keys not yet used |
| Skewed | `norm` | from normal(0.5, 0.1) over the keys not yet used |
| Adversarial | `seg` | uniformly from the leading 2m keys not yet used, for batches of m |

| Set | Used by | Size at 10^9 |
|---|---|---|
| `incre`: one stream per batch size, sliced into batches | Fig. 4, 7 | 192 GB |
| `single`: a tree and one batch of n/100 | Fig. 9 | 16 GB per distribution |

`run.sh` keeps one set at a time, so about 200 GB at once.

The real-world datasets go under `<data-path>/real_world`;
`scripts_ae/download_data.sh` fetches them (`docker-run.sh` runs it):

| Dataset | Used by | Size |
|---|---|---|
| OpenStreetMap | Fig. 5 | 28 GB |
| RED, REF, SEM, PLANET (GraphTide) | Fig. 6, 8 | 107 GB |

## Running on Other Data and Settings

- **Other sizes**: `--input-size` (or `INPUT_SIZE`) scales every synthetic
  dataset; `ROUNDS` and `THREADS` change the repetitions and the thread sweep.
- **Other keys**: the drivers read a binary file of entries: a `uint64`
  header `{N, 2, width}` (`width` 4 or 8 bytes), then `N` key-value pairs
  (`tests/binary_io.h`). `make_single_batch.py` shows the layout in a few
  lines. Point `-p` at your file, laid out as `<dir>/ins/<x>/1.base.bin` and
  `1.batch.bin` for an `x`-th of the tree per batch.
- **Other trees or leaf sizes**: `ae_run_*.sh` call one driver per tree
  (`b_test` is PaB-tree; `-T` picks the baseline); `run_tree` in
  `scripts_ae/common.sh` builds and runs one, and the build configuration
  `leaf<N>` compiles PaB-tree with leaves of N entries.
- **Other counters**: `EVENTS` in `ae_run_perf.sh` lists the perf events;
  `tests/perf_cache.cpp` holds the counted regions.
- **The library itself**: see the [manual](MANUAL.md).
