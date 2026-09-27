# ARM11 benchmark of the display-list interpreter: replays gfx_task.bin
# (GFX_CAPTURE.TXT) from sdmc:/3ds/DK64 and reports the time per run. In
# Azahar the timing follows emulated instruction counts, which makes it a
# quick way to compare interpreter changes.
cmake_minimum_required(VERSION 3.20)
project(dl_bench C CXX)
set(CMAKE_CXX_STANDARD 20)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/Pica.cmake)
add_executable(dl_bench main.cpp ${CMAKE_CURRENT_LIST_DIR}/../../src/renderer/rt64_3ds_dl.cpp)
separate_arguments(bench_flags UNIX_COMMAND "${BENCH_FLAGS}")
target_compile_options(dl_bench PRIVATE ${bench_flags})
target_include_directories(dl_bench PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/renderer)
target_link_libraries(dl_bench ctru m)
add_3dsx(dl_bench TITLE "dl bench")
