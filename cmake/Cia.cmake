# add_cia(<target> RSF <template.rsf> TITLE .. PRODUCT .. UNIQUEID 0x.....
#         [ICON png] [DESCRIPTION ..] [AUTHOR ..]
#         [BANNER_PNG png | BANNER_CGFX cgfx] [BANNER_WAV wav])
# Produces <target>.cia from the target's ELF with makerom. With bannertool
# (found through the toolchain file) the SMDH is made by it, so it can carry
# the flags the HOME Menu needs for a 3D banner (smdhtool cannot set them),
# and the banner is either a flat image or a CGFX model, with its sound
# (3 s at most, two channels). Without makerom the target is skipped.
function(add_cia target)
    cmake_parse_arguments(A "" "RSF;TITLE;PRODUCT;UNIQUEID;ICON;DESCRIPTION;AUTHOR;BANNER_PNG;BANNER_CGFX;BANNER_WAV" "" ${ARGN})
    if(NOT A_DESCRIPTION)
        set(A_DESCRIPTION "${A_TITLE}")
    endif()
    # No AUTHOR: a blank publisher. Both tools need the argument, and a custom
    # command drops an empty one (bannertool also refuses it), so a space.
    if("${A_AUTHOR}" STREQUAL "")
        set(A_AUTHOR " ")
    endif()
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
    if(A_ICON AND BANNERTOOL_EXE)
        # extendedbanner: the HOME Menu ignores a CGFX banner without it.
        add_custom_command(OUTPUT "${out}.cia.smdh"
            COMMAND "${BANNERTOOL_EXE}" makesmdh -s "${A_TITLE}" -l "${A_DESCRIPTION}" -p "${A_AUTHOR}" -i "${A_ICON}"
                    -f "visible,allow3d,recordusage,extendedbanner" -o "${out}.cia.smdh"
            DEPENDS "${A_ICON}")
        list(APPEND deps "${out}.cia.smdh")
        list(APPEND extra -icon "${out}.cia.smdh")
    elseif(A_ICON)
        add_custom_command(OUTPUT "${out}.cia.smdh"
            COMMAND "${SMDHTOOL_EXE}" --create "${A_TITLE}" "${A_DESCRIPTION}" "${A_AUTHOR}" "${A_ICON}" "${out}.cia.smdh"
            DEPENDS "${A_ICON}")
        list(APPEND deps "${out}.cia.smdh")
        list(APPEND extra -icon "${out}.cia.smdh")
    endif()
    if(A_BANNER_WAV AND BANNERTOOL_EXE AND (A_BANNER_CGFX OR A_BANNER_PNG))
        if(A_BANNER_CGFX)
            set(banner_in -ci "${A_BANNER_CGFX}")
            set(banner_dep "${A_BANNER_CGFX}")
        else()
            set(banner_in -i "${A_BANNER_PNG}")
            set(banner_dep "${A_BANNER_PNG}")
        endif()
        add_custom_command(OUTPUT "${out}.bnr"
            COMMAND "${BANNERTOOL_EXE}" makebanner ${banner_in} -a "${A_BANNER_WAV}" -o "${out}.bnr"
            DEPENDS ${banner_dep} "${A_BANNER_WAV}")
        list(APPEND deps "${out}.bnr")
        list(APPEND extra -banner "${out}.bnr")
    endif()
    add_custom_command(OUTPUT "${out}.cia"
        COMMAND "${MAKEROM_EXE}" -f cia -o "${out}.cia" -elf "$<TARGET_FILE:${target}>" -rsf "${rsf}" ${extra} -exefslogo -target t
        DEPENDS ${deps} COMMENT "makerom ${target}.cia")
    add_custom_target("${target}_cia" DEPENDS "${out}.cia")
endfunction()
