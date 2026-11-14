# the 3DS frontend.
#
# Builds the Butterscotch engine core from ../../engine/src together with the
# 3DS platform layer in source/. Configure with the devkitPro toolchain:
#
#   cmake -S platform/3ds -B build/3ds-undertale-n3ds \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
#         -DCTRPORT_GAME=undertale -DCTRPORT_TARGET=n3ds
#   cmake --build build/3ds-undertale-n3ds               # .elf + .3dsx
#   cmake --build build/3ds-undertale-n3ds --target cia  # .cia (needs makerom + bannertool)
#
# or just run platform/3ds/build.sh.
cmake_minimum_required(VERSION 3.19)
# The contract separates the public label, numeric project version and CIA allocation.
# ASM is needed for the embedded shader binary.
include("${CMAKE_CURRENT_SOURCE_DIR}/release_contract.cmake")
release_read_contract("${CMAKE_CURRENT_SOURCE_DIR}/../..")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/release/build-contract.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../${RELEASE_version_file}")
project(ctrport VERSION ${RELEASE_NUMERIC_VERSION} LANGUAGES C ASM)
# Keep the ordered object list in a response file: long checkout paths exceed
# the Windows process command-line limit even with short individual paths.
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
include("${CMAKE_CURRENT_SOURCE_DIR}/compiler_policy.cmake")
release_compiler_policy("${CMAKE_CURRENT_SOURCE_DIR}/../../docs/release/build-contract.json")

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CTRPORT_GAME "undertale" CACHE STRING "Game this build runs: undertale or deltarune")
set_property(CACHE CTRPORT_GAME PROPERTY STRINGS undertale deltarune)
option(CTRPORT_TEST_LATE_PAGE "Add a late-text page to UNDERTALE measurement builds only" OFF)

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")
get_filename_component(ENGINE_DIR "${ENGINE_DIR}" ABSOLUTE)

# Each game needs exactly one bytecode format, and building only that one lets
# the compiler drop every version check from the interpreter.
if(CTRPORT_GAME STREQUAL "undertale")
    set(GAME_WAD_DEFINE ENABLE_WAD16)
    set(GAME_TITLE "UNDERTALE")
    set(GAME_DIR_NAME "undertale")
elseif(CTRPORT_GAME STREQUAL "deltarune")
    set(GAME_WAD_DEFINE ENABLE_WAD17)
    set(GAME_TITLE "DELTARUNE")
    set(GAME_DIR_NAME "deltarune")
else()
    message(FATAL_ERROR "CTRPORT_GAME must be undertale or deltarune, not '${CTRPORT_GAME}'")
endif()

# One build per hardware class, so each gets its own .cia memory mode and, later, its own asset tier. The code can
# test CTRPORT_TARGET_N3DS / CTRPORT_TARGET_O3DS.
set(CTRPORT_TARGET "n3ds" CACHE STRING "Hardware this build is for: n3ds (New 3DS, New 2DS XL) or o3ds (Old 3DS, 3DS XL, 2DS)")
set_property(CACHE CTRPORT_TARGET PROPERTY STRINGS n3ds o3ds)
if(CTRPORT_TARGET STREQUAL "n3ds")
    set(TARGET_DEFINE CTRPORT_TARGET_N3DS)
    set(TARGET_LABEL "New 3DS")
elseif(CTRPORT_TARGET STREQUAL "o3ds")
    set(TARGET_DEFINE CTRPORT_TARGET_O3DS)
    set(TARGET_LABEL "Old 3DS/2DS")
else()
    message(FATAL_ERROR "CTRPORT_TARGET must be n3ds or o3ds, not '${CTRPORT_TARGET}'")
endif()

# .cia memory modes; trade-offs in cia/*.rsf and docs/building-cia.md. A .3dsx always gets whatever its launcher has
# (64 MB on Old 3DS, 124 MB on New 3DS).
set(CTRPORT_N3DS_MEMORY "124MB" CACHE STRING "New 3DS .cia memory: 124MB (no reboot) or 178MB (reboots on start and exit)")
set_property(CACHE CTRPORT_N3DS_MEMORY PROPERTY STRINGS 124MB 178MB)
set(CTRPORT_O3DS_MEMORY "80MB" CACHE STRING "DELTARUNE .cia Old 3DS memory: 80MB or 72MB (reboot on start and exit) or 64MB (no reboot); UNDERTALE's is always 64MB")
set_property(CACHE CTRPORT_O3DS_MEMORY PROPERTY STRINGS 80MB 72MB 64MB)
if(NOT CTRPORT_N3DS_MEMORY MATCHES "^(178MB|124MB)$")
    message(FATAL_ERROR "CTRPORT_N3DS_MEMORY must be 178MB or 124MB, not '${CTRPORT_N3DS_MEMORY}'")
endif()
if(NOT CTRPORT_O3DS_MEMORY MATCHES "^(80MB|72MB|64MB)$")
    message(FATAL_ERROR "CTRPORT_O3DS_MEMORY must be 80MB, 72MB or 64MB, not '${CTRPORT_O3DS_MEMORY}' (96MB crashes the HOME Menu)")
endif()
# UNDERTALE fits the Old 3DS's standard 64 MB, the same as its .3dsx (maintainer 10-25), so both its CIAs ignore the setting
# above (the n3ds CIA's Old 3DS mode, used only on an Old 3DS, matches), and a value cached in an old build directory
# cannot bring 80 MB back.
if(GAME_DIR_NAME STREQUAL "undertale")
    set(CIA_O3DS_MEMORY "64MB")
else()
    set(CIA_O3DS_MEMORY "${CTRPORT_O3DS_MEMORY}")
endif()

# .cia identity, one per game and target so all four install side by side. Title ID = 00040000 000<UniqueId>00.
# Keep in sync with the tables in cia/*.rsf and docs/building-cia.md.
#                              UniqueId  ProductCode  Title (8 chars max)
set(CIA_ID_undertale_n3ds      0xDE170   CTR-H-UT3N   ut3dsNew)
set(CIA_ID_undertale_o3ds      0xDE171   CTR-H-UT3O   ut3dsOld)
set(CIA_ID_deltarune_n3ds      0xDE172   CTR-H-DR3N   dr3dsNew)
set(CIA_ID_deltarune_o3ds      0xDE173   CTR-H-DR3O   dr3dsOld)
list(GET CIA_ID_${GAME_DIR_NAME}_${CTRPORT_TARGET} 0 CIA_UNIQUE_ID)
list(GET CIA_ID_${GAME_DIR_NAME}_${CTRPORT_TARGET} 1 CIA_PRODUCT_CODE)
list(GET CIA_ID_${GAME_DIR_NAME}_${CTRPORT_TARGET} 2 CIA_TITLE)

# The build ID (git's short commit, plus "+aot4" and the like for a build with generated GML) for the log header and
# the performance test results: build_id.cmake writes it into a generated header on every
# build, so it follows every commit and merge without a reconfigure. Only main.c includes it, to define the fixed-size
# array of source/ctr_build_id.h, and the header is only rewritten when the ID changes, so a new commit recompiles one
# file rather than every one.
find_package(Git QUIET)
set(CTRPORT_BUILD_ID_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CTRPORT_BUILD_ID_HEADER "${CTRPORT_BUILD_ID_DIR}/ctrport_build_id.h")
file(MAKE_DIRECTORY "${CTRPORT_BUILD_ID_DIR}")
release_package_names("${GAME_DIR_NAME}" "${CTRPORT_TARGET}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-stem.txt"
    CONTENT "${RELEASE_PACKAGE_STEM}\n" @ONLY)
set(CTRPORT_RELEASE_SOURCE_URL "" CACHE STRING "Owner-approved exact corresponding-source URL (empty: development repository)")
# The builder passes {repository}/tree/{public commit} (tools/release/release_contract.py validate_source); the build log
# keeps that link. The Legal page has 3 lines of 40 columns for source and build (ctr_legal.c luiDrawHub), too few for
# a 40-digit commit, so it shows the release tag, which release day binds to that commit
# (docs/release/build-contract.md).
set(RELEASE_SOURCE_REPOSITORY "${RELEASE_development_source_base}${GAME_DIR_NAME}-3ds")
if(CTRPORT_RELEASE_SOURCE_URL)
    string(REGEX REPLACE "/tree/[0-9a-f]+$" "" release_source_prefix "${CTRPORT_RELEASE_SOURCE_URL}")
    if(NOT release_source_prefix STREQUAL RELEASE_SOURCE_REPOSITORY
       OR NOT CTRPORT_RELEASE_SOURCE_URL MATCHES "/tree/[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$")
        message(FATAL_ERROR "CTRPORT_RELEASE_SOURCE_URL must be ${RELEASE_SOURCE_REPOSITORY}/tree/ and a 40-digit commit, not '${CTRPORT_RELEASE_SOURCE_URL}'")
    endif()
    message(STATUS "Corresponding source: ${CTRPORT_RELEASE_SOURCE_URL} (Legal page: tag v${RELEASE_VERSION})")
    set(RELEASE_SOURCE_HEADING "Source:")
    set(RELEASE_SOURCE_URL "${RELEASE_SOURCE_REPOSITORY}/tree/v${RELEASE_VERSION}")
    # luiBlock wraps after a '/': line 1 is the heading and the base, line 2 the rest of the link.
    string(LENGTH "${RELEASE_SOURCE_HEADING} ${RELEASE_development_source_base}" release_line1)
    string(LENGTH "${GAME_DIR_NAME}-3ds/tree/v${RELEASE_VERSION}" release_line2)
    if(release_line1 GREATER 40 OR release_line2 GREATER 40 OR NOT RELEASE_development_source_base MATCHES "/$")
        message(FATAL_ERROR "The Legal page's source link no longer fits two 40-column lines")
    endif()
else()
    set(RELEASE_SOURCE_HEADING "${RELEASE_VERSION} source")
    set(RELEASE_SOURCE_URL "${RELEASE_SOURCE_REPOSITORY}")
endif()
if(NOT RELEASE_SOURCE_URL MATCHES "^https://[A-Za-z0-9:/._?=#%+-]+$")
    message(FATAL_ERROR "CTRPORT_RELEASE_SOURCE_URL must be a printable HTTPS URL")
endif()
# The Legal page's copyright holder for the port (source/ctr_legal.c's summary): the name under "3DS port" in the
# Credits page's own text, docs/release/credits-port.txt, read as tools/aot/credits.py reads it. That file is the one
# place the builder kit's owner-name rule allows the name (tools/release/check_kit_hygiene.py), so it is written once.
set(release_credit_file "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/release/credits-port.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${release_credit_file}")
file(STRINGS "${release_credit_file}" release_credit_lines ENCODING UTF-8)
set(RELEASE_PORT_HOLDER "")
set(release_credit_next FALSE)
foreach(release_credit_line IN LISTS release_credit_lines)
    if(release_credit_next)
        if(release_credit_line MATCHES "^N(.+)$")
            string(STRIP "${CMAKE_MATCH_1}" RELEASE_PORT_HOLDER)
        endif()
        break()
    endif()
    if(release_credit_line STREQUAL "H3DS port")
        set(release_credit_next TRUE)
    endif()
endforeach()
if(NOT RELEASE_PORT_HOLDER MATCHES "^[A-Za-z0-9 ()&.,'-]+$")
    message(FATAL_ERROR "docs/release/credits-port.txt needs a plain name on the line after 'H3DS port' (the Legal page's copyright)")
endif()
file(CONFIGURE OUTPUT "${CTRPORT_BUILD_ID_DIR}/ctr_release_contract.h"
    CONTENT "#define CTR_RELEASE_BUILD_ID_SIZE ${RELEASE_build_id_storage_bytes}\n#define CTR_RELEASE_O3DS_FILENAME \"${RELEASE_O3DS_FILENAME}\"\n#define CTR_RELEASE_LEGAL_COLUMNS ${RELEASE_legal_columns}\n#define CTR_RELEASE_LEGAL_LINES ${RELEASE_legal_lines}\n#define CTR_RELEASE_SOURCE_URL \"${RELEASE_SOURCE_URL}\"\n#define CTR_RELEASE_SOURCE_HEADING \"${RELEASE_SOURCE_HEADING}\"\n#define CTR_RELEASE_LABEL \"${RELEASE_VERSION}\"\n#define CTR_RELEASE_PORT_HOLDER \"${RELEASE_PORT_HOLDER}\"\n" @ONLY)
add_custom_target(ctrport_build_id
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${CTRPORT_BUILD_ID_HEADER}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DAOT_DIR=${CTRPORT_AOT_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/build_id.cmake"
    BYPRODUCTS "${CTRPORT_BUILD_ID_HEADER}"
    VERBATIM)

# ===[ Engine core ]===
file(GLOB ENGINE_SOURCES "${ENGINE_DIR}/src/*.c")
# RNG contract correction: append both new objects; their entire contents are late.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_rngexact.c" "${ENGINE_DIR}/src/z_rngexact_vm.c")
if(CTRPORT_GAME STREQUAL "undertale")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_rngexact.c" "${ENGINE_DIR}/src/z_rngexact_vm.c")
else()
    # drgate: qualify these shared fixes separately when DELTARUNE work resumes.
    list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_room_name_bounds.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_rngexact.c" "${ENGINE_DIR}/src/z_rngexact_vm.c"
    PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# drhitbox is appended explicitly, on the proved DR path only.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_drhitbox.c")
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_colexact.c")
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_hitcheck.c")
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utlife.c")
# loop.c is the desktop main loop; source/main.c replaces it.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/loop.c")
# Allocation helpers are cold; link them last to keep new code out of the
# renderer/VM prefix. Ordinary literal-array TPAG readers gain no calls.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/data_win_slots_3ds.c")
list(APPEND ENGINE_SOURCES
    "${ENGINE_DIR}/src/image/image_decoder.c"
    "${ENGINE_DIR}/src/physics/physics.c"
    "${ENGINE_DIR}/src/physics/disabled/physics_disabled.c"
    # For regression runs (statehash.txt in the game folder, see main.c).
    "${ENGINE_DIR}/src/cli/state_hash.c"
)
# A reach-gate build (docs/perf/reach-gate.md; build.sh ... reachgate): never a Release build. The words in statehash.txt
# configure it.
option(CTRPORT_REACH_GATE "Compile in the reach-mode identity gate (testing)" OFF)
if(CTRPORT_REACH_GATE)
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/cli/reach_gate.c")
endif()
# Append the null guard to the existing .heapfrag section after the checked stb
# allocators. A new output section changes BFD section IDs and every AOT veneer
# hash; growing .fatalflush would move the later video and heapfrag functions.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/rvalue_numeric_string.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/rvalue_numeric_string.c")
# Late code only (colflip): last, like the late sections it fills.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/collision_compat.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/collision_compat.c")
# UNDERTALE's collision rules (colflip): late code only, after collision_compat.c. DELTARUNE (bytecode 17 only) never
# registers them: it compiles collision_gm14_dr.c in their place (freebytes G3, dec-1031-drnew-page; late_reserve.ld's
# DELTARUNE lines below keep the later late code three pages lower and the late constants and statics in place).
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/collision_gm14.c")
if(CTRPORT_GAME STREQUAL "deltarune")
    list(APPEND ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/collision_gm14_dr.c")
else()
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/collision_gm14.c")
endif()
# Typed-only units follow the original engine objects, preserving shared weak helper selection.
set(TYPED_ENGINE_SOURCES "${ENGINE_DIR}/src/vm_aot_inline.c" "${ENGINE_DIR}/src/vm_aot_typed_runtime.c")
list(REMOVE_ITEM ENGINE_SOURCES ${TYPED_ENGINE_SOURCES})
list(APPEND ENGINE_SOURCES ${TYPED_ENGINE_SOURCES})
# UNDERTALE's alarm setter (colflip, alarm14): late code only, after collision_gm14.c and typed3's units.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/alarm_gm14.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/alarm_gm14.c")
# UNDERTALE's line, circle, distance and colour builtins (colflip, follow-up check): late code only, after alarm_gm14.c.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/colline_gm14.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/colline_gm14.c")
# UNDERTALE's console prompts (btnprompt): late code only, last (after colline_gm14.c).
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_btnprompt.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_btnprompt.c")
# Escape elision's materializer (drold, G9): late code only, last (after z_btnprompt.c).
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/vm_aot_escape.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/vm_aot_escape.c")
# Its strings unmerged (-fno-merge-constants): late_reserve.ld places them after its code, where a mergeable pool would
# join z_btnprompt.c's and move what follows it.
set_source_files_properties("${ENGINE_DIR}/src/vm_aot_escape.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# UNDERTALE's fire-attack fast paths (utflames): late code only, last (after vm_aot_escape.c); empty for DELTARUNE.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflames.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflames.c")
# UNDERTALE's Photoshop Flowey pipe Draws (utflowey): late code only, last (after z_utflames.c); empty for
# DELTARUNE. Each double operation rounds once, in bytecode order, as in aot_typed.c: no fast math, no contraction.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflowey.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflowey.c")
set_source_files_properties("${ENGINE_DIR}/src/z_utflowey.c" PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
# UNDERTALE's heat shimmer (utflames): late code only, last (after z_utflames.c); empty for DELTARUNE. Its strings
# unmerged, as vm_aot_escape.c's.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflames_waver.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflames_waver.c")
set_source_files_properties("${ENGINE_DIR}/src/z_utflames_waver.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# Swarms of short-lived instances (utdust): late code only, last; UNDERTALE only (Policy E: its hooks in runner.c
# are under ENABLE_WAD16).
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utdust.c")
if(CTRPORT_GAME STREQUAL "undertale")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utdust.c")
endif()
# UNDERTALE's falling-water foreground: late code only, last (after z_utflames_waver.c); empty for
# DELTARUNE. Its strings unmerged.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflames_wfall.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utflames_wfall.c")
set_source_files_properties("${ENGINE_DIR}/src/z_utflames_wfall.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# UNDERTALE's draw_self_border_true (Asgore's sided fire): a native body bound by gmlaot.py's UTF_NATIVE. Late code
# only (late_reserve.ld: last); empty for DELTARUNE. Its constants unmerged, so they stay with its code.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_asgborder.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_asgborder.c")
set_source_files_properties("${ENGINE_DIR}/src/z_asgborder.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# The runner's event runs skipping events whose code starts with a false test of a global (z_evguard.c, vm.h's
# VM_BEGIN_EVENT_RUN). Late code only (late_reserve.ld: after itemfront, merge order); empty for DELTARUNE. Its constants unmerged.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_evguard.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_evguard.c")
set_source_files_properties("${ENGINE_DIR}/src/z_evguard.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# UNDERTALE's monster dust: native bodies for the dust maker's Draw and the dust pixel's Create, bound by
# gmlaot.py's UTF_NATIVE. Late code only, last; empty for DELTARUNE. Its strings unmerged (-fno-merge-constants), so they
# stay with its code in the late text.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utkill_vapor.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utkill_vapor.c")
set_source_files_properties("${ENGINE_DIR}/src/z_utkill_vapor.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# UNDERTALE's monster dust as a swarm: late code only, last (after z_utkill_vapor.c); UNDERTALE only.
# Its strings unmerged (-fno-merge-constants), so they stay with its code in the late text.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utkill_swarm.c")
if(CTRPORT_GAME STREQUAL "undertale")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utkill_swarm.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_utkill_swarm.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# UNDERTALE's waving sprite rows: late code only, last (after z_utkill_swarm.c); UNDERTALE only. Its
# strings unmerged (-fno-merge-constants), so they stay with its code in the late text.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utkill_waver.c")
if(CTRPORT_GAME STREQUAL "undertale")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utkill_waver.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_utkill_waver.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")
# DELTARUNE Chapter 3's board grid rebuild (boardgrid, change simplefix), bound by gmlaot.py's BOARD_NATIVE: late code
# only, last; DELTARUNE New only (gmlaot.py binds it only for --target n3ds).
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_boardgrid.c")
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_boardgrid.c")
endif()
# UNDERTALE's filled-rectangle script (utrect, change uthunt): a builtin of the script's name, registered by the wrap of
# Utgap_registerAll (VM_create's last registration; only that call's target changes). Late code only, last;
# UNDERTALE only. Its strings unmerged (-fno-merge-constants), so they stay with its code in the late text; -Os, as
# the UNDERTALE 1.0 late reserve is tight.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utrect.c")
if(CTRPORT_GAME STREQUAL "undertale")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utrect.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_utrect.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
# Omega Flowey's vine burst (utburst, change flowend r37): the vine Create's native body and the master's held sound
# calls, bound by gmlaot.py's UTF_NATIVE. Late code only, last; UNDERTALE only. Its constants unmerged, so they stay
# with its code in the late text; -Os, as the UNDERTALE late reserve is tight.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utburst.c")
if(CTRPORT_GAME STREQUAL "undertale")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utburst.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_utburst.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
# Omega Flowey's ring attack (utring, change flowend r39): the ring boss's Create's native body, bound by gmlaot.py's
# UTF_NATIVE (for --target o3ds only). Late code only, last; UNDERTALE Old only (UT New's release late text has no
# room for it, c26rev r94); its constants unmerged and -Os, as utburst.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_utring.c")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_utring.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_utring.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
# The first draw of a bone wall (sansspawn, 11-06): draw_self_border_true's first call on an instance, native too.
# Bound by gmlaot.py's UTF_NATIVE_FIRST (for --target o3ds only). Late code only, last; UNDERTALE Old only.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_sansspawn.c")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_sansspawn.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_sansspawn.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
# The snow floor's Draw at rest (poffroom, 11-06): native, bound by gmlaot.py's UTF_NATIVE (--target o3ds only). Late code
# only, last; UNDERTALE Old only.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_snowfloor.c")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_snowfloor.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_snowfloor.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
# The snow floor's Draw with kicked balls (snowkick, 11-08): native, bound by gmlaot.py's UTF_NATIVE_NEXT behind
# z_snowfloor.c's body (--target o3ds only). Late code only, last; UNDERTALE Old only.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_snowkick.c")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_snowkick.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_snowkick.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
# Automatic depth layers' element lists as a deque (depthmove): late code only, last (after z_boardgrid.c);
# DELTARUNE only (its macros in runner.c are the original code without CTRPORT_DEPTHMOVE). Its strings unmerged.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_depthmove.c")
if(CTRPORT_GAME STREQUAL "deltarune")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_depthmove.c")
endif()
set_source_files_properties("${ENGINE_DIR}/src/z_depthmove.c" PROPERTIES COMPILE_OPTIONS "-fno-merge-constants")

set(VENDOR_SOURCES
    "${ENGINE_DIR}/vendor/md5/md5.c"
    "${ENGINE_DIR}/vendor/base64/base64.c"
    "${ENGINE_DIR}/vendor/sha1/sha1.c"
    "${ENGINE_DIR}/vendor/miniz/miniz.c"
    "${ENGINE_DIR}/vendor/miniz/miniz_tdef.c"
    "${ENGINE_DIR}/vendor/miniz/miniz_tinfl.c"
    "${ENGINE_DIR}/vendor/bzip2/bz_internal_error.c"
    "${ENGINE_DIR}/vendor/bzip2/bzlib.c"
    "${ENGINE_DIR}/vendor/bzip2/crctable.c"
    "${ENGINE_DIR}/vendor/bzip2/decompress.c"
    "${ENGINE_DIR}/vendor/bzip2/huffman.c"
    "${ENGINE_DIR}/vendor/bzip2/randtable.c"
)
set_source_files_properties(${VENDOR_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# PacketVideo's AAC-LC decoder, for DELTARUNE's video audio (vendor/pvaac/README.md). Its C++ sources
# are kept as C; they only need bool from C++.
if(CTRPORT_GAME STREQUAL "deltarune")
    file(GLOB PVAAC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pvaac/src/*.c")
    set_source_files_properties(${PVAAC_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-include;stdbool.h"
        COMPILE_DEFINITIONS "OSCL_IMPORT_REF=;OSCL_EXPORT_REF=;OSCL_UNUSED_ARG=")
    list(APPEND VENDOR_SOURCES ${PVAAC_SOURCES})
endif()

# ===[ 3DS platform layer ]===
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.c")
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bootline_console.c") # Added explicitly for UT below.
# Frame was withdrawn from 1.0 by the owner; keep its recoverable sources out of every link.
list(FILTER PLATFORM_SOURCES EXCLUDE REGEX "/z_bottomframe(_core|_assets|_settings)?\\.c$")
if(NOT CTRPORT_GAME STREQUAL "undertale" OR NOT CTRPORT_TARGET STREQUAL "o3ds")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_boxcache.c")
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_aac_debug_bits.c")
endif()
if(NOT CTRPORT_GAME STREQUAL "undertale")
    list(REMOVE_ITEM PLATFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosabl.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_trace_wake.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_thinrule.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dwkeep.c")
endif()
if(NOT (CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds"))
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosalow.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_vramtop.c") # gpuburst F1: UNDERTALE Old only
endif()
if(CTRPORT_GAME STREQUAL "deltarune")
    # UNDERTALE Old's asset tables are absent from the curated DELTARUNE kit.
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utold_tile_assets.c")
endif()
if(NOT CTRPORT_GAME STREQUAL "undertale")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_keeplit.c") # keep-lit: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dimlight.c") # dimlight: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_oflog.c") # oflog: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_stackfloor.c") # stackfloor: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_scanout.c") # scanout: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_menutime.c") # menutime: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_heldslow.c") # heldslow: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_layout.c") # layout: UNDERTALE only
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bootahead.c") # bootahead: UNDERTALE only
endif()
# GPU-1 is compiled only by the New DR switch below.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c")
# collision_gm14_dr.c goes in collision_gm14.c's place in the engine list (DELTARUNE only, above).
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/collision_gm14_dr.c")
if(NOT CTRPORT_GAME STREQUAL "undertale" OR NOT CTRPORT_GOG_COMPAT)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_gogut.c")
endif()
# sdlat is appended only to profile builds below; ordinary Release has no object.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_sdlat.c")
# pacealign (change auditfixe, Option A, 2026-11-05; UNDERTALE only): a bench window's edges log the pacer's
# numbers (source/z_pacealign.c, all late code). Three calls in ctr_benchmark.c go there (ctr_benchmark.h's
# BENCH_PACE_* names), ctr_benchmark.c gains an accessor at its end, and ctr_frame_pacer.c's end cuts the pacer's
# numbers at the edge's frame end (CtrPacer_frameBegun and CtrPacer_frameEnded wrapped, below the executable).
# DELTARUNE has neither the object, the definition nor the wraps, so its code is unchanged.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_frame_pacer.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_pacealign.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_PACEALIGN)
else()
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_pacealign.c")
endif()
# UT releases do not enable the deferred DR chapter-selection runtime (Policy E).
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_chapters.c")
if(NOT (CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds"))
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_new.c")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_new.c" PROPERTIES COMPILE_OPTIONS "-Os")
if(NOT (CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds"))
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_old.c")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_old.c" PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_receipt.c" PROPERTIES COMPILE_OPTIONS "-Os")
if(NOT CTRPORT_GAME STREQUAL "undertale")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_review.c")
endif()
if(NOT CTRPORT_GAME STREQUAL "undertale")
    # Policy E: inputlate is UNDERTALE-only; do not even compile its objects for DR.
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_input.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c")
endif()
if(NOT CTRPORT_GAME STREQUAL "undertale")
    # battlesplit (the split battle layout) is UNDERTALE-only, like inputlate, whose beginFrame link calls it.
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplitbsr.c")
endif()
# The spike recorder (change spikerec, build.sh ... spikerec): playtest builds only. Without the word its file is not
# even compiled, so release builds link exactly the objects they did before (its wraps and defines are further down).
option(CTRPORT_SPIKEREC "Spike recorder: frames that miss their slot, written to spikes.txt at room changes (playtests)" OFF)
if(NOT CTRPORT_SPIKEREC)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_spikerec.c")
endif()
# labguard (source/z_labguard.c says why; change labguard, 11-08): the lab's startup guard and bounded leave, UNDERTALE
# builds without CTRPORT_RELEASE_SOURCE_URL only. Every public and builder build has that URL: they compile none of it,
# and their objects, link and bytes are what they were (its defines and wraps are further down).
option(CTRPORT_LABCRASH "Test only: labguard's fault injection (--labguard-crash, --lab-crash stall-<step>)" OFF)
if(CTRPORT_GAME STREQUAL "undertale" AND NOT CTRPORT_RELEASE_SOURCE_URL)
    set(CTRPORT_LABGUARD_ON ON)
else()
    set(CTRPORT_LABGUARD_ON OFF)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_labguard.c")
    if(CTRPORT_LABCRASH)
        message(FATAL_ERROR "labcrash: UNDERTALE test builds only (no CTRPORT_RELEASE_SOURCE_URL)")
    endif()
endif()
# Development-only code in the AAC slots (lead 11-11, devdiet decision 2): the same builds (UNDERTALE, no release
# source URL) move labguard (z_labguard.o), the pen log's own pairs and bsrdev out of late text, into dev-only AAC
# inputs after every other input (aacout.cmake), and late_reserve.ld's selectors for them are dropped from this
# build's copy (below). Release builds compile none of it, and their scripts are unchanged.
set(CTRPORT_DEVONLY_AAC ${CTRPORT_LABGUARD_ON})
# flowdiag (change flowstream round 3, build.sh ... flowdiag): every draw-time tile load and floor guard decision with
# free linear and heap memory and plan membership, lab builds only. Without the word its file is not even compiled and
# its define reaches no object (its wraps are further down).
option(CTRPORT_FLOWSTREAM_DIAG "LAB ONLY: draw-time tile loads and floor guard decisions with memory and plan membership; never release" OFF)
if(NOT CTRPORT_FLOWSTREAM_DIAG)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowdiag.c")
elseif(NOT CTRPORT_GAME STREQUAL "undertale")
    message(FATAL_ERROR "flowdiag is UNDERTALE only")
elseif(CTRPORT_RELEASE_SOURCE_URL)
    message(FATAL_ERROR "flowdiag must never be packaged as a release")
endif()
# frametrace (change benchfix, build.sh ... frametrace): a bench window's frames and SD, decode and tile events, test
# builds only. Without the word its file is not even compiled (its wraps and defines are further down).
option(CTRPORT_FRAMETRACE "Frame trace: every frame of a bench window and its SD, decode and tile events, to frametrace.txt (tests)" OFF)
if(NOT CTRPORT_FRAMETRACE)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_frametrace.c")
endif()
# change freewins (build.sh ... freewins): main-loop segment timers and off switches, diagnostic builds only. Without the
# word its file is not compiled and its define reaches no object, so release builds are unchanged.
option(CTRPORT_FREEWINS "Main-loop segment timers and off switches (lane freewins diagnostics)" OFF)
if(NOT CTRPORT_FREEWINS)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_freewins.c")
endif()
# The Titan CPU attribution recorder (change titancpu, build.sh ... titandiag): diagnosis builds only, never a release.
# Without the word its file is not compiled (its wraps are further down).
option(CTRPORT_TITANDIAG "Titan CPU recorder: slow bench frames split by phase and by event, as [td] log lines (diagnosis)" OFF)
if(NOT CTRPORT_TITANDIAG)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_titandiag.c")
endif()
# The exit trail (change credfix, build.sh ... exittrail): o2DS kernel panic diagnosis builds only, never a release.
# Without the word its file is not compiled (its wraps are further down).
option(CTRPORT_EXITTRAIL "Exit trail: the exit's steps after the last log line, in labboot.txt (diagnosis)" OFF)
if(NOT CTRPORT_EXITTRAIL)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_exittrail.c")
endif()
# labcrash's fault injection is left out with the exit trail (LG_CRASHTEST below), so asking for both would build a
# vehicle without its injection: refuse it, as build.sh does (c26rev2 DDUNIT-2).
if(CTRPORT_LABCRASH AND CTRPORT_EXITTRAIL)
    message(FATAL_ERROR "labcrash: not with exittrail (its fault injection would be left out)")
endif()
# ballast (dsplan B10.2, build.sh ... ballast): an UNDERTALE test build holding the second-screen features' reserved
# heap, linear, CPU and GPU budget (source/z_ballast.c). Test builds only, never a release: refused with a release
# source URL. Without the word its file is not compiled and nothing is retargeted (the retargets are further down).
option(CTRPORT_BALLAST "Ballast: --ini ballast_* keys hold heap, linear, CPU and GPU budget (test builds only)" OFF)
if(NOT CTRPORT_BALLAST)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_ballast.c")
elseif(CTRPORT_RELEASE_SOURCE_URL OR NOT CTRPORT_GAME STREQUAL "undertale")
    message(FATAL_ERROR "CTRPORT_BALLAST is an UNDERTALE test build and never a release (no CTRPORT_RELEASE_SOURCE_URL)")
endif()
# dodgefront (change credfix, charter rev 1 item 2, option 3): UNDERTALE Old 3DS only. The centred text line's hand front
# (source/z_dodgefront.c), bound by vm.c's late VM_aotBindFront through CtrRenderer_prefetchRoom's wrap chain
# (z_roomplan.c, ctr_wrap_chain.h). Late text only; the definition goes on the files that read it (ctr_audio_system.c:
# flowhitch's link, after roomplan's, calls textfront's).
# build.sh passes this cache setting for every target; only UT Old consumes its behavior below.
set(CTRPORT_WRITERFRONT_MODE "ON" CACHE STRING "Writer front: ON, OFF, FALLBACK or COUNT (UNDERTALE Old)")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set_property(SOURCE "${ENGINE_DIR}/src/vm.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomplan.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dodgefront.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DODGEFRONT)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dodgefront.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
    # textfront (change simplefix): the centred-text scripts' fronts with the view cull (source/z_textfront.c), the
    # prefetchRoom chain's link ahead of dodgefront's (ctr_wrap_chain.h). Late text only.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DODGEFRONT)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
    # textcase (change codeclose, lever 1): the dialogue script's switch jumps straight to its case from the builder's
    # table (source/z_textcase.c, romfs:/textcase.bin from tools/aot/textcase.py), the prefetchRoom chain's link before
    # dodgefront's (ctr_wrap_chain.h). Late text only.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textcase.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DODGEFRONT)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textcase.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
    # writerfront (change freebies): the dialogue writer's Draw as hand code with the generated function as its fallback
    # (source/z_writerfront.c), the prefetchRoom chain's link between textfront's and dodgefront's (ctr_wrap_chain.h)
    # and vm.c's late VM_aotSwapFront. Late text only. build.sh's nowriterfront / writerfront-fallback /
    # writerfront-count words (test builds) leave it out, send every Draw to the fallback, or count.
    if(CTRPORT_WRITERFRONT_MODE STREQUAL "OFF")
        list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c")
        list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_trailfront.c")
        list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_itemfront.c")
        list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfirst.c")
        list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bulletsplit.c")
    else()
        # trailfront (change asrfight): the SAVE Asriel rainbow trail's Draw as hand code, the interpreter its fallback
        # (source/z_trailfront.c), the prefetchRoom chain's link between writerfront's and dodgefront's, bound by
        # vm.c's late VM_aotSwapFront (so writerfront builds only). Late text only.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_trailfront.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_DODGEFRONT CTRPORT_WRITERFRONT CTRPORT_TRAILFRONT)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_trailfront.c" APPEND PROPERTY COMPILE_OPTIONS
            "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
        # itemfront (change asrfight): two item-name scripts as hand code, the interpreter their fallback
        # (source/z_itemfront.c), the prefetchRoom chain's link between trailfront's and dodgefront's, bound by vm.c's
        # late VM_aotBindFront. Late text only, after fpsbox.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_itemfront.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_DODGEFRONT CTRPORT_WRITERFRONT CTRPORT_TRAILFRONT)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_itemfront.c" APPEND PROPERTY COMPILE_OPTIONS
            "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
        # writerfirst (change codeclose, lever 2): wraps writerfront's front so a writer's first Draw takes it too
        # (source/z_writerfirst.c), the prefetchRoom chain's link between itemfront's and textcase's, with vm.c's late
        # VM_aotBoundFn. Late text only.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfirst.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_DODGEFRONT CTRPORT_WRITERFRONT)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfirst.c" APPEND PROPERTY COMPILE_OPTIONS
            "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
        # bulletsplit (change creddodge): the credits bullets' text drawn after their run (source/z_bulletsplit.c), the
        # prefetchRoom chain's link between writerfirst's and textcase's. Late text only.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bulletsplit.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_DODGEFRONT CTRPORT_WRITERFRONT)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bulletsplit.c" APPEND PROPERTY COMPILE_OPTIONS
            "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
        # utburst (change flowend r37b): the vine Create's stop / play scripts' check made at build time (gmlaot.py's
        # UTF_NATIVE "bound:" operands), proven at run time through vm.c's late VM_aotBoundFn (engine/src/z_utburst.c).
        set_property(SOURCE "${ENGINE_DIR}/src/z_utburst.c" APPEND PROPERTY COMPILE_DEFINITIONS UTBURST_BOUND)
        # utring (change flowend r39): the ring parts made without their Create's dispatch, when the part's Create is
        # the code gmlaot.py checked (UTF_NATIVE "objfn:"), proven at run time through VM_aotBoundFn.
        set_property(SOURCE "${ENGINE_DIR}/src/z_utring.c" APPEND PROPERTY COMPILE_DEFINITIONS UTRING_BOUND)
        set_property(SOURCE "${ENGINE_DIR}/src/vm.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_WRITERFRONT)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DODGEFRONT)
        if(CTRPORT_WRITERFRONT_MODE STREQUAL "FALLBACK")
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_WRITERFRONT_FALLBACK)
        elseif(CTRPORT_WRITERFRONT_MODE STREQUAL "COUNT")
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_WRITERFRONT_COUNT)
        endif()
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c" APPEND PROPERTY COMPILE_OPTIONS
            "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
        # letterdraw glyph-pair memo (change letterdraw, 11-11): stCarry's glyph metrics kept in one heap block
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_smalltext.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_LDMEMO)
        # letterdraw plan variants (change letterdraw L1, 11-11): bcPlanFor keeps a writer's plan per first-glyph offset
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_smalltext.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_LDPLAN)
    endif()
else()
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dodgefront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_trailfront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_itemfront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textcase.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfirst.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bulletsplit.c")
endif()
# utorder (change credfix): UNDERTALE only; its wraps and options are further down.
if(NOT CTRPORT_GAME STREQUAL "undertale")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utorder.c")
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    # Introduce the new RomFS dependency only after the original libctru inputs have been extracted. Otherwise
    # the driver's references to utf.o reorder existing library code even though the driver itself is late.
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_romfs.c")
    add_library(benchromfs_mount STATIC "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_romfs.c")
    target_compile_definitions(benchromfs_mount PRIVATE $<TARGET_PROPERTY:ctrport,COMPILE_DEFINITIONS>)
    target_include_directories(benchromfs_mount PRIVATE $<TARGET_PROPERTY:ctrport,INCLUDE_DIRECTORIES>)
    target_compile_options(benchromfs_mount PRIVATE $<TARGET_PROPERTY:ctrport,COMPILE_OPTIONS>)
endif()
# swkbd (UNDERTALE only, Policy E): Mettaton's essay opens the software keyboard (source/z_swkbd.c). Like the RomFS
# mount above it is a library linked after libctru's original members, so the keyboard applet's members join the link
# last and every library section that was already there keeps its place (its new sections are late: late_reserve.ld).
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_swkbd.c")
# The phase plan (source/z_phaseplan.c) is DELTARUNE only (review decision, 2026-10-25: on UNDERTALE it gained about
# nothing for 4.9 KB of late code); UNDERTALE doesn't build it.
if(CTRPORT_GAME STREQUAL "undertale")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_phaseplan.c")
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    add_library(swkbd_late STATIC "${CMAKE_CURRENT_SOURCE_DIR}/source/z_swkbd.c")
    target_compile_definitions(swkbd_late PRIVATE $<TARGET_PROPERTY:ctrport,COMPILE_DEFINITIONS>)
    target_include_directories(swkbd_late PRIVATE $<TARGET_PROPERTY:ctrport,INCLUDE_DIRECTORIES>)
    target_compile_options(swkbd_late PRIVATE $<TARGET_PROPERTY:ctrport,COMPILE_OPTIONS>
        -Os -fno-builtin -fno-tree-switch-conversion -fno-jump-tables)
endif()
# Keep ctr_memory_lock.c next to ctr_memory.c in sorted source order.
# Its code is in the late reserve; its two TLS bytes fit the alignment
# gap before the next TLS object. Full ELF layout is gated.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c")
list(APPEND PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c")
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_benchfid_window.c")
list(APPEND PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_benchfid_window.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_benchfid_window.c"
    PROPERTIES COMPILE_OPTIONS "-fno-builtin;-fno-jump-tables;-fno-tree-switch-conversion")


# The renderer's vertex shaders (the plain one, the one for batches that
# sample several textures, the one for palette shaders' texture pairs, and
# the one for textures sampled by target position), assembled with picasso
# and linked in as data (render2d_shader_shbin.h).
ctr_add_shader_library(render2d_shader "${CMAKE_CURRENT_SOURCE_DIR}/source/shaders/render2d.v.pica"
                       "${CMAKE_CURRENT_SOURCE_DIR}/source/shaders/render2d_multi.v.pica"
                       "${CMAKE_CURRENT_SOURCE_DIR}/source/shaders/render2d_pair.v.pica"
                       "${CMAKE_CURRENT_SOURCE_DIR}/source/shaders/render2d_texgen.v.pica")
dkp_add_embedded_binary_library(render2d_shader_data render2d_shader)

# The performance test's scenes for this game (bench/<game>.txt), then the battle sweep's battles
# (bench/sweep_<game>.txt, which may include the scenes), packaged in the romfs in the same byte order.
set(BENCH_SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/bench/${GAME_DIR_NAME}.txt" "${CMAKE_CURRENT_SOURCE_DIR}/bench/sweep_${GAME_DIR_NAME}.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BENCH_SCENE_SOURCES})
set(BENCH_SCENE_TEXT "")
foreach(source ${BENCH_SCENE_SOURCES})
    if(EXISTS "${source}")
        file(READ "${source}" text)
        string(APPEND BENCH_SCENE_TEXT "${text}\n")
    endif()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bench_scenes.txt.new" "${BENCH_SCENE_TEXT}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/bench_scenes.txt.new" "${CMAKE_CURRENT_BINARY_DIR}/bench_scenes.txt" COPYONLY)
set(CTRPORT_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${CTRPORT_ROMFS_DIR}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/bench_scenes.txt" "${CTRPORT_ROMFS_DIR}/bench_scenes.txt" COPYONLY)
# Frozen main 65726574 ELF sizes, never derived from the editable bench text. Keep this object in the old link
# order so every later rodata/data/bss address stays put. Freeing this RAM is a separate, measured layout step.
if(CTRPORT_GAME STREQUAL "deltarune")
    set(BENCH_RETAINED_SIZE 82654)
else()
    set(BENCH_RETAINED_SIZE 20854)
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/bench_placeholder.S.in"
               "${CMAKE_CURRENT_BINARY_DIR}/bench_scenes_txt.S" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/bench_placeholder.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/bench_scenes_txt.h" @ONLY)
add_library(bench_scenes_data OBJECT "${CMAKE_CURRENT_BINARY_DIR}/bench_scenes_txt.S")

# Coverage additions are an optional RomFS file next to bench_scenes.txt, never extra resident
# scene constants; the loader keeps the primary list's IDs.
set(COVERAGE_SCENE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/bench/coverage_${GAME_DIR_NAME}.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COVERAGE_SCENE_SOURCE}")
if(EXISTS "${COVERAGE_SCENE_SOURCE}")
    configure_file("${COVERAGE_SCENE_SOURCE}" "${CTRPORT_ROMFS_DIR}/bench-supplement.txt" COPYONLY)
else()
    file(WRITE "${CTRPORT_ROMFS_DIR}/bench-supplement.txt" "")
endif()
set(COVERAGE_ROMFS_FILE "${CTRPORT_ROMFS_DIR}/bench-supplement.txt")
# UNDERTALE's console prompts (btnprompt): the 3DS button glyphs, our own art (assets/btnprompt_glyphs.txt).
set(BTNPROMPT_ROMFS_FILES "")
if(CTRPORT_GAME STREQUAL "undertale")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/btnprompt_glyphs.txt" "${CTRPORT_ROMFS_DIR}/btnprompt_glyphs.txt" COPYONLY)
    set(BTNPROMPT_ROMFS_FILES "${CTRPORT_ROMFS_DIR}/btnprompt_glyphs.txt")
endif()
# The loading screens' art (loadscreen and packscreen, source/z_loadscreen.c): both games' first time setup line,
# and DELTARUNE's loading text and Chapter 3's dog, cut from the player's own files when the game is built (build.sh
# runs tools/aot/loadscreen_art.py); never in the repository. Without the file the loading screen is a bar alone.
set(CTRPORT_LOADSCREEN_ART "" CACHE FILEPATH "loadscreen.bin for the RomFS (build.sh makes it)")
set(LOADSCREEN_ROMFS_FILES "")
if(CTRPORT_LOADSCREEN_ART AND EXISTS "${CTRPORT_LOADSCREEN_ART}")
    configure_file("${CTRPORT_LOADSCREEN_ART}" "${CTRPORT_ROMFS_DIR}/loadscreen.bin" COPYONLY)
    set(LOADSCREEN_ROMFS_FILES "${CTRPORT_ROMFS_DIR}/loadscreen.bin")
else()
    file(REMOVE "${CTRPORT_ROMFS_DIR}/loadscreen.bin")
endif()

# Private opaque-run initializer. The public declaration keeps its original object,
# alignment, section and size, so this adds no runtime symbol or loader work.
set(CTRPORT_PIXGRID_RUNS "" CACHE FILEPATH "Private pixgrid_runs.inc from tools/aot/pixgrid.py")
if(CTRPORT_GAME STREQUAL "undertale")
    if(NOT CTRPORT_PIXGRID_RUNS OR NOT EXISTS "${CTRPORT_PIXGRID_RUNS}")
        message(FATAL_ERROR "Battle-grid runs missing: use build.sh with data=<your UNDERTALE folder>")
    endif()
    configure_file("${CTRPORT_PIXGRID_RUNS}" "${CTRPORT_BUILD_ID_DIR}/pixgrid_runs.inc" COPYONLY)
    get_filename_component(CTRPORT_PROMPT_INPUT_DIR "${CTRPORT_PIXGRID_RUNS}" DIRECTORY)
    foreach(prompt_input btnprompt_guards.inc btnprompt_fallback.c)
        if(NOT EXISTS "${CTRPORT_PROMPT_INPUT_DIR}/${prompt_input}")
            message(FATAL_ERROR "Private prompt table missing: use build.sh with data=<your UNDERTALE folder>")
        endif()
        configure_file("${CTRPORT_PROMPT_INPUT_DIR}/${prompt_input}" "${CTRPORT_BUILD_ID_DIR}/${prompt_input}" COPYONLY)
    endforeach()
    # UNDERTALE Old's covered-star key (tools/aot/dskip.py), included by runner.c's utkDrawRoom tail.
    if(NOT EXISTS "${CTRPORT_PROMPT_INPUT_DIR}/dskip_key.inc")
        message(FATAL_ERROR "Covered-star key missing: use build.sh with data=<your UNDERTALE folder>")
    endif()
    configure_file("${CTRPORT_PROMPT_INPUT_DIR}/dskip_key.inc" "${CTRPORT_BUILD_ID_DIR}/dskip_key.inc" COPYONLY)
endif()

# Deep frame skip's Draw classes for this game (bench/drawskip_<game>.txt), as drawskip_txt.h.
set(DRAWSKIP_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/bench/drawskip_${GAME_DIR_NAME}.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DRAWSKIP_SOURCE}")
configure_file("${DRAWSKIP_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/drawskip.txt" COPYONLY)
dkp_add_embedded_binary_library(drawskip_data "${CMAKE_CURRENT_BINARY_DIR}/drawskip.txt")

list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_legal_romfs.c")
# The test-only counter probe (CTRPORT_ASG_PROBE, below) is compiled only in its own build.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c")
# The test-only renderer table chain check (CTRPORT_VTCHAIN_CHECK, below) only in its own build.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_vtchain.c")
# The test-only half-resolution census and art (CTRPORT_HALF_CENSUS, CTRPORT_HALFART, below) only in their own builds.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_halfcensus.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_halfart.c")
add_library(relprep_romfs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_legal_romfs.c")
target_compile_options(relprep_romfs PRIVATE -Wall -Wextra)
# Defer the new reference until the original libctru archive pass has completed.
# Otherwise RomFS extracts the already-linked UTF helpers earlier and moves them.
set(RELPREP_ROMFS_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}relprep_romfs${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} \"${RELPREP_ROMFS_ARCHIVE}\" -lctru")
add_executable(ctrport ${PLATFORM_SOURCES} ${ENGINE_SOURCES} ${VENDOR_SOURCES}
    "${ENGINE_DIR}/src/data_win_slots_3ds.c" "cold/ctr_readback.c")
release_require_ndebug(ctrport)
add_dependencies(ctrport relprep_romfs)
set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${RELPREP_ROMFS_ARCHIVE}")
# source/ctr_legal.c: the legal viewer, opened from the paused settings menu's Legal notices item (its options page,
# source/ctr_screens.c, wraps CtrMenu_open and CtrMenu_update).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_legal.c"
    PROPERTIES COMPILE_OPTIONS "-fno-jump-tables;-fno-tree-switch-conversion;-fno-builtin")
# Opt-in measured layout. The generated script carries a complete reference
# forecast and fails the link on input drift; default builds retain 3dsx.specs.
set(CTRPORT_PHASE_LAYOUT "" CACHE FILEPATH "Audited phase_lock_layout.py linker script (empty: default)")
if(CTRPORT_PHASE_LAYOUT)
    get_filename_component(phase_layout "${CTRPORT_PHASE_LAYOUT}" ABSOLUTE)
    if(phase_layout MATCHES "[ \t]" OR CMAKE_CURRENT_BINARY_DIR MATCHES "[ \t]")
        message(FATAL_ERROR "GCC phase-layout specs require paths without whitespace")
    endif()
    if(NOT EXISTS "${phase_layout}")
        message(FATAL_ERROR "Missing phase layout: ${phase_layout}")
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-file-name=3dsx.specs
        OUTPUT_VARIABLE phase_specs_path OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE phase_specs_result)
    if(NOT phase_specs_result EQUAL 0 OR NOT EXISTS "${phase_specs_path}")
        message(FATAL_ERROR "Cannot locate the toolchain's 3dsx.specs")
    endif()
    file(READ "${phase_specs_path}" phase_specs)
    if(NOT phase_specs MATCHES "-T 3dsx.ld%s")
        message(FATAL_ERROR "Unexpected 3dsx.specs; regenerate and audit the layout")
    endif()
    string(REPLACE "-T 3dsx.ld%s" "-T ${phase_layout}" phase_specs "${phase_specs}")
    set(phase_specs_out "${CMAKE_CURRENT_BINARY_DIR}/phase-layout.specs")
    file(WRITE "${phase_specs_out}" "${phase_specs}")
    string(REPLACE "-specs=3dsx.specs" "-specs=${phase_specs_out}" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${phase_layout}" "${phase_specs_out}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${phase_layout}" "${phase_specs_path}")
    target_link_options(ctrport PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/phase-layout.map")
endif()
set_target_properties(ctrport PROPERTIES OUTPUT_NAME "${GAME_DIR_NAME}-3ds")
add_dependencies(ctrport ctrport_build_id)
# The bench loader no longer references the old data; retain its placeholder and object position explicitly.
target_link_options(ctrport PRIVATE -Wl,--undefined=bench_scenes_txt)
# pacealign (UNDERTALE only, above): the renderer's two calls that end a frame in the pacer go to the cut's wrappers.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPacer_frameBegun,--wrap=CtrPacer_frameEnded)
endif()
# DR New's tower emit finds coverage runs with clz (towerNextRun) and no longer calls libgcc's __ctzdi2. It stays
# linked so the code after it keeps its addresses: the late text is 4 KiB set-phase sensitive (dec-1031-setphase).
if(GAME_WAD_DEFINE STREQUAL "ENABLE_WAD17" AND CTRPORT_TARGET STREQUAL "n3ds")
    target_link_options(ctrport PRIVATE -Wl,--undefined=__ctzdi2)
endif()

# Late sections and their fixed reserves (late_reserve.ld says why and how features use them). ld only finds 3dsx.ld's
# sections for that script's INSERTs when the script comes first, and the toolchain's -specs=3dsx.specs names 3dsx.ld
# first. The link therefore uses a copy of devkitARM's 3dsx.specs that names late_reserve.ld ahead of it. The specs
# hold native paths: gcc and ld are Windows programs even when CMake runs in MSYS2.
set(CTRPORT_LATE_LD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/late_reserve.ld")
set(CTRPORT_LATE_LD "${CMAKE_CURRENT_BINARY_DIR}/late_reserve.ld")
file(READ "${CTRPORT_LATE_LD_SOURCE}" CTRPORT_LATE_LD_TEXT)
option(CTRPORT_PROFILE "Compile in the GML script profiler" OFF)
if(CTRPORT_PROFILE OR CTRPORT_REACH_GATE OR CTRPORT_HEAP_PROFILE OR CTRPORT_FREEWINS
   OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    # Resolve this before ld reads the script: DEFINED with a later --defsym
    # in 3dsx.specs' link command can still select the Release default. UNDERTALE Old's and DELTARUNE New's releases
    # already have 0x4000 (below), so their measurement builds keep one page more.
    # Heap, segment-timer and development configurations also add diagnostics;
    # keep those in the existing test reserve rather than the release reserve.
    if((CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
       OR (CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds"))
        string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x5000"
               CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    endif()
    string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x4000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# DELTARUNE: two more 4 KiB pages of late code, 0x32000 (on Old
# bggrid and crispart did not fit in the 3223 B left; on New crispart did not fit in the 4491 B left after bggrid,
# so both get pages). Only DELTARUNE's .rodata, .data and .bss move, by whole pages; UNDERTALE keeps 0x30000.
# Later: DELTARUNE Old 3DS two more pages, 0x34000 (crispart needs 6168 B of
# late code and 971 B were left); DELTARUNE New 3DS one more page, 0x33000 (123 B would be left after crispart, and
# the DELTARUNE effect work needs New late code). Only that target's .rodata, .data and .bss move, by whole pages.
# Then flowergpu: DELTARUNE New 3DS two more pages, 0x35000
# (flowergpu cuts 3 + 4a need 7984 B more late code; 3864 B were left, so 0x34000 still misses by 23 B), and one more
# page of late constants, 0x3000 (2 B would be left). Only that target's .rodata, .data and .bss move.
# Later: DELTARUNE Old two more pages, 0x36000; DELTARUNE New four, 0x39000
# (pacer 2328 B, oomport about 2 KB, utflowey's room-load swap 2768 B, dlgvid 1264 B, flowergpu3 cut 4b 3720 B on New,
# and New sized once for the DR New work in flight). Only that target's .rodata, .data and .bss move.
# Round 16 (lead rulings (a), integrate16, 2026-10-25): DELTARUNE Old two more pages, 0x38000; DELTARUNE New two,
# 0x3B000 (rename's SD folder move 3460 B on both, packscreen's first time setup 4997 B on both, flowergpu4's
# decoration runs 4536 B on New; Old had 646 B left after builderx and drstep F1, New 6710 B). Only that target's
# .rodata, .data and .bss move, by whole pages.
# Then one more page on each (review decision, 2026-10-25): DELTARUNE Old 0x39000, New 0x3C000, for the runtime RomFS
# name fix after the rename and the options menu and boot log fixes (1944 B on both; Old was 1557 B short, New 37 B).
# Only that target's .rodata, .data and .bss move, by one page.
# Round 17 (review decision option A, integrate17, 2026-10-25): five more pages on each, DELTARUNE Old 0x3E000, New
# 0x41000. The merged round (smartcut, rosalina2, the tile loop, the phase plan, the pacer spool) left Old 7,589 B
# and New 7,269 B over; screenshot (shots) builds use about 5.7 KB more; then utstall item 8 (1,190 B), flowergpu's
# hook (1,821 B, New) and one spare page. Only that target's .rodata, .data and .bss move, by whole pages.
# Round 18 (integrate18, 2026-10-26): DELTARUNE Old one more page, 0x3F000 (the merged round left 8,979 B, and
# 3,299 B in shots builds: one spare page). DELTARUNE New ten more, 0x4B000, for the work that all lands there:
# the merged round left 9,299 B (3,635 B in shots); drresid uses about 13.2 KB, hitches' room preload 0x3000, then
# drresid's Flowery work and knightnew, and one spare page. Only that target's .rodata, .data and .bss move.
# Round 21 (review decision A, gamedir2, 2026-10-26): DELTARUNE Old one more page, 0x40000, for gamedir (the game folder's
# listing and settings.ini's worker save: 8,652 B there, its constants in late text since DELTARUNE Old had 505 B of
# late constants left), 1,165 B over at 0x3F000; 2,931 B left. 4 KiB less heap on that target only.
# Then two more pages, 0x42000 (integrate21, 2026-10-26): legalui's Legal and Credits pages (3,566 B) did not fit in
# the 2,931 B gamedir2 left (the link failed), and credfix brings 1,444 B more; one spare page. Only DELTARUNE Old's
# .rodata, .data and .bss move, by two pages (8 KiB less heap on that target only).
# cand28 (freebytes page 2, review decision 2026-10-30): DELTARUNE Old one more page, 0x43000. stage75 left 844 B, and the
# pacer's game_screen fix and fpsbox, both built for every target, take about 840 B of it. Only DELTARUNE Old's
# .rodata, .data and .bss move, by one page (Old L1 set mapping unchanged).
# Round 22 (integrate22, approved shipping claims, 2026-10-27): DELTARUNE New three more pages, 0x4E000. cand21
# left 19,376 B; the approved claims are 24,753 B: hitches' room preload 12,917, titanthrash R4 2,745, flowend rounds
# 5-6 2,206, knightnew's pool 2,048 and GPU-2 1,288, perf-titan's markers <= 2,048, perf-knight rank 2 901, drsweep's
# list read 348 and credcap 252. Two pages cover them (2,815 B left); the third is the spare page (shots builds use
# about 5.7 KB more). Only DELTARUNE New's .rodata, .data and .bss move, by three pages (memory is plentiful on New).
# integrate24 event 2 (plan cbfecf919, approved 2026-10-27): DELTARUNE New four more pages, 0x52000. The stage
# left 6,307 B after freewins' wins 2+4; the claims are knightgpu L-A 1,476 and perf-flowery L2b about 10,132
# (an estimate), so three pages cover them and the fourth is the spare page. Late rodata (2,459 B left) is
# unchanged: knightgpu uses none. Only DELTARUNE New's .rodata, .data and .bss move, by four pages (linear -16 KB).
# freebytes DR New page (lead GO 2026-11-01): DELTARUNE New two more pages, 0x54000. 75zg left 5,336 B and 75zh
# 3,160 B; knightgpu's cover-cull needs 5,296 B of late text (+240 B late rodata, 4,312 B late bss) beside flowerhw
# D'+E, the pinwheel and the pacer. Text does not move; only DELTARUNE New's .rodata, .data, .bss and TLS move, by
# two pages (linear -8 KB).
# 75zo DR New pages (lead rulings 2026-11-02): DELTARUNE New five more pages, 0x59000, sized once for 75zo and the
# next stages: titancpu u1 5,024 B and pacer gbot's veneer move 80 B (in), then knightgpu FC2 5,744 B (+ up to 62 B of
# phase pads), fpsbox DR ~1,576 B, titancpu u2 rev 2 +448 B (u3 0 B), pacer KM rev 3 5,104 B, titan75's climb pre-read
# 4,096 B (ceiling) and knightgpu kolp ~1,424 B, against 7,735 B left at 75zn: about 4.6 KB spare after all of them.
# Late rodata keeps its page (2,386 B left; FC2 +294, KM +826 and titan75 +256 leave about 1,010 B). Whole pages, so
# text and the 4 KiB set phase do not move; only DELTARUNE New's .rodata, .data, .bss and TLS move, by five pages
# (linear -20 KB).
# Then one more late-constants page, 0x4000 (lead 2026-11-02): about 950 B would be left after FC2, KM and titan75, with
# kolp's constants unpriced. Only DELTARUNE New's .data, .bss and TLS move, by one whole page (linear -4 KB).
# 75zq unit 15 (review decision 2026-11-03, integrate21's 75zq ledger): DELTARUNE New three more pages, 0x5C000. zq11 left
# 7,828 B against 14,730 B of pending units: gsfix rev 3 2,439, flowerhw lever 294, the no-move dspe port ~4,108,
# titancpu sharebuf 4,976, pacer KM5 2,912 and repacka F2 1. Two pages cover them (~1,290 B left); the third is the
# spare page. Text and the 4 KiB set phase do not move; only DELTARUNE New's .rodata, .data, .bss and TLS move, by three
# whole pages (linear -12 KB).
# 75zq unit 22 (lead decision 2026-11-03, freebytes design b3f7153a1): DELTARUNE New two more pages, 0x5E000. zq18 left
# 9,336 B against 12,224 B of firm pending units: knightgpu B'' trim 2,608, titancpu G2 rev 2 2,580, flowerhw entry
# ~1,044, pacer KM5 5,992 and repacka F2 320; with repacka L1 + L2 1,759 and the titancpu composite ~600 about 2.6 KB
# are left. Late constants keep their page (~3.5 KB left after them). Text and the 4 KiB set phase do not move; only
# DELTARUNE New's .rodata, .data, .bss and TLS move, by two whole pages (linear -8 KB).
# 75zq unit 28 (review decision 2026-11-03, freebytes trigger f108cae26): DELTARUNE Old one more page, 0x44000. dofl2
# (unit 27, 1,599 B on both DELTARUNE targets) left 104 B, and repacka's padded F2 needs 256 B (proved on o3DS:
# DR Old Flowery worst frames 13-70 ms lower). Text and the 4 KiB set phase do not move; only DELTARUNE Old's
# .rodata, .data, .bss and TLS move, by one whole page. DR Old's linear memory is at its 32 MB cap, so the 4 KiB
# comes off the heap (.3dsx 25,536 -> 25,532 KB; installed 80 MB CIA about 39,880 -> 39,876 KB).
# zq27 DR New pages (review decision 2026-11-04, one reserve-only unit composed ahead of its users): DELTARUNE New four
# more pages, 0x62000. Frozen 75zq27 left 4,464 B against weirdshd production-ON 5,016 B (its own page folds into
# these), flowerhw palpre (palfast + palpre) 7,040 B, flowerhw vrgen 320 B and flowerhw afill ~3.5-4.5 KB (an
# estimate): about 4-5 KB are left after all of them. Late constants keep their page. Text and the 4 KiB set phase
# do not move; only DELTARUNE New's .rodata, .data, .bss and TLS move, by four whole pages (linear -16 KB).
# menusound (11-06): DELTARUNE Old one more page, 0x45000, as unit 28 did. The menu's hold,
# the ">" rows and the counter's corner are 592 B on every target, and it had 307 B left: 284 B over, the link fails. Text and
# the 4 KiB set phase do not move; only DELTARUNE Old's .rodata, .data, .bss and TLS move, by one whole page, and the 4 KiB comes
# off the heap, as linear is at its cap. 3,811 B left. UNDERTALE and DELTARUNE New are untouched.
# bubblecheck test builds (CTRPORT_BUBBLE_CHECK, never Release): four more pages for the checker, which is late code
# with the rest of z_smalltext.c. Added ahead of the game's own value below, which then replaces the placeholder.
if(CTRPORT_BUBBLE_CHECK)
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "0x4000 + /* LATE_TEXT_RESERVE */ 0x30000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds")
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "0x45000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
elseif(CTRPORT_GAME STREQUAL "deltarune")
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "0x62000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x4000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
else()
    # UNDERTALE, Old and New 3DS: late code 0x54000, set once for a batch of merges. A probe with all of them (the
    # waterfall, lava2, the Smoothing menu, the dust swarm, the builder) used 0x4c564 B on Old (+42,055 B over the
    # previous 0x44000 build); the rest is room for z_sdmigrate (3.5 KB) and the merge-later list (the tile loop and
    # helix step, symverdict, the battle code reads, Asgore 8.9 KB, smartcut, the keyboard: about 20 KB) plus one spare
    # page. Before that it was 0x44000.
    # Round 17 (review decision option A, integrate17, 2026-10-25): ten more pages, 0x5E000. The merged round (Asgore,
    # the flame tile loop, circle and helix, the kill waves, smartcut, utflowey's tile aging, the pacer spool,
    # rosalina2) left Old 27,845 B and New 26,389 B over 0x54000; shots builds use about 5.6 KB more; then utstall
    # item 8 (1,190 B) and one spare page.
    # Round 18 (integrate18, 2026-10-26): one more page, 0x5F000 (GOG 0x61000). The merged round (the bench's
    # presentation fields, Mettaton's break card, utstall F1, the packed tier fix) left Old 7,531 B and New 9,495 B,
    # but Old only 1,891 B in shots builds: one spare page. Kept small: on the Old 64 MB layout every page comes off
    # the heap, and so off the packed tier.
    # Round 19 (integrate19, 2026-10-26): one more page, 0x60000 (GOG 0x62000). The merged round (Hotland partial
    # sprites, utflowey's opera pre-cull, turned runs and dialogue code read-ahead, cheaplevers) left Old 6,607 B and
    # New 8,575 B, but only 967 B / 2,927 B in shots builds: hintpos (small) and one spare page.
    # Round 21 (integrate21, 2026-10-26): four more pages, 0x64000 (GOG 0x66000). cand20 left Old 8,623 B and New
    # 10,591 B; round 21 brings about 20.5 KB to both (gamedir2 ~8 KB, legalui ~3.5 KB, bubbleclip ~4.5 KB, credfix
    # ~2.2 KB). On the Old 64 MB layout the 16 KiB comes off the heap and so off the packed tier (measured on Azahar).
    # Only that target's .rodata, .data and .bss move by whole pages. check_layout --expect-shift summarizes
    # verified uniform OBJECT shifts; inspect remaining failures (TLS separately), and require shiftproof.py PROVEN.
    # Round 21f (integrate21, review decision 2026-10-27): one more page, 0x65000 (GOG 0x67000). utflowey2 (packed copies
    # in spare linear memory, 6,409 B) left Steam Old 305 B, and the Old shots and halfart builds no longer linked
    # (about 1.3 / 1.8 KB over); P1 work still to come (credfix option 3 about 2 KB, uttour's helpers, pacer no-age).
    # Round 22 (integrate22, review decision dec-1027-07, 2026-10-27): one more page, 0x66000 (GOG 0x68000). The approved
    # UNDERTALE Old claims are 5,595 B (credfix option 3 2,135 + credcap 252, flowend rounds 5-6 2,856, drsweep's
    # list read 348) against the 4,402 B that round 21f's page left.
    # Round 25 (integrate21 cand25 PLAN, approval 2026-10-28): two more pages on the Old 3DS only, 0x68000 (GOG
    # 0x6A000); New keeps 0x66000 and its addresses. Old had 2,389 B left after the sound-effect pack (10,360 B),
    # pcmwait, c112dtm and pacer NF1, against credfix T1 (2,505 B), benchfix r7 (2,292 B) and freewins (b). On the Old
    # 64 MB layout the heap is on its 20 MB floor, so the 8 KiB comes off linear memory (the tile budget).
    # cand28 (freebytes, 2026-10-29): one more page on the Old 3DS only, 0x69000 (GOG 0x6B000). cand27 left
    # 5,063 B; pausemus (immediate pause hold and exact resume, maintainer's ask) linked 316 B past the limit on 542fca37e,
    # and fpsbox, homecrash, flowend's pre-open, redpix and the pacer's game_screen fix come in the same batch. Only Old
    # .rodata, .data and .bss move, by one page (Old L1 set mapping unchanged); the 4 KiB comes off linear memory.
    # cand28 page 2 (freebytes, review decision 2026-10-30): one more, 0x6A000 (GOG 0x6C000). stage75 left 8,868 B; the
    # ranked set (pacer 596, pausemus 4,116, pre-open 1,088, loadaudio ~716, fpsbox 244, fs5 2,200 B) left 4 B, and
    # homecrash (1,176 B) and flowhitch (448 B) did not fit. The same whole-page move: linear 4 KiB less.
    # Page 3 (freebytes, 2026-10-30; merges only if a felt fix needs it): 0x6B000 (GOG 0x6D000). The full cand28
    # set leaves about 450 B; asrfight's native trail front (1.2-1.8 KB) and a Hopes and Dreams pre-open (150-250 B)
    # may need room. Same whole-page move, UNDERTALE Old only: linear 4 KiB less.
    # Page 4 (integrate21 stage75q, review decision 2026-10-30): 0x6C000 (GOG 0x6E000). On stage75p (11,244 B left) the
    # cand28 set (flowhitch 3m 2,900, flowend 4d 856, loadaudio 1,560, itemfront 3,180, asgore30 asb 4,117 B) linked
    # 1,369 B past the limit. Same whole-page move, UNDERTALE Old only: linear 4 KiB less (memcap 59,140).
    # Page 5 (freebytes, GO 2026-11-01 on stage75zg): 0x6D000 (GOG 0x6F000). 75zg left 1,080 B; the credits scene
    # (creddodge) still misses locked 30 and its levers (lever 1 573 B + dprank 164 B, b' about 1.0-1.3 KB, the pacer
    # e-rule 400-736 B) need more. Same whole-page move, UNDERTALE Old only: linear 4 KiB less.
    # Page 6 (freebytes, review decision 2026-11-01 on stage75zl): 0x6E000 (GOG 0x70000). 75zl left 1,120 B against the
    # next Flowey levers (pacer's chunked kick about 3.9 KB, omegaflowey L1+L2 rev 3 about 690 B, flowend's burst front
    # about 600 B, a possible half-art port about 1.9 KB). Same whole-page move, UNDERTALE Old only: linear 4 KiB less.
    # Pages 7 + 8 and New page 1 (integrate21 stage75zn, GO 2026-11-02 on stage75zm): Old 0x70000 (GOG 0x72000),
    # New 0x67000 (GOG 0x69000). 75zm left Old 3,411 B and New 1,457 B against about 8.5-9 KB (Old) and 3 KB (New) of
    # GO units (oldexact, restartexact, racefix, audiofix, flowend G1+G2) and the ones still in review (hitbox,
    # asgore30, cpugen one pool). The same whole-page move: text does not move; only .rodata, .data, .bss and TLS move,
    # by whole pages (same cache sets). Linear memory 8 KiB less on Old (memcap 59,120), 4 KiB less on New.
    # New page 2 (lead grant 2026-11-02, stage75zn): 0x68000 (GOG 0x6A000). audiofix's coming head adds about 2.7 KB on
    # UT New (H1/M1/F1/F2/S2, drone, play-through), with restartexact 630, hitbox 960, cpugen about 1,000 and redpix
    # about 370 B: past the 5,553 B one page left. The same whole-page move; New's linear 4 KiB less again.
    # Pages 9 + 10 (review decision 2026-11-02, option C, on freebytes' 75zn ledger): Old 0x72000 (GOG 0x74000). After
    # hitbox UT Old had 8,318 B left against 8,694 B of READY/gated units (cpugen, the rest of audiofix, redpix, fpsbox,
    # ctrlui, dlgtop linger, flowend G1+G2) with btlentry and loadgame on top. The same whole-page move: text does not
    # move; only .rodata, .data, .bss and TLS move, by whole pages. Linear memory 8 KiB less on Old (memcap 59,112).
    # Old page 11 + New page 3 (integrate21 stage75zo, GO 2026-11-02): Old 0x73000 (GOG 0x75000), New 0x69000
    # (GOG 0x6B000). After audiofix 75zo left Old 1,866 B (btlentry's slot holds r4 rev 2 + r5) and New 2,434 B against
    # ctrlui (~1,152 B and its strings), loadgame/dlgtop (up to 1,924 B), dlghold (168 B), omegaflowey's room-330
    # variant and cpugen (~1 KB). The same whole-page move. Old's heap stays 20 MiB (DEFAULT_HEAP_MIN_MB); its linear
    # memory, the rest under the 38 MiB cap, drops 8 KiB with the late-constants page below (37.72 MiB, memcap 59,104;
    # the Omega Flowey set evicted at 36). New's linear 4 KiB less.
    # Old page 12 (review decision 2026-11-03, freebytes zq ledger): loadgame 1,336 + omegaflowey W2 2,010 + pacer C/A
    # against 1,391 B left. The same whole-page move; Old's linear memory 4 KiB less (memcap 59,100).
    # Old pages 13 + 14 (review decision 2026-11-03, 75zq unit 32): pcorder 1,076 + arena ~1,720 + thinart 1,460 +
    # loadgame <=700 (+ creddodge textfast ~4.6 KB if its rows show value) against 3,422 B left. The same whole-page
    # move; Old's linear memory 8 KiB less (memcap 59,092). CONSTRAINT: at the Omega Flowey scene's entry, Old sits
    # within about 4-8 KiB of the linear floor that sheds sound effects and tiles. At 59,092 that shed evicts one more
    # tile (0.1 MB) than at 59,100 (UT10 capped twins; the tile is not loaded again in that session). Each further Old
    # page costs a tile there unless linear memory grows (memsplit's 19 / 39 split is the clean fix).
    # New pages 4 + 5 (lead GO 2026-11-04, 75zq unit 38): New 0x6B000 (GOG 0x6D000). After pcorder New has 1,520 B
    # left against rngexact 1,556, thinart ~1,460-1,576, loadgame 688, creddodge speed memo 260 and utlife A 88
    # (4,168 B firm, ~4.9 KB with utlife B and offdraw early-destroy): one page would leave ~700 B. The same
    # whole-page move: text does not move; only New's .rodata, .data, .bss and TLS move, by two whole pages (New's
    # heap and linear about 4 KiB less each of 57,268 KB).
    # Feature reserve (change bufexp, 2026-11-04, on the maintainer's ask for room for post-1.0 features): Old 256 KiB
    # (0x40000) and New 512 KiB (0x80000) more, unused until features land. The same whole-page move: text does not
    # move; .rodata, .data, .bss and TLS move by exactly the reserve. On Old every KiB comes off linear memory and the
    # texture budget (heap held at 20 MiB): Omega Flowey's whole set (25,984 KB) must fit the base budget
    # (z_utstall.c utflWholeAll), which left ~620 KiB at zq45, so 256 KiB keeps ~360 KiB for the features' own memory
    # (bufexp's results: 512 KiB clean on Azahar, 1024 KiB cut the set, 3 held frames in the fight).
    # New loses half each from heap and linear (57,264 KB each). memcap: tools/analysis/console_memcap.py.
    set(ctrport_ut_late 0xEB000)
    if(CTRPORT_TARGET STREQUAL "o3ds")
        # Old 1.0 envelope (the maintainer's conditional OK, 11-09): six more
        # pages of late code, 0xB6000 -> 0xBC000, with the AAC text slot's +36 KiB (aacout.cmake) and 284 B of zeroed
        # late .bss (below).
        set(ctrport_ut_late 0xBC000)
    endif()
    if(CTRPORT_GOG_COMPAT)
        # review decision, gogut round 17: GOG UT alone needs two more 4 KiB pages over Steam (integrate22: over
        # round 22's 0x66000).
        math(EXPR ctrport_ut_late "${ctrport_ut_late} + 0x2000" OUTPUT_FORMAT HEXADECIMAL)
    endif()
    # testpage (build.sh "testpage"; measurement builds only, never a release): one more 4 KiB page, so that a
    # lever's A and B both link while the shipping layout is decided at integration (change uthunt T1, 2026-10-31).
    # Only .rodata, .data and .bss move, by one whole page (set-neutral).
    if(CTRPORT_TEST_LATE_PAGE)
        math(EXPR ctrport_ut_late "${ctrport_ut_late} + 0x1000" OUTPUT_FORMAT HEXADECIMAL)
    endif()
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "${ctrport_ut_late}"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# UNDERTALE Old 3DS: two more 4 KiB pages of late constants, 0x4000: the first when 45 B were left, with the
# per-effect fast paths still to come; the second (integrate21 stage75zo, GO 2026-11-02) when 731 B were left
# against the controls page's strings. After the profile/reachgate substitution above (0x5000 on this target).
# Only this target's .data and .bss move, by whole pages; no text or .rodata address changes.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x4000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    # Old 1.0 envelope (relready-resize.md): 284 B of zeroed late .bss booked for the pending roots; it crosses into one
    # more 4 KiB page by design. Features that need late statics take them from here.
    string(REPLACE "/* RESIZE_BSS_RESERVE */" "__resize_bss_reserve = ABSOLUTE(.); . += 284;"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# UNDERTALE New 3DS: one more 4 KiB page of late constants, 0x3000 (integrate21 dv8q, GO 11-09), when 46 B were
# left. Only this target's .data and .bss move, by one whole page; no text or .rodata address changes. 0x3000, not
# 0x4000: the dev line's New builds (Release type) link with this same reserve, so a bigger one would not make release
# match anything already run on hardware; the profile/heap/reachgate/freewins and non-Release builds keep their 0x4000
# (substituted above), one page more, as the comment above asks.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "n3ds")
    string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x3000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# freebytes (lead brief 2026-10-29): UNDERTALE Old 3DS's late code reserve is the one every fix spends, while its late
# constants page had 3,609 B spare. Two cold constant blocks move from the late text block to the end of the late
# constants list: the crash screen's (z_argcrash.c, 2,004 B) and the bench files' names (ctr_bench_io.c, 480 B). Same
# bytes, read only in both places; only late addresses change (tools/analysis/check_smoothux_layout.py proves it).
# ctr_bench_io.o's defensive catch-all for its other read-only input moves with them (it would otherwise take the
# bench names back into late text). Other targets keep all three lines in late text (DELTARUNE Old has about 400 B of
# late constants spare).
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    foreach(freebytes_sel "*(.rodata.argcrash_in_text)" "*(.rodata.pacer_io_in_text)" "*ctr_bench_io.o(.rodata .rodata.*)")
        string(FIND "${CTRPORT_LATE_LD_TEXT}" "    ${freebytes_sel}
" freebytes_at)
        if(freebytes_at LESS 0)
            message(FATAL_ERROR "freebytes: late_reserve.ld has no line ${freebytes_sel}")
        endif()
        string(REPLACE "    ${freebytes_sel}
" "" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
        string(REPLACE "/* FREEBYTES_RODATA */" "${freebytes_sel}
    /* FREEBYTES_RODATA */"
               CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    endforeach()
    # freebytes round 3: ctr_screens.c's settings strings (2,036 B, read only by the screens menu, the settings save
    # and the mode names) go to the end of the late data page (3.3 KB spare), the one cut before the first hot late
    # unit, so all late code after it moves by one amount. ctr_display.h's CTR_COLD_OS is the other half.
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "    *(.rodata.screens_in_text)
" freebytes_at)
    if(freebytes_at LESS 0)
        message(FATAL_ERROR "freebytes: late_reserve.ld has no line *(.rodata.screens_in_text)")
    endif()
    string(REPLACE "    *(.rodata.screens_in_text)
" "" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    string(REPLACE "/* FREEBYTES_DATA */" "*(.rodata.screens_in_text)" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# freebytes G3 (dec-1031-drnew-page, 2026-10-31): DELTARUNE compiles collision_gm14_dr.c in collision_gm14.c's place
# (GameMaker 1.4's collision rules never apply to bytecode 17). Its two small functions take the start of the old
# block; a pad then ends the block 0x3000 bytes (three 4 KiB pages) before its old end, which was collision_gm14.c's
# 15,308 bytes (.text.late.colflip_gm14 0x3a98 plus its two RValue copies) after the block's 8-byte alignment. Every
# later late function moves by exactly -0x3000, so the instruction-cache sets of all of them stay the same. The pad
# would be negative, and the link fail, if the new code ever outgrew the kept 3,020 bytes. The late constants that
# were collision_gm14.c's are kept (KEEP: nothing references them now), so no late constant moves.
if(CTRPORT_GAME STREQUAL "deltarune")
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "    *(.text.late.colflip_gm14)
" g3_at)
    if(g3_at LESS 0)
        message(FATAL_ERROR "freebytes G3: late_reserve.ld has no line *(.text.late.colflip_gm14)")
    endif()
    string(REPLACE "    *(.text.late.colflip_gm14)
" "    __colflip14_dr_start = ABSOLUTE(.);
    *(.text.late.colflip_gm14)
" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "    *collision_gm14.o(.text .text.*)
" g3_at)
    if(g3_at LESS 0)
        message(FATAL_ERROR "freebytes G3: late_reserve.ld has no line *collision_gm14.o(.text .text.*)")
    endif()
    string(REPLACE "    *collision_gm14.o(.text .text.*)
" "    __colflip14_dr_pad = ABSOLUTE(.);
    . += __colflip14_dr_start + ((0 - __colflip14_dr_start) & 7) + 15308 - 0x3000 - __colflip14_dr_pad;
" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "    *(.rodata.late.colflip_gm14)
" g3_at)
    if(g3_at LESS 0)
        message(FATAL_ERROR "freebytes G3: late_reserve.ld has no line *(.rodata.late.colflip_gm14)")
    endif()
    string(REPLACE "    *(.rodata.late.colflip_gm14)
" "    KEEP(*(.rodata.late.colflip_gm14))
" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# relprep move (integrate21 stage73, review decision 2026-10-29): the Legal page's code and constants go last in late text
# and late constants, since their size follows the release source URL (the builder's release profile). The pads left
# in their old places keep everything after them on its stage72 / cand26 cache-line phase: each anchor is the address
# mod 32 that followed relprep's code (text) and its constants (rodata) in stage72's Steam release link of this game
# and model (measured on stage72's maps). The pad is 0-31 B, computed by ld from it.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set(ctrport_relprep_phase 20 12)
elseif(CTRPORT_GAME STREQUAL "undertale")
    set(ctrport_relprep_phase 28 24)
elseif(CTRPORT_TARGET STREQUAL "o3ds")
    set(ctrport_relprep_phase 12 24)
else()
    set(ctrport_relprep_phase 12 20)
endif()
list(GET ctrport_relprep_phase 0 ctrport_relprep_text_phase)
list(GET ctrport_relprep_phase 1 ctrport_relprep_rodata_phase)
foreach(kind TEXT RODATA)
    string(TOLOWER "${kind}" kind_lower)
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "/* RELPREP_${kind}_PHASE */ 0" relprep_at)
    if(relprep_at LESS 0)
        message(FATAL_ERROR "relprep: late_reserve.ld has no RELPREP_${kind}_PHASE anchor")
    endif()
    string(REPLACE "/* RELPREP_${kind}_PHASE */ 0" "${ctrport_relprep_${kind_lower}_phase}"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endforeach()
# Phase pads after UNDERTALE's smalltext, z_utflames.o's fast paths, utflames and utflames-helix late text (stage73,
# review decision 2026-10-29): stage72's address mod 32 after each group, per model (measured on stage72's
# maps). DELTARUNE has none of these groups: its anchor is the pad's own start, so the pad is 0 B.
# freebytes round 4 (stage74): pads after utkill (UNDERTALE Old's StateHash_runDraw is smaller) and bootlog (two of its
# once-a-run functions compiled for size): stage72's address mod 32 after each group on UNDERTALE Old (8 and 8, the
# same on stage73/74 before freebytes). Elsewhere utkill anchors at the pad's own start, so the pad is 0 B.
# The bootlog anchor is per target since the boot-log fix (freebytes, 10-29; stage75: the hiding starts before the
# log's first line and its once-a-run functions are compiled for size on all four targets): cand27's address mod 32
# after bootlog's constants on each (UT Old 8, UT New 16, DR Old 8, DR New 28), so later late code keeps cand27's phase.
# The itemfront anchor (lead GO 10-30, dec-1029-phase-pads): UNDERTALE Old 24, the phase itemfront was measured at on
# the console (asrfight frozen/if4); elsewhere itemfront is empty and the pad is 0 B. The evguard anchor (lead GO 10-30,
# dec-1029-phase-pads): UNDERTALE Old 16, the phase of evguard's console rows and rev 2 identity build (asgore30
# frozen/evg1, evg2); a 0 B pad elsewhere (UNDERTALE New was not measured on a console; DELTARUNE has no evguard).
# The pacerflt anchor (lead GO 10-30, dec-1029-phase-pads): UNDERTALE Old 0, the phase of the prefilter's console rows
# (change pacer, prefstack run B); a 0 B pad elsewhere (10-30: no New pads; DELTARUNE has none).
# The flowhitch anchor (lead GO 10-31, dec-1029-phase-pads): UNDERTALE Old 24, the phase of flowhitch rev 3m's console
# rows (0x18); a 0 B pad elsewhere (no New pads; DELTARUNE's flowhitch section is empty).
# The postkick anchor (lead 03:2x 10-31, dec-1029-phase-pads; it runs every frame): UNDERTALE Old 8, the phase of its
# console rows (change pacer, postkick run B: __wrap_Utsw_handleRoomChange at 0x6703e8); a 0 B pad elsewhere
# (no New pads; DELTARUNE's postkick section is empty).
# freebytes r5b pads (dec-1029-phase-pads, c26rev r5 F1): smalltextro, aotlate, utsfx, utlava, utoldreg, cheaplevers
# and asgborder sit at the r5 cut boundaries; each anchor is stage75zd's address mod 32 there (47d487eb0, built at
# 58baa4700; the next late input's start; utlava: aotul_11, utoldreg: aotult_11_00B8), per UNDERTALE model, so every
# later late function keeps stage75zd's phase; aotlhelp, aotlgen and utlhelp keep the folded AOT objects' own
# aotSlotFind copies and aotl_31 on stage75zd's phase too. DELTARUNE has none of the cuts: each anchor is the pad's
# own start, a 0 B pad.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set(ctrport_smalltext_text_phase 16)
    set(ctrport_utfaot_text_phase 24)
    set(ctrport_utflames_text_phase 9)
    set(ctrport_utfhelix_text_phase 16)
    set(ctrport_statehash_text_phase 8)
    set(ctrport_bootlog_text_phase 8)
    set(ctrport_itemfront_text_phase 24)
    set(ctrport_evguard_text_phase 16)
    set(ctrport_pacerflt_text_phase 0)
    set(ctrport_flowhitch_text_phase 24)
    set(ctrport_postkick_text_phase 8)
    set(ctrport_smalltextro_text_phase 24)
    set(ctrport_aotlate_text_phase 4080)
    set(ctrport_utsfx_text_phase 0)
    set(ctrport_utlava_text_phase 16)
    set(ctrport_utoldreg_text_phase 3136)
    set(ctrport_cheaplevers_text_phase 8)
    set(ctrport_asgborder_text_phase 780)
    set(ctrport_aotlhelp_text_phase 988)
    set(ctrport_aotlgen_text_phase 1888)
    set(ctrport_utlhelp_text_phase 1896)
elseif(CTRPORT_GAME STREQUAL "undertale")
    set(ctrport_smalltext_text_phase 0)
    set(ctrport_utfaot_text_phase 8)
    set(ctrport_utflames_text_phase 25)
    set(ctrport_utfhelix_text_phase 24)
    set(ctrport_statehash_text_phase __statehash_text_gap_start)
    set(ctrport_bootlog_text_phase 16)
    set(ctrport_itemfront_text_phase __itemfront_text_gap_start)
    set(ctrport_evguard_text_phase __evguard_text_gap_start)
    set(ctrport_pacerflt_text_phase __pacerflt_text_gap_start)
    set(ctrport_smalltextro_text_phase 8)
    set(ctrport_aotlate_text_phase 0)
    set(ctrport_utsfx_text_phase 8)
    set(ctrport_utlava_text_phase 24)
    set(ctrport_utoldreg_text_phase 8)
    set(ctrport_cheaplevers_text_phase 16)
    set(ctrport_asgborder_text_phase 20)
    set(ctrport_aotlhelp_text_phase 12)
    set(ctrport_aotlgen_text_phase 16)
    set(ctrport_utlhelp_text_phase 16)
    set(ctrport_flowhitch_text_phase __flowhitch_text_gap_start)
    set(ctrport_postkick_text_phase __postkick_text_gap_start)
else()
    foreach(pad smalltext utfaot utflames utfhelix statehash itemfront evguard pacerflt flowhitch postkick
            smalltextro aotlate utsfx utlava utoldreg cheaplevers asgborder aotlhelp aotlgen utlhelp)
        set(ctrport_${pad}_text_phase __${pad}_text_gap_start)
    endforeach()
    if(CTRPORT_TARGET STREQUAL "o3ds")
        set(ctrport_bootlog_text_phase 8)
    else()
        set(ctrport_bootlog_text_phase 28)
    endif()
endif()
# knightgpu FC rev 2 pads (c26rev r103 F2, dec-1029-phase-pads): rev 2 grows SY and FC on DELTARUNE New. After SY:
# stage dr75's (728fe66e8) release address mod 32 of CtrMemory_vramAllocFit, 0x3458c8 (8), FC's own proven phase.
# After FC: 75zo's (integrate21, 11-02) flowerhw tshare tsStep0 before SY + FC came in, 0x3456b4 (20), so tshare and
# every late function after it keep their 32 B phase. Elsewhere, and with FC off, each anchor is the pad's own start, a
# 0 B pad.
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds"
   AND NOT (DEFINED CTRPORT_KNIGHTGPU_FRAGCOPY AND NOT CTRPORT_KNIGHTGPU_FRAGCOPY))
    set(ctrport_kgsy_text_phase 8)
    set(ctrport_kgfc_text_phase 20)
else()
    set(ctrport_kgsy_text_phase __kgsy_text_gap_start)
    set(ctrport_kgfc_text_phase __kgfc_text_gap_start)
endif()
foreach(pad SMALLTEXT UTFAOT UTFLAMES UTFHELIX STATEHASH BOOTLOG ITEMFRONT EVGUARD PACERFLT FLOWHITCH POSTKICK
            SMALLTEXTRO AOTLATE UTSFX UTLAVA UTOLDREG CHEAPLEVERS ASGBORDER AOTLHELP AOTLGEN UTLHELP KGSY KGFC)
    string(TOLOWER "${pad}" pad_lower)
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "/* ${pad}_TEXT_PHASE */ 0" pad_at)
    if(pad_at LESS 0)
        message(FATAL_ERROR "phase pads: late_reserve.ld has no ${pad}_TEXT_PHASE anchor")
    endif()
    string(REPLACE "/* ${pad}_TEXT_PHASE */ 0" "${ctrport_${pad_lower}_text_phase}"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endforeach()
# freebytes H1 (change freebytes, 2026-10-31; late_reserve.ld's H1 holes): on UNDERTALE Old the cold cuts and AOT folds
# leave holes in place, filled with whole cold loading-screen functions, and the loading screen's end pad moves the late
# code after it by exactly -4096 B (one I-cache set period). Each hole pad ends on stage75zd's address mod 4096 there
# (b89469fb3 built with the release URL: the end of the selector the hole follows); the fold pads above take the same
# mod-4096 anchors with a 4095 mask on UNDERTALE Old (each is its old mod-32 anchor's 4096 B form), 31 elsewhere.
# Other targets: each H1 pad's own start, a 0 B pad.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set(ctrport_h1loadtime_text_phase 40)
    set(ctrport_h1coverage_text_phase 2716)
    set(ctrport_h1rend_text_phase 515)
    set(ctrport_h1utstall_text_phase 264)
    set(ctrport_h1bxreview_text_phase 1460)
    set(ctrport_h1relprep_text_phase 4076)
    set(ctrport_h1utloop_text_phase 3104)
    set(ctrport_h1_fold_mask 4095)
else()
    foreach(pad loadtime coverage rend utstall bxreview relprep utloop)
        set(ctrport_h1${pad}_text_phase __h1${pad}_text_gap_start)
    endforeach()
    set(ctrport_h1_fold_mask 31)
endif()
foreach(pad LOADTIME COVERAGE REND UTSTALL BXREVIEW RELPREP UTLOOP)
    string(TOLOWER "${pad}" pad_lower)
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "/* H1${pad}_TEXT_PHASE */ 0" pad_at)
    if(pad_at LESS 0)
        message(FATAL_ERROR "freebytes H1: late_reserve.ld has no H1${pad}_TEXT_PHASE anchor")
    endif()
    string(REPLACE "/* H1${pad}_TEXT_PHASE */ 0" "${ctrport_h1${pad_lower}_text_phase}"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endforeach()
foreach(pad AOTLHELP AOTLGEN AOTLATE UTLAVA UTLHELP UTOLDREG ASGBORDER)
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "/* ${pad}_TEXT_MASK */ 31" pad_at)
    if(pad_at LESS 0)
        message(FATAL_ERROR "freebytes H1: late_reserve.ld has no ${pad}_TEXT_MASK")
    endif()
    string(REPLACE "/* ${pad}_TEXT_MASK */ 31" "${ctrport_h1_fold_mask}" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endforeach()
# freebytes H1 hole limits (flushrev R4-F2): on UNDERTALE Old each hole's end in the frozen H1 layout (stage75zd + H1
# rev 2, frozen build B2). A hole pads to it while its start is at or before it and pads nothing otherwise, so test
# profiles that add late code before a hole (shots, spikerec, ...) still link, packed. Release builds (the release
# source URL set) also ASSERT every hole ends at or before it, so any overflow stops the link whatever its size (a
# wrapped pad would end the hole 4 KiB or more later). A change that moves late code by whole pages before a hole
# updates these. Other targets: the hole's own start and end, always true.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set(ctrport_h1loadtime_text_limit 0x608028)
    set(ctrport_h1coverage_text_limit 0x610a9c)
    set(ctrport_h1rend_text_limit 0x622203)
    set(ctrport_h1utstall_text_limit 0x62b108)
    set(ctrport_aotlhelp_text_limit 0x6353dc)
    set(ctrport_aotlgen_text_limit 0x635760)
    set(ctrport_aotlate_text_limit 0x635ff0)
    set(ctrport_utlava_text_limit 0x640010)
    set(ctrport_utlhelp_text_limit 0x641768)
    set(ctrport_h1utloop_text_limit 0x641c20)
    set(ctrport_utoldreg_text_limit 0x641c40)
    set(ctrport_h1bxreview_text_limit 0x64e5b4)
    set(ctrport_h1relprep_text_limit 0x66bfec)
    set(ctrport_asgborder_text_limit 0x66e30c)
    # Old 1.0 envelope (relready-resize.md, 029892e87): the AAC text slot's +36 KiB moves all late code by nine whole
    # pages, so every hole limit moves with it (the rule above); the mod-4096 phases stay.
    foreach(hole h1loadtime h1coverage h1rend h1utstall aotlhelp aotlgen aotlate utlava utlhelp h1utloop utoldreg
                 h1bxreview h1relprep asgborder)
        math(EXPR ctrport_${hole}_text_limit "${ctrport_${hole}_text_limit} + 0x9000" OUTPUT_FORMAT HEXADECIMAL)
    endforeach()
endif()
foreach(hole H1LOADTIME H1COVERAGE H1REND H1UTSTALL AOTLHELP AOTLGEN AOTLATE UTLAVA UTLHELP H1UTLOOP UTOLDREG H1BXREVIEW H1RELPREP ASGBORDER)
    string(TOLOWER "${hole}" hole_lower)
    foreach(kind LIMIT STRICT)
        if(kind STREQUAL "LIMIT")
            set(hole_marker "/* ${hole}_TEXT_LIMIT */ __${hole_lower}_text_gap_start")
        else()
            set(hole_marker "/* ${hole}_TEXT_STRICT */ __${hole_lower}_text_gap_end")
        endif()
        string(FIND "${CTRPORT_LATE_LD_TEXT}" "${hole_marker}" hole_at)
        if(hole_at LESS 0)
            message(FATAL_ERROR "freebytes H1: late_reserve.ld has no ${hole}_TEXT_${kind}")
        endif()
        if(DEFINED ctrport_${hole_lower}_text_limit AND (kind STREQUAL "LIMIT" OR CTRPORT_RELEASE_SOURCE_URL))
            string(REPLACE "${hole_marker}" "${ctrport_${hole_lower}_text_limit}" CTRPORT_LATE_LD_TEXT
                   "${CTRPORT_LATE_LD_TEXT}")
        endif()
    endforeach()
endforeach()
# UNDERTALE did not link libctru's RomFS driver on main 36a55611. benchromfs (first) put romfs_dev.o right after its own
# late lines, so its addresses are frozen there; relprep's later RELPREP_ROMFS_<kind> placeholders stay empty (a second
# selector for an input section already placed would match nothing). DELTARUNE already linked it at frozen addresses.
# The archive selectors are read before 3dsx.ld's general section wildcards.
foreach(kind text rodata data bss)
    string(REPLACE "/* RELPREP_ROMFS_${kind} */" "" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    if(CTRPORT_GAME STREQUAL "undertale")
        string(REPLACE "*(.${kind}.late.benchromfs)"
               "*(.${kind}.late.benchromfs)
    *libctru.a:romfs_dev.o(.${kind} .${kind}.*)"
               CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    endif()
endforeach()
# The spike recorder's playtest builds only (change spikerec): two more 4 KiB pages of late code on top of the target's
# reserve, for its 4.8 KB (UNDERTALE Old had 572 B left on main a7ed1173e, New 2036 B). Text keeps its addresses;
# .rodata, .data and .bss move by two whole pages in those builds alone. Release builds never see this.
if(CTRPORT_SPIKEREC)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x2000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# frametrace's test builds only (change benchfix): the same two pages for its code and constants (z_frametrace.c). Text
# keeps its addresses; .rodata, .data and .bss move by whole pages in those builds alone.
if(CTRPORT_FRAMETRACE)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x2000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# flowdiag's lab builds only (change flowstream): one more page for its code and constants (z_flowdiag.c, in flowstream's
# late section). Text keeps its addresses; .rodata, .data and .bss move by one page in those builds alone.
if(CTRPORT_FLOWSTREAM_DIAG)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x1000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# The Titan CPU recorder (change titancpu, titandiag builds only): two more 4 KiB pages of late code for its own. Never a
# release; .rodata, .data and .bss move by two pages in those builds alone.
if(CTRPORT_TITANDIAG)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x2000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# Screenshot (shots) builds on Old 3DS, both games (review decision, legalui round 21, 2026-10-26): one more 4 KiB page of
# late code. Shots builds keep test code release builds leave out, and cand20 left UNDERTALE Old 2,984 B and DELTARUNE
# Old 1,807 B there, less than the Legal and Credits pages' 3.5 KB. Lab and test builds only: release builds, their
# layout and heap are unchanged; in shots builds .rodata, .data and .bss move by one page.
# New 3DS too (integrate21 zq48, 2026-11-04): zq47 left UNDERTALE New 1,449 B of late code and its shots build no
# longer linked, so auditfixe's capture builds patched this line by hand. Still shots builds only, never a release.
if(CTRPORT_PLAYER_SHOTS)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x1000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# UNDERTALE New, every build (review decision, integrate21 zq50, 2026-11-04): one more 4 KiB page of late code, Steam,
# GOG and shots builds alike (shots builds then have two). zq49 left UNDERTALE New 591 B there, and utlife B (+704 B)
# and the load bar (<=150 B) come next. UNDERTALE New has no frozen freebytes holes; its .rodata, .data and .bss move by
# one page and the memory it splits between heap and linear memory is 4 KiB smaller. UNDERTALE Old and DELTARUNE are
# unchanged.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "n3ds")
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x1000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# DELTARUNE Old, every build (review decision, integrate21 zq47, 2026-11-04): one more 4 KiB page of late code. zq46 left
# DELTARUNE Old 860 B there, and auditfixe3's overlay reads (z_overlayread.c, R195b/c/d: a save that is there is never
# read as missing, both games) need 2,079 B. DELTARUNE is parked: its .rodata, .data and .bss move by one page and its
# heap is 4 KB smaller; DELTARUNE measurements are re-baselined. UNDERTALE and DELTARUNE New are unchanged.
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds")
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x1000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# dlgtop: main's existing late inputs all have explicit selectors. Its new UT
# console initializer must reach the appended change selector, past the holes.
if(CTRPORT_GAME STREQUAL "undertale")
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "*z_utlife.o" CTRPORT_DLGTOP_EXCLUSION)
    if(CTRPORT_DLGTOP_EXCLUSION EQUAL -1)
        message(FATAL_ERROR "dlgtop: late linker exclusions have no *z_utlife.o; main.o cannot be placed safely")
    endif()
    string(REPLACE "*z_utlife.o" "*source/main.o *z_utlife.o"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
option(CTRPORT_DIMLIGHT_DIAG "Dimlight sandbox/overlay/fault/optical diagnostic (never release)" OFF)
if(CTRPORT_DIMLIGHT_DIAG)
    # Its new sandbox field accessor belongs to the appended dimlight input,
    # past every fixed hole; ordinary sandbox code keeps its original place.
    string(REPLACE "SORT_BY_NAME(EXCLUDE_FILE (" "SORT_BY_NAME(EXCLUDE_FILE (*sandbox_file_system.o "
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# battletouch: libctru's hidTouchRead goes to late code in UNDERTALE only, where battletouch first links it. DELTARUNE
# keeps it where it is (Old's hot-text pin keeps it linked in the library tail, which must not move).
if(CTRPORT_GAME STREQUAL "deltarune")
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "    *libctru.a:hid.o(.text.hidTouchRead)
" battletouch_at)
    if(battletouch_at LESS 0)
        message(FATAL_ERROR "battletouch: late_reserve.ld has no line *libctru.a:hid.o(.text.hidTouchRead)")
    endif()
    string(REPLACE "    *libctru.a:hid.o(.text.hidTouchRead)
" "" CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# Development-only code in the AAC slots (CTRPORT_DEVONLY_AAC above): this build's copy of the script drops the late
# selectors for labguard, the pen log's own pair and bsrdev, so aacout.cmake's dev-only inputs (after every other AAC
# input) take them. Release builds keep the script as it is; every line must be found exactly once.
if(CTRPORT_DEVONLY_AAC)
    foreach(devonly_line
            "    *z_labguard.o(.text .text.* .rodata .rodata.*)\n"
            "    *(.text_battletouchpen)\n"
            "    *(.rodata_battletouchpen)\n"
            "    *(.text_bsrdev)\n"
            "    *(.rodata_bsrdev_in_text)\n"
            "    *(.homesave_code .homesave_const)\n")
        string(REPLACE "${devonly_line}" "" devonly_rest "${CTRPORT_LATE_LD_TEXT}")
        string(LENGTH "${CTRPORT_LATE_LD_TEXT}" devonly_before)
        string(LENGTH "${devonly_rest}" devonly_after)
        string(LENGTH "${devonly_line}" devonly_length)
        math(EXPR devonly_removed "${devonly_before} - ${devonly_after}")
        if(NOT devonly_removed EQUAL devonly_length)
            message(FATAL_ERROR "dev-only AAC: late_reserve.ld must have exactly one line '${devonly_line}'")
        endif()
        if(devonly_line MATCHES "homesave")
            # labguard's CtrHomesave_pauseLab only; the other homesave code stays where it is.
            string(REPLACE "${devonly_line}" "    *(EXCLUDE_FILE (*z_labguard.o) .homesave_code .homesave_const)\n"
                   CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
        else()
            set(CTRPORT_LATE_LD_TEXT "${devonly_rest}")
        endif()
    endforeach()
    # c4 (lead 11-11): P5-1's two inputs exist only with the release source URL (its block below), so in these builds
    # a pad takes their place, the same bytes at the same alignment (.text_p51 0x44 at 4, .rodata.p51_in_text 0x25,
    # both models at dv8u12), and every later late input keeps the release phase. tools/analysis/check_devonly_aac.py
    # checks it against the release map; if P5-1 changes, change these numbers with it.
    set(devonly_p51 "    *(.text_p51)\n    *(.rodata.p51_in_text)\n")
    string(FIND "${CTRPORT_LATE_LD_TEXT}" "${devonly_p51}" devonly_p51_at)
    if(devonly_p51_at LESS 0)
        message(FATAL_ERROR "dev-only AAC: late_reserve.ld has no P5-1 pair to pad")
    endif()
    string(REPLACE "${devonly_p51}" "${devonly_p51}    __devonly_p51_pad_start = ABSOLUTE(.);\n    . = ALIGN(4);\n    . += 0x44;\n    . += 0x25;\n    __devonly_p51_pad_end = ABSOLUTE(.);\n"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
file(WRITE "${CTRPORT_LATE_LD}.new" "${CTRPORT_LATE_LD_TEXT}")
configure_file("${CTRPORT_LATE_LD}.new" "${CTRPORT_LATE_LD}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CTRPORT_LATE_LD_SOURCE}")
set(CTRPORT_SPECS "${CMAKE_CURRENT_BINARY_DIR}/ctrport-3dsx.specs")
get_filename_component(CTRPORT_DKA_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
set(CTRPORT_3DSX_SPECS "${CTRPORT_DKA_BIN}/../arm-none-eabi/lib/3dsx.specs")
if(NOT EXISTS "${CTRPORT_3DSX_SPECS}")
    message(FATAL_ERROR "late_reserve.ld: no 3dsx.specs at ${CTRPORT_3DSX_SPECS}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CTRPORT_3DSX_SPECS}")
find_program(CTRPORT_CYGPATH cygpath)
mark_as_advanced(CTRPORT_CYGPATH)
function(ctrport_native_path out path)
    # GCC expands specs after shell quoting is gone. All generated linker inputs live beside the link
    # command: relative names avoid embedding a player's home/work path in GCC's specs language.
    file(RELATIVE_PATH path "${CMAKE_CURRENT_BINARY_DIR}" "${path}")
    if(path MATCHES "[ %]")
        message(FATAL_ERROR "late_reserve.ld: a specs file can't name '${path}' (space or %)")
    endif()
    set(${out} "${path}" PARENT_SCOPE)
endfunction()
ctrport_native_path(CTRPORT_LATE_LD_NATIVE "${CTRPORT_LATE_LD}")
ctrport_native_path(CTRPORT_SPECS_NATIVE "${CTRPORT_SPECS}")
file(READ "${CTRPORT_3DSX_SPECS}" CTRPORT_SPECS_TEXT)
string(REPLACE "-T 3dsx.ld%s" "-T ${CTRPORT_LATE_LD_NATIVE} -T 3dsx.ld%s" CTRPORT_SPECS_NEW "${CTRPORT_SPECS_TEXT}")
if(CTRPORT_SPECS_NEW STREQUAL CTRPORT_SPECS_TEXT)
    message(FATAL_ERROR "late_reserve.ld: ${CTRPORT_3DSX_SPECS} has no '-T 3dsx.ld%s' to put the script ahead of")
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    include("${CMAKE_CURRENT_SOURCE_DIR}/aacout.cmake")
endif()
# (The specs file is written after the options below: the hot-text alignment pad may add a script to it.)
string(REPLACE "-specs=3dsx.specs" "-specs=${CTRPORT_SPECS_NATIVE}" CTRPORT_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if(CTRPORT_EXE_LINKER_FLAGS STREQUAL CMAKE_EXE_LINKER_FLAGS)
    message(FATAL_ERROR "late_reserve.ld: CMAKE_EXE_LINKER_FLAGS has no -specs=3dsx.specs to replace")
endif()
set(CMAKE_EXE_LINKER_FLAGS "${CTRPORT_EXE_LINKER_FLAGS}")
set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CTRPORT_LATE_LD}" "${CTRPORT_SPECS}")

target_include_directories(ctrport PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${CTRPORT_BUILD_ID_DIR}"
    "${ENGINE_DIR}/src"
    "${ENGINE_DIR}/src/image"
    "${ENGINE_DIR}/src/video"
)
target_include_directories(ctrport SYSTEM PRIVATE
    "${ENGINE_DIR}/vendor/stb/ds"
    "${ENGINE_DIR}/vendor/stb/vorbis"
    "${ENGINE_DIR}/vendor/stb/image"
    "${ENGINE_DIR}/vendor/md5"
    "${ENGINE_DIR}/vendor/base64"
    "${ENGINE_DIR}/vendor/sha1"
    "${ENGINE_DIR}/vendor/miniz"
    "${ENGINE_DIR}/vendor/bzip2"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pvaac/src"
)

target_compile_definitions(ctrport PRIVATE
    PLATFORM_3DS
    ${GAME_WAD_DEFINE}
    ${GAME_DEFINES}
    ${TARGET_DEFINE}
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_STDIO
    CTRPORT_GAME_TITLE="${GAME_TITLE}"
    CTRPORT_GAME_DIR="sdmc:/3ds/${GAME_DIR_NAME}-3ds"
)

target_compile_options(ctrport PRIVATE
    -Wall -Wextra -Wvla
    # devkitARM defines int32_t as long, so the engine's %d formats all warn.
    -Wno-format
    -ffast-math -fno-finite-math-only
    -fomit-frame-pointer
)
# Paths in the build are relative to the repository: __FILE__ (safeMalloc, require*) and the debug information name
# "./engine/src/vm.c" rather than the checkout's absolute path. A build then comes out byte-identical in every
# checkout, and the compiler cache (tools/buildcache) can share objects between them.
# -DCTRPORT_FILE_PREFIX_MAP=OFF keeps the absolute paths, as builds before 2026-10-15 had them.
option(CTRPORT_FILE_PREFIX_MAP "Build with repo-relative paths in __FILE__ and the debug information" ON)
if(CTRPORT_FILE_PREFIX_MAP)
    get_filename_component(CTRPORT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    target_compile_options(ctrport PRIVATE "-ffile-prefix-map=${CTRPORT_REPO_ROOT}=.")
endif()

# Music is decoded with Tremor (integer Vorbis) when the portlibs are installed:
#   pacman -S 3ds-libvorbisidec 3ds-libogg
# Builds the VM with the GML script profiler compiled in. It only runs when
# profile.txt exists in the game folder, but the hooks cost a little even then,
# so release builds leave it out.
set(CTRPORT_DEBUG_RECORD_FAIL_AFTER "" CACHE STRING "Diagnostic: fail recorded-surface growth after this many operations (empty: off)")
if(NOT CTRPORT_DEBUG_RECORD_FAIL_AFTER STREQUAL "")
    if(NOT CTRPORT_DEBUG_RECORD_FAIL_AFTER MATCHES "^(0|[1-9][0-9]*)$" OR CTRPORT_DEBUG_RECORD_FAIL_AFTER GREATER 32767)
        message(FATAL_ERROR "CTRPORT_DEBUG_RECORD_FAIL_AFTER must be an operation count from 0 through 32767")
    endif()
    target_compile_definitions(ctrport PRIVATE "CTRPORT_DEBUG_RECORD_FAIL_AFTER=${CTRPORT_DEBUG_RECORD_FAIL_AFTER}")
endif()
# A performance test that reaches game_end lets the game close (build.sh gameend), to test the exit path; a test
# otherwise ends the scene there. 0 in every other build, where the code is unchanged.
option(CTRPORT_LET_GAME_END "Let game_end close the game during a performance test" OFF)
if(CTRPORT_LET_GAME_END)
    target_compile_definitions(ctrport PRIVATE CTRPORT_LET_GAME_END=1)
    # Its check's scene (bench/gameend_<game>.txt) follows the coverage scenes in the RomFS supplement.
    set(GAMEEND_SCENE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/bench/gameend_${GAME_DIR_NAME}.txt")
    if(EXISTS "${GAMEEND_SCENE_SOURCE}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GAMEEND_SCENE_SOURCE}")
        file(READ "${COVERAGE_SCENE_SOURCE}" coverage_text)
        file(READ "${GAMEEND_SCENE_SOURCE}" gameend_text)
        file(WRITE "${COVERAGE_ROMFS_FILE}" "${coverage_text}
${gameend_text}")
    endif()
else()
    target_compile_definitions(ctrport PRIVATE CTRPORT_LET_GAME_END=0)
endif()
option(CTRPORT_SLOT_DIAGNOSTICS "Log per-room dynamic sprite/TPAG slot lifetimes and heap layout" OFF)
if(CTRPORT_SLOT_DIAGNOSTICS)
    target_compile_definitions(ctrport PRIVATE CTRPORT_SLOT_DIAGNOSTICS)
endif()
# Diagnostic: the state hash dumps every variable's hash at this frame (diag/ctr_state_dump.c sets
# STATE_HASH_DUMP_FRAME before main). Empty: off.
set(CTRPORT_STATE_DUMP_FRAME "" CACHE STRING "State hash: dump every variable's hash at this frame (diagnostic)")
if(NOT CTRPORT_STATE_DUMP_FRAME STREQUAL "")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_state_dump.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_state_dump.c" PROPERTIES
        COMPILE_DEFINITIONS "CTRPORT_STATE_DUMP_FRAME=${CTRPORT_STATE_DUMP_FRAME}")
endif()
# Diagnostic (change layout S2): a per-bank VRAM census at the bench's own VRAM samples (platform/3ds/diag/
# ctr_vram_census.c). Only ctr_benchmark.c's vramSpaceFree calls are renamed to it, so it samples where "vram min"
# comes from.
option(CTRPORT_VRAM_CENSUS "Per-bank VRAM free and largest gap at the bench's samples (diagnostic)" OFF)
if(CTRPORT_VRAM_CENSUS)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_vram_census.c")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c" APPEND PROPERTY COMPILE_DEFINITIONS
        vramSpaceFree=CtrVramCensus_benchFree)
    # layout's transition test driver (settings.ini layout_ops) and its records of targets and largest gaps.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_layout.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CTRPORT_LAYOUT_TEST)
endif()
# Diagnostic: records what the DSP mixes and every channel start/stop (platform/3ds/diag/ctr_audio_capture.c).
# ctr_audio_system.c gets a forced include that routes its NDSP channel calls there; its source is unchanged.
option(CTRPORT_AUDIO_CAPTURE "Record the DSP mix and channel events to the SD card (diagnostic)" OFF)
if(CTRPORT_AUDIO_CAPTURE)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_audio_capture.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_audio_capture_wrap.h")
    # menusound's probe in ctr_audio_capture.c: scripted SELECT presses and every menu sound (audiocap-menu.txt).
    target_link_options(ctrport PRIVATE -Wl,--wrap=hidScanInput,--wrap=hidKeysDown,--wrap=CtrAudio_menuSound)
    if(DEFINED ENV{CTRPORT_MENUSOUND_PROBE}) # console rows have no audiocap-menu.txt: the same three numbers at the build
        target_compile_definitions(ctrport PRIVATE CTRPORT_MENUSOUND_PROBE="$ENV{CTRPORT_MENUSOUND_PROBE}")
    endif()
endif()
# Diagnostic: when the sound and the picture stop on HOME (platform/3ds/diag/ctr_homeaudio_probe.c, change homeaudio).
# ON: real presses; AUTO: timed jumps; FAKE: timed holds with no jump (emulators). Off, its file isn't compiled and no wrap is added.
set(CTRPORT_HOMEAUDIO_PROBE OFF CACHE STRING "HOME stamps: OFF, ON (real presses), AUTO (timed jumps) or FAKE (no jump); never release")
if(CTRPORT_HOMEAUDIO_PROBE)
    if(NOT CTRPORT_GAME STREQUAL "undertale")
        message(FATAL_ERROR "homeaudioprobe is UNDERTALE only")
    endif()
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_homeaudio_probe.c")
    if(NOT CTRPORT_HOMEAUDIO_PROBE STREQUAL "ON")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_homeaudio_probe.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_HOMEAUDIO_AUTO)
    endif()
    if(CTRPORT_HOMEAUDIO_PROBE STREQUAL "FAKE")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_homeaudio_probe.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_HOMEAUDIO_FAKE)
    endif()
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrAudio_homeFreeze,--wrap=CtrAudio_homeThaw,--wrap=gspPresentBuffer,--wrap=hidScanInput,--wrap=aptShouldJumpToHome)
endif()
# Diagnostic: logs an FNV-1a hash of every stream's queued PCM (ctr_audio_system.c), to compare builds sample for sample.
option(CTRPORT_AUDIO_PCM_HASH "Log a hash of every stream's PCM (diagnostic)" OFF)
if(CTRPORT_AUDIO_PCM_HASH)
    target_compile_definitions(ctrport PRIVATE CTR_AUDIO_PCM_HASH)
endif()
option(CTRPORT_LOADAUDIO_CONTROL "Matched loadaudio observer without spare first-buffer preparation (pcmhash only)" OFF)
if(CTRPORT_LOADAUDIO_CONTROL)
    if(NOT CTRPORT_GAME STREQUAL "undertale" OR NOT CTRPORT_AUDIO_PCM_HASH)
        message(FATAL_ERROR "loadaudio control is an UNDERTALE pcmhash diagnostic only")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_LOADAUDIO_CONTROL)
endif()
option(CTRPORT_ADPCM_PROBE "Compare native DSP-ADPCM against its PCM reference before audio startup" OFF)
option(CTRPORT_ADPCM_SFX "Prototype compressed SFX through the existing PCM feeder" OFF)
option(CTRPORT_ADPCM_MUSIC "Prototype native DSP music with explicit Vorbis fallback" OFF)
option(CTRPORT_ADPCM_TEST_UNKNOWN_MTIME "TEST ONLY: accept unknown timestamps on a detected emulator" OFF)
# UNDERTALE's exact-dual-mono sound effects as DSP-ADPCM from one pack beside data.win (adpcm-sfx.dtsp, made by the
# builder with tools/audio/sfxpack.py), checked by content at audio startup (source/ctr_adpcm_pack.inc).
option(CTRPORT_ADPCM_SFXPACK "UNDERTALE mono sound effects as DSP-ADPCM from the builder's pack" OFF)
if(CTRPORT_ADPCM_SFXPACK)
    if(NOT CTRPORT_GAME STREQUAL "undertale" OR CTRPORT_ADPCM_MUSIC OR CTRPORT_ADPCM_SFX OR CTRPORT_ADPCM_PROBE)
        message(FATAL_ERROR "adpcmpack is UNDERTALE only and owns the audio factory wrapper alone")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_ADPCM_SFXPACK)
    target_link_options(ctrport PRIVATE "-Wl,--wrap=CtrAudioSystem_create")
endif()
# TEST ONLY (build word adpcmfault): one linear allocation of the sound feeder or the pack fails (1 tail, 2 seam,
# 3 piece pool, 4 a voice's ring), to prove no compressed payload reaches a raw DSP queue.
set(CTRPORT_ADPCM_FAULT "0" CACHE STRING "TEST ONLY: the feeder/pack allocation that fails (0 none)")
if(NOT CTRPORT_ADPCM_FAULT STREQUAL "0")
    if(NOT CTRPORT_ADPCM_SFXPACK OR NOT CTRPORT_ADPCM_FAULT MATCHES "^[1-4]$")
        message(FATAL_ERROR "adpcmfault needs adpcmpack and one of tail, seam, pool, ring")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_ADPCM_FAULT=${CTRPORT_ADPCM_FAULT})
endif()
if(CTRPORT_ADPCM_TEST_UNKNOWN_MTIME)
    if(NOT CTRPORT_ADPCM_MUSIC)
        message(FATAL_ERROR "adpcmtestmtime requires the adpcmmusic measurement build")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_adpcm_store.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_ADPCM_TEST_UNKNOWN_MTIME=1)
endif()
if(CTRPORT_ADPCM_MUSIC)
    if(CTRPORT_ADPCM_PROBE OR CTRPORT_ADPCM_SFX)
        message(FATAL_ERROR "adpcmmusic owns a separate audio factory wrapper")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_ADPCM_MUSIC)
    target_link_options(ctrport PRIVATE "-Wl,--wrap=CtrAudioSystem_create")
endif()
if(CTRPORT_ADPCM_SFX)
    if(CTRPORT_ADPCM_PROBE)
        message(FATAL_ERROR "adpcmsfx and adpcmprobe own separate audio factory wrappers")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_ADPCM_SFX)
    target_link_options(ctrport PRIVATE "-Wl,--wrap=CtrAudioSystem_create")
endif()
if(CTRPORT_ADPCM_PROBE)
    if(NOT CTRPORT_GAME STREQUAL "undertale" OR NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CTRPORT_PROFILE)
        message(FATAL_ERROR "adpcmprobe is an unprofiled UNDERTALE Release diagnostic")
    endif()
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_adpcm_probe.c")
    target_link_options(ctrport PRIVATE "-Wl,--wrap=CtrAudioSystem_create")
endif()
option(CTRPORT_AUDIO_METRICS "Attribute audio decoder allocations and decode ticks (diagnostic)" OFF)
if(CTRPORT_AUDIO_METRICS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CTRPORT_PROFILE)
        message(FATAL_ERROR "audiometrics retains Release allocator slots; use an unprofiled Release build")
    endif()
    if(CTRPORT_HEAP_PROFILE)
        message(FATAL_ERROR "audiometrics and heap use different allocator hooks")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_AUDIO_METRICS)
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=_malloc_r -Wl,--wrap=_calloc_r -Wl,--wrap=_realloc_r
        -Wl,--wrap=CtrDecoder_openFile -Wl,--wrap=CtrDecoder_openMemory
        -Wl,--wrap=CtrDecoder_read -Wl,--wrap=CtrDecoder_seek -Wl,--wrap=CtrDecoder_close)
endif()
# Diagnostic (natsurf6): logs how every game surface is made, bound and drawn out (platform/3ds/diag/
# ctr_surface_census.inc, included at the end of ctr_renderer.c; main.c installs it). Release builds never have it.
option(CTRPORT_SURFACE_CENSUS "Log how every game surface is made, bound and drawn out (diagnostic)" OFF)
if(CTRPORT_SURFACE_CENSUS)
    target_compile_definitions(ctrport PRIVATE CTRPORT_SURFACE_CENSUS)
endif()
# Diagnostic (change offdraw): instance census and offdraw.txt's kill / hash-skip experiments (platform/3ds/diag/
# ctr_offdraw.c; main.c and engine/src/cli/state_hash.c call it). Release builds never have it.
option(CTRPORT_OFFDRAW "Instance census and off-screen kill experiments (diagnostic)" OFF)
if(CTRPORT_OFFDRAW)
    target_compile_definitions(ctrport PRIVATE CTRPORT_OFFDRAW)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_offdraw.c")
endif()
# natsurf6: native-scale game surfaces of any even size (source/ctr_natsurf6.c, late code). Its outer vtable goes in
# after the other wrappers, from CtrRenderer_loadFlickerProbe, which main.c calls once per load (UNDERTALE), or from
# CtrBtnprompt_setDisplay, main.c's next once-per-load call (DELTARUNE, where cmdlabel wraps the probe). Surfaces whose draws may differ by a native pixel are native-scale too (step 2); the wide
# build reads other class masks from natsurf6.txt for experiments.
# CtrBtnprompt_setDisplay is wrapped once, by boxedge's line below (a symbol can be wrapped once); on DELTARUNE
# natsurf6's wrapper installs its table and then calls boxedge's (Boxedge_setDisplay), which calls the real one.
if(NOT CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_loadFlickerProbe)
endif()
option(CTRPORT_NATSURF6_WIDE "natsurf6: class mask, size and watch from natsurf6.txt (experiments)" OFF)
if(CTRPORT_NATSURF6_WIDE)
    target_compile_definitions(ctrport PRIVATE CTRPORT_NATSURF6_WIDE)
endif()
if(CTRPORT_PROFILE)
    # CTRPORT_RENDER_TIMING times the renderer's own CPU work for the log.
    target_compile_definitions(ctrport PRIVATE ENABLE_VM_GML_PROFILER CTRPORT_RENDER_TIMING)
endif()

# typedaot prototype only: event counters for the CPU census (CTRPORT_TYPED_CENSUS) and the per-frame step/draw
# log (CTRPORT_TYPED_FRAMELOG). Never on a merge path.
option(CTRPORT_TYPED_CENSUS "typedaot census counters (prototype)" OFF)
if(CTRPORT_TYPED_CENSUS)
    target_compile_definitions(ctrport PRIVATE CTRPORT_TYPED_CENSUS)
endif()
# spdcount (change creddodge, build.sh "spdcount"): per-frame [spd] counts of the speed/direction memo. Test only.
option(CTRPORT_SPDCOUNT "creddodge: per-frame speed/direction memo counts in log.txt (test builds only)" OFF)
if(CTRPORT_SPDCOUNT)
    target_compile_definitions(ctrport PRIVATE CTRPORT_SPDCOUNT)
endif()
option(CTRPORT_TYPED_FRAMELOG "typedaot: per-frame step/draw log for matched-frame A/B (prototype)" OFF)
if(CTRPORT_TYPED_FRAMELOG)
    target_compile_definitions(ctrport PRIVATE CTRPORT_TYPED_FRAMELOG)
endif()

# Typed AOT regions (tools/aot/typed.py): the generated code came from prepare.py --typed and has
# an aot_typed.c, compiled with CTRPORT_TYPED_AOT_OPTIONS below. CTRPORT_TYPED_STATS counts each region's entries,
# guard failures, hand-overs and exits for the log (the every-entry check); CTRPORT_TYPED_STRESS=<n> fails the guards
# of one region entry in n and hands over at one block start in n (a test of the fallback paths).
option(CTRPORT_TYPED_AOT "Compile the typed regions (aot_typed.c) of code from prepare.py --typed" OFF)
option(CTRPORT_TYPED_STATS "Typed AOT: count each region's entries, guard failures, hand-overs and exits" OFF)
set(CTRPORT_TYPED_STRESS "" CACHE STRING "Typed AOT: fail one region entry in n and hand over at one block in n (empty: off)")
# Each double operation in a typed region rounds once, in bytecode order, as the interpreter's handlers do: no fast math,
# no contraction (vm_aot_typed.h refuses to compile with __FAST_MATH__).
set(CTRPORT_TYPED_AOT_OPTIONS "-fno-schedule-insns;-fno-var-tracking;-fno-var-tracking-assignments;-fno-fast-math;-ffp-contract=off")
# The definitions go only on the files that read them: vm.c (aot/vm_aot_bridge.inc), main.c (the stats log) and the
# generated aot_typed.c (below and in aot/aot_modules.cmake). Everything else compiles as in a build without typed
# regions, so the compiler cache shares it.
set(CTRPORT_TYPED_DEFS "")
if(CTRPORT_TYPED_AOT)
    if(CTRPORT_TYPED_STATS)
        list(APPEND CTRPORT_TYPED_DEFS CTRPORT_TYPED_STATS)
    endif()
    if(CTRPORT_TYPED_STRESS)
        list(APPEND CTRPORT_TYPED_DEFS CTRPORT_TYPED_STRESS=${CTRPORT_TYPED_STRESS})
    endif()
    set_property(SOURCE "${ENGINE_DIR}/src/vm_aot_typed_runtime.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TYPED_AOT ${CTRPORT_TYPED_DEFS})
    if(CTRPORT_TYPED_STATS)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TYPED_STATS)
    endif()
elseif(CTRPORT_TYPED_STATS OR CTRPORT_TYPED_STRESS)
    message(FATAL_ERROR "CTRPORT_TYPED_STATS and CTRPORT_TYPED_STRESS need CTRPORT_TYPED_AOT (build.sh typed)")
endif()

# ctr_apt_trace.c logs the libctru and citro3d calls on the way to the HOME Menu.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=GSPGPU_SaveVramSysArea,--wrap=GSPGPU_ImportDisplayCaptureInfo,--wrap=GSPGPU_ReleaseRight,--wrap=GSPGPU_AcquireRight,--wrap=gxCmdQueueWait)
# ctr_watchdog.c records which thread holds newlib's heap lock for hang reports.
target_link_options(ctrport PRIVATE -Wl,--wrap=__malloc_lock,--wrap=__malloc_unlock)
# spdmemo (change creddodge): z_spdmemo.c keeps the last two speed/direction results in front of the engine function.
target_link_options(ctrport PRIVATE -Wl,--wrap=Instance_computeSpeedFromComponents)
# ofpoll (change creddodge): ctr_open_files.c's end looks at the keepers without the lock before CtrOpenFiles_poll
# takes it, so a frame with nothing to shed never waits on a thread that holds it.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrOpenFiles_poll)
# glyphsafe (change auditfixe, DEEP-02): z_glyphsafe.c draws argui's text (first-boot hint, FPS counter) only when every
# font page and tile it reads is resident.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrFps_drawText,--wrap=CtrFpsbox_drawDigits)
# inisafe (change auditfixe, DEEP-01): z_inisafe.c points the builtins that change a file at stubs that drop the INI
# cache first, once VMBuiltins_registerAll has registered them.
target_link_options(ctrport PRIVATE -Wl,--wrap=VMBuiltins_registerAll)
# dndrel (change auditfixe, SCOURS3-002): z_dndrel.c points action_set_motion at a builtin that adds the motion (as
# motion_add) when the D&D Relative flag is set, once Hitbox_registerAll (UNDERTALE's last builtin registration) ends.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Hitbox_registerAll)
endif()
# alphabyte (change gpuburst C(i)): the two draws other files call directly, z_smalltext.c's Smalltext_drawTexture and
# z_utkill_waver.c's CtrUtk_waverRows, reach them through ctr_renderer.c's draw alpha stubs (ABQ), as the vtable's do.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Smalltext_drawTexture,--wrap=CtrUtk_waverRows)
endif()
# bakkeep (change auditfixe, SCOURSOL2-001): z_bakkeep.c recovers a save stranded in its .bak (a failed write) before
# ctr_file_system.c's rename or delete forgets the backup, and keeps the .bak if that recovery fails.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrSafeSave_forgetBackup)
# symkeep (change auditfixe, SCOURT5-003): z_symkeep.c gives symmetric halving's copy array (CtrRenderer.symTex) its
# whole capacity at a room change while it is empty, so a batch never draws a copy through a block that moved; where
# the array still grows (R193), CtrArray_add draws the renderer's batches first, and the display caps link names
# the renderer.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_chainFontVerdictsSymverdict,--wrap=CtrArray_add,--wrap=CtrRenderer_chainCapsItemverdict)
endif()
# credfail (change auditfixe, DEEP-11): z_credfail.c says on the Credits page when the build has no credits to show
# (romfs:/credits.txt missing, empty or unreadable) and that B goes back, once ctr_legal.c's CtrLegalui_open drew it.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLegalui_open)
endif()
# sleepflush (change auditfixe, SCOURS2-005): z_sleepflush.c adds an APT hook at the first CtrFileSystem_create that
# flushes the save queue before the console sleeps or the HOME Menu opens.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrFileSystem_create,--wrap=CtrFileSystem_destroy)
# wrapfix (change auditfixe, SCOURT2-001): z_wrapfix.c points draw_text_ext at a copy whose wrap keeps every letter of a
# word wider than the width and runs it past the width, as GameMaker 1.4 on PC does, once UtKill_registerAll (in
# VM_create's builtin chain, called from z_utflames.c) ends.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=UtKill_registerAll)
endif()
# overlayread (change auditfixe3, SCOURSOL3-002): z_overlayread.c gives every overlay file system whole-file readers
# that fail unless they read the whole file (ctr_file_system.c's and main.c's OverlayFileSystem_create calls).
target_link_options(ctrport PRIVATE -Wl,--wrap=OverlayFileSystem_create)
# Setup notices (polish audit, 11-06): the layout and the advice of a notice, main.c and the audio frozen.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrNotice_first,--wrap=CtrNotice_missing)
# credclip (change auditfixe, CREDPOP-001): z_credclip.c keeps the Credits roll's rows outside its window black after
# each line ctr_legal.c draws through the menu font's painter, so lines slide in and out instead of popping.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrOptmenu_text)
endif()
# scanout (site 2): a Game screen change presents each screen's new picture with its new format at a vblank, no refresh
# of a half-written or wrong-format buffer. The move runs from keepBottom's releaseMirrors call (the end of
# ctr_renderer.c), so no code before it changes.
# The wrapper tells keepBottom's call by its return address, so every UNDERTALE link is checked: all keepBottom's
# calls in the window, no new one, keepBottom longer than the window; the ELF is deleted otherwise (R428-1).
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_releaseMirrors)
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/check_scanout_hook.cmake")
    add_custom_command(TARGET ctrport POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:ctrport>" "-DNM=${CTRPORT_DKA_BIN}/arm-none-eabi-nm"
                "-DOBJDUMP=${CTRPORT_DKA_BIN}/arm-none-eabi-objdump"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" -DCALLS=3
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_scanout_hook.cmake"
        VERBATIM)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_credclip.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Timestamp logs from before libctru service/heap setup, retaining its r0 argument.
target_link_options(ctrport PRIVATE -Wl,--wrap=__libctru_init)
# ctr_frame_pacer.c holds citro3d's screen flips for the vblank that ends each frame's slot.
target_link_options(ctrport PRIVATE -Wl,--wrap=gfxScreenSwapBuffers)

# Opt-in pacer experiment. Observation is identical on R0 and R2+R3a arms.
# Bench results drain on every normal quit, before filesystem teardown.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrExit_begin)
option(CTRPORT_PACER_OBSERVE "Observe benchmark presentation and cushion latency" OFF)
option(CTRPORT_PACER_R2R3A "Prototype: margin recovery and per-room cushion reactivation" OFF)
if(CTRPORT_PACER_OBSERVE OR CTRPORT_PACER_R2R3A)
    message(FATAL_ERROR "Adaptive pacer cushion retired by owner: use constant low input delay")
endif()
if(NOT (CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds"))
    # The bench's held-frame lines (ctr_benchheld.inc, change stallfix): read at benchshownRead's flip read, reported at
    # each scene's result. The retired cushion prototype cannot wrap this path.
    # DELTARUNE Old builds link none of it (its +2 KB of late text did not fit there, integrate24, 10-28): that build
    # stays byte-identical but for its build id, until a DELTARUNE Old late page takes it.
    target_compile_definitions(ctrport PRIVATE CTRPORT_BENCH_HELD=1)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPacer_getFlipTotals,--wrap=CtrPacer_logLevels)
endif()

# change flowerhw D'+E (DR New tower cuts, vine_new_retain.inc): its tower CPU profile (build.sh ... towerprof),
# [tprof] lines with the tower draws' CPU time per frame. Test builds only.
option(CTRPORT_FLOWERHW_TPROF "Test-only: tower draw CPU profile (flowerhw)" OFF)
if(CTRPORT_FLOWERHW_TPROF)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FLOWERHW_TPROF)
endif()
# Its forced branches (build.sh ... towerforce1 / towerforce2): the tower strips without coverage (1), and with every
# cut 7d draw refused as well (2), to prove towerEmit's other branches give the same pictures (vine_new_retain.inc).
set(CTRPORT_FLOWERHW_FORCE "0" CACHE STRING "Test-only: tower strips' forced branches, 0 off, 1 or 2 (flowerhw)")
if(NOT CTRPORT_FLOWERHW_FORCE STREQUAL "0")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FLOWERHW_FORCE=${CTRPORT_FLOWERHW_FORCE})
endif()

option(CTRPORT_PACER_CSV_DIAG "Test-only sweep CSV filesystem bypass; never a release" OFF)
if(CTRPORT_PACER_CSV_DIAG)
    if(NOT GAME_DIR_NAME STREQUAL "undertale")
        message(FATAL_ERROR "pacer-csv-diag is an UNDERTALE-only diagnostic")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_PACER_CSV_DIAG=1)
    target_link_options(ctrport PRIVATE -Wl,--wrap=stat,--wrap=fopen,--wrap=fputs,--wrap=fputc,--wrap=fclose)
else()
    # Log-only timing rows; unchanged benchmark/stdio bodies, late policy code.
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrExitFiles_openLog,--wrap=stat,--wrap=fopen,--wrap=fputs,--wrap=fputc,--wrap=fclose
        -Wl,--wrap=fflush,--wrap=fprintf,--wrap=fwrite,--wrap=mkdir)
    if(GAME_DIR_NAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds")
        # Restore round-13 TLS GOT/literal bytes after adding the I/O wrappers.
        # Proven by pacer-got-link-probe-r2; strict old-byte gates stay required.
        target_link_options(ctrport PRIVATE -Wl,--hash-size=16411)
    endif()
endif()
# benchfix: a tour room's warm-up starts after a warp frame longer than the warm-up (ctr_benchmark.c's end).
target_link_options(ctrport PRIVATE -Wl,--wrap=benchfidBeginFrame)
# benchfix: a bench's stat of chapter 1's data.win (the sweep start's chapterAvailable(1)) answers from one asked on the
# bench's first frame (ctr_benchmark.c's end; newlib's stat() is the one caller of _stat_r).
target_link_options(ctrport PRIVATE -Wl,--wrap=_stat_r)
# benchfix: the held-frame lines' gpuwait also counts CtrGpu_waitDraws (ctr_benchmark.c's end), wherever the held lines
# are built (ctr_benchheld.inc's condition: not pacer-observe, not DELTARUNE Old). Only ctr_renderer.c's one call moves.
if(NOT CTRPORT_PACER_OBSERVE AND NOT (CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds"))
    target_compile_definitions(ctrport PRIVATE CTRPORT_BENCHFIX_DRAWWAIT=1)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrGpu_waitDraws)
endif()
# benchfix probe (build.sh ... bfprobe; never a release): times the main thread's waits in a sweep (ctr_benchmark.c's
# end). Only call targets change; without the word nothing here is compiled or wrapped.
option(CTRPORT_BENCHFIX_PROBE "Main-thread wait probe for sweep starts (benchfix; diagnostics only)" OFF)
if(CTRPORT_BENCHFIX_PROBE)
    if(CTRPORT_SPIKEREC)
        message(FATAL_ERROR "bfprobe and spikerec both wrap FSFILE_Read")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_watchdog.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BENCHFIX_PROBE)
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrLog_beginConsole,--wrap=LightLock_Lock,--wrap=RecursiveLock_Lock,--wrap=LightEvent_Wait
        -Wl,--wrap=CondVar_Wait,--wrap=CondVar_WaitTimeout
        -Wl,--wrap=LightEvent_WaitTimeout,--wrap=svcSleepThread,--wrap=svcWaitSynchronization
        -Wl,--wrap=svcWaitSynchronizationN,--wrap=FSUSER_OpenFile,--wrap=FSUSER_OpenFileDirectly
        -Wl,--wrap=FSUSER_OpenDirectory,--wrap=FSUSER_CreateDirectory,--wrap=FSUSER_DeleteFile,--wrap=FSUSER_RenameFile
        -Wl,--wrap=FSFILE_Read,--wrap=FSFILE_Write,--wrap=FSFILE_GetSize,--wrap=FSFILE_Flush,--wrap=FSFILE_Close
        -Wl,--wrap=FSDIR_Read)
endif()

# Every linear memory call, libctru's and citro3d's included, takes the lock in source/ctr_memory.c: libctru's linear
# allocator has none, and several threads use it. Linear and VRAM frees made while the GPU is busy are held back
# there until it is done with what it was running.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=linearAlloc,--wrap=linearMemAlign,--wrap=linearRealloc,--wrap=linearGetSize,--wrap=linearFree,--wrap=linearSpaceFree
    -Wl,--wrap=vramAlloc,--wrap=vramAllocAt,--wrap=vramMemAlign,--wrap=vramMemAlignAt,--wrap=vramFree,--wrap=vramSpaceFree)
# pcmwait (both games): a linear allocation that fails frees the sound blocks waiting for the DSP that no channel
# reads any more and tries once more (ctr_audio_system.c's end). Wrapped, so ctr_memory_lock.c's entry keeps its code;
# only their call target changes.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrMemory_linearAllocOnce)
# pcmwait (both games): every channel clear and reset notes the NDSP frame callback count (the DSP takes the cut a
# few frames later, and a block it may still read waits until then), and the uta callback's set/unset and ndspExit
# keep the DSP's on/off state in a flag (no check reads the audio system, which a worker can outlive). Wrapped, so
# their callers keep their code; only the call target changes.
target_link_options(ctrport PRIVATE -Wl,--wrap=ndspChnReset,--wrap=ndspChnWaveBufClear,--wrap=ndspSetCallback,--wrap=ndspExit)
# Azahar only: every new VRAM block is touched (source/ctr_vram_touch.c, late code). Wrapped, so the allocator entries
# in source/ctr_memory_lock.c and the renderer's tile copies stay as they were.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=CtrMemory_vramAllocOnce,--wrap=CtrMemory_vramAllocAtOnce,--wrap=CtrMemory_vramMemAlignOnce,--wrap=CtrMemory_vramMemAlignAtOnce
    -Wl,--wrap=CtrTextures_addMirror)
# malloc, calloc, realloc and free go through source/ctr_memory.c (the heap profiler's wrappers replace these and call
# it): when the heap can't satisfy an allocation, the texture cache drops its packed copies (game thread only), and
# then linear memory takes it. Threads free newlib's per-thread number caches when they end. newlib's own _free_r is
# wrapped too: fclose frees setvbuf's buffer, which came from malloc, with it.
target_link_options(ctrport PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=_free_r,--wrap=threadCreate)
target_link_options(ctrport PRIVATE -Wl,--wrap=gspInit)
# source/main.c: an abort after such an allocation failed anyway shows an out-of-memory screen.
target_link_options(ctrport PRIVATE -Wl,--wrap=abort)
# R216b4 (change auditfixe2): CtrLab_fail and CtrLab_aimHost's callers in other files (main.c, z_argcrash.c,
# z_leavekeep.c) go through ctr_lab.c's end, which reads the save guard's clock before the claim and goes on with the
# same arguments and return address. Their code and ctr_lab.c's old code keep every instruction. UNDERTALE only, as the
# whole R216 net (R216d, source/ctr_r216_ut.h): DELTARUNE Old's late text has no room for it (DELTARUNE round).
if(GAME_WAD_DEFINE STREQUAL "ENABLE_WAD16")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLab_fail,--wrap=CtrLab_aimHost)
endif()
# source/memory_report.c (heapfrag): each heap layout report also gives the largest block malloc can hand out and the
# largest linear piece, and the out-of-memory screen records the top chunk at the failure; the log writer thread warms
# its number caches before it starts. Wrapped, so ctr_memory.c's code and its callers' stay as they were.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrMemory_logHeapLayout,--wrap=CtrMemory_emergencyFree,--wrap=CtrLab_runThread)
# nativemac: data versions added after revision 1 (ctr_data_version.c's end). The one wrap of CtrDataVersion_check;
# its chain's links and their order are in source/ctr_wrap_chain.h.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDataVersion_check)
# utrect (change uthunt; UNDERTALE only): z_utrect.c registers the filled-rectangle script's builtin after VM_create's
# other registrations through this wrap, so vm.c changes only that call's target.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Utgap_registerAll)
endif()
# source/z_chapterleak.c (chapterleak): the chapter switch gives back the renderer's effect textures (ctr_renderer.c's
# late tail) before the renderer goes. Wrapped, so ctr_chapters.c and main.c stay as they were.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrChapters_unloadGame)
# source/z_itemverdict.c (smartcut): Smart smoothing's item verdicts from the build, read right after the font verdicts
# at a room change and at the options menu: the last link of the CtrRenderer_loadFontVerdicts and
# CtrRenderer_getDisplayCaps chains (source/ctr_wrap_chain.h; each is wrapped once below, per game), so z_roomload.c
# stays as it was; ctr_renderer.c gains a data table. All of it is late code, and its formats are named late constants
# (no printf folding, no switch tables in .rodata).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_itemverdict.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# source/z_loadscreen.c (loadscreen): the loading screens' thread follows the load through these. Wrapped, so main.c
# stays as it was. Compiled like ctr_screens.c (its strings are named arrays in the late code), and for size. It also
# heads the CtrDataVersion_check and CtrWatchdog_stop wrap chains (source/ctr_wrap_chain.h), wrapped once elsewhere.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=C3D_Init,--wrap=CtrPack_setDataWinPath,--wrap=DataWin_parse
    -Wl,--wrap=CtrPackScreen_run,--wrap=VM_create,--wrap=SandboxFileSystem_create,--wrap=CtrPacer_loadStress
    -Wl,--wrap=Runner_initFirstRoom)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_loadscreen.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# The game screen and other screen settings (screens-port), as late code: source/ctr_screens.c and the
# late code at the ends of ctr_renderer.c, ctr_frame_pacer.c and main.c. Wrapped, so main's settings, menu, renderer and
# pacer code keeps its addresses, its magnifier compiled but no longer selectable.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=CtrDisplay_load,--wrap=CtrDisplay_override,--wrap=CtrDisplay_saveEdits,--wrap=CtrMenu_open,--wrap=CtrMenu_update
    -Wl,--wrap=CtrRenderer_create,--wrap=CtrRenderer_setDisplay,--wrap=CtrRenderer_yieldBottomScreen,--wrap=CtrRenderer_leaveForExit
    -Wl,--wrap=CtrPacer_onVBlank,--wrap=C3D_FrameCounter,--wrap=gspWaitForEvent,--wrap=gspSetEventCallback,--wrap=CtrPacer_setGameScreen,--wrap=CtrPacer_frameEnding
    -Wl,--wrap=CtrDisplay_topModeName,--wrap=CtrDisplay_stereoModeName,--wrap=CtrDisplay_bottomModeName,--wrap=CtrLog_emergencyKeepLink)
# The options menu holds every sound while it is open (menupause), as late code at the
# end of source/ctr_audio_system.c: the menu's open wrap above calls CtrLegal_close first (its only caller outside
# ctr_legal.c), and every close goes through CtrMenu_close. Wrapped, so main's and the menu's code keep their addresses.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLegal_close,--wrap=CtrMenu_close)
# SCOURSOL2-004 (change auditfixe2, 11-04): a held key on the Legal and Credits pages keeps repeating past 255 frames,
# through a wrap at ctr_legal.c's end (.text_legalheld), so CtrLegalui_update keeps its size (RULES 3adf7ac34).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLegalui_update)
# The game folder's names from one listing, settings.ini read once and written once (gamedir, smoothux and gamedir2):
# source/z_gamedir.c, a layer over the card's device that main()'s first call (CtrOpenFiles_track, wrapped) installs
# over ctr_open_files' own; CtrLog_openFile, wrapped, logs what the listing found. A missing name in the game folder
# is answered without the card, which took about 200-400 ms a lookup on the Old test consoles; settings.ini's writes
# go to a copy in memory that a worker thread writes once, crash-safe, after they go quiet. All late code
# (late_reserve.ld), compiled for size like z_sdmigrate.c.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrOpenFiles_track,--wrap=CtrLog_openFile)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_gamedir.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# Backlights the app powered off come back on for HOME, sleep, closing and the exit (menupause),
# as late code at the end of source/ctr_screens.c: every power-off and power-on is wrapped to keep which
# screens are dark, and gfxExit to switch them on as the app leaves.
target_link_options(ctrport PRIVATE -Wl,--wrap=GSPLCD_PowerOffBacklight,--wrap=GSPLCD_PowerOnBacklight,--wrap=gfxExit)
# Shared fatal-exit fix: invalidate the software painter before libctru closes
# graphics. Preserve the existing backlight wrapper and redirect its one call.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_screens.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "__real_gfxExit=CtrFatalGfx_exit")
endif()
# Luma3DS's Rosalina menu with the other screen Off (change rosalina2, maintainer 2026-10-25): source/ctr_rosalina.c, all late
# code, lights the bottom backlight while the player's buttons form Rosalina's combo. gspLcdExit is wrapped to start
# its watcher at the first backlight change that leaves the bottom screen dark. Compiled like ctr_screens.c (named
# constants, no folded printf or switch tables). Release builds log nothing from it; CTRPORT_ROSALINA_PROBE (build.sh
# "rosaprobe") adds its pause and switch log lines, and CTRPORT_ROSALINA_SIM (an Azahar test) also fakes Luma.
target_link_options(ctrport PRIVATE -Wl,--wrap=gspLcdExit)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# exitpanic rosacore (10-30): the watcher's threadCreate calls go to z_leavekeep.c's CtrRosalina_threadCreate (the
# same call, another target; no instruction in ctr_rosalina.c changes). On a core-1 ask with the app's CPU time limit
# still 0 it returns NULL as the kernel's refusal (E0A01839) would, without the refused create: that create looks
# to keep a thread count past the process's end, one per run (docs/3ds-hardware.md, "Thread slots lost across 3dsx
# launches"). The watcher then falls back to -2, as it did after the refusal.
# R216b5 (change auditfixe2, c26rev2 R216b4 findings 2-3; UNDERTALE only, DELTARUNE Old's late text is full): the
# create goes through ctr_rosalina.c's CtrRosalina_threadPub first (it publishes the watcher's handle atomically and
# makes no watcher once a crash screen has stopped it), and the file's CtrScreens_bottomDark calls go to its
# CtrRosalina_dark (the bottom counts as lit once a crash screen has stopped the watcher); z_argcrash.c's
# CtrRosalina_quiet call goes to its CtrArgcrash_quiet, which asks the pending screen's paint again while the watcher
# is not yet quiet. Each is the same call to another target; no instruction of an existing body changes. (R216d: the
# watcher's thread then runs CtrRosalina_watchRun, which lights the bottom after a crash stop; DELTARUNE's create is
# CtrRosalina_threadCreate as before R216.)
if(GAME_WAD_DEFINE STREQUAL "ENABLE_WAD16")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "threadCreate=CtrRosalina_threadPub" "CtrScreens_bottomDark=CtrRosalina_dark")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_argcrash.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrRosalina_quiet=CtrArgcrash_quiet")
else()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "threadCreate=CtrRosalina_threadCreate")
endif()
# rosafreeze3 bug 2 (11-05; source/z_rosabl.c says why): no bottom power-off while Rosalina's combo is forming or
# just was. The watcher's sleep, the HID shared memory it reads and its gsp::Lcd send (R217d: its power-off after the
# hold and power-on after that carry out the app's newest bottom request) go to z_rosabl.c in ctr_rosalina.c, and the
# real power-off and power-on calls go there in ctr_screens.c: the same calls and loads, other targets, so no
# instruction in either file changes (on UNDERTALE Old their late code can't grow: the freebytes H1 holes after it pad
# 0 B). z_rosabl.c is appended late code, compiled like ctr_rosalina.c.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrRosalina_guardSleep=CtrDimlight_rosalinaSleep" "hidSharedMem=CtrRosabl_hid" "svcSendSyncRequest=CtrRosabl_send")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_screens.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_GSPLCD_PowerOffBacklight=CtrRosabl_powerOff" "__real_GSPLCD_PowerOnBacklight=CtrRosabl_powerOn")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosabl.c" PROPERTIES COMPILE_OPTIONS
        "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
endif()
# keep-lit (change dlglight, 11-07; source/z_keeplit.c says why): Other screen Off with dialogue on the bottom keeps that
# screen lit. The same calls, other targets, so no instruction in either file changes. ctr_renderer.c checks the offsets.
if(CTRPORT_GAME STREQUAL "undertale")
    set(CTRPORT_KEEPLIT_OFFSETS "CTRPORT_KEEPLIT_DIRTY_AT=12828" "CTRPORT_KEEPLIT_BACKLIGHT_AT=12829")
    # layout F8: ctr_screens.c's question goes through CtrLayout_keepLit (z_layout.c), which asks keep-lit's and adds
    # the top screen while a refused borrow of the bottom screen holds the game there.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_screens.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrArgui_keepLit=CtrLayout_keepLit")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrArgui_keepLit=CtrKeeplit_keepLit" "CtrRenderer_chainDisplayInputlate=CtrKeeplit_setDisplay" ${CTRPORT_KEEPLIT_OFFSETS})
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_keeplit.c" APPEND PROPERTY COMPILE_DEFINITIONS ${CTRPORT_KEEPLIT_OFFSETS})
    # layout's test builds log the bottom backlight's request from the same offset (z_layout.c, the vramcensus build only).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_layout.c" APPEND PROPERTY COMPILE_DEFINITIONS ${CTRPORT_KEEPLIT_OFFSETS})
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_keeplit.c" PROPERTIES COMPILE_OPTIONS "-Os")
endif()

# dimlight: retain existing power/keep-lit bodies and retarget their callers.
# The early loader records settings after optmenu and all CLI overrides. Only
# its calls use these new names; the existing parser and menu wrappers stay put.
if(CTRPORT_GAME STREQUAL "undertale")
    # Cold LCD opens must not wait for session capacity during restore/exit.
    # The wrapper passes every other service straight to libctru.
    target_link_options(ctrport PRIVATE -Wl,--wrap=srvGetServiceHandle,--wrap=gspLcdInit)
    # Rosalina already owns __wrap_gspLcdExit. Keep its body/start-watcher
    # behavior and retarget only its real-release continuation through our guard
    # (below: in development builds through battlesplit-rework's log first).
    # battlesplit-rework (z_dimlight.c's end, 11-10 4(i), c26rev5 F3): in development builds only (no release
    # source URL), the backlight requests are logged through dimlight's dev wrappers; release builds make the calls as
    # before.
    if(CTRPORT_RELEASE_SOURCE_URL)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "__real_gspLcdExit=CtrDimlight_lcdExit")
        target_compile_definitions(ctrport PRIVATE
            "GSPLCD_PowerOnBacklight=CtrDimlight_powerOn"
            "GSPLCD_PowerOffBacklight=CtrDimlight_powerOff" "gfxExit=CtrDimlight_gfxExit")
    else()
        target_compile_definitions(ctrport PRIVATE
            "GSPLCD_PowerOnBacklight=CtrDimlight_powerOnBsr"
            "GSPLCD_PowerOffBacklight=CtrDimlight_powerOffBsr" "gfxExit=CtrDimlight_gfxExit")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "__real_gspLcdExit=CtrDimlight_lcdExitBsr")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dimlight.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplitbsr.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BSR_LOG)
        # The renderer's LCD opens go through CtrBattlesplit_lcdInitBsr (z_battlesplitbsr.c), which logs a failed one
        # (the same call; dimlight's wrapper still answers it).
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "gspLcdInit=CtrBattlesplit_lcdInitBsr")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrKeeplit_setDisplay=CtrDimlight_setDisplay")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "fpsboxFlushConsoles=CtrDimlight_flushConsoles")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_loadscreen.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrDisplay_load=CtrDimlight_load" "CtrDisplay_override=CtrDimlight_override"
        "gspLcdInit=CtrDimlight_topOnInit")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_leavekeep.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrAptTrace_exit=CtrDimlight_exit")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrScanout_noticeClear=CtrDimlight_noticeClear" "gspLcdInit=CtrDimlight_emergencyInit")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_screens.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "gspLcdInit=CtrDimlight_backlightsInit")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_dimlight.c" PROPERTIES COMPILE_OPTIONS "-Os")
endif()
# oflog (first update, change bootmask; source/z_oflog.c says why): a room change's open-files line takes the handle
# census only where the kernel gives a believable count, which every console does; on Azahar the census was 61 ms of
# every room change. Late code only.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrOpenFiles_log)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_oflog.c" PROPERTIES COMPILE_OPTIONS "-Os")
endif()
# stackfloor (change stackfloor, 11-07; source/z_stackfloor.c says why): no thread stack under 8 KiB. ctr_memory.c's
# threadCreate wrap calls the floor instead of CtrExitGsp_threadCreate: the same call, another target, so no instruction
# there changes; the floor is late code, appended last.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_memory.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrExitGsp_threadCreate=CtrStackfloor_threadCreate")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_stackfloor.c" PROPERTIES COMPILE_OPTIONS "-Os")
endif()
# bootahead (first update, changes bootmask and bootahead2; source/z_bootahead.c says why): the menu's song and its level's
# tiles behind the loading bar. Boot-only code, in the AAC slot (aacout.cmake), run once a boot.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_bootahead.c" PROPERTIES COMPILE_OPTIONS "-Os")
endif()
# exitpanic argbind (10-31; DELTARUNE only, source/z_argbind.c says why): argui's texture cache, audio system and asset
# indices follow a chapter switch from its first frame. ctr_renderer.c's CtrTextures_init call and main.c's
# CtrBench_sessionStartedLate call go to z_argbind.c (the same calls, another target; no other instruction in either
# file changes), which make them and then set the new load's values, as argui's prefetch wrap does one frame later.
# main.c's CtrChapters_unloadGame call goes there too: it empties argui's cache and audio pointers before the old load
# is freed (the new renderer's screen set-up swaps buffers before its texture set-up; c26rev r65 F1).
if(CTRPORT_GAME STREQUAL "deltarune")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrTextures_init=CtrArgbind_texturesInit")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrBench_sessionStartedLate=CtrArgbind_sessionStartedLate" "CtrChapters_unloadGame=CtrArgbind_unloadGame")
endif()
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_argbind.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
option(CTRPORT_ROSALINA_SIM "Test only: run the Rosalina watcher without Luma, on simulated buttons and a pause" OFF)
option(CTRPORT_ROSALINA_PROBE "Test only: log each pause and switch the Rosalina watcher sees" OFF)
if(CTRPORT_ROSALINA_PROBE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALINA_PROBE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosabl.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALINA_PROBE)
endif()
# UT-001 on UNDERTALE Old (change rosaport; source/z_rosalow.c says why): the app's core 1 limit goes down to 25 while the
# player's buttons form Rosalina's combo. z_rosabl.c's sleep goes to z_rosalow.c, the session's input start is wrapped, so
# and the watcher's thread create, so no existing instruction changes; ctr_rosalina.c gets one late function at its end (CTRPORT_ROSALOW), which starts the
# watcher at session start whatever the bottom backlight does. The watcher then passes at HID's 234 a second, so
# z_rosabl.c's 150 ms of grace takes 36 passes. z_rosalow.c is appended late code, compiled like ctr_rosalina.c.
# CTRPORT_ROSALOW_TEST (build.sh rosalow=arm or rosalow=script, tests only): the arm byte, or Azahar's scripted buttons.
set(CTRPORT_ROSALOW_TEST "" CACHE STRING "Test only: arm or script (source/z_rosalow.c)")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrInput_start -Wl,--wrap=CtrRosalina_threadCreate)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosabl.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrRosalina_guardSleep=CtrRosalow_wait" "ROSABL_GRACE_POLLS=36")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALOW)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosalow.c" PROPERTIES COMPILE_OPTIONS
        "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
    if(CTRPORT_ROSALINA_PROBE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosalow.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALINA_PROBE)
    endif()
    if(CTRPORT_ROSALOW_TEST STREQUAL "arm")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosalow.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALOW_ARM)
    elseif(CTRPORT_ROSALOW_TEST STREQUAL "script")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosalow.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALOW_SCRIPT)
    endif()
endif()
if(CTRPORT_ROSALINA_SIM)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALINA_SIM)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_rosabl.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CTRPORT_ROSALINA_SIM CTRPORT_ROSALINA_PROBE)
    # When the simulated pause starts after the watcher does, and how long every app thread stays stopped (ms).
    set(CTRPORT_ROSALINA_SIM_AT_MS 3000 CACHE STRING "Rosalina sim: pause start after the watcher starts (ms)")
    set(CTRPORT_ROSALINA_SIM_PAUSE_MS 12000 CACHE STRING "Rosalina sim: pause length (ms)")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CTRPORT_ROSALINA_SIM_AT_MS=${CTRPORT_ROSALINA_SIM_AT_MS} CTRPORT_ROSALINA_SIM_PAUSE_MS=${CTRPORT_ROSALINA_SIM_PAUSE_MS})
endif()
# The release options menu (optmenu), all late code in source/ctr_optmenu.c: argui's menu and
# settings entry points are wrapped, so main.c, z_btnprompt_ctr.c and argui's own code keep their addresses. Compiled
# like ctr_screens.c, for size: no switch tables or folded printf calls in .rodata.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=CtrArgui_menuOpen,--wrap=CtrArgui_menuUpdate,--wrap=CtrArgui_menuClose
    -Wl,--wrap=CtrArgui_load,--wrap=CtrArgui_override,--wrap=CtrArgui_saveEdits,--wrap=CtrArgui_keepLit)
# The menu opens on SELECT's press: ctr_optmenu.c wraps hidKeysUp, which only main's
# combo calls, so main's code keeps its addresses.
target_link_options(ctrport PRIVATE -Wl,--wrap=hidKeysUp)
# The boot's log unseen unless Other screen is Stats and log, late code at the end of
# source/main.c: main's CtrSdMigrate_run call moves the stats and log consoles to the bottom screen's other buffer,
# CtrLab_booted shows them again for Stats and log, the renderer's move of the consoles to the top screen during the
# boot (game_screen Bottom) keeps them unseen, and the first CtrLog_setConsoleEnabled moves them back. Wrapped, so
# main's and the renderer's code keep their addresses.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLab_booted,--wrap=CtrLog_setConsoleEnabled,--wrap=CtrScreens_moveConsoles)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# The options menu's Controls page (change ctrlui, 11-02), all late code in source/ctr_ctrlui.c: optmenu's Controls row
# and its keys for a page not its own call it (two call changes in ctr_optmenu.c, slots kept). Compiled like optmenu.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_ctrlui.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# The player's L+SELECT screenshot (off in release builds, kept for debug and development). Its wrap,
# late code in source/ctr_screens.c, does nothing unless CTRPORT_PLAYER_SHOTS (build.sh ... shots). There it also
# wraps hidScanInput to hold the capture back while D-pad Down could be part of Rosalina's L+Down+SELECT.
option(CTRPORT_PLAYER_SHOTS "Keep the player's L+SELECT screenshot combo (debug and development)" OFF)
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_requestPlayerScreenshot)
if(CTRPORT_PLAYER_SHOTS)
    target_compile_definitions(ctrport PRIVATE CTRPORT_PLAYER_SHOTS)
    target_link_options(ctrport PRIVATE -Wl,--wrap=hidScanInput)
    # optmenu's test probe (ctr_optmenu.c) counts frames there; release builds don't wrap it.
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_dialogueShown)
    # Its SELECT press probe (opt_press_at) reports the press there.
    target_link_options(ctrport PRIVATE -Wl,--wrap=hidKeysDown)
endif()
# Its strings are named arrays in the late constants; printf folded to puts, or a switch table, would add constants to
# .rodata and move it.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_screens.c" PROPERTIES COMPILE_OPTIONS
    "-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# UNDERTALE's console prompts (btnprompt): late code, constants and statics only, compiled like ctr_screens.c,
# and for size (-Os): it runs at load and on a settings change, never per frame.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_btnprompt_ctr.c" "${ENGINE_DIR}/src/z_btnprompt.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# source/ctr_renderer.c (contention): tiles' copies in VRAM on the Old models, late code at the end of the file. Wrapped,
# so the renderer's code before it, which calls these, stays as it was.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=CtrTextures_takeHot,--wrap=CtrMemory_vramAllocNow,--wrap=C3D_TexInitWithParams,--wrap=C3D_RenderTargetCreate
    -Wl,--wrap=CtrTextures_destroy
    -Wl,--wrap=CtrTextures_prefetchRoom,--wrap=C3D_SyncDisplayTransfer,--wrap=CtrScreens_flushConsoles)
# source/ctr_gpu_queue.c (contention): a frame begin at the command ring's end takes its start without waiting for
# the GPU when the completion count proves it free, and such a frame runs on past its cap instead of running so far
# (canWrap), late code at the end of the file. Wrapped, so the renderer's calls and the queue's code before stay as
# they were.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrGpu_frameBeginOverlapped -Wl,--wrap=CtrGpu_canWrapCommandBuffer)
# Its log-only finish callback check (FINISH MISMATCH): a lock per queue finish, so identity and diagnostic builds
# only, never production or timing builds.
option(CTRPORT_RING_FINISH_CHECK "Compile in the command ring's finish callback check (identity, diagnostics)" OFF)
if(CTRPORT_RING_FINISH_CHECK)
    target_compile_definitions(ctrport PRIVATE CTRPORT_RING_FINISH_CHECK)
endif()

# view_snap (uthud; UNDERTALE's default since pixfix): source/ctr_uthud_snap.c, at the display-caps query
# after each CtrRenderer_create, reads the switch and hooks the renderer. screens-port already wraps CtrRenderer_create
# and the settings loaders. No existing function changes. UNDERTALE only: DELTARUNE builds compile the file empty.
# -Os: the UNDERTALE 1.0 late reserve is tight (a new file, so nothing else is compiled differently).
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_getDisplayCaps)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_uthud_snap.c" PROPERTIES COMPILE_OPTIONS "-Os")
# DELTARUNE's HP label at one shape (pixfix, task D): source/ctr_pixfix_hpname.c, at the same display-caps query,
# hooks the renderer's init. DELTARUNE only (UNDERTALE's builds wrap the query for view_snap): UNDERTALE compiles it empty.
# Either way the query is wrapped once, and the chain's links (itemverdict's after these) are in source/ctr_wrap_chain.h.
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_getDisplayCaps)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_pixfix_hpname.c" PROPERTIES COMPILE_OPTIONS "-Os")
# UNDERTALE's battle grid at native (pixfix, task F): source/ctr_pixgrid.c puts an outer vtable in front of the
# renderer's once main.c has made it (CtrRenderer_setSurfaceReaders runs once per load right after
# CtrRenderer_create, and nothing else wraps it). ctr_renderer.c only gains a data table at its end. DELTARUNE builds
# compile the file empty.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_setSurfaceReaders)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_pixgrid.c" PROPERTIES COMPILE_OPTIONS "-Os")
# UNDERTALE's KR label kept sharp under Smart smoothing (smoothux): source/ctr_krsharp.c gives the KR meter's items
# the verdict Smart keeps sharp, once per frame from CtrRenderer_setFrameSkipKeepApp (main.c's per-frame call, which
# nothing else wraps). Late code only. DELTARUNE builds compile the file empty.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_setFrameSkipKeepApp)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_krsharp.c" PROPERTIES COMPILE_OPTIONS "-Os" COMPILE_DEFINITIONS "Krsharp_verdicts=Menulag_verdicts")
# argui (round 5, after btnprompt): game-themed options menu, first-boot hint and FPS counter, late code only
# (source/ctr_argui.c). GSPLCD_PowerOffBacklight is wrapped once, by menupause (ctr_screens.c), whose wrapper
# asks CtrArgui_keepLit first.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# DELTARUNE's attack-bar PRESS drawn for the screen (pixfix, task E; after argui): source/ctr_pixfix_press.c finds the sprite
# after CtrTextures_init and rewrites its word in each tile as it loads, at the tile's C3D_TexFlush (nothing else
# wraps either), and forgets the pages at the start of the renderer init (CtrPacer_init). DELTARUNE only: UNDERTALE
# compiles the file empty.
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_init,--wrap=C3D_TexFlush,--wrap=CtrPacer_init)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_pixfix_press.c" PROPERTIES COMPILE_OPTIONS "-Os")
# DELTARUNE's stretched battle box keeps its corners on its edges at native (pixfix, task C):
# source/ctr_pixfix_nslice.c holds the pixel grid for the stretched battle box's nine-slice corners, from an
# outer vtable installed after CtrRenderer_create (UNDERTALE wraps the same function for ctr_pixgrid.c). DELTARUNE only:
# UNDERTALE compiles the file empty.
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_setSurfaceReaders)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_pixfix_nslice.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Crisp small text (smalltext; UNDERTALE, and DELTARUNE New through source/z_smalltext_dr.c, linked last like
# z_symverdict_dr.c below; DELTARUNE Old's late code reserve can't hold it; the Smoothing menu's choices since smoothux,
# UNDERTALE only): source/z_smalltext.c puts an outer vtable with its own text
# draws in front of the renderer's at a room change, when a Crisp key is on; CtrRenderer_loadFontVerdicts (z_roomload.c's call into the renderer) is wrapped
# for that, so z_roomload.c and ctr_renderer.c keep their code. Its --ini keys come through the CtrArgui_override chain
# (source/ctr_wrap_chain.h); DELTARUNE wraps CtrRenderer_loadFontVerdicts for crispart below, and on New the chain
# starts at smalltext. Compiled like z_loadscreen.c: its strings
# are named arrays in the late code area, and no printf folding or switch tables add constants to .rodata.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_loadFontVerdicts)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_smalltext.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/z_smalltext_dr.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# CTRPORT_BUBBLE_CHECK (build.sh "bubblecheck"; test builds only): z_smalltext.c checks every Crisp line, any font,
# against PC's ink and logs those closer to their bubble's or the battle box's right edge than PC allows, off the screen
# where PC is on it, or closer to the line below than PC (tools/analysis/bubblecheck_report.py).
option(CTRPORT_BUBBLE_CHECK "Test only: log Crisp lines that cross their bubble, box, screen or neighbours" OFF)
if(CTRPORT_BUBBLE_CHECK)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_smalltext.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BUBBLE_CHECK)
endif()
# Symmetric halving's first look from the build (utfirstuse, UNDERTALE only): source/z_symverdict.c marks the items
# tools/aot/symverdict.py found symCopy would draw plain (romfs/fonts/<data.win folder>.dtsy, written by build.sh's
# fontverdict.py step) in the renderer's symState at each room change, a link of the CtrRenderer_loadFontVerdicts chain
# wrapped above for smalltext (source/ctr_wrap_chain.h); ctr_renderer.c gains a data table. Late code only, compiled
# like z_smalltext.c. DELTARUNE: New 3DS builds only, after crispart (below), compiled through z_symverdict_dr.c so
# that it links last (late_reserve.ld); Old builds compile both empty, and UNDERTALE z_symverdict_dr.c.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_symverdict.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/z_symverdict_dr.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# source/z_sdmigrate.c (sdmigrate, both games): what is left of the old SD folder's move (retired 2026-10-27,
# hashfix: CtrSdMigrate_run does nothing) and, on DELTARUNE, the chapter check's line and cache folder (a wrap of
# BxChapterHash_check). Compiled like z_smalltext.c, so all of it stays in the late code area.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_sdmigrate.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
if(GAME_DIR_NAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=BxChapterHash_check)
endif()
# source/z_roomload.c (roomload): the room change finishes the room's code and tile reads before its first frame is
# shown. It runs inside argui's wrapper of CtrTextures_prefetchRoom (ctr_argui.c calls CtrRoomload_prefetchRoom, which
# calls __real_CtrTextures_prefetchRoom), so CtrTextures_prefetchRoom and its caller stay as they were; all of it is
# late code, and its log formats are named late constants (no printf folding, no switch tables in .rodata).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomload.c" PROPERTIES COMPILE_OPTIONS
    "-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# The room plan's code list goes through roomload's read (z_roomload.c): its own per-entry read stands down.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomplan.c" APPEND PROPERTY COMPILE_DEFINITIONS ROOMPLAN_OWN_CODE_READ=0)
# utstall (UNDERTALE): source/z_utstall.c moves more of a room's known tile loads into the hidden room load: a
# battle group's planned tiles stand in for the battle room's record, the plan's tiles first used after frame 45 are marked
# for the room load, and the room load gets more time. It wraps CtrRoomload_prefetchRoom (ctr_argui.c's call) and
# CtrTextures_roomLoadFinish (z_roomload.c's call), which nothing else wraps. Late code only; DELTARUNE compiles it empty.
option(CTRPORT_PKLIN_DIAG "UNDERTALE Old: a linear free-block census at each room load (test only)" OFF)
option(CTRPORT_PKLIN_STRESS "UNDERTALE Old: slow promotion reads that each room load stops (test only)" OFF)
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRoomload_prefetchRoom,--wrap=CtrTextures_roomLoadFinish)
    # change utflowey: z_utstall.c also wraps RoomLoad_collect (z_roomload.c; texcacheCollectWholeRead's once-a-frame call), so
    # that in a scene rule A cut, tiles with a packed copy look older than tiles that would come from the SD card.
    target_link_options(ctrport PRIVATE -Wl,--wrap=RoomLoad_collect)
    if(CTRPORT_TARGET STREQUAL "o3ds")
        # change utflowey, packed-tier fix (UNDERTALE Old only): a promotion needs its size plus 4 MB of heap free (the
        # low-heap line, 3 MB, and 1 MB), not 8 MB, which a 64 MB Old 3DS never has in play. Within the packed budget,
        # idle copies go one for one for it. z_utstall.c's RoomLoad_collect wrap promotes only a page read from the SD
        # card again within a minute, and sizes the Old budget from the game's measured heap need.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_tile_reader.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "READER_PROMOTION_RESERVE=(4u * 1024u * 1024u)")
        # change utflowey2 (UNDERTALE Old only): its linear tier is removed (11-11); CTRPORT_PKLIN stays for the CTR_PKLIN_*
        # names (z_utstall.c's forwards) and for freewins L1, which keys on it. pklindiag and pklinstress do nothing now.
        target_compile_definitions(ctrport PRIVATE CTRPORT_PKLIN)
        # change codeclose (F1/F6, UNDERTALE Old only): usReadCode's call of RoomPlan_roomCode reads the plan's code after
        # the tiles with z_roomload.c's coalesced, non-aborting reader and hands usReadCode nothing (z_roomload.c's end).
        target_link_options(ctrport PRIVATE -Wl,--wrap=RoomPlan_roomCode)
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrAudioSystem_shedSfx)
        # freewins L1: the overflow allocator's linear piece (z_utstall_l2.c; ctr_memory.c calls it in place of
        # linearAlloc). The heap reliever's wrap hands it the texture cache; nothing else wraps it in UNDERTALE.
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrMemory_setHeapReliever)
        if(CTRPORT_PKLIN_DIAG)
            target_compile_definitions(ctrport PRIVATE CTRPORT_PKLIN_DIAG)
        endif()
        if(CTRPORT_PKLIN_STRESS)
            target_compile_definitions(ctrport PRIVATE CTRPORT_PKLIN_STRESS)
        endif()
    endif()
else()
    # change utflowey, DELTARUNE only: source/z_phaseplan.c (phaseplan) wraps RoomLoad_collect. romfs:/roomplan/phases.dtph
    # (tools/roomplan/build_phases.py, made by build.sh) splits a long room's tiles into phases; once a phase starts,
    # the next one's tiles are readied in the background.
    target_link_options(ctrport PRIVATE -Wl,--wrap=RoomLoad_collect)
endif()
# change utstall item 8 step 1 (both games): a tile read the game thread performs while it waits for another
# (CtrTileReader_wait) uses the cache file the game thread keeps open instead of opening its own and handing it to
# the reader thread to close. Late code in ctr_tile_reader.c and ctr_textures.c; nothing else wraps these two.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTileReader_wait,--wrap=CtrTileReader_start)
# Step 1b' (both games): a finished pack worker closes its data.win handles (ctr_texture_pack.c's tail, the wrap of
# CtrPackWorker_poll, which nothing else wraps).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPackWorker_poll)
# Step 1c (both games): with the complete marker there at the chapter's start, the background worker skips the
# header-only checks once packed copies stop fitting (ctr_texture_pack.c's tail; nothing else wraps these two).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPack_isComplete,--wrap=CtrPackWorker_setKeepPacked)
# racefix (both games): distrustPack's deletion of a damaged page file tells the background worker first (ctr_textures.c's
# end; nothing else wraps it).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPack_removeFile)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utstall.c" PROPERTIES COMPILE_OPTIONS
    "-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_phaseplan.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# DELTARUNE's battle command labels at native (cmdlabel): at native size in extended space by default (option 2b), and
# settings.ini's cmdlabel key picks another mode; the HP label is redrawn 5 output rows tall (hplabel key, 0 for as
# before). The same hook asks Smart smoothing for a verdict on the hero heads' small items over tiles of two formats
# (smartmix key, 0 for as before). source/ctr_cmdlabel.c hooks the renderer's drawSprite, drawSpritePart and init at
# CtrRenderer_loadFlickerProbe, which main.c calls once per load after CtrRenderer_setSurfaceReaders, and which nothing
# else wraps. DELTARUNE only: UNDERTALE compiles the file empty.
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_loadFlickerProbe)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_cmdlabel.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Its wrap hands on to source/ctr_panelgrid.c (panelgrid key, 0 by default): the battle panel's slots on the pixel grid
# at native, each slot and Chapter 1's button row moved as one piece.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_panelgrid.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Thin bars at half resolution (boxedge; both games): source/ctr_boxedge.c hooks the renderer's drawSprite where
# main.c applies the loaded settings, CtrBtnprompt_setDisplay, after every other hook is in place (on DELTARUNE
# natsurf6's wrapper of it calls boxedge's, Boxedge_setDisplay). Late code only, for size (-Os).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrBtnprompt_setDisplay)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_boxedge.c" PROPERTIES COMPILE_OPTIONS "-Os")
# DELTARUNE's battle grid between output pixels at native (bggrid; settings.ini's bggrid key, mode 4 by default
# so that the grid matches the PC picture; 0 draws it as before): source/ctr_bggrid.c
# hooks the renderer's drawSpriteTiled at CtrRenderer_setSliderOverride, which main.c calls once per load with -1
# after CtrRenderer_loadFlickerProbe, and which nothing else wraps.
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_setSliderOverride)
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_bggrid.c" PROPERTIES COMPILE_OPTIONS "-Os")

# TPAG helpers use the shared late reserve; layout includes retain vacated cold slots.
# Friendly crash screen and release-quiet logging (argcrash): source/z_argcrash.c, all in .text.late.argcrash,
# .rodata.argcrash_in_text and .bss.late.argcrash. Wrapped so every existing translation unit keeps its exact code.
option(CTRPORT_CRASH_TEST "Compile in the debug crash-test trigger in source/z_argcrash.c" OFF)
set(ARGCRASH_COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables;-DCTRPORT_RELEASE_VER=${RELEASE_VERSION}")
if(CTRPORT_CRASH_TEST)
    list(APPEND ARGCRASH_COMPILE_OPTIONS "-DCTRPORT_CRASH_TEST=1")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_argcrash.c" PROPERTIES COMPILE_OPTIONS "${ARGCRASH_COMPILE_OPTIONS}")
# The one wrap of CtrWatchdog_stop; its chain's links and their order are in source/ctr_wrap_chain.h.
target_link_options(ctrport PRIVATE
    -Wl,--wrap=CtrLaunchArgs_parse,--wrap=CtrMemory_markGameThread,--wrap=CtrWatchdog_watch,--wrap=CtrWatchdog_stop,--wrap=CtrHbldr_setNext
    -Wl,--wrap=CtrLab_exiting,--wrap=CtrLab_poll,--wrap=CtrLog_emergency,--wrap=LightEvent_Init,--wrap=LightEvent_Signal
    -Wl,--wrap=platformLog,--wrap=svcOutputDebugString,--wrap=aptMainLoop,--wrap=svcBreak,--wrap=exit)

# Builds with the heap profiler (source/ctr_heap_profile.c), which logs the call stacks holding the most heap at
# each [mem] line. Every allocation walks its call stack, so loading is slower.
option(CTRPORT_HEAP_PROFILE "Compile in the heap profiler" OFF)
if(CTRPORT_HEAP_PROFILE)
    target_compile_definitions(ctrport PRIVATE CTRPORT_HEAP_PROFILE)
    target_compile_options(ctrport PRIVATE -funwind-tables)
    # strdup allocates inside newlib, out of sight of the malloc wrapper, and so do number conversion and stdio
    # (_malloc_r and the rest, recorded only when malloc's wrapper isn't the caller).
    target_link_options(ctrport PRIVATE -Wl,--wrap=strdup,--wrap=strndup,--wrap=_malloc_r,--wrap=_calloc_r,--wrap=_realloc_r)
endif()

# Test only (change utasgore): performance counters around the step, the draw and sampled drawSprite calls
# (source/ctr_asgprobe.c, all late code). The link sends main.c's Runner_step and Runner_drawViews calls to its wrappers.
option(CTRPORT_ASG_PROBE "Compile in the performance-counter frame probe" OFF)
if(CTRPORT_ASG_PROBE)
    target_compile_definitions(ctrport PRIVATE CTRPORT_ASG_PROBE)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_step,--wrap=Runner_drawViews)
    # (asgore30) runner.c's event runs, for the probe's split of the step by code.
    target_link_options(ctrport PRIVATE -Wl,--wrap=VM_beginEventRun,--wrap=VM_endEventRun,--wrap=VM_runEventBound)
    # Compiled for size, to fit the late text reserve beside the units it measures.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c" APPEND PROPERTY COMPILE_OPTIONS "-Os")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c" APPEND PROPERTY COMPILE_DEFINITIONS "AP_LEAN")
    # UNDERTALE's inputlate wraps Runner_step too: in this build its wrapper is renamed (apInputlateStep) and the
    # probe's calls it, so its step runs inside the probe's (ctr_asgprobe.c).
    if(CTRPORT_GAME STREQUAL "undertale")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c" APPEND PROPERTY COMPILE_DEFINITIONS
                     "__wrap_Runner_step=apInputlateStep" "__real_Runner_step=apRealStep")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c" APPEND PROPERTY COMPILE_DEFINITIONS
                     "AP_INPUTLATE")
    endif()
endif()

# Test only (change flowerhw, 2026-11-03): each table put in front of the renderer's notes itself (CTR_VTCHAIN_NOTE,
# ctr_renderer.h) and source/ctr_vtchain.c stops the run when one goes on in front of a chain that already holds it (a
# copy calling back into its own table loops: smalltext over the tower decorations). build.sh never sets it: a test
# build directory's cache does, and a release never carries it (the notes are no tokens without it; the run logs a
# "vtchain:" line at each install with it).
option(CTRPORT_VTCHAIN_CHECK "Test only: stop the run when a renderer table goes on in front of a chain holding it" OFF)
if(CTRPORT_VTCHAIN_CHECK)
    target_compile_definitions(ctrport PRIVATE CTRPORT_VTCHAIN_CHECK)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_vtchain.c")
endif()
# Test only (change flowhalf): the largest scale each texture page item is drawn at in one room (source/ctr_halfcensus.c,
# all late code). The link sends main.c's Runner_drawViews call to its wrapper.
option(CTRPORT_HALF_CENSUS "Compile in the half-resolution draw-scale census" OFF)
if(CTRPORT_HALF_CENSUS)
    target_compile_definitions(ctrport PRIVATE CTRPORT_HALF_CENSUS)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_halfcensus.c")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_drawViews)
endif()
# Prototype (change flowhalf): art stored at half resolution (source/ctr_halfart.c, all late code), from the patch
# tools/analysis/halfart_pack.py made at build time (romfs:/halfart/<game>.dthr). The link sends the texture cache's
# key and its PNG page decodes to the wrappers there.
set(CTRPORT_HALFART_FILE "" CACHE FILEPATH "The half-resolution art patch for the RomFS (build.sh halfart=...; empty: none)")
if(CTRPORT_HALFART_FILE)
    # Its SD texture cache goes in a folder of its own, cache_lt_<n>, n from the patch's SHA-256 with the top bit set
    # (above any texture_cache_run setting), so a cache written by a full-art build or another patch is never read.
    file(SHA256 "${CTRPORT_HALFART_FILE}" CTRPORT_HALFART_SHA)
    string(SUBSTRING "${CTRPORT_HALFART_SHA}" 0 8 CTRPORT_HALFART_TAG)
    math(EXPR CTRPORT_HALFART_CACHE_RUN "0x${CTRPORT_HALFART_TAG} | 0x80000000" OUTPUT_FORMAT DECIMAL)
    target_compile_definitions(ctrport PRIVATE CTRPORT_HALFART "CTRPORT_HALFART_PATH=\"romfs:/halfart/${CTRPORT_GAME}.dthr\""
        "CTRPORT_HALFART_CACHE_RUN=${CTRPORT_HALFART_CACHE_RUN}u")
    message(STATUS "Half-resolution art: ${CTRPORT_HALFART_FILE}, texture cache folder cache_lt_${CTRPORT_HALFART_CACHE_RUN}")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_halfart.c")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPack_keyFor,--wrap=CtrPng_decode,--wrap=CtrPack_setTestRun)
    configure_file("${CTRPORT_HALFART_FILE}" "${CTRPORT_ROMFS_DIR}/halfart/${CTRPORT_GAME}.dthr" COPYONLY)
else()
    file(REMOVE "${CTRPORT_ROMFS_DIR}/halfart/${CTRPORT_GAME}.dthr")
endif()

# The retained engine slots were measured for unprofiled GCC Release only.
# Keep relocation and slot emission on the same explicit build-type gate.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_FREEWINS)
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" "${ENGINE_DIR}/src/vm_builtins.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_LAYERFIX_RELEASE_LAYOUT)
    add_custom_command(TARGET ctrport PRE_LINK
        COMMAND "${CMAKE_COMMAND}" "-DOBJECT_DIR=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ctrport.dir"
            "-DENGINE_DIR=${ENGINE_DIR}" "-DOBJDUMP=${CMAKE_OBJDUMP}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_layerfix_inputs.cmake"
        VERBATIM)
endif()

# simplefix (2026-10-28): runner.c's and vm_builtins.c's Collision_computeBBox round their edges through
# z_bboxround.c's Collision_bboxRound (the same bits as GMLReal_bankersRound, without newlib's floor). Their slots are
# kept at each file's end under the release-layout gate above. Other users of collision.h keep the old rounding.
set_property(SOURCE "${ENGINE_DIR}/src/runner.c" "${ENGINE_DIR}/src/vm_builtins.c"
    APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BBOX_ROUND)

# The reach gate (CTRPORT_REACH_GATE, with the engine sources above): its hooks and accessor compile in only here.
if(CTRPORT_REACH_GATE)
    target_compile_definitions(ctrport PRIVATE BS_REACH_GATE)
endif()

# A test build for an emulator that grants more memory than the console: the memory the heap and linear memory split
# between, in KB, as the console grants it (source/ctr_memory.c). Empty (the default) takes what the system grants.
# A separate installed-CIA ELF preserves ordinary HBL/UT/New code bytes.
option(CTRPORT_DR_OLD_CIA80_TILES "DR Old CIA80: shift 8 MiB of heap to linear tiles" OFF)
if(CTRPORT_DR_OLD_CIA80_TILES)
    if(NOT CTRPORT_GAME STREQUAL "deltarune" OR NOT CTRPORT_TARGET STREQUAL "o3ds"
       OR NOT CTRPORT_O3DS_MEMORY STREQUAL "80MB")
        message(FATAL_ERROR "CIA80 tiles require DELTARUNE o3ds with an 80MB CIA exheader")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_DR_OLD_CIA80_TILES)
    target_link_options(ctrport PRIVATE -Wl,--wrap=__system_allocateHeaps,--undefined=__system_allocateHeaps)
endif()
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_memory.c" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_allocate_heaps.inc")

set(CTRPORT_MEMORY_CAP_KB "" CACHE STRING "Memory to split between the heap and linear memory, in KB (empty: all granted)")
if(CTRPORT_MEMORY_CAP_KB)
    target_compile_definitions(ctrport PRIVATE CTRPORT_MEMORY_CAP_KB=${CTRPORT_MEMORY_CAP_KB})
endif()

# Selective AOT: CTRPORT_AOT_DIR names a directory holding the aot_gen_*.c and aot_table.c
# that tools/aot/gmlaot.py made from your own data.win. They are compiled alongside the engine; code entries
# they do not match run in the interpreter. Empty (the default) builds the plain interpreter.
set(CTRPORT_AOT_DIR "" CACHE PATH "Directory with the generated aot_gen_*.c and aot_table.c (empty: no AOT)")
if(CTRPORT_GAME STREQUAL "undertale" AND NOT CTRPORT_AOT_DIR)
    # Same upstream table format, privately generated even when AOT is off.
    target_sources(ctrport PRIVATE "${CTRPORT_BUILD_ID_DIR}/btnprompt_fallback.c")
endif()
if(CTRPORT_AOT_DIR)
    # Cold dependency invalidation retains the original VM functions and their warm input-section placement.
    target_link_options(ctrport PRIVATE -Wl,--wrap=VM_reset)
endif()
option(CTRPORT_AOT_STRESS "Make the generated code hand over to the interpreter every few blocks (testing)" OFF)
# A directory from prepare.py --chapters holds one folder of generated code per chapter (aot_modules.txt lists them).
# Each is linked into a module of its own, loaded when its chapter starts (source/ctr_aot_module.h); see "AOT modules"
# below. The region they load into is CTRPORT_AOT_REGION_MB, by default the target's budget.
set(CTRPORT_AOT_MODULES OFF)
if(CTRPORT_AOT_DIR AND EXISTS "${CTRPORT_AOT_DIR}/aot_modules.txt")
    set(CTRPORT_AOT_MODULES ON)
endif()
if(CTRPORT_TARGET STREQUAL "o3ds")
    set(CTRPORT_AOT_REGION_DEFAULT 4)
else()
    set(CTRPORT_AOT_REGION_DEFAULT 12)
endif()
set(CTRPORT_AOT_REGION_MB "${CTRPORT_AOT_REGION_DEFAULT}" CACHE STRING "MB reserved for the AOT module of the running chapter")
if(CTRPORT_AOT_MODULES)
    # Rebinding exists only with chapter modules; static-table UT must not retain this unused VM body.
    target_link_options(ctrport PRIVATE -Wl,--wrap=VM_aotUseTable)
    if(CTRPORT_AOT_STRESS)
        message(FATAL_ERROR "aot-stress builds link the generated code in: use aot=<dir> with a directory from prepare.py without --chapters")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_AOT CTRPORT_AOT_MODULES CTRPORT_AOT_REGION_MB=${CTRPORT_AOT_REGION_MB})
elseif(CTRPORT_AOT_DIR)
    # aot_late.c: UNDERTALE's late copies (gmlaot.py's LATE_COPIES; late_reserve.ld puts the whole file in late text).
    file(GLOB CTRPORT_AOT_SOURCES CONFIGURE_DEPENDS "${CTRPORT_AOT_DIR}/aot_gen_*.c" "${CTRPORT_AOT_DIR}/aot_table.c"
         "${CTRPORT_AOT_DIR}/aot_late.c" "${CTRPORT_AOT_DIR}/aot_gettext.c")
    # Typed regions: aot_typed.c from a typed list, or aot_typed_bind.c and aot_typed_<n>.c from prepare.py --typed all
    # (aot/aot_modules.cmake; linked in, every file is kept).
    set(CTRPORT_TYPED_SOURCES "")
    if(CTRPORT_TYPED_AOT)
        file(GLOB CTRPORT_TYPED_SOURCES CONFIGURE_DEPENDS "${CTRPORT_AOT_DIR}/aot_typed.c" "${CTRPORT_AOT_DIR}/aot_typed_*.c")
        if(NOT CTRPORT_TYPED_SOURCES)
            message(FATAL_ERROR "CTRPORT_AOT_DIR ${CTRPORT_AOT_DIR} has no aot_typed.c: run tools/aot/prepare.py with --typed")
        endif()
        list(APPEND CTRPORT_AOT_SOURCES ${CTRPORT_TYPED_SOURCES})
    endif()
    # Appended entries follow every existing unit, including typed code. Keep the original glob/link order intact.
    file(GLOB CTRPORT_AOT_APPEND_SOURCES CONFIGURE_DEPENDS "${CTRPORT_AOT_DIR}/aot_append_*.c")
    list(APPEND CTRPORT_AOT_SOURCES ${CTRPORT_AOT_APPEND_SOURCES})
    # A fixed AOT block (change aotprof, 2026-10-28): tools/aot/selection/<game>-<target>-pad.txt (tools/aot/aot_pad.py)
    # gives the bytes aot_pad.c, linked after every other AOT object, adds to the AOT code, constants and statics and
    # to the string pool, so they end where a base build had them and the libraries and data after them keep their
    # addresses when the selection changes. Only the AOT functions move. The padding has no symbols
    # (tools/analysis/check_layout.py compares symbols).
    set(CTRPORT_AOT_PAD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/aot/selection/${CTRPORT_GAME}-${CTRPORT_TARGET}-pad.txt")
    if(EXISTS "${CTRPORT_AOT_PAD_FILE}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CTRPORT_AOT_PAD_FILE}")
        foreach(kind text rodata bss str)
            set(CTRPORT_AOT_PAD_${kind} 0)
        endforeach()
        file(STRINGS "${CTRPORT_AOT_PAD_FILE}" CTRPORT_AOT_PAD_LINES REGEX "^(text|rodata|bss|str) [0-9]+$")
        foreach(line ${CTRPORT_AOT_PAD_LINES})
            string(REGEX MATCH "^([a-z]+) ([0-9]+)$" _ "${line}")
            set(CTRPORT_AOT_PAD_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endforeach()
        string(CONCAT CTRPORT_AOT_PAD_ASM
            [[__asm__(".section .text.aotpad,\"axR\",%progbits\n\t.p2align 0\n\t.space ]] "${CTRPORT_AOT_PAD_text}" [[\n"]] "\n"
            [[        ".section .rodata,\"aR\",%progbits\n\t.p2align 0\n\t.space ]] "${CTRPORT_AOT_PAD_rodata}" [[\n"]] "\n"
            [[        ".section .bss,\"awR\",%nobits\n\t.p2align 0\n\t.space ]] "${CTRPORT_AOT_PAD_bss}" [[\n");]] "\n")
        if(CTRPORT_AOT_PAD_str GREATER 0)
            # One string of str bytes with its terminator, of DEL characters (0x7f), which no other string ends with,
            # so the pool merges nothing into it.
            math(EXPR CTRPORT_AOT_PAD_str_fill "${CTRPORT_AOT_PAD_str} - 1")
            string(CONCAT CTRPORT_AOT_PAD_ASM "${CTRPORT_AOT_PAD_ASM}"
                [[__asm__(".section .rodata.str1.4,\"aMSR\",%progbits,1\n\t.p2align 2\n\t.fill ]] "${CTRPORT_AOT_PAD_str_fill}"
                [[, 1, 0x7f\n\t.byte 0\n");]] "\n")
        endif()
        set(CTRPORT_AOT_PAD_C "${CMAKE_CURRENT_BINARY_DIR}/aotpad/aot_pad.c")
        file(CONFIGURE OUTPUT "${CTRPORT_AOT_PAD_C}" CONTENT "// Generated by platform/3ds/CMakeLists.txt from tools/aot/selection/${CTRPORT_GAME}-${CTRPORT_TARGET}-pad.txt.\n${CTRPORT_AOT_PAD_ASM}" @ONLY)
        list(APPEND CTRPORT_AOT_SOURCES "${CTRPORT_AOT_PAD_C}")
        message(STATUS "AOT block padding: text ${CTRPORT_AOT_PAD_text} B, rodata ${CTRPORT_AOT_PAD_rodata} B, bss ${CTRPORT_AOT_PAD_bss} B, string pool ${CTRPORT_AOT_PAD_str} B")
    endif()
    if(NOT EXISTS "${CTRPORT_AOT_DIR}/aot_table.c")
        message(FATAL_ERROR "CTRPORT_AOT_DIR ${CTRPORT_AOT_DIR} has no aot_table.c: run tools/aot/gmlaot.py first")
    endif()
    target_sources(ctrport PRIVATE ${CTRPORT_AOT_SOURCES})
    # A generated function can be thousands of instructions long. GCC's scheduling before register allocation and its
    # variable tracking for debug info both take time that grows much faster than the function; without them the largest
    # file builds in a third of the time. Line information is kept.
    set_source_files_properties(${CTRPORT_AOT_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-schedule-insns;-fno-var-tracking;-fno-var-tracking-assignments")
    if(CTRPORT_AOT_APPEND_SOURCES)
        # Appended load-time entries have their own helpers; fit them in the retained budget without moving old code.
        set_property(SOURCE ${CTRPORT_AOT_APPEND_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-merge-constants")
    endif()
    # aot_late.c's strings and constants unmerged: late_reserve.ld places them in late text, where a mergeable pool would
    # take over an equal string or constant of an earlier late input and move what follows it.
    set_property(SOURCE "${CTRPORT_AOT_DIR}/aot_late.c" APPEND PROPERTY COMPILE_OPTIONS "-fno-merge-constants")
    # aot_gettext.c: UNDERTALE's text lookup front (gmlaot.py's GETTEXT_FAST), the whole file after every other late input.
    set_property(SOURCE "${CTRPORT_AOT_DIR}/aot_gettext.c" APPEND PROPERTY COMPILE_OPTIONS "-fno-merge-constants")
    # UNDERTALE's generated numeric lava copy. Both entire objects (including
    # header helpers/constants) live in utoldloop's late reserve selectors.
    file(GLOB CTRPORT_NUMERIC_SOURCES CONFIGURE_DEPENDS "${CTRPORT_AOT_DIR}/aot_utold_loop.c" "${CTRPORT_AOT_DIR}/aot_utold_regions.c")
    if(CTRPORT_NUMERIC_SOURCES)
        if(NOT CTRPORT_GAME STREQUAL "undertale" OR NOT CTRPORT_TYPED_AOT)
            message(FATAL_ERROR "Pre-generated late numeric AOT requires UNDERTALE and typed; regenerate without late numeric code to disable it")
        endif()
        list(LENGTH CTRPORT_NUMERIC_SOURCES CTRPORT_NUMERIC_SOURCE_COUNT)
        if(NOT CTRPORT_NUMERIC_SOURCE_COUNT EQUAL 2)
            message(FATAL_ERROR "Late numeric caller and regions must both exist")
        endif()
        target_sources(ctrport PRIVATE "${CTRPORT_AOT_DIR}/aot_utold_loop.c" "${CTRPORT_AOT_DIR}/aot_utold_regions.c")
        set_source_files_properties("${CTRPORT_AOT_DIR}/aot_utold_loop.c" PROPERTIES
            COMPILE_OPTIONS "-fno-schedule-insns;-fno-var-tracking;-fno-var-tracking-assignments;-fno-merge-constants")
        set_source_files_properties("${CTRPORT_AOT_DIR}/aot_utold_regions.c" PROPERTIES
            COMPILE_OPTIONS "-fno-schedule-insns;-fno-var-tracking;-fno-var-tracking-assignments;-fno-merge-constants;-fno-fast-math;-ffp-contract=off"
            COMPILE_DEFINITIONS "${CTRPORT_TYPED_DEFS}")
        # freebytes: aot_late.c and aot_utold_loop.c share one aotStoreSlow and aotToInt32Slow (engine/src/aot/vm_aot.h,
        # AOT_LATE_SHARED_*) and call collision_gm14.c's RValue conversions (engine/src/rvalue.h, RVALUE_OTHER_EXTERN);
        # these two files' copies were byte-identical after relocation and their callers' code is unchanged
        # (freebytes results). UNDERTALE Old only, as part of H1 (2026-10-31): each fold's freed bytes
        # stay a hole in place (late_reserve.ld's H1 holes), filled with cold loading-screen code, so no hot code moves
        # by less than a whole 4 KiB set period. UNDERTALE New keeps its own copies and its bytes.
        if(CTRPORT_TARGET STREQUAL "o3ds")
            set(CTRPORT_LATE_OTHER_DEFS "RValue_toRealOther=Colflip14_toRealOther;RValue_toInt32Other=Colflip14_toInt32Other;RVALUE_OTHER_EXTERN")
            set_property(SOURCE "${CTRPORT_AOT_DIR}/aot_late.c" APPEND PROPERTY COMPILE_DEFINITIONS AOT_LATE_SHARED_DEFINE ${CTRPORT_LATE_OTHER_DEFS})
            set_property(SOURCE "${CTRPORT_AOT_DIR}/aot_utold_loop.c" APPEND PROPERTY COMPILE_DEFINITIONS AOT_LATE_SHARED_EXTERN ${CTRPORT_LATE_OTHER_DEFS})
            # z_asgborder.c (Asgore's sided fire, UNDERTALE late) had a third aotStoreSlow from the same source: it calls
            # aot_late.c's. Not byte-identical: GCC scheduled three blocks differently, with the same live state, memory
            # writes and calls at each join (c26rev r5 review, c27e983a9).
            set_property(SOURCE "${ENGINE_DIR}/src/z_asgborder.c" APPEND PROPERTY COMPILE_DEFINITIONS AOT_LATE_SHARED_EXTERN)
        endif()
    endif()
    # UNDERTALE's late-text reclaim (change argcheck) leaves aotResolveInlineBindings out of static-table builds
    # (vm_aot_inline.c); it comes back only when the generated code calls it (gmlaot.py emits the call in aot_gen_*.c
    # for a typed region that inlines a script). The default UNDERTALE typed list inlines none.
    foreach(_aot_src IN LISTS CTRPORT_AOT_SOURCES CTRPORT_NUMERIC_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_aot_src}")
        file(STRINGS "${_aot_src}" _aot_binds REGEX "aotResolveInlineBindings\\(" LIMIT_COUNT 1)
        if(_aot_binds)
            set_property(SOURCE "${ENGINE_DIR}/src/vm_aot_inline.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TYPED_INLINE)
            break()
        endif()
    endforeach()
    if(CTRPORT_TYPED_SOURCES)
        set_source_files_properties(${CTRPORT_TYPED_SOURCES} PROPERTIES COMPILE_OPTIONS "${CTRPORT_TYPED_AOT_OPTIONS}"
                                    COMPILE_DEFINITIONS "${CTRPORT_TYPED_DEFS}")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_AOT)
    if(CTRPORT_AOT_STRESS)
        target_compile_definitions(ctrport PRIVATE CTRPORT_AOT_STRESS)
    endif()
endif()

# Without them the build falls back to the vendored stb_vorbis, which works but
# decodes in floating point and costs noticeably more CPU.
option(CTRPORT_USE_TREMOR "Decode Ogg Vorbis with Tremor if it is installed" ON)
set(CTRPORT_AUDIO_LIBS "")
if(CTRPORT_USE_TREMOR)
    find_path(TREMOR_INCLUDE_DIR tremor/ivorbisfile.h)
    find_library(TREMOR_LIBRARY vorbisidec)
    find_library(OGG_LIBRARY ogg)
endif()
if(CTRPORT_USE_TREMOR AND TREMOR_INCLUDE_DIR AND TREMOR_LIBRARY AND OGG_LIBRARY)
    message(STATUS "Ogg Vorbis decoder: Tremor (${TREMOR_LIBRARY})")
    target_compile_definitions(ctrport PRIVATE CTRPORT_HAVE_TREMOR)
    target_include_directories(ctrport SYSTEM PRIVATE "${TREMOR_INCLUDE_DIR}")
    set(CTRPORT_AUDIO_LIBS ${TREMOR_LIBRARY} ${OGG_LIBRARY})
else()
    message(STATUS "Ogg Vorbis decoder: stb_vorbis (install 3ds-libvorbisidec and 3ds-libogg for Tremor)")
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    aacout_configure_decoder()
endif()

# Software H.264 decoding where there's no hardware decoder (the Old 3DS, or an
# emulator without the New 3DS's): openh264's decoder (BSD-2-Clause) built for
# the ARM11. tools/deps/openh264/build.sh fetches and builds it into
# build/deps/openh264/ (libopenh264dec.a and include/), which is used when it is
# there; CTRPORT_OPENH264_DIR names another directory laid out the same way.
# Without it, videos play without pictures where there's no hardware decoder.
# UNDERTALE plays no videos, so it goes without.
option(CTRPORT_VIDEO_OPENH264 "Decode H.264 video in software with openh264 where there's no hardware decoder" ON)
set(CTRPORT_OPENH264_DIR "" CACHE PATH "Directory with libopenh264dec.a and include/codec_api.h (empty: build/deps/openh264)")
set(OPENH264_DIR "${CTRPORT_OPENH264_DIR}")
if(NOT OPENH264_DIR)
    set(OPENH264_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build/deps/openh264")
endif()
get_filename_component(OPENH264_DIR "${OPENH264_DIR}" ABSOLUTE)
if(CTRPORT_GAME STREQUAL "undertale")
    message(STATUS "Software H.264 decoder: none (UNDERTALE plays no videos)")
elseif(CTRPORT_VIDEO_OPENH264 AND CTRPORT_OPENH264_DIR AND NOT EXISTS "${OPENH264_DIR}/libopenh264dec.a")
    message(FATAL_ERROR "CTRPORT_OPENH264_DIR: no libopenh264dec.a in ${OPENH264_DIR}")
elseif(CTRPORT_VIDEO_OPENH264 AND EXISTS "${OPENH264_DIR}/libopenh264dec.a")
    message(STATUS "Software H.264 decoder: openh264 from ${OPENH264_DIR}")
    target_compile_definitions(ctrport PRIVATE CTRPORT_HAVE_OPENH264)
    target_include_directories(ctrport SYSTEM PRIVATE "${OPENH264_DIR}/include")
    set(CTRPORT_AUDIO_LIBS ${CTRPORT_AUDIO_LIBS} "${OPENH264_DIR}/libopenh264dec.a" stdc++)
    set(CTRPORT_OPENH264_LINKED ON)
elseif(CTRPORT_VIDEO_OPENH264)
    message(STATUS "Software H.264 decoder: none, so no video pictures without the hardware decoder (run tools/deps/openh264/build.sh to fetch and build openh264)")
else()
    message(STATUS "Software H.264 decoder: none (CTRPORT_VIDEO_OPENH264 is off)")
endif()
target_link_libraries(ctrport PRIVATE render2d_shader_data bench_scenes_data drawskip_data citro3d ${CTRPORT_AUDIO_LIBS} m)
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_libraries(ctrport PRIVATE ctru m benchromfs_mount swkbd_late ctru)
endif()
target_link_options(ctrport PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/release-link.map")

# Hot-text alignment pad. On the Old models, whose 16 KB 4-way
# I-cache has no L2 behind it, shifting the hot code by a few cache lines cost up to 2.4% fps.
# hotpin/<game>-<target>.ld puts the code back at the addresses of the base it was generated from, with zero fill that
# never runs. It only matches the configuration it was made for (plain Release, no openh264), and it only holds until
# the next text change ahead of the hot set.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CTRPORT_LAYOUT_PINS_ENABLED)
    if(CTRPORT_PROFILE)
        set(KNFUSE_TILED_BYTES 2960)
    else()
        set(KNFUSE_TILED_BYTES 2464)
    endif()
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/check_knfuse_size.cmake")
    add_custom_command(TARGET ctrport POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:ctrport>" "-DNM=${CTRPORT_DKA_BIN}/arm-none-eabi-nm"
                "-DEXPECTED=${KNFUSE_TILED_BYTES}" -P "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/check_knfuse_size.cmake"
        VERBATIM)
endif()
# The heap / linear memory split (source/ctr_memory.c) is worked out from what the load image leaves of the grant, and
# some of its rules switch at a threshold. check_memory_split.cmake stops the build when this ELF is within 4 MB of
# one (integrate16: UNDERTALE New's image passed 12 MB and its linear memory fell from 56 to 32 MB unnoticed).
# A CTRPORT_MEMORY_CAP_KB test build splits a different amount, so it is not checked.
if(NOT CTRPORT_MEMORY_CAP_KB)
    if(GAME_WAD_DEFINE STREQUAL "ENABLE_WAD17")
        set(CTRPORT_SPLIT_WAD17 1)
    else()
        set(CTRPORT_SPLIT_WAD17 0)
    endif()
    if(CTRPORT_AOT_MODULES)
        set(CTRPORT_SPLIT_REGION_MB "${CTRPORT_AOT_REGION_MB}")
    else()
        set(CTRPORT_SPLIT_REGION_MB 0)
    endif()
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/check_memory_split.cmake")
    add_custom_command(TARGET ctrport POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:ctrport>" "-DREADELF=${CTRPORT_DKA_BIN}/arm-none-eabi-readelf"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_memory.c" "-DCONSOLE=${CTRPORT_TARGET}"
                "-DWAD17=${CTRPORT_SPLIT_WAD17}" "-DAOT_REGION_MB=${CTRPORT_SPLIT_REGION_MB}"
                "-DCIA80_TILES=${CTRPORT_DR_OLD_CIA80_TILES}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_memory_split.cmake"
        VERBATIM)
endif()
# creddodge tbox (c26rev r19 F1): drawTextImpl's string test calls the cdReachBox thunk, which passes r9 on as the text
# length. check_cdreach_thunk.cmake proves r9 holds textLen at that call on every build, whatever the compiler, pins or
# profile, and deletes the ELF otherwise, before either package target reads it. 0 B at run time.
if(GAME_WAD_DEFINE STREQUAL "ENABLE_WAD16" AND NOT CTRPORT_PROFILE AND NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|MinSizeRel)$")
    set(CTRPORT_CDREACH_EXPECT 1) # ctr_renderer.c builds the thunk (UNDERTALE without CTRPORT_RENDER_TIMING)
else()
    set(CTRPORT_CDREACH_EXPECT 0)
endif()
set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/check_cdreach_thunk.cmake")
add_custom_command(TARGET ctrport POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:ctrport>" "-DNM=${CTRPORT_DKA_BIN}/arm-none-eabi-nm"
            "-DOBJDUMP=${CTRPORT_DKA_BIN}/arm-none-eabi-objdump" "-DEXPECT=${CTRPORT_CDREACH_EXPECT}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cdreach_thunk.cmake"
    VERBATIM)
set(CTRPORT_HOTPIN_LD "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/${GAME_DIR_NAME}-${CTRPORT_TARGET}.ld")
option(CTRPORT_HOTPIN "Link with the hot-text alignment pad when hotpin/ has one for this game and target" ON)
if(CTRPORT_LAYOUT_PINS_ENABLED AND CTRPORT_HOTPIN AND EXISTS "${CTRPORT_HOTPIN_LD}" AND CMAKE_BUILD_TYPE STREQUAL "Release"
   AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_AOT_STRESS AND NOT CTRPORT_MEMORY_CAP_KB
   AND NOT CTRPORT_OPENH264_LINKED AND NOT CTRPORT_REACH_GATE
   AND CTRPORT_DEBUG_RECORD_FAIL_AFTER STREQUAL "")
    # CMake may hash any object's directory. Expand the platform exclusion by
    # unique filenames so new platform objects still go to .text.pinrest.
    get_target_property(hotpin_sources ctrport SOURCES)
    set(hotpin_names "")
    set(hotpin_platform_names "")
    foreach(source IN LISTS hotpin_sources)
        get_filename_component(name "${source}" NAME)
        string(REGEX REPLACE "\\.[cCSs]$" ".o" name "${name}")
        list(APPEND hotpin_names "${name}")
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${absolute}")
        if(relative MATCHES "^source/" AND name MATCHES "\\.o$")
            list(APPEND hotpin_platform_names "${name}")
        endif()
    endforeach()
    set(CTRPORT_HOTPIN_PLATFORM_OBJECTS "")
    foreach(name IN LISTS hotpin_platform_names)
        set(matches "${hotpin_names}")
        string(REPLACE "." "\\." pattern "${name}")
        list(FILTER matches INCLUDE REGEX "^${pattern}$")
        list(LENGTH matches count)
        if(NOT count EQUAL 1)
            message(FATAL_ERROR "hotpin: ${name} matches ${count} application sources; expected 1")
        endif()
        string(APPEND CTRPORT_HOTPIN_PLATFORM_OBJECTS " */${name}")
    endforeach()
    if(NOT CTRPORT_HOTPIN_PLATFORM_OBJECTS)
        message(FATAL_ERROR "hotpin: no platform objects to exclude")
    endif()
    configure_file("${CTRPORT_HOTPIN_LD}" "${CMAKE_CURRENT_BINARY_DIR}/hotpin.ld" @ONLY)
    ctrport_native_path(CTRPORT_HOTPIN_LD_NATIVE "${CMAKE_CURRENT_BINARY_DIR}/hotpin.ld")
    # The active pad already fills ctrCreateSpriteFromSurface's vacated tail.
    target_compile_definitions(ctrport PRIVATE CTRPORT_TPAGGROW_HOTPIN)
    string(REPLACE "-T ${CTRPORT_LATE_LD_NATIVE} " "-T ${CTRPORT_LATE_LD_NATIVE} -T ${CTRPORT_HOTPIN_LD_NATIVE} "
           CTRPORT_SPECS_NEW "${CTRPORT_SPECS_NEW}")
    # The pad's ASSERTs stop the link when a pinned function would start past its pin. check_hot.cmake then checks
    # that every hot function in the .hot list (engine and libraries included) is still at its address, and deletes
    # the ELF if one moved. Either way the build stops when a text change shifts the hot set.
    # CTRPORT_HOTPIN_CHECK=OFF skips the .hot check, only for the one build that regenerates the list.
    set(CTRPORT_HOTPIN_HOT "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/${GAME_DIR_NAME}-${CTRPORT_TARGET}.hot")
    option(CTRPORT_HOTPIN_CHECK "Check after the link that the pad's hot functions are at their listed addresses" ON)
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CTRPORT_HOTPIN_LD}" "${CMAKE_CURRENT_BINARY_DIR}/hotpin.ld")
    if(CTRPORT_HOTPIN_CHECK)
        if(NOT EXISTS "${CTRPORT_HOTPIN_HOT}")
            message(FATAL_ERROR "Hot-text alignment pad: hotpin/${GAME_DIR_NAME}-${CTRPORT_TARGET}.ld has no .hot list")
        endif()
        set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CTRPORT_HOTPIN_HOT}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/check_hot.cmake")
        add_custom_command(TARGET ctrport POST_BUILD
            COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:ctrport>" "-DNM=${CTRPORT_DKA_BIN}/arm-none-eabi-nm"
                    "-DLIST=${CTRPORT_HOTPIN_HOT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/check_hot.cmake"
            VERBATIM)
        message(STATUS "Hot-text alignment pad: hotpin/${GAME_DIR_NAME}-${CTRPORT_TARGET}.ld")
    else()
        message(STATUS "Hot-text alignment pad: hotpin/${GAME_DIR_NAME}-${CTRPORT_TARGET}.ld (no .hot check)")
    endif()
else()
    message(STATUS "Hot-text alignment pad: none")
endif()
file(WRITE "${CTRPORT_SPECS}.new" "${CTRPORT_SPECS_NEW}")
configure_file("${CTRPORT_SPECS}.new" "${CTRPORT_SPECS}" COPYONLY)

# Qualification explicitly opts in; the player/builder path never installs this command by default.
set(ut_hot_list "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/undertale-${CTRPORT_TARGET}.hot")
set(ut_hot_check "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/check_ut_hot.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/hotpin/ut_hot_policy.cmake")
ut_hot_eligible(ut_hot_enabled ut_hot_skip)
if(ut_hot_enabled)
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${ut_hot_list}" "${ut_hot_check}")
    add_custom_command(TARGET ctrport POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:ctrport>"
                "-DNM=${CTRPORT_DKA_BIN}/arm-none-eabi-nm" "-DLIST=${ut_hot_list}"
                -P "${ut_hot_check}"
        VERBATIM)
    message(STATUS "UT release hot-symbol guard: ${ut_hot_list}")
elseif(CTRPORT_UT_HOTCHECK AND ut_hot_skip)
    message(STATUS "UT release hot-symbol guard: skipped (${ut_hot_skip})")
endif()

# ===[ .3dsx ]===
# Placeholder art drawn by assets/make_art.py: one icon and banner per game and target.
if(CTRPORT_GAME STREQUAL "undertale")
    set(APP_NAME "UNDERTALE")
else()
    set(APP_NAME "deltarune-3ds (unofficial)")
endif()
set(CTRPORT_3DSX_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${RELEASE_PACKAGE_STEM}.3dsx")
include("${CMAKE_CURRENT_SOURCE_DIR}/legal.cmake")
# Description and build identity are generated only in SMDH, never in the ELF.
set(CTRPORT_SMDH_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/smdh.cmake")
set(CTRPORT_SMDH_ARGS "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/../.." "-DGAME=${CTRPORT_GAME}"
    "-DMODEL=${CTRPORT_TARGET}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}")
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-${GAME_DIR_NAME}-${CTRPORT_TARGET}.png")
set(APP_BANNER "${CMAKE_CURRENT_SOURCE_DIR}/assets/banner-${GAME_DIR_NAME}-${CTRPORT_TARGET}.png")

# Enabled by build.sh only for the measured GOG data digest; Steam retains the original link and bodies.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_GOG_COMPAT)
    target_compile_definitions(ctrport PRIVATE CTRPORT_GOG_COMPAT)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrChapters_init,--wrap=CtrChapters_returnToLauncher,--wrap=CtrPack_romfsName)
endif()
# datarefuse (change auditfixe, playblocker #8; R215c, maintainer 11-05): UNDERTALE builds only from an unmodified Steam or
# GOG data.win. build.sh passes that file's size after tools/aot/data_versions.py --build-input checked it against the
# known rows (and refused anything else); the data-version tables then keep only that row (DATA_VERSION_SIZE,
# source/ctr_data_version.h), so any other data.win, another known copy too, is unknown on the console: the RomFS
# tables are made from the input. main.c's data-version notice goes to source/z_datarefuse.c (its one CtrNotice_report
# call; main.c's code changes only that branch target): a warning the player continues past with A, or a stop when
# data.win can't be read. Empty (no data found when building): every known row stays.
set(CTRPORT_DATA_SIZE "" CACHE STRING "Size of the UNDERTALE data.win the build was made from (build.sh; empty: any known one)")
if(CTRPORT_GAME STREQUAL "undertale")
    if(CTRPORT_DATA_SIZE)
        if(NOT CTRPORT_DATA_SIZE MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "CTRPORT_DATA_SIZE must be a data.win size in bytes, not '${CTRPORT_DATA_SIZE}'")
        endif()
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_data_version.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/source/z_gogut.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_DATA_SIZE=${CTRPORT_DATA_SIZE}ULL)
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrNotice_report=CtrDataRefuse_report)
    # crash.txt and each hang.txt report start with the unsupported data.win's line (R243b/R243c): the three report
    # writers' file writes go to z_datarefuse.c, which writes the line ahead of a report. Only these call targets change.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_argcrash.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_lab.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_watchdog.c" APPEND PROPERTY COMPILE_DEFINITIONS
        FSFILE_Write=CtrDataRefuse_reportWrite)
    # An unknown data.win never gets offkill.bin/offdoom.bin's early destroys (build-input object numbers, z_datarefuse.c).
    target_link_options(ctrport PRIVATE -Wl,--wrap=OffKill_roomChange)
    # Test builds only: the warning saves both screens and reads A as pressed (datawarntest.txt, z_datarefuse.c).
    option(CTRPORT_DATAWARN_TEST "UNDERTALE: the data.win warning's screen capture and A press (test builds)" OFF)
    if(CTRPORT_DATAWARN_TEST)
        if(CTRPORT_PLAYER_SHOTS)
            message(FATAL_ERROR "CTRPORT_DATAWARN_TEST and shots builds both wrap hidKeysDown")
        endif()
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_datarefuse.c" APPEND PROPERTY COMPILE_DEFINITIONS
            DATAREFUSE_WARN_TEST)
        target_link_options(ctrport PRIVATE -Wl,--wrap=hidKeysDown)
    endif()
endif()
set(APP_BANNER_AUDIO "${CMAKE_CURRENT_SOURCE_DIR}/assets/banner.wav")

# Check at every build, including after a commit without reconfiguration. The
# helper rewrites its output only when metadata, icon or packaging tool changes.
add_custom_target(ctrport_smdh
    COMMAND "${CMAKE_COMMAND}" ${CTRPORT_SMDH_ARGS} "-DMODE=3dsx" "-DTOOL=${CTR_SMDHTOOL_EXE}"
        "-DNAME=${APP_NAME}" "-DICON=${APP_ICON}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh" -P "${CTRPORT_SMDH_SCRIPT}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh"
    DEPENDS ctrport_build_id "${APP_ICON}" "${CTRPORT_SMDH_SCRIPT}"
    VERBATIM)
set(CTRPORT_ROMFS_RAW "${CMAKE_CURRENT_BINARY_DIR}/canonical.romfs")
set(CTRPORT_ROMFS_IVFC "${CMAKE_CURRENT_BINARY_DIR}/canonical.ivfc")
if(CTRPORT_AOT_MODULES)
    include("${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_modules.cmake")
else()
    # ctr_create_3dsx accepts directories only; feed the shared canonical image directly.
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}"
        COMMAND "${CTR_3DSXTOOL_EXE}" "$<TARGET_FILE:ctrport>" "${CTRPORT_3DSX_OUTPUT}"
                "--smdh=${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh" "--romfs=${CTRPORT_ROMFS_RAW}"
        DEPENDS ctrport "${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh" "${CTRPORT_ROMFS_RAW}"
        VERBATIM)
    add_custom_target(ctrport_3dsx ALL DEPENDS "${CTRPORT_3DSX_OUTPUT}")
    dkp_set_target_file(ctrport_3dsx "${CTRPORT_3DSX_OUTPUT}")
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND
        DEPENDS "${CTRPORT_ROMFS_DIR}/bench_scenes.txt" "${COVERAGE_ROMFS_FILE}")
endif()
if(BTNPROMPT_ROMFS_FILES)
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS ${BTNPROMPT_ROMFS_FILES})
endif()
if(LOADSCREEN_ROMFS_FILES)
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS ${LOADSCREEN_ROMFS_FILES})
endif()
# repack (ctr_repack.inc, DELTARUNE on New 3DS) moves the new room's tile marks onto its new tiles before the room
# change's hidden load (z_roomload.c's call), so that load reads them. On UNDERTALE utstall wraps the same call.
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_roomLoadFinish)
endif()
# Test builds (build.sh repacktest=<folder>): repack's files (tools/repack/repack.py, made from the player's own cache)
# go into romfs/repack/<data.win folder>/, and diag/ctr_repack_test.c copies them into the cache folder at startup
# (repack.bin as repack.tst, the name test builds read; its sections are last in late text), so a
# hardware row needs no SD step. Never in a release build; built from files outside the repository.
set(CTRPORT_REPACK_TEST_DIR "" CACHE PATH "Folder of <data.win folder>/ repack files for a test build's RomFS (build.sh repacktest=)")
file(REMOVE_RECURSE "${CTRPORT_ROMFS_DIR}/repack")
if(CTRPORT_REPACK_TEST_DIR)
    if(NOT IS_DIRECTORY "${CTRPORT_REPACK_TEST_DIR}")
        message(FATAL_ERROR "repacktest: ${CTRPORT_REPACK_TEST_DIR} is not a folder")
    endif()
    file(GLOB_RECURSE REPACK_TEST_SOURCES RELATIVE "${CTRPORT_REPACK_TEST_DIR}" "${CTRPORT_REPACK_TEST_DIR}/*/*")
    set(REPACK_TEST_ROMFS_FILES "")
    foreach(f IN LISTS REPACK_TEST_SOURCES)
        configure_file("${CTRPORT_REPACK_TEST_DIR}/${f}" "${CTRPORT_ROMFS_DIR}/repack/${f}" COPYONLY)
        list(APPEND REPACK_TEST_ROMFS_FILES "${CTRPORT_ROMFS_DIR}/repack/${f}")
    endforeach()
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS ${REPACK_TEST_ROMFS_FILES})
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_REPACK_TEST=1)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_repack_test.c")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_repack_test.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_REPACK_TEST=1)
endif()
add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS ${RELEASE_LEGAL_FILES})
# The font and item verdict tables (romfs/fonts/*.dtfv, *.dtiv): build.sh writes them after configuring, with
# tools/aot/fontverdict.py, which rewrites fonts-inputs.txt beside the RomFS whenever it rewrites them. A new table
# repacks the .3dsx and the .cia. Made empty here when missing, so that a build without them has the file too.
set(CTRPORT_FONT_TABLES_STAMP "${CMAKE_CURRENT_BINARY_DIR}/fonts-inputs.txt")
if(NOT EXISTS "${CTRPORT_FONT_TABLES_STAMP}")
    file(WRITE "${CTRPORT_FONT_TABLES_STAMP}" "")
endif()
add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${CTRPORT_FONT_TABLES_STAMP}")
add_dependencies(ctrport_3dsx legal_texts ctrport_smdh)

# ===[ .cia ]===
# makerom (Project_CTR) and bannertool are not part of devkitPro; docs/building-cia.md says where to get them. Without
# them the `cia` target only prints a note, so the .3dsx build is unaffected.
find_program(MAKEROM_EXE NAMES makerom HINTS "${DEVKITPRO}/tools/bin")
find_program(BANNERTOOL_EXE NAMES bannertool HINTS "${DEVKITPRO}/tools/bin")
set(CIA_RSF "${CMAKE_CURRENT_SOURCE_DIR}/cia/${CTRPORT_TARGET}.rsf")
set(CIA_EXTRA_DEPENDS "")
# Use the same sorted payload as the 3dsx, wrapped in an IVFC hash tree.
set(CIA_EXTRA_DEPENDS ${CTRPORT_AOT_MODULE_FILES} "${CTRPORT_ROMFS_DIR}/bench_scenes.txt"
    "${COVERAGE_ROMFS_FILE}" ${BTNPROMPT_ROMFS_FILES} ${LOADSCREEN_ROMFS_FILES} "${CTRPORT_FONT_TABLES_STAMP}"
    ${REPACK_TEST_ROMFS_FILES})
set(CIA_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${RELEASE_PACKAGE_STEM}.cia")

if(MAKEROM_EXE)
    # makerom v0.19.0 drops the minor part of the title version (Project_CTR issue #169), so 0.1.0 comes out as 0.0.0.
    execute_process(COMMAND "${MAKEROM_EXE}" OUTPUT_VARIABLE MAKEROM_BANNER ERROR_VARIABLE MAKEROM_BANNER)
    if(MAKEROM_BANNER MATCHES "MAKEROM v([0-9.]+)")
        set(MAKEROM_VERSION "${CMAKE_MATCH_1}")
        if(MAKEROM_VERSION VERSION_EQUAL 0.19.0)
            message(WARNING "makerom ${MAKEROM_VERSION} writes a wrong .cia title version; use v0.18.4")
        endif()
    endif()
endif()

if(MAKEROM_EXE AND BANNERTOOL_EXE)
    set(CIA_SMDH_FLAGS "visible,allow3d,recordusage")
    if(CTRPORT_TARGET STREQUAL "n3ds")
        # The Old 3DS HOME Menu refuses to start it instead of running it in a memory mode it wasn't built for.
        string(APPEND CIA_SMDH_FLAGS ",new3ds")
    endif()

    add_custom_target(ctrport_cia_icon
        COMMAND "${CMAKE_COMMAND}" ${CTRPORT_SMDH_ARGS} "-DMODE=cia" "-DTOOL=${BANNERTOOL_EXE}"
            "-DNAME=${APP_NAME}" "-DICON=${APP_ICON}" "-DFLAGS=${CIA_SMDH_FLAGS}"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/ctrport.icn" -P "${CTRPORT_SMDH_SCRIPT}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/ctrport.icn"
        DEPENDS ctrport_build_id "${APP_ICON}" "${CTRPORT_SMDH_SCRIPT}"
        VERBATIM)
    add_custom_command(OUTPUT ctrport.bnr
        COMMAND "${BANNERTOOL_EXE}" makebanner -i "${APP_BANNER}" -a "${APP_BANNER_AUDIO}" -o ctrport.bnr
        DEPENDS "${APP_BANNER}" "${APP_BANNER_AUDIO}"
        VERBATIM
    )
    add_custom_command(OUTPUT "${CIA_OUTPUT}"
        COMMAND "${MAKEROM_EXE}" -f cia -o "${CIA_OUTPUT}" -target t -exefslogo
                -rsf "${CIA_RSF}" -elf "$<TARGET_FILE:ctrport>"
                -icon ctrport.icn -banner ctrport.bnr -romfs "${CTRPORT_ROMFS_IVFC}"
                -major ${RELEASE_CIA_MAJOR} -minor ${RELEASE_CIA_MINOR} -micro ${RELEASE_CIA_PATCH}
                "-DAPP_TITLE=${CIA_TITLE}" "-DAPP_PRODUCT_CODE=${CIA_PRODUCT_CODE}" "-DAPP_UNIQUE_ID=${CIA_UNIQUE_ID}"
                "-DAPP_SYSTEM_MODE=${CIA_O3DS_MEMORY}" "-DAPP_SYSTEM_MODE_EXT=${CTRPORT_N3DS_MEMORY}"
        DEPENDS ctrport "${CIA_RSF}" "${CMAKE_CURRENT_BINARY_DIR}/ctrport.icn" "${CMAKE_CURRENT_BINARY_DIR}/ctrport.bnr"
                ${CIA_EXTRA_DEPENDS} ${RELEASE_LEGAL_FILES} "${CTRPORT_ROMFS_IVFC}"
        COMMENT "Building CIA ${CIA_OUTPUT}"
        VERBATIM
    )
    add_custom_target(cia DEPENDS "${CIA_OUTPUT}")
    add_dependencies(cia legal_texts ctrport_cia_icon)
    if(CTRPORT_AOT_MODULES)
        # The modules are linked by aotm_modules only: a parallel build would otherwise link them twice at once.
        add_dependencies(cia aotm_modules)
    endif()
else()
    set(CIA_MISSING "")
    if(NOT MAKEROM_EXE)
        list(APPEND CIA_MISSING makerom)
    endif()
    if(NOT BANNERTOOL_EXE)
        list(APPEND CIA_MISSING bannertool)
    endif()
    string(REPLACE ";" " and " CIA_MISSING "${CIA_MISSING}")
    set(CIA_SKIP_NOTE "CIA packaging skipped: ${CIA_MISSING} not found in ${DEVKITPRO}/tools/bin or PATH")
    message(WARNING "${CIA_SKIP_NOTE}. The .3dsx still builds.")
    add_custom_target(cia COMMAND "${CMAKE_COMMAND}" -E echo "${CIA_SKIP_NOTE}" VERBATIM)
endif()

# Choose the palette retry hooks for the Old binary once at link time.
# The New binary keeps main's calls and loading policy with no runtime test.
if(CTRPORT_TARGET STREQUAL "o3ds")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_beginFrame,--wrap=CtrTextures_lendBudget)
endif()

# Private Old reference: save the original renderer assembly.
include("${CMAKE_CURRENT_SOURCE_DIR}/screenmovestamp.cmake")
get_target_property(vine_delegate ctrport C_COMPILER_LAUNCHER)
if(NOT CTRPORT_PYTHON_EXECUTABLE)
  message(FATAL_ERROR "Diagnostic launcher requires the qualified host Python")
endif()
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/vine_old_observer.inc" "${CMAKE_CURRENT_SOURCE_DIR}/source/vine_new_retain.inc" "${CMAKE_CURRENT_SOURCE_DIR}/source/drpix_replay.inc")
# titancpu L1's late code, and the launcher's own routing tables (a changed route recompiles the renderer).
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/vine_titancpu.inc"
             "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/vine_old_observer_compile.py" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/vine_old_parent.py")
set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/vine_old_observer_compile.py;${vine_delegate};--")

# The room plan (roomplan): source/z_roomplan.c gives each room without a rooms.dtm record the plan's first tiles
# (tools/roomplan/build_plan.py, made by build.sh from the player's data.win), and leaves rooms still as planned out of
# rooms.dtm; phase 2 reads the scene's code entries back at the room change where the bytecode lives on file (Old).
# Late code only; wrapped, so the boot check, the texture cache and the tile reader keep their code. UNDERTALE:
# romfs:/roomplan/undertale.dtp, seeded after CtrTextures_init. DELTARUNE: romfs:/roomplan/<chapter folder>.dtp, one per
# chapter, seeded at CtrTextures_init's CtrMemory_setHeapReliever call (pixpress wraps CtrTextures_init there).
set(CTRPORT_ROOMPLAN_FILE "" CACHE FILEPATH "The UNDERTALE room plan for the RomFS (build.sh makes it; empty: none)")
set(CTRPORT_ROOMPLAN_DIR "" CACHE PATH "The DELTARUNE room plans (<chapter folder>.dtp) for the RomFS (build.sh makes them; empty: none)")
option(CTRPORT_ROOMPLAN_RECORDER "Record each room visit's draws, tiles and code reads (debug: the room plan campaign)" OFF)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomplan.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
if(CTRPORT_GAME STREQUAL "undertale")
    # CtrDataVersion_check: roomplan is a link of its chain (source/ctr_wrap_chain.h), wrapped once above.
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_init,--wrap=CtrTileReader_writeFile)
    # Phase 2: the scene's code entries are read back at the room change where the bytecode lives on file (Old).
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_prefetchRoom)
    if(CTRPORT_ROOMPLAN_FILE)
        configure_file("${CTRPORT_ROOMPLAN_FILE}" "${CTRPORT_ROMFS_DIR}/roomplan/undertale.dtp" COPYONLY)
        add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${CTRPORT_ROMFS_DIR}/roomplan/undertale.dtp")
    else()
        file(REMOVE "${CTRPORT_ROMFS_DIR}/roomplan/undertale.dtp")
    endif()
else()
    # CtrDataVersion_check: roomplan is a link of its chain (source/ctr_wrap_chain.h), wrapped once above.
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrMemory_setHeapReliever,--wrap=CtrTileReader_writeFile,--wrap=CtrRenderer_prefetchRoom)
    file(GLOB roomplan_stale "${CTRPORT_ROMFS_DIR}/roomplan/*.dtp")
    if(roomplan_stale)
        file(REMOVE ${roomplan_stale})
    endif()
    if(CTRPORT_ROOMPLAN_DIR)
        file(GLOB roomplan_files "${CTRPORT_ROOMPLAN_DIR}/*.dtp")
        foreach(roomplan_file IN LISTS roomplan_files)
            get_filename_component(roomplan_name "${roomplan_file}" NAME)
            configure_file("${roomplan_file}" "${CTRPORT_ROMFS_DIR}/roomplan/${roomplan_name}" COPYONLY)
            add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${CTRPORT_ROMFS_DIR}/roomplan/${roomplan_name}")
        endforeach()
    endif()
endif()
# The phase plan (phaseplan, DELTARUNE only; source/z_phaseplan.c, wired with RoomLoad_collect's wrap above).
set(CTRPORT_PHASEPLAN_FILE "" CACHE FILEPATH "The phase plan for the RomFS (build.sh makes it; empty: none)")
if(CTRPORT_PHASEPLAN_FILE AND CTRPORT_GAME STREQUAL "deltarune")
    configure_file("${CTRPORT_PHASEPLAN_FILE}" "${CTRPORT_ROMFS_DIR}/roomplan/phases.dtph" COPYONLY)
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${CTRPORT_ROMFS_DIR}/roomplan/phases.dtph")
else()
    file(REMOVE "${CTRPORT_ROMFS_DIR}/roomplan/phases.dtph")
endif()
# The sound plan (utsfx, UNDERTALE only): source/z_utsfx.c copies the sounds each scene plays into the SFX cache at
# the room change, in the frame that isn't shown (tools/roomplan/build_sfxplan.py, made by build.sh from the player's
# data.win, as romfs:/roomplan/undertale-sfx.dtp). Late code only; CtrRenderer_skipPresent is wrapped, so main.c keeps
# its code, and CtrDataVersion_check's chain has a link for it (source/ctr_wrap_chain.h).
set(CTRPORT_SFXPLAN_FILE "" CACHE FILEPATH "The UNDERTALE sound plan for the RomFS (build.sh makes it; empty: none)")
set(CTRPORT_LAYOUT_SCENES_FILE "" CACHE FILEPATH "The UNDERTALE scene table for the RomFS (build.sh makes it; empty: none)")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utsfx.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_freebies.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_skipPresent)
    # usstop (UT-002): shutdownAudio's CtrReadAhead_shutdown call first holds the sound plan's worker idle
    # (source/ctr_audio_system.c's end). Wrapped, so shutdownAudio keeps its code; DELTARUNE has no worker. A copy
    # still under way after its wait keeps the heap: the exit's owner decides at aptExit, and svcExitProcess calls
    # a return address set aside there (exit trail builds chain aptExit after z_exittrail.c's wrap).
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrReadAhead_shutdown,--wrap=aptExit,--wrap=svcExitProcess)
    # freebies (source/z_freebies.c): restarting the room into another planned scene gets the room load. Wrapped, so the
    # runner, main.c and z_utstall.c keep their code; DELTARUNE compiles the file empty and wraps nothing.
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_handlePendingRoomChange,--wrap=RoomPlan_utstallPlan)
    if(CTRPORT_TARGET STREQUAL "o3ds")
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_roomLoadedOld)
    endif()
    if(CTRPORT_SFXPLAN_FILE)
        configure_file("${CTRPORT_SFXPLAN_FILE}" "${CTRPORT_ROMFS_DIR}/roomplan/undertale-sfx.dtp" COPYONLY)
        add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${CTRPORT_ROMFS_DIR}/roomplan/undertale-sfx.dtp")
    else()
        file(REMOVE "${CTRPORT_ROMFS_DIR}/roomplan/undertale-sfx.dtp")
    endif()
endif()
# drresid (DELTARUNE New): source/z_drresid.c streams a fight's later tiles in ahead of their first draw, from
# romfs:/roomplan/tracks.dtp (tools/roomplan/build_tracks.py, made by build.sh). It wraps CtrTextures_beginFrame,
# which the New builds don't otherwise wrap (the Old builds do, for the palette retries; the recorder build does, and
# compiles drresid empty). Late code only; UNDERTALE and the Old models compile it empty.
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds" AND NOT CTRPORT_ROOMPLAN_RECORDER)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_beginFrame)
endif()
# flowerhw cold (DELTARUNE New; review decision 2026-11-03): on the first room after a front-end or game-over room (a save,
# chapter or retry load) the room load runs once more with up to 1 s more (z_drresid.c wraps CtrRoomload_prefetchRoom,
# which nothing else wraps there; the flowcode probe build does, so it goes without). OFF builds without it (an A arm).
option(CTRPORT_FLOWERHW_COLD "Finish the first room's seed after a save or chapter load, on DELTARUNE New" ON)
if(CTRPORT_FLOWERHW_COLD AND NOT CTRPORT_FLOWCODE AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds"
   AND NOT CTRPORT_ROOMPLAN_RECORDER)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_drresid.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_FLOWERHW_COLD)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRoomload_prefetchRoom)
endif()
# flowstream (UNDERTALE Old): source/z_flowstream.c, the background refill of plan tiles evicted in play (late code
# only; every other build compiles it empty). Small rather than fast: once a frame, and on the reader once a refill read.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowstream.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_drresid.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# flowstream (UNDERTALE Old; review decision 2026-10-29): the define that compiles z_flowstream.c. A refill's tile read is
# admitted with the promotion reserve (4 MiB there) and allocated with heap relief off: ctr_tile_reader.c's heapFree
# mallinfo, perform's header parse and its malloc call z_flowstream.c's late hooks instead (CTR_READ_*; every other build
# compiles the original calls). fs5 rev 2: its page file opens go through a hook too (the game thread's wait never
# opens or reads for the refill read), and ctr_textures.c gets CtrTextures_flowstreamInstall (late code at its end).
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_tile_reader.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowstream.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_textures.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FLOWSTREAM_READ)
endif()
# btlentry (UNDERTALE Old): source/z_btlentry.c reads a battle group's planned tiles on the reader during the encounter
# animation, before the room change (late code only; every other build compiles it empty). It wraps flowstream's
# once-a-frame call and its five reader hooks; the wrapped calls are in z_utstall.c and ctr_tile_reader.c. Round 5 reads
# the battle's code too, and wraps main.c's DataWin_moveBytecodeToFile call for the bytecode file's path.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_btlentry.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds" AND NOT CTRPORT_ROOMPLAN_RECORDER)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_btlentry.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FLOWSTREAM_READ)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrFlowstream_collect,--wrap=CtrFlowstream_readHeader,--wrap=CtrFlowstream_readMallinfo,--wrap=CtrFlowstream_readMalloc
        -Wl,--wrap=CtrFlowstream_gameFile,--wrap=CtrFlowstream_openFile,--wrap=DataWin_moveBytecodeToFile
        -Wl,--wrap=rename) # AUD-16 (z_btlentry.c's end): the bytecode file read back as it is written
    # utfade A (z_btlentry.c's end): a door's destination read during its fade-out. Two calls are pointed at it by name,
    # each unit's code the same size: z_utstall.c's once-a-frame call of flowstream (it calls btlentry's wrap on) and
    # ctr_textures.c's call of the room load in CtrTextures_roomLoadedOld (it calls z_utstall.c's wrap on).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utstall.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrFlowstream_collect=CtrUtfadeA_collect)
    # entryload (ctr_textures.c's end): z_utstall.c's calls of the real CtrTextures_roomLoadFinish go to entryload's
    # by name, which makes that call and then keeps the dialogue fonts' tiles packed in the heap; a draw's
    # CtrTextures_ensureTiles unpacks a missing one from there. flowdiag and frametrace wrap those calls themselves, so
    # their builds keep nothing.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utstall.c" APPEND PROPERTY COMPILE_DEFINITIONS
        __real_CtrTextures_roomLoadFinish=CtrEntryload_roomLoadFinish)
    if(NOT CTRPORT_FLOWSTREAM_DIAG AND NOT CTRPORT_FRAMETRACE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_textures.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_ENTRYLOAD_TILES)
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_ensureTiles,--wrap=CtrTextures_ensureTilesBut)
        # tilenames (change tilemiss, 11-08): entryload's wraps call z_utstall.c's in place of the real ensure functions
        # (each call the same size), which call the real ones and log a line for each tile a draw reads from the card.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_textures.c" APPEND PROPERTY COMPILE_DEFINITIONS
            __real_CtrTextures_ensureTilesBut=CtrTilenames_ensureTilesBut __real_CtrTextures_ensureTiles=CtrTilenames_ensureTiles)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utstall.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_TILENAMES)
    endif()
    # bthand (change tilemiss, 11-08; z_btlentry.c's end): that call goes to bthand's first, which hands btlentry's
    # finished reads to the battle room's load and then calls utfade A's (the same size).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_textures.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrRoomload_prefetchRoom=CtrBtHand_roomLoad)
    # r220 F1: btlentry's calls of the reader's CtrTileReader_finished go to utfade A's (the same size, in its slot), so
    # its once-a-frame installs never take a door read; utfade A reaches the reader's own by its assembler name.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_btlentry.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrTileReader_finished=CtrUtfadeA_finished)
endif()
# leftcode (change auditfixe R295, UNDERTALE Old): after the whole room load, the room's object code its step 1 left on a
# slow card (z_roomload.c's end, CtrLeftcode_roomLoaded, which calls CtrTextures_roomLoadedOld first). ctr_argui.c's call
# is pointed at it by name, the same size; nothing else changes.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrTextures_roomLoadedOld=CtrLeftcode_roomLoaded)
endif()
# menufirst (change menufirst, 11-07; UNDERTALE): the options menu's two sounds go into the SFX cache at each room load,
# the first behind the loading bar, so its first open sounds at once (ctr_optmenu.c's end, CtrOptmenu_prepareRoom, which
# calls CtrOptmenu_prepareGlyphs first). ctr_argui.c's call is pointed at it by name, the same size; nothing else changes.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrOptmenu_prepareGlyphs=CtrOptmenu_prepareRoom)
    # R420: credwait's vblank wait for a moved Legal or Credits page (z_credclip.c's one call) goes through
    # CtrOptmenu_credWait, which notes it, so optFlush's copy of those pages doesn't wait a second time that frame.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_credclip.c" APPEND PROPERTY COMPILE_DEFINITIONS
        gspWaitForEvent=CtrOptmenu_credWait)
endif()
# LAB ONLY, never release (review decision 11-02 on btlentry's reserve): the smallest heap free per window around a battle's
# start, after every heap allocation (newlib's allocators wrapped); with FAULT, btlentry's reads cycle through refused,
# failed and dropped outcomes, for the fallback's identity gate.
option(CTRPORT_BTLENTRY_DIAG "LAB ONLY btlentry heap free proof, never release" OFF)
option(CTRPORT_BTLENTRY_FAULT "LAB ONLY btlentry read faults, never release" OFF)
if(CTRPORT_BTLENTRY_DIAG OR CTRPORT_BTLENTRY_FAULT)
    if(CTRPORT_RELEASE_SOURCE_URL OR CTRPORT_HEAP_PROFILE OR CTRPORT_AUDIO_METRICS)
        message(FATAL_ERROR "btlentry's diag is lab only, and wraps newlib's allocators as the heap profiler and audiometrics do")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_btlentry.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BTLENTRY_DIAG)
    if(CTRPORT_BTLENTRY_FAULT)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_btlentry.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BTLENTRY_FAULT)
    endif()
    target_link_options(ctrport PRIVATE -Wl,--wrap=_malloc_r,--wrap=_calloc_r,--wrap=_realloc_r,--wrap=_memalign_r)
endif()
if(CTRPORT_FLOWSTREAM_DIAG)
    if(CTRPORT_FRAMETRACE OR CTRPORT_ROOMPLAN_RECORDER)
        message(FATAL_ERROR "flowdiag, frametrace and the room plan recorder all wrap CtrTextures_ensureTiles")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowdiag.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowstream.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FLOWSTREAM_DIAG)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowdiag.c" PROPERTIES COMPILE_OPTIONS
        "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_ensureTiles,--wrap=CtrTextures_ensureTilesBut)
endif()
if(CTRPORT_ROOMPLAN_RECORDER)
    # The Old builds wrap CtrTextures_beginFrame for the palette retries (above); the campaign runs on the New model.
    if(NOT CTRPORT_TARGET STREQUAL "n3ds")
        message(FATAL_ERROR "the room plan recorder is for n3ds builds")
    endif()
    target_compile_definitions(ctrport PRIVATE CTRPORT_ROOMPLAN_RECORDER)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_beginFrame,--wrap=CtrTextures_ensureTiles,--wrap=CtrTextures_ensureTilesBut,--wrap=Runner_create)
endif()

# cut10: 1.0 has Standard / Normal only, including legacy --ini values. Keep
# the retained parser/default bodies and all hot renderer code in place.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDisplay_defaults)
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_screens.c" APPEND PROPERTY
    COMPILE_DEFINITIONS "__real_CtrDisplay_override=CtrCut10_override")
# cut10 + benchfid: the screens wrapper now hands --ini lines to benchfid's override first
# (z_benchfid_window.c), whose fallthrough must still reach cut10's normalization, not the raw parser.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_benchfid_window.c" APPEND PROPERTY
    COMPILE_DEFINITIONS "__real_CtrDisplay_override=CtrCut10_override")
# Approved Crisp sprite art (crispart): exact-sized chapter packs and late hooks only. Existing wrappers chain
# through ctr_wrap_chain.h / optmenu's next-link aliases. UNDERTALE compiles z_crispart.c empty.
set(CTRPORT_CRISPART_DIR "" CACHE PATH "Verified chapter .dtca packs made by build.sh")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_crispart.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_loadFontVerdicts)
    # crispart's next link (symverdict's on New 3DS, then itemverdict's) is named in source/ctr_wrap_chain.h.
    file(GLOB crispart_stale "${CTRPORT_ROMFS_DIR}/art/*.dtca")
    if(crispart_stale)
        file(REMOVE ${crispart_stale})
    endif()
    if(CTRPORT_CRISPART_DIR)
        file(GLOB crispart_files "${CTRPORT_CRISPART_DIR}/*.dtca")
        foreach(crispart_file IN LISTS crispart_files)
            get_filename_component(crispart_name "${crispart_file}" NAME)
            set(crispart_dest "${CTRPORT_ROMFS_DIR}/art/${crispart_name}")
            configure_file("${crispart_file}" "${crispart_dest}" COPYONLY)
            add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${crispart_dest}")
            if(MAKEROM_EXE AND BANNERTOOL_EXE)
                add_custom_command(OUTPUT "${CIA_OUTPUT}" APPEND DEPENDS "${crispart_dest}")
            endif()
        endforeach()
    endif()
endif()

# inputlate: the accepted HID latch, all new code/state late. Existing display,
# menu and watchdog links keep their bodies; downstream order is in ctr_wrap_chain.h.
if(CTRPORT_GAME STREQUAL "undertale")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_input.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c"
    PROPERTIES COMPILE_OPTIONS "-Os;-fno-builtin;-fno-tree-switch-conversion;-fno-jump-tables")
set(CTRPORT_INPUTLATE_PERIOD_MS "16" CACHE STRING "HID sampler interval; 0/8 are cost diagnostics only")
set(CTRPORT_INPUTLATE_STALL_MS "0" CACHE STRING "Diagnostic Core 1 worker-read blackout, once after 120 main drains")
if(NOT CTRPORT_INPUTLATE_PERIOD_MS MATCHES "^(0|8|16)$" OR
   NOT CTRPORT_INPUTLATE_STALL_MS MATCHES "^[0-9]+$" OR CTRPORT_INPUTLATE_STALL_MS GREATER 1000)
    message(FATAL_ERROR "Invalid inputlate diagnostic interval or stall (0..1000 ms)")
endif()
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_input.c" APPEND PROPERTY COMPILE_DEFINITIONS
    "CTRPORT_INPUTLATE_PERIOD_MS=${CTRPORT_INPUTLATE_PERIOD_MS}"
    "CTRPORT_INPUTLATE_STALL_MS=${CTRPORT_INPUTLATE_STALL_MS}")
target_link_options(ctrport PRIVATE
    -Wl,--wrap=CtrBench_sessionStarted,--wrap=CtrRenderer_beginFrame,--wrap=CtrRenderer_endFrame
    -Wl,--wrap=CtrBench_beginFrameTimed,--wrap=hidKeysHeld
    -Wl,--wrap=RunnerKeyboard_onKeyDown,--wrap=RunnerKeyboard_onKeyUp,--wrap=Runner_step
    -Wl,--wrap=InputRecording_createPlayer,--wrap=InputRecording_processFrame,--wrap=CtrChapters_takeGameChange,--wrap=_exit)
# The diagnostic recorder and the input latch both observe bench begin. Keep inputlate's wrapper outermost:
# recorder -> real begin -> recorder accounting -> inputlate pause latch. Release preprocessing is unchanged.
if(CTRPORT_TITANDIAG)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CTRPORT_TITANDIAG_INPUTLATE_CHAIN)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_titandiag.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__wrap_CtrBench_beginFrameTimed=Titandiag_beginFrameTimed")
endif()
endif()

# postkick (change pacer): a frame that changes no room runs main.c's UTSW_ROOMCHANGE after the GPU has the frame
# (source/z_postkick.c; the endFrame chain's last link, ctr_wrap_chain.h). Its own block, after inputlate's link
# options (the same order on the link line), so inputlate's block keeps one target_link_options call
# (checked by a link-order test).
if(CTRPORT_GAME STREQUAL "undertale")
target_link_options(ctrport PRIVATE -Wl,--wrap=Utsw_handleRoomChange)
endif()

# build.sh refreshes this after every RomFS producer, including fonts. This covers added/removed
# chapter modules, plans and font verdicts as well as changes to files known at configure time.
set(ROMFS_CONTENT_STAMP "${CMAKE_CURRENT_BINARY_DIR}/romfs-content.stamp")
if(NOT EXISTS "${ROMFS_CONTENT_STAMP}")
    file(WRITE "${ROMFS_CONTENT_STAMP}" "")
endif()
add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${ROMFS_CONTENT_STAMP}")
if(MAKEROM_EXE AND BANNERTOOL_EXE)
    add_custom_command(OUTPUT "${CIA_OUTPUT}" APPEND DEPENDS "${ROMFS_CONTENT_STAMP}")
endif()

# Both external packers enumerate directories in host order. Serialize once with
# an explicit UTF-16 key; no timestamp, locale or filesystem-order dependency.
add_custom_command(OUTPUT "${CTRPORT_ROMFS_RAW}" "${CTRPORT_ROMFS_IVFC}"
    COMMAND "${CTRPORT_PYTHON_EXECUTABLE}" -X utf8 -I "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/romfs.py"
            --source "${CTRPORT_ROMFS_DIR}" --raw "${CTRPORT_ROMFS_RAW}" --ivfc "${CTRPORT_ROMFS_IVFC}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/romfs.py"
            "${ROMFS_CONTENT_STAMP}" "${CTRPORT_FONT_TABLES_STAMP}"
            ${CTRPORT_AOT_MODULE_FILES} ${RELEASE_LEGAL_FILES}
            "${CTRPORT_ROMFS_DIR}/bench_scenes.txt" "${COVERAGE_ROMFS_FILE}"
            ${BTNPROMPT_ROMFS_FILES} ${LOADSCREEN_ROMFS_FILES}
    COMMENT "Packing sorted RomFS for both formats"
    VERBATIM)
add_custom_target(ctrport_romfs DEPENDS "${CTRPORT_ROMFS_RAW}" "${CTRPORT_ROMFS_IVFC}")
add_dependencies(ctrport_romfs legal_texts)
if(CTRPORT_AOT_MODULES)
    add_dependencies(ctrport_romfs aotm_modules)
endif()
add_dependencies(ctrport_3dsx ctrport_romfs)
if(MAKEROM_EXE AND BANNERTOOL_EXE)
    add_dependencies(cia ctrport_romfs)
endif()

# A public UNDERTALE build (CTRPORT_RELEASE_SOURCE_URL: every builder and release build) has no Performance test in
# its options menu (maintainer 11-04): the Debug page is Stats and log and Back (ctr_optmenu.c), and the review adapter
# greys no row (z_builderx_review.c). Only these two objects see the define, so lab and development builds compile
# as before and keep the test.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_RELEASE_SOURCE_URL)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_builderx_review.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_PUBLIC_MENU)
    # Same public-build policy: ignore leftover lab file triggers, retaining explicit --bench.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_public_benchfile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_PUBLIC_BENCH_FILE)
    target_link_options(ctrport PRIVATE -Wl,--wrap=bootHookStat)

endif()
# Keep UT's release-only performance-test menu guard; DR uses upstream's exact runtime.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(builderx_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py;${builderx_delegate};--builderx-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c"
        APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py")
endif()
# DR compact adapters: only main's three cold calls change; UT's compiler path stays exactly as qualified.
if(CTRPORT_GAME STREQUAL "deltarune")
    get_target_property(builderx_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py;${builderx_delegate};--builderx-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c"
        APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py")
endif()

# Lab-only Azahar SD floors, selected by benchrun.txt. No code or link wrap in
# ordinary Release; no existing function body or compiler line number changes.
option(CTRPORT_SDLAT "Include the lab SD hook in profile builds (OFF: layout reference)" ON)
if(CTRPORT_PROFILE AND CTRPORT_SDLAT)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_sdlat.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_sdlat.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-jump-tables")
    # CtrOpenFiles_track is wrapped by gamedir in every build; sdlat is the next link of its chain (ctr_wrap_chain.h).
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrBench_autorun,--wrap=CtrPacer_logStats)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_gamedir.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CTRPORT_SDLAT_CHAIN)
endif()

# uttc (change uthunt T1; build.sh "uttc=off" / "uttc-force", test builds only): without the tile pre-filter (runner.h's
# UTTC_ON), or with its fallbacks forced (runner.c's end).
set(CTRPORT_UTTC_MODE "" CACHE STRING "uttc test mode: empty, OFF or FORCE")
if(CTRPORT_UTTC_MODE STREQUAL "OFF")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_UTTC_OFF)
elseif(CTRPORT_UTTC_MODE STREQUAL "FORCE")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_UTTC_FORCE)
endif()

# dprank (change uthunt; build.sh "dprank=off" / "dprank-diag", test builds only): drawPassFind always takes the scan, or
# also counts its rank hits and fallbacks into the log (runner.c, below drawPassFirst).
set(CTRPORT_DPRANK_MODE "" CACHE STRING "dprank test mode: empty, OFF or DIAG")
if(CTRPORT_DPRANK_MODE STREQUAL "OFF")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DPRANK_OFF)
elseif(CTRPORT_DPRANK_MODE STREQUAL "DIAG")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DPRANK_DIAG)
endif()

if(CTRPORT_FREEWINS)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_freewins.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" "${ENGINE_DIR}/src/runner.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FREEWINS)
endif()

# The spike recorder (change spikerec; source/z_spikerec.c says how it works): the main loop's watchdog marks, its
# room-change call and the game thread's SD card reads are wrapped, so main.c and every existing function keep their
# code and addresses; only those call targets change. Its define reaches only the two files that use it (z_spikerec.c,
# and ctr_audio_system.c's accessor at its end), so every other object is the release build's. Compiled like
# ctr_screens.c (named-array strings, no folded printf calls or switch tables), and for size: it runs once a frame.
if(CTRPORT_SPIKEREC)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_spikerec.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_SPIKEREC)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_spikerec.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrWatchdog_mark,--wrap=CtrBench_handleRoomChange,--wrap=FSFILE_Read)
endif()
# frametrace (change benchfix; source/z_frametrace.c says what it records): the main loop's watchdog marks, the SD and
# RomFS calls, audio decode reads and the renderer's tile loads are wrapped, so every existing function keeps its code
# and address; only those call targets change. Its define reaches only the three files that use it (z_frametrace.c, and
# ctr_benchmark.c's and ctr_frame_pacer.c's accessors at their ends). ctr_audio_system.c's own CtrDecoder_read wrap
# (flowend's, unconditional) is renamed there, and z_frametrace.c's wrap calls it. Compiled like z_spikerec.c.
if(CTRPORT_FRAMETRACE)
    if(CTRPORT_SPIKEREC)
        message(FATAL_ERROR "frametrace and spikerec both wrap CtrWatchdog_mark and FSFILE_Read")
    endif()
    if(CTRPORT_BENCHFIX_PROBE)
        message(FATAL_ERROR "frametrace and bfprobe both wrap the FSUSER and FSFILE calls")
    endif()
    if(CTRPORT_ROOMPLAN_RECORDER)
        message(FATAL_ERROR "frametrace and the room plan recorder both wrap CtrTextures_ensureTiles")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_frametrace.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_frame_pacer.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FRAMETRACE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__wrap_CtrDecoder_read=ftInner_CtrDecoder_read")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_frametrace.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrWatchdog_mark,--wrap=FSUSER_OpenFile,--wrap=FSUSER_OpenFileDirectly,--wrap=FSUSER_RenameFile
        -Wl,--wrap=FSUSER_DeleteFile,--wrap=FSFILE_Read,--wrap=FSFILE_Write,--wrap=FSFILE_Flush,--wrap=FSFILE_Close
        -Wl,--wrap=CtrTextures_ensureTiles,--wrap=CtrTextures_ensureTilesBut,--wrap=CtrBench_takeFinished)
endif()
# The Titan CPU recorder (change titancpu; source/z_titandiag.c says how it works): the main loop's watchdog marks, the
# bench's per-frame calls, the runner's profiler zones, its event calls into the VM and the render target calls are
# wrapped, so every existing function keeps its code; only those call targets change. Diagnosis builds only.
if(CTRPORT_TITANDIAG)
    if(CTRPORT_FRAMETRACE)
        message(FATAL_ERROR "titandiag and frametrace both wrap CtrWatchdog_mark")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_titandiag.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TITANDIAG)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_titandiag.c" APPEND PROPERTY COMPILE_OPTIONS "-O2;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrWatchdog_mark,--wrap=CtrBench_sessionStartedLate,--wrap=CtrBench_beginFrameTimed,--wrap=CtrBench_endFrameTimed,--wrap=Profiler_enterKind,--wrap=Profiler_exit,--wrap=VM_beginEventRun,--wrap=VM_endEventRun,--wrap=VM_runEvent,--wrap=VM_executeCode,--wrap=C3D_RenderTargetDelete,--wrap=CtrPacer_waitForSlot,--wrap=CtrPacer_levelWait)
endif()
# The exit trail (source/z_exittrail.c says what it marks): calls into the exit's steps are wrapped, so every existing
# function keeps its code; only those call targets change.
if(CTRPORT_EXITTRAIL)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_exittrail.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrExit_closeFilesLog,--wrap=threadJoin,--wrap=fsExit,--wrap=aptExit,--wrap=srvExit,--wrap=__appExit,--wrap=__libctru_exit,--wrap=CtrLinkFiles_enable,--wrap=CtrExit_flush,--wrap=CtrExitOnce_chain,--wrap=CtrLinkFiles_prepare,--wrap=CtrLab_crashTest)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_leavekeep.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_EXITTRAIL)
    # usstop's provoke test (--ini usprovoke=1-12 or usprovoke.txt, UNDERTALE): source/ctr_audio_system.c's end.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_EXITTRAIL)
    if(CTRPORT_GAME STREQUAL "undertale")
        # usstop's provoke test: with usprovoke.txt, the run leaves after its bench (Azahar has no --exit-after).
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLaunchArgs_sendFilesMB,--wrap=envDestroyHandles,--wrap=srvGetServiceHandleDirect)
    endif()
endif()
# leavekeep (source/z_leavekeep.c says why): the lab leaves and filesRecover end without libctru's heap unmap (and,
# in exit trail builds only, --lab-crash can fire after NDSP starts: the CtrLab_crashTest wrap in the block above).
# svcControlMemory, not __libctru_exit, is wrapped: a wrap's __real_ reference
# from a change object pulls its libctru archive member earlier and moves libctru's text (svc.o is in the link already).
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_leavekeep.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
target_link_options(ctrport PRIVATE -Wl,--wrap=svcControlMemory)
# labguard (source/z_labguard.c; UNDERTALE non-public builds only, CTRPORT_LABGUARD_ON above): ctr_lab.c's leave calls
# (the renames below) and main.c's lab config (ctr_lab.h's CTR_LABGUARD_PICK) change only call targets; the
# rest is late code and these wraps (the log open, boot trail marks, which config starts the lab). Its --lab-crash
# kinds need CtrLab_crashTest's wrap, which the exit trail's own wrap takes in its builds.
if(CTRPORT_LABGUARD_ON)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_labguard.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_LABGUARD)
    # homesave must pause the startup fallback before its queued-save drain.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_sleepflush.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_LABGUARD)
    # ctr_lab.c's leave steps go through z_labguard.c: these names are renamed for that one file (as datarefuse's
    # FSFILE_Write is, and before its first line, so its declarations are renamed with its calls). Its source is
    # unchanged and every function keeps its code shape; only call targets change. fallbackMain's wait becomes the
    # steps' monitor; CtrSaveguard_exit and CtrSaveguard_labAppend are what R216's renames of CtrExitFiles_labExit and
    # LabProto_append lead to; datarefuse's report writer leads to the report file's stamp.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_lab.c" APPEND PROPERTY COMPILE_DEFINITIONS
        LightEvent_Signal=CtrLabguard_leaveSignal LightEvent_Wait=CtrLabguard_fallback
        CtrHbldr_setNext=CtrLabguard_setNext FSUSER_OpenFileDirectly=CtrLabguard_fsOpen
        FSFILE_SetSize=CtrLabguard_fsSetSize CtrDataRefuse_reportWrite=CtrLabguard_fsWrite
        FSFILE_Close=CtrLabguard_fsClose aptExit=CtrLabguard_aptExit CtrSaveguard_exit=CtrLabguard_labExit
        CtrSaveguard_labAppend=CtrLabguard_append)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_labguard.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-jump-tables;-fno-merge-constants")
    # datarefuse's report writer (every UNDERTALE build's FSFILE_Write define for ctr_lab.c, above) stays the next link.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_labguard.c" APPEND PROPERTY COMPILE_DEFINITIONS
        LG_FS_WRITE_NEXT=CtrDataRefuse_reportWrite)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrWatchdog_start,--wrap=CtrBootTrail_mark,--wrap=CtrLab_start)
    # labguard's own fault injection (--labguard-crash, --lab-crash stall-<step>) only with build.sh's "labcrash"
    # (lead 11-11, devdiet): no row uses it, so the plain dev build leaves it out and CtrLab_crashTest binds as in
    # release.
    if(CTRPORT_LABCRASH AND NOT CTRPORT_EXITTRAIL)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_labguard.c" APPEND PROPERTY COMPILE_DEFINITIONS LG_CRASHTEST)
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLab_crashTest)
        # --labguard-crash: argcrash's parse wrap calls labguard's argument filter next (ctr_wrap_chain.h).
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_argcrash.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_LABGUARD_ARGS)
    endif()
endif()
# utorder (source/z_utorder.c says why; UNDERTALE only): GameMaker 1.4's draw order at equal depth. Each instance's key
# is taken where it is made and where its depth changes, and main.c's draw calls keep the list first (no full rebuild
# by id); all are calls from other files, so wraps leave every existing function's code as it was (only the call
# targets change).
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utorder.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Instance_create,--wrap=Runner_moveInstanceToDepthLayer
        -Wl,--wrap=Runner_drawPre,--wrap=Runner_drawPost,--wrap=Runner_drawGUI)
endif()
# noshots (source/z_noshots.c says why; both games): a timing row (--bench noshots, which bench_io=0 adds) skips the
# scenes' "shot" steps. benchRoomsTakeShot's call goes to the wrap; no existing function's code changes.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_noshots.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
target_link_options(ctrport PRIVATE -Wl,--wrap=BenchScript_takeShot)
# exitonce (z_leavekeep.c): one thread runs the exit. UNDERTALE already wraps _exit (inputlate; ctr_wrap_chain.h
# puts exitonce after it); DELTARUNE has no other _exit link.
if(NOT CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=_exit)
endif()

# flowervramg round18: DR Old only, after roomload collection and before
# phaseplan/Step/Draw. UT's packed-tier sizing/admission belongs to utflowey.
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_phaseplan.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "__real_RoomLoad_collect=FvgPromotion_collect")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_flowervramg_promotion.c"
        PROPERTIES COMPILE_OPTIONS "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
endif()

# Palette priority belongs only to the separate DR Old 80 MB CIA layout.
# Ordinary HBL, UT and New keep their existing palette path and code bytes.
# The option's game/model/exheader validation above rejects other targets.
if(CTRPORT_DR_OLD_CIA80_TILES)
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrTextures_roomThrashing,--wrap=CtrTextures_lendBudgetInRoom
        -Wl,--undefined=CtrTextures_roomThrashing,--undefined=CtrTextures_lendBudgetInRoom)
endif()

set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/flowervramg_palette_retry.py")
# flowfull's exact New DR code acquisition probe; no renderer or VM instrumentation.
option(CTRPORT_FLOWCODE "Move recognized chapter-5 bytecode to exact hidden-room preloads" OFF)
set(CTRPORT_FLOWCODE_PLAN "" CACHE FILEPATH "Numeric exact-file FCD1 room code plan")
if(CTRPORT_FLOWCODE)
    if(NOT CTRPORT_GAME STREQUAL "deltarune" OR NOT CTRPORT_TARGET STREQUAL "n3ds" OR CTRPORT_ROOMPLAN_RECORDER OR CTRPORT_ASG_PROBE)
        message(FATAL_ERROR "flowcode requires normal DELTARUNE New without recorder/frame probe")
    endif()
    if(NOT EXISTS "${CTRPORT_FLOWCODE_PLAN}")
        message(FATAL_ERROR "Generate the exact-file flowcode plan first")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowcode.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_crispart.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FLOWCODE=1)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_flowcode.c" PROPERTIES COMPILE_OPTIONS
        "-Os;-fno-builtin;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=Runner_create,--wrap=Runner_free,--wrap=Runner_step
        -Wl,--wrap=DataWin_loadCodeBytes,--wrap=CtrBench_handleRoomChange,--wrap=CtrRoomload_prefetchRoom)
    set(flowcode_dest "${CTRPORT_ROMFS_DIR}/roomplan/chapter5-flowcode.bin")
    configure_file("${CTRPORT_FLOWCODE_PLAN}" "${flowcode_dest}" COPYONLY)
    add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${flowcode_dest}")
    if(MAKEROM_EXE AND BANNERTOOL_EXE)
        add_custom_command(OUTPUT "${CIA_OUTPUT}" APPEND DEPENDS "${flowcode_dest}")
    endif()
else()
    file(REMOVE "${CTRPORT_ROMFS_DIR}/roomplan/chapter5-flowcode.bin")
endif()

# Knight GPU-1 (change knightnew, round 21): the Knight's afterimage spill copies skip their unused native columns.
# build.sh turns it on for DELTARUNE New by default ("knightnew-history=off" leaves it out); the switch is accepted but
# empty in both UNDERTALE targets and DELTARUNE Old. Cache/preload experiments stay parked (drresid owns SD stalls).
option(CTRPORT_KNIGHTNEW_HISTORY "Trim Knight history spill padding on New DR" OFF)
# Knight GPU A (change knightgpu): with L-A on, a hot spilled surface that finds no VRAM gap of its size gets one by
# sliding a bank's surfaces together (ctr_renderer.c's and ctr_memory.c's ends). build.sh "knightgpu-compact=off" is
# the control without it; "knightgpu-compact=force" the forced control (moves every few frames; tests only).
option(CTRPORT_KNIGHTGPU_COMPACT "Compact VRAM for hot spilled surfaces on New DR" ON)
option(CTRPORT_KNIGHTGPU_COMPACT_FORCE "Forced VRAM compaction control (tests only)" OFF)
if(CTRPORT_KNIGHTNEW_HISTORY AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c")
    foreach(part ctr_renderer z_knightnew_history)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${part}.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_KNIGHTNEW_HISTORY)
    endforeach()
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrRenderer_beginFrame,--wrap=CtrRenderer_endFrame,--wrap=GX_TextureCopy)
    # Knight GPU L-A (change knightgpu): while the application surface draws native, its texture's unused full-resolution
    # memory holds hot spilled surfaces (ctr_renderer.c's end), through GPU-1's beginFrame wrapper. Exact pixels.
    option(CTRPORT_KNIGHTGPU_LEND "Lend the application surface's unused memory to spilled surfaces on New DR" ON)
    if(CTRPORT_KNIGHTGPU_LEND)
        foreach(part ctr_renderer z_knightnew_history)
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${part}.c" APPEND PROPERTY
                COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_LENDER)
        endforeach()
        # C3D_FrameBufClear: the slots go back before a resolution change's clear (ctr_renderer.c's end).
        target_link_options(ctrport PRIVATE -Wl,--wrap=C3D_TexDelete,--wrap=C3D_FrameBufClear)
        if(CTRPORT_KNIGHTGPU_COMPACT)
            foreach(part ctr_renderer ctr_memory ctr_vram_touch z_knightnew_history)
                set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${part}.c" APPEND PROPERTY
                    COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_COMPACT_ON)
            endforeach()
            if(CTRPORT_KNIGHTGPU_COMPACT_FORCE)
                set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
                    COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_COMPACT_FORCE_ON)
            endif()
        endif()
    endif()
    # Knight GPU d2 (change knightgpu): tiled draws make no tile quads out of view, and held additive passes are made a
    # texture tile at a time (ctr_renderer.c's end, through e5xDrawSpriteTiled). Exact pixels.
    option(CTRPORT_KNIGHTGPU_TILED "Cull tiled draws' tile quads and batch held additive passes by tile on New DR" ON)
    if(CTRPORT_KNIGHTGPU_TILED)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_TILED_ON)
    endif()
    # Knight GPU SY (change knightgpu): tiles' copies in VRAM are given up for a surface only when that leaves a block
    # it fits (ctr_renderer.c's end, through initVramTarget's call). Exact pixels.
    option(CTRPORT_KNIGHTGPU_SMARTYIELD "Give up tiles' VRAM copies for a surface only when that makes room on New DR" ON)
    if(CTRPORT_KNIGHTGPU_SMARTYIELD)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_KGSY_ON)
    endif()
    # Knight GPU FC (change knightgpu): while the detail-keeping shrink's strip is neither made nor wanted (native drawing),
    # hot tiles get VRAM copies placed best fit, leaving the room's wanted size in one piece, instead of none while its
    # 1408 KB stays free (ctr_renderer.c's and ctr_memory.c's ends, through KnightGpu_beginFrame). Exact pixels. Needs
    # the lending (L-A) and SY. TEST-ONLY branch knightgpu-fc75 (on stage75zk): ON here, for its console verdict;
    # its late code fits the 0x54000 late text page that 75zk already has (freebytes 73ba6de03).
    option(CTRPORT_KNIGHTGPU_FRAGCOPY "Copy hot tiles into VRAM gaps a room cannot use on New DR" ON)
    if(CTRPORT_KNIGHTGPU_FRAGCOPY AND CTRPORT_KNIGHTGPU_LEND AND CTRPORT_KNIGHTGPU_SMARTYIELD)
        foreach(part ctr_renderer ctr_memory)
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${part}.c" APPEND PROPERTY
                COMPILE_DEFINITIONS CTRPORT_KGFC_ON)
        endforeach()
        # FC rev 2: the copies are invisible to every other VRAM request (ctr_renderer.c's end): the free-space and
        # block queries leave them out, and vramAlloc's entries (ctr_vram_touch.c) and the block moves give them up
        # first. Wrapped, so the callers keep their code.
        if(CTRPORT_KNIGHTGPU_COMPACT)
            foreach(part ctr_renderer ctr_vram_touch)
                set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${part}.c" APPEND PROPERTY
                    COMPILE_DEFINITIONS CTRPORT_KGFC2_ON)
            endforeach()
            target_link_options(ctrport PRIVATE
                -Wl,--wrap=CtrMemory_vramBlocks,--wrap=CtrMemory_vramTaken,--wrap=CtrMemory_vramFreeNow,--wrap=CtrMemory_vramMoveTo)
            # FC's tiles go into mirrorHotTiles' list as without their copies. flowdiag, frametrace and the room plan
            # recorder wrap these too: in those builds the list leaves FC's tiles out while they have copies.
            if(NOT CTRPORT_FLOWSTREAM_DIAG AND NOT CTRPORT_FRAMETRACE AND NOT CTRPORT_ROOMPLAN_RECORDER)
                set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
                    COMPILE_DEFINITIONS CTRPORT_KGFC2_HOT)
                target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTextures_ensureTiles,--wrap=CtrTextures_ensureTilesBut)
            endif()
        endif()
    endif()
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
endif()
# Knight GPU O9 (change knightgpu): blend and fog calls made while a batch is open are held, and drawn only when they
# change something (ctr_renderer.c's end). The exports other files draw with are wrapped to settle them first. The
# condition below is the one line that picks the builds (DELTARUNE New now; UNDERTALE once freebytes frees its bytes:
# widen it to (CTRPORT_GAME STREQUAL "undertale" OR (TILED and n3ds)); the pads already fit UNDERTALE New and Old).
# New only, named here so a stray -DCTRPORT_KNIGHTGPU_TILED=ON cannot turn it on for Old. Never with the natsurf6 wide
# experiment: its soft draws change filterOverride before the checked draw, and a held run that merges (blend and fog
# back where they were) does not compare filterOverride, so the open batch would flush with the new filter.
option(CTRPORT_KNIGHTGPU_LAZY "Hold blend and fog calls that change nothing" ON)
# knightgpu T1 (on O9 only): O9's eager blend and fog setters write citro3d's state straight into the command buffer
# (the changed combiner stages, the blend word), so the next batch keeps the texture-only draw shortcut. OFF
# (build.sh knightgpu-texenv=off) builds the same binary as O9 alone.
option(CTRPORT_KNIGHTGPU_TEXENV "Cheaper blend and fog switches" ON)
if(CTRPORT_KNIGHTGPU_LAZY AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds" AND
   CTRPORT_KNIGHTGPU_TILED AND NOT CTRPORT_NATSURF6_WIDE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_LAZY_ON)
    if(CTRPORT_KNIGHTGPU_TEXENV)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_TEXENV_ON)
    endif()
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrRenderer_uploadVideoFrame,--wrap=CtrRenderer_fillVideoSurface
        -Wl,--wrap=CtrRenderer_createVideoSurface,--wrap=CtrRenderer_releaseVideo)
    if(CTRPORT_GAME STREQUAL "deltarune")
        target_link_options(ctrport PRIVATE -Wl,--wrap=Crispart_drawTexture)
    endif()
endif()
# knightgpu dspe (DELTARUNE New): typed regions' draw_surface_part_ext calls with real arguments
# (Knight_scalarSurfacePartExt, vm_builtins.c's end); SESSION adds their draw session (vine_titancpu.inc's
# Knight_stripDrawExt). build.sh turns on both this and the generator's side (CTRPORT_AOT_DSPE for tools/aot/typed.py).
set(CTRPORT_KNIGHTGPU_DSPE "OFF" CACHE STRING "draw_surface_part_ext in typed AOT regions: OFF, SCALAR or SESSION (build.sh: SESSION in DELTARUNE New)")
if(NOT CTRPORT_KNIGHTGPU_DSPE STREQUAL "OFF" AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    set_property(SOURCE "${ENGINE_DIR}/src/vm_builtins.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_DSPE)
    if(CTRPORT_KNIGHTGPU_DSPE STREQUAL "SESSION")
        set_property(SOURCE "${ENGINE_DIR}/src/vm_builtins.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c"
                     APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_KNIGHTGPU_DSPE_SESSION)
    endif()
endif()
# flowend (both games): the audio loader thread's decode reads into the SFX cache go in pieces with every queued sound
# read done between them (ctr_audio_system.c's end). Wrapped, so runPrepDecode, utaDecRead and the stream decode
# keep their code; only their call target changes, and other threads' reads pass straight through.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDecoder_read)
# flowend (UNDERTALE only): the room's planned sounds stay out of keepLinearFloor's sound shedding (ctr_audio_system.c's
# end). Wrapped, so ctr_textures.c and z_utsfx.c keep their code.
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrAudioSystem_beginSfxPlan,--wrap=CtrAudioSystem_preloadSfx,--wrap=CtrAudioSystem_queueSfx,--wrap=CtrAudioSystem_shedSfx)
endif()
# flowend (both games): the next created stream is opened ahead of its play on the opener thread, at its "waiting"
# note (ctr_audio_system.c's end). Wrapped, so openStream and the opener keep their code.
if(CTRPORT_USE_TREMOR AND TREMOR_INCLUDE_DIR AND TREMOR_LIBRARY AND OGG_LIBRARY) # the code needs Tremor's state
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDecoder_openFile,--wrap=CtrWatchdog_note)
    # pacer C3 (UNDERTALE Old 3DS): streams with the same codebooks share one decoded set (ctr_audio_system.c's end).
    # Only vorbisfile.o calls these two, so only its call targets change. The code reads Tremor's private set-up
    # (codec_setup_info: books +0x1c, book_param +0x820, fullbooks +0xc20; codebook 0x34 B), which no installed header
    # declares, so it is on only for the libvorbisidec.a whose layout was checked (3ds-libvorbisidec 1.2.1, disassembly
    # of block.o / info.o / sharedbook.o). Any other build of the library: C3 compiles out and Tremor's own path runs.
    if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds" AND NOT CTRPORT_AUDIO_METRICS)
        file(SHA256 "${TREMOR_LIBRARY}" ctrport_tremor_sha256)
        if(ctrport_tremor_sha256 STREQUAL "8b40b8425702d85fe5bc4249204069184da6c5293817a950ffee8be55d6c7b23")
            target_compile_definitions(ctrport PRIVATE CTRPORT_PACER_SHARE)
            target_link_options(ctrport PRIVATE -Wl,--wrap=vorbis_synthesis_init,--wrap=vorbis_info_clear)
        else()
            message(STATUS "Vorbis codebook share off: ${TREMOR_LIBRARY} is not the checked Tremor build")
        endif()
    endif()
endif()
# pacer A + E1 (UNDERTALE Old 3DS): the audio threads' blocks of up to 256 KiB come from an arena of their own, so a
# core-1 audio thread paused inside malloc or free (the app time limit) no longer holds newlib's heap lock against the
# game thread (ctr_audio_system.c's end). newlib's allocator entries are wrapped (only the call targets in newlib's own
# members change) and ctr_memory.c's free and usable-size paths end in the arena's. Not with the diagnostics that wrap
# the same entries (heap profiler, audiometrics, btlentry's diag): those keep newlib's heap for every thread.
option(CTRPORT_PACER_ARENA_LOG "Test only: audio arena peak, fallback and heap-lock census log lines (never release)" OFF)
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds" AND NOT CTRPORT_AUDIO_METRICS
   AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_BTLENTRY_DIAG AND NOT CTRPORT_BTLENTRY_FAULT)
    target_compile_definitions(ctrport PRIVATE CTRPORT_PACER_ARENA)
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=_malloc_r,--wrap=_calloc_r,--wrap=_realloc_r,--wrap=_memalign_r)
    if(CTRPORT_PACER_ARENA_LOG)
        if(CTRPORT_RELEASE_SOURCE_URL)
            message(FATAL_ERROR "arenalog is a test word, never a release")
        endif()
        target_compile_definitions(ctrport PRIVATE PACER_ARENA_LOG) # E1: the release arena's size
    endif()
elseif(CTRPORT_PACER_ARENA_LOG)
    message(FATAL_ERROR "arenalog needs the audio arena: UNDERTALE o3ds, no heap profiler, audiometrics or btlentry diag")
endif()

# utold Continue transition: registration is cold and all new storage is late.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utold_contblack.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Btnprompt_registerAll)
endif()

# utlife: Outside Room's UT compatibility bounds; absent from DR builds.
if(CTRPORT_GAME STREQUAL "undertale")
    target_sources(ctrport PRIVATE "${ENGINE_DIR}/src/z_utlife.c")
    set_source_files_properties("${ENGINE_DIR}/src/z_utlife.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants")
endif()

# hitcheck: all UT box readers select PC bounds. Install inside the
# colexact/RNG assembly chain, so their pool and retained-slot passes still run.
if(CTRPORT_GAME STREQUAL "undertale")
    target_sources(ctrport PRIVATE "${ENGINE_DIR}/src/z_hitcheck.c")
    set_source_files_properties("${ENGINE_DIR}/src/z_hitcheck.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables"
        COMPILE_DEFINITIONS CTRPORT_BBOX_ROUND)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_FREEWINS)
        get_target_property(hitcheck_delegate ctrport C_COMPILER_LAUNCHER)
        set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
            "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/hitcheck_compile.py;${hitcheck_delegate};--hitcheck-compiler")
        set_property(SOURCE "${ENGINE_DIR}/src/runner.c" "${ENGINE_DIR}/src/vm_builtins.c" "${ENGINE_DIR}/src/spatial_grid.c"
            APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_HITCHECK_ROUTED)
        set_property(SOURCE "${ENGINE_DIR}/src/runner.c" "${ENGINE_DIR}/src/vm_builtins.c" "${ENGINE_DIR}/src/spatial_grid.c"
            APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/hitcheck_compile.py")
        set_property(SOURCE "${ENGINE_DIR}/src/collision_gm14.c" "${ENGINE_DIR}/src/z_utflames.c" "${ENGINE_DIR}/src/colline_gm14.c" APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/hitcheck_compile.py")
    endif()
endif()

# colexact: append the new bbox correction object, preserving earlier weak helpers.
if(CTRPORT_GAME STREQUAL "undertale")
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_FREEWINS)
        set_property(SOURCE "${ENGINE_DIR}/src/spatial_grid.c" "${ENGINE_DIR}/src/debug_overlay.c"
            APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_LAYERFIX_RELEASE_LAYOUT)
    endif()
    target_sources(ctrport PRIVATE "${ENGINE_DIR}/src/z_colexact.c")
    set_source_files_properties("${ENGINE_DIR}/src/z_colexact.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    # Option A (2026-10-29): restore the two complete string groups, keeping
    # labels and every code chunk. The adapter rejects any unexpected pool.
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_FREEWINS)
        get_target_property(colexact_delegate ctrport C_COMPILER_LAUNCHER)
        set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
            "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/colexact_compile.py;${colexact_delegate};--colexact-compiler")
        set_property(SOURCE "${ENGINE_DIR}/src/vm_builtins.c" APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/colexact_compile.py")
    endif()
endif()

# utbugs4: replace center before VM_create caches it, set Runner position hooks before room creation events.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/utwindow_compile.py")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utwindow.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utold_contblack.c" APPEND PROPERTY
        COMPILE_DEFINITIONS __real_Btnprompt_registerAll=CtrUtWindow_register)
    # The room change reaches the real function from ctr_loadaudio_mix.inc (CtrLoadmix_handleRoomChange, included by
    # ctr_audio_system.c) since loadaudio 76dd8ab09; on z_freebies.c this define was dead and the window hooks never
    # installed (UT-036 regression, zq47). tools/analysis/test_utwindow_chain.py checks the chain from the caller.
    # utfade (source/z_utfade.c) sits in front: a door's room change shows black while the room loads. The chain is
    # CtrLoadmix_handleRoomChange -> CtrUtfade_pendingRoom -> CtrUtWindow_pendingRoom
    # -> the real function; each unit's __real_ call names the next link. No new link wraps.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" APPEND PROPERTY
        COMPILE_DEFINITIONS __real_Runner_handlePendingRoomChange=CtrUtfade_pendingRoom)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utfade.c" APPEND PROPERTY
        COMPILE_DEFINITIONS __real_Runner_handlePendingRoomChange=CtrUtWindow_pendingRoom)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utfade.c" PROPERTIES COMPILE_OPTIONS
        "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
    # utfadeprobe (build.sh "utfadeprobe", tests only, never Release): z_utfade.c writes the game screen's picture
    # during each room change's load to the game folder (its probe's comment says how).
    option(CTRPORT_UTFADE_PROBE "Test only: write the game screen's picture during each room change's load" OFF)
    if(CTRPORT_UTFADE_PROBE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utfade.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_UTFADE_PROBE)
    endif()
endif()

# fpsbox: retain the two shrinking original glyph/paint slots exactly; all
# additions are tail inputs. The adapter only inserts unreachable zero bytes.
get_target_property(fpsbox_delegate ctrport C_COMPILER_LAUNCHER)
if(CTRPORT_GAME STREQUAL "deltarune")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrArgbind_unloadGame)
endif()
set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
    "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/fpsbox_compile.py;${fpsbox_delegate};--fpsbox-compiler")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/fpsbox_compile.py")

# labcut: one lab-only join wrapper, with the two cold exit bodies retained in
# their stage75w slots. Keep its late input last after player-facing units.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrExit_closeLog)
get_target_property(labcut_delegate ctrport C_COMPILER_LAUNCHER)
set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
    "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/labcut_compile.py;${labcut_delegate};--labcut-compiler")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_exit_log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_exit_midfile.c" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/labcut_compile.py")
# depthmove (DELTARUNE only): runner.c keeps automatic layers' element lists as deques (z_depthmove.c). The other
# objects' layer lookups are wrapped so that a list they grow with arrput always has a spare slot; runner.c's own
# calls are direct.
if(CTRPORT_GAME STREQUAL "deltarune")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" "${ENGINE_DIR}/src/z_depthmove.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DEPTHMOVE)
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_findRuntimeLayerById)
endif()

# knsurf: a forced-off control compiles the original call and no new code.
option(CTRPORT_KNSURF "Reuse a prepared surface strip batch on DELTARUNE New" ON)
if(CTRPORT_KNSURF AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds" AND NOT CTRPORT_PROFILE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_KNSURF)
endif()

# sharebuf (change titancpu): surfaces made by the objects the builder lists (romfs:/roomplan/sharebuf.dtp,
# tools/roomplan/build_sharebuf.py) share one buffer. ctr_renderer_sharebuf.inc is compiled in the vine helper pass
# only (the original pass, and so the file's own code, never sees it); vine_old_observer_compile.py routes the calls
# to it in the assembly, and its sections are linked last. Off (build.sh sharebuf=off) compiles none of it;
# build.sh sharebuf=test forces one violation, to show its log fires. G2 (ctr_renderer_sharebuf_g2.inc, appended
# after G1 in sections of its own) is on with sharebuf; sharebuf=g1 leaves it out (the base of its layout check),
# and sharebuf=fulltest fills every whole clear of a shared scratch whole and checks the rows G2 would have filled
# (SHAREBUF BOX MISS), a test build.
option(CTRPORT_SHAREBUF "Let listed objects' surfaces share one buffer on DELTARUNE New" ON)
option(CTRPORT_SHAREBUF_TEST "Force one sharebuf violation (test builds only)" OFF)
option(CTRPORT_SHAREBUF_G2 "Sharebuf G2: whole clears of shared scratches fill only the rows drawn on" ON)
option(CTRPORT_SHAREBUF_FULLTEST "Fill shared scratches whole and check G2's rows (test builds only)" OFF)
if(CTRPORT_SHAREBUF AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_SHAREBUF)
    # The helper pass's include is in no dependency file the original pass writes.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer_sharebuf.inc" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/vine_extract.py")
    if(CTRPORT_SHAREBUF_G2)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_SHAREBUF_G2)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer_sharebuf_g2.inc")
    endif()
    if(CTRPORT_SHAREBUF_TEST)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_SHAREBUF_TEST)
    endif()
    if(CTRPORT_SHAREBUF_G2 AND CTRPORT_SHAREBUF_FULLTEST)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_SHAREBUF_FULLTEST)
    endif()
endif()

# redpix: preserve the original packing callback's section position after its cold call is retargeted.
if(CTRPORT_GAME STREQUAL "undertale" AND CMAKE_BUILD_TYPE STREQUAL "Release")
    get_target_property(redpix_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/redpix_compile.py;${redpix_delegate};--redpix-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_texture_pack.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/redpix_compile.py")
endif()

# dlghold: retain genuine dialogue gaps, but end replay on the first menu frame.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_dlghold.c" PROPERTIES COMPILE_OPTIONS "-Os;-fno-merge-constants")
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDialogue_holdAllowed
        -Wl,--undefined=__wrap_CtrDialogue_holdAllowed)
else()
    # DELTARUNE New: Crisp art's ARM entry is fpsbox's retained one (ctr_fpsbox.c, CtrFpsbox_crispVeneer, which
    # z_crispart.c names); dlghold adds none of its own (75zo, one shared entry).
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDialogue_init)
endif()

# flowerhw L2 (DELTARUNE New): the phase plan's eviction drops the wanted marks of the tiles it took (z_phaseplan.c's
# ppEvict), so the fill doesn't load them again at once. New only: DELTARUNE Old keeps its phase plan as it is.
# CTRPORT_FLOWERHW_PPFORCE is TEST-ONLY, never in a release: the eviction runs as if no room were left and logs L2's
# counts, so Azahar (which never reaches phase 4) takes the path; with L2 OFF it is the control arm.
option(CTRPORT_FLOWERHW_L2 "Drop the wanted marks of the tiles the phase plan evicts on DELTARUNE New" ON)
option(CTRPORT_FLOWERHW_PPFORCE "TEST-ONLY: force the phase plan's eviction and log L2's counts" OFF)
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    if(CTRPORT_FLOWERHW_L2)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_phaseplan.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_PPL2)
    endif()
    if(CTRPORT_FLOWERHW_PPFORCE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_phaseplan.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_PPFORCE)
    endif()
endif()

# GPU kick (change pacer, 2026-10-31; cheaplevers' guard20 kick, ef31f2fbd): while a DELTARUNE New frame still
# records, an existing split hands the GPU the commands recorded so far (ctr_gpu_kick.inc, included at the end of
# ctr_gpu_queue.c). build.sh's gpukick words; default ON in DELTARUNE New in multi mode with the rev3 gate, and
# "gpukick=off" is the control: then no object or link command changes. Policy E: the flags and hooks never reach
# UNDERTALE or DELTARUNE Old. The launcher compiles each ordinary body without the kick and
# appends only the kick's late sections; z_knightnew_history.c's wrappers and benchfix's draw wait keep their bodies,
# with only their downstream calls routed (tools/analysis/cheaplevers_kick_compile.py). Mode: single (one prefix per
# recording, history guard), multi (a prefix at each later split once the last is off the GPU, series-time guard) or
# multi-force (every recording; Azahar identity diagnostic only, never a console or release package).
# Rev2 (lead 2026-11-01): no prefix in a frame that loads or sync-copies textures, or right after one that loaded
# (C3D_SyncTextureCopy is wrapped to count them), and a per-frame kick trace for slow frames in the log.
# Rev3 (lead 2026-11-02, Knight kfinal lever 1): multi kicks only after 3 frames in a row whose main + GPU passed the
# slot while each fit it (ctr_gpu_kick.inc's gate), and logs each turn of the gate.
# KM rev 5 (change pacer, c26rev R123/R131; 75zq append-only port): a bounded chain of 8 KB ranges, the gate on the
# late-serial shape, no frame-end self-joins (ctr_gpu_kick5.inc). The launcher's first helper pass still builds rev 4.1
# (ctr_gpu_kick.inc) in place, its exports branching to rev 5, whose code, constants and state are appended in their
# own late sections (.pacer_km5_late), so no existing address moves. Multi only (the single prefix is gone).
option(CTRPORT_GPU_KICK "Submit flushed command prefixes while recording (DR New)" ON)
set(CTRPORT_GPU_KICK_MODE "multi" CACHE STRING "GPU kick mode: multi or multi-force (identity diagnostic)")
# R123's compact per-frame trace (KT lines between bench windows, a 1024-record ring): OFF for release (R131 F3);
# console row builds turn it on (build.sh gpukick-trace).
option(CTRPORT_GPU_KICK_TRACE "GPU kick per-frame trace between bench windows (KM rev 5; row builds only)" OFF)
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds" AND CTRPORT_GPU_KICK)
    set(kick_sources "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_memory.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c")
    if(CTRPORT_KNIGHTNEW_HISTORY)
        list(APPEND kick_sources "${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c")
    endif()
    set_property(SOURCE ${kick_sources} APPEND PROPERTY COMPILE_DEFINITIONS CTR_GPU_KICK)
    if(CTRPORT_GPU_KICK_MODE STREQUAL "multi")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTR_KICK_MULTI)
    elseif(CTRPORT_GPU_KICK_MODE STREQUAL "multi-force")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTR_KICK_MULTI CTR_KICK_FORCE)
    else()
        # KM rev 5's chain is the multi kick only; the single prefix (cheaplevers) is gone.
        message(FATAL_ERROR "CTRPORT_GPU_KICK_MODE must be multi or multi-force")
    endif()
    get_target_property(kick_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/cheaplevers_kick_compile.py;${kick_delegate};--kick-cc--")
    set_property(SOURCE ${kick_sources} APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_kick.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_kick.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_kick_renderer.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_kick5.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_kick5_renderer.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_kick_chain.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/cheaplevers_kick_compile.py")
    # The same link options as cheaplevers' release31 kick (waitDraws, beginFrame and endFrame are wrapped already
    # in DELTARUNE New: benchfix and Knight GPU-1; the launcher renames the kick's own links into those chains).
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=C3D_FrameSplit,--wrap=C3D_FrameEnd,--wrap=CtrGpu_frameEndBehind
        -Wl,--wrap=CtrGpu_waitIdle,--wrap=CtrGpu_waitDraws,--wrap=CtrGpu_busy
        -Wl,--wrap=CtrGpu_wrapCommandBuffer,--wrap=CtrGpu_cancelCopy,--wrap=CtrRenderer_beginFrame
        -Wl,--wrap=CtrRenderer_endFrame,--wrap=CtrRenderer_stopBackgroundWork,--wrap=C3D_SyncTextureCopy
        -Wl,--wrap=gxCmdQueueRun,--wrap=CtrPacer_stressStep
        -Wl,-u,C3D_FrameSplit,-u,C3D_FrameEnd,-u,CtrGpu_frameEndBehind
        -Wl,-u,CtrGpu_waitIdle,-u,CtrGpu_waitDraws,-u,CtrGpu_busy
        -Wl,-u,CtrGpu_wrapCommandBuffer,-u,CtrGpu_cancelCopy,-u,CtrRenderer_beginFrame,-u,C3D_SyncTextureCopy)
    # KM rev 4.1: the totals are bench-only output, printed as a test run ends (main.c's CtrBench_takeFinished call).
    # frametrace wraps that call itself (a symbol is wrapped once): with it on, KM prints no totals.
    if(NOT CTRPORT_FRAMETRACE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTR_KICK_BENCH_TOTALS)
        target_link_options(ctrport PRIVATE -Wl,--wrap=CtrBench_takeFinished)
        if(CTRPORT_GPU_KICK_TRACE)
            # Read only by rev 5's helper pass (the launcher drops it from the ordinary and rev 4.1 passes).
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c" APPEND PROPERTY
                COMPILE_DEFINITIONS CTR_KICK_FRAME_TRACE)
        endif()
    endif()
endif()

# drhitbox: append-only WAD17 / GM 2022.9 legacy collision correction.
# Do not add this object, adapter, or wraps to UNDERTALE.
if(CTRPORT_GAME STREQUAL "deltarune")
    target_sources(ctrport PRIVATE "${ENGINE_DIR}/src/z_drhitbox.c")
    set_source_files_properties("${ENGINE_DIR}/src/z_drhitbox.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    get_target_property(drhitbox_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/drhitbox_compile.py;${drhitbox_delegate};--drhitbox-compiler")
    set_property(SOURCE "${ENGINE_DIR}/src/collision_compat.c" "${ENGINE_DIR}/src/runner.c"
        APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/drhitbox_compile.py")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Colflip_registerAll,--wrap=Colflip14_scanPixels)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/pinwheel.cmake")

# dlgtop: UT loading only; DR compile and link paths are untouched.
if(CTRPORT_GAME STREQUAL "undertale")
# loadgame: choose the console screen before its first print; retain main's calls.
target_link_options(ctrport PRIVATE -Wl,--wrap=consoleInit)

# Retain the frozen loader's cold function order and shortened body extents.
get_target_property(loadgame_delegate ctrport C_COMPILER_LAUNCHER)
set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
    "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/loadgame_compile.py;${loadgame_delegate};--loadgame-compiler")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_loadscreen.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c"
    APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/loadgame_compile.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/loadgame-order.json")
endif()

# drawbatch: seam copies are for the Old models (a burst of draws there holds a frame; the New 3DS has the time).
if(NOT CTRPORT_TARGET STREQUAL "o3ds")
    target_compile_definitions(ctrport PRIVATE CTRPORT_SEAM_OFF)
endif()
# drawbatch: seam copies (source/z_seamcopy.c, UNDERTALE only), in their own unit so ctr_renderer.c keeps its code and
# static data where they were; late code only, its formats named late constants (no printf folding, no switch
# tables in .rodata).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_seamcopy.c" PROPERTIES
    COMPILE_OPTIONS "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# thinrule (first update, UNDERTALE only): Smart smoothing's whole-rows table, read after the item verdicts (the font
# verdict and display caps chains, source/ctr_wrap_chain.h). Late code only; its strings are named late constants.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_thinrule.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# numcache (change loadclass, both games): a thread's number-cache walk takes newlib's real list count, and the
# UNDERTALE Old 3DS audio arena's calloc zeroes its whole block (source/z_numcache.c). Late code only; no strings.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_numcache.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
# dwkeep (change loadclass 10, UNDERTALE only): data.win's later opens to read are sub-files of one kept handle, so
# they skip the game folder's lookup (source/z_dwkeep.c; a link of CtrOpenFiles_track's chain, ctr_wrap_chain.h).
# Late code only; its strings are named constants in its late text.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_dwkeep.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")

# thinart: small late-only loader; keep all literals named in its late section.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_thinart.c" PROPERTIES
    COMPILE_OPTIONS "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")

# RNG entry redirects run after the existing canonical-pool pass. Ordinary
# compiler input and graph stay identical to the frozen release bodies.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(rngexact_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/rngexact_compile.py;${rngexact_delegate};--rngexact-compiler")
    set_property(SOURCE "${ENGINE_DIR}/src/vm_builtins.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/rngexact_compile.py")
endif()

# saveaudit: startup cannot expose a partial save index or failed backup recovery.
# Redirect only the three external calls in this object, preserving its bodies.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_file_system.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "opendir=CtrSaveaudit_scanOpen;readdir=CtrSaveaudit_scanNext;CtrSafeSave_recover=CtrSaveaudit_scanRecover")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_saveaudit.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants")
endif()

# R333: test-only fatal UI injection, isolated from the player's saves directory.
option(CTRPORT_SAVEAUDIT_FATAL_TEST "Inject startup EIO against saveaudit-fatal-test only (never ship)" OFF)
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_saveaudit_scan.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants")
    if(CTRPORT_SAVEAUDIT_FATAL_TEST)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_saveaudit_scan.c" APPEND PROPERTY
            COMPILE_DEFINITIONS "CTRPORT_SAVEAUDIT_FATAL_TEST")
    endif()
endif()

# Native UT 1.08 serialization, before auditfixe's existing INI cache wrappers.
# Redirect one already-late registration call; leave engine object bytes intact.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inisafe.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "__real_VMBuiltins_registerAll=CtrSaveaudit_registerAll")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_saveaudit_format.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants")
endif()

# audpar: restore the two original position jump-table chunks after the paused
# query changes GCC's emission order. The linked-image proof remains strict.
if(CTRPORT_GAME STREQUAL "undertale" AND CMAKE_BUILD_TYPE STREQUAL "Release")
    get_target_property(audpar_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/audpar_compile.py;${audpar_delegate};--audpar-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/audpar_compile.py")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomload.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/audpar_compile.py")
endif()

# upd1 SCOURW6-001: remember the menu-open snapshot and submit pending edits at
# the existing post-loop call, before teardown. Only call targets change in main.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CtrArgui_menuOpen=Upd1_menuOpen;CtrRenderer_endFlickerProbe=Upd1_endFlickerProbe;ctrReleaseStatsFormat=Upd1_statsFormat")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_fps_clock.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "ctrReleaseStatsFormat=Upd1_statsFormat")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_upd1_audio.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_upd1_options.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants")
endif()

# battlesplit (change battlesplit, UNDERTALE): the split battle layout. All its code, constants and state are late
# (late_reserve.ld). It takes main.c's Runner_drawViews call, except in the test builds that take it themselves (the
# counter probe and the half census), where it never splits.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplitbsr.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    if(NOT CTRPORT_ASG_PROBE AND NOT CTRPORT_HALF_CENSUS)
        target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_drawViews)
    endif()
    # battlesplit11f: runner.c's run draws, so each later draw of a run is routed (z_battlesplit.c defines the
    # wrappers where runner.c makes the calls, UTK_PLAIN_RUNS).
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrUtk_plainAt,--wrap=CtrUtk_plainBatch,--wrap=CtrUtk_turnedKept,--wrap=CtrUtk_turnedFast)
    # battlesplit17: every layer switch from z_battlesplit.c passes its band clip first (the wrapper is in that file,
    # battlesplit's AAC tenant; CtrRenderer_bsLayer itself stays where it is).
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_bsLayer)
    # battlesplit22 (ctr_renderer.c's end): the Split's top margin drawn before the present, and 14c/14f's strip noted.
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_bsPresent5,--wrap=CtrRenderer_bsStripCopy14)
    # bsplace (build.sh "bsplace", tests only, never Release): L switches preset A's placement between (b) and (c).
    option(CTRPORT_BS_PLACESWITCH "Test only: L switches the split battle's box placement between (b) and (c)" OFF)
    if(CTRPORT_BS_PLACESWITCH)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_BS_PLACESWITCH)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_BS_PLACESWITCH)
    elseif(CTRPORT_LABGUARD_ON)
        # devsplit (lead 11-11 08:4x, maintainer: "3ds mode on by default for our testing" until it is reliable): every dev
        # build (UNDERTALE, no release source URL) defaults battle_layout to Split when the menu, --ini and settings.ini
        # leave it unset. Nothing else differs from release's Split: per-fight seams (bsSeamB), the same present, no L
        # switch and no seam-crossing log (those stay bsplace's). Release builds never get the define. Parked: back to
        # release's Original default when maintainer says the 3DS mode is reliable (owner: lead).
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_BS_DEVSPLIT)
    endif()
endif()

# E1: all new code, constants and state append after S7. Existing short labels
# and sprite bodies remain in place; the text builtin is wrapped before caching.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_errtext_e1.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=Colline14_registerAll,--wrap=CtrBtnprompt_begin,--wrap=CtrBtnprompt_buttonSprite
        -Wl,--wrap=CtrRenderer_chainDisplayInputlate,--wrap=CtrArgui_chainSaveEditsSmalltext,--wrap=Btnprompt_setLabels)
endif()

# Append legacy background dispatch after all existing assembly adapters. The
# runner and builtin compiler bodies remain frozen; only call targets change.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # These deliberate unmapped accesses/chunk-header writes exercise crashes.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_argcrash.c" APPEND PROPERTY COMPILE_OPTIONS "-Wno-array-bounds")
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    # GCC -Os treats distinct flicker.tag/words members as possibly overlapping.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_OPTIONS "-Wno-restrict")
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(gmlstubs_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/gmlstubs_compile.py;${gmlstubs_delegate};--gmlstubs-compiler")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" "${ENGINE_DIR}/src/vm_builtins.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/gmlstubs_compile.py")
    target_link_options(ctrport PRIVATE
        "-Wl,--wrap=VMBuiltins_getVariable" "-Wl,--wrap=VMBuiltins_setVariable")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_gmlstubs.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
endif()

# popbot: the overworld's popups follow the dialogue onto the bottom screen (z_popbot.c). All its code and constants
# append after menuborder on its branch (in S8b after papvoice); the dialogue path is only wrapped.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_popbot.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    # Steps 5 and 6 (z_popbot2.c), appended after every other late input; z_popbot.c's wrappers call into it.
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_popbot2.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDialogue_init,--wrap=CtrDialogue_allowed)
    # Its settings link after keep-lit's (the same call in z_keeplit.c, another target): a popup open when dialogue
    # leaves the bottom screen is closed there (review c26rev3).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_keeplit.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrRenderer_chainDisplayInputlate=CtrPopbot_setDisplay")
endif()

# dlgwhite's helper is emitted only by the UNDERTALE renderer's second pass.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgwhite.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/vine_extract.py")
endif()

# battletouch (UNDERTALE): the stylus in battles (source/z_battletouch.c, all late; DELTARUNE compiles it empty). Only
# call targets change: inputlate's CtrInput_take and Runner_step link (ctr_wrap_chain.h), and screens' calls to
# benchfidLoad, benchfidOverride and CtrDisplay_save (the battle_touch setting). Its options are two rows on the main
# page (tools/buildcache/builderx_compile.py routes optmenu's main-page keys and draw there); the Controls page is as
# before, so the first slot's Battles row and page code is not wrapped in, and its __real_ calls name the originals
# (--defsym: no code). CTRPORT_TOUCH_TEST (build.sh
# "touchtest"; test builds only): touchscript.txt in the game folder drives the pen, and each stylus frame is logged.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrInput_take,--wrap=benchfidLoad,--wrap=benchfidOverride,--wrap=CtrDisplay_save
        -Wl,--defsym=__real_CtrCtrlui_main=CtrCtrlui_main,--defsym=__real_CtrCtrlui_update=CtrCtrlui_update
        -Wl,--defsym=__real_CtrCtrlui_holdMoves=CtrCtrlui_holdMoves)
    # The first-battle hint (split: painted on the top screen's finished frames before their flip): heldslow's wrap of
    # the frames' swap continues to Battletouch_pacerSwap, which calls the real swap. A call target only.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_frame_pacer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_CtrScreens_pacerSwap=Battletouch_pacerSwap")
endif()
option(CTRPORT_TOUCH_TEST "Test only: touchscript.txt drives the stylus in battles, and stylus frames are logged" OFF)
if(CTRPORT_TOUCH_TEST)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TOUCH_TEST)
endif()
# The pen log (lead 11-08): test builds keep every stylus sample in a ring and write it as one [pen] line a while after
# the pen lifts, so a player's log has the console's samples. Never in a public build (CTRPORT_RELEASE_SOURCE_URL).
# The same switch gives battlesplit's pick log its bottom-screen sampler (z_battlesplit.c, c26rev5's F2 on P6).
if(CTRPORT_GAME STREQUAL "undertale" AND NOT CTRPORT_RELEASE_SOURCE_URL)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c"
                 "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c"
                 APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_PEN_LOG)
endif()
# menutime (change scanout, ethanlog5 row 13; UNDERTALE only): the options menu's open and close timed for the log, and
# its glyphs read in the game's load behind the loading bar so that its first open never reads the card, and the press
# that closes it no longer redraws its page (z_menutime.c). main.c's open, update, save, close, display caps, display
# set-up and frame wait, ctr_optmenu.c's screens' open, vblank waits and Legal mark, z_loadscreen.c's call of the first
# room's set-up (inside its loading-bar wrap) and z_roomplan.c's call of the texture cache's set-up are pointed at it by
# name: the same calls with other targets, so every existing object keeps its code and addresses. Its code (and
# ctr_optmenu.c's appended CtrOptmenu_pressResumes and CtrOptmenu_flushPage), constants and state (in the late data's
# reserve) are appended last.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_menutime.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "Upd1_menuOpen=CtrMenutime_open" "CtrArgui_menuClose=CtrMenutime_close"
        "CtrBtnprompt_saveEdits=CtrMenutime_saveEdits" "CtrRenderer_capsAuditfixh=CtrMenutime_caps"
        "CtrRenderer_setDisplay=CtrMenutime_setDisplay" "CtrRenderer_beginFrame=CtrMenutime_beginFrame"
        "CtrArgui_menuUpdate=CtrMenutime_update")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrMenu_chainOpenInputlate=CtrMenutime_screensOpen" "gspWaitForEvent=CtrMenutime_wait"
        "CtrLegalui_mark=CtrMenutime_legalMark")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_loadscreen.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_Runner_initFirstRoom=CtrMenutime_initFirstRoom")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomplan.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_CtrTextures_init=CtrMenutime_texturesInit")
endif()

# logfix (change scanout, logaudit G8, both games): the audio system's queued log records arrive whole, each ending its
# line (z_logfix.c). ctr_audio_system.c's formatting calls are pointed at CtrLogfix_vsnprintf by name, which marks a
# text that did not fit and keeps its newline; the sound-effect pack's posts go through CtrLogfix_post (its table in
# ctr_adpcm_pack.inc), which splits a record longer than a message slot. The same calls with other targets, so no
# existing code or address moves; the code and constants are appended last in late text.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" APPEND PROPERTY COMPILE_DEFINITIONS
    "vsnprintf=CtrLogfix_vsnprintf")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_logfix.c" PROPERTIES
    COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")

# heldslow (change scanout, logaudit G1, UNDERTALE only): a [held] record of each game-screen image that stayed up
# longer than its frames' slots planned (a late frame, a room change's load, a stall), and G2's [slow] record of each
# frame whose main thread took over 33.4 ms, split at main.c's watchdog marks; printed every [perf] window. The frames'
# swap is wrapped (ctr_argui.c's branch is its only caller); ctr_frame_pacer.c's flips, ctr_gpu_queue.c's display
# transfers, ctr_renderer.c's half-rate skip and main.c's stats call and marks are pointed at the appended code by
# name. No existing code or address moves. movehold (change bonepulse, 11-09): the flips' swaps go by way of
# CtrMovehold_swap (the end of ctr_frame_pacer.c), which counts the picture a screen move kept up, then heldslow's.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_frame_pacer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_gfxScreenSwapBuffers=CtrMovehold_swap" CTRPORT_HELDSLOW=1)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrPacer_skipSlot=CtrHeldslow_skipSlot")
    # The bottom count (ctr_gpu_queue.c's end): releaseReady asks it, and it asks dlgblank. C3D_FrameEnd's display
    # transfers go by way of the frame's note at its hand-over (c26rev7 F1).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "C3D_FrameBufTransfer=CtrHeldslow_transfer" CTRPORT_HELDSLOW=1)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrPacer_logStats=CtrHeldslow_logStats" "CtrWatchdog_mark=CtrHeldslow_mark" CTRPORT_HELDSLOW=1)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_heldslow.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrScreens_pacerSwap)
endif()

# dimlightdiag: test-only call redirections; release objects remain untouched.
if(CTRPORT_DIMLIGHT_DIAG)
    if(NOT CTRPORT_GAME STREQUAL "undertale")
        message(FATAL_ERROR "dimlightdiag requires undertale")
    endif()
    # Diagnostic scenes stay in the optional RomFS supplement. No new resident
    # constant or SD override, and no change to the release scene list.
    set(DIMLIGHT_SCENE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/bench/dimlight_undertale.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DIMLIGHT_SCENE_SOURCE}")
    file(READ "${DIMLIGHT_SCENE_SOURCE}" dimlight_scenes)
    file(APPEND "${COVERAGE_ROMFS_FILE}" "\n${dimlight_scenes}")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dimlight.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CTRPORT_DIMLIGHT_DIAG;svcSendSyncRequest=CtrDimdiag_send")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CtrBench_running=CtrDimdiag_mainRunning;CtrRenderer_endFrame=CtrDimdiag_endFrame")
    # menutime already names main's beginFrame/saveEdits targets in the respin.
    # Keep that outer layer and observe its continuation, avoiding duplicate
    # command-line definitions (and retaining its page-wait bookkeeping).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/source/z_menutime.c")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_menutime.c" APPEND PROPERTY
            COMPILE_DEFINITIONS "CtrRenderer_beginFrame=CtrDimdiag_beginFrame;CtrBtnprompt_saveEdits=CtrDimdiag_saveEdits")
    else()
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY
            COMPILE_DEFINITIONS "CtrRenderer_beginFrame=CtrDimdiag_beginFrame;CtrBtnprompt_saveEdits=CtrDimdiag_saveEdits")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CtrBench_running=CtrDimdiag_paintRunning")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_upd1_options.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CtrBtnprompt_saveEdits=CtrDimdiag_saveEdits")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CtrDimlight_setDisplay=CtrDimdiag_setDisplay;CtrPacer_frameEnding=CtrDimdiag_frameEnding")
    # The argui assembly branch enters heldslow's wrapper first. In composed
    # trees, redirect only that wrapper's continuation through the diagnostic;
    # the diagnostic then calls the real screen swap. Release has no aliases.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/source/z_heldslow.c")
        # battletouch's hint swap now follows heldslow's wrap (ctr_frame_pacer.c's define): the diagnostic follows it.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c" APPEND PROPERTY
            COMPILE_DEFINITIONS "__real_CtrScreens_pacerSwap=CtrDimdiag_pacerSwap")
    else()
        # This change's earlier base predates heldslow: the diagnostic owns the
        # wrapper here, with the same observer and real-swap continuation.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dimlight.c" APPEND PROPERTY
            COMPILE_DEFINITIONS "CtrDimdiag_pacerSwap=__wrap_CtrScreens_pacerSwap")
    endif()
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrScreens_pacerSwap)
    set_property(SOURCE "${ENGINE_DIR}/src/sandbox_file_system.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CTRPORT_DIMLIGHT_DIAG")
endif()

# dimlight: preserve GCC's original local call graph and optimized slots.
# The renderer's existing utwindow adapter routes its cold LCD operands.
# The outer adapter routes the watcher and restores the screens object's cold
# function order; it forwards renderer -c to the existing inner adapter.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(dimlight_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/dimlight_compile.py;${dimlight_delegate};--dimlight-compiler")
    foreach(dimlight_source ctr_renderer ctr_rosalina ctr_screens)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${dimlight_source}.c" APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/dimlight_compile.py")
    endforeach()
endif()

# layout (change layout, UNDERTALE): the element layout model, its settings keys (text_place, layout_preset) and the
# stylus's answer to what the bottom screen shows (z_layout.c, ctr_layout.h). ctr_argui.c's three display-settings calls
# are pointed at its links by name, the same size (argui -> layout -> screens); nothing else in it changes. All its
# code, constants and state are late (late_reserve.ld, "layout").
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_layout.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_argui.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CtrDisplay_load=CtrLayout_chainLoad CtrDisplay_override=CtrLayout_chainOverride
        CtrDisplay_saveEdits=CtrLayout_chainSaveEdits)
    # A2: the display settings' chain goes errtext's wrap -> CtrLayout_display18 -> inputlate's link (z_layout.c says
    # why: the battle's move up runs the whole chain before the frame without taking the frame's input).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_errtext_e1.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_CtrRenderer_chainDisplayInputlate=CtrLayout_display18")
    # LAY-1: the beginFrame chain goes inputlate's link -> CtrLayout_chain18 (a Battle layout pick's repick, outside the
    # frame) -> shopsplit's wrap -> battlesplit's chain. inputlate's one call, another target (z_layout.c says why).
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrRenderer_chainBeginFrameBattlesplit=CtrLayout_chain18")
    # The scene table (S3): build.sh resolves tools/layout/undertale-scenes.json against the player's data.win
    # (tools/layout/build_scenes.py); without one every battle is Original.
    if(CTRPORT_LAYOUT_SCENES_FILE)
        configure_file("${CTRPORT_LAYOUT_SCENES_FILE}" "${CTRPORT_ROMFS_DIR}/layout/undertale.lst" COPYONLY)
        add_custom_command(OUTPUT "${CTRPORT_3DSX_OUTPUT}" APPEND DEPENDS "${CTRPORT_ROMFS_DIR}/layout/undertale.lst")
    else()
        file(REMOVE "${CTRPORT_ROMFS_DIR}/layout/undertale.lst")
    endif()
endif()

# bonegate: utorder keeps its existing symbol names and slots; only its three continuation call targets change.
# The two diagnostic builds install their own tables during Draw, with no gate compiled or linked.
if(CTRPORT_GAME STREQUAL "undertale" AND NOT CTRPORT_HALF_CENSUS AND NOT CTRPORT_ASG_PROBE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_utorder.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_Runner_drawPre=Bonegate_drawPre" "__real_Runner_drawGUI=Bonegate_drawGUI"
        "__real_Instance_create=Bonegate_instanceCreate")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_addInstanceToObjectLists)
endif()

# ctr_renderer.c's C3D_FrameEnd link (the two blocks below, one per UNDERTALE target): bottompaint's; in a ballast build
# ballast's, which draws its quads and then calls bottompaint's (z_ballast.c). Ballast (build.sh ... ballast, UNDERTALE
# test builds only) also takes main.c's hidKeysUp, CtrRenderer_endFrame and --ini calls (CtrArgui_override, through
# ctr_display.h's redirect), each by name and the same size; hidKeysUp because main.c calls it once, in the main loop
# (hidScanInput is also the fatal and out-of-memory screens' poll, which must never spin); its code and constants go
# in the AAC slots after every production input (aacout.cmake) and its state is the last late .bss line, so every
# other address is the plain build's.
set(CTRPORT_FRAMEEND_LINK CtrBottomPaint_frameEnd)
if(CTRPORT_BALLAST)
    set(CTRPORT_FRAMEEND_LINK CtrBallast_frameEnd)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_ballast.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_ballast.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BALLAST)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BALLAST)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "hidKeysUp=CtrBallast_keysUp" "CtrRenderer_endFrame=CtrBallast_endFrame" "CtrArgui_override=CtrBallast_override")
endif()

# boxcache composition (paintchg): ca8140068's transfer-kept path, heap state.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "C3D_FrameBufClear=CtrBoxcache_frameBufClear")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "C3D_FrameEnd=${CTRPORT_FRAMEEND_LINK}")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrDimlight_rosalinaSleep=CtrPaintchg_rosalinaSleep")
endif()

# Shared idle-bottom contract, both UNDERTALE targets. The new module
# continues through CtrRenderer_chainEndFrameBoxcache on Old, real endFrame on New.
if(CTRPORT_GAME STREQUAL "undertale")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_postkick.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "__real_CtrRenderer_endFrame=CtrBottomPaint_endFrame")
    if(NOT CTRPORT_TARGET STREQUAL "o3ds")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "C3D_FrameEnd=${CTRPORT_FRAMEEND_LINK}")
    endif()
    foreach(paintchg_writer main ctr_log ctr_argui ctr_optmenu ctr_renderer z_scanout z_loadscreen ctr_rosalina z_pacerflush z_argcrash)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${paintchg_writer}.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "GSPGPU_FlushDataCache=CtrBottomPaint_cpuFlush")
    endforeach()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dimlight.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "CtrArgui_keepLit=CtrBottomPaint_keepLit")
    foreach(paintchg_entry CtrBlank_yieldBottom CtrDimlight_setDisplay CtrDimlight_noticeClear CtrScreens_screenshotNotice CtrDimlight_rosalinaSleep)
        target_link_options(ctrport PRIVATE "-Wl,--wrap=${paintchg_entry}")
    endforeach()
    # Frame counters serve the lab's skip proof only. The public release path
    # retains every draw/ownership decision without the counter writes/payload.
    if(CTRPORT_RELEASE_SOURCE_URL)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_boxcache.c" APPEND PROPERTY COMPILE_DEFINITIONS
            CTRPORT_PAINTCHG_RELEASE)
    endif()
endif()

# Frame is not a 1.0 feature (owner ruling, 2026-11-09). Sources remain
# recoverable, but neither an old CMake cache nor a private pack enables it.
if(CTRPORT_GAME STREQUAL "undertale")
    target_compile_definitions(ctrport PRIVATE CTRPORT_NO_BOTTOMFRAME)
endif()
file(REMOVE "${CTRPORT_ROMFS_DIR}/bottomframe.bin")
foreach(retired_frame_area RANGE 0 6)
    file(REMOVE "${CTRPORT_ROMFS_DIR}/bottomframe/${retired_frame_area}.bff")
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/asrholdbenchready.cmake")
# noticescroll: original call graphs and frozen bodies compile as before; three clear calls
# are redirected to the appended bulk clear (including its bridge preserving GCC's extra r1 promise).
# Reader edges use resident records and an 8 KiB decode cache, with bounded FILE fallback. Existing close/exit wrappers release both.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(noticescroll_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/noticescroll_compile.py;${noticescroll_delegate};--noticescroll-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_legal.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/noticescroll_compile.py")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_legal.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_noticescroll_cache.inc")
endif()

# Menu copies already own their presentation schedule. Qualify final object relocations
# after the normal layout adapters; leave all original body bytes and other copies alone.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(portmenu_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/portmenu_compile.py;${portmenu_delegate};--portmenu-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_menuborder.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_scanout.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/portmenu_compile.py")
endif()

# bottomflip (change bonepulse, 11-09; UNDERTALE only): with the game on top, a held bottom-screen flip is presented from
# the bottom's own vblank callback instead of the top's, and both screens' real shown intervals are counted (the late
# code at the end of source/ctr_frame_pacer.c). Link wraps only, so no existing body changes. The HOME audio probe
# wraps gspPresentBuffer itself (a test build), so it leaves bottomflip out. CtrPacer_init's wrap (the state, made
# before the first frame) is DELTARUNE's pixfix's there; both are never in one build.
if(GAME_WAD_DEFINE STREQUAL "ENABLE_WAD16" AND NOT CTRPORT_HOMEAUDIO_PROBE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_frame_pacer.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_BOTTOMFLIP)
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=gspPresentBuffer,--wrap=gspIsPresentPending,--wrap=CtrBottomPaint_vblank1Lock,--wrap=CtrGpu_takePaceStats
        -Wl,--wrap=CtrPacer_init)
endif()

# touchui step 1: battletouch's weak sample extension supplies the pen/keys and owns the merge; no HID wrapper.
# All new code/constants append after battletouch in the text reserve, with state allocated on the heap.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchui.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchmute.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=VM_registerBuiltin)
    if(CTRPORT_TOUCH_TEST)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchui.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchmute.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchquiz.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TOUCH_TEST)
    endif()
endif()

# Self-contained touchui lab packages. This changes no Release or normal touchtest
# build, and never replaces the shared sampler. The existing fopen policy keeps
# its body; only its exported name changes so the queue adapter can forward to it.
option(CTRPORT_TOUCH_QUEUE "Test only: read touch tape and bench scenes from this build's RomFS" OFF)
if(CTRPORT_TOUCH_QUEUE)
    if(NOT CTRPORT_TOUCH_TEST OR NOT CTRPORT_GAME STREQUAL "undertale" OR CTRPORT_PACER_CSV_DIAG)
        message(FATAL_ERROR "touchqueue requires UNDERTALE touchtest with the ordinary bench I/O policy")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchqueue.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TOUCH_QUEUE)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchqueue.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_bench_io.c"
        APPEND PROPERTY COMPILE_DEFINITIONS __wrap_fopen=CtrTouchQueue_originalOpen)
endif()

# Quiz: preserve the shared sampler's GCC call graph and every allocated object
# byte. Route only its physical-input continuation and sample callback operands.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchquiz.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    get_target_property(touchquiz_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/touchquiz_compile.py;${touchquiz_delegate};--touchquiz-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/touchquiz_compile.py")
endif()

# Popup menus reuse touchui's retained scheduler and extend its sample callback.
# Their reader/adapter use AAC slots; only the heap-root pointer is appended BSS.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchmenus.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTouchExt_sample)
endif()

# Overworld: append AAC code and preserve the existing sampler/menu/audio objects.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchoverworld.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    if(CTRPORT_TOUCH_TEST)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchoverworld.c"
            APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TOUCH_TEST)
    endif()
    get_target_property(touchoverworld_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/touchoverworld_compile.py;${touchoverworld_delegate};--touchoverworld-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchquiz.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchmute.c"
        APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/touchoverworld_compile.py")
endif()

# Naming: append ordinary late code/constants; retain the two qualified edges (lead 11-10).
option(CTRPORT_TOUCH_NAME_TRACE "Test only: per-frame naming trace for naming campaigns" OFF)
if(CTRPORT_TOUCH_NAME_TRACE)
    if(NOT CTRPORT_TOUCH_TEST OR NOT CTRPORT_GAME STREQUAL "undertale" OR CTRPORT_RELEASE_SOURCE_URL)
        message(FATAL_ERROR "nametrace requires a private UNDERTALE touchtest naming campaign")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchname.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TOUCH_NAME_TRACE)
endif()
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchname.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    if(CTRPORT_TOUCH_TEST)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchname.c"
            APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_TOUCH_TEST)
    endif()
    get_target_property(touchname_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/touchname_compile.py;${touchname_delegate};--touchname-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battletouch.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchquiz.c"
        APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/touchname_compile.py")
endif()

# P5-1 runs last: earlier layout adapters may emit assembly before producing the final main.o.
# This orders the inner adapters; the reviewed outer launchers below delegate through P5-1.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_RELEASE_SOURCE_URL)
    # Preserve main.c's approved source revision and its compiled slot. Route its sole stat relocation only.
    get_target_property(p51_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/public_benchfile_compile.py;${p51_delegate};--p51-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/public_benchfile_compile.py")
endif()

# Settings are stdio, outside the game FileSystem bench sandbox. Retarget only
# these units' fopen operands; read-only opens and non-settings output pass through.
foreach(benchsettings_unit ctr_display ctr_screens ctr_argui ctr_optmenu
        z_smalltext z_smalltext_dr z_crispart z_layout
        z_battlesplit z_battletouch ctr_notice)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${benchsettings_unit}.c"
        APPEND PROPERTY COMPILE_DEFINITIONS fopen=CtrBenchSettings_fopen)
endforeach()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_benchsettings.c" PROPERTIES
    COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")

# shopsplit: S3 room admission, existing Text place, shared held targets.
if(CTRPORT_GAME STREQUAL "undertale")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_shopsplit.c")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrRenderer_chainBeginFrameBattlesplit,--wrap=CtrRenderer_chainEndFrameBattlesplit)
endif()

# gpuburst F1 (UNDERTALE on the Old models): the tiles' and seam copies' CtrMemory_vramAllocTop by VRAM bank
# (source/z_vramtop.c, appended late code). Wrapped, so the callers keep their code; --undefined keeps ctr_memory.c's
# version, unused, where it was.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrMemory_vramAllocTop,--undefined=CtrMemory_vramAllocTop)
endif()

# bootline: consoleInit publishes only its physical console LCD. Retain the SDK
# global-swap definition in its old slot even though its sole caller is wrapped.
if(CTRPORT_GAME STREQUAL "undertale")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_bootline_console.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_bootline_console.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-jump-tables")
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=gfxSwapBuffersGpu,--undefined=gfxSwapBuffersGpu)
endif()

# dlgflash: UNDERTALE routing transitions only; no DR compile/link changes.
if(CTRPORT_GAME STREQUAL "undertale")
    # Filter the base policy before either popup wrapper initialises its metadata.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_popbot2.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "__real_CtrDialogue_init=CtrDlgflash_init")
    get_target_property(dlgflash_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/dlgflash_compile.py;${dlgflash_delegate};--dlgflash-compiler")
    set_property(SOURCE "${ENGINE_DIR}/src/runner.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/dlgflash_compile.py")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgflash.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/utwindow_compile.py")
    if(NOT CTRPORT_RELEASE_SOURCE_URL)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_DLGFLASH_LOG)
    endif()
endif()

# S4: keep the original frame chain and retarget its real begin-frame call.
# The renderer helper pass supplies the AAC-only implementation.
if(CTRPORT_GAME STREQUAL "undertale")
    # Return S4's loan before battlesplit admits a flight or moves up, before
    # Step. The old inputlate body retains only a changed branch operand.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "CtrLayout_chain18=CtrDlgtop_frame")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c" APPEND PROPERTY
        COMPILE_DEFINITIONS "__real_CtrRenderer_beginFrame=CtrDlgtop_begin")
    # Last links: boxcache delegates twice on Old; bottompaint once on New.
    # Retarget only their real end-frame calls, retaining both existing bodies.
    foreach(dlgtop_end_unit z_boxcache z_bottompaint)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${dlgtop_end_unit}.c" APPEND PROPERTY
            COMPILE_DEFINITIONS "__real_CtrRenderer_endFrame=CtrDlgtop_end")
    endforeach()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgtop.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgtop_lease.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgtop_buffers.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgtop_birth.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_dlgtop.h")
endif()
# mexdlg: default-on fixed-address native-M writer adapter for dev and release.
include("${CMAKE_CURRENT_SOURCE_DIR}/mexdlg_native_m.cmake")

# Shops/SAVE: an outer continuation, retaining the complete accepted chain.
# New code/constants use underscore sections, after every existing late input.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_touchshops.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-marm;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Nn_sample)
endif()

# fpsghost: bounded telemetry belongs to the non-public dev line, independent of
# the optional player screenshot feature. Public release objects see no define.
# The diagnostic reads GPU registers and logs inside every Options open/close window, so it would skew dev
# timing rows (toolfix F2): dev builds compile it only with this opt-in word (lead 11-11 09:0x).
option(CTRPORT_FPSGHOST_DIAG "fpsghost Options-window GPU/log diagnostic, dev opt-in" OFF)
if(CTRPORT_GAME STREQUAL "undertale" AND NOT CTRPORT_RELEASE_SOURCE_URL AND CTRPORT_FPSGHOST_DIAG)
    foreach(fpsghost_unit ctr_fpsbox ctr_optmenu ctr_frame_pacer ctr_argui)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${fpsghost_unit}.c"
            APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FPSGHOST_DEV)
    endforeach()
endif()

# fpsghost return: retain the existing teardown slot and append its one-time
# RGB565 reclaim. Other-console and transfer/blank gate instructions stay exact.
if(CTRPORT_GAME STREQUAL "undertale")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_fpsghost_return.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_fpsghost_return.c" PROPERTIES
        COMPILE_OPTIONS "-marm;-Os;-fno-merge-constants;-fno-jump-tables")
    get_target_property(fpsghost_return_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/fpsghost_return_compile.py;${fpsghost_return_delegate};--fpsghost-return-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_gpu_queue.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/fpsghost_return_compile.py")
endif()

# Third reviewed outer launcher: dlgflash, fpsghost_return, then sfxlate.
# sfxlate intentionally retargets one main.o call after both passthroughs.
# sfxlate: room-258 fan PCM must be ready before its first pitch-3 play. The
# ordinary object passes every existing adapter first; only appended code and
# one qualified main call relocation are added. Debug/profile builds retain
# their original path (the optimized resolver slot is a Release-only contract).
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds"
   AND CMAKE_BUILD_TYPE STREQUAL "Release"
   AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_FREEWINS
   AND CTRPORT_USE_TREMOR AND TREMOR_INCLUDE_DIR AND TREMOR_LIBRARY AND OGG_LIBRARY)
    get_target_property(sfxlate_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/sfxlate_compile.py;${sfxlate_delegate};--sfxlate-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/sfxlate_compile.py;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/sfxlate_object.py;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/portmenu_compile.py;${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_sfxlate_cooler.inc")
endif()
