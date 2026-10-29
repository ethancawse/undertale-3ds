# AOT modules (source/ctr_aot_module.h): included by CMakeLists.txt when CTRPORT_AOT_DIR comes from
# tools/aot/prepare.py --chapters. Each chapter's generated code is compiled with the main program's settings and
# linked on its own, at the start of the heap's address range, against the main program's symbols (--just-symbols).
# The modules go into the romfs as aot/<chapter folder>.aotm, which this file packs into the .3dsx (the .cia's RomFS
# comes from CMakeLists.txt).
#
# The main program keeps every symbol the modules use: aot_hostsyms.cmake lists them from the modules' objects into
# aot_hostsyms.c, which refers to each of them, so --gc-sections keeps them, and gives the loader the addresses to check
# a module against.

file(STRINGS "${CTRPORT_AOT_DIR}/aot_modules.txt" AOTM_LINES REGEX "^part ")
set(AOTM_PARTS "")
foreach(line IN LISTS AOTM_LINES)
    string(REGEX REPLACE "^part " "" part "${line}")
    list(APPEND AOTM_PARTS "${part}")
endforeach()
set(AOTM_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot")
# Retain the original host-table placement slot without moving its input section. ARM GCC ignores
# the retain attribute; an explicit linker root keeps it through --gc-sections on both models.
target_link_options(ctrport PRIVATE "-Wl,--undefined=ctrAotHostSymbols_slot74")
set(CTRPORT_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${AOTM_DIR}" "${CTRPORT_ROMFS_DIR}/aot")
foreach(tool NM OBJCOPY)
    if(NOT CMAKE_${tool})
        message(FATAL_ERROR "AOT modules need ${tool} from devkitARM (CMAKE_${tool} is not set)")
    endif()
endforeach()

# Everything a module holds is compiled exactly as the main program is.
function(aotm_compile_like_main target)
    target_compile_definitions(${target} PRIVATE $<TARGET_PROPERTY:ctrport,COMPILE_DEFINITIONS>)
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:ctrport,INCLUDE_DIRECTORIES>)
    target_compile_options(${target} PRIVATE $<TARGET_PROPERTY:ctrport,COMPILE_OPTIONS>)
endfunction()

set(AOTM_OBJECT_LISTS "")
foreach(part IN LISTS AOTM_PARTS)
    file(GLOB srcs CONFIGURE_DEPENDS "${CTRPORT_AOT_DIR}/${part}/aot_gen_*.c" "${CTRPORT_AOT_DIR}/${part}/aot_table.c")
    if(NOT EXISTS "${CTRPORT_AOT_DIR}/${part}/aot_table.c")
        message(FATAL_ERROR "${CTRPORT_AOT_DIR}/${part} has no aot_table.c: run tools/aot/prepare.py --chapters again")
    endif()
    # Typed regions (CMakeLists.txt): from a typed list, aot_typed.c, linked into the module whatever else is left out
    # (aot_module_link.cmake keeps every object but the numbered aot_gen and aot_typed files), and --gc-sections drops the
    # regions no kept file calls. From the player's data (prepare.py --typed all), aot_typed_bind.c, always linked, and
    # the regions in aot_typed_<n>.c, which the module linker leaves out first, the lowest-priority first.
    set(typed_srcs "")
    if(CTRPORT_TYPED_AOT)
        file(GLOB typed_srcs CONFIGURE_DEPENDS "${CTRPORT_AOT_DIR}/${part}/aot_typed.c" "${CTRPORT_AOT_DIR}/${part}/aot_typed_*.c")
        if(NOT typed_srcs)
            message(FATAL_ERROR "${CTRPORT_AOT_DIR}/${part} has no aot_typed.c: run tools/aot/prepare.py --chapters --typed")
        endif()
        list(APPEND srcs ${typed_srcs})
    endif()
    add_library(aotm_${part} OBJECT ${srcs})
    aotm_compile_like_main(aotm_${part})
    target_compile_definitions(aotm_${part} PRIVATE CTR_AOT_MODULE)
    # As for code linked in (CMakeLists.txt): large generated functions build much faster without these.
    set_source_files_properties(${srcs} PROPERTIES COMPILE_OPTIONS "-fno-schedule-insns;-fno-var-tracking;-fno-var-tracking-assignments")
    if(typed_srcs)
        set_source_files_properties(${typed_srcs} PROPERTIES COMPILE_OPTIONS "${CTRPORT_TYPED_AOT_OPTIONS}"
                                    COMPILE_DEFINITIONS "${CTRPORT_TYPED_DEFS}")
    endif()
    list(APPEND AOTM_OBJECT_LISTS "$<JOIN:$<TARGET_OBJECTS:aotm_${part}>,|>")
endforeach()
list(JOIN AOTM_OBJECT_LISTS "|" AOTM_ALL_OBJECTS)
list(TRANSFORM AOTM_PARTS PREPEND "aotm_" OUTPUT_VARIABLE AOTM_PART_TARGETS)

set(AOTM_HOSTSYMS "${AOTM_DIR}/aot_hostsyms.c" "${AOTM_DIR}/aot_hostsyms.h")
# Run again whenever a module's objects change (regenerated code may use other symbols); aot_hostsyms.cmake rewrites
# the files only when the list changes, so the stamp is what says it ran.
set(AOTM_HOSTSYMS_OBJECTS "")
foreach(part IN LISTS AOTM_PARTS)
    list(APPEND AOTM_HOSTSYMS_OBJECTS "$<TARGET_OBJECTS:aotm_${part}>")
endforeach()
add_custom_command(OUTPUT "${AOTM_DIR}/aot_hostsyms.stamp"
    BYPRODUCTS ${AOTM_HOSTSYMS} "${AOTM_DIR}/aot_numeric_string.cmake"
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DOUT_DIR=${AOTM_DIR}" "-DOBJECTS=${AOTM_ALL_OBJECTS}"
            "-DGAME=${CTRPORT_GAME}" -P "${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_hostsyms.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${AOTM_DIR}/aot_hostsyms.stamp"
    DEPENDS ${AOTM_PART_TARGETS} ${AOTM_HOSTSYMS_OBJECTS} "${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_hostsyms.cmake"
    COMMENT "Listing the symbols the AOT modules use"
    VERBATIM)
add_custom_target(aotm_hostsyms DEPENDS "${AOTM_DIR}/aot_hostsyms.stamp")
target_sources(ctrport PRIVATE "${AOTM_DIR}/aot_hostsyms.c")
add_dependencies(ctrport aotm_hostsyms)

# The header, and the module's copy of the host symbols.
add_library(aotm_common OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_module_header.c" "${AOTM_DIR}/aot_hostsyms.c")
aotm_compile_like_main(aotm_common)
target_include_directories(aotm_common PRIVATE "${AOTM_DIR}")
target_compile_definitions(aotm_common PRIVATE CTR_AOT_MODULE)
add_dependencies(aotm_common aotm_hostsyms)

# The main program's --wrap options: a module's malloc must be the main program's malloc.
get_target_property(AOTM_MAIN_LINK_OPTIONS ctrport LINK_OPTIONS)
set(AOTM_WRAPS "")
foreach(opt IN LISTS AOTM_MAIN_LINK_OPTIONS)
    if(opt MATCHES "--wrap=")
        list(APPEND AOTM_WRAPS "${opt}")
    endif()
endforeach()
separate_arguments(AOTM_ARCH UNIX_COMMAND "${CTR_ARCH_SETTINGS}")
list(JOIN AOTM_ARCH "|" AOTM_ARCH_JOINED)
list(JOIN AOTM_WRAPS "|" AOTM_WRAPS_JOINED)
math(EXPR AOTM_REGION_BYTES "${CTRPORT_AOT_REGION_MB} * 1024 * 1024")
# binutils' size, next to objcopy: aot_module_link.cmake sizes the objects it may leave out with it.
get_filename_component(AOTM_BINUTILS_DIR "${CMAKE_OBJCOPY}" DIRECTORY)
get_filename_component(AOTM_OBJCOPY_NAME "${CMAKE_OBJCOPY}" NAME)
string(REGEX REPLACE "objcopy" "size" AOTM_SIZE_NAME "${AOTM_OBJCOPY_NAME}")
set(AOTM_SIZE "${AOTM_BINUTILS_DIR}/${AOTM_SIZE_NAME}")

# Each module is linked by aot_module_link.cmake, which leaves out its lowest-ranked generated files if it would not
# otherwise fit its region (the space for generated code), and says so.
set(CTRPORT_AOT_MODULE_FILES "")
foreach(part IN LISTS AOTM_PARTS)
    set(elf "${AOTM_DIR}/${part}.elf")
    set(module "${CTRPORT_ROMFS_DIR}/aot/${part}.aotm")
    add_custom_command(OUTPUT "${module}"
        COMMAND "${CMAKE_COMMAND}" "-DCC=${CMAKE_C_COMPILER}" "-DARCH=${AOTM_ARCH_JOINED}"
                "-DLDSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_module.ld" "-DREGION=${AOTM_REGION_BYTES}"
                "-DMAIN_ELF=$<TARGET_FILE:ctrport>" "-DWRAPS=${AOTM_WRAPS_JOINED}" "-DMAP=${AOTM_DIR}/${part}.map"
                "-DELF=${elf}" "-DMODULE=${module}" "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:aotm_${part}>,|>"
                "-DCOMMON=$<JOIN:$<TARGET_OBJECTS:aotm_common>,|>" "-DTABLE_C=${CTRPORT_AOT_DIR}/${part}/aot_table.c"
                "-DNM=${CMAKE_NM}" "-DOBJCOPY=${CMAKE_OBJCOPY}" "-DSIZE=${AOTM_SIZE}" "-DPART=${part}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_module_link.cmake"
        DEPENDS ctrport aotm_${part} aotm_common $<TARGET_OBJECTS:aotm_${part}> $<TARGET_OBJECTS:aotm_common>
                "${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_module.ld" "${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_module_link.cmake"
        COMMENT "Linking AOT module ${part}"
        VERBATIM)
    list(APPEND CTRPORT_AOT_MODULE_FILES "${module}")
endforeach()
add_custom_target(aotm_modules ALL DEPENDS ${CTRPORT_AOT_MODULE_FILES})

# The .3dsx, as ctr_create_3dsx makes it, with the romfs.
set(AOTM_3DSX "${CTRPORT_3DSX_OUTPUT}")
add_custom_command(OUTPUT "${AOTM_3DSX}"
    COMMAND "${CTR_3DSXTOOL_EXE}" "$<TARGET_FILE:ctrport>" "${AOTM_3DSX}" "--smdh=${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh"
            "--romfs=${CTRPORT_ROMFS_RAW}"
    DEPENDS ctrport "${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh" ${CTRPORT_AOT_MODULE_FILES}
            "${CTRPORT_ROMFS_DIR}/bench_scenes.txt" "${COVERAGE_ROMFS_FILE}" "${CTRPORT_ROMFS_RAW}"
    COMMENT "Building 3DSX executable target ctrport_3dsx"
    VERBATIM)
add_custom_target(ctrport_3dsx ALL DEPENDS "${AOTM_3DSX}")
# The modules are linked by aotm_modules only (a target that lists another's outputs would link them a second time,
# in parallel with the first, and read a half-written ELF): this target waits for it.
add_dependencies(ctrport_3dsx aotm_modules)
dkp_set_target_file(ctrport_3dsx "${AOTM_3DSX}")
