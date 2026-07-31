cmake_minimum_required(VERSION 3.15)

# CompileFlags:
#   Add:
#     - -I/home/ziyang/code/ParallelMultiwayOrderedSet/include/parlaylib/include
#     - -I/home/ziyang/code/ParallelMultiwayOrderedSet/include/pmos
#     - -I/home/ziyang/code/ParallelMultiwayOrderedSet/include/

project(
	PMOS
	VERSION 0.1
	DESCRIPTION "Parallel Multi-way Ordered Set"
	HOMEPAGE_URL ""
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # works
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --gcc-toolchain=/opt/rh/gcc-toolset-14/root/usr/")
# add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wunused -Wsign-conversion -Wfloat-equal -Wpedantic -Wuninitialized -Wformat -Wformat-security -Wmissing-braces -Wparentheses -Wsequence-point -Wswitch -Wswitch-enum -Wstrict-aliasing -Wdeprecated-declarations -Wreturn-type -Wunused-result)
# add_compile_options(-Wall -Wextra -Werror)
add_compile_options(-w)

option(DEBUG "" OFF)
option(CILKPLUS "" OFF)
option(OPENCILK "" OFF)
option(SERIAL "" OFF)
option(STDALLOC "" OFF)
option(MEMCHECK "" OFF)
option(DYNAMIC_GRAPH OFF)
option(OSM OFF)
option(SIEVE "" OFF)
option(JEMA "" ON)
option(CCP "" OFF)


if(CILKPLUS)
	add_compile_options(-DPARLAY_CILKPLUS -DCILK -fcilkplus)
elseif(OPENCILK)
	add_compile_options(-DPARLAY_OPENCILK -DCILK -fopencilk)
elseif(SERIAL)
	add_compile_options(-DPARLAY_SEQUENTIAL)
else()
	add_compile_options(-pthread)
endif()

if(LEAF)
	message(">>>>> Compiling with LEAF=" ${LEAF})
	add_compile_definitions(LEAF${LEAF})
endif()

if(SIEVE)
	message(">>>>> Compiling with SIEVE")
	add_compile_definitions(SIEVE)
endif()

if(DYNAMIC_GRAPH)
	message(">>>>> Compiling with DYNAMIC_GRAPH")
	add_compile_definitions(DYNAMIC_GRAPH)
endif()

if(OSM)
	message(">>>>> Compiling with OSM")
	add_compile_definitions(OSM)
endif()

if(MEMCHECK)
	message(">>>>> Compiling with MEMCHECK")
	add_compile_definitions(MEMCHECK)
endif()

if(APPLE)
	message(">>>>> Compiling on MacOS")
	add_compile_definitions(MAC)
endif()

if(DEBUG)
	set(CMAKE_BUILD_TYPE "Debug")
	add_compile_options(-g -march=native -DDEBUG)
	if(NOT APPLE)
		add_compile_options(-mcx16)
	endif()
	if(CCP)
		message(">>>>> Set CCP=ON")
		add_compile_options(-DCCP)
	endif()
else()
	set(CMAKE_BUILD_TYPE "Release")
	add_compile_options(-O3 -march=native)
	if(NOT APPLE)
		add_compile_options(-mcx16)
	endif()
endif()

if(STDALLOC)
	add_compile_options(-DPARLAY_USE_STD_ALLOC)
endif()

add_subdirectory(external/parlaylib)

# NOTE: clear CMAKE_MODULE_PATH to avoid finding parlay's cmake files
list(REMOVE_ITEM CMAKE_MODULE_PATH "${PARLAY_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${PMOS_SOURCE_DIR}/cmake")

# NOTE: GoogleTest
add_subdirectory(external/googletest)
enable_testing()


# NOTE: PMOS
add_library(PMOS INTERFACE)
set(PMOS_INCLUDE_DIR "${PMOS_SOURCE_DIR}/include/pb_tree" "${PMOS_SOURCE_DIR}/include/" "${PMOS_SOURCE_DIR}/external")
target_link_libraries(PMOS INTERFACE parlay)
target_include_directories(PMOS INTERFACE $<BUILD_INTERFACE:${PMOS_INCLUDE_DIR}>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# add_executable(b_ccp ${PROJECT_SOURCE_DIR}/tests/b_ccp.cpp)
# target_link_libraries(b_ccp PUBLIC PMOS)
# target_include_directories(b_ccp PUBLIC ${PROJECT_SOURCE_DIR}/include)

# NOTE: CPAM
add_library(CPAM INTERFACE)
set(CPAM_INCLUDE_DIR "${PMOS_SOURCE_DIR}/external/CPAM/include/cpam" "${PMOS_SOURCE_DIR}/external/CPAM/include/pam")
target_link_libraries(CPAM INTERFACE parlay PMOS)
target_include_directories(CPAM INTERFACE ${CPAM_INCLUDE_DIR} ${PARLAY_INCLUDE_DIR})

# NOTE: PMA
if(NOT APPLE)
	add_library(PMA INTERFACE)
	set(PMA_INCLUDE_DIR "${PMOS_SOURCE_DIR}/external/Packed-Memory-Array" "${PMOS_SOURCE_DIR}/external/Packed-Memory-Array/include" "${PROJECT_SOURCE_DIR}/external/Packed-Memory-Array/tlx" "${PMOS_SOURCE_DIR}/external/Packed-Memory-Array/ParallelTools" "${PMOS_SOURCE_DIR}/external/Packed-Memory-Array/StructOfArrays/include")
	target_link_libraries(PMA INTERFACE parlay PMOS)
	target_include_directories(PMA INTERFACE ${PMA_INCLUDE_DIR} ${PARLAY_INCLUDE_DIR})
endif()

# NOTE: TEST_FRAME
add_library(TEST_FRAME INTERFACE)
set(TEST_FRAME_INCLUDE_DIR ${PARLAY_INCLUDE_DIR} ${PMOS_INCLUDE_DIR})
target_link_libraries(TEST_FRAME INTERFACE parlay PMOS)

# NOTE: executables for tests
add_executable(b_test ${PROJECT_SOURCE_DIR}/tests/b_test.cpp)
target_link_libraries(b_test PUBLIC TEST_FRAME)
target_include_directories(b_test PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# NOTE: CPAM
add_executable(cpam ${PROJECT_SOURCE_DIR}/tests/cpam.cpp)
target_link_libraries(cpam PUBLIC TEST_FRAME)
target_link_libraries(cpam PUBLIC CPAM)
target_include_directories(cpam PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${CPAM_INCLUDE_DIR})

# NOTE: StdSet
add_executable(stdset ${PROJECT_SOURCE_DIR}/tests/stdset.cpp)
target_link_libraries(stdset PUBLIC TEST_FRAME)
target_include_directories(stdset PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# NOTE: TLX B+ tree (header-only baseline)
add_executable(tlxbtree ${PROJECT_SOURCE_DIR}/tests/tlxbtree.cpp)
target_link_libraries(tlxbtree PUBLIC TEST_FRAME)
target_include_directories(tlxbtree PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${PROJECT_SOURCE_DIR}/external/tlx)

# NOTE: PMA
if(NOT APPLE)
	add_executable(pma ${PROJECT_SOURCE_DIR}/tests/pma.cpp)
	target_link_libraries(pma PUBLIC TEST_FRAME PMA)
	target_include_directories(pma PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${PMA_INCLUDE_DIR})
	target_compile_definitions(pma PUBLIC PARALLEL=1 PARLAY=1)
endif()

# NOTE: GoogleTest for correctness check
add_executable(unit_test ${PROJECT_SOURCE_DIR}/tests/unit_test.cpp)
target_link_libraries(unit_test PUBLIC TEST_FRAME gtest_main)
target_include_directories(unit_test PUBLIC ${TEST_FRAME_INCLUDE_DIR})
add_test(NAME UnitTest COMMAND unit_test)
# ctest -R BCCPTest -V

if(JEMA)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
	message(">>>>> Linking with jemalloc")

	# target_link_libraries(myapp PRIVATE PkgConfig::JEMALLOC)
	target_link_libraries(b_test PUBLIC PkgConfig::JEMALLOC)
	target_link_libraries(cpam PUBLIC PkgConfig::JEMALLOC)
	# target_link_libraries(stdset PUBLIC PkgConfig::JEMALLOC)
	# target_link_libraries(tlxbtree PUBLIC PkgConfig::JEMALLOC)
	# target_link_libraries(masstree PUBLIC PkgConfig::JEMALLOC)
	# target_link_libraries(b_test PUBLIC libjemalloc.so)
	# target_link_libraries(cpam PUBLIC libjemalloc.so)
endif()

add_executable(data_generator ${PROJECT_SOURCE_DIR}/tests/data_generate.cpp)
target_link_libraries(data_generator PUBLIC TEST_FRAME)
target_include_directories(data_generator PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# batches for the incremental-update benchmarks (b_test tags 1 << 6 / 1 << 7)
add_executable(incre_data_generator ${PROJECT_SOURCE_DIR}/tests/incre_data_generate.cpp)
target_link_libraries(incre_data_generator PUBLIC TEST_FRAME)
target_include_directories(incre_data_generator PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# convert text .in datasets to binary .bin
add_executable(data_to_binary ${PROJECT_SOURCE_DIR}/tests/data_to_binary.cpp)
target_link_libraries(data_to_binary PUBLIC TEST_FRAME)
target_include_directories(data_to_binary PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# to generate temporal graph osm 
add_executable(temporal_graph_osm ${PROJECT_SOURCE_DIR}/tests/temporal_graph_osm.cpp)
target_link_libraries(temporal_graph_osm PUBLIC TEST_FRAME)
target_include_directories(temporal_graph_osm PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# to generate temporal graph signal
add_executable(temporal_graph_signal ${PROJECT_SOURCE_DIR}/tests/temporal_graph_signal.cpp)
target_link_libraries(temporal_graph_signal PUBLIC TEST_FRAME)
target_include_directories(temporal_graph_signal PUBLIC ${TEST_FRAME_INCLUDE_DIR})

# to generate osm
add_subdirectory(external/libmorton)

add_executable(filter_csv_year ${PROJECT_SOURCE_DIR}/tests/filter_csv_year.cpp)
target_link_libraries(filter_csv_year PUBLIC TEST_FRAME libmorton)
target_include_directories(filter_csv_year PUBLIC ${TEST_FRAME_INCLUDE_DIR})

set(INDEX_MICROBENCH_DIR "${PROJECT_SOURCE_DIR}/external/index-microbench")
file(GLOB INDEX_MICROBENCH_SKIPLIST_SOURCES
	"${INDEX_MICROBENCH_DIR}/nohotspot-skiplist/*.cpp")
set(INDEX_MICROBENCH_BTREE_SOURCE "${INDEX_MICROBENCH_DIR}/btree-rtm/btree.c")
set_source_files_properties(${INDEX_MICROBENCH_BTREE_SOURCE} PROPERTIES LANGUAGE CXX)

add_executable(index_microbench_workload
	"${INDEX_MICROBENCH_DIR}/workload.cpp"
	"${INDEX_MICROBENCH_DIR}/BwTree/bwtree.cpp"
	"${INDEX_MICROBENCH_DIR}/ARTOLC/Tree.cpp"
	"${INDEX_MICROBENCH_BTREE_SOURCE}"
	${INDEX_MICROBENCH_SKIPLIST_SOURCES}
)
set_target_properties(index_microbench_workload PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS ON
)
target_include_directories(index_microbench_workload PRIVATE ${INDEX_MICROBENCH_DIR} ${TEST_FRAME_INCLUDE_DIR})
# target_compile_definitions(index_microbench_workload PRIVATE USE_TBB BWTREE_NODEBUG NDEBUG)
target_compile_definitions(index_microbench_workload PRIVATE BWTREE_NODEBUG NDEBUG)
target_compile_options(index_microbench_workload PRIVATE
	-Wno-invalid-offsetof
	-include "${INDEX_MICROBENCH_DIR}/masstree/config.h"
)
find_package(Threads REQUIRED)
find_package(TBB QUIET)
if(TBB_FOUND)
	target_link_libraries(index_microbench_workload PRIVATE TEST_FRAME Threads::Threads m TBB::tbb papi tcmalloc_minimal)
else()
	message(WARNING "TBB CMake package not found; falling back to plain tbb link target")
	target_link_libraries(index_microbench_workload PRIVATE TEST_FRAME Threads::Threads m tbb papi tcmalloc_minimal)
endif()

find_library(INDEX_MICROBENCH_ATOMIC_LIB
	NAMES atomic libatomic.so.1
	PATHS /lib64 /usr/lib64 /lib /usr/lib
)
if(INDEX_MICROBENCH_ATOMIC_LIB)
	target_link_libraries(index_microbench_workload PRIVATE "${INDEX_MICROBENCH_ATOMIC_LIB}")
else()
	message(WARNING "libatomic was not found for index_microbench_workload")
endif()

if(EXISTS "${INDEX_MICROBENCH_DIR}/masstree/mtIndexAPI.a")
	target_link_libraries(index_microbench_workload PRIVATE "${INDEX_MICROBENCH_DIR}/masstree/mtIndexAPI.a")
else()
	message(WARNING "mtIndexAPI.a not found in ${INDEX_MICROBENCH_DIR}/masstree; compiling required masstree sources directly for index_microbench_workload")
	target_sources(index_microbench_workload PRIVATE
		"${INDEX_MICROBENCH_DIR}/masstree/mtIndexAPI.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/kvthread.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/json.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/str.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/string.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/straccum.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/misc.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/kvio.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/kvrandom.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/value_string.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/value_array.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/value_versioned_array.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/perfstat.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/string_slice.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/file.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/compiler.cc"
		"${INDEX_MICROBENCH_DIR}/masstree/clp.c"
	)
	set_source_files_properties("${INDEX_MICROBENCH_DIR}/masstree/clp.c" PROPERTIES LANGUAGE CXX)
endif()


# add_executable(data_washer ${PROJECT_SOURCE_DIR}/tests/data_wash.cpp)
# target_link_libraries(data_washer PUBLIC PMOS)
# target_include_directories(data_washer PUBLIC ${PROJECT_SOURCE_DIR}/include)

# Generate .clangd configuration from compile_commands.json
execute_process(
	COMMAND ${CMAKE_COMMAND} -E echo "Generating .clangd configuration..."
)
execute_process(
	COMMAND python3 ${PROJECT_SOURCE_DIR}/scripts/generate_clangd.py
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	RESULT_VARIABLE CLANGD_RESULT
)
