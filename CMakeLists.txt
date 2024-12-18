cmake_minimum_required(VERSION 3.5)
project(ptl_bench)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")
find_package(Portals REQUIRED)
find_package(MPI REQUIRED)

add_executable(ptl_bench "ptl_bench.c" "util.c")
target_compile_features(ptl_bench PRIVATE "c_std_11")
target_include_directories(ptl_bench PUBLIC "./include")
target_link_libraries(ptl_bench PUBLIC "Portals::Portals" "MPI::MPI_C")

add_executable(ptl_memory_bench "ptl_memory_bench.c" "util.c")
target_compile_features(ptl_memory_bench PRIVATE "c_std_11")
target_include_directories(ptl_memory_bench PUBLIC "./include")
target_link_libraries(ptl_memory_bench PUBLIC "Portals::Portals" "MPI::MPI_C")

add_executable(ptl_ping_pong "ptl_ping_pong.c" "util.c")
target_compile_features(ptl_ping_pong PRIVATE "c_std_11")
target_include_directories(ptl_ping_pong PUBLIC "./include")
target_link_libraries(ptl_ping_pong PUBLIC "Portals::Portals" "MPI::MPI_C")

add_executable(ptl_me_none_persistent "ptl_me_none_persistent.c" "util.c")
target_compile_features(ptl_me_none_persistent PRIVATE "c_std_11")
target_include_directories(ptl_me_none_persistent PUBLIC "./include")
target_link_libraries(ptl_me_none_persistent PUBLIC "Portals::Portals" "MPI::MPI_C")

add_executable(pf_bench "page_fault.c")
target_compile_features(pf_bench PRIVATE "c_std_11")