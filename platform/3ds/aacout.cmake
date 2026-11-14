# AAC retirement (UNDERTALE only). Insert reusable input regions at the original
# vendor boundary inside the existing output sections, retaining their extents.
# Inputs deliberately start with .aacout, not .text/.rodata: earlier wildcards
# must not consume them. See late_reserve.ld's AAC append contract.
# Explicit assignment, in merge order: append an exact input name to ONE list.
# Do not insert, sort, grow an earlier input, or move existing late constants.
# The synthetic tenant is compiled only by tests with CTRPORT_AACSLOT_TEST;
# the emitter always puts it AFTER these production lists.
# One ruled exception (lead 11-09, dv8q): battlesplit14 moved once to the end, proved by a relocation-driven move
# proof. Since 11-10 (lead, layout's Q1) new tenants append AFTER it again, in the slot's suffix, so nothing that
# exists moves; a later battlesplit14 growth moves only the tenants after it (layoutup, bsup and btup: of those only
# battlesplit18's frame layout runs every frame, in battle rooms; the rest at a room change, a set-up or a menu row).
# Suffix growth (lead 11-10, dv8u6): an earlier suffix input (layoutup, bsup, btup) may grow in place only with a lead
# ruling and a relocation-driven move proof for every tenant behind it; bootahead, benchsettings and battlesplit14
# never without a new ruling; NEW functions still go to a new last input. This settles "growth inside a function
# already in AAC stays in place" against "do not grow an earlier input" (above) for the suffix only.
# btgrey (lead 11-13, battlesplit item A: z_battletouch.c's Battle stylus row grey under Top screen) is such a new last
# input, after touchoverworld: nothing that exists moves.
# S4 occupies part of the retired Frame hole. Its padding stays fixed, so
# battlesplit14 and every existing tenant retain their dv8q addresses.
set(aacout_text_hole_inputs
    .aacout.text.dlgtop4
)
set(aacout_rodata_hole_inputs
    .aacout.rodata.dlgtop4
)
set(aacout_text_inputs
    .aacout.text.bootahead
    .aacout.text.benchsettings
    .aacout.text.battlesplit14
    .aacout.text.layoutup
    .aacout.text.bsup
    .aacout.text.btup
    .aacout.text.touchquiz
    .aacout.text.touchmenus
    .aacout.text.touchoverworld
    .aacout.text.btgrey
)
set(aacout_rodata_inputs
    .aacout.rodata.bootahead
    .aacout.rodata.benchsettings
    .aacout.rodata.battlesplit14
    .aacout.rodata.layoutup
    .aacout.rodata.bsup
    .aacout.rodata.btup
    .aacout.rodata.touchquiz
    .aacout.rodata.touchmenus
    .aacout.rodata.touchoverworld
    .aacout.rodata.btgrey
)
# Development-only inputs (lead 11-11, devdiet decision 2; CTRPORT_DEVONLY_AAC, UNDERTALE builds without the release
# source URL): labguard (z_labguard.o, every code and constant section), the pen log's own pairs (z_battletouch.c's
# .text_/.rodata_battletouchpen, z_battlesplit.c's .text_/.rodata_battlesplitpen) and bsrdev (z_battlesplitbsr.c's
# and z_dimlight.c's), after every other input, the AAC test tenant included, so no production tenant moves; release
# builds compile none of it and emit none of these lines. CMakeLists.txt drops their late selectors from the dev
# build's late script. They are raw selectors, not KEEP: unused dev code is collected as it was in late text. They
# move whenever a production input grows (dev-only sections are never pinned). Not production inputs: these lists are
# not aacout_<kind>_inputs, which tools/analysis/late_order.py reads.
set(aacout_devonly_text
    "*/z_labguard.o(.text.* .homesave_code)"
    "*(.text_battletouchpen)"
    "*(.text_battlesplitpen)"
    "*(.text_bsrdev)"
)
set(aacout_devonly_rodata
    "*/z_labguard.o(.rodata .rodata.*)"
    "*(.rodata_battletouchpen)"
    "*(.rodata_battlesplitpen)"
    "*(.rodata_bsrdev_in_text)"
)
set(aacout_aot_manifest "${CMAKE_CURRENT_LIST_DIR}/../../tools/aot/aacslot.json")
file(READ "${aacout_aot_manifest}" aacout_aot_json)
string(JSON aacout_aot_count LENGTH "${aacout_aot_json}" functions)
if(aacout_aot_count GREATER 0)
    math(EXPR aacout_aot_last "${aacout_aot_count} - 1")
    foreach(index RANGE ${aacout_aot_last})
        string(JSON id GET "${aacout_aot_json}" functions ${index} id)
        if(NOT id MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
            message(FATAL_ERROR "Invalid AAC AOT id: ${id}")
        endif()
        string(JSON phase GET "${aacout_aot_json}" functions ${index} phase ${CTRPORT_TARGET})
        if(NOT phase MATCHES "^[0-9]+$" OR phase GREATER 31)
            message(FATAL_ERROR "Invalid AAC AOT phase: ${phase}")
        endif()
        set(aacout_aot_phase_.aacout.text.${id} "${phase}")
        set(aacout_aot_id_.aacout.text.${id} "${id}")
    endforeach()
endif()
# Native inputs use the same ordered emitter and optional 32 B phase pad.
# This table pins the real late-tail/AAC placement comparison; new native
# inputs may instead use their natural alignment and register only the lists.
set(aacout_native_manifest "${CMAKE_CURRENT_LIST_DIR}/aacslot-native.json")
file(READ "${aacout_native_manifest}" aacout_native_json)
string(JSON aacout_native_count LENGTH "${aacout_native_json}" tenants)
if(aacout_native_count GREATER 0)
    math(EXPR aacout_native_last "${aacout_native_count} - 1")
    foreach(index RANGE ${aacout_native_last})
        string(JSON id GET "${aacout_native_json}" tenants ${index} id)
        string(JSON input GET "${aacout_native_json}" tenants ${index} text input)
        string(JSON phase GET "${aacout_native_json}" tenants ${index} text phase32 ${CTRPORT_TARGET})
        string(JSON alignment GET "${aacout_native_json}" tenants ${index} text alignment_B)
        if(NOT id MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR
           NOT input MATCHES "^\\.aacout\\.text\\.[A-Za-z0-9_.]+$" OR
           NOT phase MATCHES "^[0-9]+$" OR phase GREATER 31 OR
           NOT alignment MATCHES "^(4|8|16|32)$")
            message(FATAL_ERROR "Invalid native AAC phase record: ${id}")
        endif()
        math(EXPR aacout_phase_remainder "${phase} % ${alignment}")
        if(NOT aacout_phase_remainder EQUAL 0)
            message(FATAL_ERROR "Native AAC phase ${phase} is incompatible with ${alignment} B input alignment: ${id}")
        endif()
        set(aacout_aot_phase_${input} "${phase}")
        set(aacout_aot_id_${input} "aacslot_native_${id}")
    endforeach()
endif()
set(aacout_toolchain_ld "${CTRPORT_DKA_BIN}/../arm-none-eabi/lib/3dsx.ld")
file(READ "${aacout_toolchain_ld}" aacout_ld)
foreach(kind text rodata)
    set(aacout_prefix "")
    if(kind STREQUAL "text")
        # GCC's startup object precedes the application objects in .text.*.
        set(aacout_prefix "        *crtbegin.o(.text.*)\n")
    endif()
    foreach(source IN LISTS PLATFORM_SOURCES ENGINE_SOURCES VENDOR_SOURCES)
        # CMake hashes source directories when an object path would be too long.
        # Only the filename survives; the pre-link check requires exactly one
        # matching linker input, including when two sources share a basename.
        get_filename_component(aacout_object "${source}" NAME)
        string(REGEX REPLACE "\\.[cCSs]$" ".o" aacout_object "${aacout_object}")
        if(CTRPORT_DEVONLY_AAC AND aacout_object STREQUAL "z_labguard.o")
            # Its sections are dev-only inputs (above), after every production tenant.
            continue()
        endif()
        if(kind STREQUAL "text")
            if(aacout_object STREQUAL "ctr_audio_decoder.o")
                # The retired shared wrapper has no caller; retain its padding.
                string(APPEND aacout_prefix "        /* aacout object */ KEEP(*/${aacout_object}(.text.*))\n")
            else()
                string(APPEND aacout_prefix "        /* aacout object */ */${aacout_object}(.text.*)\n")
            endif()
        else()
            string(APPEND aacout_prefix "        /* aacout object */ */${aacout_object}(.rodata)\n")
        endif()
    endforeach()
    if(kind STREQUAL "text")
        set(aacout_capacity 43396)
        if(CTRPORT_TARGET STREQUAL "o3ds")
            # Old 1.0 envelope (relready-resize.md, integrate21 dv8q): 36 KiB more, nine pages.
            set(aacout_capacity 80260)
        endif()
        set(aacout_match "*(.text.*)")
        set(aacout_flags "SHF_ALLOC & !SHF_WRITE")
    else()
        # 23,530 B vendor payload, plus its original 2 B internal alignment.
        set(aacout_capacity 23532)
        set(aacout_match "*(.rodata)")
        set(aacout_flags "SHF_ALLOC & !SHF_WRITE & !SHF_EXECINSTR")
    endif()
    set(aacout_inputs "")
    # Frame was withdrawn from UT 1.0. Keep its named holes so surviving
    # tenants retain the frozen offsets; future claims must fit in these holes.
    set(aacout_frame_hole "")
    if(".aacout.${kind}.battlesplit14" IN_LIST aacout_${kind}_inputs)
        if(kind STREQUAL "text")
            set(aacout_frame_bytes 9308)
        else()
            set(aacout_frame_bytes 748)
        endif()
        set(aacout_frame_hole "        __aacout_${kind}_retired_frame_start = ABSOLUTE(.);\n        __aacout_${kind}_dlgtop4_start = ABSOLUTE(.);\n        KEEP(INPUT_SECTION_FLAGS (${aacout_flags}) *(${aacout_${kind}_hole_inputs}))\n        __aacout_${kind}_dlgtop4_end = ABSOLUTE(.);\n        ASSERT(__aacout_${kind}_dlgtop4_end - __aacout_${kind}_dlgtop4_start <= 6300, \"S4 exceeds 6300 B budget\");\n        . += ${aacout_frame_bytes} - (__aacout_${kind}_dlgtop4_end - __aacout_${kind}_dlgtop4_start);\n        __aacout_${kind}_retired_frame_end = ABSOLUTE(.);\n")
    endif()
    # This contract must survive removal/renaming of the neighbour above.
    # Only isolated synthetic-emitter tests intentionally omit production holes.
    set(aacout_frame_contract "")
    if(NOT CTRPORT_AACOUT_FIXTURE_NO_FRAME_HOLES)
        if(kind STREQUAL "text")
            set(aacout_frame_size 9308)
        else()
            set(aacout_frame_size 748)
        endif()
        set(aacout_frame_contract "        ASSERT(DEFINED(__aacout_${kind}_retired_frame_start) && DEFINED(__aacout_${kind}_retired_frame_end), \"Retired Frame ${kind} hole missing\");\n        ASSERT(DEFINED(__aacout_${kind}_retired_frame_start) ? __aacout_${kind}_retired_frame_start == __aacout_${kind}_start : 0, \"Retired Frame ${kind} hole start changed\");\n        ASSERT((DEFINED(__aacout_${kind}_retired_frame_start) && DEFINED(__aacout_${kind}_retired_frame_end)) ? __aacout_${kind}_retired_frame_end - __aacout_${kind}_retired_frame_start == ${aacout_frame_size} : 0, \"Retired Frame ${kind} hole size changed\");\n")
    endif()
    set(aacout_unlisted ".aacout.${kind} .aacout.${kind}.*")
    if(kind STREQUAL "rodata")
        # Text was consumed above. Reject every remaining AAC name, including
        # writable .aacout.data/.bss and misspelled section families.
        set(aacout_unlisted ".aacout .aacout.*")
    endif()
    # ballast (build.sh ... ballast, test builds only): z_ballast.c's and ctr_renderer.c's .aacout.<kind>.ballast,
    # after every production input, so each keeps its release address in a ballast build. Not in the production
    # lists; without CTRPORT_BALLAST nothing is emitted and the script is the release one.
    set(aacout_ballast "")
    if(CTRPORT_BALLAST)
        set(aacout_ballast .aacout.${kind}.ballast)
    endif()
    set(aacout_order ${aacout_${kind}_inputs} ${aacout_ballast} .aacout.${kind}.aacslot_test)
    # battlesplit14 is the last release tenant (lead 11-09, dv8q), so the hole's neighbour is the first listed tenant:
    # assert that one sits right after the hole, whichever it is.
    set(aacout_first_tenant TRUE)
    foreach(input IN LISTS aacout_order)
        if(NOT input MATCHES "^\\.aacout\\.${kind}\\.[A-Za-z0-9_.]+$")
            message(FATAL_ERROR "AAC ${kind}: expected an exact .aacout.${kind}.<lane> input, got '${input}'")
        endif()
        if(aacout_first_tenant AND NOT aacout_frame_hole STREQUAL "")
            string(APPEND aacout_inputs "        __aacout_${kind}_first_tenant_start = ABSOLUTE(.);\n        ASSERT(__aacout_${kind}_first_tenant_start == __aacout_${kind}_start + ${aacout_frame_bytes}, \"Retired Frame ${kind} hole moved the first tenant\");\n")
        endif()
        set(aacout_first_tenant FALSE)
        if(CTRPORT_DEVONLY_AAC AND NOT input IN_LIST aacout_${kind}_inputs AND NOT DEFINED aacout_production_end_${kind})
            # Dev builds only: where the production lists end (before ballast and the test tenant), a release
            # build's used_end. check_devonly_aac.py checks it against the release link.
            set(aacout_production_end_${kind} TRUE)
            string(APPEND aacout_inputs "        __aacout_${kind}_production_end = ABSOLUTE(.);\n")
        endif()
        if(input STREQUAL ".aacout.${kind}.battlesplit14")
            string(APPEND aacout_inputs "        __aacout_${kind}_battlesplit14_start = ABSOLUTE(.);\n")
        endif()
        if(DEFINED aacout_aot_phase_${input})
            set(id "${aacout_aot_id_${input}}")
            set(phase "${aacout_aot_phase_${input}}")
            string(APPEND aacout_inputs "        __${id}_gap_start = ABSOLUTE(.);\n")
            string(APPEND aacout_inputs "        . += DEFINED(__${id}_present) ? ((${phase} - __${id}_gap_start) & 31) : 0;\n")
            string(APPEND aacout_inputs "        __${id}_gap_end = ABSOLUTE(.);\n")
        endif()
        string(APPEND aacout_inputs "        KEEP(INPUT_SECTION_FLAGS (${aacout_flags}) *(${input}))\n")
    endforeach()
    if(CTRPORT_DEVONLY_AAC)
        # The dev-only tail and its budgets (c26rev2's contracts, 11-11): the plain dev tail, alignment included,
        # at most 8,192 B of text; build.sh's labcrash (LG_CRASHTEST's ~0.9 KB more) at most 9,216 B, its own budget.
        # Old plain dev keeps at least 8,192 B of the text slot free, and Old's production lists leave the 16,384 B
        # protected reserve. Test vehicles (ballast, labcrash, the AAC test tenant) are held to the slot's capacity only.
        string(APPEND aacout_inputs "        __aacout_${kind}_devonly_start = ABSOLUTE(.);\n")
        foreach(selector IN LISTS aacout_devonly_${kind})
            string(APPEND aacout_inputs "        /* aacout devonly */ INPUT_SECTION_FLAGS (${aacout_flags}) ${selector}\n")
        endforeach()
        string(APPEND aacout_inputs "        __aacout_${kind}_devonly_end = ABSOLUTE(.);\n")
        if(kind STREQUAL "text")
            if(CTRPORT_LABCRASH)
                set(aacout_devonly_budget 9216)
                set(aacout_devonly_label "labcrash test vehicle")
            else()
                set(aacout_devonly_budget 8192)
                set(aacout_devonly_label "plain dev")
            endif()
            # The cap counts the dev-only tail from its own start: ballast and the AAC test tenant sit between
            # production_end and devonly_start and are held to the slot's capacity, not to this cap (c26rev2 DDUNIT-1).
            string(APPEND aacout_inputs "        ASSERT(__aacout_text_devonly_end - __aacout_text_devonly_start <= ${aacout_devonly_budget}, \"AAC text: ${aacout_devonly_label} dev-only tail over ${aacout_devonly_budget} B\");\n")
            if(CTRPORT_TARGET STREQUAL "o3ds")
                string(APPEND aacout_inputs "        ASSERT(__aacout_text_start + ${aacout_capacity} - __aacout_text_production_end >= 16384, \"AAC text: Old production lists inside the 16,384 B protected reserve\");\n")
                if(NOT CTRPORT_LABCRASH AND NOT CTRPORT_BALLAST AND NOT CTRPORT_AACSLOT_TEST)
                    string(APPEND aacout_inputs "        ASSERT(__aacout_text_start + ${aacout_capacity} - __aacout_text_devonly_end >= 8192, \"AAC text: Old plain dev under 8,192 B free\");\n")
                endif()
            endif()
        endif()
    endif()
    set(aacout_region "${aacout_prefix}
        . = ALIGN(4);
        __aacout_${kind}_start = ABSOLUTE(.);
${aacout_frame_hole}
${aacout_frame_contract}
${aacout_inputs}
        __aacout_${kind}_used_end = ABSOLUTE(.);
        KEEP(*(${aacout_unlisted}))
        ASSERT(ABSOLUTE(.) == __aacout_${kind}_used_end, \"Unlisted or invalid AAC ${kind} input\");
        ASSERT(__aacout_${kind}_used_end <= __aacout_${kind}_start + ${aacout_capacity}, \"AAC ${kind} slot overflow\");
        . += __aacout_${kind}_used_end <= __aacout_${kind}_start + ${aacout_capacity} ? __aacout_${kind}_start + ${aacout_capacity} - __aacout_${kind}_used_end : 0;
        __aacout_${kind}_end = ABSOLUTE(.);
        ${aacout_match}")
    string(FIND "${aacout_ld}" "${aacout_match}" aacout_found)
    if(aacout_found LESS 0)
        message(FATAL_ERROR "aacout: unsupported toolchain linker script; missing ${aacout_match}")
    endif()
    string(REPLACE "${aacout_match}" "${aacout_region}" aacout_ld "${aacout_ld}")
endforeach()
set(aacout_output "${CMAKE_CURRENT_BINARY_DIR}/aacout.ld")
file(WRITE "${aacout_output}.new" "${aacout_ld}")
configure_file("${aacout_output}.new" "${aacout_output}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${aacout_toolchain_ld}" "${CMAKE_CURRENT_LIST_FILE}" "${aacout_aot_manifest}" "${aacout_native_manifest}")
set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${aacout_output}")
string(REPLACE "-T 3dsx.ld%s" "-T aacout.ld" CTRPORT_SPECS_NEW "${CTRPORT_SPECS_NEW}")

# Remove AAC in every UT configuration. Select padding after Tremor discovery;
# only Release/RelWithDebInfo with Tremor have the reviewed decoder extents.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_decoder.c"
    APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_NO_AAC)
function(aacout_configure_decoder)
    if(aacout_aot_count GREATER 0 AND CTRPORT_AOT_SOURCES)
        set_property(SOURCE ${CTRPORT_AOT_SOURCES} APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/buildcache/aacslot_compile.py")
    endif()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" aacout_build_type)
    if(NOT aacout_build_type MATCHES "^(DEBUG|RELEASE|RELWITHDEBINFO|MINSIZEREL)$")
        message(FATAL_ERROR "aacout: unsupported build type '${CMAKE_BUILD_TYPE}'; expected Debug, Release, RelWithDebInfo or MinSizeRel")
    endif()
    set(aacout_mode "")
    if((aacout_build_type STREQUAL "RELEASE" OR aacout_build_type STREQUAL "RELWITHDEBINFO")
       AND CTRPORT_USE_TREMOR AND TREMOR_INCLUDE_DIR AND TREMOR_LIBRARY AND OGG_LIBRARY)
        set(aacout_mode "--aacout-pad")
    endif()
    get_target_property(aacout_delegate ctrport C_COMPILER_LAUNCHER)
    if(aacout_aot_count GREATER 0)
        set(aacout_delegate "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/buildcache/aacslot_compile.py;${aacout_delegate};--aacslot-compiler")
    endif()
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/buildcache/aacout_compile.py;${aacout_mode};${aacout_delegate};--aacout-compiler")
endfunction()
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_decoder.c"
    APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/aacout_compile.py")

# Check the actual linker argv/response files before invoking GCC. This launch
# property supports CMake 3.19; C_LINKER_LAUNCHER requires CMake 3.21.
set_property(TARGET ctrport PROPERTY RULE_LAUNCH_LINK
    "\"${CTRPORT_PYTHON_EXECUTABLE}\" -X utf8 -I \"${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/aacout_compile.py\" --aacout-link \"${aacout_output}\" --")
set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/aacout_compile.py")
