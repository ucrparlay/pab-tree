# CMake generated Testfile for 
# Source directory: /home/zmen002/ParallelMultiwayOrderedSet
# Build directory: /home/zmen002/ParallelMultiwayOrderedSet/scripts
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(UnitTest "/home/zmen002/ParallelMultiwayOrderedSet/scripts/unit_test")
set_tests_properties(UnitTest PROPERTIES  _BACKTRACE_TRIPLES "/home/zmen002/ParallelMultiwayOrderedSet/CMakeLists.txt;137;add_test;/home/zmen002/ParallelMultiwayOrderedSet/CMakeLists.txt;0;")
subdirs("external/parlaylib")
subdirs("external/googletest")
