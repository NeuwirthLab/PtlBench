cmake_minimum_required(VERSION 3.5)

find_package(Portals REQUIRED)

add_executable(PtlPerf "ptl_perf.c")

target_compile_features(PtlPerf PRIVATE "c_std_11")
target_link_libraries(PtlPerf PUBLIC "Portals::Portals")
