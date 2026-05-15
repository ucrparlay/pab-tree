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
                    show_left_title = TRUE, show_right_title = TRUE,
                    right_scale_divisor = 1,
                    extra_bars = NULL,
                    bar_scale = 1e6) {
  all_bar_cols <- c("size", if (!is.null(extra_bars)) extra_bars else character(0))
  solvers <- setdiff(colnames(t), c("year", all_bar_cols))
  solvers <- intersect(TREE_ORDER, solvers)  # keep defined order

  t$year <- as.integer(t$year)
  for (col in all_bar_cols) t[[col]] <- as.numeric(t[[col]]) / bar_scale

  tb_size <- t %>% select(year, size)
  year_breaks <- seq(min(tb_size$year), max(tb_size$year), by = 2)

  tc <- t %>%
    select(year, all_of(solvers)) %>%
    pivot_longer(!year, names_to = "solver", values_to = "time",
                 values_transform = list(time = as.numeric)) %>%
    mutate(solver = factor(solver, levels = solvers)) %>%
    arrange(solver == "BTree") %>%
    mutate(solver = recode(solver, "BTree" = "PBTree (Ours)"))

  # scale right axis to align with left log axis (based on main size column)
  rightScaleValue <- 10^(
    mean(log10(range(tb_size$size, na.rm = TRUE))) -
    mean(log10(range(tc$time, na.rm = TRUE)))
  ) / right_scale_divisor
  tc$time_scaled <- tc$time * rightScaleValue

  shapeLevel <- c(1, 0, 2, 5, 7, 14)[seq_along(solvers)]

  bar_fill_values <- c(size          = "#fbf8cc",
                       extra_total        = "#a8d8ea")
  bar_fill_labels <- c(size          = "Batch / Tree size",
                       extra_total        = "Reinsert + Leaf balance size")

  if (is.null(extra_bars)) {
    tb <- tb_size
    bar_geom <- geom_bar(
      aes(y = size, fill = "size"),
      stat = "identity", color = "black", width = 0.5
    )
    fill_scale <- scale_fill_manual(
      "", values = bar_fill_values["size"], labels = bar_fill_labels["size"]
    )
  } else {
    tb <- t %>%
      select(year, size, all_of(extra_bars)) %>%
      mutate(extra_total = rowSums(across(all_of(extra_bars)), na.rm = TRUE),
             extra_total = ifelse(extra_total <= 0, NA_real_, extra_total)) %>%
      select(year, size, extra_total) %>%
      pivot_longer(!year, names_to = "bar_type", values_to = "bar_value") %>%
      mutate(bar_type = factor(bar_type, levels = c("size", "extra_total")))

    bar_geom <- geom_bar(
      aes(y = bar_value, fill = bar_type, group = bar_type),
      stat = "identity", color = "black", width = 0.35,
      position = position_dodge(width = 0.6)
    )
    fill_scale <- scale_fill_manual(
      "",
      values = bar_fill_values[c("size", "extra_total")],
      labels = bar_fill_labels[c("size", "extra_total")]
    )
  }

  plt <- ggplot(tb, aes(x = year)) +
    bar_geom +
    fill_scale +
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
      breaks = year_breaks,
      labels = year_breaks - 2000
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
      fill  = guide_legend(order = 1, nrow = 1, keywidth = unit(0.8, "lines"), keyheight = unit(1.0, "lines")),
      color = guide_legend(order = 2, nrow = 1, keyheight = unit(1.0, "lines")),
      shape = guide_legend(order = 2, nrow = 1, keyheight = unit(1.0, "lines"),
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

g1 <- plot_op(tInsert,  "(a) Batch Insert", expression(underline("Batch size (×1K)")), expression(underline("Time (sec.)")), FALSE, titleSize, axisSize, legendSize, show_left_title = TRUE,  show_right_title = FALSE, bar_scale = 1e3, right_scale_divisor = 100)

g2 <- plot_op(tDelete,  "(b) Batch Delete", expression(underline("Batch size (×1K)")), expression(underline("Time (sec.)")), TRUE,  titleSize, axisSize, legendSize, show_left_title = FALSE, show_right_title = TRUE,  bar_scale = 1e3, right_scale_divisor = 100)

g3 <- plot_op(tRange,   "(c) Neighbor Query",  expression(underline("Tree size (×1M)")),  expression(underline("Time (sec.)")), FALSE, titleSize, axisSize, legendSize, show_left_title = TRUE,  show_right_title = FALSE, right_scale_divisor = 10)

g4 <- plot_op(tFlatten, "(d) Flatten",      expression(underline("Tree size (×1M)")),  expression(underline("Time (sec.)")), FALSE, titleSize, axisSize, legendSize, show_left_title = FALSE, show_right_title = TRUE,  right_scale_divisor = 10)

g <- ggpubr::ggarrange(
  plotlist = list(g1, g2, g3, g4),
  nrow = 2,
  ncol = 2,
  common.legend = TRUE,
  legend = "top",
  legend.grob = ggpubr::get_legend(g2)
) + theme(plot.margin = grid::unit(c(0, 0, 0, 0), "mm"))

g <- annotate_figure(g,
  bottom = text_grob(TeX("Year of the batch"), size = faxisSize),
)

out_path <- file.path(data_dir, paste0(input_type, "_dynamic_graph.pdf"))
ggsave(out_path, width = 10, height = 6, dpi = 1800)
cat("Saved to", out_path, "\n")
