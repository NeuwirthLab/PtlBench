cmake_minimum_required(VERSION 3.20)
project(ptl_bench LANGUAGES C)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")

find_package(Portals REQUIRED)
find_package(MPI REQUIRED COMPONENTS C)

add_executable(ptl_bench "ptl_bench.c" "util.c")
target_compile_features(ptl_bench PRIVATE c_std_11)
target_include_directories(ptl_bench PRIVATE "./include")
target_link_libraries(ptl_bench PRIVATE "Portals::Portals" "MPI::MPI_C")

add_executable(ptl_memory_bench "ptl_memory_bench.c" "util.c")
target_compile_features(ptl_memory_bench PRIVATE c_std_11)
target_include_directories(ptl_memory_bench PRIVATE "./include")
target_link_libraries(ptl_memory_bench PRIVATE "Portals::Portals" "MPI::MPI_C")

add_executable(ptl_ping_pong "ptl_ping_pong.c" "util.c")
target_compile_features(ptl_ping_pong PRIVATE c_std_11)
target_include_directories(ptl_ping_pong PRIVATE "./include")
target_link_libraries(ptl_ping_pong PRIVATE "Portals::Portals" "MPI::MPI_C")

add_executable(ptl_me_none_persistent "ptl_me_none_persistent.c" "util.c")
target_compile_features(ptl_me_none_persistent PRIVATE c_std_11)
target_include_directories(ptl_me_none_persistent PRIVATE "./include")
target_link_libraries(ptl_me_none_persistent PRIVATE "Portals::Portals" "MPI::MPI_C")

add_executable(pf_bench "page_fault.c")
target_compile_features(pf_bench PRIVATE c_std_11)

include(GNUInstallDirs)
install(
  TARGETS ptl_bench ptl_memory_bench ptl_ping_pong ptl_me_none_persistent pf_bench
  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
