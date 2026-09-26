# CMake toolchain for Nintendo 3DS homebrew with devkitARM + libctru + citro3d.
#
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=<this file> ...
# Needs DEVKITPRO (and optionally DEVKITARM) in the environment or cache.
# devkitPro ships a 3DS.cmake with its cmake package; this file is
# self-contained so a user-prefix install (no /opt/devkitpro) works too.

if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    else()
        message(FATAL_ERROR "DEVKITPRO is not set (export it or pass -DDEVKITPRO=...)")
    endif()
endif()
if(NOT DEFINED DEVKITARM)
    if(DEFINED ENV{DEVKITARM})
        set(DEVKITARM "$ENV{DEVKITARM}")
    else()
        set(DEVKITARM "${DEVKITPRO}/devkitARM")
    endif()
endif()
set(DEVKITPRO "${DEVKITPRO}" CACHE PATH "devkitPro root")
set(DEVKITARM "${DEVKITARM}" CACHE PATH "devkitARM root")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)
set(NINTENDO_3DS TRUE)

set(TOOL_PREFIX "${DEVKITARM}/bin/arm-none-eabi-")
set(CMAKE_C_COMPILER   "${TOOL_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOL_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${TOOL_PREFIX}gcc")
set(CMAKE_AR           "${TOOL_PREFIX}gcc-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${TOOL_PREFIX}gcc-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${TOOL_PREFIX}strip" CACHE FILEPATH "")
set(CMAKE_SIZE         "${TOOL_PREFIX}size" CACHE FILEPATH "")

# The link test needs 3dsx.specs; a static-library try_compile avoids it.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARCH_FLAGS "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -mtp=soft -mword-relocations")
set(CMAKE_C_FLAGS_INIT   "${ARCH_FLAGS} -ffunction-sections -fdata-sections -D__3DS__ -DARM11")
set(CMAKE_CXX_FLAGS_INIT "${ARCH_FLAGS} -ffunction-sections -fdata-sections -D__3DS__ -DARM11 -fexceptions -frtti")
set(CMAKE_ASM_FLAGS_INIT "${ARCH_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=3dsx.specs ${ARCH_FLAGS} -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/libctru" "${DEVKITPRO}/portlibs/3ds" "${DEVKITARM}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

include_directories(SYSTEM "${DEVKITPRO}/libctru/include" "${DEVKITPRO}/portlibs/3ds/include")
link_directories("${DEVKITPRO}/libctru/lib" "${DEVKITPRO}/portlibs/3ds/lib")

# Host tools shipped with devkitPro.
set(DKP_TOOLS "${DEVKITPRO}/tools/bin")
find_program(PICASSO_EXE  picasso  HINTS "${DKP_TOOLS}")
find_program(BIN2S_EXE    bin2s    HINTS "${DKP_TOOLS}" "${DEVKITARM}/bin")
find_program(SMDHTOOL_EXE smdhtool HINTS "${DKP_TOOLS}")
find_program(TDSXTOOL_EXE 3dsxtool HINTS "${DKP_TOOLS}")
find_program(MAKEROM_EXE  makerom  HINTS "${DKP_TOOLS}")
find_program(BANNERTOOL_EXE bannertool HINTS "${DKP_TOOLS}")
