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
# bsflash (battlesplit2, 11-14: the Split's cover draws on the top screen's margins; z_battlesplit.c's and
# ctr_renderer.c's ends) is the next last text input, after btgrey; it has no constants (its rodata input, paired as
# every tenant's is, stays empty).
# bsnap (battlesplit2, 11-14: Napstablook's way down in Mad Dummy's Split; z_battlesplit.c's and ctr_renderer.c's
# ends) is the next, after bsflash; it has no constants (its rodata input stays empty). Both empty, N8a's two
# readonly-only inputs after them do not move.
# btswitch (lead 11-15, change battletouch #51: z_battletouch.c's tap switch, ctr_renderer.c's bottom hold,
# ctr_gpu_queue.c's frame-end setter) is the next new last text input, after bsnap. Its constants are the
# next new last rodata input, after N8a's two (appended there, so N8a's Old-release constants keep their offsets).
# btstory (lead 11-15, change battletouch u3 r5: the story screens' taps, z_battletouch.c) appends after every
# tenant of its base, the reclaim block included (a unit never grows a tenant before the block). Text only:
# its constants sit in its text input (BT_TEXTRO), as dev links N8a's readonly inputs in late text.
# listfree (lead 11-14, change stepdrift: z_listfree.c frees the empty per-object instance lists after a room change;
# c26rev8 GO 2a4bbb5d9, LF-3) appends after btstory. It has no constants: its empty rodata input appends after
# btswitch's, at the rodata tail, so nothing before it moves.
# soulahead (change audiomain 11-15, ctr_soulahead.inc: Omega Flowey's first SOUL songs opened ahead; c26rev7 GO) is
# the next tail append, after listfree; its rodata input (empty) after listfree's.
# msglock (audiomain, 11-14 21:2x: ctr_audio_system.c's frame drain and game-thread posts that only try msgLock;
# c26rev6 GO) follows soulahead; its rodata input (empty: its three strings are assembled into the text input) too.
# btomega (lead 11-15, change battletouch Omega r3: Omega Flowey with the stylus; z_battletouch.c's and z_battlesplit.c's
# ends; c26rev5 GO) follows msglock. Text only: its constants sit in its text input (BT_ROOM).
# bsexend (battlesplit2, 11-15: Mettaton EX's ending in Split; z_bsmex.c's and z_battlesplit.c's ends), bsgap (the
# Split's flash on the bottom screen's empty rows; ctr_renderer.c's end) and bsg47 (Undyne's arena drawn higher in
# Split, the HUD in place; z_battlesplit.c's and ctr_renderer.c's ends), c26rev8 GO, follow btomega in that order.
# None has constants: their rodata inputs stay empty, after msglock's.
# storyentry (change storyentry, 11-15; UNDERTALE Old, empty elsewhere: a New Home story battle's encounter scene and
# its flight and room load, z_storyentry.c and z_btlentry.c's end) is the next new last text input, appended after
# N12's last (bsg47; at compose, after whatever is last then: integrate sets the order). Its constants are
# in its text input; its rodata input, after bsg47's, stays empty.
# soulfly (change soulfly, 11-15: the Split battle entry's SOUL flight across the seam; z_battlesplit.c's end, called by
# layoutup's chain18; c26rev6 GO at N11) follows storyentry (N13 compose order). Text only, no constants: no rodata input (as asgfuse).
# samestart (audiomain #109, 11-15: two same-step plays of one sound start on one DSP frame; ctr_samestart.inc)
# follows soulfly (N13 compose order; originally after bsg47) on UNDERTALE Old only, placed per model (ctr_place.h, late-order.json per_model); New links it as late
# text. Text only: no rodata input (its constants are in its literal pools).
# soulahead2 (audiomain soulahead r5, 11-15: the warning-only test of soulahead r4 as an append, so soulahead's
# own input keeps its size) follows samestart; text only, on both models as soulahead's.
# bshinge (battlesplit2, 11-15: the hinge rows table and settings.ini's hinge_rows; z_battlesplit.c's end)
# follows soulahead2 (N13 compose order; originally after bsg47); no constants: its rodata input stays empty, after
# bsg47's.
# btchoice (change battletouch btchoice, maintainer 11-15: the two-button choice screens with the stylus, drag then tap;
# z_battletouch.c's end) follows bshinge (N13 compose order; originally after bsg47), text and rodata alike. Per model (ctr_place.h, as asgfuse): its text input
# links on UNDERTALE Old only (New: late text, .text_btchoice); its rodata input (24 B of names) on both.
# bsmexr (battlesplit2, 11-15: Mettaton EX's slide back up to the top screen after his heart turns, his
# ratings on the top screen in them; z_bsmex.c's, z_battlesplit.c's and ctr_renderer.c's ends) follows btchoice (N14 compose order; originally after bshinge).
# Per model (ctr_place.h, as asgfuse): its text input links on UNDERTALE Old only (New: late text, .text_mexslide);
# no constants, so no rodata input.
# framelead (stepdrift, 11-15: ctr_frame_pacer.c's end, a frame's work starts frame_lead_ms before its slot's
# vblank) follows opt3ds (N14 compose order; originally after bsg47), on UNDERTALE Old only: placed per model (ctr_place.h, late-order.json per_model), and New
# builds none of it, so its input is empty there. Text only: its constants sit in its text input (UT_COLD_TEXT_CONST),
# as N8a's release-only rodata inputs must stay the rodata slot's last (check_devonly_aac.py).
# bstall (N14 rebase): append after opt3ds; constants before the release-only reclaim suffix.
# S4 occupies part of the retired Frame hole. Its padding stays fixed, so
# battlesplit14 and every existing tenant retain their dv8q addresses.
# opt3ds (options3ds, N14): page code is an appended AAC tenant on Old and late text on New;
# constants append to AAC rodata on both, through ctr_place.h. No earlier tenant moves.
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
    .aacout.text.bsflash
    .aacout.text.bsnap
    .aacout.text.asgfuse
    .aacout.text.btswitch
    .aacout.text.latereclaim_dlg
    .aacout.text.latereclaim_asg
    .aacout.text.latereclaim_arena
    .aacout.text.latereclaim_choice
    .aacout.text.latereclaim_notice
    .aacout.text.latereclaim_gofps
    .aacout.text.btstory
    .aacout.text.listfree
    .aacout.text.soulahead
    .aacout.text.msglock
    .aacout.text.btomega
    .aacout.text.bsexend
    .aacout.text.bsgap
    .aacout.text.bsg47
    .aacout.text.storyentry
    .aacout.text.soulfly
    .aacout.text.samestart
    .aacout.text.soulahead2
    .aacout.text.bshinge
    .aacout.text.btchoice
    .aacout.text.mexslide
    .aacout.text.undlg_speech
    .aacout.text.opt3ds
    .aacout.text.framelead
    .aacout.text.bstall
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
    .aacout.rodata.bsflash
    .aacout.rodata.bsnap
    .aacout.rodata.btswitch
    .aacout.rodata.listfree
    .aacout.rodata.soulahead
    .aacout.rodata.msglock
    .aacout.rodata.bsexend
    .aacout.rodata.bsgap
    .aacout.rodata.bsg47
    .aacout.rodata.storyentry
    .aacout.rodata.bshinge
    .aacout.rodata.btchoice
    .aacout.rodata.undlg_speech
    .aacout.rodata.opt3ds
    .aacout.rodata.bstall
)
# Release-only reclaim inputs (review decision 11-16, N13 option A): N8a's two readonly inputs, linked into AAC rodata by
# UNDERTALE Old's shipping release only (CTRPORT_N8A_RODATA below; dev and New keep them in late text/rodata). They
# are emitted after every production rodata input, so a later production tenant keeps its release address in dev
# (check_devonly_aac: release-only reclaim inputs must be release's last). New tenants append to
# aacout_rodata_inputs above, never here.
set(aacout_rodata_reclaim_inputs
    .aacout.rodata.n8a_optmenu
    .aacout.rodata.n8a_utfadea
)
# Intact final late suffix, UT Old shipping release only. N11's asgfuse precedes these inputs.
# A later bschoice input (G100, 76 B) is consumed below without changing its source or other targets.
set(aacout_latereclaim FALSE)
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds" AND CTRPORT_RELEASE_SOURCE_URL)
    set(aacout_latereclaim TRUE)
    if(NOT DEFINED CTRPORT_LATERECLAIM_CHOICE_BYTES OR
            NOT CTRPORT_LATERECLAIM_CHOICE_BYTES MATCHES "^(0|76)$")
        message(FATAL_ERROR "latereclaim: choice routing must run before AAC emission")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_LATERECLAIM_DLG_SECTION=".aacout.text.latereclaim_dlg")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_battlesplit.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_LATERECLAIM_ASG_SECTION=".aacout.text.latereclaim_asg"
        CTRPORT_LATERECLAIM_ARENA_SECTION=".aacout.text.latereclaim_arena")
endif()
# N8a: only UT Old release moves these two immutable inputs; all other targets retain their late inputs.
if(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds" AND CTRPORT_RELEASE_SOURCE_URL)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_optmenu.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_btlentry.c" APPEND PROPERTY
        COMPILE_DEFINITIONS CTRPORT_N8A_RODATA)
endif()
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
# UNDERTALE Old's protected AAC text reserve (lead 11-14, audit F3-1 T7; lead 11-15 N13 ruling L2): the plain dev-only
# tail's 8,192 B cap plus plain Old dev's free floor, 2,048 B since 11-16 (lead, for framelead; 4,096 B from 11-15,
# 8,192 B before: nothing occupied it; it stays as dev-probe and hotfix headroom). tools/analysis/aac_capacity.py is the tools' copy (test_aac_capacity.py).
set(aacout_old_plain_free 2048)
set(aacout_old_reserve 10240)
# The touchtest vehicle (CTRPORT_TOUCH_TEST, with or without touchqueue and testpage; 11-16, N14 n14g): a test
# build, never a release (CMakeLists.txt refuses it with a release source URL), whose touch-test code adds ~824 B to
# Old's production lists. Like the ballast, labcrash and AAC test tenant vehicles it is held to the slot's capacity
# only: no protected reserve and no plain dev floor, which protect shipped-build headroom. Instead it gets its own named
# capacity ASSERT, so it fails loudly when the slot is full rather than through the generic overflow.
set(aacout_touchtest_vehicle FALSE)
if(CTRPORT_TOUCH_TEST)
    set(aacout_touchtest_vehicle TRUE)
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
        # UNDERTALE New: 43,396 B until N10, then 16 KiB more (59,780 B; review decision 11-15, N10 rider R9): flash + Nap D
        # left New dev 2,904 B over the old slot, and ~8-9 KB of New tenants are queued. The 16 KiB comes off New's late
        # reserve (CMakeLists.txt, ctrport_ut_late), so text after the slot moves by four whole pages and nothing after
        # text moves. tools/analysis/aac_capacity.py is the tools' copy of these numbers (test_aac_capacity.py).
        set(aacout_capacity 59780)
        if(CTRPORT_TARGET STREQUAL "o3ds")
            # Old 1.0 envelope (relready-resize.md, integrate21 dv8q): 36 KiB more, nine pages.
            set(aacout_capacity 80260)
        endif()
        set(aacout_match "*(.text.*)")
        set(aacout_flags "SHF_ALLOC & !SHF_WRITE")
    else()
        # 23,530 B vendor payload, plus its original 2 B internal alignment.
        set(aacout_capacity 23532)
        if(CTRPORT_TARGET STREQUAL "o3ds")
            # oldspace [R] prototype: the four pages go to Old ordinary late text.
            # Keep this independent of the release URL; Old dev gets the same exchange.
            set(aacout_capacity 7148)
        endif()
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
    # Release-only reclaim inputs (rodata: N8a's pair) follow every production input; dev's production end precedes them.
    set(aacout_order ${aacout_${kind}_inputs} ${aacout_${kind}_reclaim_inputs} ${aacout_ballast} .aacout.${kind}.aacslot_test)
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
        # One minimal 4 KiB phase pad for the entire suffix; no padding between its inputs.
        # 0x928 is w10's source phase, independently checked against the actual pre-move map.
        if(aacout_latereclaim AND input STREQUAL ".aacout.text.latereclaim_dlg")
            string(APPEND aacout_inputs "        __latereclaim_text_gap_start = ABSOLUTE(.);\n        . += ((2344 - __latereclaim_text_gap_start) & 4095);\n        __latereclaim_text_gap_end = ABSOLUTE(.);\n        __latereclaim_start = ABSOLUTE(.);\n")
        endif()
        if(aacout_latereclaim AND input STREQUAL ".aacout.text.latereclaim_notice")
            string(APPEND aacout_inputs "        . = ALIGN(8);\n")
        endif()
        if(aacout_latereclaim AND input MATCHES "^\\.aacout\\.text\\.latereclaim_(dlg|asg|arena|choice|notice|gofps)$")
            set(part "${CMAKE_MATCH_1}")
            string(APPEND aacout_inputs "        __latereclaim_${part}_start = ABSOLUTE(.);\n")
        endif()
        string(APPEND aacout_inputs "        KEEP(INPUT_SECTION_FLAGS (${aacout_flags}) *(${input}))\n")
        if(aacout_latereclaim AND input STREQUAL ".aacout.text.latereclaim_choice")
            # G100 may compose first: preserve the optional last input intact, with its original map name.
            string(APPEND aacout_inputs "        KEEP(INPUT_SECTION_FLAGS (${aacout_flags}) *(.text_bschoice))\n")
        endif()
        if(aacout_latereclaim AND input STREQUAL ".aacout.text.latereclaim_notice")
            string(APPEND aacout_inputs "        KEEP(INPUT_SECTION_FLAGS (${aacout_flags}) *(.text_noticegate_late))\n")
        endif()
        if(aacout_latereclaim AND input STREQUAL ".aacout.text.latereclaim_gofps")
            string(APPEND aacout_inputs "        KEEP(INPUT_SECTION_FLAGS (${aacout_flags}) *(.text_gofps_late))\n")
        endif()
        if(aacout_latereclaim AND input MATCHES "^\\.aacout\\.text\\.latereclaim_(dlg|asg|arena|choice|notice|gofps)$")
            string(APPEND aacout_inputs "        __latereclaim_${part}_end = ABSOLUTE(.);\n")
            if(part STREQUAL "gofps")
                string(APPEND aacout_inputs "        __latereclaim_end = ABSOLUTE(.);\n")
                foreach(pair "dlg;0;824" "asg;824;268" "arena;1092;192")
                    list(GET pair 0 name)
                    list(GET pair 1 offset)
                    list(GET pair 2 size)
                    string(APPEND aacout_inputs "        ASSERT(__latereclaim_${name}_start - __latereclaim_start == ${offset}, \"latereclaim ${name} offset\");\n        ASSERT(__latereclaim_${name}_end - __latereclaim_${name}_start == ${size}, \"latereclaim ${name} size\");\n")
                endforeach()
                # ld folds constant sums in maps; emit the same single constants the proof expects.
                math(EXPR latereclaim_notice_offset "((2344 + 1284 + ${CTRPORT_LATERECLAIM_CHOICE_BYTES} + 7) & ~7) - 2344")
                math(EXPR latereclaim_gofps_offset "${latereclaim_notice_offset} + 144")
                string(APPEND aacout_inputs "        ASSERT(__latereclaim_notice_start - __latereclaim_start == ${latereclaim_notice_offset}, \"latereclaim notice offset\");\n        ASSERT(__latereclaim_notice_end - __latereclaim_notice_start == 144, \"latereclaim notice size\");\n        ASSERT(__latereclaim_gofps_start - __latereclaim_start == ${latereclaim_gofps_offset}, \"latereclaim gofps offset\");\n        ASSERT(__latereclaim_gofps_end - __latereclaim_gofps_start == 192, \"latereclaim gofps size\");\n")
                string(APPEND aacout_inputs "        ASSERT(__latereclaim_choice_start - __latereclaim_start == 1284, \"latereclaim choice offset\");\n        ASSERT(__latereclaim_choice_end - __latereclaim_choice_start == ${CTRPORT_LATERECLAIM_CHOICE_BYTES}, \"latereclaim choice size\");\n        ASSERT((__latereclaim_start & 4095) == 2344, \"latereclaim source phase\");\n        ASSERT(__aacout_text_used_end - (__latereclaim_end - __latereclaim_text_gap_start) <= __aacout_text_start + ${aacout_capacity} - ${aacout_old_reserve}, \"latereclaim enters protected AAC text reserve\");\n")
            endif()
        endif()
    endforeach()
    if(CTRPORT_DEVONLY_AAC)
        # The dev-only tail and its budgets (c26rev2's contracts, 11-11): the plain dev tail, alignment included,
        # at most 8,192 B of text; build.sh's labcrash (LG_CRASHTEST's ~0.9 KB more) at most 9,216 B, its own budget.
        # Old plain dev keeps at least 2,048 B of the text slot free, and Old's production lists leave the 10,240 B
        # protected reserve. Test vehicles (ballast, labcrash, the AAC test tenant, touchtest) are held to the slot's capacity only.
        string(APPEND aacout_inputs "        __aacout_${kind}_devonly_start = ABSOLUTE(.);\n")
        # u0-tailalign (lead 11-15): the tail's first input starts at the largest tail-input alignment of its kind
        # today, so a production growth that is not a multiple of it moves every tail input by one delta
        # (check_devonly_aac.py's check_tail_shift), not by two when an aligned input pads inside the tail. Text 8:
        # z_labguard.o's 8-aligned code (the pen and bsrdev code is 4). Rodata 4: z_labguard.o's .rodata (the pen
        # and bsrdev constants are 1). The tail start stays the production end (rule 1); the gap is zero fill.
        if(kind STREQUAL "text")
            string(APPEND aacout_inputs "        . = ALIGN(8);\n")
        else()
            string(APPEND aacout_inputs "        . = ALIGN(4);\n")
        endif()
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
            if(CTRPORT_TARGET STREQUAL "o3ds" AND aacout_touchtest_vehicle)
                string(APPEND aacout_inputs "        ASSERT(__aacout_text_used_end <= __aacout_text_start + ${aacout_capacity}, \"AAC text: Old touchtest vehicle over the slot capacity\");\n")
            elseif(CTRPORT_TARGET STREQUAL "o3ds")
                string(APPEND aacout_inputs "        ASSERT(__aacout_text_start + ${aacout_capacity} - __aacout_text_production_end >= ${aacout_old_reserve}, \"AAC text: Old production lists inside the 10,240 B protected reserve\");\n")
                if(NOT CTRPORT_LABCRASH AND NOT CTRPORT_BALLAST AND NOT CTRPORT_AACSLOT_TEST)
                    string(APPEND aacout_inputs "        ASSERT(__aacout_text_start + ${aacout_capacity} - __aacout_text_devonly_end >= ${aacout_old_plain_free}, \"AAC text: Old plain dev under 2,048 B free\");\n")
                endif()
            endif()
        endif()
    endif()
    # Old release keeps the same 10,240 B after its production lists that the dev link asserts above (lead 11-14,
    # audit F3-1 T7). That room is dev's: the 8,192 B dev-only tail plus plain dev's 2,048 B free. A release-only
    # compose fails here, not first at the next dev link. It is measured at dev's production end (lead 11-15, N13
    # ruling L1): the latereclaim block and its phase pad (__latereclaim_text_gap_start to __latereclaim_end, release
    # only; dev links those inputs in late text and pays for them there) do not count against the dev reserve. Every
    # byte of the block is pinned by the latereclaim ASSERTs above (offsets and sizes), so nothing else can be
    # excluded. Test vehicles (ballast, the AAC test tenant, touchtest) are held to the slot's capacity only, as in dev.
    set(aacout_release_reserve "")
    if(kind STREQUAL "text" AND CTRPORT_TARGET STREQUAL "o3ds" AND NOT CTRPORT_DEVONLY_AAC AND NOT CTRPORT_BALLAST
       AND NOT CTRPORT_AACSLOT_TEST AND NOT aacout_touchtest_vehicle)
        set(aacout_release_used "__aacout_text_used_end")
        if(aacout_latereclaim)
            set(aacout_release_used "(__aacout_text_used_end - (__latereclaim_end - __latereclaim_text_gap_start))")
        endif()
        set(aacout_release_reserve "        ASSERT(__aacout_text_start + ${aacout_capacity} - ${aacout_release_used} >= ${aacout_old_reserve}, \"AAC text: Old release lists inside the 10,240 B dev reserve\");\n")
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
${aacout_release_reserve}        . += __aacout_${kind}_used_end <= __aacout_${kind}_start + ${aacout_capacity} ? __aacout_${kind}_start + ${aacout_capacity} - __aacout_${kind}_used_end : 0;
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
