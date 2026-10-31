# HOW TO RUN (from scripts_ae/, after the merge step)
#   Rscript plot_fig5_osm.R
#
# From the paper's plot_osm_graph.R: default input under data/, output in plots/.

library(dplyr)
library(tidyr)
library(ggplot2)
library(latex2exp)
library(zoo)
library(scales)
library(ggpubr)
library(ggsci)

# the body font of the paper, so that the figure sets its text in the same face
# as the surrounding prose. cairo_pdf resolves the family through fontconfig and
# embeds it, so the name has to match `fc-list` exactly.
FONT <- "Linux Libertine"

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

barColorMap <- c("blue" = "#a3c4f3", "yellow" = "#fbf8cc",
                 "pink" = "#ffcfd2", "grey"   = "#cfcfcf", "green" = "#b9fbc0")

# args: data_dir path
args <- commandArgs(trailingOnly = TRUE)
input_type <- "osm"
data_dir <- ifelse(length(args) >= 1, args[1], "data/osm")

read_csv_op <- function(op) {
  path <- file.path(data_dir, paste0(input_type, "_", op, "_by_year.csv"))
  read.csv(path, stringsAsFactors = FALSE)
}

# plot one subplot: lines for solver times
# t          : data frame with columns [year, solver1, solver2, ...]
# title      : subplot title
# time_label : y-axis label
# show_legend: whether to show the legend
# show_y_title: whether to show the y-axis title
plot_op <- function(t, title, time_label, show_legend,
                    titleSize, axisSize, legendSize,
                    show_y_title = TRUE, x_min = NULL) {
  solvers <- intersect(TREE_ORDER, colnames(t))

  t$year <- as.integer(t$year)
  x_start <- if (!is.null(x_min)) x_min else min(t$year)
  year_breaks <- seq(x_start, max(t$year), by = 1)

  tc <- t %>%
    select(year, all_of(solvers)) %>%
    pivot_longer(!year, names_to = "solver", values_to = "time",
                 values_transform = list(time = as.numeric)) %>%
    mutate(solver = factor(solver, levels = solvers)) %>%
    arrange(solver == "BTree") %>%
    mutate(solver = recode(solver, "BTree" = "Ours",
                           "MassTree" = "Mass-tree", "BwTree" = "Bw-tree"))

  solvers_named <- recode(solvers, "BTree" = "Ours",
                          "MassTree" = "Mass-tree", "BwTree" = "Bw-tree")
  shapeLevel <- setNames(c(1, 0, 2, 5, 7, 14)[seq_along(solvers)], solvers_named)
  colorLevel <- setNames(pal_locuszoom()(length(solvers)), solvers_named)

  tc_others <- tc %>% filter(solver != "Ours")
  tc_ours   <- tc %>% filter(solver == "Ours")

  plt <- ggplot(tc, aes(x = year)) +
    geom_line(
      data = tc_others,
      aes(y = time, color = solver, group = solver),
      linewidth = 0.8
    ) +
    geom_point(
      data = tc_others,
      aes(y = time, color = solver, shape = solver, group = solver),
      size = 3, stroke = 1.3
    ) +
    geom_line(
      data = tc_ours,
      aes(y = time, color = solver, group = solver),
      linewidth = 0.8
    ) +
    geom_point(
      data = tc_ours,
      aes(y = time, color = solver, shape = solver, group = solver),
      size = 3, stroke = 1.3
    ) +
    scale_shape_manual(values = shapeLevel, breaks = solvers_named, labels = label_expr) +
    scale_x_continuous(
      breaks = year_breaks,
      labels = year_breaks - 2000,
      limits = c(x_start, max(t$year))
    ) +
    scale_y_log10(
      name = if (show_y_title) time_label else NULL,
      # the axis is still logarithmic, but the ticks are written as the numbers
      # they are rather than as powers of ten
      breaks = scales::trans_breaks("log10", function(x) 10^x, n = 2),
      labels = scales::label_number(drop0trailing = TRUE),
      expand = expansion(mult = c(.1, .1))
    ) +
    annotation_logticks(sides = "l", outside = TRUE) +
    coord_cartesian(clip = "off") +
    ggtitle(title) +
    theme_light(base_family = FONT) +
    theme(
      plot.title          = element_text(hjust = 0.5, size = titleSize, margin = margin(t = 8, b =8)),
      plot.title.position = "plot",
      plot.margin         = margin(0, 0, 0, 0),
      axis.text.x         = element_text(size = axisSize - 1, color = "black", margin = margin(0, 0, 0, 0)),
      # the log ticks are an annotation drawn outside the panel, so ggplot
      # reserves no room for them: keep the labels clear of them by hand
      axis.text.y         = element_text(size = axisSize - 1, color = "black", margin = margin(r = 6)),
      axis.title.x        = element_blank(),
      axis.title.y        = element_text(size = axisSize - 1, color = "black", margin = margin(0, 0, 0, 0)),
      legend.text         = element_text(size = legendSize),
      legend.title        = element_blank(),
      legend.position     = ifelse(show_legend, "top", "none"),
      legend.margin       = margin(0, 10, ifelse(show_legend, 0, 0), 0),
      legend.box.margin   = margin(0, 0, 0, 0),
      legend.spacing.x    = unit(0, "pt"),
      legend.spacing.y    = unit(0, "pt"),
      # six entries share one row, so the gap between them is tighter than the
      # ggplot default, but wide enough that a label never crowds the next key
      legend.key.spacing.x = unit(10, "pt"),
      legend.box          = "vertical",
      panel.grid.minor    = element_blank()
    ) +
    guides(
      color = guide_legend(order = 1, nrow = 1,
                           keywidth = unit(0.9, "lines"), keyheight = unit(1.0, "lines")),
      shape = guide_legend(order = 1, nrow = 1,
                           keywidth = unit(0.9, "lines"), keyheight = unit(1.0, "lines"),
                           override.aes = list(size = 4, stroke = 1.2))
    ) +
    scale_color_manual(values = colorLevel, breaks = solvers_named, labels = label_expr)

  return(plt)
}

# plot dodged bar chart: batch size, overflow, underflow per year (normalized to tree size)
plot_size_bars <- function(t_insert, t_delete, t_tree,
                           title, show_legend,
                           titleSize, axisSize, legendSize) {
  tree_size <- as.numeric(t_tree$size)
  df <- data.frame(
    year         = as.integer(t_tree$year),
    insert_batch = as.numeric(t_insert$size)                / tree_size,
    overflow     = as.numeric(t_insert$split_leaf_size)     / tree_size,
    delete_batch = as.numeric(t_delete$size)                / tree_size,
    underflow    = (as.numeric(t_delete$reinsert_size) +
                    as.numeric(t_delete$leaf_balance_size)) / tree_size
  )
  first      <- df$year == min(df$year)
  insert_sz  <- as.numeric(t_insert$size[first])
  df$insert_batch[first] <- NA
  df$overflow[first]     <- NA
  df$delete_batch[first] <- as.numeric(t_delete$size[first])                / insert_sz
  df$underflow[first]    <- (as.numeric(t_delete$reinsert_size[first]) +
                              as.numeric(t_delete$leaf_balance_size[first])) / insert_sz

  bar_levels <- c("insert_batch", "overflow", "delete_batch", "underflow")
  for (b in bar_levels) {
    df[[b]][!is.na(df[[b]]) & df[[b]] == 0] <- NA
  }
  bar_labels <- c(insert_batch = "Insert Batch",
                  overflow     = "Insert Overflow",
                  delete_batch = "Delete Batch",
                  underflow    = "Delete Underflow")
  bar_colors <- c(insert_batch = barColorMap[["yellow"]],
                  overflow     = barColorMap[["pink"]],
                  delete_batch = barColorMap[["blue"]],
                  underflow    = barColorMap[["grey"]])
  df_long <- df %>%
    pivot_longer(!year, names_to = "bar_type", values_to = "value") %>%
    mutate(bar_type = factor(bar_type, levels = bar_levels))

  year_breaks <- seq(min(df$year), max(df$year), by = 1)
  dodge_w          <- 1.0

  n_bars   <- length(bar_levels)
  step     <- dodge_w / n_bars
  bar_offsets <- setNames(
    ((seq_len(n_bars) - 1) - (n_bars - 1) / 2) * step,
    bar_levels
  )
  empty_crosses <- do.call(rbind, lapply(bar_levels, function(b) {
    yrs <- df$year[is.na(df[[b]])]
    if (length(yrs) == 0) return(NULL)
    data.frame(x = yrs + bar_offsets[[b]])
  }))

  ggplot(df_long, aes(x = year, y = value, fill = bar_type)) +
    geom_bar(stat = "identity", color = "black", linewidth = 0.3,
             position = position_dodge2(width = dodge_w, padding = 0, preserve = "single"),
             width = 0.75) +
    annotate("text", x = empty_crosses$x, y = 0, label = "×",
             vjust = 0, size = 6, fontface = "bold", color = "red", family = FONT) +
    scale_fill_manual("", values = bar_colors, labels = bar_labels) +
    scale_x_continuous(breaks = year_breaks, labels = year_breaks - 2000) +
    scale_y_continuous(
      name = "Fraction of tree size",
      expand = expansion(mult = c(0, .1))
    ) +
    ggtitle(title) +
    theme_light(base_family = FONT) +
    theme(
      plot.title          = element_text(hjust = 0.5, size = titleSize, margin = margin(t = 8, b = 10)),
      plot.title.position = "plot",
      plot.margin         = margin(0, 0, 0, 0),
      axis.text.x         = element_text(size = axisSize - 1, color = "black", margin = margin(0,0,0,0)),
      axis.text.y         = element_text(size = axisSize - 1, color = "black", margin = margin(0,0,0,0)),
      axis.title.x        = element_blank(),
      axis.title.y        = element_text(size = axisSize, color = "black", margin = margin(0,0,0,0)),
      legend.text         = element_text(size = legendSize),
      legend.title        = element_blank(),
      legend.position     = ifelse(show_legend, "top", "none"),
      legend.margin       = margin(0, 10, ifelse(show_legend, 2, 0), 0),
      legend.box.margin   = margin(0, 0, 0, 0),
      legend.spacing.x    = unit(0, "pt"),
      legend.spacing.y    = unit(0, "pt"),
      # the four labels are long, so the width the wider keys take is given
      # back by tightening the gap between the entries
      legend.key.spacing.x = unit(2, "pt"),
      panel.grid.minor    = element_blank()
    ) +
    guides(
      fill = guide_legend(order = 1, nrow = 1,
                          keywidth  = unit(0.75, "lines"),
                          keyheight = unit(1.0, "lines"))
    )
}

titleSize <-   19
axisSize  <-   19
legendSize <-   20
final_axis_title_size  <-   20

last_n_years <- function(df, n = 5) {
  yrs <- sort(unique(df$year))
  df %>% filter(year %in% tail(yrs, n))
}

tInsert_raw <- read_csv_op("insert")
tDelete_raw <- read_csv_op("delete")

tInsert  <- tInsert_raw %>% rename(size = insert_n) %>% select(-tree_n)
tDelete  <- tDelete_raw %>% rename(size = delete_n) %>% select(-tree_n) %>% filter(size != 0)
tRange   <- read_csv_op("range")  %>% rename(size = tree_n)
tMember  <- read_csv_op("member") %>% rename(size = tree_n)

tInsert_bars <- tInsert_raw %>% rename(size = insert_n)
tDelete_bars <- tDelete_raw %>% rename(size = delete_n)
tTree_bars   <- read_csv_op("range") %>% rename(size = tree_n)

g1 <- plot_op(tInsert,  "(a) Batch Insert",   "Time (sec.)", FALSE, titleSize, axisSize, legendSize, show_y_title = TRUE)
g2 <- plot_op(tDelete,  "(b) Batch Delete",   "Time (sec.)", TRUE,  titleSize, axisSize, legendSize, show_y_title = FALSE, x_min = 2014)
g3 <- plot_op(tMember,  "(c) Find Query",   "Time (sec.)", FALSE, titleSize, axisSize, legendSize, show_y_title = TRUE)
g4 <- plot_op(tRange,   "(d) Range Report", "Time (sec.)", FALSE, titleSize, axisSize, legendSize, show_y_title = FALSE)
# the size breakdown names four quantities rather than six trees, and its labels
# are long, so its legend runs smaller than the one above it or it would not fit
# the width
barLegendSize <- 18
g5 <- plot_size_bars(tInsert_bars, tDelete_bars, tTree_bars, "(e) Size Breakdown",
                     TRUE, titleSize, axisSize, barLegendSize)

FIG_W <- 8
FIG_H <- 9

# a grob is measured with whatever device is open when it is built, so the
# figure is assembled with a cairo device open, matching the one it is drawn on
scratch_dev <- tempfile(fileext = ".pdf")
cairo_pdf(scratch_dev, width = FIG_W, height = FIG_H)

# ggpubr lays the legend out against its own device rather than the one the
# figure is drawn on, so the label cells come out at the default font's widths
# while cairo draws them at this font's, and the entries collide or drift apart.
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

g_lines <- ggpubr::ggarrange(
  plotlist = lapply(list(g1, g2, g3, g4), function(p) p + theme(legend.position = "none")),
  nrow = 2,
  ncol = 2
) + theme(plot.margin = grid::unit(c(0, 0, 0, 0), "mm"))

body <- ggpubr::ggarrange(
  g_lines, g5,
  nrow = 2,
  heights = c(2, 1.2)
) + theme(plot.margin = grid::unit(c(2, 2, 0, 2), "mm"))

body <- annotate_figure(body,
  bottom = text_grob(TeX("Year of the batch"), size = final_axis_title_size, family = FONT),
)

# the label is added outside the arrangement, so it needs its own room; a zero
# margin would crop its descenders at the page edge
body <- body + theme(plot.margin = grid::unit(c(0, 0, 2, 0), "mm"))

# the blank row above the legend keeps the keys off the page edge. nothing is
# added below it: the top margin of the panel titles already separates the two
legend_grob <- freeze_units(ggpubr::get_legend(g2))
g <- gridExtra::arrangeGrob(
  grid::nullGrob(), legend_grob, ggplotGrob(body),
  ncol = 1,
  heights = grid::unit.c(
    grid::unit(2, "mm"), grid::grobHeight(legend_grob), grid::unit(1, "null")
  )
)

invisible(dev.off())
unlink(scratch_dev)

out_path <- file.path("plots", "fig5_osm.pdf")
cairo_pdf(out_path, width = FIG_W, height = FIG_H)
grid::grid.draw(g)
invisible(dev.off())
cat("Saved to", out_path, "\n")
