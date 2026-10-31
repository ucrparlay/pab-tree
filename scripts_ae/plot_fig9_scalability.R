# HOW TO RUN (from scripts_ae/, after the merge step)
#   Rscript plot_fig9_scalability.R
#
# From the paper's plot_scalability.R: default input under data/, output in plots/.

library(dplyr)
library(tidyr)
library(ggplot2)
library(latex2exp)
library(scales)
library(ggpubr)
library(ggsci)

# the body font of the paper, so that the figure sets its text in the same face
# as the surrounding prose. cairo resolves the family through fontconfig, so the
# name has to match `fc-list` exactly.
FONT <- "Linux Libertine"

TREE_ORDER <- c("BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree")

# the shapes are not equally inky at one size: a triangle covers about half of
# its bounding box and a diamond about two thirds, so both read smaller than
# the circle and the squares.  each shape therefore carries its own multiplier,
# which makes the legend keys and the plotted points look the same size.
PT_SIZE <- 3
SHAPE_SIZE <- c(
  "1" = 1.00, # circle
  "0" = 0.95, # square
  "2" = 1.25, # triangle
  "5" = 1.15, # diamond
  "7" = 0.95, # square with a cross
  "14" = 0.95 # square with a triangle
)


# one row per operation, one column per key distribution
OP_ORDER <- c("build", "insert", "delete")
OP_NAMES <- c(build = "Build", insert = "Batch Insert", delete = "Batch Delete")
DIS_ORDER <- c("uni", "norm", "seg")
DIS_NAMES <- c(uni = "Uniform", norm = "Skewed", seg = "Adversarial")

# args: data_dir path
args <- commandArgs(trailingOnly = TRUE)
input_type <- "scalability"
data_dir <- ifelse(length(args) >= 1, args[1], "data/scalability")

read_csv_scalability <- function() {
  path <- file.path(data_dir, paste0(input_type, ".csv"))
  read.csv(path, stringsAsFactors = FALSE)
}

# the thread counts in the data; one twice the count before it is the run on
# every hyper-thread, labelled like the paper's 112h
CORE_ORDER <- sort(unique(read_csv_scalability()$cores))
CORE_LABELS <- as.character(CORE_ORDER)
n_cores <- length(CORE_ORDER)
if (n_cores >= 2 && CORE_ORDER[n_cores] == 2 * CORE_ORDER[n_cores - 1]) {
  CORE_LABELS[n_cores] <- paste0(CORE_ORDER[n_cores - 1], "h")
}

rename_trees <- function(x) {
  recode(x,
    "BTree" = "Ours",
    "MassTree" = "Mass-tree", "BwTree" = "Bw-tree"
  )
}

# "Ours" stays the internal name, so the ours/others split below keeps working;
# only the printed label carries the plotmath, which makes the "+" of B+-tree a
# real superscript rather than the undersized U+207A glyph. every other name is
# quoted and prints as it stands.
TREE_LABEL_EXPR <- c("Ours" = '"PaB-tree"')
label_expr <- function(br) {
  br <- as.character(br)
  txt <- unname(TREE_LABEL_EXPR[br])
  txt[is.na(txt)] <- paste0('"', br[is.na(txt)], '"')
  parse(text = txt)
}

# normalized speedup: the 1-thread time of our tree over t(n threads), so that
# the 1-thread column also shows how the trees compare sequentially.  The
# *_su columns of the CSV are self-relative and are therefore not used here.
kBaseCore <- 1
kBaseTree <- "BTree"

normalized_speedup <- function(t, op, dis) {
  tp <- t %>%
    filter(benchmark == dis, tree %in% TREE_ORDER, cores %in% CORE_ORDER)

  base <- tp %>%
    filter(tree == kBaseTree, cores == kBaseCore) %>%
    pull(!!op) %>%
    as.numeric()

  if (length(base) == 0 || !is.finite(base[1]) || base[1] <= 0) {
    stop(sprintf("no %s %s time on %d thread(s) for %s", kBaseTree, op, kBaseCore, dis))
  }

  tp %>%
    transmute(
      cores = factor(cores, levels = CORE_ORDER),
      solver = tree,
      speedup = base[1] / suppressWarnings(as.numeric(.data[[op]]))
    ) %>%
    filter(is.finite(speedup), speedup > 0)
}

# plot one subplot: normalized speedup per tree, one line per tree
# t          : the full scalability table
# op         : build | insert | delete
# dis        : uni | exp
# title      : subplot title
# show_legend: whether to show the legend
# y_limits   : common y range, so that all six panels share one axis
plot_op <- function(t, op, dis, title, show_legend,
                    titleSize, axisSize, legendSize, y_limits) {
  tc <- normalized_speedup(t, op, dis)

  solvers <- intersect(TREE_ORDER, unique(tc$solver))
  solvers_named <- rename_trees(solvers)

  tc <- tc %>%
    mutate(solver = factor(solver, levels = solvers)) %>%
    arrange(solver == "BTree") %>%
    mutate(solver = factor(rename_trees(as.character(solver)), levels = solvers_named))

  shapeLevel <- setNames(c(1, 0, 2, 5, 7, 14)[seq_along(solvers)], solvers_named)
  sizeLevel <- setNames(PT_SIZE * SHAPE_SIZE[as.character(shapeLevel)], solvers_named)
  colorLevel <- setNames(pal_locuszoom()(length(solvers)), solvers_named)

  tc_others <- tc %>% filter(solver != "Ours")
  tc_ours <- tc %>% filter(solver == "Ours")

  plt <- ggplot(tc, aes(x = cores)) +
    geom_line(
      data = tc_others,
      aes(y = speedup, color = solver, group = solver),
      linewidth = 0.8
    ) +
    geom_point(
      data = tc_others,
      aes(y = speedup, color = solver, shape = solver, size = solver, group = solver),
      stroke = 1.3
    ) +
    geom_line(
      data = tc_ours,
      aes(y = speedup, color = solver, group = solver),
      linewidth = 0.8
    ) +
    geom_point(
      data = tc_ours,
      aes(y = speedup, color = solver, shape = solver, size = solver, group = solver),
      stroke = 1.3
    ) +
    scale_shape_manual(values = shapeLevel, breaks = solvers_named, labels = label_expr) +
    scale_size_manual(values = sizeLevel, breaks = solvers_named, labels = label_expr) +
    scale_x_discrete(breaks = as.character(CORE_ORDER), labels = CORE_LABELS) +
    scale_y_log10(
      name = NULL,
      breaks = c(0.1, 1, 10, 100),
      labels = c("0.1", "1", "10", "100"),
      limits = y_limits,
      expand = expansion(mult = c(.06, .06))
    ) +
    annotation_logticks(
      sides = "l", outside = TRUE,
      long = unit(0.16, "cm"),
      mid = unit(0.10, "cm"),
      short = unit(0.06, "cm")
    ) +
    coord_cartesian(clip = "off") +
    ggtitle(title) +
    theme_light(base_family = FONT) +
    theme(
      plot.title          = element_text(hjust = 0.5, size = titleSize, margin = margin(t = 8, b = 8)),
      plot.title.position = "plot",
      plot.margin         = margin(0, 4, 0, 0),
      axis.text.x         = element_text(size = axisSize - 1, angle = 20, color = "black", margin = margin(0, 0, 0, 0)),
      # the log ticks are an annotation, so ggplot reserves no room for them:
      # keep the labels clear of the longest tick by hand
      axis.text.y         = element_text(size = axisSize - 1, color = "black", margin = margin(r = 5)),
      axis.title          = element_blank(),
      legend.text         = element_text(size = legendSize),
      legend.title        = element_blank(),
      legend.position     = ifelse(show_legend, "top", "none"),
      legend.margin       = margin(0, 10, ifelse(show_legend, 6, 0), 0),
      legend.box.margin   = margin(0, 0, 0, 0),
      legend.spacing.x    = unit(0, "pt"),
      legend.spacing.y    = unit(0, "pt"),
      legend.box          = "vertical",
      panel.grid.minor    = element_blank()
    ) +
    guides(
      # six entries have to fit on one row of a 6-inch canvas, so the keys are
      # narrower than the ggplot default
      color = guide_legend(
        order = 1, nrow = 1,
        keywidth = unit(0.9, "lines"), keyheight = unit(1.0, "lines")
      ),
      # no size in override.aes: it would flatten the per-shape sizes again
      shape = guide_legend(
        order = 1, nrow = 1,
        keywidth = unit(0.9, "lines"), keyheight = unit(1.0, "lines"),
        override.aes = list(stroke = 1)
      ),
      # the three guides merge into one key, so only the shape guide states
      # override.aes; repeating it here would only raise a warning
      size = guide_legend(
        order = 1, nrow = 1,
        keywidth = unit(0.9, "lines"), keyheight = unit(1.0, "lines")
      )
    ) +
    scale_color_manual(values = colorLevel, breaks = solvers_named, labels = label_expr)

  return(plt)
}

titleSize <- 18
axisSize <- 16
legendSize <- 19
final_axis_title_size <- 19

# the canvas grows with the third column so that a panel keeps the width it had
# with two, and the figure keeps its aspect: on the page it occupies the same
# box, with each panel a third of it rather than a half
FIG_W <- 8
FIG_H <- 8

# a grob is measured with whatever device is open when it is built, so the
# figure is assembled with a cairo device open, matching the one it is drawn on
scratch_dev <- tempfile(fileext = ".png")
png(scratch_dev, width = FIG_W, height = FIG_H, units = "in", res = 300, type = "cairo")

t <- read_csv_scalability()

# the panels, row by row: one row per operation, one column per distribution
PANEL_SPEC <- unlist(
  lapply(OP_ORDER, function(op) {
    lapply(DIS_ORDER, function(dis) list(op = op, dis = dis))
  }),
  recursive = FALSE
)

# every panel draws the same quantity, so they share one y range
su_all <- unlist(lapply(PANEL_SPEC, function(s) {
  normalized_speedup(t, s$op, s$dis)$speedup
}))
y_limits <- c(min(su_all), max(su_all))

YLAB <- "Normalized speedup"

panels <- lapply(seq_along(PANEL_SPEC), function(k) {
  s <- PANEL_SPEC[[k]]
  title <- paste0(OP_NAMES[[s$op]], ", ", DIS_NAMES[[s$dis]])
  plot_op(t, s$op, s$dis, title,
    show_legend = (k == 1),
    titleSize, axisSize, legendSize, y_limits
  )
})

# ggpubr lays the legend out against its own device rather than the one the
# figure is drawn on, so the label cells come out at the default font's widths
# while cairo draws them at this font's, and entries collide or drift apart.
# converting the legend's units to absolute points under the cairo device pins
# each cell to the width cairo will actually draw. the pinning only survives if
# nothing rebuilds the arrangement afterwards, so the legend goes on by hand,
# after the panels and their labels are assembled.
freeze_units <- function(g) {
  if (inherits(g, "gtable")) {
    g$widths <- grid::unit(grid::convertWidth(g$widths, "pt", valueOnly = TRUE), "pt")
    g$heights <- grid::unit(grid::convertHeight(g$heights, "pt", valueOnly = TRUE), "pt")
    g$grobs <- lapply(g$grobs, freeze_units)
  }
  g
}

body <- ggpubr::ggarrange(
  plotlist = lapply(panels, function(p) p + theme(legend.position = "none")),
  nrow = length(OP_ORDER),
  ncol = length(DIS_ORDER)
) + theme(plot.margin = grid::unit(c(2, 2, 0, 2), "mm"))

# the panels form one block, so a single title serves them all
body <- annotate_figure(body,
  bottom = text_grob(TeX("Number of threads"), size = final_axis_title_size + 1, family = FONT),
  left   = text_grob(YLAB, size = final_axis_title_size, rot = 90, family = FONT)
)

# the labels are added outside the arrangement, so they need their own room;
# a zero margin would crop their descenders at the page edge
body <- body + theme(plot.margin = grid::unit(c(0, 0, 2, 0), "mm"))

legend_grob <- freeze_units(ggpubr::get_legend(panels[[1]]))
g <- gridExtra::arrangeGrob(
  grid::nullGrob(), legend_grob, ggplotGrob(body),
  ncol = 1,
  heights = grid::unit.c(
    grid::unit(2, "mm"), grid::grobHeight(legend_grob), grid::unit(1, "null")
  )
)

invisible(dev.off())
unlink(scratch_dev)

out_path <- file.path("plots", "fig9_scalability.png")
png(out_path, width = FIG_W, height = FIG_H, units = "in", res = 300, type = "cairo")
grid::grid.draw(g)
invisible(dev.off())
cat("Saved to", out_path, "\n")
