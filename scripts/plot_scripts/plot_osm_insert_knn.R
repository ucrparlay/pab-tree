library(dplyr)
library(tidyr)
library(ggplot2)
library(latex2exp)
library(zoo)
library(scales)
library(ggpubr)

lineColorMap <- c("Ours" = "#D43F3A", "Zd-tree" = "#46B8DA", "Log-tree" = "#5CB85C", "BHL-tree" = "#EEA236", "CGAL" = "#357EBD")

wash_name <- function(t) {
  t$solver[t$solver == "PTree-H"] <- "SPaC-H"
  t$solver[t$solver == "PTree-Z"] <- "SPaC-Z"
  t$solver[t$solver == "OrthTree"] <- "P-Orth"
  t$solver[t$solver == "KdTree"] <- "PkdTree"
  return(t)
}

plot <- function(t, titleSize, axisSize, legendSize, title, plot_type) {
  nameLevel <- c("Ours", "Log-tree", "BHL-tree", "CGAL")
  colors <- c(
    as.character(lineColorMap[nameLevel[[1]]]),
    as.character(lineColorMap[nameLevel[[2]]]),
    as.character(lineColorMap[nameLevel[[3]]]),
    as.character(lineColorMap[nameLevel[[4]]])
  )

  t <- t %>%
    mutate(year = year - 2000)

  tb <- t %>%
    select(year, batchSize)

  if (plot_type == 2) {
    tb <- tb %>%
      mutate(
        batchSize =
          ifelse(
            row_number() <= 5,
            cumsum(batchSize),
            rollapply(batchSize, width = 5, FUN = sum, align = "right", fill = NA)
          )
      )
  }
  tb$batchSize <- tb$batchSize / 1e6

  tc <- t %>%
    select(-batchSize) %>%
    pivot_longer(!c(year), names_to = "solver", values_to = "insert")
  tc <- wash_name(tc)
  tc$solver <- factor(tc$solver, levels = nameLevel)
  tc <- tc %>%
    arrange(desc(solver))

  rightScaleValue <- 30
  rightScaleValue <- ifelse(plot_type == 1, 450, rightScaleValue)
  tc$insert <- tc$insert * rightScaleValue

  shapeLevel <- c(1, 2, 9, 8)
  tc$solver <- factor(tc$solver, levels = nameLevel)
  if (plot_type != 2) {
    tmp_break <- c(1, 100)
  } else {
    tmp_break <- scales::trans_breaks("log10", function(x) 10^x, n = 3)
  }
  plt <- ggplot(tb, aes(x = year)) +
    geom_bar(aes(y = batchSize, fill = "steelblue"),
      stat = "identity",
      color = "black",
      width = 0.5,
    ) +
    scale_fill_manual("", values = "#fbf8cc", label = "Batch / Tree size") +
    geom_line(data = tc, aes(y = insert, color = solver, group = solver)) +
    geom_point(
      data = tc, aes(y = insert, color = solver, shape = solver, group = solver),
      size = 2.5, stroke = 1.5
    ) +
    scale_shape_manual(values = shapeLevel) +
    scale_x_continuous(breaks = seq(14, 23, 2), labels = seq(14, 23, 2)) +
    scale_y_log10(
      name = ifelse(plot_type == 0 | plot_type == 1, TeX("Batch size ($\\times 1 M$)"), TeX("Tree size ($\\times 1 M$)")),
      breaks = scales::trans_breaks("log10", function(x) 10^x, n = 3),
      labels = scales::trans_format("log10", scales::math_format(10^.x)),
      expand = expansion(mult = c(0, .1)),
      sec.axis = sec_axis(~ . / rightScaleValue,
        name = ifelse(plot_type == 0, "Time (sec.)", ifelse(plot_type == 1, "Time (sec.)", "Time (sec.)")),
        breaks = tmp_break,
        labels = scales::trans_format("log10", scales::math_format(10^.x))
      ),
    ) +
    ggtitle(title) +
    theme(
      text = element_text(size = 12),
      plot.margin = unit(c(ifelse(plot_type == 2, -16, 0), 12, .0, .0), "pt"),
      plot.title = element_text(hjust = 0.5, vjust = ifelse(plot_type == 2, -20, 0.2), size = titleSize),
      plot.title.position = "plot",
      axis.text.x = element_text(size = axisSize - 1, color = "black", margin = margin(0, 0, 0, 0)),
      axis.text.y = element_text(size = axisSize - 1, color = "black", margin = margin(0, 0, 0, 0)),
      axis.text.y.right = element_text(size = axisSize - 1, color = "black", margin = margin(0, 0, 0, 0)),
      axis.title.x = element_blank(),
      axis.title.y = element_text(size = axisSize, color = "black", margin = margin(0, 0, 0, 0)),
      axis.title.y.right = element_text(size = axisSize, color = "black", margin = margin(0, 0, 0, 0)),
      legend.text = element_text(size = legendSize),
      legend.title = element_blank(),
      legend.position = ifelse(plot_type == 2, "top", "none"),
      legend.margin = margin(0, 10, ifelse(plot_type == 2, 6, 0), 0),
      legend.box.margin = margin(0, 0, 0, 0),
      legend.spacing.x = unit(0.0, "pt"),
      legend.spacing.y = unit(0.0, "pt"),
      panel.grid.minor = element_blank()
    ) +
    guides(fill = guide_legend(order = 1, nrow = 2), color = guide_legend(order = 2, nrow = 2), shape = guide_legend(order = 2, nrow = 2, override.aes = list(size = 3, stroke = 1))) +
    scale_color_manual(values = colors)

  if (plot_type != 2) {
    plt <- plt + theme(axis.title.y = element_blank())
  }
  return(plt)
}

tYearInsert <- read.csv("../data/osmBatchInsert.csv", sep = ",", row.names = NULL)
tYearDelete <- read.csv("../data/osmBatchDelete.csv", sep = ",", row.names = NULL)
tYearKNN <- read.csv("../data/osmKNN.csv", sep = ",", row.names = NULL)

titleSize <- 13
axisSize <- 13
legendSize <- 13
faxisSize <- 13
g1 <- plot(tYearInsert, titleSize, axisSize, legendSize, "(a) Batch Insert", 0)
g2 <- plot(tYearDelete, titleSize, axisSize, legendSize, "(b) Batch Delete", 1)
g3 <- plot(tYearKNN, titleSize, axisSize, legendSize, "(c) 10-NN", 2)

glist <- list(g1, g2)
g <- ggpubr::ggarrange(
  ggpubr::ggarrange(
    plotlist = glist,
    nrow = 2,
    ncol = 1,
    legend = "none"
  ) + theme(
    plot.margin = grid::unit(c(0.0, 0.0, 0.0, 0.0), "mm")
  ),
  g3,
  ncol = 2,
  legend = "top"
)
g <- annotate_figure(g,
  left = text_grob(TeX("Batch size ($\\times 1 M$)"), size = faxisSize, rot = 90),
  bottom = text_grob(TeX("Year of the batch"), size = faxisSize),
)

ggsave("figures/osmInsertWide.pdf", width = 10, height = 3.2, dpi = 1800)
