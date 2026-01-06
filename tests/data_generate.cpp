#include "data_op.h"

int main(int argc, char* argv[]) {
  commandLine P(argc, argv,
                "[-k {1,...,100}] [-d {2,3,5,7,9,10}] [-n <node num>] [-t "
                "<parallelTag>] [-p <inFile>] [-r {1,...,5}] [-q {0,1}] [-i "
                "<_insertFile>] [-s <kSummary>]");
  std::string path;
  int base_n, batch_n, pts_dim, file_num, dis_type, batch_op;
  double param;

  path = P.getOptionValue("-p");
  base_n = P.getOptionIntValue("-n", 1'000'000);
  batch_n = P.getOptionIntValue("-batch_n", 100'000);
  pts_dim = P.getOptionIntValue("-d", 2);
  file_num = P.getOptionIntValue("-file_num", 2);
  // PARA: 0: unifrom, 1: exp, 2: zipfian
  dis_type = P.getOptionIntValue("-dis", 0);
  // PARA: 0: insert, 1: delete
  batch_op = P.getOptionIntValue("-batch_op", 0);
  param = P.getOptionDoubleValue("-param", 1.0);
  std::cout << "pts_num: " << base_n << " pts_dim: " << pts_dim
            << " file_num: " << file_num << " distribution_type: " << dis_type
            << " param: " << param << std::endl;

  // NOTE: ../kdtree/
  path += std::string(*path.rbegin() == '/' ? "" : "/");
  std::filesystem::create_directory(path);

  path += std::string(dis_type == 0 ? "uni/" : dis_type == 1 ? "exp/" : "zip/");
  std::filesystem::create_directory(path);

  path += std::string(batch_op == 0 ? "ins" : "del");
  std::filesystem::create_directory(path);

  path += toString(base_n) + "_" + toString(pts_dim) + "/";
  std::filesystem::create_directory(path);

  auto generate = [&]<typename E>(std::string const& new_path) {
    using EntrySeq = parlay::sequence<E>;
    using K = typename E::K;
    std::cout << "Generating... " << std::endl;
    EntrySeq base_seq, batch_seq;
    if (batch_op == 0) {
      if (dis_type == 0) {
        std::tie(base_seq, batch_seq) =
            Generator<E, UniformGenerator<>>::GenInsertBatch(base_n, batch_n,
                                                             param);
      } else if (dis_type == 1) {
        std::tie(base_seq, batch_seq) =
            Generator<E, ExponentialGenerator<>>::GenInsertBatch(
                base_n, batch_n, param);
      } else if (dis_type == 2) {
        std::tie(base_seq, batch_seq) =
            Generator<E, ZipfianGenerator<>>::GenInsertBatch(base_n, batch_n,
                                                             param);
      }
    } else {
      if (dis_type == 0) {
        std::tie(base_seq, batch_seq) =
            Generator<E, UniformGenerator<>>::GenDeleteBatch(base_n, batch_n,
                                                             param);
      } else if (dis_type == 1) {
        std::tie(base_seq, batch_seq) =
            Generator<E, ExponentialGenerator<>>::GenDeleteBatch(
                base_n, batch_n, param);
      } else if (dis_type == 2) {
        std::tie(base_seq, batch_seq) =
            Generator<E, ZipfianGenerator<>>::GenDeleteBatch(base_n, batch_n,
                                                             param);
      }
    }
    std::cout << "Writing... " << std::endl;
    PrintToFile(new_path + ".base.in", base_seq);
    PrintToFile(new_path + ".batch.in", batch_seq);
  };

  for (int i = 0; i < file_num; i++) {
    std::string newpath = path + toString(i + 1);
    std::cout << newpath << std::endl;
    if (pts_dim == 2) {
      generate.operator()<Wrapper::Element<Axis, std::tuple<Axis>>>(newpath);
    }
    // else if (pts_dim == 3) {
    //   generate.operator()<psi::BasicPoint<Axis, 3>>(newpath);
    // } else if (pts_dim == 5) {
    //   generate.operator()<psi::BasicPoint<Axis, 5>>(newpath);
    // } else if (pts_dim == 7) {
    //   generate.operator()<psi::BasicPoint<Axis, 7>>(newpath);
    // } else if (pts_dim == 9) {
    //   generate.operator()<psi::BasicPoint<Axis, 9>>(newpath);
    // } else if (pts_dim == 12) {
    //   generate.operator()<psi::BasicPoint<Axis, 12>>(newpath);
    // } else if (pts_dim == 16) {
    //   generate.operator()<psi::BasicPoint<Axis, 16>>(newpath);
    // } else {
    //   throw std::runtime_error("Invalid dimension");
    // }
  }
  return 0;
}
