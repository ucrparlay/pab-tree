#include "test_framework.h"

int
main(int argc, char* argv[])
{
  commandLine params(argc, argv);
  Wrapper::Options opts(params);

  auto test_func = []<class TreeDesc, typename Point>(Wrapper::Options opts) {

  };

  Wrapper::ApplyOrthogonal(opts, DefaultTestFunc);

  return 0;
}
