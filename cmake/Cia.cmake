# add_cia(<target> RSF <template.rsf> TITLE .. PRODUCT .. UNIQUEID 0x.....
#         [ICON png] [BANNER_PNG png BANNER_WAV wav])
# Produces <target>.cia from the target's ELF with makerom (and bannertool
# for the banner when both banner inputs are given). Both tools are found
# through the toolchain file; without makerom the target is skipped.
function(add_cia target)
    cmake_parse_arguments(A "" "RSF;TITLE;PRODUCT;UNIQUEID;ICON;BANNER_PNG;BANNER_WAV" "" ${ARGN})
    if(NOT MAKEROM_EXE)
        message(STATUS "makerom not found: no CIA target for ${target}")
        return()
    endif()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(rsf "${out}.rsf")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${A_RSF}")   # re-generate on template edits
    file(READ "${A_RSF}" rsf_text)
    string(REPLACE "@TITLE@" "${A_TITLE}" rsf_text "${rsf_text}")
    string(REPLACE "@PRODUCT@" "${A_PRODUCT}" rsf_text "${rsf_text}")
    string(REPLACE "@UNIQUEID@" "${A_UNIQUEID}" rsf_text "${rsf_text}")
    file(WRITE "${rsf}" "${rsf_text}")
    set(deps "${target}" "${rsf}")
    set(extra)
    if(A_ICON)
        add_custom_command(OUTPUT "${out}.cia.smdh"
            COMMAND "${SMDHTOOL_EXE}" --create "${A_TITLE}" "${A_TITLE}" "unknown" "${A_ICON}" "${out}.cia.smdh"
            DEPENDS "${A_ICON}")
        list(APPEND deps "${out}.cia.smdh")
        list(APPEND extra -icon "${out}.cia.smdh")
    endif()
    if(A_BANNER_PNG AND A_BANNER_WAV AND BANNERTOOL_EXE)
        add_custom_command(OUTPUT "${out}.bnr"
            COMMAND "${BANNERTOOL_EXE}" makebanner -i "${A_BANNER_PNG}" -a "${A_BANNER_WAV}" -o "${out}.bnr"
            DEPENDS "${A_BANNER_PNG}" "${A_BANNER_WAV}")
        list(APPEND deps "${out}.bnr")
        list(APPEND extra -banner "${out}.bnr")
    endif()
    add_custom_command(OUTPUT "${out}.cia"
        COMMAND "${MAKEROM_EXE}" -f cia -o "${out}.cia" -elf "$<TARGET_FILE:${target}>" -rsf "${rsf}" ${extra} -exefslogo -target t
        DEPENDS ${deps} COMMENT "makerom ${target}.cia")
    add_custom_target("${target}_cia" DEPENDS "${out}.cia")
endfunction()
