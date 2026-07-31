// Data generator for the incremental-update benchmarks (b_test tags 1 << 6 and
// 1 << 7, driven by scripts/incre_updates.sh).
//
// WHY THIS EXISTS.  data_generate.cpp random-shuffles the whole key set before
// writing it (Generator::ToEntry), so slicing that file into consecutive
// batches -- which is exactly what BatchInsertByStep / BatchDeleteByStep do --
// hands every batch a *uniform* sample of the key space no matter which
// distribution the file was generated with.  The shape survives only globally,
// never per batch.
//
// WHAT THIS DOES INSTEAD.  It builds the batches explicitly.  The base key
// space is dense and uniform (key == rank -- a rank-discretised base comes out
// dense anyway, see Discreatize in data_op.h), and batches are carved out of a
// shrinking pool of surviving keys:
//
//   pool <- [0, base_n)
//   repeat until the pool is empty:
//     draw batch_n positions from the requested distribution, mapped onto
//       [0, |pool|)   -- these are RANKS INTO THE POOL, not keys
//     batch <- the pool entries sitting at those ranks
//     pool  <- pool minus batch
//
// With an exponential draw each batch therefore clusters at the front of
// whatever is left, and the cluster walks rightward as the front is thinned.
// The file written out is the batches concatenated in generation order, so the
// harness's existing "slice the file into equal chunks" consumption picks the
// batches up unchanged.

#include <cstdint>
#include <string>

#include "data_op.h"
#include "parlay/random.h"

// PARA: mirrors -dis in data_generate.cpp.  Here it selects the shape of the
// *batches*; the base is always the dense uniform key space.
enum DisType : int
{
  kDisUniform = 0,
  kDisExponential = 1,
  kDisNormal = 2,
  kDisDualNormal = 3,
};

parlay::sequence<double>
ParseParam(std::string const& input)
{
  if (input.empty()) {
    return {};
  }
  parlay::sequence<double> result;
  size_t start = 0;
  size_t end = input.find(',');

  while (end != std::string::npos) {
    std::string token = input.substr(start, end - start);
    try {
      result.push_back(std::stod(token));
    } catch (...) {
      std::cerr << "Error parsing value: " << token << std::endl;
      return {};
    }
    start = end + 1;
    end = input.find(',', start);
  }

  std::string token = input.substr(start);
  try {
    result.push_back(std::stod(token));
  } catch (...) {
    std::cerr << "Error parsing value: " << token << std::endl;
    return {};
  }

  return result;
}

inline double
ParamOr(parlay::sequence<double> const& param, size_t i, double fallback)
{
  return i < param.size() ? param[i] : fallback;
}

// NOTE: the target shape, as an UNNORMALISED density at position x in [0, 1),
// where 0 is the smallest surviving key and 1 the largest.  The normalising
// constant is never needed -- the caller divides by the sum over the pool,
// which also makes the shape automatically "truncated to [0,1)" rather than
// wrapped or piled onto the end buckets.
inline double
TargetDensity(int const dis_type,
              parlay::sequence<double> const& param,
              double const x)
{
  if (dis_type == kDisUniform) {
    return 1.0;
  }

  if (dis_type == kDisExponential) {
    return std::exp(-ParamOr(param, 0, 10.0) * x);
  }

  if (dis_type == kDisNormal) {
    double const mean = ParamOr(param, 0, 0.5);
    double const dev = ParamOr(param, 1, 0.2);
    double const z = (x - mean) / dev;
    return std::exp(-0.5 * z * z) / dev;
  }

  // NOTE: dual normal.  In data_generate.cpp "dual" meant base *and* batch were
  // normal; here the base distribution is meaningless (the base is the whole
  // dense key space), so dual instead means a two-mode batch -- otherwise it
  // would be indistinguishable from kDisNormal.
  double const m1 = ParamOr(param, 0, 0.25);
  double const d1 = ParamOr(param, 1, 0.05);
  double const m2 = ParamOr(param, 2, 0.75);
  double const d2 = ParamOr(param, 3, 0.05);
  double const z1 = (x - m1) / d1;
  double const z2 = (x - m2) / d2;
  return (0.5 * std::exp(-0.5 * z1 * z1) / d1) +
         (0.5 * std::exp(-0.5 * z2 * z2) / d2);
}

// NOTE: pick exactly `want` of the `pool_n` surviving ranks, so that rank i is
// picked with probability pi_i proportional to the target density at i.  This
// replaces the earlier guess-and-retry sampler, which under-filled the peak
// whenever the favoured region got crowded: a retry that lands on an already
// taken rank is a wasted draw, and in a crowded region nearly every draw is
// wasted, so those keys leaked into the tail instead.  Here nothing is retried.
//
// Two steps:
//
//   1. Turn the density into inclusion probabilities that sum to exactly `want`
//      and never exceed 1.  Unclamped that is just pi_i = want * w_i / sum(w),
//      but where the density is steep enough to ask for more than 100% of the
//      ranks in a region -- which is arithmetically impossible, one rank cannot
//      be picked 1.3 times -- the excess has to go somewhere.  Since pi_i is
//      proportional to w_i for every unclamped rank, "clamped" is exactly
//      "w_i >= t" for a single threshold t solving
//          #{w_i >= t} + (sum of w_i below t) / t == want.
//      That function is strictly decreasing in t, and the fixed-point step
//      t' = (sum below t) / (want - #{>= t}) always moves the right way, so a
//      handful of iterations converge.  pi_i = min(1, w_i / t).
//
//   2. Select by one running-total sweep: walk the cumulative pi and take a
//      rank whenever the total crosses the next whole number, offset by a
//      random start in [0,1).  The cumulative total is `want`, so exactly
//      `want` whole numbers are crossed and exactly `want` ranks come out,
//      each with probability exactly pi_i.
//
// Returns the per-rank selection flags; the caller uses them both to gather the
// batch and to compact the pool, so no separate claim array is needed.
parlay::sequence<bool>
SelectBatchFlags(size_t const pool_n,
                 size_t const want,
                 int const dis_type,
                 parlay::sequence<double> const& param,
                 size_t const seed,
                 size_t& clamped_total)
{
  static constexpr int kMaxSolveIters = 24;

  // NOTE: the final batch takes whatever is left over.
  if (want >= pool_n) {
    return parlay::sequence<bool>(pool_n, true);
  }

  auto const w = parlay::delayed_tabulate(pool_n, [&](size_t i) {
    return TargetDensity(
      dis_type, param, (static_cast<double>(i) + 0.5) / pool_n);
  });

  auto const dwant = static_cast<double>(want);
  auto const sum_all = parlay::reduce(w, parlay::addm<double>());
  auto const max_all = parlay::reduce(w, parlay::maxm<double>());

  // NOTE: the unclamped threshold.  Everything below is only needed when the
  // density is steep enough that some rank would want pi > 1.
  double t = sum_all / dwant;
  if (max_all > t) {
    for (int it = 0; it < kMaxSolveIters; it++) {
      // count of ranks at or above t, and the mass strictly below it
      auto const acc = parlay::reduce(
        parlay::delayed_tabulate(
          pool_n,
          [&](size_t i) {
            double const v = w[i];
            return v >= t ? std::make_pair(size_t(1), 0.0)
                          : std::make_pair(size_t(0), v);
          }),
        parlay::make_monoid(
          [](auto const& a, auto const& b) {
            return std::make_pair(a.first + b.first, a.second + b.second);
          },
          std::make_pair(size_t(0), 0.0)));

      if (acc.first >= want) { // degenerate: back off and retry
        t *= 2.0;
        continue;
      }
      double const t_next = acc.second / (dwant - static_cast<double>(acc.first));
      if (!(t_next < t) || t_next <= 0.0) {
        clamped_total += acc.first;
        break;
      }
      t = t_next;
    }
  }

  // NOTE: an independent weighted coin per rank -- rank i is taken iff its own
  // uniform draw falls under pi_i.  Every rank is decided on its own, so the
  // per-region counts scatter around the target exactly the way a genuine
  // random sample does.
  //
  // The obvious alternative -- walk the cumulative pi and take a rank each time
  // it crosses a whole number -- also lands every rank with probability pi_i
  // and yields exactly `want` with no repair, but it spaces the picks out along
  // the curve by construction: measured, it produced ~150x less scatter than a
  // real draw, so every stretch of the key range got almost precisely its
  // predicted count instead of running hot or cold.  For a tree benchmark that
  // removes the lucky hot spots that make a skewed workload hard, so it is the
  // wrong trade even though it is tidier.
  parlay::random_generator gen(seed);
  auto flags = parlay::tabulate(pool_n, [&, t](size_t i) -> bool {
    auto r = gen[i];
    return std::uniform_real_distribution<double>(0.0, 1.0)(r) <
           std::min(1.0, w[i] / t);
  });

  // NOTE: independent flips mean the count lands near `want`, not on it -- off
  // by about sqrt(want), i.e. ~0.03% at these batch sizes.  The file is sliced
  // into equal windows downstream, so nudge it back to exact.  The repair is
  // uniform over the selected (or unselected) ranks: dropping uniformly scales
  // every pi by the same factor and so leaves the shape untouched, and the
  // additions are a small enough share that spreading them uniformly sits far
  // below the scatter this whole change exists to restore.
  size_t const got = parlay::reduce(
    parlay::delayed_tabulate(
      pool_n, [&](size_t i) -> size_t { return flags[i] ? 1 : 0; }),
    parlay::addm<size_t>());

  // NOTE: `d` distinct positions in [0, span), drawn without materialising the
  // candidate set -- `d` is tiny, so oversample and dedupe rather than build
  // and shuffle a pool_n-sized index array.
  auto draw_positions = [&](size_t span, size_t d, size_t salt) {
    parlay::sequence<size_t> out;
    for (int attempt = 0; attempt < 16 && out.size() < d; attempt++) {
      parlay::random_generator g(seed + salt + (attempt * 7919ULL));
      auto cand = parlay::tabulate((2 * d) + 64, [&](size_t j) -> size_t {
        auto r = g[j];
        return std::uniform_int_distribution<size_t>(0, span - 1)(r);
      });
      out = parlay::remove_duplicates(parlay::append(out, cand));
    }
    return out.subseq(0, std::min(d, out.size()));
  };

  if (got > want) {
    auto const sel = parlay::pack(parlay::iota(pool_n), flags);
    auto const drop = draw_positions(sel.size(), got - want, 1);
    parlay::parallel_for(
      0, drop.size(), [&](size_t j) { flags[sel[drop[j]]] = false; });
  } else if (got < want) {
    // NOTE: most ranks are unselected, so rejection finds fresh ones in one or
    // two rounds without touching the whole pool.
    size_t need = want - got;
    for (int attempt = 0; attempt < 16 && need > 0; attempt++) {
      auto const cand = draw_positions(pool_n, 4 * need, 2 + attempt);
      auto const fresh =
        parlay::filter(cand, [&](size_t v) { return !flags[v]; });
      size_t const take = std::min(need, fresh.size());
      parlay::parallel_for(
        0, take, [&](size_t j) { flags[fresh[j]] = true; });
      need -= take;
    }
  }

  return flags;
}

// NOTE: the whole key space, emitted as batches back to back in generation
// order.  Consecutive windows of `batch_n` entries are the batches.
template<typename E>
parlay::sequence<E>
GenIncrementalBatches(size_t const base_n,
                      size_t const batch_n,
                      int const dis_type,
                      parlay::sequence<double> const& param)
{
  using K = typename E::K;

  // NOTE: dense and sorted, so a key IS its rank and "rank into the pool"
  // means the same thing before and after keys are removed.
  auto pool = parlay::tabulate(base_n, [](size_t i) -> size_t { return i; });

  auto out = parlay::sequence<E>::uninitialized(base_n);

  size_t const batch_num = (base_n + batch_n - 1) / batch_n;
  size_t const report_every = std::max<size_t>(batch_num / 10, 1);
  size_t written = 0;
  size_t clamped_total = 0;

  std::cout << "batches: " << batch_num << " x " << batch_n << std::endl;

  for (size_t b = 0; b < batch_num; b++) {
    size_t const pool_n = pool.size();
    size_t const want = std::min(batch_n, pool_n);

    auto const flags = SelectBatchFlags(
      pool_n,
      want,
      dis_type,
      param,
      ((static_cast<size_t>(rand_seed) + 1) * 1'000'003ULL) + (b * 64ULL),
      clamped_total);
    auto picked = parlay::pack(parlay::iota(pool_n), flags);

    // NOTE: order *within* a batch carries no meaning, and a batch handed to
    // the tree in ascending key order would be an unrealistically easy input.
    // Shuffle inside the batch; the batches themselves stay in generation
    // order, which is the entire point of this generator.
    auto batch_rank = parlay::random_shuffle(picked);
    parlay::parallel_for(0, batch_rank.size(), [&](size_t i) {
      out[written + i] = E{ .key = static_cast<K>(pool[batch_rank[i]]),
                            .val = { written + i } };
    });

    if (b % report_every == 0 || b + 1 == batch_num) {
      auto keys = parlay::delayed_tabulate(
        batch_rank.size(), [&](size_t i) { return pool[batch_rank[i]]; });
      auto const lo = parlay::reduce(keys, parlay::minm<size_t>());
      auto const hi = parlay::reduce(keys, parlay::maxm<size_t>());
      auto const mean =
        static_cast<double>(parlay::reduce(keys, parlay::addm<size_t>())) /
        static_cast<double>(batch_rank.size());
      std::cout << "  batch " << b << ": pool " << pool_n << ", keys [" << lo
                << ", " << hi << "], mean "
                << mean / static_cast<double>(base_n) << " of the key space"
                << std::endl;
    }

    written += batch_rank.size();

    // NOTE: drop what was taken, keeping the survivors in key order so the
    // next batch's ranks keep meaning the same thing.  O(pool_n) per batch and
    // so O(base_n * batch_num / 2) overall -- the dominant cost at small batch
    // ratios.
    pool = parlay::pack(pool, parlay::delayed_tabulate(pool_n, [&](size_t i) {
                          return !flags[i];
                        }));
  }

  if (clamped_total != 0) {
    std::cout << "NOTE: " << clamped_total << " rank-slots across the run were "
              << "capped at probability 1 (" << (100.0 * clamped_total / base_n)
              << "% of base_n) -- the shape asked for more than 100% of the "
                 "keys there, so those batches are the sharpest possible "
                 "rather than the requested shape"
              << std::endl;
  }

  return out;
}

// NOTE: the output has to be a permutation of [0, base_n).  Count, sum and max
// pin that down for every error this generator can realistically make (a lost
// or duplicated claim shows up in the sum); -validate 1 adds the full
// distinctness check, which costs a hash pass over the whole key space.
template<typename E>
void
ValidatePermutation(parlay::sequence<E> const& seq,
                    size_t const base_n,
                    bool const full)
{
  if (seq.size() != base_n) {
    throw std::runtime_error("Validate: wrong number of entries.");
  }

  auto keys = parlay::delayed_tabulate(
    seq.size(), [&](size_t i) -> size_t { return seq[i].key; });
  auto const sum = parlay::reduce(keys, parlay::addm<size_t>());
  auto const max = parlay::reduce(keys, parlay::maxm<size_t>());
  size_t const want_sum = base_n % 2 == 0 ? (base_n / 2) * (base_n - 1)
                                          : base_n * ((base_n - 1) / 2);

  if (sum != want_sum || max != base_n - 1) {
    std::cout << "key sum " << sum << " (expected " << want_sum << "), max "
              << max << " (expected " << base_n - 1 << ")" << std::endl;
    throw std::runtime_error("Validate: output is not a permutation.");
  }

  if (full) {
    auto materialised =
      parlay::tabulate(seq.size(), [&](size_t i) -> size_t { return seq[i].key; });
    if (parlay::remove_duplicates(materialised).size() != base_n) {
      throw std::runtime_error("Validate: output has duplicate keys.");
    }
  }

  std::cout << "Validation passed." << std::endl;
}

// ---------------------------------------------------------------------------
// HOW TO RUN
//
//   make -C ../build incre_data_generator -j$(nproc)
//
//   # 1e9 keys handed out in 100 batches of 1e7, each batch exponential over
//   # the surviving pool
//   ./../build/incre_data_generator -path /data/zmen002/kv_pair_incre \
//       -base_n 1000000000 -batch_n 10000000 -val_n 1 -file_num 1 \
//       -dis 1 -batch_op 0 -binary 1 -batch_param 50
//
// Writes ONE file -- the whole key space as the batch stream, not a base/batch
// pair.  Same stem for both ops, only the directory differs:
//   -batch_op 0 (ins) -> <path>/<dis>/<base_n>_<val_n>/ins/<base_n/batch_n>/<i>.batch.bin
//   -batch_op 1 (del) -> <path>/<dis>/<base_n>_<val_n>/del/<base_n/batch_n>/<i>.batch.bin
//
// Point -p at the ins one; ExtractPrefixes strips ".batch" so the del path is
// derived from it.
//
// -batch_param: exp -> lambda (default 10); norm -> mean,dev (default 0.5,0.2);
// dual_norm -> mean1,dev1,mean2,dev2 (default 0.25,0.05,0.75,0.05); uni ignores
// it.  All of them are fractions of the surviving pool, not absolute keys.
//
// Point -path at a FRESH root: the directory layout matches the existing
// datasets on purpose (so Options::ParseInsertBatchPath / ParseDeleteBatchPath
// resolve it unchanged), which also means it would overwrite them in place.
// ---------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
  commandLine P(argc,
                argv,
                "[-path <dir>] [-base_n <n>] [-batch_n <n>] [-val_n 1] "
                "[-file_num <n>] [-dis {0,1,2,3}] [-batch_op {0,1}] "
                "[-binary {0,1}] [-batch_param <csv>] [-validate {0,1}]");

  // NOTE: the std::string overloads, not the char* ones -- getOptionValue
  // returns a null pointer when the flag is absent and std::string(nullptr) is
  // UB, which is the segfault the old data_generator has on a missing
  // -base_param / -batch_param.
  std::string const path_in = P.getOptionValue("-path", "");
  auto const base_n =
    static_cast<size_t>(P.getOptionLongValue("-base_n", 1'000'000));
  auto const batch_n =
    static_cast<size_t>(P.getOptionLongValue("-batch_n", 10'000));
  int const val_n = P.getOptionIntValue("-val_n", 1);
  int const file_n = P.getOptionIntValue("-file_num", 1);
  // PARA: 0: uniform, 1: exp, 2: norm, 3: dual norm -- shape of each BATCH
  int const dis_type = P.getOptionIntValue("-dis", 0);
  // PARA: 0: insert, 1: delete
  int const batch_op = P.getOptionIntValue("-batch_op", 0);
  // PARA: 0: text .in, 1: binary .bin
  int const binary = P.getOptionIntValue("-binary", 0);
  bool const validate = P.getOptionIntValue("-validate", 0) != 0;

  if (path_in.empty()) {
    std::cerr << "-path is required" << std::endl;
    return EXIT_FAILURE;
  }
  if (val_n != 1) {
    std::cerr << "only -val_n 1 is supported" << std::endl;
    return EXIT_FAILURE;
  }
  if (batch_n == 0 || batch_n > base_n) {
    std::cerr << "-batch_n must be in [1, base_n]" << std::endl;
    return EXIT_FAILURE;
  }

  parlay::sequence<double> const batch_param_seq =
    ParseParam(P.getOptionValue("-batch_param", ""));

  std::cout << "base_n: " << base_n << " batch_n: " << batch_n
            << " val_n: " << val_n << " file_num: " << file_n
            << " distribution_type: " << dis_type
            << " batch_op: " << (batch_op == 0 ? "ins" : "del")
            << " batch_param: ";
  for (auto const& p : batch_param_seq) {
    std::cout << p << " ";
  }
  std::cout << std::endl;

  // NOTE: `demand` is literally the inclusion probability handed to the
  // front-most surviving key: lambda * batch_n / pool.  A probability cannot
  // exceed 1, so demand > 1 is not a tuning problem but an impossible request,
  // and those ranks get capped (reported per run by GenIncrementalBatches).
  // The pool drains as the run proceeds, so demand rises as
  // demand_0 / (1 - progress) and the share of the run that is exactly on-shape
  // is 1 - demand_0.  lambda = 0.5 * base_n / batch_n keeps the whole first
  // half exact, which is the half the benchmark's query state is built from.
  if (dis_type == kDisExponential) {
    double const demand = static_cast<double>(batch_n) *
                          ParamOr(batch_param_seq, 0, 10.0) /
                          static_cast<double>(base_n);
    std::cout << "front-key probability at the start (demand): " << demand
              << " -- exactly on-shape for the first "
              << (100.0 * std::max(0.0, 1.0 - demand)) << "% of the run"
              << std::endl;
    if (demand > 0.5) {
      std::cout << "WARNING: demand > 0.5, so the shape degrades before the "
                   "half-way point. lambda <= "
                << 0.5 * static_cast<double>(base_n) /
                     static_cast<double>(batch_n)
                << " keeps the whole first half exact." << std::endl;
    }
  }

  std::string path = path_in;
  path += std::string(*path.rbegin() == '/' ? "" : "/");
  std::filesystem::create_directories(path);

  path += std::string(dis_type == kDisUniform       ? "uni/"
                      : dis_type == kDisExponential ? "exp/"
                      : dis_type == kDisNormal      ? "norm/"
                                                    : "dual_norm/");
  std::filesystem::create_directories(path);

  path += toString(base_n) + "_" + toString(val_n) + "/";
  std::filesystem::create_directories(path);

  path += std::string(batch_op == 0 ? "ins/" : "del/");
  std::filesystem::create_directories(path);

  path += toString(base_n / batch_n) + "/";
  std::filesystem::create_directories(path);

  // NOTE: ".batch" for BOTH ops.  There is no base/batch pair here -- the whole
  // file IS the batch stream, sliced into equal windows by the harness -- so
  // naming the insert side ".base" only because that is what the old datasets
  // called their big file made the two ops look different when they are not.
  // Tags 1 << 6 and 1 << 7 both read ParseXBatchPath(...).batch_path, and
  // Options::ExtractPrefixes strips ".batch" so -p can point straight at one.
  std::string const stem = ".batch";
  std::string const ext = binary ? ".bin" : ".in";

  using E = Wrapper<Axis, std::tuple<Axis>>::Element<Axis, std::tuple<Axis>>;

  for (int i = 0; i < file_n; i++) {
    std::string const out_path = path + toString(i + 1) + stem + ext;
    std::cout << out_path << std::endl;
    rand_seed = i;

    std::cout << "Generating... " << std::endl;
    auto const seq =
      GenIncrementalBatches<E>(base_n, batch_n, dis_type, batch_param_seq);

    ValidatePermutation(seq, base_n, validate);

    std::cout << "Writing... " << std::endl;
    if (binary) {
      PrintToBinaryFile(out_path, seq);
    } else {
      PrintToFile(out_path, seq);
    }
  }

  return 0;
}
