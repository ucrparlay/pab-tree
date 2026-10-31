# HOW TO RUN (from scripts_ae/, after the merge step)
#   Rscript plot_fig6_temporal_graph.R
#
# From the paper's plot_signal_bench.R: default input under data/, output in plots/.

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

# operation (dodge) order along the x-axis, with clean display labels
OP_ORDER <- c("insert", "delete", "range", "flatten")
OP_LABELS <- c(
  insert = "Insert", delete = "Delete",
  range = "Neighbor", flatten = "Flatten"
)

# benchmark (subfigure) order, with clean display titles (edit as needed)
BENCH_ORDER <- c("redd", "ref", "sem", "osm")
BENCH_LABELS <- c(
  redd = "(a) RED", ref = "(b) REF",
  sem = "(c) SEM", osm = "(d) PLANET"
)

# display ceiling for normalized time: any bar taller than CAP is clipped at
# CAP and annotated with its true value, so the close bars stay readable.
CAP <- 3

# args: signal csv path, osm csv path
args <- commandArgs(trailingOnly = TRUE)
csv_path <- ifelse(length(args) >= 1, args[1],
  "data/temp_signal/temp_signal.csv"
)
osm_path <- ifelse(length(args) >= 2, args[2],
  "data/temp_osm/temp_osm.csv"
)

read_bench <- function(path) {
  read.csv(path, stringsAsFactors = FALSE, check.names = FALSE) %>%
    select(benchmark, tree, operation, sum_t)
}

# the osm file labels its benchmark "temp_osm"; rename it to "osm"
raw <- bind_rows(
  read_bench(csv_path),
  read_bench(osm_path) %>% mutate(benchmark = "osm")
)

# keep only the sum_t (drop the per-batch time-series columns 0..N),
# clean up the operation names ("insert_t(s)" -> "insert")
dat <- raw %>%
  mutate(
    sum_t     = suppressWarnings(as.numeric(sum_t)),
    operation = sub("_t\\(s\\)$", "", operation)
  ) %>%
  filter(!is.na(sum_t), operation %in% OP_ORDER) %>%
  # normalize each bar to our PB-tree (BTree) for that (benchmark, operation):
  # PB-tree = 1.0, others = how many times slower (>1) or faster (<1)
  group_by(benchmark, operation) %>%
  mutate(norm = sum_t / sum_t[tree == "BTree"]) %>%
  ungroup()

# global, stable color/legend mapping (independent of which baselines
# happen to be present in a given benchmark)
solvers_all <- intersect(TREE_ORDER, unique(dat$tree))
solvers_all_named <- recode(solvers_all,
  "BTree" = "Ours",
  "MassTree" = "Mass-tree", "BwTree" = "Bw-tree"
)

# same bar palette as plot_randomness.R
barColorMap <- c(
  "blue" = "#a3c4f3", "yellow" = "#fbf8cc",
  "pink" = "#ffcfd2", "grey" = "#cfcfcf", "green" = "#b9fbc0"
)
barPalette <- unname(barColorMap[c("pink", "yellow", "blue", "grey", "green")])
colorLevel <- setNames(barPalette[seq_along(solvers_all)], solvers_all_named)

# plot one benchmark: dodged bars, one dodge per operation, one bar per baseline
# (geometric-mean time normalized to the fastest baseline of that operation)
# tb           : data frame for a single benchmark [tree, operation, norm]
# title        : subplot title
# show_y_title : whether to show the y-axis title
plot_bench <- function(tb, title, show_legend,
                       titleSize, axisSize, legendSize,
                       show_y_title = TRUE) {
  tb <- tb %>%
    mutate(
      tree = recode(factor(tree, levels = solvers_all),
        "BTree" = "Ours",
        "MassTree" = "Mass-tree", "BwTree" = "Bw-tree"
      ),
      tree = factor(tree, levels = solvers_all_named),
      operation = factor(operation,
        levels = OP_ORDER,
        labels = OP_LABELS[OP_ORDER]
      ),
      lab = ifelse(norm > CAP, sprintf("%.1f×", norm), NA_character_),
      # x position past each (dodged) bar's right edge, with a uniform gap.
      # bar cluster width is 0.8 (the geom_col width); right edge of slot s is
      # at  operation - 0.8/2 + s*(0.8/n).  0.15 is the gap to the label.
      x_lab = (as.integer(operation) - 0.8 / 2 +
        as.integer(tree) * (0.8 / length(solvers_all_named))) + 0.15
    )

  ggplot(tb, aes(x = operation, y = norm, fill = tree)) +
    geom_col(
      position = position_dodge2(width = 0.9, padding = 0, preserve = "single"),
      width = 0.8, color = "black", linewidth = 0.5
    ) +
    geom_hline(yintercept = 1, linetype = "dashed", color = "grey40", linewidth = 0.4) +
    # bars taller than CAP are clipped at CAP; print their true value, rotated,
    # just to the right of the clipped bar
    geom_text(
      data = function(d) d[!is.na(d$lab), ],
      aes(x = x_lab, y = CAP, label = lab),
      # geom_text sizes in mm; .pt converts from the points the theme uses
      angle = 90, hjust = 1.02, vjust = 0.5, size = (axisSize - 2) / .pt,
      color = "black", family = FONT
    ) +
    scale_fill_manual("",
      values = colorLevel, breaks = solvers_all_named,
      labels = label_expr, drop = FALSE
    ) +
    scale_x_discrete(drop = FALSE) +
    scale_y_continuous(
      name = if (show_y_title) "Normalized time" else NULL,
      breaks = seq(1, CAP, by = 1),
      expand = expansion(mult = c(0, .05))
    ) +
    coord_cartesian(ylim = c(0, CAP), clip = "on") +
    ggtitle(title) +
    theme_light(base_family = FONT) +
    theme(
      plot.title = element_text(hjust = 0.5, size = titleSize, margin = margin(t = 8, b = 8)),
      plot.title.position = "plot",
      plot.margin = margin(0, 4, 0, 4),
      # four operation names share a panel, so the tick labels run smaller than
      # the rest of the axis text or the outer two would touch
      axis.text.x = element_text(size = axisSize - 3, color = "black", margin = margin(0, 0, 0, 0)),
      axis.text.y = element_text(size = axisSize - 1, color = "black", margin = margin(0, 0, 0, 0)),
      axis.title.x = element_blank(),
      # the right margin of a rotated title is the gap toward its axis
      axis.title.y = element_text(size = axisSize - 2, color = "black", margin = margin(r = 8)),
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

titleSize <- 21
axisSize <- 22
legendSize <- 22
final_axis_title_size <- 22

FIG_W <- 9
FIG_H <- 8

# a grob is measured with whatever device is open when it is built, and cairo's
# metrics for this font differ from the default device's. built with no cairo
# device open, the legend reserves label cells that do not match the text cairo
# then draws, and the error shows up as uneven gaps between the entries. so the
# figure is assembled with a cairo device open, matching the one it is saved on.
scratch_dev <- tempfile(fileext = ".png")
png(scratch_dev, width = FIG_W, height = FIG_H, units = "in", res = 300, type = "cairo")

# ggpubr lays the legend out against its own device rather than the one the
# figure is drawn on, so the label cells come out at the default font's widths
# while cairo draws them at this font's. the error shows as uneven gaps between
# the entries. converting the legend's units to absolute points under the cairo
# device pins each cell to the width cairo will actually draw. the pinning only
# survives if nothing rebuilds the arrangement afterwards, which is why the
# legend is put on by hand below rather than by ggarrange.
freeze_units <- function(g) {
  if (inherits(g, "gtable")) {
    g$widths <- grid::unit(grid::convertWidth(g$widths, "pt", valueOnly = TRUE), "pt")
    g$heights <- grid::unit(grid::convertHeight(g$heights, "pt", valueOnly = TRUE), "pt")
    g$grobs <- lapply(g$grobs, freeze_units)
  }
  g
}

plots <- lapply(seq_along(BENCH_ORDER), function(i) {
  b <- BENCH_ORDER[i]
  tb <- dat %>% filter(benchmark == b)
  plot_bench(tb, BENCH_LABELS[[b]],
    show_legend = (i == 1),
    titleSize, axisSize, legendSize,
    show_y_title = (i %in% c(1, 3))
  )
})

# the panels alone, with the label they share underneath. a zero margin lets the
# text run into the page edge, which crops it, so each side keeps a little room.
body <- ggpubr::ggarrange(
  plotlist = lapply(plots, function(p) p + theme(legend.position = "none")),
  nrow = 2, ncol = 2
) + theme(plot.margin = grid::unit(c(2, 1, 2, 1), "mm"))

body <- annotate_figure(body,
  bottom = text_grob("Operations", size = final_axis_title_size, family = FONT)
)

# the x label is added outside the arrangement, so it needs its own room below
body <- body + theme(plot.margin = grid::unit(c(0, 0, 2, 0), "mm"))

# the legend sits on top as a grob of its own, so its pinned widths reach the
# page untouched; the blank row above it keeps the keys off the page edge
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

out_path <- file.path("plots", "fig6_temporal_graph.png")
png(out_path, width = FIG_W, height = FIG_H, units = "in", res = 300, type = "cairo")
grid::grid.draw(g)
invisible(dev.off())
cat("Saved to", out_path, "\n")
