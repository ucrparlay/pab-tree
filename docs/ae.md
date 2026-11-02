This repository will be used as an artifact evaluation (ae) for the paper @docs/btree_paper.pdf, to the conference ppopp 2027 https://ppopp27.sigplan.org/track/PPoPP-2027-artifact-evaluation.

The goal is to make the experiments results from ae can matches the ones in the paper. All bagdes are needed.

The ae should follow the style in the @/home/zmen002/SpaceTreeLib :
- The readme should follow the style in the @/home/zmen002/SpaceTreeLib as well
- use the docker to provide the environment for the artifact evaluation
- the scritps we used to run the experiments is in the scripts folder, the scripts we plot is in the @~/btree_paper/figures/data-plots/plots/plot_scripts
- we will use the Rocky Linux 8.10 as the base image for the docker, and we will use the GCC 15.2.0 to compile the code.
- no need to do the experiments in the appendix, only the main figures and tables are needed.

## Status (2026-10-30)

Submit by 2026-11-09 AoE. The switch to generated synthetic data (item 1,
2026-11-02) is pushed, `8d3ca0b`; CI rebuilds `latest` on every push. Details:
`CLAUDE.md`, "Artifact evaluation".

The artifact covers the paper's body: Fig 4 (a csv table) and Fig 5 to 9.
Not Fig 1 (your answer 6) and not the appendix: Fig 10, Fig 11, Table 1
(answer 11, line 10).

Done:
- Docs in PSI style: `README.md` (pitch, links; Citation empty); the old
  README is now `docs/MANUAL.md`; new `docs/QUICK_START.md`,
  `docs/DOCKER_QUICK_REFERENCE.md`, `docs/ARTIFACT_EVALUATION.md` (getting
  started, steps per figure, parameters, datasets, other data and settings
  for Reusable).
- `LICENSE` copyright line (PSI wording); submodule over HTTPS;
  `.gitignore` admits the new files.
- Docker: `Dockerfile` (Rocky Linux 8.10, `gcc-toolset-15` GCC 15.2.1, R
  with the plot packages, Linux Libertine, UTF-8), `docker-run.sh`,
  `docker-compose.yml`, `.env.example`, `Makefile.docker`, CI
  `.github/workflows/docker-build.yml` (build, `self_test`, push to
  `ghcr.io/ucrparlay/pb-tree`).
- `scripts_ae/`: `run.sh` (all, synthetic, real, or any figN), one
  `ae_run_*.sh` per figure, data generated before use and deleted after
  (~200 GB peak at 1e9), merges, plots as PDF, Fig 4 as csv.

Your answers, applied:
- 2, Fig 7: `measure-cache`'s perf hooks are on `main` as a separate driver,
  `tests/perf_cache.cpp` and `tests/perf_control.h` (targets `perf_btree`,
  `perf_cpam`, `perf_pma`); `test_framework.h` is unchanged.
  `scripts_ae/ae_run_perf.sh` runs it under `perf stat` on the Skewed data,
  the data Fig 7 shows. Needs `./docker-run.sh ... --perf`.
- 3, Fig 5: your `scripts/plot_scripts/plot_osm_{insert,delete,range,
  member}.py`, copied as `scripts_ae/merge_osm_*.py` (paths only). On
  `scripts/logs/osm` they write the same csvs as the originals, and the plot
  renders. Two notes: `scripts/data/osm/*_by_year.csv` are older than those
  logs (BTree and CPAM columns differ); the split and reinsert sizes in
  panel (e) are numbers written into the scripts, not measured by the run.
- 4: the AE runs on gazelle; this box (cheetah) is for development.
- 5: no paper changes. Figure text stays the paper's; Fig 4's csv uses the
  paper's column names.
- 6: Fig 1 dropped.
- 7: CI pushes `ghcr.io/ucrparlay/pb-tree` on every push to `main`.
- 9: Mass-tree and Bw-tree are off by default; `--with-masstree-bwtree`
  (`docker-run.sh` or `run.sh`) adds them.
- 10: synthetic data is generated in the container.
- 11: appendix dropped; Fig 4 as csv. Figures are PDF (your later answer).
- 12: committed to `main`; you pushed. CI passed on `edea032` and published
  `ghcr.io/ucrparlay/pab-tree` (`latest`, `main`, `sha-edea032`); it pulls
  without login.
- The repository is now `ucrparlay/pab-tree` (the old `pb-tree` name
  redirects), so the image is `ghcr.io/ucrparlay/pab-tree`: scripts, docs,
  the path in the image (`/workspace/pab-tree`) and the git remote were
  renamed. Pushed `e20fd85`; CI passed and `latest` is that commit's image.

Tested here, 1e6 entries, in Docker with `--perf`, data on
`/ssd1/zmen002/ae`:
- `./run.sh synthetic`: Fig 4 (csv), Fig 7, Fig 9, no failures; plots
  re-rendered as PDF (Linux Libertine embedded).
- Fig 5: merges and plot from the paper's OSM logs (no real data here).
- Not run: Fig 6, 8 (no real data); Mass-tree and Bw-tree since they
  became optional.

Left on this box: image `pab-tree-ae:test`, 1e6 outputs in
`scripts_ae/{data,logs,plots}`.

## Datasets (2026-10-30)

GB = 10^9 bytes. Every file: header `[N, dim, width]` (24 B), then rows.

### Real-world: what goes to Hugging Face

The files the AE reads, under `<data-path>/real_world/`. The copies are in
`/data/zmen002/geometry/` (`osm_by_year/` is the AE's `osm/`).

| Dataset | Fig. | File(s) | Size | Rows | Made from |
|---|---|---|---|---|---|
| OSM (Asia) | 5 | `osm/asia_year_zcode.bin` | 27.6 GB | 1.72 B (Morton code, year) | Geofabrik `asia-latest.osm.pbf` (15.8 GB, fetched 2026-04-14), ODbL |
| RED | 6 | `temporal_graph/signal/redd/0.bin` | 9.0 GB | 747 M edges, 17 years | GraphTide `redd.bin` (107.9 GB) |
| REF | 6 | `temporal_graph/signal/ref/0.bin` | 13.7 GB | 1.14 B edges, 27 years | GraphTide `ref.bin` (13.7 GB) |
| SEM | 6 | `temporal_graph/signal/sem/0.bin` | 29.9 GB | 2.49 B edges, 229 years | GraphTide `sem.bin` (29.9 GB) |
| PLANET | 6, 8 | `temporal_graph/osm/planet_year/<0..20>_{insert,delete}.bin`, 42 files | 8.1 GB | 745 M inserts, 266 M deletes | GraphTide `planet.bin` (15.2 GB) |
| all | | 46 files | 88.1 GB | | |

- Nothing else is read. `ARTIFACT_EVALUATION.md`'s 107 GB for the graphs
  counted the `all_batch.bin` next to each `0.bin` (same size, unused): the
  graphs are 61 GB, and its disk estimate drops from ~330 to ~290 GB. To
  fix with the URLs.
- Edge and batch counts match the paper. Text vs. files (FYI, no paper
  change): the OSM run keeps 2015-2024 (`test_framework.h`, tag 8), 1.67 B
  records in the paper's log; the text says 1.35 B, 2014-2023. PLANET has
  21 year files; the text says 20 batches.
- Converters: the graphs by `tests/temporal_graph_signal.cpp` (dedup, one
  batch per year, shuffled within a year) and `tests/temporal_graph_osm.cpp
  -group_by_year 1`. OSM: pbf to csv by pyosmium (SpaceTreeLib
  `script/osm.py`), `.in` to `.bin` by `scripts/convert_to_binary.sh`; the
  year and Morton-code step is in neither repo.
- Hugging Face (its docs, 2026-10-30): files < 200 GB recommended, 500 GB
  hard; < 10k files per folder. All fit. Public storage on a free account
  is "best-effort"; PRO includes up to 10 TB. Large datasets need a
  dataset card (I can write it).

### Synthetic: generated in the container (answer 10)

n = 10^9 (`INPUT_SIZE`), by `ae_data_generate.sh`, under
`<data-path>/synthetic/`. 32-bit keys and values: 8 B per entry.

| Set | Fig. | Files | Size | Made by |
|---|---|---|---|---|
| `incre` | 4; 7 (norm, x = 100) | `incre/<dis>/1000000000_1/{ins,del}/<x>/1.batch.bin`, x = 10, 100, 1000, 10000: 24 x 8.0 GB | 192 GB | `incre_data_generator` |
| `single` | 9 | `single/<dis>/1000000000_1/{ins,del}/100/1.{base,batch}.bin`: 8.0 GB + 80 MB | 16.2 GB per dis, 48.5 GB | uni, norm: `data_generator`; seg: `incre_data_generator` + `make_single_batch.py` |

`<dis>`: uni; norm (Skew), each batch normal(0.5, 0.1); seg (Adv), each
batch from the leading 2m keys left.

Against the paper's copies on `/data/zmen002`:
- `incre` = `kv_pair_incre/{uni,norm,seg}` (2026-07-31 to 08-01), uploaded
  as they are. Regenerated with the image at 10^9, every x: norm and seg are
  the same bytes, 16 of 16 files. uni is another permutation of the same
  distribution (mean 0.50, std 0.2887): its files predate `d5cff3e` and
  `01f74fc`. The norm batches measure sigma = 0.100
  (`scripts/gen_incre_data.sh` now says 0.05; that did not make them).
- Generation, measured 2026-11-02 at 10^9 (64 threads, other users' jobs
  alongside): `incre` 58 min, 47 of them norm at x = 10000; `single` 3.5 min.
  About an hour against 73 min to download the same 240 GB. The ~5 h written
  here before was my guess from August file times, half of it `del`, which is
  now a hard link.
- Memory: `data_generator` (`single` uni, norm) peaks at 106 GB, so this box
  needs other users' jobs out of the way: it aborted 9 times with `bad_alloc`,
  7 of them in a row while another user's benchmark ran. A rerun goes on from
  the finished files. `incre_data_generator` takes 40-44 GB. To check: what
  gazelle has free.
- `single`: uploaded from `/ssd1/zmen002/ae/synthetic/single`, made with
  the published image, 3.5 min for all of it. uni is byte for byte the
  paper's `kv_pair/uni`. Regenerated: 9 of 12 files the same. norm is a new
  sample on every run: `data_generator`
  shares one `normal_distribution` between threads. Its keys stay right (batch
  mean 0.49996, std 0.1018). norm and seg are not the paper's Fig 9 input: that
  read `kv_pair/{exp,norm}` (its norm measures sigma ~0.2), and
  `plot_scalability.R` calls exp Skewed and norm Adversarial. The AE follows
  your answer: Skew = norm 0.1, Adv = seg.
- Copies: 112 of the 329 GB are byte-identical files (216.5 GB unique), and
  none is a mistake. The incre generator's `batch_op` only names the
  directory, so each `del` stream is its `ins` stream (96 GB); `single/seg`
  del base = `incre/seg` x = 100; `single` uni and norm share a del base.

## Needs you

1. Real data: where to host OSM (28 GB) and RED, REF, SEM, PLANET
   (107 GB)? May GraphTide (under review) be redistributed? Fig 5, 6, 8
   need it; I then fill the URLs in `scripts_ae/download_data.sh`.
   Answer: lets use huggingface
   Then, for the upload (table in Datasets):
   - owner and name: your account or an org (e.g. `ucrparlay`)?
   - real only, or the synthetic sets too (+240 GB)?
   - GraphTide: OK to make RED, REF, SEM, PLANET public now?
   - a write token: `huggingface_hub` is not on this box; once I install
     it, you run `hf auth login`.
   Answer: owner ucrparlay, both, ok, yes, I will run 
   Changed 2026-10-31: "change the organzition to the personal, e.g., I own
   the dataset". No org.
   Changed 2026-11-01: "let's use the MIT licence inthe data distribution".
   Applied: MIT, with the repo's `LICENSE` in the dataset, except what is
   built from OpenStreetMap: `real_world/osm/` and
   `real_world/temporal_graph/osm/` (PLANET) stay under ODbL, which requires
   that. Nobody but OSM's rights holders can relabel them.
   Done 2026-11-01: https://huggingface.co/datasets/romaLzhih/pab-tree-data,
   public (the repo name is mine). 41 min, 209 GB sent; the Hub counts 216.5
   GB; its sha256 matches `scripts_ae/datasets.sha256` on 82 of 82 files.
   Downloaded back in the image with no token: 55 MB/s with checksums, so all
   329 GB take about 1 h 40 min. Resume tested on the real repo. That test
   found a bug, now fixed: a run killed during a checksum lost the file.
   Changed 2026-11-02: "ok now lets avoid uploading the synthetic data to the
   hugging face, but insetead generate it locally, since downloading takes too
   long, don't delete the one on the huggingface for now".
   Applied, `8d3ca0b`: the artifact downloads only `real_world/` (88 GB, 27
   min) and generates the rest. The Hub's synthetic copy is untouched. Its card
   is stale (`DOWNLOAD=0`, about 5 hours); a corrected copy waits for your
   go-ahead.
   `a7d267c`'s image, run with nothing overlaid and no token: its
   `download_data.sh` and `fetch` got files that pass the checksums.
   PLANET's OSM origin comes from names only (`temporal_graph/osm/`,
   `planet.bin`). RED, REF, SEM are MIT on your word; their original sources'
   terms are not checked.

2. Available badge: a Zenodo DOI, from a GitHub release of the repo.
   Answer:

3. BibTeX: authors and DOI once published.

## Answered

Your words, then what was done (see Status).

- ghcr.io public: "i have make it public, check". Checked: anonymous pull
  of `ghcr.io/ucrparlay/pab-tree` works.
- Figures: "pdf". Done.
- Commit: "pushed".
- Fig 7 and Table 1 (perf hooks): "port it to the `main` branch and script
  both. Don't mess it the current test framework, considering use an
  separate file." Table 1 then dropped with the appendix.
- Fig 5 csv: "there should have scripts to generate the csv, you can check
  it manually"
- Tested machine: "it will be run in another machine gazelle, not this one
  cheetah"
- Paper vs. AE: "no need to do that. as long as the figures texts are same
  its okay."
- Fig 1: "No need to run experiments for fig 1."
- Image: "ok do it."
- Bw-tree crashes: "add a separate commandline option to decide whether to
  run the masstree and the bwtree, in default false."
- Synthetic data: "currently generate the data as needed in the container."
- Tables: "no need to do the experiments in the appendix. For figure 4 use
  the csv format"
- Commit: "main."
- Badges: all: Available, Reusable, Results Reproduced.
- Scope: the AE runs later on the tested machine; this box is for
  development and testing.
- Datasets: Adversarial is `seg`; Skewed is normal(0.5, 0.1).
- Compiler: Red Hat's `gcc-toolset-15`, GCC 15.2.1, not built from source
  (first 15.2.0 from source; changed in chat).
- README: rewritten in PSI style.
- Baselines did not compile (tag `1<<8` block): you removed it, `ace4ed6`.
- LICENSE: PSI's MIT wording. Submodule `fast-cpp-csv-parser`: HTTPS.
  Third-party LICENSE files under `external/index-microbench`: not tracked.
  BibTeX: empty for now.
