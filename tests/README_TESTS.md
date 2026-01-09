# B_CCP Unit Tests

This directory contains GoogleTest unit tests for the `b_ccp` correctness checking functionality.

## Overview

The `unit_test.cpp` file implements unit tests that replicate the behavior of `scripts/checkCorrect.sh`. It tests the correctness of the B-tree implementation with various data distributions and sizes.

## Test Structure

The test suite (`BCCPTest`) runs the same operations as `checkCorrect.sh`:
- Builds a tree from base data files
- Performs batch insert operations
- Validates tree structure
- Performs membership queries

### Test Parameters

The tests use the following default parameters (matching `checkCorrect.sh`):
- **Data prefix**: `/ssd1/zmen002/kv_pair`
- **Tag**: `0b0001` (insert operation)
- **Batch ratio**: `1.0`
- **Query type**: `0b001` (membership query)
- **Tree type**: `0` (B-tree)
- **Dimensions**: `1`
- **Rounds**: `2`
- **Distributions**: `uni`, `exp`, `norm`
- **Node sizes**: `1000000`, `5000000`, `10000000`, `50000000`

### Test Cases

The test suite includes 12 test cases covering all combinations of:
- 3 distributions (uniform, exponential, normal)
- 4 node sizes (1M, 5M, 10M, 50M)

Each test:
1. Searches for `.base.in` files in the appropriate data directory
2. Skips if data files are not found
3. Runs the correctness check for each found file
4. Uses ASSERT/EXPECT macros to verify correctness

## Building

### Prerequisites

GoogleTest is included as a Git submodule in `external/googletest`.

### Build Commands

```bash
cd build
cmake -DDEBUG=ON -DCCP=ON ..
make unit_test
```

## Running Tests

### Run all tests:
```bash
cd build
./unit_test
```

### Run with CTest:
```bash
cd build
ctest -R UnitTest -V
```

### Run specific test:
```bash
cd build
./unit_test --gtest_filter=BCCPTest.Uni1M
```

### List available tests:
```bash
cd build
./unit_test --gtest_list_tests
```

## Data Requirements

The tests expect data files in the following structure:
```
/ssd1/zmen002/kv_pair/
├── uni/
│   ├── 1000000_1/ins/100/*.base.in
│   ├── 5000000_1/ins/100/*.base.in
│   └── ...
├── exp/
│   └── ...
└── norm/
    └── ...
```

If data files are not found, the corresponding tests will be skipped with an appropriate message.

## Comparison with checkCorrect.sh

### Similarities:
- Uses the same data paths and file structure
- Applies identical parameters (tag, batch_ratio, query_type, etc.)
- Reads data from files rather than generating it
- Validates tree structure and performs membership queries

### Differences:
- GoogleTest framework provides structured test reporting
- Individual test cases for each distribution/size combination
- Tests skip gracefully when data is unavailable
- Better integration with CI/CD pipelines via CTest
- More granular failure reporting

## Extending the Tests

To add new test cases:

1. Add a new test method to the `BCCPTest` class:
```cpp
TEST_F(BCCPTest, NewTestCase) {
  std::string files_path = /* construct path */;
  auto base_files = FindBaseFiles(files_path);
  
  if (base_files.empty()) {
    GTEST_SKIP() << "Data files not found";
  }
  
  for (const auto& file : base_files) {
    SCOPED_TRACE("Testing file: " + file);
    EXPECT_NO_THROW(RunTest(file));
  }
}
```

2. Rebuild and run:
```bash
make b_ccp_test
./b_ccp_test
```

## Notes

- The CCP (Correctness Check Parallel) flag must be enabled during build: `-DCCP=ON`
- Debug mode is recommended for testing: `-DDEBUG=ON`
- Tests use jemalloc if enabled in the build configuration
- All tests run independently and can be executed in parallel
