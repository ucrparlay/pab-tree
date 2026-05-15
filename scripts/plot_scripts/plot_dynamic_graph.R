library(dplyr)
library(tidyr)
library(ggplot2)
library(latex2exp)
library(zoo)
library(scales)
library(ggpubr)
library(ggsci)

TREE_ORDER <- c("BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree")

# args: input_type (e.g. "osm"), data_dir path
args <- commandArgs(trailingOnly = TRUE)
input_type <- ifelse(length(args) >= 1, args[1], "temp")
data_dir <- ifelse(length(args) >= 2, args[2], "../data/temp")

read_csv_op <- function(op) {
  path <- file.path(data_dir, paste0(input_type, "_", op, "_by_year.csv"))
  read.csv(path, stringsAsFactors = FALSE)
}

# plot one subplot: bars for size, lines for solver times
# t        : data frame with columns [year, size, solver1, solver2, ...]
# title    : subplot title
# size_label: left y-axis label
# time_label: right y-axis label
# show_legend: whether to show the legend
plot_op <- function(t, title, size_label, time_label, show_legend,
                    titleSize, axisSize, legendSize,
                    show_left_title = TRUE, show_right_title = TRUE) {
  solvers <- setdiff(colnames(t), c("year", "size"))
  solvers <- intersect(TREE_ORDER, solvers)  # keep defined order

  t$year <- as.integer(t$year)
  t$size <- as.numeric(t$size) / 1e6

  tb <- t %>% select(year, size)

  tc <- t %>%
    select(year, all_of(solvers)) %>%
    pivot_longer(!year, names_to = "solver", values_to = "time",
                 values_transform = list(time = as.numeric)) %>%
    mutate(solver = factor(solver, levels = solvers))

  # scale right axis to align with left log axis
  rightScaleValue <- 10^(
    mean(log10(range(tb$size, na.rm = TRUE))) -
    mean(log10(range(tc$time, na.rm = TRUE)))
  )
  tc$time_scaled <- tc$time * rightScaleValue

  shapeLevel <- c(1, 0, 2, 5, 7, 14)[seq_along(solvers)]

  plt <- ggplot(tb, aes(x = year)) +
    geom_bar(
      aes(y = size, fill = "size"),
      stat = "identity",
      color = "black",
      width = 0.5
    ) +
    scale_fill_manual("", values = "#fbf8cc", label = "Batch / Tree size") +
    geom_line(
      data = tc,
      aes(y = time_scaled, color = solver, group = solver)
    ) +
    geom_point(
      data = tc,
      aes(y = time_scaled, color = solver, shape = solver, group = solver),
      size = 2.5, stroke = 1.5
    ) +
    scale_shape_manual(values = shapeLevel) +
    scale_x_continuous(
      breaks = seq(min(tb$year), max(tb$year), by = 2),
      labels = seq(min(tb$year), max(tb$year), by = 2) - 2000
    ) +
    scale_y_log10(
      name = if (show_left_title) size_label else NULL,
      breaks = scales::trans_breaks("log10", function(x) 10^x, n = 3),
      labels = scales::trans_format("log10", scales::math_format(10^.x)),
      expand = expansion(mult = c(0, .1)),
      sec.axis = sec_axis(
        ~ . / rightScaleValue,
        name = if (show_right_title) time_label else NULL,
        breaks = scales::trans_breaks("log10", function(x) 10^x, n = 3),
        labels = scales::trans_format("log10", scales::math_format(10^.x))
      )
    ) +
    ggtitle(title) +
    theme_light() +
    theme(
      plot.title          = element_text(hjust = 0.5, size = titleSize),
      plot.title.position = "plot",
      axis.text.x         = element_text(size = axisSize - 1, color = "black"),
      axis.text.y         = element_text(size = axisSize - 1, color = "black"),
      axis.text.y.right   = element_text(size = axisSize - 1, color = "black"),
      axis.title.x        = element_blank(),
      axis.title.y        = element_text(size = axisSize, color = "black"),
      axis.title.y.right  = element_text(size = axisSize, color = "black"),
      legend.text         = element_text(size = legendSize),
      legend.title        = element_blank(),
      legend.position     = ifelse(show_legend, "top", "none"),
      legend.margin       = margin(0, 10, ifelse(show_legend, 6, 0), 0),
      legend.box.margin   = margin(0, 0, 0, 0),
      legend.spacing.x    = unit(0, "pt"),
      legend.spacing.y    = unit(0, "pt"),
      panel.grid.minor    = element_blank()
    ) +
    guides(
      fill  = guide_legend(order = 1, nrow = 1),
      color = guide_legend(order = 2, nrow = 1),
      shape = guide_legend(order = 2, nrow = 1,
                           override.aes = list(size = 3, stroke = 1))
    ) +
    scale_color_locuszoom()

  return(plt)
}

titleSize <- 13
axisSize  <- 13
legendSize <- 12
faxisSize  <- 13

tInsert  <- read_csv_op("insert")
tDelete  <- read_csv_op("delete")
tRange   <- read_csv_op("range")
tFlatten <- read_csv_op("flatten")

g1 <- plot_op(tInsert,  "(a) Batch Insert", TeX("Batch size ($\\times 1M$)"), "Time (sec.)", FALSE, titleSize, axisSize, legendSize, show_left_title = TRUE,  show_right_title = FALSE)
g2 <- plot_op(tDelete,  "(b) Batch Delete", TeX("Batch size ($\\times 1M$)"), "Time (sec.)", FALSE, titleSize, axisSize, legendSize, show_left_title = FALSE, show_right_title = TRUE)
g3 <- plot_op(tRange,   "(c) Neighbor Query",  TeX("Tree size ($\\times 1M$)"),  "Time (sec.)", FALSE, titleSize, axisSize, legendSize, show_left_title = TRUE,  show_right_title = FALSE)
g4 <- plot_op(tFlatten, "(d) Flatten",      TeX("Tree size ($\\times 1M$)"),  "Time (sec.)", TRUE,  titleSize, axisSize, legendSize, show_left_title = FALSE, show_right_title = TRUE)

g <- ggpubr::ggarrange(
  plotlist = list(g1, g2, g3, g4),
  nrow = 2,
  ncol = 2,
  common.legend = TRUE,
  legend = "top"
) + theme(plot.margin = grid::unit(c(0, 0, 0, 0), "mm"))

g <- annotate_figure(g,
  bottom = text_grob(TeX("Year of the batch"), size = faxisSize),
)

out_path <- file.path(data_dir, paste0(input_type, "_dynamic_graph.pdf"))
ggsave(out_path, width = 10, height = 6, dpi = 1800)
cat("Saved to", out_path, "\n")
