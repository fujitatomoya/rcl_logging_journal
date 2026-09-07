cmake_minimum_required(VERSION 3.20)
project(rcl_logging_journal_benchmark CXX)

# Standalone driver for scripts/benchmark/run_matrix.sh. It is intentionally
# not part of the ROS package build: it dlopen()s whichever backend library is
# requested at runtime, so all compared backends must be in LD_LIBRARY_PATH
# (source the workspace before running).

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(rcl_logging_interface REQUIRED)
find_package(rcutils REQUIRED)

add_executable(bench_backend bench_backend.cpp)
target_compile_options(bench_backend PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(bench_backend PRIVATE
  rcl_logging_interface::rcl_logging_interface
  rcutils::rcutils
  ${CMAKE_DL_LIBS})
