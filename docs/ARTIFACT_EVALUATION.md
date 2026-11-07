# Artifact Evaluation

The artifact for *PaB-tree: Parallel B+-tree with Batch Updates* (PPoPP 2027).

## Hardware Requirements

The paper's numbers were measured on four Intel Xeon Platinum 8176 CPUs (112 cores, 224 hyper-threads) with 1.47 TB RAM, compiled with GCC 15.2.0 at `-O3`.

- **Memory**: At least 512GB RAM, preferable 1T.
- **CPU**: all available cores, preferable > 128 cores.
- **Disk**: At least 512 GB under `--data-path`.
- **Docker**, and permission to run containers.

**Before timing anything:** stop other jobs on the machine. Background load slows parallel runs far more than the CPU share it takes.

**Warning**: The scripts for results merging and figures plotting may not execute properly if a baseline failed to generate the log and exit correctly, i.e., the resource requested exceeds the RAM. Please make sure the machine where the artifact is evaluated satisfied above requirements. Thanks.

## Getting the Code

```bash
git clone --recursive https://github.com/ucrparlay/pab-tree.git && cd pab-tree
```

`--recursive` fetches the submodules under `external/`, which `./docker-run.sh build` needs; after a plain clone, run `git submodule update --init --recursive`.

This artifact is archived on [Zenodo](https://zenodo.org/records/24044003) with doi `10.5281/zenodo.24044003`.

## Getting Started Test (about 30 minutes)

The getting start test run every synthetic dataset with size $10^6$, instead of the $10^9$ used in the paper. This checks that everything builds and runs.

```bash
./docker-run.sh pull        # or: ./docker-run.sh build
./docker-run.sh run --data-path /path-to-disk --input-size 1000000 --no-download --perf
# in the container:
cd scripts_ae && ./run.sh synthetic
```

Expected: `scripts_ae/plots/` holds `fig4_synthetic.csv`, `fig7_perf_cache.pdf` and `fig9_scalability.pdf`, and the run ends with `done`. Without `--perf`, Fig. 7 is skipped and listed in `scripts_ae/logs/failed.txt`.

Before the full evaluation, remove the test's results on the host, so none of them reach the full run's figures (not in the docker, in the server console): `rm -rf scripts_ae/{data,logs,plots}`.

## Full Evaluation
The baselines in the paper evaluated are in two categorizes. The first one are the parallel baselines, including the CPAM, PAM, PMA. Our PaB-tree is a parallel baseline as well. The second group are concurrent baselines, such as the Mass-tree and Bw-tree.

### Running Parallel Baselines
```bash
./docker-run.sh full --data-path /path-to-disk --perf
```

This downloads the real-world datasets, then runs `scripts_ae/run.sh` at 10^9 entries: it generates each synthetic dataset just before the experiments that read it, runs every experiment, merges the logs into `scripts_ae/data/` and writes `scripts_ae/plots/`: each figure as a PDF, Fig. 4 as a csv table. A step that fails is listed in `scripts_ae/logs/failed.txt` and the rest go on; `scripts_ae/logs/env.txt` records the machine, compiler and commit.

Please run it inside the `tmux` or other persistent session: it takes about 11 hours on the paper's machine, plus the 88 GB download (about 30 minutes at 55 MB/s).

### Running the Concurrent Baselines
The Mass-tree and Bw-tree (Fig. 4, 5 and 9) are left out by default in above commands: they are the slowest trees by far (about 15 more hours, 12 of them in Fig. 9). The original implementation is fragile and contains flaws, we try to make it work on our servers but cannot guarantee it work on all machines.

To run them, add `--with-masstree-bwtree` in the `run.sh`. For example, after a full run of parallel baselines,
```bash
./docker-run.sh run --data-path /path-to-disk --perf
./run.sh --only-masstree-bwtree
```
It runs Fig. 4, 5 and 9 for Mass-tree and Bw-tree alone and keeps the other trees' logs, so the merged figures show every tree.

## Reading the Results

To redraw the figures from the existing logs, without rerunning any experiment:

```bash
cd scripts_ae
./run.sh merge plot
```

The experiment scripts and figures are listed below, with each step's time in a full run on the paper's machine.

| Paper | `run.sh` step | Script | Output in `scripts_ae/plots/` | Time |
|---|---|---|---|---|
| Fig. 4 | `fig4` | `ae_run_incre_updates.sh` | `fig4_synthetic.csv` | 2 h 45 min |
| Fig. 5 | `fig5` | `ae_run_osm.sh` | `fig5_osm.pdf` | 12 min |
| Fig. 6 | `fig6` | `ae_run_temporal_graph.sh` | `fig6_temporal_graph.pdf` | 44 min |
| Fig. 7 | `fig7` (needs `--perf`) | `ae_run_perf.sh` | `fig7_perf_cache.pdf` | 9 min |
| Fig. 8 | `fig8` | `ae_run_randomness.sh` | `fig8_randomness.pdf` | 11 min |
| Fig. 9 | `fig9` | `ae_run_scalability.sh` | `fig9_scalability.pdf` | 5 h 55 min |

`./run.sh synthetic` runs Fig. 4, 7 and 9; `./run.sh real` runs Fig. 5, 6 and 8, which need the real-world datasets. `./run.sh fig9 merge plot` reruns Fig. 9 alone.

**Reading the results.** Times depend on the machine; please compare the shape of each figure and the order of the trees with the paper. The titles, labels and legends are the paper's. Fig. 4 is a table presented in a CSV. Fig. 5's overflow and underflow shares come from numbers the paper's scripts hold since their measurement needs to manually edit the source code. N.A. in a csv or table is a number a run did not print: it failed (listed in `logs/failed.txt`), or was stopped.

## Cleanup
In the host:
```bash
docker run --rm -v [/mnt/large-disk/pab-tree]:/data ghcr.io/ucrparlay/pab-tree rm -rf /data/real_world /data/synthetic
./docker-run.sh clean
```

The first command deletes the datasets under `--data-path`, downloaded and generated; it runs in a container because a `--perf` run leaves them owned by root. The second stops the container and removes the image and the results in `scripts_ae/{data,logs,plots}`: copy what you want to keep first.

## Parameters

For `docker-run.sh`:

- `--data-path PATH` - Host directory for the datasets (required)
- `--input-size N` - Entries per synthetic dataset (default: 10^9)
- `--no-download` - Do not download the real-world datasets; Fig. 5, 6 and 8 will run only if their data is already there
- `--perf` - Allow hardware counters, for Fig. 7; the container runs as root
- `--with-masstree-bwtree` - Also run Mass-tree and Bw-tree `--cpus NUM`, `--memory SIZE` - Limit the container; usually left unset

`run.sh` will run the experiments one-by-one. 

In default it will not run the concurrent indexes, use `--with-masstree-bwtree` or `--only-masstree-bwtree` to include or only run for them.
It run the `ae_*.sh` scripts read these from the environment:

| Variable | Meaning | Default |
|---|---|---|
| `INPUT_SIZE` | Entries per synthetic dataset | `1000000000` |
| `ROUNDS` | Timed runs per measurement, after one warm-up | `3` |
| `THREADS` | Thread counts of Fig. 9 | 1, 4, 16, 32, 64 up to the cores, the cores, all hyper-threads |
| `WITH_MASSTREE_BWTREE` | `1` also runs Mass-tree and Bw-tree | `0` |
| `KEEP_DATA` | `0` deletes each generated dataset after the last step that reads it | `1` |
| `RETRIES` | Reruns of a Mass-tree or Bw-tree run that fails or times out | `2` |
| `RUN_TIMEOUT` | Time limit of a Mass-tree or Bw-tree run (`timeout` duration; `0` for none) | `2h`; Fig. 9: 8 h / threads, at least 1 h |

See the [Docker reference](DOCKER_QUICK_REFERENCE.md) for the rest.

## Datasets

`scripts_ae/ae_data_generate.sh` generates the synthetic data under `<data-path>/synthetic`, with distinct 32-bit keys and values:

| Paper | Name | Each batch is drawn |
|---|---|---|
| Uniform | `uni` | uniformly from the keys not yet used |
| Skewed | `norm` | from normal(0.5, 0.1) over the keys not yet used |
| Adversarial | `seg` | uniformly from the leading $2m$ keys not yet used, for batches of $m$ |

| Set | Used by | Size at 10^9 | Time to generate |
|---|---|---|---|
| `single`: a tree and one batch of n/100 | Fig. 9 | 16 GB per distribution | about 1 min per distribution |
| `incre`: a single dataset sliced into batches | Fig. 4, 7 | 96 GB | 53 min |


The real-world datasets go under `<data-path>/real_world`. `scripts_ae/download_data.sh` fetches them from [romaLzhih/pab-tree-data](https://huggingface.co/datasets/romaLzhih/pab-tree-data) and checks them against `scripts_ae/datasets.sha256` (`docker-run.sh` runs it). No account is needed.

| Dataset | Used by | Size |
|---|---|---|
| OpenStreetMap | Fig. 5 | 28 GB |
| RED, REF, SEM, PLANET (GraphTide) | Fig. 6, 8 | 61 GB |


