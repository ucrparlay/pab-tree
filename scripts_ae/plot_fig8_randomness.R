# HOW TO RUN (from scripts_ae/, after the merge step)
#   Rscript plot_fig8_randomness.R
#
# From the paper's plot_randomness_dynamic_graph.R: default input under data/, output in plots/.

library(dplyr)
library(tidyr)
library(ggplot2)
library(scales)
library(ggsci)
library(gtable)

# the body font of the paper, so that the figure sets its text in the same face
# as the surrounding prose. cairo_pdf resolves the family through fontconfig and
# embeds it, so the name has to match `fc-list` exactly.
FONT <- "Linux Libertine"

# Rscript plot_randomness.R /path/to/100batches.csv /path/to/summary_filtered.csv

args <- commandArgs(trailingOnly = TRUE)
data_path <- ifelse(length(args) >= 1, args[1], "data/randomness/dynamic.csv")

raw <- read.csv(data_path, stringsAsFactors = FALSE, check.names = TRUE)

# check.names turns "insert_t(s)" -> "insert_t.s." etc.
colnames(raw)[colnames(raw) == "insert_t.s."] <- "insert_t"
colnames(raw)[colnames(raw) == "delete_t.s."] <- "delete_t"
colnames(raw)[colnames(raw) == "range.s."] <- "query_t"

df <- raw %>%
  select(randomness, insert_t, delete_t, query_t) %>%
  pivot_longer(
    cols      = c(insert_t, delete_t, query_t),
    names_to  = "operation",
    values_to = "time"
  ) %>%
  mutate(
    operation  = factor(operation, levels = c("insert_t", "delete_t", "query_t")),
    randomness = factor(randomness)
  )

# CPAM's lines: its total times on PLANET, from Fig. 6's run
cpam_path <- ifelse(length(args) >= 2, args[2], "data/temp_osm/temp_osm.csv")
cpam_ops <- c(insert_t = "insert_t(s)", delete_t = "delete_t(s)", query_t = "range_t(s)")
cpam <- read.csv(cpam_path, stringsAsFactors = FALSE, check.names = FALSE) %>%
  filter(tree == "CPAM", operation %in% cpam_ops)
hlines <- data.frame(
  operation = factor(names(cpam_ops), levels = c("insert_t", "delete_t", "query_t")),
  yval = sapply(cpam_ops, function(op) as.numeric(cpam$sum_t[cpam$operation == op][1]))
)

barColorMap <- c(
  "blue" = "#a3c4f3", "yellow" = "#fbf8cc",
  "pink" = "#ffcfd2", "grey" = "#cfcfcf", "green" = "#b9fbc0"
)

opColorMap <- c(
  insert_t = barColorMap[["yellow"]],
  delete_t = barColorMap[["blue"]],
  query_t  = barColorMap[["pink"]]
)
opLabels <- c(insert_t = "Insert (Ours)", delete_t = "Delete (Ours)", query_t = "Neighbor query (Ours)")
opLineLabels <- c(insert_t = "Insert (CPAM)", delete_t = "Delete (CPAM)", query_t = "Neighbor query (CPAM)")

lzoom <- pal_locuszoom()(7)
lineColorMap <- c(
  insert_t = lzoom[2], # orange-ish
  delete_t = lzoom[5], # blue
  query_t  = lzoom[1] # red
)
lineTypeMap <- c(insert_t = "solid", delete_t = "dashed", query_t = "twodash")

titleSize <- 20
axisSize <- 20
legendSize <- 18

g <- ggplot(df, aes(x = randomness, y = time, fill = operation)) +
  geom_bar(
    stat     = "identity",
    position = position_dodge2(width = 0.7, padding = 0),
    color    = "black",
    width    = 0.6
  ) +
  geom_hline(
    data = hlines,
    aes(yintercept = yval, color = operation, linetype = operation),
    linewidth = 1
  ) +
  scale_fill_manual(values = opColorMap, labels = opLabels) +
  scale_color_manual(values = lineColorMap, labels = opLineLabels) +
  scale_linetype_manual(values = lineTypeMap, labels = opLineLabels) +
  scale_y_continuous(
    name   = "Time (sec.)",
    labels = function(x) formatC(x, digits = 0, format = "f")
  ) +
  theme_light(base_family = FONT) +
  theme(
    plot.title = element_text(hjust = 0.5, size = titleSize, margin = margin(t = 8)),
    plot.title.position = "plot",
    plot.margin = margin(5, 5, 5, 5),
    axis.text.x = element_text(size = axisSize - 2, color = "black", margin = margin(2, 0, 0, 0)),
    axis.text.y = element_text(size = axisSize - 1, color = "black"),
    axis.title.x = element_text(size = axisSize + 1, color = "black", margin = margin(6, 0, 0, 0)),
    # the y title names the quantity while the x title names the variable the
    # figure sweeps, so the two do not run at the same size
    axis.title.y = element_text(size = axisSize - 1, color = "black", margin = margin(0, 4, 0, 0)),
    # no trailing margin on a label: the gap to the next key is set by
    # legend.key.spacing.x, and trailing padding would push the block off centre
    legend.text = element_text(size = legendSize, margin = margin(l = 4, r = 0)),
    legend.title = element_blank(),
    legend.position = "top",
    legend.margin = margin(0, 10, 0, 0),
    legend.box.margin = margin(0, 0, 10, 0),
    legend.spacing.x = unit(0, "pt"),
    # the gap between one entry and the next key
    legend.key.spacing.x = unit(20, "pt"),
    legend.spacing.y = unit(7, "pt"),
    legend.key.height = unit(0.5, "lines"),
    panel.grid.minor = element_blank()
  ) +
  guides(
    # the two rows are stacked, so their keys are the same width: only then can
    # a bar entry and the line entry below it share a column
    fill = guide_legend(order = 1, nrow = 1, keywidth = unit(1.5, "lines"), keyheight = unit(1.0, "lines")),
    color = guide_legend(
      order = 2, nrow = 1, keywidth = unit(1.5, "lines"), keyheight = unit(1.0, "lines"),
      override.aes = list(linewidth = 0.8)
    ),
    linetype = guide_legend(order = 2, nrow = 1, keywidth = unit(1.5, "lines"), keyheight = unit(1.0, "lines"))
  ) +
  theme(legend.box = "vertical", legend.box.just = "left") +
  labs(x = "Randomness (%)")

FIG_W <- 8
FIG_H <- 4

# a grob is measured with whatever device is open when it is built, so the
# figure is assembled with a cairo device open, matching the one it is drawn on
scratch_dev <- tempfile(fileext = ".pdf")
cairo_pdf(scratch_dev, width = FIG_W, height = FIG_H)

# the bar row and the line row are separate guides, and ggplot places each at
# its own offset inside the guide box, so a bar never sits above its line. the
# two are therefore taken out and stacked by hand: every cell is widened to the
# wider of the two rows, which makes the columns match, and stacking them in one
# grid column gives them a common origin.
pt_widths <- function(gt) grid::convertWidth(gt$widths, "pt", valueOnly = TRUE)
pt_height <- function(gt) sum(grid::convertHeight(gt$heights, "pt", valueOnly = TRUE))

built <- ggplotGrob(g)
box <- built$grobs[[which(built$layout$name == "guide-box-top")]]
guides <- box$grobs[vapply(box$grobs, inherits, logical(1), "gtable")]

wide <- do.call(pmax, lapply(guides, pt_widths))
# the guide ends with a margin cell that no ink reaches; keeping it would make
# the block wider than it looks and centre it off to the left
wide[length(wide)] <- 0

# ggplot leaves slack in the label cells, so the block reserves more width than
# its text occupies and centring the block leaves the text off centre. each
# label column is therefore cut back to the wider of the two labels it holds.
LABEL_COLS <- c(3, 6, 9)
LABEL_PAD <- 4 # the left margin legend.text keeps
text_width <- function(s) {
  grid::convertWidth(
    grid::grobWidth(grid::textGrob(
      s,
      gp = grid::gpar(fontsize = legendSize, fontfamily = FONT)
    )),
    "pt",
    valueOnly = TRUE
  )
}
for (k in seq_along(LABEL_COLS)) {
  wide[LABEL_COLS[k]] <- max(text_width(opLabels[k]), text_width(opLineLabels[k])) + LABEL_PAD
}
guides <- lapply(guides, function(gt) {
  gt$widths <- grid::unit(wide, "pt")
  gt
})

# rbind rather than stacking two grobs: it merges the rows into one table with
# one set of columns, so a bar key and the line key below it are in the same
# cell and cannot drift apart
legend_block <- rbind(guides[[1]], guides[[2]], size = "max")

# the two guides meet with no space between them once merged, so a blank row is
# inserted at the seam
ROW_GAP <- 8
legend_block <- gtable::gtable_add_rows(
  legend_block, grid::unit(ROW_GAP, "pt"),
  pos = nrow(guides[[1]])
)

# the two side columns are stated in points rather than as null units: null
# columns are not guaranteed to split the slack evenly, and unequal ones leave
# the block off centre.
#
# the guide also carries padding of its own that no ink reaches, so a block
# centred on its declared width still prints left of centre. the offset below
# is measured from the rendered figure and shifts the block back; re-measure it
# if the labels or the legend size change.
LEGEND_NUDGE <- 14.4
legend_pad <- (FIG_W * 72 - sum(wide)) / 2
legend_row <- gridExtra::arrangeGrob(
  grid::nullGrob(), legend_block, grid::nullGrob(),
  ncol = 3,
  widths = grid::unit.c(
    grid::unit(legend_pad + LEGEND_NUDGE, "pt"), grid::unit(sum(wide), "pt"),
    grid::unit(legend_pad - LEGEND_NUDGE, "pt")
  )
)

body <- ggplotGrob(g + theme(legend.position = "none"))
final <- gridExtra::arrangeGrob(
  # the blank row keeps the keys off the page edge, which would crop them
  grid::nullGrob(), legend_row, grid::nullGrob(), body,
  ncol = 1,
  heights = grid::unit.c(
    grid::unit(2, "mm"),
    # the two guide rows plus the blank one inserted between them
    grid::unit(sum(vapply(guides, pt_height, numeric(1))) + ROW_GAP, "pt"),
    grid::unit(4, "mm"),
    grid::unit(1, "null")
  )
)

invisible(dev.off())
unlink(scratch_dev)

out_path <- "plots/fig8_randomness.pdf"
cairo_pdf(out_path, width = FIG_W, height = FIG_H)
grid::grid.draw(final)
invisible(dev.off())
cat("Saved to", out_path, "\n")
