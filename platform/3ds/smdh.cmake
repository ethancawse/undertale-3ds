# Packaging metadata only: never include this file in a compiler command.
# HOME Menu text (maintainer, 2026-11-04: "trim the name down"): the long description is the game's name; the publisher
# line says "3lit3hax - Unofficial port" for UNDERTALE releases, or "Unofficial port - v<version> - <model>" for
# DELTARUNE releases, only when release packaging asks for it (CTRPORT_SMDH_RELEASE=1, set by a kit's builder, in a
# tree with no .git and no source identity from the builder), else "Unofficial port - <label> <commit> - <model>",
# which says which source it was built from. An unofficial source build never shows the release version (R204):
# - a Git checkout: git's own commit and status. Lab builds may set CTRPORT_SMDH_BUILD_TAG=stage73 (or cand27); an
#   exact candidate/stage tag is used automatically; other checkouts say "source build". A checkout git can't read
#   (no git, a .git git refuses, or a .git link whose target is gone) says "source build" with no commit.
# - the builder run from a checkout: its mirror has no .git, so the builder passes the commit it read (and
#   "-dirty") as CTRPORT_SMDH_SOURCE. In a Git checkout git wins over that variable.
# - an archive built by hand: nothing says which source it is, and it may be changed, so it says "source build" with
#   no commit. A bug report's build can then be told from a release (c26rev3 F3).
# Homebrew Launcher text (the 3dsx only): the description is "<build> (o3ds/o2ds)" or "<build> (n3ds/n2ds)".
# Always query Git at build time, so an incremental build follows HEAD too. Git is read without optional locks
# (GIT_OPTIONAL_LOCKS=0: git status never writes .git/index, so a killed status leaves no index.lock), each call gets
# GIT_TIMEOUT seconds (120; a hang guard for a loaded host), and git's own error is printed. A checkout git cannot read
# says "source build" as above; a git call that timed out, or a status or tag query that failed in a checkout git did
# read, fails the build: the identity is unknown, and a guess ("clean", or no label) would mislabel a bug report.
get_filename_component(ROOT "${ROOT}" ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/release_contract.cmake")
release_read_contract("${ROOT}")
if(NOT DEFINED GIT_TIMEOUT)
    set(GIT_TIMEOUT 120)
endif()
# Stops the build when a git call timed out (its result is CMake's text, not git's status).
function(smdh_git_timed_out what result error)
    if(result MATCHES "timeout")
        message(FATAL_ERROR "SMDH: git ${what} did not end in ${GIT_TIMEOUT} s (${result}); the source identity is "
            "unknown. ${error}")
    endif()
endfunction()
if(NOT GAME MATCHES "^(undertale|deltarune)$" OR NOT MODEL MATCHES "^(o3ds|n3ds)$")
    message(FATAL_ERROR "Unknown SMDH game or model")
endif()
string(TOUPPER "${GAME}" game_name)
set(model_name "Old 3DS")
if(MODEL STREQUAL "n3ds")
    set(model_name "New 3DS")
endif()
set(build "source build")
set(release_build FALSE)
# EXISTS is false for a .git junction or symlink whose target is gone; that is still a checkout marker.
set(checkout_marker FALSE)
if(EXISTS "${ROOT}/.git" OR IS_SYMLINK "${ROOT}/.git")
    set(checkout_marker TRUE)
endif()
if(NOT checkout_marker AND DEFINED ENV{CTRPORT_SMDH_SOURCE})
    set(source "$ENV{CTRPORT_SMDH_SOURCE}")
    if(NOT source MATCHES "^[0-9a-f]+(-dirty)?$")
        message(FATAL_ERROR "SMDH source must be a commit (7 to 12 lowercase hex, optional -dirty)")
    endif()
    string(REGEX REPLACE "-dirty$" "" source_commit "${source}")
    string(LENGTH "${source_commit}" length)
    # git's short form; 12 characters plus "-dirty" still fit the publisher line.
    if(length LESS 7 OR length GREATER 12)
        message(FATAL_ERROR "SMDH source must be a commit (7 to 12 lowercase hex, optional -dirty)")
    endif()
    set(build "source build ${source}")
elseif(NOT checkout_marker AND "$ENV{CTRPORT_SMDH_RELEASE}" STREQUAL "1")
    set(build "v${RELEASE_VERSION}")
    set(release_build TRUE)
endif()
if(GIT_EXECUTABLE AND EXISTS "${ROOT}/.git")
    # Only this source tree's directory/worktree marker permits a lab stamp.
    # Inherited Git redirection can report the current directory as the top
    # level while reading another repository. Keep it out of these queries.
    foreach(git_env GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE
            GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_NAMESPACE
            GIT_PREFIX GIT_CEILING_DIRECTORIES GIT_DISCOVERY_ACROSS_FILESYSTEM)
        unset(ENV{${git_env}})
    endforeach()
    set(ENV{GIT_OPTIONAL_LOCKS} 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel WORKING_DIRECTORY "${ROOT}"
        RESULT_VARIABLE git_result OUTPUT_VARIABLE git_root OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE git_error ERROR_STRIP_TRAILING_WHITESPACE TIMEOUT ${GIT_TIMEOUT})
    smdh_git_timed_out("rev-parse --show-toplevel" "${git_result}" "${git_error}")
    if(NOT git_result EQUAL 0)
        message(STATUS "SMDH: git cannot read this checkout (${git_result}): ${git_error}")
    endif()
    # Git reports the top level with links resolved; compare it with the same form of this tree's root.
    get_filename_component(git_root "${git_root}" REALPATH)
    get_filename_component(root_path "${ROOT}" REALPATH)
    # Archives inside another checkout must not inherit its identity or lab label.
    if(git_result EQUAL 0 AND git_root STREQUAL root_path)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD WORKING_DIRECTORY "${ROOT}"
            RESULT_VARIABLE git_result OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE git_error ERROR_STRIP_TRAILING_WHITESPACE TIMEOUT ${GIT_TIMEOUT})
        smdh_git_timed_out("rev-parse HEAD" "${git_result}" "${git_error}")
        if(NOT git_result EQUAL 0 OR NOT commit MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "Cannot identify the SMDH source commit (${git_result}): ${git_error}")
        endif()
        set(label "$ENV{CTRPORT_SMDH_BUILD_TAG}")
        if(label AND NOT label MATCHES "^(cand|stage)[0-9]+[a-zA-Z0-9._-]*$")
            message(FATAL_ERROR "SMDH build tag must be a candidate or stage label")
        endif()
        if(NOT label)
            execute_process(COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD --list "cand[0-9]*" "stage[0-9]*"
                WORKING_DIRECTORY "${ROOT}" RESULT_VARIABLE tag_result OUTPUT_VARIABLE tags
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE git_error ERROR_STRIP_TRAILING_WHITESPACE
                TIMEOUT ${GIT_TIMEOUT})
            smdh_git_timed_out("tag --points-at" "${tag_result}" "${git_error}")
            if(NOT tag_result EQUAL 0)
                message(FATAL_ERROR "Cannot read the SMDH source tags (${tag_result}): ${git_error}")
            endif()
            string(REPLACE "\n" ";" tags "${tags}")
            list(SORT tags COMPARE NATURAL ORDER DESCENDING)
            foreach(tag IN LISTS tags)
                if(tag MATCHES "^(cand|stage)[0-9]+[a-zA-Z0-9._-]*$")
                    set(label "${tag}")
                    break()
                endif()
            endforeach()
        endif()
        if(NOT label)
            set(label "source build")
        endif()
        # Lock-free (also GIT_OPTIONAL_LOCKS above): status may not refresh .git/index, so killing it leaves no lock.
        execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${ROOT}" RESULT_VARIABLE status_result OUTPUT_VARIABLE changes
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE git_error ERROR_STRIP_TRAILING_WHITESPACE
            TIMEOUT ${GIT_TIMEOUT})
        smdh_git_timed_out("status" "${status_result}" "${git_error}")
        if(NOT status_result EQUAL 0)
            message(FATAL_ERROR "Cannot check the SMDH source status (${status_result}): ${git_error}")
        endif()
        if(changes)
            string(APPEND commit "-dirty")
        endif()
        set(build "${label} ${commit}")
    endif()
endif()
set(description "${game_name}")
# The Homebrew Launcher's rows (maintainer, 2026-11-06: "v1.0.0-beta.1 (o3ds/o2ds)", "dont add word soup"): the 3dsx's
# description is "<build> <consoles>", so players see the version they have installed and which row is theirs.
# <build> is the publisher's, so R204 holds here too: a source build shows its commit, never the version.
# The CIA's stays the game's name (HOME Menu, above).
if(MODE STREQUAL "3dsx")
    set(description "${build} (o3ds/o2ds)")
    if(MODEL STREQUAL "n3ds")
        set(description "${build} (n3ds/n2ds)")
    endif()
endif()
set(publisher "Unofficial port - ${build} - ${model_name}")
set(UNDERTALE_RELEASE_PUBLISHER "3lit3hax - Unofficial port")
if(GAME STREQUAL "undertale" AND release_build)
    set(publisher "${UNDERTALE_RELEASE_PUBLISHER}")
endif()
# All components are ASCII. SMDH has 128 UTF-16 code units for the description and 64 for the publisher, NULs included.
string(LENGTH "${description}" length)
if(length GREATER 127)
    message(FATAL_ERROR "SMDH description exceeds 127 characters")
endif()
string(LENGTH "${publisher}" length)
if(length GREATER 63)
    message(FATAL_ERROR "SMDH publisher exceeds 63 characters: ${publisher}")
endif()
if(DESCRIPTION_OUT)
    file(WRITE "${DESCRIPTION_OUT}" "${description}")
    if(PUBLISHER_OUT)
        file(WRITE "${PUBLISHER_OUT}" "${publisher}")
    endif()
    return()
endif()
if(NOT MODE MATCHES "^(3dsx|cia)$")
    message(FATAL_ERROR "Unknown SMDH packaging mode")
endif()
file(SHA256 "${ICON}" icon_hash)
file(SHA256 "${TOOL}" tool_hash)
string(SHA256 fingerprint "${MODE}|${NAME}|${description}|${publisher}|${icon_hash}|${tool_hash}|${FLAGS}")
set(previous "")
if(EXISTS "${OUT}" AND EXISTS "${OUT}.stamp")
    file(SHA256 "${OUT}" output_hash)
    file(READ "${OUT}.stamp" previous)
    if(previous STREQUAL "${fingerprint}\n${output_hash}\n")
        return()
    endif()
endif()
if(MODE STREQUAL "3dsx")
    set(args --create "${NAME}" "${description}" "${publisher}" "${ICON}" "${OUT}.part")
else()
    set(args makesmdh -s "${NAME}" -l "${description}" -p "${publisher}" -i "${ICON}" -f "${FLAGS}" -o "${OUT}.part")
endif()
execute_process(COMMAND "${TOOL}" ${args} RESULT_VARIABLE result TIMEOUT 60)
if(NOT result EQUAL 0)
    file(REMOVE "${OUT}.part")
    message(FATAL_ERROR "SMDH packaging failed: ${result}")
endif()
file(RENAME "${OUT}.part" "${OUT}")
file(SHA256 "${OUT}" output_hash)
file(WRITE "${OUT}.stamp" "${fingerprint}\n${output_hash}\n")
