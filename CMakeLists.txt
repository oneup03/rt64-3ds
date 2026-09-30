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

# The RDRAM model (librecomp/addresses.hpp, nmr patch 0003). A game whose
# patches' RAM runs past the default heap start (0x80A00000) sets
# RT64_3DS_HEAP_START above the end of its patches' .bss, and
# RT64_3DS_MEM_SIZE_MB if 16 MB is not enough; rt64_3ds_check_patch_ram()
# below turns an overlap into a build error.
if(DEFINED RT64_3DS_HEAP_START)
    target_compile_definitions(librecomp PUBLIC RECOMP_HEAP_START=${RT64_3DS_HEAP_START})
endif()
if(DEFINED RT64_3DS_MEM_SIZE_MB)
    target_compile_definitions(librecomp PUBLIC RECOMP_MEM_SIZE_MB=${RT64_3DS_MEM_SIZE_MB})
endif()

# rt64_3ds_check_patch_ram(<target> <patches.map>): before <target> links,
# fail if the patches' sections end above the recomp heap's start.
function(rt64_3ds_check_patch_ram target map_file)
    if(DEFINED RT64_3DS_HEAP_START)
        set(heap_start ${RT64_3DS_HEAP_START})
    else()
        set(heap_start 0x80A00000)
    endif()
    add_custom_target(${target}_patch_ram
        COMMAND ${CMAKE_COMMAND} -DMAP=${map_file} -DHEAP_START=${heap_start}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/CheckPatchRam.cmake"
        COMMENT "Checking the patches' RAM against the heap start ${heap_start}"
        VERBATIM)
    add_dependencies(${target} ${target}_patch_ram)
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Pica.cmake")

add_library(rt64_3ds STATIC
    src/renderer/rt64_3ds_common.cpp
    src/renderer/rt64_3ds_null_context.cpp
    src/renderer/rt64_3ds_context.cpp
    src/renderer/rt64_3ds_dl.cpp
    src/renderer/rt64_3ds_texture.cpp
    src/renderer/rt64_3ds_tev.cpp
    src/renderer/rt64_3ds_stereo.cpp
)
add_pica_shader(rt64_3ds "${CMAKE_CURRENT_LIST_DIR}/src/renderer/shaders/rt64_3ds.v.pica")
target_include_directories(rt64_3ds PUBLIC src/renderer)
target_link_libraries(rt64_3ds PUBLIC ultramodern librecomp citro3d ctru m)
target_compile_features(rt64_3ds PUBLIC cxx_std_20)

add_library(recomp_3ds STATIC
    src/platform/recomp3ds_run.cpp
    src/platform/settings_menu.cpp
    src/platform/audio_ndsp.cpp
    src/platform/rom_stream.cpp
    src/platform/naudio_hle.cpp
    src/platform/input_hid.cpp
    src/platform/loadmon.cpp
    src/platform/log.cpp
    src/platform/autotest.cpp
)
target_include_directories(recomp_3ds PUBLIC src/platform)
target_include_directories(recomp_3ds PRIVATE third_party/GamepadMotionHelpers)
target_link_libraries(recomp_3ds PUBLIC rt64_3ds ultramodern librecomp ctru m)
target_compile_features(recomp_3ds PUBLIC cxx_std_20)
