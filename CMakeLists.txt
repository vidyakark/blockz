cmake_minimum_required(VERSION 3.14)
project(blockz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)

add_library(blockz_core STATIC
  src/crc32.cpp src/huffman.cpp src/deflate_enc.cpp src/inflate.cpp src/gzip.cpp)
target_include_directories(blockz_core PUBLIC include)
target_link_libraries(blockz_core PUBLIC Threads::Threads)
target_compile_options(blockz_core PRIVATE -Wall -Wextra)

add_executable(blockz src/main.cpp)
target_link_libraries(blockz PRIVATE blockz_core)
add_executable(bench tools/bench.cpp)
target_link_libraries(bench PRIVATE blockz_core)
add_executable(gen_data tools/gen_data.cpp)

enable_testing()
add_executable(run_tests tests/test_main.cpp)
target_link_libraries(run_tests PRIVATE blockz_core)
add_test(NAME unit COMMAND run_tests)
