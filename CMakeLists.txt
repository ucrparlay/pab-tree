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

if(NOT APPLE)
	# NOTE: PMA
	add_executable(pma ${PROJECT_SOURCE_DIR}/tests/pma.cpp)
	target_link_libraries(pma PUBLIC TEST_FRAME PMA)
	target_include_directories(pma PUBLIC ${TEST_FRAME_INCLUDE_DIR} ${PMA_INCLUDE_DIR})
	target_compile_definitions(pma PUBLIC PARALLEL=1 PARLAY=1)
endif()

# NOTE: GoogleTest for b_ccp
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
