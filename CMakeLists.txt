cmake_minimum_required(VERSION 3.5)
project(ptl_bench)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")
find_package(Portals REQUIRED)
find_package(MPI REQUIRED)

add_executable(ptl_bench "ptl_bench.c" "util.c")

target_compile_features(ptl_bench PRIVATE "c_std_11")
#target_include_directories(ptl_bench ".")
target_link_libraries(ptl_bench PUBLIC "Portals::Portals" "MPI::MPI_C")