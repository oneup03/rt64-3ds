# cmake -DMAP=<patches.map> -DHEAP_START=<0x...> -P CheckPatchRam.cmake
#
# Reads an ld.lld link map of a game's patches and fails when the end of
# its last section in RDRAM (VMA + size of .text/.rodata/.data/.bss and the
# recomp sections) lies above HEAP_START, where the recomp heap begins: the
# heap would overwrite the patches' data.
if(NOT EXISTS "${MAP}")
    message(FATAL_ERROR "No patch map at ${MAP} (build the patches first)")
endif()
file(STRINGS "${MAP}" lines REGEX "^ *[0-9a-fA-F]+ +[0-9a-fA-F]+ +[0-9a-fA-F]+ +[0-9]+ +\\.[a-z_]+$")
set(end 0)
foreach(line IN LISTS lines)
    string(REGEX MATCH "^ *([0-9a-fA-F]+) +[0-9a-fA-F]+ +([0-9a-fA-F]+)" _ "${line}")
    math(EXPR vma "0x${CMAKE_MATCH_1}")
    math(EXPR size "0x${CMAKE_MATCH_2}")
    # Only sections placed in kseg0 (the relocation and symbol tables sit at 0).
    if(vma GREATER_EQUAL 0x80000000)
        math(EXPR section_end "${vma} + ${size}")
        if(section_end GREATER end)
            set(end ${section_end})
        endif()
    endif()
endforeach()
math(EXPR heap "${HEAP_START}")
math(EXPR end_hex "${end}" OUTPUT_FORMAT HEXADECIMAL)
if(end GREATER heap)
    message(FATAL_ERROR "The patches' RAM ends at ${end_hex}, above the recomp heap's start ${HEAP_START}: raise RT64_3DS_HEAP_START")
endif()
math(EXPR spare "(${heap} - ${end}) / 1024")
message(STATUS "Patches' RAM ends at ${end_hex}; ${spare} KB below the heap at ${HEAP_START}")
