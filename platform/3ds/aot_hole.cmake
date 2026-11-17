# Hole tenants (hole hook v2, change codeshrink-aot, 2026-11-16): engine/src/aot/hole/aot_hole_<name>.c. A tenant is
# ordinary late code (a unit's own file) that UNDERTALE Old's padded AOT block can hold in place of padding bytes.
# - Its options are its own, never the generated files': the target's flags plus what aot_hole_<name>.options next to
#   it names (lines "option <flag>" and "define <NAME[=value]>", "#" comments), the same lines the unit's
#   set_property(SOURCE ...) calls gave it as an ordinary late source. Each line is one literal argument: no "$" (so no
#   generator expressions or variable references) and no ";" anywhere in the file, an option starts with "-" and is not
#   -D/-U (definitions take "define"), and CTRPORT_AOT_HOLE is the hook's own name, set by the hook only. A sidecar
#   without its tenant fails, and adding, editing or removing a sidecar configures again.
# - In the hole (UNDERTALE Old's static-table AOT build with its pad file) it links right before aot_pad.c and compiles
#   with CTRPORT_AOT_HOLE defined. In every other build (UNDERTALE New, builds without AOT, chapter modules, DELTARUNE)
#   it is an ordinary source in its usual place, without CTRPORT_AOT_HOLE.
# - The fallback a tenant provides: its ordinary-late placement under #ifndef CTRPORT_AOT_HOLE (the late section names
#   it had, .text.late.* and its constants' sections), plain sections under #ifdef CTRPORT_AOT_HOLE (stock 3dsx.ld
#   places .text.* in link order, so the place is the position in the source list), and the #if guards for the games
#   and targets it is built for. CTRPORT_AOT_HOLE changes section names only, so a tenant's bytes in the hole equal its
#   bytes as ordinary late code apart from relocations. Initialised data has no padding in the block and fails the
#   layout gate; zero statics and constants join the block's .bss and .rodata runs.
# tools/aot/test_aot_hole.py drives these functions directly (cmake -P and a small host project).

# ctrport_aot_hole_options(<tenant.c> <options var> <defines var>): the tenant's sidecar lines as two lists.
function(ctrport_aot_hole_options src out_options out_defines)
    string(REGEX REPLACE "\\.c$" ".options" opts "${src}")
    set(options "")
    set(defines "")
    if(EXISTS "${opts}")
        file(READ "${opts}" text)
        # Refused before splitting: ";" would split a line in two, and "$" would reach CMake's generator expressions.
        if(text MATCHES "[;$]")
            message(FATAL_ERROR "Hole tenant options ${opts}: ';' and '$' are not allowed (one literal argument per line)")
        endif()
        string(REPLACE "\r" "" text "${text}")
        string(REPLACE "\n" ";" lines "${text}")
        foreach(line IN LISTS lines)
            if(line MATCHES "^[ \t]*(#.*)?$")
                continue()
            elseif(line MATCHES "CTRPORT_AOT_HOLE")
                message(FATAL_ERROR "Hole tenant options ${opts}: the line '${line}' names CTRPORT_AOT_HOLE, which only the hook sets")
            elseif(line MATCHES "^option (-[^DU \t][^ \t]*)$")
                list(APPEND options "${CMAKE_MATCH_1}")
            elseif(line MATCHES "^define ([A-Za-z_][A-Za-z0-9_]*(=[^ \t]*)?)$")
                list(APPEND defines "${CMAKE_MATCH_1}")
            else()
                message(FATAL_ERROR "Hole tenant options ${opts}: the line '${line}' is not 'option <flag>' (a flag other than -D/-U) or 'define <NAME[=value]>'")
            endif()
        endforeach()
    endif()
    set(${out_options} "${options}" PARENT_SCOPE)
    set(${out_defines} "${defines}" PARENT_SCOPE)
endfunction()

# ctrport_aot_hole_tenants(<hole dir> <sources var>): the tenants, each with its own options set on the source.
function(ctrport_aot_hole_tenants dir out)
    file(GLOB srcs CONFIGURE_DEPENDS "${dir}/aot_hole_*.c")
    # Sidecars are globbed too, so one created or removed after configure configures again; edits are watched below.
    file(GLOB sidecars CONFIGURE_DEPENDS "${dir}/aot_hole_*.options")
    foreach(opts IN LISTS sidecars)
        string(REGEX REPLACE "\\.options$" ".c" tenant "${opts}")
        if(NOT EXISTS "${tenant}")
            message(FATAL_ERROR "Hole tenant options ${opts}: no tenant ${tenant} next to it")
        endif()
    endforeach()
    foreach(src IN LISTS srcs)
        if(NOT src MATCHES "/aot_hole_[a-z0-9_]+\\.c$")
            message(FATAL_ERROR "Hole tenant ${src}: the name must be aot_hole_<lower-case name>.c")
        endif()
        string(REGEX REPLACE "\\.c$" ".options" opts "${src}")
        if(EXISTS "${opts}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${opts}")
        endif()
        ctrport_aot_hole_options("${src}" options defines)
        foreach(o IN LISTS options)
            set_property(SOURCE "${src}" APPEND PROPERTY COMPILE_OPTIONS "${o}")
        endforeach()
        foreach(d IN LISTS defines)
            set_property(SOURCE "${src}" APPEND PROPERTY COMPILE_DEFINITIONS "${d}")
        endforeach()
    endforeach()
    set(${out} "${srcs}" PARENT_SCOPE)
endfunction()

# ctrport_aot_hole_placed(<var> <game> <target> <pad file>): ON when a static-table AOT build places tenants in the hole:
# UNDERTALE Old with its pad file only, whatever other pad files exist.
function(ctrport_aot_hole_placed out game target pad_file)
    set(placed OFF)
    if("${game}" STREQUAL "undertale" AND "${target}" STREQUAL "o3ds" AND EXISTS "${pad_file}")
        set(placed ON)
    endif()
    set(${out} "${placed}" PARENT_SCOPE)
endfunction()
