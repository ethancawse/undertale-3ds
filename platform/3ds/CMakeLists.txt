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
# UNDERTALE fits the Old 3DS's standard 64 MB, the same as its .3dsx (decided 10-25), so both its CIAs ignore the setting
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
file(CONFIGURE OUTPUT "${CTRPORT_BUILD_ID_DIR}/ctr_release_contract.h"
    CONTENT "#define CTR_RELEASE_BUILD_ID_SIZE ${RELEASE_build_id_storage_bytes}\n#define CTR_RELEASE_O3DS_FILENAME \"${RELEASE_O3DS_FILENAME}\"\n#define CTR_RELEASE_LEGAL_COLUMNS ${RELEASE_legal_columns}\n#define CTR_RELEASE_LEGAL_LINES ${RELEASE_legal_lines}\n#define CTR_RELEASE_SOURCE_URL \"${RELEASE_SOURCE_URL}\"\n#define CTR_RELEASE_SOURCE_HEADING \"${RELEASE_SOURCE_HEADING}\"\n#define CTR_RELEASE_LABEL \"${RELEASE_VERSION}\"\n" @ONLY)
add_custom_target(ctrport_build_id
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${CTRPORT_BUILD_ID_HEADER}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DAOT_DIR=${CTRPORT_AOT_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/build_id.cmake"
    BYPRODUCTS "${CTRPORT_BUILD_ID_HEADER}"
    VERBATIM)

# ===[ Engine core ]===
file(GLOB ENGINE_SOURCES "${ENGINE_DIR}/src/*.c")
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
# UNDERTALE's collision rules (colflip): late code only, after collision_compat.c.
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/collision_gm14.c")
list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/collision_gm14.c")
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
# DELTARUNE Chapter 3's board grid rebuild (boardgrid, simplefix), bound by gmlaot.py's BOARD_NATIVE: late code
# only, last; DELTARUNE New only (gmlaot.py binds it only for --target n3ds).
list(REMOVE_ITEM ENGINE_SOURCES "${ENGINE_DIR}/src/z_boardgrid.c")
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    list(APPEND ENGINE_SOURCES "${ENGINE_DIR}/src/z_boardgrid.c")
endif()

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

# PacketVideo's AAC-LC decoder, for the audio of the games' intro videos (vendor/pvaac/README.md). Its C++ sources
# are kept as C; they only need bool from C++.
file(GLOB PVAAC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pvaac/src/*.c")
set_source_files_properties(${PVAAC_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;-include;stdbool.h"
    COMPILE_DEFINITIONS "OSCL_IMPORT_REF=;OSCL_EXPORT_REF=;OSCL_UNUSED_ARG=")
list(APPEND VENDOR_SOURCES ${PVAAC_SOURCES})

# ===[ 3DS platform layer ]===
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.c")
# GPU-1 is compiled only by the New DR switch below.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c")
if(NOT CTRPORT_GAME STREQUAL "undertale" OR NOT CTRPORT_GOG_COMPAT)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_gogut.c")
endif()
# sdlat is appended only to profile builds below; ordinary Release has no object.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_sdlat.c")
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
# The spike recorder (spikerec, build.sh ... spikerec): playtest builds only. Without the word its file is not
# even compiled, so release builds link exactly the objects they did before (its wraps and defines are further down).
option(CTRPORT_SPIKEREC "Spike recorder: frames that miss their slot, written to spikes.txt at room changes (playtests)" OFF)
if(NOT CTRPORT_SPIKEREC)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_spikerec.c")
endif()
# frametrace (benchfix, build.sh ... frametrace): a bench window's frames and SD, decode and tile events, test
# builds only. Without the word its file is not even compiled (its wraps and defines are further down).
option(CTRPORT_FRAMETRACE "Frame trace: every frame of a bench window and its SD, decode and tile events, to frametrace.txt (tests)" OFF)
if(NOT CTRPORT_FRAMETRACE)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_frametrace.c")
endif()
# freewins (build.sh ... freewins): main-loop segment timers and off switches, diagnostic builds only. Without the
# word its file is not compiled and its define reaches no object, so release builds are unchanged.
option(CTRPORT_FREEWINS "Main-loop segment timers and off switches (freewins diagnostics)" OFF)
if(NOT CTRPORT_FREEWINS)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_freewins.c")
endif()
# The exit trail (credfix, build.sh ... exittrail): o2DS kernel panic diagnosis builds only, never a release.
# Without the word its file is not compiled (its wraps are further down).
option(CTRPORT_EXITTRAIL "Exit trail: the exit's steps after the last log line, in labboot.txt (diagnosis)" OFF)
if(NOT CTRPORT_EXITTRAIL)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_exittrail.c")
endif()
# dodgefront (credfix, charter rev 1 item 2, option 3): UNDERTALE Old 3DS only. The centred text line's hand front
# (source/z_dodgefront.c), bound by vm.c's late VM_aotBindFront through CtrRenderer_prefetchRoom's wrap chain
# (z_roomplan.c, ctr_wrap_chain.h). Late text only; the definition goes on the three files that read it.
# build.sh passes this cache setting for every target; only UT Old consumes its behavior below.
set(CTRPORT_WRITERFRONT_MODE "ON" CACHE STRING "Writer front: ON, OFF, FALLBACK or COUNT (UNDERTALE Old)")
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    set_property(SOURCE "${ENGINE_DIR}/src/vm.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/z_roomplan.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dodgefront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DODGEFRONT)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dodgefront.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
    # textfront (simplefix): the centred-text scripts' fronts with the view cull (source/z_textfront.c), the
    # prefetchRoom chain's link ahead of dodgefront's (ctr_wrap_chain.h). Late text only.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_DODGEFRONT)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c" APPEND PROPERTY COMPILE_OPTIONS
        "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
    # writerfront (freebies): the dialogue writer's Draw as hand code with the generated function as its fallback
    # (source/z_writerfront.c), the prefetchRoom chain's link between textfront's and dodgefront's (ctr_wrap_chain.h)
    # and vm.c's late VM_aotSwapFront. Late text only. build.sh's nowriterfront / writerfront-fallback /
    # writerfront-count words (test builds) leave it out, send every Draw to the fallback, or count.
    if(CTRPORT_WRITERFRONT_MODE STREQUAL "OFF")
        list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c")
    else()
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
    endif()
else()
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_dodgefront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_textfront.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c")
endif()
# utorder (credfix): UNDERTALE only; its wraps and options are further down.
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
# The phase plan (source/z_phaseplan.c) is DELTARUNE only (ruling, 2026-10-25: on UNDERTALE it gained about
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
endif()

# Deep frame skip's Draw classes for this game (bench/drawskip_<game>.txt), as drawskip_txt.h.
set(DRAWSKIP_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/bench/drawskip_${GAME_DIR_NAME}.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DRAWSKIP_SOURCE}")
configure_file("${DRAWSKIP_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/drawskip.txt" COPYONLY)
dkp_add_embedded_binary_library(drawskip_data "${CMAKE_CURRENT_BINARY_DIR}/drawskip.txt")

list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_legal_romfs.c")
# The test-only counter probe (CTRPORT_ASG_PROBE, below) is compiled only in its own build.
list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c")
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

# Late sections and their fixed reserves (late_reserve.ld says why and how features use them). ld only finds 3dsx.ld's
# sections for that script's INSERTs when the script comes first, and the toolchain's -specs=3dsx.specs names 3dsx.ld
# first. The link therefore uses a copy of devkitARM's 3dsx.specs that names late_reserve.ld ahead of it. The specs
# hold native paths: gcc and ld are Windows programs even when CMake runs in MSYS2.
set(CTRPORT_LATE_LD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/late_reserve.ld")
set(CTRPORT_LATE_LD "${CMAKE_CURRENT_BINARY_DIR}/late_reserve.ld")
file(READ "${CTRPORT_LATE_LD_SOURCE}" CTRPORT_LATE_LD_TEXT)
option(CTRPORT_PROFILE "Compile in the GML script profiler" OFF)
if(CTRPORT_PROFILE OR CTRPORT_REACH_GATE)
    # Resolve this before ld reads the script: DEFINED with a later --defsym
    # in 3dsx.specs' link command can still select the Release default.
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
# Round 16 (rulings (a), integrate16, 2026-10-25): DELTARUNE Old two more pages, 0x38000; DELTARUNE New two,
# 0x3B000 (rename's SD folder move 3460 B on both, packscreen's first time setup 4997 B on both, flowergpu4's
# decoration runs 4536 B on New; Old had 646 B left after builderx and drstep F1, New 6710 B). Only that target's
# .rodata, .data and .bss move, by whole pages.
# Then one more page on each (ruling, 2026-10-25): DELTARUNE Old 0x39000, New 0x3C000, for the runtime RomFS
# name fix after the rename and the options menu and boot log fixes (1944 B on both; Old was 1557 B short, New 37 B).
# Only that target's .rodata, .data and .bss move, by one page.
# Round 17 (ruling option A, integrate17, 2026-10-25): five more pages on each, DELTARUNE Old 0x3E000, New
# 0x41000. The merged round (smartcut, rosalina2, the tile loop, the phase plan, the pacer spool) left Old 7,589 B
# and New 7,269 B over; screenshot (shots) builds use about 5.7 KB more; then utstall item 8 (1,190 B), flowergpu's
# hook (1,821 B, New) and one spare page. Only that target's .rodata, .data and .bss move, by whole pages.
# Round 18 (integrate18, 2026-10-26): DELTARUNE Old one more page, 0x3F000 (the merged round left 8,979 B, and
# 3,299 B in shots builds: one spare page). DELTARUNE New ten more, 0x4B000, for the work that all lands there:
# the merged round left 9,299 B (3,635 B in shots); drresid uses about 13.2 KB, hitches' room preload 0x3000, then
# drresid's Flowery work and knightnew, and one spare page. Only that target's .rodata, .data and .bss move.
# Round 21 (ruling A, gamedir2, 2026-10-26): DELTARUNE Old one more page, 0x40000, for gamedir (the game folder's
# listing and settings.ini's worker save: 8,652 B there, its constants in late text since DELTARUNE Old had 505 B of
# late constants left), 1,165 B over at 0x3F000; 2,931 B left. 4 KiB less heap on that target only.
# Then two more pages, 0x42000 (integrate21, 2026-10-26): legalui's Legal and Credits pages (3,566 B) did not fit in
# the 2,931 B gamedir2 left (the link failed), and credfix brings 1,444 B more; one spare page. Only DELTARUNE Old's
# .rodata, .data and .bss move, by two pages (8 KiB less heap on that target only).
# Round 22 (integrate22, approved shipping claims, 2026-10-27): DELTARUNE New three more pages, 0x4E000. cand21
# left 19,376 B; the approved claims are 24,753 B: hitches' room preload 12,917, titanthrash R4 2,745, flowend rounds
# 5-6 2,206, knightnew's pool 2,048 and GPU-2 1,288, perf-titan's markers <= 2,048, perf-knight rank 2 901, drsweep's
# list read 348 and credcap 252. Two pages cover them (2,815 B left); the third is the spare page (shots builds use
# about 5.7 KB more). Only DELTARUNE New's .rodata, .data and .bss move, by three pages (memory is plentiful on New).
# integrate24 event 2 (plan cbfecf919, approved 2026-10-27): DELTARUNE New four more pages, 0x52000. The stage
# left 6,307 B after freewins' wins 2+4; the claims are knightgpu L-A 1,476 and perf-flowery L2b about 10,132
# (an estimate), so three pages cover them and the fourth is the spare page. Late rodata (2,459 B left) is
# unchanged: knightgpu uses none. Only DELTARUNE New's .rodata, .data and .bss move, by four pages (linear -16 KB).
# bubblecheck test builds (CTRPORT_BUBBLE_CHECK, never Release): four more pages for the checker, which is late code
# with the rest of z_smalltext.c. Added ahead of the game's own value below, which then replaces the placeholder.
if(CTRPORT_BUBBLE_CHECK)
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "0x4000 + /* LATE_TEXT_RESERVE */ 0x30000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
if(CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "o3ds")
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "0x42000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
elseif(CTRPORT_GAME STREQUAL "deltarune")
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "0x52000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
    string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x3000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
else()
    # UNDERTALE, Old and New 3DS: late code 0x54000, set once for a batch of merges. A probe with all of them (the
    # waterfall, lava2, the Smoothing menu, the dust swarm, the builder) used 0x4c564 B on Old (+42,055 B over the
    # previous 0x44000 build); the rest is room for z_sdmigrate (3.5 KB) and the merge-later list (the tile loop and
    # helix step, symverdict, the battle code reads, Asgore 8.9 KB, smartcut, the keyboard: about 20 KB) plus one spare
    # page. Before that it was 0x44000.
    # Round 17 (ruling option A, integrate17, 2026-10-25): ten more pages, 0x5E000. The merged round (Asgore,
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
    # Only that target's .rodata, .data and .bss move, by whole pages (check_layout --expect-shift, shiftproof.py).
    # Round 21f (integrate21, ruling 2026-10-27): one more page, 0x65000 (GOG 0x67000). utflowey2 (packed copies
    # in spare linear memory, 6,409 B) left Steam Old 305 B, and the Old shots and halfart builds no longer linked
    # (about 1.3 / 1.8 KB over); P1 work still to come (credfix option 3 about 2 KB, uttour's helpers, pacer no-age).
    # Round 22 (integrate22, ruling dec-1027-07, 2026-10-27): one more page, 0x66000 (GOG 0x68000). The approved
    # UNDERTALE Old claims are 5,595 B (credfix option 3 2,135 + credcap 252, flowend rounds 5-6 2,856, drsweep's
    # list read 348) against the 4,402 B that round 21f's page left.
    # Round 25 (integrate21 cand25 PLAN, approval 2026-10-28): two more pages on the Old 3DS only, 0x68000 (GOG
    # 0x6A000); New keeps 0x66000 and its addresses. Old had 2,389 B left after the sound-effect pack (10,360 B),
    # pcmwait, c112dtm and pacer NF1, against credfix T1 (2,505 B), benchfix r7 (2,292 B) and freewins (b). On the Old
    # 64 MB layout the heap is on its 20 MB floor, so the 8 KiB comes off linear memory (the tile budget).
    set(ctrport_ut_late 0x66000)
    if(CTRPORT_TARGET STREQUAL "o3ds")
        set(ctrport_ut_late 0x68000)
    endif()
    if(CTRPORT_GOG_COMPAT)
        # Ruling, gogut round 17: GOG UT alone needs two more 4 KiB pages over Steam (integrate22: over
        # round 22's 0x66000).
        math(EXPR ctrport_ut_late "${ctrport_ut_late} + 0x2000" OUTPUT_FORMAT HEXADECIMAL)
    endif()
    string(REPLACE "/* LATE_TEXT_RESERVE */ 0x30000" "${ctrport_ut_late}"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# UNDERTALE Old 3DS: one more 4 KiB page of late constants, 0x3000 (45 B
# were left, with the per-effect fast paths still to come). After the profile/reachgate substitution above, which
# already gives 0x4000. Only this target's .data and .bss move, by one whole page; no text or .rodata address changes.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    string(REPLACE "/* TEST_RODATA_RESERVE */ 0x2000" "0x3000"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
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
# The spike recorder's playtest builds only (spikerec): two more 4 KiB pages of late code on top of the target's
# reserve, for its 4.8 KB (UNDERTALE Old had 572 B left on main a7ed1173e, New 2036 B). Text keeps its addresses;
# .rodata, .data and .bss move by two whole pages in those builds alone. Release builds never see this.
if(CTRPORT_SPIKEREC)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x2000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# frametrace's test builds only (benchfix): the same two pages for its code and constants (z_frametrace.c). Text
# keeps its addresses; .rodata, .data and .bss move by whole pages in those builds alone.
if(CTRPORT_FRAMETRACE)
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x2000 +"
           CTRPORT_LATE_LD_TEXT "${CTRPORT_LATE_LD_TEXT}")
endif()
# Screenshot (shots) builds on Old 3DS, both games (ruling, legalui round 21, 2026-10-26): one more 4 KiB page of
# late code. Shots builds keep test code release builds leave out, and cand20 left UNDERTALE Old 2,984 B and DELTARUNE
# Old 1,807 B there, less than the Legal and Credits pages' 3.5 KB. Lab and test builds only: release builds, their
# layout and heap are unchanged; in shots builds .rodata, .data and .bss move by one page.
if(CTRPORT_PLAYER_SHOTS AND CTRPORT_TARGET STREQUAL "o3ds")
    string(REPLACE "__late_text_limit = ALIGN(__late_text_start, 0x1000) +" "__late_text_limit = ALIGN(__late_text_start, 0x1000) + 0x1000 +"
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
# Diagnostic: records what the DSP mixes and every channel start/stop (platform/3ds/diag/ctr_audio_capture.c).
# ctr_audio_system.c gets a forced include that routes its NDSP channel calls there; its source is unchanged.
option(CTRPORT_AUDIO_CAPTURE "Record the DSP mix and channel events to the SD card (diagnostic)" OFF)
if(CTRPORT_AUDIO_CAPTURE)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_audio_capture.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_system.c" PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/diag/ctr_audio_capture_wrap.h")
endif()
# Diagnostic: logs an FNV-1a hash of every stream's queued PCM (ctr_audio_system.c), to compare builds sample for sample.
option(CTRPORT_AUDIO_PCM_HASH "Log a hash of every stream's PCM (diagnostic)" OFF)
if(CTRPORT_AUDIO_PCM_HASH)
    target_compile_definitions(ctrport PRIVATE CTR_AUDIO_PCM_HASH)
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
# Diagnostic (offdraw): instance census and offdraw.txt's kill / hash-skip experiments (platform/3ds/diag/
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
    # The bench's held-frame lines (ctr_benchheld.inc, stallfix): read at benchshownRead's flip read, reported at
    # each scene's result. The retired cushion prototype cannot wrap this path.
    # DELTARUNE Old builds link none of it (its +2 KB of late text did not fit there, integrate24, 10-28): that build
    # stays byte-identical but for its build id, until a DELTARUNE Old late page takes it.
    target_compile_definitions(ctrport PRIVATE CTRPORT_BENCH_HELD=1)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPacer_getFlipTotals,--wrap=CtrPacer_logLevels)
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
# source/memory_report.c (heapfrag): each heap layout report also gives the largest block malloc can hand out and the
# largest linear piece, and the out-of-memory screen records the top chunk at the failure; the log writer thread warms
# its number caches before it starts. Wrapped, so ctr_memory.c's code and its callers' stay as they were.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrMemory_logHeapLayout,--wrap=CtrMemory_emergencyFree,--wrap=CtrLab_runThread)
# nativemac: data versions added after revision 1 (ctr_data_version.c's end). The one wrap of CtrDataVersion_check;
# its chain's links and their order are in source/ctr_wrap_chain.h.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrDataVersion_check)
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
    -Wl,--wrap=CtrPacer_onVBlank
    -Wl,--wrap=CtrDisplay_topModeName,--wrap=CtrDisplay_stereoModeName,--wrap=CtrDisplay_bottomModeName,--wrap=CtrLog_emergencyKeepLink)
# The options menu holds every sound while it is open (menupause), as late code at the
# end of source/ctr_audio_system.c: the menu's open wrap above calls CtrLegal_close first (its only caller outside
# ctr_legal.c), and every close goes through CtrMenu_close. Wrapped, so main's and the menu's code keep their addresses.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLegal_close,--wrap=CtrMenu_close)
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
# Luma3DS's Rosalina menu with the other screen Off (rosalina2, playtest report 2026-10-25): source/ctr_rosalina.c, all late
# code, lights the bottom backlight while the player's buttons form Rosalina's combo. gspLcdExit is wrapped to start
# its watcher at the first backlight change that leaves the bottom screen dark. Compiled like ctr_screens.c (named
# constants, no folded printf or switch tables). Release builds log nothing from it; CTRPORT_ROSALINA_PROBE (build.sh
# "rosaprobe") adds its pause and switch log lines, and CTRPORT_ROSALINA_SIM (an Azahar test) also fakes Luma.
target_link_options(ctrport PRIVATE -Wl,--wrap=gspLcdExit)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
option(CTRPORT_ROSALINA_SIM "Test only: run the Rosalina watcher without Luma, on simulated buttons and a pause" OFF)
option(CTRPORT_ROSALINA_PROBE "Test only: log each pause and switch the Rosalina watcher sees" OFF)
if(CTRPORT_ROSALINA_PROBE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALINA_PROBE)
endif()
if(CTRPORT_ROSALINA_SIM)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_rosalina.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_ROSALINA_SIM)
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
# source/main.c: CtrLab_booted (main's call before the boot header) moves the stats and log consoles to the bottom
# screen's other buffer, and the first CtrLog_setConsoleEnabled moves them back. Wrapped, so main's code keeps its
# addresses.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrLab_booted,--wrap=CtrLog_setConsoleEnabled)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c" PROPERTIES COMPILE_OPTIONS
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
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_krsharp.c" PROPERTIES COMPILE_OPTIONS "-Os")
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
    # utflowey: z_utstall.c also wraps RoomLoad_collect (z_roomload.c; texcacheCollectWholeRead's once-a-frame call), so
    # that in a scene rule A cut, tiles with a packed copy look older than tiles that would come from the SD card.
    target_link_options(ctrport PRIVATE -Wl,--wrap=RoomLoad_collect)
    if(CTRPORT_TARGET STREQUAL "o3ds")
        # utflowey, packed-tier fix (UNDERTALE Old only): a promotion needs its size plus 4 MB of heap free (the
        # low-heap line, 3 MB, and 1 MB), not 8 MB, which a 64 MB Old 3DS never has in play. Within the packed budget,
        # idle copies go one for one for it. z_utstall.c's RoomLoad_collect wrap promotes only a page read from the SD
        # card again within a minute, and sizes the Old budget from the game's measured heap need.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_tile_reader.c" APPEND PROPERTY COMPILE_DEFINITIONS
            "READER_PROMOTION_RESERVE=(4u * 1024u * 1024u)")
        # utflowey2 (option 4, UNDERTALE Old only): packed copies also live in the linear memory a room's plan leaves
        # spare (z_utstall.c's end; ctr_texture_pack.h). The shared files' calls go to it through CTR_PKLIN_* names that
        # every other build compiles as the original calls. The wraps: the reader's promotion copy and read, the game
        # thread's whole reads, every free of a copy, the floor keeper's shed and the frame's whole-read collect
        # (the per-frame step). pklindiag (census) and pklinstress
        # (a slow promotion read that each room load stops) are test builds of their own.
        target_compile_definitions(ctrport PRIVATE CTRPORT_PKLIN)
        # codeclose (F1/F6, UNDERTALE Old only): usReadCode's call of RoomPlan_roomCode reads the plan's code after
        # the tiles with z_roomload.c's coalesced, non-aborting reader and hands usReadCode nothing (z_roomload.c's end).
        target_link_options(ctrport PRIVATE -Wl,--wrap=RoomPlan_roomCode)
        target_link_options(ctrport PRIVATE
            -Wl,--wrap=CtrPacked_new,--wrap=CtrPacked_free,--wrap=CtrPacked_read,--wrap=CtrPack_readFile,--wrap=CtrAudioSystem_shedSfx
            -Wl,--wrap=CtrTextures_collectWholeRead)
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
    # utflowey, DELTARUNE only: source/z_phaseplan.c (phaseplan) wraps RoomLoad_collect. romfs:/roomplan/phases.dtph
    # (tools/roomplan/build_phases.py, made by build.sh) splits a long room's tiles into phases; once a phase starts,
    # the next one's tiles are readied in the background.
    target_link_options(ctrport PRIVATE -Wl,--wrap=RoomLoad_collect)
endif()
# utstall item 8 step 1 (both games): a tile read the game thread performs while it waits for another
# (CtrTileReader_wait) uses the cache file the game thread keeps open instead of opening its own and handing it to
# the reader thread to close. Late code in ctr_tile_reader.c and ctr_textures.c; nothing else wraps these two.
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrTileReader_wait,--wrap=CtrTileReader_start)
# Step 1b' (both games): a finished pack worker closes its data.win handles (ctr_texture_pack.c's tail, the wrap of
# CtrPackWorker_poll, which nothing else wraps).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPackWorker_poll)
# Step 1c (both games): with the complete marker there at the chapter's start, the background worker skips the
# header-only checks once packed copies stop fitting (ctr_texture_pack.c's tail; nothing else wraps these two).
target_link_options(ctrport PRIVATE -Wl,--wrap=CtrPack_isComplete,--wrap=CtrPackWorker_setKeepPacked)
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

# Test only (utasgore): performance counters around the step, the draw and sampled drawSprite calls
# (source/ctr_asgprobe.c, all late code). The link sends main.c's Runner_step and Runner_drawViews calls to its wrappers.
option(CTRPORT_ASG_PROBE "Compile in the performance-counter frame probe" OFF)
if(CTRPORT_ASG_PROBE)
    target_compile_definitions(ctrport PRIVATE CTRPORT_ASG_PROBE)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_step,--wrap=Runner_drawViews)
    # UNDERTALE's inputlate wraps Runner_step too: in this build its wrapper is renamed (apInputlateStep) and the
    # probe's calls it, so its step runs inside the probe's (ctr_asgprobe.c).
    if(CTRPORT_GAME STREQUAL "undertale")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_inputlate.c" APPEND PROPERTY COMPILE_DEFINITIONS
                     "__wrap_Runner_step=apInputlateStep" "__real_Runner_step=apRealStep")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_asgprobe.c" APPEND PROPERTY COMPILE_DEFINITIONS
                     "AP_INPUTLATE")
    endif()
endif()

# Test only (flowhalf): the largest scale each texture page item is drawn at in one room (source/ctr_halfcensus.c,
# all late code). The link sends main.c's Runner_drawViews call to its wrapper.
option(CTRPORT_HALF_CENSUS "Compile in the half-resolution draw-scale census" OFF)
if(CTRPORT_HALF_CENSUS)
    target_compile_definitions(ctrport PRIVATE CTRPORT_HALF_CENSUS)
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_halfcensus.c")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_drawViews)
endif()
# Prototype (flowhalf): art stored at half resolution (source/ctr_halfart.c, all late code), from the patch
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
    # A fixed AOT block (aotprof, 2026-10-28): tools/aot/selection/<game>-<target>-pad.txt (tools/aot/aot_pad.py)
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
    endif()
    # UNDERTALE's late-text reclaim (argcheck) leaves aotResolveInlineBindings out of static-table builds
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
set(CTRPORT_HOTPIN_LD "${CMAKE_CURRENT_SOURCE_DIR}/hotpin/${GAME_DIR_NAME}-${CTRPORT_TARGET}.ld")
option(CTRPORT_HOTPIN "Link with the hot-text alignment pad when hotpin/ has one for this game and target" ON)
if(CTRPORT_LAYOUT_PINS_ENABLED AND CTRPORT_HOTPIN AND EXISTS "${CTRPORT_HOTPIN_LD}" AND CMAKE_BUILD_TYPE STREQUAL "Release"
   AND NOT CTRPORT_PROFILE AND NOT CTRPORT_HEAP_PROFILE AND NOT CTRPORT_AOT_STRESS AND NOT CTRPORT_MEMORY_CAP_KB
   AND NOT CTRPORT_OPENH264_LINKED AND NOT CTRPORT_REACH_GATE
   AND CTRPORT_DEBUG_RECORD_FAIL_AFTER STREQUAL "")
    ctrport_native_path(CTRPORT_HOTPIN_LD_NATIVE "${CTRPORT_HOTPIN_LD}")
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
    set_property(TARGET ctrport APPEND PROPERTY LINK_DEPENDS "${CTRPORT_HOTPIN_LD}")
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

# ===[ .3dsx ]===
# Placeholder art drawn by assets/make_art.py: one icon and banner per game and target.
if(CTRPORT_GAME STREQUAL "undertale")
    set(APP_NAME "undertale-3ds")
else()
    set(APP_NAME "deltarune-3ds (unofficial)")
endif()
set(CTRPORT_3DSX_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${RELEASE_PACKAGE_STEM}.3dsx")
include("${CMAKE_CURRENT_SOURCE_DIR}/legal.cmake")
set(APP_DESCRIPTION "GameMaker runner (Butterscotch), ${TARGET_LABEL} build")
set(APP_AUTHOR "Community contributors")
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-${GAME_DIR_NAME}-${CTRPORT_TARGET}.png")
set(APP_BANNER "${CMAKE_CURRENT_SOURCE_DIR}/assets/banner-${GAME_DIR_NAME}-${CTRPORT_TARGET}.png")

# Enabled by build.sh only for the measured GOG data digest; Steam retains the original link and bodies.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_GOG_COMPAT)
    target_compile_definitions(ctrport PRIVATE CTRPORT_GOG_COMPAT)
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrChapters_init,--wrap=CtrChapters_returnToLauncher,--wrap=CtrPack_romfsName)
endif()
set(APP_BANNER_AUDIO "${CMAKE_CURRENT_SOURCE_DIR}/assets/banner.wav")

ctr_generate_smdh(ctrport.smdh
    NAME "${APP_NAME}"
    DESCRIPTION "${APP_DESCRIPTION}"
    AUTHOR "${APP_AUTHOR}"
    ICON "${APP_ICON}"
)
# Make also needs an input dependency when title/description arguments change.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ctrport.smdh" APPEND
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
if(CTRPORT_AOT_MODULES)
    include("${CMAKE_CURRENT_SOURCE_DIR}/aot/aot_modules.cmake")
else()
    ctr_create_3dsx(ctrport OUTPUT "${CTRPORT_3DSX_OUTPUT}" SMDH ctrport.smdh ROMFS "${CTRPORT_ROMFS_DIR}")
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
# go into romfs/repack/<data.win folder>/, and ctr_repack.inc copies them into the cache folder at startup, so a
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
add_dependencies(ctrport_3dsx legal_texts)

# ===[ .cia ]===
# makerom (Project_CTR) and bannertool are not part of devkitPro; docs/building-cia.md says where to get them. Without
# them the `cia` target only prints a note, so the .3dsx build is unaffected.
find_program(MAKEROM_EXE NAMES makerom HINTS "${DEVKITPRO}/tools/bin")
find_program(BANNERTOOL_EXE NAMES bannertool HINTS "${DEVKITPRO}/tools/bin")
set(CIA_RSF "${CMAKE_CURRENT_SOURCE_DIR}/cia/${CTRPORT_TARGET}.rsf")
set(CIA_EXTRA_DEPENDS "")
# Every package includes legal texts and the bench scenes; chapter AOT modules share this RomFS.
# makerom paths are relative to its build working directory.
file(READ "${CIA_RSF}" CIA_RSF_TEXT)
set(CIA_RSF_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/cia-${CTRPORT_TARGET}-romfs.rsf")
file(RELATIVE_PATH CIA_ROMFS_REL "${CMAKE_CURRENT_BINARY_DIR}" "${CTRPORT_ROMFS_DIR}")
file(WRITE "${CIA_RSF_ROMFS}" "${CIA_RSF_TEXT}
RomFs:
  RootPath: ${CIA_ROMFS_REL}
")
set(CIA_RSF "${CIA_RSF_ROMFS}")
set(CIA_EXTRA_DEPENDS ${CTRPORT_AOT_MODULE_FILES} "${CTRPORT_ROMFS_DIR}/bench_scenes.txt"
    "${COVERAGE_ROMFS_FILE}" ${BTNPROMPT_ROMFS_FILES} ${LOADSCREEN_ROMFS_FILES} "${CTRPORT_FONT_TABLES_STAMP}")
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

    add_custom_command(OUTPUT ctrport.icn
        COMMAND "${BANNERTOOL_EXE}" makesmdh -s "${APP_NAME}" -l "${APP_DESCRIPTION}" -p "${APP_AUTHOR}"
                -i "${APP_ICON}" -f "${CIA_SMDH_FLAGS}" -o ctrport.icn
        DEPENDS "${APP_ICON}" "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
        VERBATIM
    )
    add_custom_command(OUTPUT ctrport.bnr
        COMMAND "${BANNERTOOL_EXE}" makebanner -i "${APP_BANNER}" -a "${APP_BANNER_AUDIO}" -o ctrport.bnr
        DEPENDS "${APP_BANNER}" "${APP_BANNER_AUDIO}"
        VERBATIM
    )
    add_custom_command(OUTPUT "${CIA_OUTPUT}"
        COMMAND "${MAKEROM_EXE}" -f cia -o "${CIA_OUTPUT}" -target t -exefslogo
                -rsf "${CIA_RSF}" -elf "$<TARGET_FILE:ctrport>"
                -icon ctrport.icn -banner ctrport.bnr
                -major ${RELEASE_CIA_MAJOR} -minor ${RELEASE_CIA_MINOR} -micro ${RELEASE_CIA_PATCH}
                "-DAPP_TITLE=${CIA_TITLE}" "-DAPP_PRODUCT_CODE=${CIA_PRODUCT_CODE}" "-DAPP_UNIQUE_ID=${CIA_UNIQUE_ID}"
                "-DAPP_SYSTEM_MODE=${CIA_O3DS_MEMORY}" "-DAPP_SYSTEM_MODE_EXT=${CTRPORT_N3DS_MEMORY}"
        DEPENDS ctrport "${CIA_RSF}" "${CMAKE_CURRENT_BINARY_DIR}/ctrport.icn" "${CMAKE_CURRENT_BINARY_DIR}/ctrport.bnr"
                ${CIA_EXTRA_DEPENDS} ${RELEASE_LEGAL_FILES}
        COMMENT "Building CIA ${CIA_OUTPUT}"
        VERBATIM
    )
    add_custom_target(cia DEPENDS "${CIA_OUTPUT}")
    add_dependencies(cia legal_texts)
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
get_target_property(vine_delegate ctrport C_COMPILER_LAUNCHER)
if(NOT CTRPORT_PYTHON_EXECUTABLE)
  message(FATAL_ERROR "Diagnostic launcher requires the qualified host Python")
endif()
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/vine_old_observer.inc" "${CMAKE_CURRENT_SOURCE_DIR}/source/vine_new_retain.inc")
set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER "${CTRPORT_PYTHON_EXECUTABLE};-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analysis/vine_old_observer_compile.py;${vine_delegate};--")

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
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utsfx.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_freebies.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
if(CTRPORT_GAME STREQUAL "undertale")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrRenderer_skipPresent)
    # freebies (source/z_freebies.c): restarting the room into another planned scene gets the room load. Wrapped, so the
    # runner, main.c and z_utstall.c keep their code; DELTARUNE compiles the file empty and wraps nothing.
    target_link_options(ctrport PRIVATE -Wl,--wrap=Runner_handlePendingRoomChange,--wrap=RoomPlan_utstallPlan)
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
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_drresid.c" PROPERTIES COMPILE_OPTIONS
    "-Os;-fno-builtin-printf;-fno-builtin-puts;-fno-builtin-putchar;-fno-builtin-fprintf;-fno-builtin-fputs;-fno-tree-switch-conversion;-fno-jump-tables")
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

# Keep UT's release-only performance-test menu guard; DR uses upstream's exact runtime.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(builderx_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py;${builderx_delegate};--builderx-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c"
        APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py")
endif()
# DR compact adapters: only main's three cold calls change; UT's compiler path stays exactly as qualified.
if(CTRPORT_GAME STREQUAL "deltarune")
    get_target_property(builderx_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/builderx_compile.py;${builderx_delegate};--builderx-compiler")
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

if(CTRPORT_FREEWINS)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_freewins.c" "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" "${ENGINE_DIR}/src/runner.c"
        APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_FREEWINS)
endif()

# The spike recorder (spikerec; source/z_spikerec.c says how it works): the main loop's watchdog marks, its
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
# frametrace (benchfix; source/z_frametrace.c says what it records): the main loop's watchdog marks, the SD and
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
# The exit trail (source/z_exittrail.c says what it marks): calls into the exit's steps are wrapped, so every existing
# function keeps its code; only those call targets change.
if(CTRPORT_EXITTRAIL)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_exittrail.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=CtrExit_closeFilesLog,--wrap=threadJoin,--wrap=fsExit,--wrap=aptExit,--wrap=srvExit,--wrap=__appExit,--wrap=__libctru_exit,--wrap=CtrLinkFiles_enable,--wrap=CtrExit_flush,--wrap=CtrExitOnce_chain,--wrap=CtrLinkFiles_prepare,--wrap=CtrLab_crashTest)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_leavekeep.c" APPEND PROPERTY COMPILE_DEFINITIONS CTRPORT_EXITTRAIL)
endif()
# leavekeep (source/z_leavekeep.c says why): the lab leaves and filesRecover end without libctru's heap unmap (and,
# in exit trail builds only, --lab-crash can fire after NDSP starts: the CtrLab_crashTest wrap in the block above).
# svcControlMemory, not __libctru_exit, is wrapped: a wrap's __real_ reference
# from our own object pulls its libctru archive member earlier and moves libctru's text (svc.o is in the link already).
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_leavekeep.c" APPEND PROPERTY COMPILE_OPTIONS "-Os;-fno-jump-tables")
target_link_options(ctrport PRIVATE -Wl,--wrap=svcControlMemory)
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

# Knight GPU-1 (knightnew, round 21): the Knight's afterimage spill copies skip their unused native columns.
# build.sh turns it on for DELTARUNE New by default ("knightnew-history=off" leaves it out); the switch is accepted but
# empty in both UNDERTALE targets and DELTARUNE Old. Cache/preload experiments stay parked (drresid owns SD stalls).
option(CTRPORT_KNIGHTNEW_HISTORY "Trim Knight history spill padding on New DR" OFF)
if(CTRPORT_KNIGHTNEW_HISTORY AND CTRPORT_GAME STREQUAL "deltarune" AND CTRPORT_TARGET STREQUAL "n3ds")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c")
    foreach(part ctr_renderer z_knightnew_history)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/${part}.c" APPEND PROPERTY
            COMPILE_DEFINITIONS CTRPORT_KNIGHTNEW_HISTORY)
    endforeach()
    target_link_options(ctrport PRIVATE
        -Wl,--wrap=CtrRenderer_beginFrame,--wrap=CtrRenderer_endFrame,--wrap=GX_TextureCopy)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_knightnew_history.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-tree-switch-conversion;-fno-jump-tables")
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
endif()

# utold Continue transition: registration is cold and all new storage is late.
if(CTRPORT_GAME STREQUAL "undertale")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_utold_contblack.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables")
    target_link_options(ctrport PRIVATE -Wl,--wrap=Btnprompt_registerAll)
endif()
