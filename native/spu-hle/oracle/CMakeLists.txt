# Optional CMake build for the SPU DSP HLE oracle (parity with the Makefile).
#   cmake -S . -B build && cmake --build build && ./build/oracle selftest
cmake_minimum_required(VERSION 3.16)
project(spu_hle_oracle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(oracle oracle_main.cpp)
target_compile_options(oracle PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME selftest COMMAND oracle selftest)
