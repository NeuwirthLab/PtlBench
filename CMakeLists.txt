cmake_minimum_required(VERSION 3.5)
project(PtlBench)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")

add_subdirectory(components)
add_subdirectory(perf)
