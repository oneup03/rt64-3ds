cmake_minimum_required(VERSION 3.20)
project(rt64_3ds LANGUAGES C CXX ASM)

# Consumers set RT64_3DS_NMR_DIR to their N64ModernRuntime checkout (with the
# nmr-patches applied) before adding this directory. The runtime is built
# without the mod system, or, with RT64_3DS_STATIC_MODS, with it for mods
# baked into the executable (tools/mod_bake; the console cannot generate or
# patch code at runtime).
if(NOT DEFINED RT64_3DS_NMR_DIR)
    message(FATAL_ERROR "Set RT64_3DS_NMR_DIR to the game's N64ModernRuntime directory")
endif()
option(RT64_3DS_STATIC_MODS "Build the runtime's mod system for baked mods" OFF)
if(RT64_3DS_STATIC_MODS)
    set(RECOMP_NO_MODS OFF CACHE BOOL "rt64-3ds builds the runtime for baked mods" FORCE)
    set(RECOMP_STATIC_MODS ON CACHE BOOL "rt64-3ds builds the runtime for baked mods" FORCE)
else()
    set(RECOMP_NO_MODS ON CACHE BOOL "rt64-3ds builds the runtime without mods" FORCE)
    set(RECOMP_STATIC_MODS OFF CACHE BOOL "rt64-3ds builds the runtime without mods" FORCE)
endif()
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

# rt64_3ds_check_gpr32(<dir>...): RECOMP_GPR32 (32-bit N64 registers; see
# N64Recomp's recomp.h) is only sound for generated code that holds no 64-bit
# register instruction: doubleword arithmetic and shifts, ld/sd/ldl/ldr/sdl/sdr
# of a register, dmult/ddiv, dmtc1/dmfc1. Call it with the directories of the
# recompiled sources (RecompiledFuncs, RecompiledPatches); it fails the
# configure step naming the offending lines.
function(rt64_3ds_check_gpr32)
    set(pattern "ctx->r[0-9]+ = LD\\(|SD\\(ctx->r[0-9]+,|do_ldl|do_ldr|do_sdl|do_sdr|DMULT|DDIV|<< 32|\\+ 32\\)|= ctx->r[0-9]+ << [0-9(]|= ctx->r[0-9]+ >> [0-9(]|ctx->r[0-9]+ = SIGNED\\(ctx->r[0-9]+\\) >>|= ctx->r[0-9]+ [-+] ctx->r[0-9]+;|= ctx->r[0-9]+ [-+] -?0X[0-9A-F]+;|\\.u64 = ctx->r|ctx->r[0-9]+ = [^;]*\\.u64")
    set(sources)
    foreach(dir ${ARGN})
        file(GLOB dir_sources "${dir}/*.c")
        list(APPEND sources ${dir_sources})
    endforeach()
    if(NOT sources)
        message(FATAL_ERROR "rt64_3ds_check_gpr32: no recompiled sources in ${ARGN}")
    endif()
    find_program(RT64_3DS_GREP grep)
    set(hits "")
    if(RT64_3DS_GREP)
        execute_process(COMMAND "${RT64_3DS_GREP}" -nE "${pattern}" ${sources}
                        RESULT_VARIABLE rc OUTPUT_VARIABLE hits ERROR_QUIET)
        if(NOT rc EQUAL 0)
            set(hits "")
        endif()
    else()
        foreach(src ${sources})
            file(STRINGS "${src}" lines REGEX "${pattern}")
            foreach(line ${lines})
                string(APPEND hits "${src}: ${line}\n")
            endforeach()
        endforeach()
    endif()
    if(hits)
        string(SUBSTRING "${hits}" 0 3000 hits)
        message(FATAL_ERROR "RECOMP_GPR32 needs generated code without 64-bit register instructions, but these were found (first lines):\n${hits}")
    endif()
    message(STATUS "RECOMP_GPR32: no 64-bit register instructions in the generated code")
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Pica.cmake")

add_library(rt64_3ds STATIC
    src/renderer/rt64_3ds_common.cpp
    src/renderer/rt64_3ds_null_context.cpp
    src/renderer/rt64_3ds_context.cpp
    src/renderer/rt64_3ds_dl.cpp
    src/renderer/rt64_3ds_interp.cpp
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
    src/platform/game_modes.cpp
    src/platform/bps_stream.cpp
    src/platform/touch_pad.cpp
    src/platform/naudio_hle.cpp
    src/platform/aspmain_hle.cpp
    src/platform/audio_hle_diff.cpp
    src/platform/dsp_synth.cpp
    src/platform/dsp_reverb.cpp
    src/platform/input_hid.cpp
    src/platform/loadmon.cpp
    src/platform/stall_watch.cpp
    src/platform/log.cpp
    src/platform/autotest.cpp
)
target_include_directories(recomp_3ds PUBLIC src/platform)
target_include_directories(recomp_3ds PRIVATE third_party/GamepadMotionHelpers)
target_link_libraries(recomp_3ds PUBLIC rt64_3ds ultramodern librecomp ctru m)
target_compile_features(recomp_3ds PUBLIC cxx_std_20)
