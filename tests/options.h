#pragma once

#include <filesystem>

#include "common/parse_command_line.h"

struct Options
{
  Options(commandLine& params)
  {
    this->val_n = params.getOptionIntValue("-d", 1);
    this->input_file_path = std::string(params.getOptionValue("-p"));
    this->tag = params.getOptionIntValue("-t", 1);
    this->rounds = params.getOptionIntValue("-r", 3);
    this->query_type = params.getOptionIntValue("-q", 0);
    this->summary = params.getOptionIntValue("-s", 0);
    this->batch_ratio = params.getOptionDoubleValue("-b", 0.1);
    this->tree_type = params.getOptionIntValue("-T", 0);
    this->dis_type = std::string(params.getOptionValue("-D"));
    this->temporal_graph = params.getOptionIntValue("-G", 0);
    // NOTE: perf --control fds, passed by scripts/perf_cache.sh. 0 = not under
    // perf, which makes every PerfRegion a no-op. See perf_control.h.
    this->perf_ctl_fd = params.getOptionIntValue("-pcf", 0);
    this->perf_ctl_ack_fd = params.getOptionIntValue("-pcaf", 0);
    // NOTE: which guarded region to count: "build", "insert", "delete",
    // "membership", "incre_insert" or "incre_delete". Empty (the default) arms
    // every region, which sums them -- a run with -t 1 would report
    // build+insert together.
    this->perf_region = params.getOptionValue("-pcr", std::string(""));
  }

  void PrintOptions() const
  {
    std::cout << "InputFile: "
              << std::filesystem::path(input_file_path).filename().string()
              << "; ";
    std::cout << "Dis: " << this->dis_type << "; ";
  }

  static auto ExtractPrefixes(std::string const& path)
    -> std::pair<std::string, std::string>
  {
    // locate pivot: first occurrence of "/ins/" or "/del/"
    size_t posIns = path.find("/ins/");
    size_t posDel = path.find("/del/");
    size_t pivot = std::string::npos;
    if (posIns != std::string::npos) {
      pivot = posIns;
    }
    if (posDel != std::string::npos &&
        (pivot == std::string::npos || posDel < pivot)) {
      pivot = posDel;
    }

    if (pivot == std::string::npos) {
      // pattern not found
      return { "", "" };
    }

    // directory prefix (up to just before /ins/ or /del/)
    std::string dirPrefix = path.substr(0, pivot) + "/";

    // filename part
    size_t lastSlash = path.find_last_of('/');
    std::string filename =
      (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
    // strip extensions
    auto strip_suffix = [](std::string& s, std::string const& suf) {
      if (s.size() >= suf.size() &&
          s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
        s.erase(s.size() - suf.size());
      }
    };
    strip_suffix(filename, ".bin");
    strip_suffix(filename, ".in");
    strip_suffix(filename, ".base");
    // NOTE: ".batch" too, so -p can point at a batch file. The incremental
    // datasets (incre_data_generator) name both the insert and the delete
    // stream "<i>.batch.<ext>" -- there is no base/batch pair there, the whole
    // file IS the batch stream. Without this the prefix comes out as
    // "<i>.batch" and every derived path becomes "<i>.batch.base.<ext>".
    strip_suffix(filename, ".batch");

    return { dirPrefix, filename };
  }

  // NOTE: derived base/batch paths inherit the input file's extension, so a
  // ".bin" input selects ".bin" batch files (read via ReadEntriesBinary)
  [[nodiscard]] auto InputExtension() const -> std::string
  {
    return input_file_path.ends_with(".bin") ? ".bin" : ".in";
  }

  [[nodiscard]] auto ParseInsertBatchPath(double batch_ratio) const
    -> std::pair<std::string, std::string>
  {
    // PARA: /ssd1/zmen002/kv_pair/uni/1000000_1/ins/100"
    // PARA: dir_pre: ../kdtree/uni/1000000_1/
    // PARA: file_pre: 1
    auto [dir_prefix, file_prefix] = ExtractPrefixes(this->input_file_path);
    std::string base_path =
      dir_prefix + "ins/" +
      std::to_string(static_cast<size_t>(1 / batch_ratio)) + "/" + file_prefix +
      ".base" + InputExtension();

    std::string batch_path = base_path;
    batch_path.replace(batch_path.find(".base."), 6, ".batch.");
    return std::make_pair(base_path, batch_path);
  }

  [[nodiscard]] auto ParseDeleteBatchPath(double batch_ratio) const
    -> std::pair<std::string, std::string>
  {
    // NOTE: ../kdtree/uni/1000000_1/ins/10/1.base.in
    auto [dir_prefix, file_prefix] = ExtractPrefixes(this->input_file_path);
    std::string base_path =
      dir_prefix + "del/" +
      std::to_string(static_cast<size_t>(1 / batch_ratio)) + "/" + file_prefix +
      ".base" + InputExtension();

    std::string batch_path = base_path;
    batch_path.replace(batch_path.find(".base."), 6, ".batch.");

    return std::make_pair(base_path, batch_path);
  }

  void PrintOptionValue() const
  {
    std::cout << "Options: " << std::endl;
    std::cout << "val_n: " << val_n << std::endl;
    std::cout << "input_file_path: " << input_file_path << std::endl;
    std::cout << "tag: " << tag << std::endl;
    std::cout << "rounds: " << rounds << std::endl;
    std::cout << "query_type: " << query_type << std::endl;
    std::cout << "summary: " << summary << std::endl;
    std::cout << "batch_ratio: " << batch_ratio << std::endl;
    std::cout << "tree_type: " << tree_type << std::endl;
    std::cout << "dis_type: " << dis_type << std::endl;
  }

  int val_n;
  std::string input_file_path = "";
  int tag;
  int rounds;
  int query_type;
  bool summary;
  double batch_ratio; // decides the default batch file path
  int tree_type;
  int temporal_graph;
  std::string dis_type;
  int perf_ctl_fd;
  int perf_ctl_ack_fd;
  std::string perf_region;
};
