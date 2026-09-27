cmake_minimum_required(VERSION 3.20)
project(rt64_3ds LANGUAGES C CXX ASM)

# Consumers set RT64_3DS_NMR_DIR to their N64ModernRuntime checkout (with the
# nmr-patches applied) before adding this directory; it is built here with
# the mod system off.
if(NOT DEFINED RT64_3DS_NMR_DIR)
    message(FATAL_ERROR "Set RT64_3DS_NMR_DIR to the game's N64ModernRuntime directory")
endif()
set(RECOMP_NO_MODS ON CACHE BOOL "rt64-3ds builds the runtime without mods" FORCE)
add_subdirectory("${RT64_3DS_NMR_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/nmr")

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Pica.cmake")

add_library(rt64_3ds STATIC
    src/renderer/rt64_3ds_common.cpp
    src/renderer/rt64_3ds_null_context.cpp
)
target_include_directories(rt64_3ds PUBLIC src/renderer)
target_link_libraries(rt64_3ds PUBLIC ultramodern librecomp citro3d ctru m)
target_compile_features(rt64_3ds PUBLIC cxx_std_20)

add_library(recomp_3ds STATIC
    src/platform/recomp3ds_run.cpp
    src/platform/audio_ndsp.cpp
    src/platform/naudio_hle.cpp
    src/platform/input_hid.cpp
    src/platform/loadmon.cpp
    src/platform/log.cpp
    src/platform/autotest.cpp
)
target_include_directories(recomp_3ds PUBLIC src/platform)
target_link_libraries(recomp_3ds PUBLIC rt64_3ds ultramodern librecomp ctru m)
target_compile_features(recomp_3ds PUBLIC cxx_std_20)
