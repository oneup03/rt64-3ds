# A console check of the GPU path's vertex shader (shaders/rt64_3ds_gpu.v.pica)
# apart from the renderer: one triangle through the CPU path's program and
# one through the GPU path's (its raw layout, an indexed draw, a palette
# uniform), side by side on the top screen. Build with the devkitARM
# toolchain file (cmake/devkitARM-3ds.cmake) and run the .3dsx in Azahar
# (the port skill's azahar-run.sh); sdmc:/3ds/gpu_test/result.txt gets the log.
cmake_minimum_required(VERSION 3.20)
project(gpu_test C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/Pica.cmake)
add_executable(gpu_test main.cpp)
add_pica_shader(gpu_test "${CMAKE_CURRENT_LIST_DIR}/../../src/renderer/shaders/rt64_3ds.v.pica")
add_pica_shader(gpu_test "${CMAKE_CURRENT_LIST_DIR}/test_min.v.pica")
target_link_libraries(gpu_test citro3d ctru m)
add_3dsx(gpu_test TITLE "gpu test")
