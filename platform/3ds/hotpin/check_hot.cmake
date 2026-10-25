# Checks a link made with the hot-text alignment pad (hotpin/<game>-<target>.ld): every function in the .hot list must
# still start at its listed address. The pad's own ASSERTs stop the link when growth pushes a pinned function past its
# pin, but the engine, vendor and library code is pinned as one block, so a change inside it moves its hot functions
# without tripping them. This catches that, and a hot function that was renamed or removed.
#
# On a mismatch the ELF is deleted, so the next build links and checks again instead of passing on a stale ELF.
#
#   cmake -DELF=<*-3ds.elf> -DNM=<arm-none-eabi-nm> -DLIST=<hotpin/*.hot> -P check_hot.cmake

foreach(var ELF NM LIST)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "check_hot.cmake: -D${var}= is required")
    endif()
endforeach()

execute_process(COMMAND "${NM}" "${ELF}" OUTPUT_VARIABLE nm_out RESULT_VARIABLE nm_rc)
if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "check_hot.cmake: ${NM} ${ELF} failed (${nm_rc})")
endif()
string(REPLACE "\r" "" nm_out "\n${nm_out}")

file(STRINGS "${LIST}" entries REGEX "^[0-9a-f]+ ")
set(moved "")
set(count 0)
foreach(entry IN LISTS entries)
    string(REGEX MATCH "^([0-9a-f]+) (.+)$" _ "${entry}")
    set(addr "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    math(EXPR count "${count} + 1")
    set(found FALSE)
    foreach(type t T w W)
        string(FIND "${nm_out}" "\n${addr} ${type} ${name}\n" at)
        if(at GREATER_EQUAL 0)
            set(found TRUE)
            break()
        endif()
    endforeach()
    if(NOT found)
        string(REGEX MATCH "\n([0-9a-f]+) [tTwW] ${name}\n" now "${nm_out}")
        if(now)
            string(APPEND moved "\n  ${name}: 0x${addr} -> 0x${CMAKE_MATCH_1}")
        else()
            string(APPEND moved "\n  ${name}: 0x${addr} -> gone")
        endif()
    endif()
endforeach()

if(moved)
    file(REMOVE "${ELF}")
    message(FATAL_ERROR
        "hotpin: hot functions moved off their pinned addresses (${LIST}):${moved}\n"
        "Text ahead of the hot set changed, so the pad no longer holds. Regenerate the pad and its .hot list from this "
        "tree (docs/3ds-hardware.md, \"Old-model I-cache layout luck\"), or configure with -DCTRPORT_HOTPIN=OFF to "
        "build without it. ${ELF} was deleted.")
endif()
message(STATUS "hotpin: ${count} hot functions at their pinned addresses")
