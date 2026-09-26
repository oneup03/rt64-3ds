# add_pica_shader(<target> <file.v.pica>): assembles the shader with picasso and
# adds the bin2s object plus a generated header (<name>_shbin.h) to <target>.
function(add_pica_shader target pica)
    get_filename_component(name "${pica}" NAME_WE)
    string(REPLACE "." "_" cname "${name}")
    set(shbin "${CMAKE_CURRENT_BINARY_DIR}/${name}.shbin")
    set(sfile "${CMAKE_CURRENT_BINARY_DIR}/${name}_shbin.s")
    set(hfile "${CMAKE_CURRENT_BINARY_DIR}/${name}_shbin.h")
    add_custom_command(OUTPUT "${shbin}"
        COMMAND "${PICASSO_EXE}" -o "${shbin}" "${pica}"
        DEPENDS "${pica}" COMMENT "picasso ${name}")
    add_custom_command(OUTPUT "${sfile}" "${hfile}"
        COMMAND "${BIN2S_EXE}" -a 4 -H "${hfile}" "${shbin}" > "${sfile}"
        DEPENDS "${shbin}" COMMENT "bin2s ${name}")
    target_sources("${target}" PRIVATE "${sfile}")
    target_include_directories("${target}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# add_3dsx(<target> [ICON png] [TITLE ..] [DESCRIPTION ..] [AUTHOR ..]):
# produces <target>.3dsx (+ .smdh) next to the ELF.
function(add_3dsx target)
    cmake_parse_arguments(A "" "ICON;TITLE;DESCRIPTION;AUTHOR" "" ${ARGN})
    if(NOT A_TITLE)
        set(A_TITLE "${target}")
    endif()
    if(NOT A_DESCRIPTION)
        set(A_DESCRIPTION "${target}")
    endif()
    if(NOT A_AUTHOR)
        set(A_AUTHOR "unknown")
    endif()
    set(elf "$<TARGET_FILE:${target}>")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(smdh_args)
    if(A_ICON)
        add_custom_command(OUTPUT "${out}.smdh"
            COMMAND "${SMDHTOOL_EXE}" --create "${A_TITLE}" "${A_DESCRIPTION}" "${A_AUTHOR}" "${A_ICON}" "${out}.smdh"
            DEPENDS "${A_ICON}" COMMENT "smdhtool ${target}")
        set(smdh_args --smdh=${out}.smdh)
        set(smdh_dep "${out}.smdh")
    endif()
    add_custom_command(OUTPUT "${out}.3dsx"
        COMMAND "${TDSXTOOL_EXE}" "${elf}" "${out}.3dsx" ${smdh_args}
        DEPENDS "${target}" ${smdh_dep} COMMENT "3dsxtool ${target}")
    add_custom_target("${target}_3dsx" ALL DEPENDS "${out}.3dsx")
endfunction()
