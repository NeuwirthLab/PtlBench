cmake_minimum_required(VERSION 3.5)

find_package(Portals REQUIRED)

add_executable(PtlComponents "ptl_components.c")

target_compile_features(PtlComponents PRIVATE "c_std_11")
target_link_libraries(PtlComponents PUBLIC "Portals::Portals")
