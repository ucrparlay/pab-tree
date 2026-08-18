/*
 * Data generator for the incremental-update benchmarks (b_test tags 1 << 6 and
 * 1 << 7, driven by scripts/incre_updates.sh).
 *
 * WHY THIS EXISTS.  data_generate.cpp random-shuffles the whole key set before
 * writing it (Generator::to_entry), so slicing that file into consecutive
 * batches -- which is exactly what batch_insert_by_step and
 * batch_delete_by_step do --
 * hands every batch a *uniform* sample of the key space no matter which
 * distribution the file was generated with.  The shape survives only globally,
 * never per batch.
 *
 * WHAT THIS DOES INSTEAD.  It builds the batches explicitly.  The base key
 * space is dense and uniform (key == rank -- a rank-discretised base comes out
 * dense anyway, see Discreatize in data_op.h), and batches are carved out of a
 * shrinking pool of surviving keys:
 *
 *   pool <- [0, base_n)
 *   repeat until the pool is empty:
 *     draw batch_n positions from the requested distribution, mapped onto
 *       [0, |pool|)   -- these are RANKS INTO THE POOL, not keys
 *     batch <- the pool entries sitting at those ranks
 *     pool  <- pool minus batch
 *
 * With an exponential draw each batch therefore clusters at the front of
 * whatever is left, and the cluster walks rightward as the front is thinned.
 * The file written out is the batches concatenated in generation order, so the
 * harness's existing "slice the file into equal chunks" consumption picks the
 * batches up unchanged.
 */

#include <algorithm>
#include <cstdint>
#include <string>

#include "data_op.h"
#include "parlay/random.h"

/*
 * PARA: mirrors -dis in data_generate.cpp.  Here it selects the shape of the
 * *batches*; the base is always the dense uniform key space.
 */
enum dis_type : int {
	dis_uniform = 0,
	dis_exponential = 1,
	dis_normal = 2,
	dis_dual_normal = 3,
	/*
	 * NOTE: "seg" -- a sliding segment.  Each batch of m is drawn uniformly from
	 * the first `mult * m` surviving keys (mult = 2 by default), then removed, so
	 * the hot region is a fixed-width window of ranks that advances by exactly m
	 * per batch.  Unlike exp/norm its width does not depend on the pool size at
	 * all, which makes it the one shape that stays equally concentrated from the
	 * first batch to the last.
	 */
	dis_seg = 4,
};

parlay::sequence<double> parse_param(std::string const &input)
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

inline double param_or(parlay::sequence<double> const &param, size_t i,
					   double fallback)
{
	return i < param.size() ? param[i] : fallback;
}

/*
 * NOTE: the target shape, as an UNNORMALISED density at position x in [0, 1),
 * where 0 is the smallest surviving key and 1 the largest.  The normalising
 * constant is never needed -- the caller divides by the sum over the pool,
 * which also makes the shape automatically "truncated to [0,1)" rather than
 * wrapped or piled onto the end buckets.
 */
inline double target_density(int const dis_type,
							 parlay::sequence<double> const &param,
							 double const x)
{
	/*
	 * NOTE: seg is flat like uniform -- everything that makes it a segment lives
	 * in window_size, which restricts the draw to the leading `mult * m` ranks.
	 * Inside that window every surviving key is equally likely.
	 */
	if (dis_type == dis_uniform || dis_type == dis_seg) {
		return 1.0;
	}

	if (dis_type == dis_exponential) {
		return std::exp(-param_or(param, 0, 10.0) * x);
	}

	if (dis_type == dis_normal) {
		double const mean = param_or(param, 0, 0.5);
		double const dev = param_or(param, 1, 0.2);
		double const z = (x - mean) / dev;
		return std::exp(-0.5 * z * z) / dev;
	}

	/*
	 * NOTE: dual normal.  In data_generate.cpp "dual" meant base *and* batch were
	 * normal; here the base distribution is meaningless (the base is the whole
	 * dense key space), so dual instead means a two-mode batch -- otherwise it
	 * would be indistinguishable from dis_normal.
	 */
	double const m1 = param_or(param, 0, 0.25);
	double const d1 = param_or(param, 1, 0.05);
	double const m2 = param_or(param, 2, 0.75);
	double const d2 = param_or(param, 3, 0.05);
	double const z1 = (x - m1) / d1;
	double const z2 = (x - m2) / d2;
	return (0.5 * std::exp(-0.5 * z1 * z1) / d1) +
		   (0.5 * std::exp(-0.5 * z2 * z2) / d2);
}

/*
 * NOTE: an upper bound on the density over [0, 1), used as the rejection
 * ceiling in the sparse path.  Evaluated on a grid plus the analytic mode
 * locations, then given headroom: over-estimating only costs a few wasted
 * draws, under-estimating would clip the peak and silently flatten the shape.
 */
inline double target_density_bound(int const dis_type,
								   parlay::sequence<double> const &param)
{
	double m = 0.0;
	static constexpr int k_grid = 4096;
	for (int i = 0; i <= k_grid; i++) {
		m = std::max(m, target_density(dis_type, param, double(i) / k_grid));
	}
	/*
	 * NOTE: a narrow peak can fall between grid points, so probe the modes too.
	 */
	if (dis_type == dis_exponential) {
		m = std::max(m, target_density(dis_type, param, 0.0));
	} else if (dis_type == dis_normal) {
		m = std::max(
			m, target_density(dis_type, param,
							  std::clamp(param_or(param, 0, 0.5), 0.0, 1.0)));
	} else if (dis_type == dis_dual_normal) {
		m = std::max(
			m, target_density(dis_type, param,
							  std::clamp(param_or(param, 0, 0.25), 0.0, 1.0)));
		m = std::max(
			m, target_density(dis_type, param,
							  std::clamp(param_or(param, 2, 0.75), 0.0, 1.0)));
	}
	return m * 1.05;
}

/*
 * NOTE: how many LEADING ranks of the pool can possibly matter this batch.
 *
 * A steep exponential is essentially zero past the front: the mass beyond
 * x = K/lambda is e^-K of the total, so at lambda = 5000 more than 99% of every
 * pass was being spent evaluating std::exp on ranks whose probability rounds to
 * zero.  K = 40 puts the skipped mass at ~4e-18; the expected number of picks
 * lost to the truncation is bounded by lambda * want * e^-K, which is ~1e-9 even
 * at the largest configuration here, i.e. far below one key.
 *
 * The floor of 8 * want keeps the window usable when the shape is capped (demand
 * past 1), where the front saturates at probability 1 and the batch has to reach
 * further back than the density alone suggests.
 *
 * Only the exponential is front-loaded, so every other shape returns the whole
 * pool and behaves exactly as before.  A centred window would work for
 * norm/dual_norm but is a different change.
 */
inline size_t window_size(int const dis_type,
						  parlay::sequence<double> const &param,
						  size_t const pool_n, size_t const want)
{
	/*
	 * NOTE: seg IS its window -- the batch is drawn uniformly from the leading
	 * `mult * want` surviving keys and nothing else is ever a candidate, so this
	 * is the definition of the shape, not an approximation of it.  mult must be
	 * >= 1 (mult == 1 degenerates to "take the front `want` keys outright").
	 */
	if (dis_type == dis_seg) {
		double const mult = std::max(1.0, param_or(param, 0, 2.0));
		double const wide = mult * static_cast<double>(want);
		return wide >= static_cast<double>(pool_n) ? pool_n :
													 static_cast<size_t>(wide);
	}

	if (dis_type != dis_exponential) {
		return pool_n;
	}
	double const lambda = param_or(param, 0, 10.0);
	if (!(lambda > 0.0)) {
		return pool_n;
	}

	static constexpr double k_tail_cutoff = 40.0;
	double const frac = k_tail_cutoff / lambda;
	size_t const by_shape =
		frac >= 1.0 ? pool_n : static_cast<size_t>(frac * pool_n) + 1;
	size_t const floor_n = (want > pool_n / 8) ? pool_n : 8 * want;
	return std::min(pool_n, std::max(by_shape, floor_n));
}

/*
 * NOTE: pick exactly `want` of the `pool_n` surviving ranks, so that rank i is
 * picked with probability pi_i proportional to the target density at i.  This
 * replaces the earlier guess-and-retry sampler, which under-filled the peak
 * whenever the favoured region got crowded: a retry that lands on an already
 * taken rank is a wasted draw, and in a crowded region nearly every draw is
 * wasted, so those keys leaked into the tail instead.  Here nothing is retried.
 *
 * Two steps:
 *
 *   1. Turn the density into inclusion probabilities that sum to exactly `want`
 *      and never exceed 1.  Unclamped that is just pi_i = want * w_i / sum(w),
 *      but where the density is steep enough to ask for more than 100% of the
 *      ranks in a region -- which is arithmetically impossible, one rank cannot
 *      be picked 1.3 times -- the excess has to go somewhere.  Since pi_i is
 *      proportional to w_i for every unclamped rank, "clamped" is exactly
 *      "w_i >= t" for a single threshold t solving
 *          #{w_i >= t} + (sum of w_i below t) / t == want.
 *      That function is strictly decreasing in t, and the fixed-point step
 *      t' = (sum below t) / (want - #{>= t}) always moves the right way, so a
 *      handful of iterations converge.  pi_i = min(1, w_i / t).
 *
 *   2. Select by one running-total sweep: walk the cumulative pi and take a
 *      rank whenever the total crosses the next whole number, offset by a
 *      random start in [0,1).  The cumulative total is `want`, so exactly
 *      `want` whole numbers are crossed and exactly `want` ranks come out,
 *      each with probability exactly pi_i.
 *
 * Returns the per-rank selection flags; the caller uses them both to gather the
 * batch and to compact the pool, so no separate claim array is needed.
 * `win_n` is how many leading ranks are actually considered (see window_size);
 * `pool_n` stays the denominator for the density position, so the shape is the
 * same whether or not the tail is skipped.
 */
parlay::sequence<bool>
select_batch_flags(size_t const win_n, size_t const pool_n, size_t const want,
				   int const dis_type, parlay::sequence<double> const &param,
				   size_t const seed, size_t &clamped_total)
{
	static constexpr int k_max_solve_iters = 24;

	/* NOTE: the final batch takes whatever is left over. */
	if (want >= win_n) {
		return parlay::sequence<bool>(win_n, true);
	}

	auto const w = parlay::delayed_tabulate(win_n, [&](size_t i) {
		return target_density(dis_type, param,
							  (static_cast<double>(i) + 0.5) / pool_n);
	});

	/*
	 * ---------------------------------------------------------------- sparse
	 * NOTE: when the batch is a tiny share of the window, the per-rank sweep
	 * below is enormously wasteful -- at 1e9 ranks and want=1e5 it evaluates the
	 * density a billion times and draws a billion uniforms to select a hundred
	 * thousand keys.  Measured: the density evaluations alone are ~2/3 of the
	 * runtime for `norm` (107.9s vs 35.3s for a constant density at identical
	 * pool-work).
	 *
	 * In that regime draw the ranks directly instead: pick a uniform rank, keep
	 * it with probability w/bound.  That is rejection sampling from the same
	 * shape, and it costs O(want / acceptance) rather than O(win_n).
	 *
	 * It is only valid while collisions are negligible.  Sampling without
	 * replacement by "draw, drop duplicates, redraw" differs from the exact
	 * inclusion probabilities by O(peak inclusion probability) -- which is the
	 * very bias that sank the original guess-and-retry sampler.  Here the path is
	 * gated on that quantity being under k_sparse_pi, so the error stays
	 * ~1e-2 of
	 * the natural scatter.  Above the gate we fall through to the exact sweep.
	 */
	static constexpr double k_sparse_pi = 0.02;
	{
		double const bound = target_density_bound(dis_type, param);
		/* mean of w by sampling -- O(1), no pass over the pool */
		static constexpr int k_probe = 4096;
		double acc = 0.0;
		for (int i = 0; i < k_probe; i++) {
			acc += target_density(dis_type, param,
								  (double(i) + 0.5) / k_probe *
									  (double(win_n) / pool_n));
		}
		double const mean_w = acc / k_probe;
		double const peak_pi =
			mean_w > 0.0 ?
				(want * bound) / (static_cast<double>(win_n) * mean_w) :
				1.0;
		double const accept = bound > 0.0 ? mean_w / bound : 0.0;

		if (peak_pi < k_sparse_pi && accept > 1e-3) {
			/*
			 * NOTE: the flags array is only needed to hand the result back in the
			 * shape the windowed compaction wants.  When the caller can take ranks
			 * instead (see sparse_ranks) it skips this O(win_n) allocation entirely.
			 */
			auto flags = parlay::sequence<bool>(win_n, false);
			size_t got_sparse = 0;
			for (int attempt = 0; attempt < 32 && got_sparse < want;
				 attempt++) {
				size_t const need = want - got_sparse;
				size_t const draw_n =
					static_cast<size_t>((need / accept) * 1.3) + 64;
				parlay::random_generator g(seed + 5077 + (attempt * 6151ULL));
				auto const cand =
					parlay::tabulate(draw_n, [&](size_t j) -> size_t {
						auto r = g[j];
						return std::uniform_int_distribution<size_t>(
							0, win_n - 1)(r);
					});
				auto const keep =
					parlay::tabulate(draw_n, [&](size_t j) -> bool {
						size_t const v = cand[j];
						if (flags[v]) {
							return false;
						}
						auto r = g[draw_n + j];
						return std::uniform_real_distribution<double>(0.0,
																	  1.0)(r) <
							   (target_density(dis_type, param,
											   (static_cast<double>(v) + 0.5) /
												   pool_n) /
								bound);
					});
				auto const fresh =
					parlay::remove_duplicates(parlay::pack(cand, keep));
				size_t const take = std::min(need, fresh.size());
				parlay::parallel_for(0, take,
									 [&](size_t j) { flags[fresh[j]] = true; });
				got_sparse += take;
			}
			if (got_sparse == want) {
				return flags;
			}
			/*
			 * NOTE: could not fill it -- fall through to the exact sweep rather than
			 * ship a short batch.
			 */
		}
	}
	/*
	 * ------------------------------------------------------------ end sparse
	 */

	auto const dwant = static_cast<double>(want);
	auto const sum_all = parlay::reduce(w, parlay::addm<double>());
	auto const max_all = parlay::reduce(w, parlay::maxm<double>());

	/*
	 * NOTE: the unclamped threshold.  Everything below is only needed when the
	 * density is steep enough that some rank would want pi > 1.
	 */
	double t = sum_all / dwant;
	if (max_all > t) {
		for (int it = 0; it < k_max_solve_iters; it++) {
			/* count of ranks at or above t, and the mass strictly below it */
			auto const acc = parlay::reduce(
				parlay::delayed_tabulate(
					win_n,
					[&](size_t i) {
						double const v = w[i];
						return v >= t ? std::make_pair(size_t(1), 0.0) :
										std::make_pair(size_t(0), v);
					}),
				parlay::make_monoid(
					[](auto const &a, auto const &b) {
						return std::make_pair(a.first + b.first,
											  a.second + b.second);
					},
					std::make_pair(size_t(0), 0.0)));

			if (acc.first >= want) { /* degenerate: back off and retry */
				t *= 2.0;
				continue;
			}
			double const t_next =
				acc.second / (dwant - static_cast<double>(acc.first));
			if (!(t_next < t) || t_next <= 0.0) {
				clamped_total += acc.first;
				break;
			}
			t = t_next;
		}
	}

	/*
	 * NOTE: an independent weighted coin per rank -- rank i is taken iff its own
	 * uniform draw falls under pi_i.  Every rank is decided on its own, so the
	 * per-region counts scatter around the target exactly the way a genuine
	 * random sample does.
	 *
	 * The obvious alternative -- walk the cumulative pi and take a rank each time
	 * it crosses a whole number -- also lands every rank with probability pi_i
	 * and yields exactly `want` with no repair, but it spaces the picks out along
	 * the curve by construction: measured, it produced ~150x less scatter than a
	 * real draw, so every stretch of the key range got almost precisely its
	 * predicted count instead of running hot or cold.  For a tree benchmark that
	 * removes the lucky hot spots that make a skewed workload hard, so it is the
	 * wrong trade even though it is tidier.
	 */
	parlay::random_generator gen(seed);
	auto flags = parlay::tabulate(win_n, [&, t](size_t i) -> bool {
		auto r = gen[i];
		return std::uniform_real_distribution<double>(0.0, 1.0)(r) <
			   std::min(1.0, w[i] / t);
	});

	/*
	 * NOTE: independent flips mean the count lands near `want`, not on it -- off
	 * by about sqrt(want), i.e. ~0.03% at these batch sizes.  The file is sliced
	 * into equal windows downstream, so nudge it back to exact.  The repair is
	 * uniform over the selected (or unselected) ranks: dropping uniformly scales
	 * every pi by the same factor and so leaves the shape untouched, and the
	 * additions are a small enough share that spreading them uniformly sits far
	 * below the scatter this whole change exists to restore.
	 */
	size_t const got = parlay::reduce(
		parlay::delayed_tabulate(
			win_n, [&](size_t i) -> size_t { return flags[i] ? 1 : 0; }),
		parlay::addm<size_t>());

	/*
	 * NOTE: `d` distinct positions in [0, span), drawn without materialising the
	 * candidate set -- `d` is tiny, so oversample and dedupe rather than build
	 * and shuffle a win_n-sized index array.
	 */
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
		auto const sel = parlay::pack(parlay::iota(win_n), flags);
		auto const drop = draw_positions(sel.size(), got - want, 1);
		parlay::parallel_for(0, drop.size(),
							 [&](size_t j) { flags[sel[drop[j]]] = false; });
	} else if (got < want) {
		/*
		 * NOTE: the additions have to follow the target density, NOT be spread
		 * uniformly over the free ranks.  Uniform top-up is invisible when the
		 * shape is gentle but obvious when it is steep: at lambda = 5000 it put
		 * ~0.5% of the batch flat across the whole window, which pushed the largest
		 * key out to 4x where the exponential could plausibly reach.  So draw
		 * candidates uniformly and then accept each with probability w/max(w),
		 * which is rejection sampling from the shape itself.
		 */
		size_t need = want - got;
		for (int attempt = 0; attempt < 24 && need > 0; attempt++) {
			/*
			 * NOTE: candidates are drawn WITHOUT deduping -- acceptance is only a few
			 * percent, so deduping the raw draw would hash tens of thousands of
			 * entries per batch to save a handful of collisions.  Dedupe the accepted
			 * set instead, which is ~`need` elements.
			 */
			size_t const draw_n = (64 * need) + 64;
			parlay::random_generator g(seed + 991 + (attempt * 7717ULL));
			auto const cand = parlay::tabulate(draw_n, [&](size_t j) -> size_t {
				auto r = g[j];
				return std::uniform_int_distribution<size_t>(0, win_n - 1)(r);
			});
			auto const keep = parlay::tabulate(draw_n, [&](size_t j) -> bool {
				size_t const v = cand[j];
				if (flags[v]) {
					return false;
				}
				auto r = g[draw_n + j];
				return std::uniform_real_distribution<double>(0.0, 1.0)(r) <
					   (w[v] / max_all);
			});
			auto const fresh =
				parlay::remove_duplicates(parlay::pack(cand, keep));
			size_t const take = std::min(need, fresh.size());
			parlay::parallel_for(0, take,
								 [&](size_t j) { flags[fresh[j]] = true; });
			need -= take;
		}
		/*
		 * NOTE: last resort if the shaped draw could not fill it (only reachable
		 * when the density is degenerate); correctness of the batch size wins.
		 */
		if (need > 0) {
			auto const free_ranks =
				parlay::pack(parlay::iota(win_n),
							 parlay::delayed_tabulate(
								 win_n, [&](size_t i) { return !flags[i]; }));
			parlay::parallel_for(
				0, std::min(need, free_ranks.size()),
				[&](size_t j) { flags[free_ranks[j]] = true; });
		}
	}

	return flags;
}

/*
 * NOTE: the whole key space, emitted as batches back to back in generation
 * order.  Consecutive windows of `batch_n` entries are the batches.
 */
template <typename Entry>
parlay::sequence<Entry>
gen_incremental_batches(size_t const base_n, size_t const batch_n,
						int const dis_type,
						parlay::sequence<double> const &param)
{
	using key_type = typename Entry::key_type;

	auto out = parlay::sequence<Entry>::uninitialized(base_n);

	size_t const batch_num = (base_n + batch_n - 1) / batch_n;
	size_t const report_every = std::max<size_t>(batch_num / 10, 1);
	size_t const seed0 = (static_cast<size_t>(rand_seed) + 1) * 1'000'003ULL;

	std::cout << "batches: " << batch_num << " x " << batch_n << std::endl;

	auto report = [&](size_t b, size_t pool_n, size_t win_n) {
		size_t const beg = b * batch_n;
		size_t const cnt = std::min(batch_n, base_n - beg);
		auto keys = parlay::delayed_tabulate(
			cnt, [&](size_t i) -> size_t { return out[beg + i].key; });
		auto const key_lo = parlay::reduce(keys, parlay::minm<size_t>());
		auto const key_hi = parlay::reduce(keys, parlay::maxm<size_t>());
		auto const mean =
			static_cast<double>(parlay::reduce(keys, parlay::addm<size_t>())) /
			static_cast<double>(cnt);
		std::cout << "  batch " << b << ": pool " << pool_n << ", window "
				  << win_n << ", keys [" << key_lo << ", " << key_hi
				  << "], mean " << mean / static_cast<double>(base_n)
				  << " of the key space" << std::endl;
	};

	/*
	 * NOTE: uniform has a closed form -- one global shuffle IS the whole
	 * batch-by-batch carve.  Cut a uniform random permutation of [0, base_n) into
	 * consecutive batch_n blocks: block 0 is a uniform random subset of the key
	 * space, block 1 a uniform random subset of what is left, and so on, which is
	 * exactly the loop below.  Identical in distribution, O(base_n) instead of
	 * O(base_n * batch_num / 2) -- at 1e9 with 10000 batches that is seconds
	 * rather than 46 minutes, and the ratio stops mattering entirely.
	 *
	 * The seed is explicit: parlay::random_shuffle defaults to a FIXED seed, so
	 * without it every -file_num instance would come out identical.
	 */
	if (dis_type == dis_uniform) {
		auto const perm = parlay::random_shuffle(
			parlay::tabulate(base_n, [](size_t i) -> size_t { return i; }),
			parlay::random(seed0));
		parlay::parallel_for(0, base_n, [&](size_t i) {
			out[i] =
				Entry{ .key = static_cast<key_type>(perm[i]), .val = { i } };
		});
		for (size_t b = 0; b < batch_num; b++) {
			if (b % report_every == 0 || b + 1 == batch_num) {
				report(b, base_n - (b * batch_n), 0);
			}
		}
		std::cout << "pool-work: " << static_cast<double>(base_n)
				  << " (uniform closed form -- one shuffle, no per-batch carve)"
				  << std::endl;
		return out;
	}

	/*
	 * NOTE: dense and sorted, so a key IS its rank and "rank into the pool"
	 * means the same thing before and after keys are removed.  The buffer is
	 * allocated once at full size and never reallocated; the live pool is the
	 * suffix [lo, base_n), so ranks are offsets from `lo`.
	 */
	auto pool = parlay::tabulate(base_n, [](size_t i) -> size_t { return i; });
	size_t lo = 0;
	/*
	 * NOTE: the live pool is [lo, lo + live_n).  The windowed compaction leaves it
	 * a SUFFIX (lo advances), the unwindowed one leaves it a PREFIX (lo resets),
	 * so the length has to be tracked on its own rather than derived from lo.
	 */
	size_t live_n = base_n;
	size_t written = 0;
	size_t clamped_total = 0;
	size_t win_total = 0; /* for the cost report */

	/*
	 * NOTE: scratch buffer for the unwindowed compaction below.  Allocated once
	 * and ping-ponged with `pool`; the old code let every parlay::pack allocate a
	 * fresh multi-GB sequence, which at 10000 batches meant 10000 allocate-and-
	 * first-touch cycles.  Measured effective throughput was 20-50 GB/s on a box
	 * that sustains >100, and that gap was where it went.
	 */
	parlay::sequence<size_t> pool_alt;
	bool const needs_alt = (dis_type != dis_exponential &&
							dis_type != dis_seg && dis_type != dis_uniform);
	if (needs_alt) {
		pool_alt = parlay::sequence<size_t>::uninitialized(base_n);
	}

	for (size_t b = 0; b < batch_num; b++) {
		size_t const pool_n = live_n;
		size_t const want = std::min(batch_n, pool_n);
		size_t const win_n = window_size(dis_type, param, pool_n, want);
		win_total += win_n;

		auto const flags =
			select_batch_flags(win_n, pool_n, want, dis_type, param,
							   seed0 + (b * 64ULL), clamped_total);
		auto picked = parlay::pack(parlay::iota(win_n), flags);

		/*
		 * NOTE: order *within* a batch carries no meaning, and a batch handed to
		 * the tree in ascending key order would be an unrealistically easy input.
		 * Shuffle inside the batch; the batches themselves stay in generation
		 * order, which is the entire point of this generator.  Seeded per batch --
		 * parlay::random_shuffle otherwise uses one fixed seed for every call.
		 */
		auto batch_rank = parlay::random_shuffle(
			picked, parlay::random(seed0 + (b * 64ULL) + 33));
		parlay::parallel_for(0, batch_rank.size(), [&](size_t i) {
			out[written + i] =
				Entry{ .key = static_cast<key_type>(pool[lo + batch_rank[i]]),
					   .val = { written + i } };
		});

		size_t const took = batch_rank.size();
		written += took;

		if (b % report_every == 0 || b + 1 == batch_num) {
			report(b, pool_n, win_n);
		}

		if (!needs_alt) {
			/*
			 * NOTE: windowed shapes (exp, seg) -- only the window could have lost
			 * anything, so compact just the window and leave the untouched suffix
			 * where it is.  The window's survivors are shifted RIGHT to end flush
			 * against the window's end, which makes the new live pool the suffix
			 * [lo + took, base_n) -- contiguous, still in key order, tail never
			 * moved.  That is what turns the per-batch cost into O(win_n).
			 */
			auto const survivors =
				parlay::pack(pool.cut(lo, lo + win_n),
							 parlay::delayed_tabulate(
								 win_n, [&](size_t i) { return !flags[i]; }));
			parlay::parallel_for(0, survivors.size(), [&](size_t i) {
				pool[lo + took + i] = survivors[i];
			});
			lo += took;
			live_n -= took;
		} else {
			/*
			 * NOTE: unwindowed shapes (norm, dual_norm) remove from all over the
			 * pool, so there is no suffix to spare -- but the removals are only
			 * `took` positions out of pool_n, and `picked` already lists them in
			 * ascending order.  So copy the (took + 1) runs BETWEEN the removed
			 * positions straight into the scratch buffer: run j slides left by
			 * exactly j, every destination is disjoint, and it is one read plus one
			 * write with no flags array and no allocation.
			 *
			 * Writing into a separate buffer is what makes it safely parallel -- run
			 * j's destination overlaps run j-1's SOURCE, so an in-place version would
			 * have to run sequentially.
			 */
			parlay::parallel_for(0, took + 1, [&](size_t j) {
				size_t const beg = (j == 0) ? 0 : picked[j - 1] + 1;
				size_t const end = (j == took) ? pool_n : picked[j];
				for (size_t i = beg; i < end; i++) {
					pool_alt[i - j] = pool[lo + i];
				}
			});
			std::swap(pool, pool_alt);
			lo = 0; /* the live pool is now the prefix [0, pool_n - took) */
			live_n -= took;
		}
	}

	{
		/*
		 * NOTE: total pool-work is what the runtime tracks; the ratio shows how much
		 * the front-window cut off (1.0x = no saving, i.e. every shape but exp).
		 */
		double const full =
			static_cast<double>(base_n) * static_cast<double>(batch_num) / 2.0;
		std::cout << "pool-work: " << static_cast<double>(win_total) << " vs "
				  << full << " unwindowed ("
				  << (full / std::max(1.0, static_cast<double>(win_total)))
				  << "x saved)" << std::endl;
	}

	if (clamped_total != 0) {
		std::cout
			<< "NOTE: " << clamped_total << " rank-slots across the run were "
			<< "capped at probability 1 (" << (100.0 * clamped_total / base_n)
			<< "% of base_n) -- the shape asked for more than 100% of the "
			   "keys there, so those batches are the sharpest possible "
			   "rather than the requested shape"
			<< std::endl;
	}

	return out;
}

/*
 * NOTE: the output has to be a permutation of [0, base_n).  Count, sum and max
 * pin that down for every error this generator can realistically make (a lost
 * or duplicated claim shows up in the sum); -validate 1 adds the full
 * distinctness check, which costs a hash pass over the whole key space.
 */
template <typename Entry>
void validate_permutation(parlay::sequence<Entry> const &seq,
						  size_t const base_n, bool const full)
{
	if (seq.size() != base_n) {
		throw std::runtime_error("validate: wrong number of entries.");
	}

	auto keys = parlay::delayed_tabulate(
		seq.size(), [&](size_t i) -> size_t { return seq[i].key; });
	auto const sum = parlay::reduce(keys, parlay::addm<size_t>());
	auto const max = parlay::reduce(keys, parlay::maxm<size_t>());
	size_t const want_sum = base_n % 2 == 0 ? (base_n / 2) * (base_n - 1) :
											  base_n * ((base_n - 1) / 2);

	if (sum != want_sum || max != base_n - 1) {
		std::cout << "key sum " << sum << " (expected " << want_sum << "), max "
				  << max << " (expected " << base_n - 1 << ")" << std::endl;
		throw std::runtime_error("validate: output is not a permutation.");
	}

	if (full) {
		auto materialised = parlay::tabulate(
			seq.size(), [&](size_t i) -> size_t { return seq[i].key; });
		if (parlay::remove_duplicates(materialised).size() != base_n) {
			throw std::runtime_error("validate: output has duplicate keys.");
		}
	}

	std::cout << "Validation passed." << std::endl;
}

/*
 * ---------------------------------------------------------------------------
 * HOW TO RUN
 *
 *   make -C ../build incre_data_generator -j$(nproc)
 *
 *   # 1e9 keys handed out in 100 batches of 1e7, each batch exponential over
 *   # the surviving pool
 *   ./../build/incre_data_generator -path /data/zmen002/kv_pair_incre \
 *       -base_n 1000000000 -batch_n 10000000 -val_n 1 -file_num 1 \
 *       -dis 1 -batch_op 0 -binary 1 -batch_param 50
 *
 * Writes ONE file -- the whole key space as the batch stream, not a base/batch
 * pair.  Same stem for both ops, only the directory differs:
 *   -batch_op 0 (ins) -> <path>/<dis>/<base_n>_<val_n>/ins/<base_n/batch_n>/<i>.batch.bin
 *   -batch_op 1 (del) -> <path>/<dis>/<base_n>_<val_n>/del/<base_n/batch_n>/<i>.batch.bin
 *
 * Point -p at the ins one; extract_prefixes strips ".batch" so the del path is
 * derived from it.
 *
 * -batch_param: exp -> lambda (default 10); norm -> mean,dev (default 0.5,0.2);
 * dual_norm -> mean1,dev1,mean2,dev2 (default 0.25,0.05,0.75,0.05);
 * seg -> window multiplier (default 2, i.e. draw each batch of m uniformly from
 * the leading 2m surviving keys); uni ignores it.  For exp/norm/dual_norm these
 * are fractions of the surviving pool, not absolute keys.
 *
 * Point -path at a FRESH root: the directory layout matches the existing
 * datasets on purpose (so Options::parse_insert_batch_path and
 * parse_delete_batch_path
 * resolve it unchanged), which also means it would overwrite them in place.
 * ---------------------------------------------------------------------------
 */
int main(int argc, char *argv[])
{
	commandLine cmd(argc, argv,
					"[-path <dir>] [-base_n <n>] [-batch_n <n>] [-val_n 1] "
					"[-file_num <n>] [-dis {0,1,2,3,4}] [-batch_op {0,1}] "
					"[-binary {0,1}] [-batch_param <csv>] [-validate {0,1}]");

	/*
	 * NOTE: the std::string overloads, not the char* ones -- getOptionValue
	 * returns a null pointer when the flag is absent and std::string(nullptr) is
	 * UB, which is the segfault the old data_generator has on a missing
	 * -base_param / -batch_param.
	 */
	std::string const path_in = cmd.getOptionValue("-path", "");
	auto const base_n =
		static_cast<size_t>(cmd.getOptionLongValue("-base_n", 1'000'000));
	auto const batch_n =
		static_cast<size_t>(cmd.getOptionLongValue("-batch_n", 10'000));
	int const val_n = cmd.getOptionIntValue("-val_n", 1);
	int const file_n = cmd.getOptionIntValue("-file_num", 1);
	/*
	 * PARA: 0: uniform, 1: exp, 2: norm, 3: dual norm, 4: seg -- shape of each
	 * BATCH
	 */
	int const dis_type = cmd.getOptionIntValue("-dis", 0);
	/* PARA: 0: insert, 1: delete */
	int const batch_op = cmd.getOptionIntValue("-batch_op", 0);
	/* PARA: 0: text .in, 1: binary .bin */
	int const binary = cmd.getOptionIntValue("-binary", 0);
	bool const validate = cmd.getOptionIntValue("-validate", 0) != 0;

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
		parse_param(cmd.getOptionValue("-batch_param", ""));

	std::cout << "base_n: " << base_n << " batch_n: " << batch_n
			  << " val_n: " << val_n << " file_num: " << file_n
			  << " distribution_type: " << dis_type
			  << " batch_op: " << (batch_op == 0 ? "ins" : "del")
			  << " batch_param: ";
	for (auto const &p : batch_param_seq) {
		std::cout << p << " ";
	}
	std::cout << std::endl;

	/*
	 * NOTE: `demand` is literally the inclusion probability handed to the
	 * front-most surviving key: lambda * batch_n / pool.  A probability cannot
	 * exceed 1, so demand > 1 is not a tuning problem but an impossible request,
	 * and those ranks get capped (reported per run by gen_incremental_batches).
	 * The pool drains as the run proceeds, so demand rises as
	 * demand_0 / (1 - progress) and the share of the run that is exactly on-shape
	 * is 1 - demand_0.  lambda = 0.5 * base_n / batch_n keeps the whole first
	 * half exact, which is the half the benchmark's query state is built from.
	 */
	if (dis_type == dis_exponential) {
		double const demand = static_cast<double>(batch_n) *
							  param_or(batch_param_seq, 0, 10.0) /
							  static_cast<double>(base_n);
		std::cout << "front-key probability at the start (demand): " << demand
				  << " -- exactly on-shape for the first "
				  << (100.0 * std::max(0.0, 1.0 - demand)) << "% of the run"
				  << std::endl;
		if (demand > 0.5) {
			std::cout
				<< "WARNING: demand > 0.5, so the shape degrades before the "
				   "half-way point. lambda <= "
				<< 0.5 * static_cast<double>(base_n) /
					   static_cast<double>(batch_n)
				<< " keeps the whole first half exact." << std::endl;
		}
	}

	if (dis_type == dis_seg) {
		double const mult = std::max(1.0, param_or(batch_param_seq, 0, 2.0));
		std::cout << "segment width: " << mult << " x batch_n = "
				  << static_cast<size_t>(mult * static_cast<double>(batch_n))
				  << " keys; each batch takes " << (100.0 / mult)
				  << "% of its window, and the window advances by batch_n"
				  << std::endl;
		if (mult < 1.0001) {
			std::cout
				<< "NOTE: mult == 1 means every batch is exactly the leading "
				   "batch_n surviving keys -- no randomness at all"
				<< std::endl;
		}
	}

	std::string path = path_in;
	path += std::string(*path.rbegin() == '/' ? "" : "/");
	std::filesystem::create_directories(path);

	path += std::string(dis_type == dis_uniform		? "uni/" :
						dis_type == dis_exponential ? "exp/" :
						dis_type == dis_normal		? "norm/" :
						dis_type == dis_seg			? "seg/" :
													  "dual_norm/");
	std::filesystem::create_directories(path);

	path += toString(base_n) + "_" + toString(val_n) + "/";
	std::filesystem::create_directories(path);

	path += std::string(batch_op == 0 ? "ins/" : "del/");
	std::filesystem::create_directories(path);

	path += toString(base_n / batch_n) + "/";
	std::filesystem::create_directories(path);

	/*
	 * NOTE: ".batch" for BOTH ops.  There is no base/batch pair here -- the whole
	 * file IS the batch stream, sliced into equal windows by the harness -- so
	 * naming the insert side ".base" only because that is what the old datasets
	 * called their big file made the two ops look different when they are not.
	 * Tags 1 << 6 and 1 << 7 both read ParseXBatchPath(...).batch_path, and
	 * Options::extract_prefixes strips ".batch" so -p can point straight
	 * at one.
	 */
	std::string const stem = ".batch";
	std::string const ext = binary ? ".bin" : ".in";

	using entry_type =
		wrapper<axis, std::tuple<axis>>::element<axis, std::tuple<axis>>;

	for (int i = 0; i < file_n; i++) {
		std::string const out_path = path + toString(i + 1) + stem + ext;
		std::cout << out_path << std::endl;
		rand_seed = i;

		std::cout << "Generating... " << std::endl;
		auto const seq = gen_incremental_batches<entry_type>(
			base_n, batch_n, dis_type, batch_param_seq);

		validate_permutation(seq, base_n, validate);

		std::cout << "Writing... " << std::endl;
		if (binary) {
			print_to_binary_file(out_path, seq);
		} else {
			print_to_file(out_path, seq);
		}
	}

	return 0;
}
