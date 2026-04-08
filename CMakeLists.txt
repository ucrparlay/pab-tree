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
set(PMOS_INCLUDE_DIR "${PMOS_SOURCE_DIR}/include/pmos" "${PMOS_SOURCE_DIR}/include/")
target_link_libraries(PMOS INTERFACE parlay)
target_include_directories(PMOS INTERFACE $<BUILD_INTERFACE:${PMOS_INCLUDE_DIR}>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# add_executable(b_ccp ${PROJECT_SOURCE_DIR}/tests/b_ccp.cpp)
# target_link_libraries(b_ccp PUBLIC PMOS)
# target_include_directories(b_ccp PUBLIC ${PROJECT_SOURCE_DIR}/include)

# NOTE: CPAM
add_library(CPAM INTERFACE)
set(CPAM_INCLUDE_DIR "${PMOS_SOURCE_DIR}/include/baselines/CPAM/include/cpam" "${PMOS_SOURCE_DIR}/include/baselines/CPAM/include/pam")
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

# NOTE: PMA
if(NOT APPLE)
	add_executable(pma ${PROJECT_SOURCE_DIR}/tests/pma.cpp)
	target_link_libraries(pma PUBLIC TEST_FRAME PMA)
	target_include_directories(pma PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${PMA_INCLUDE_DIR})
	target_compile_definitions(pma PUBLIC PARALLEL=1 PARLAY=1)
endif()

# NOTE: MASSTREE
set(MASSTREE_DIR "${PMOS_SOURCE_DIR}/external/masstree-beta")
set(MASSTREE_SOURCES
	${MASSTREE_DIR}/compiler.cc
	${MASSTREE_DIR}/str.cc
	${MASSTREE_DIR}/straccum.cc
	${MASSTREE_DIR}/string.cc
	${MASSTREE_DIR}/string_slice.cc
	${MASSTREE_DIR}/kvthread.cc
	${MASSTREE_DIR}/json.cc
	${MASSTREE_DIR}/msgpack.cc
	${MASSTREE_DIR}/kvrandom.cc
	${MASSTREE_DIR}/memdebug.cc
	${MASSTREE_DIR}/misc.cc
	${MASSTREE_DIR}/query_masstree.cc
	${MASSTREE_DIR}/value_string.cc
	${MASSTREE_DIR}/value_array.cc
	${MASSTREE_DIR}/value_versioned_array.cc
	${MASSTREE_DIR}/checkpoint.cc
	${MASSTREE_DIR}/file.cc
	${MASSTREE_DIR}/log.cc
	${MASSTREE_DIR}/kvio.cc
)

add_library(MASSTREE ${MASSTREE_SOURCES})
target_include_directories(MASSTREE PUBLIC ${MASSTREE_DIR})
target_compile_definitions(MASSTREE PUBLIC 
	NOSUPERPAGE=1
	HAVE___BUILTIN_CLZ=1
	HAVE___BUILTIN_CLZL=1
	HAVE___BUILTIN_CLZLL=1
	HAVE___BUILTIN_CTZ=1
	HAVE___BUILTIN_CTZL=1
	HAVE___BUILTIN_CTZLL=1
	HAVE_TYPE_TRAITS=1
	ENABLE_PRECONDITIONS=0
	ENABLE_INVARIANTS=0
	CACHE_LINE_SIZE=64
	MASSTREE_MAXKEYLEN=255
)
# Define assertion macros as no-ops since we disabled them
target_compile_options(MASSTREE PRIVATE 
	-Wno-error -Wno-all -Wno-extra
	-Dprecondition\(x,...\)=
	-Dinvariant\(x,...\)=
	-Dmasstree_precondition\(x,...\)=
	-Dmasstree_invariant\(x,...\)=
	-Dalways_assert\(x,...\)=
	-Dmandatory_assert\(x,...\)=
)
target_link_libraries(MASSTREE PUBLIC pthread)

# NOTE: MASSTREE test executable
add_executable(masstree ${PROJECT_SOURCE_DIR}/tests/masstree.cpp ${PROJECT_SOURCE_DIR}/include/baselines/masstree_globals.cc)
target_link_libraries(masstree PUBLIC TEST_FRAME MASSTREE)
target_include_directories(masstree PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${MASSTREE_DIR})
target_compile_options(masstree PRIVATE
	-Dprecondition\(x,...\)=
	-Dinvariant\(x,...\)=
	-Dmasstree_precondition\(x,...\)=
	-Dmasstree_invariant\(x,...\)=
	-Dalways_assert\(x,...\)=
	-Dmandatory_assert\(x,...\)=
)

# NOTE: BWTREE
set(BWTREE_DIR "${PMOS_SOURCE_DIR}/external/BwTree/src")
find_library(BWTREE_ATOMIC_LIB atomic)

if(NOT BWTREE_ATOMIC_LIB)
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libatomic.so
		OUTPUT_VARIABLE BWTREE_ATOMIC_SO
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(EXISTS "${BWTREE_ATOMIC_SO}" AND NOT BWTREE_ATOMIC_SO STREQUAL "libatomic.so")
		set(BWTREE_ATOMIC_LIB "${BWTREE_ATOMIC_SO}")
	endif()
endif()

if(NOT BWTREE_ATOMIC_LIB)
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libatomic.a
		OUTPUT_VARIABLE BWTREE_ATOMIC_A
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(EXISTS "${BWTREE_ATOMIC_A}" AND NOT BWTREE_ATOMIC_A STREQUAL "libatomic.a")
		set(BWTREE_ATOMIC_LIB "${BWTREE_ATOMIC_A}")
	endif()
endif()

if(NOT BWTREE_ATOMIC_LIB AND EXISTS "/usr/lib64/libatomic.so.1")
	set(BWTREE_ATOMIC_LIB "/usr/lib64/libatomic.so.1")
endif()

if(NOT BWTREE_ATOMIC_LIB AND EXISTS "/usr/lib/gcc/x86_64-redhat-linux/8/libatomic.so")
	set(BWTREE_ATOMIC_LIB "/usr/lib/gcc/x86_64-redhat-linux/8/libatomic.so")
endif()

if(NOT BWTREE_ATOMIC_LIB)
	message(FATAL_ERROR "libatomic not found for bwtree target")
endif()

add_executable(bwtree ${PROJECT_SOURCE_DIR}/tests/BwTree.cpp)
target_link_libraries(bwtree PUBLIC TEST_FRAME pthread ${BWTREE_ATOMIC_LIB})
target_include_directories(bwtree PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${BWTREE_DIR})
target_compile_options(bwtree PRIVATE -fpermissive -fno-access-control)

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
	# target_link_libraries(masstree PUBLIC PkgConfig::JEMALLOC)
	# target_link_libraries(b_test PUBLIC libjemalloc.so)
	# target_link_libraries(cpam PUBLIC libjemalloc.so)
endif()

add_executable(data_generator ${PROJECT_SOURCE_DIR}/tests/data_generate.cpp)
target_link_libraries(data_generator PUBLIC TEST_FRAME)
target_include_directories(data_generator PUBLIC ${TEST_FRAME_INCLUDE_DIR})


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
