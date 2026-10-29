# launcher96: the Old 3DS's way into a game's hidden 96 MB CIA (main.cpp).
#   cmake -S tools/launcher96 -B build-launcher96 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/devkitARM-3ds.cmake
#   cmake --build build-launcher96     # build-launcher96/launcher96.3dsx
cmake_minimum_required(VERSION 3.20)
project(launcher96 CXX)
set(CMAKE_CXX_STANDARD 20)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Pica.cmake")
add_executable(launcher96 main.cpp)
target_compile_options(launcher96 PRIVATE -O2 -Wall)
target_link_libraries(launcher96 PRIVATE ctru m)
add_3dsx(launcher96 TITLE "96 MB launcher" DESCRIPTION "Old 3DS: a game's 96 MB CIA" AUTHOR "rt64-3ds")
