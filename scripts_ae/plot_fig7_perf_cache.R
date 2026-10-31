# HOW TO RUN (from scripts_ae/, after the merge step)
#   Rscript plot_fig7_perf_cache.R [data/perf_cache/perf_cache.csv] [norm]
#
# From the paper's plot_perf_cache.R: default input under data/, output in plots/.

library(dplyr)
library(tidyr)
library(ggplot2)
library(latex2exp)
library(scales)
library(ggpubr)

# the body font of the paper, so that the figure sets its text in the same face
# as the surrounding prose. cairo resolves the family through fontconfig, so the
# name has to match `fc-list` exactly.
FONT <- "Linux Libertine"

# Rscript plot_fig7_perf_cache.R data/perf_cache/perf_cache.csv norm

# tree (baseline) order; "BTree" is ours
TREE_ORDER <- c("BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree")

# "Ours" stays the internal name, so the ours/others split keeps working; only
# the printed label carries the plotmath, which lets a name carry markup
# such as a superscript when it needs one. every other name is
# quoted and prints as it stands.
TREE_LABEL_EXPR <- c("Ours" = '"PaB-tree"')
label_expr <- function(br) {
  br <- as.character(br)
  txt <- unname(TREE_LABEL_EXPR[br])
  txt[is.na(txt)] <- paste0('"', br[is.na(txt)], '"')
  parse(text = txt)
}

# counter (dodge) order along the x-axis, with clean display labels.
# read.csv turns "cache-misses" into "cache.misses"
METRIC_ORDER <- c("cycles", "instructions", "misses")
METRIC_LABELS <- c(
  cycles = "Cycles", instructions = "Inst.", misses = "Cache misses"
)

# region (subfigure) order, with clean display titles. "membership" is the
# half-half probe: half the queried keys are in the tree and half are not.
# the count in a title is how many elements the region touches: the whole input
# for build and for the incremental sweeps, which insert or delete it a batch at
# a time, and a hundredth of it for the probe.
REGION_ORDER <- c("build", "membership", "incre_insert", "incre_delete")
REGION_LABELS <- c(
  build = "(a) Build (1B)", membership = "(b) Find (10M)",
  incre_insert = "(c) Incremental Insert (1B)",
  incre_delete = "(d) Incremental Delete (1B)"
)

# args: perf_cache_metrics csv path, distribution
args <- commandArgs(trailingOnly = TRUE)
# the normal distribution is profiled by the incremental sweep, which is the
# only one holding it: the uni/exp sweep covers those two distributions alone
csv_path <- ifelse(length(args) >= 1, args[1],
  "data/perf_cache/perf_cache.csv"
)
# the counters are profiled for several distributions; this figure plots one
DIS <- ifelse(length(args) >= 2, args[2], "norm")

raw <- read.csv(csv_path, stringsAsFactors = FALSE)

# the raw counters are normalized rather than the *_per_elem ones: within a
# region every tree processes the same region_n, so the two give the same ratio.
dat <- raw %>%
  filter(dis == DIS, tree %in% TREE_ORDER, region %in% REGION_ORDER) %>%
  select(region, tree, cycles, instructions, misses = cache.misses) %>%
  pivot_longer(c(cycles, instructions, misses),
    names_to = "metric", values_to = "count"
  ) %>%
  # normalize each bar to ours for that (region, counter): ours = 1.0, others =
  # how many times more (>1) or fewer (<1) events
  group_by(region, metric) %>%
  mutate(norm = count / count[tree == "BTree"]) %>%
  ungroup()

# global, stable color/legend mapping (independent of which baselines
# happen to be present in a given region)
solvers_all <- intersect(TREE_ORDER, unique(dat$tree))
solvers_all_named <- recode(solvers_all,
  "BTree" = "Ours",
  "MassTree" = "Mass-tree", "BwTree" = "Bw-tree"
)

# same bar palette as plot_signal_bench.R: pink marks ours
barColorMap <- c(
  "blue" = "#a3c4f3", "yellow" = "#fbf8cc",
  "pink" = "#ffcfd2", "grey" = "#cfcfcf", "green" = "#b9fbc0"
)
barPalette <- unname(barColorMap[c("pink", "yellow", "blue", "grey", "green")])
colorLevel <- setNames(barPalette[seq_along(solvers_all)], solvers_all_named)

# plot one region: dodged bars, one dodge per counter, one bar per baseline
# (event count normalized to ours for that counter)
# tb           : data frame for a single region [tree, metric, norm]
# title        : subplot title
# show_y_title : whether to show the y-axis title
# cap          : display ceiling; a bar above it is clipped there and annotated
#                with its true value, so the bars near 1 stay readable. NULL
#                lets the panel take the range its own data spans.
plot_region <- function(tb, title, show_legend,
                        titleSize, axisSize, legendSize,
                        show_y_title = TRUE, cap = NULL) {
  tb <- tb %>%
    mutate(
      tree = recode(factor(tree, levels = solvers_all),
        "BTree" = "Ours",
        "MassTree" = "Mass-tree", "BwTree" = "Bw-tree"
      ),
      tree = factor(tree, levels = solvers_all_named),
      metric = factor(metric,
        levels = METRIC_ORDER,
        labels = METRIC_LABELS[METRIC_ORDER]
      ),
      lab = if (is.null(cap)) {
        NA_character_
      } else {
        ifelse(norm > cap, sprintf("%.1f×", norm), NA_character_)
      },
      # x position past each (dodged) bar's right edge, with a uniform gap.
      # bar cluster width is 0.8 (the geom_col width); right edge of slot s is
      # at  metric - 0.8/2 + s*(0.8/n).  0.12 is the gap to the label.
      x_lab = (as.integer(metric) - 0.8 / 2 +
        as.integer(tree) * (0.8 / length(solvers_all_named))) + 0.12
    )

  ggplot(tb, aes(x = metric, y = norm, fill = tree)) +
    geom_col(
      position = position_dodge2(width = 0.9, padding = 0, preserve = "single"),
      width = 0.7, color = "black", linewidth = 0.5
    ) +
    geom_hline(yintercept = 1, linetype = "dashed", color = "grey40", linewidth = 0.4) +
    # a bar taller than the cap is clipped there; print its true value, rotated,
    # just to the right of the clipped bar
    geom_text(
      data = function(d) d[!is.na(d$lab), ],
      aes(x = x_lab, y = cap, label = lab),
      # geom_text sizes in mm; .pt converts from the points the theme uses
      angle = 90, hjust = 1.00, vjust = 0.5, size = (axisSize - 4) / .pt,
      color = "black", family = FONT
    ) +
    scale_fill_manual("",
      values = colorLevel, breaks = solvers_all_named,
      labels = label_expr, drop = FALSE
    ) +
    scale_x_discrete(drop = FALSE) +
    scale_y_continuous(
      name = if (show_y_title) "Normalized count" else NULL,
      # the panels are small, so the axis carries few labels. zero is dropped:
      # every bar starts there, so the tick states nothing the baseline does not
      breaks = function(lims) {
        b <- scales::breaks_extended(n = 3)(lims)
        b[b > 0]
      },
      expand = expansion(mult = c(0, .05))
    ) +
    # each panel takes its own range, so the small differences stay legible
    # where one baseline runs away with the scale
    coord_cartesian(ylim = if (is.null(cap)) NULL else c(0, cap), clip = "on") +
    ggtitle(title) +
    theme_light(base_family = FONT) +
    theme(
      plot.title = element_text(hjust = 0.5, size = titleSize, margin = margin(t = 8, b = 8)),
      plot.title.position = "plot",
      plot.margin = margin(0, 4.5, 0, 4),
      # "Cache misses" is the widest of the three and still fits a panel-third
      axis.text.x = element_text(size = axisSize - 3, color = "black", margin = margin(0, 0, 0, 0)),
      axis.text.y = element_text(size = axisSize - 3, color = "black", margin = margin(0, 0, 0, 0)),
      axis.title.x = element_blank(),
      # the right margin of a rotated title is the gap toward its axis. the
      # title runs smaller than the axis text: it names the quantity, while the
      # numbers are what a reader compares
      axis.title.y = element_text(size = axisSize - 4, color = "black", margin = margin(r = 8)),
      legend.text = element_text(size = legendSize),
      legend.title = element_blank(),
      legend.position = ifelse(show_legend, "top", "none"),
      legend.margin = margin(0, 10, ifelse(show_legend, 6, 0), 0),
      legend.box.margin = margin(0, 0, 0, 0),
      legend.spacing.x = unit(0, "pt"),
      legend.spacing.y = unit(0, "pt"),
      legend.key.spacing.x = unit(18, "pt"),
      axis.ticks.y.left = element_line(color = "black", linewidth = 0.4),
      axis.ticks.length.y.left = unit(4, "pt"),
      panel.grid.minor = element_blank()
    ) +
    guides(
      fill = guide_legend(
        order = 1, nrow = 1,
        keywidth = unit(1.4, "lines"),
        keyheight = unit(1.0, "lines")
      )
    )
}

titleSize <- 16
axisSize <- 17
legendSize <- 15
final_axis_title_size <- 16

FIG_W <- 6
FIG_H <- 5

# a grob is measured with whatever device is open when it is built, so the
# figure is assembled with a cairo device open, matching the one it is drawn on
scratch_dev <- tempfile(fileext = ".png")
png(scratch_dev, width = FIG_W, height = FIG_H, units = "in", res = 300, type = "cairo")

# ggpubr lays the legend out against its own device rather than the one the
# figure is drawn on, so the label cells come out at the default font's widths
# while cairo draws them at this font's, and the entries drift apart unevenly.
# pinning the units to absolute points under cairo fixes each cell to the width
# cairo will draw; the pinning only survives if nothing rebuilds the arrangement
# afterwards, so the legend goes on by hand once the body is assembled.
freeze_units <- function(g) {
  if (inherits(g, "gtable")) {
    g$widths <- grid::unit(grid::convertWidth(g$widths, "pt", valueOnly = TRUE), "pt")
    g$heights <- grid::unit(grid::convertHeight(g$heights, "pt", valueOnly = TRUE), "pt")
    g$grobs <- lapply(g$grobs, freeze_units)
  }
  g
}

# incremental insert is the one region where a baseline runs far past the rest,
# so it is capped and the clipped bars state their value; the others are left to
# the range their own data spans
REGION_CAP <- c(incre_insert = 3)

plots <- lapply(seq_along(REGION_ORDER), function(i) {
  rg <- REGION_ORDER[i]
  tb <- dat %>% filter(region == rg)
  plot_region(tb, REGION_LABELS[[rg]],
    show_legend = (i == 1),
    titleSize, axisSize, legendSize,
    show_y_title = (i %in% c(1, 3)),
    cap = if (rg %in% names(REGION_CAP)) REGION_CAP[[rg]] else NULL
  )
})

body <- ggpubr::ggarrange(
  plotlist = lapply(plots, function(p) p + theme(legend.position = "none")),
  nrow = 2, ncol = 2
) + theme(plot.margin = grid::unit(c(2, 2, 0, 2), "mm"))

body <- annotate_figure(body,
  bottom = text_grob("Hardware counter", size = final_axis_title_size + 1, family = FONT)
)

# the label is added outside the arrangement, so it needs its own room; a zero
# margin would crop its descenders at the page edge
body <- body + theme(plot.margin = grid::unit(c(0, 0, 2, 0), "mm"))

legend_grob <- freeze_units(ggpubr::get_legend(plots[[1]]))
g <- gridExtra::arrangeGrob(
  grid::nullGrob(), legend_grob, ggplotGrob(body),
  ncol = 1,
  heights = grid::unit.c(
    grid::unit(2, "mm"), grid::grobHeight(legend_grob), grid::unit(1, "null")
  )
)

invisible(dev.off())
unlink(scratch_dev)

# the figure the paper includes keeps the plain name; any other distribution is
# written beside it under its own name
out_name <- if (DIS == "norm") "fig7_perf_cache.png" else paste0("fig7_perf_cache_", DIS, ".png")
out_path <- file.path("plots", out_name)
png(out_path, width = FIG_W, height = FIG_H, units = "in", res = 300, type = "cairo")
grid::grid.draw(g)
invisible(dev.off())
cat("Saved to", out_path, "\n")
