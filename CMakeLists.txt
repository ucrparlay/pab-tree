cmake_minimum_required(VERSION 3.15)

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
  add_compile_definitions(LEAF${LEAF})
endif()

if(DEBUG)
	set(CMAKE_BUILD_TYPE "Debug")
	add_compile_options(-g -march=native -DDEBUG)
	if(NOT APPLE)
		add_compile_options(-mcx16)
	endif()
	if(CCP)
		message("CCP=ON")
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
elseif(JEMA)
	include_directories("/usr/local/lib")
	set(CMAKE_BUILD_RPATH "/usr/local/lib")
endif()

add_subdirectory(include/parlaylib)

# NOTE: clear CMAKE_MODULE_PATH to avoid finding parlay's cmake files
list(REMOVE_ITEM CMAKE_MODULE_PATH "${PARLAY_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${PMOS_SOURCE_DIR}/cmake")


# NOTE: PMOS
add_library(PMOS INTERFACE)
set(PMOS_INCLUDE_DIR "${PMOS_SOURCE_DIR}/include/pmos")
target_link_libraries(PMOS INTERFACE parlay)
target_include_directories(PMOS INTERFACE $<BUILD_INTERFACE:${PMOS_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_executable(b_test ${PROJECT_SOURCE_DIR}/tests/b_test.cpp)
target_link_libraries(b_test PUBLIC PMOS)
target_include_directories(b_test PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_executable(b_ccp ${PROJECT_SOURCE_DIR}/tests/b_ccp.cpp)
target_link_libraries(b_ccp PUBLIC PMOS)
target_include_directories(b_ccp PUBLIC ${PROJECT_SOURCE_DIR}/include)
#
# add_executable(p_test ${PROJECT_SOURCE_DIR}/tests/p_test.cpp)
# target_link_libraries(p_test PUBLIC PMOS)
# target_include_directories(p_test PUBLIC ${PROJECT_SOURCE_DIR}/include)
#
add_executable(baselines ${PROJECT_SOURCE_DIR}/tests/baselines.cpp)
target_link_libraries(baselines PUBLIC PMOS)
target_include_directories(baselines PUBLIC ${PROJECT_SOURCE_DIR}/include)
#
# add_executable(testers ${PROJECT_SOURCE_DIR}/tests/testers.cpp)
# target_link_libraries(testers PUBLIC PMOS)
# target_include_directories(testers PUBLIC ${PROJECT_SOURCE_DIR}/include)

if(JEMA)
	target_link_libraries(b_test PUBLIC libjemalloc.so)
	# target_link_libraries(p_test PUBLIC libjemalloc.so)
	# target_link_libraries(baselines PUBLIC libjemalloc.so)
endif()

add_executable(data_generator ${PROJECT_SOURCE_DIR}/tests/data_generate.cpp)
target_link_libraries(data_generator PUBLIC PMOS)
target_include_directories(data_generator PUBLIC ${PROJECT_SOURCE_DIR}/include)


# add_executable(data_washer ${PROJECT_SOURCE_DIR}/tests/data_wash.cpp)
# target_link_libraries(data_washer PUBLIC PMOS)
# target_include_directories(data_washer PUBLIC ${PROJECT_SOURCE_DIR}/include)

file(GLOB_RECURSE all_files
  ${PROJECT_SOURCE_DIR}/include/pmos/*
)


FIND_PROGRAM(CLANG_FORMAT "clang-format")

IF(CLANG_FORMAT)
	add_custom_target(
		format
		WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
		COMMAND ${CLANG_FORMAT}
		-style=file
		-i
		${all_files}
	)
endif()
