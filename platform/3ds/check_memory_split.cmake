# Checks after the link that the heap / linear memory split (source/ctr_memory.c, __system_allocateHeaps) is not near
# one of its cliffs for this ELF. The split is worked out from what remains of the system's grant once the program's
# load image is committed. Some of its rules switch at a threshold (LARGE_MEMORY, the AOT module region), and a few KB
# of code growth can move a build across one: in integrate16 (2026-10-25) UNDERTALE New's image passed 12 MB, left
# 112 MB minus 32 KB of the 124 MB grant, under the then 112 MB LARGE_MEMORY, and its linear memory went from 56 to
# 32 MB (half the tile budget) with no warning.
#
# For each grant the target can run with, this computes the split at the remaining memory and at 4 MB less and more.
# Near no cliff, moving the remaining memory by 4 MB moves the heap and linear memory by at most 4 MB each. A larger
# change means a cliff within 4 MB of image growth (or of a grant a little different from the one assumed); the build
# stops and the ELF is deleted.
#
# Inputs: ELF, READELF, SOURCE (ctr_memory.c), CONSOLE (n3ds or o3ds), WAD17 (1 for DELTARUNE), AOT_REGION_MB (0
# without AOT modules).
execute_process(COMMAND "${READELF}" -lW "${ELF}" OUTPUT_VARIABLE segments RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "memory split: could not read ${ELF}")
endif()
string(REGEX MATCHALL "LOAD +0x[0-9a-f]+ +0x[0-9a-f]+ +0x[0-9a-f]+ +0x[0-9a-f]+ +0x[0-9a-f]+" loads "${segments}")
if(NOT loads)
    message(FATAL_ERROR "memory split: no LOAD segments in ${ELF}")
endif()
set(image_kb 0)
foreach(load IN LISTS loads)
    string(REGEX MATCH "(0x[0-9a-f]+)$" memsz "${load}")
    math(EXPR image_kb "${image_kb} + ((${memsz} + 4095) / 4096) * 4")
endforeach()

file(READ "${SOURCE}" source)
function(source_mb name regex)
    string(REGEX MATCH "${regex}" match "${source}")
    if(NOT match)
        file(REMOVE "${ELF}")
        message(FATAL_ERROR "memory split: ${name} not found in ${SOURCE}; update check_memory_split.cmake with it")
    endif()
    math(EXPR value "${CMAKE_MATCH_1} * 1024")
    set(${name} ${value} PARENT_SCOPE)
endfunction()
source_mb(large_kb [=[#define LARGE_MEMORY \(([0-9]+)u \* MB\)]=])
set(fixed_kb 0)
set(target_kb 0)
if(WAD17 AND CONSOLE STREQUAL "n3ds")
    source_mb(fixed_kb [=[#define FIXED_HEAP_SIZE \(([0-9]+)u \* MB\)]=])
elseif(WAD17)
    source_mb(target_kb [=[#define O3DS_HEAP_TARGET_MB ([0-9]+)u]=])
endif()
math(EXPR region_kb "${AOT_REGION_MB} * 1024")
# libctru's default rule's heap floor and linear cap (DEFAULT_HEAP_MIN_MB / DEFAULT_LINEAR_MAX_MB): 24 / 32 MB, and for
# UNDERTALE on the Old 3DS the first definitions in the source (20 / 38 MB, utflowey's 1a and option 1).
set(default_heap_kb 24576)
set(default_linear_kb 32768)
if(NOT WAD17 AND CONSOLE STREQUAL "o3ds")
    source_mb(default_heap_kb [=[#define DEFAULT_HEAP_MIN_MB ([0-9]+)u]=])
    source_mb(default_linear_kb [=[#define DEFAULT_LINEAR_MAX_MB ([0-9]+)u]=])
endif()

# The split in KB, as __system_allocateHeaps makes it (4 KB pages).
function(memory_split remaining grant out_heap out_linear)
    math(EXPR region_floor "${region_kb} + 49152")
    if(region_kb GREATER 0 AND remaining GREATER region_floor)
        math(EXPR remaining "${remaining} - ${region_kb}")
    endif()
    math(EXPR linear "(${remaining} / 8) * 4")
    math(EXPR rest "${remaining} - ${linear}")
    if(large_kb GREATER remaining AND rest GREATER default_heap_kb)
        math(EXPR linear "${remaining} - ${default_heap_kb}")
        if(linear GREATER default_linear_kb)
            set(linear ${default_linear_kb})
        endif()
    endif()
    math(EXPR fixed_floor "${fixed_kb} + 32768")
    if(fixed_kb GREATER 0 AND remaining GREATER fixed_floor)
        math(EXPR linear "${remaining} - ${fixed_kb}")
    endif()
    if(target_kb GREATER 0 AND remaining GREATER target_kb)
        math(EXPR above "${remaining} - ${target_kb}")
        if(above GREATER linear)
            set(linear ${above})
        endif()
    endif()
    # The CIA80 entry alone shifts 8 MiB, selected by the grant before image/AOT.
    # Mirror ctr_allocate_heaps.inc, including its minimum heap condition.
    math(EXPR before_shift "${remaining} - ${linear}")
    if(CIA80_TILES AND WAD17 AND CONSOLE STREQUAL "o3ds"
       AND grant GREATER_EQUAL 77824 AND grant LESS_EQUAL 81920 AND before_shift GREATER_EQUAL 32768)
        math(EXPR linear "${linear} + 8192")
    endif()
    math(EXPR heap "${remaining} - ${linear}")
    set(${out_heap} ${heap} PARENT_SCOPE)
    set(${out_linear} ${linear} PARENT_SCOPE)
endfunction()

# Grants in KB, for the heap, linear memory, the AOT module region and the load image together: a New 3DS gives
# 124 MB; an Old 3DS .3dsx 65528 KB (64 MB less 8 KB: the consoles' boot lines, docs/3ds-hardware.md "An Old 3DS
# .3dsx gets 65,528 KB"), the .cia's 80 MB mode 78 MB.
if(CONSOLE STREQUAL "n3ds")
    set(grants 126976)
else()
    set(grants 65528 79872)
endif()
foreach(grant IN LISTS grants)
    math(EXPR remaining "${grant} - ${image_kb}")
    memory_split(${remaining} ${grant} heap linear)
    foreach(step -4096 4096)
        math(EXPR moved "${remaining} + ${step}")
        memory_split(${moved} ${grant} heap2 linear2)
        math(EXPR dheap "${heap2} - ${heap}")
        math(EXPR dlinear "${linear2} - ${linear}")
        if(dheap LESS -4100 OR dheap GREATER 4100 OR dlinear LESS -4100 OR dlinear GREATER 4100)
            file(REMOVE "${ELF}")
            message(FATAL_ERROR "memory split: ${CONSOLE} with ${grant} KB granted and a ${image_kb} KB load image leaves "
                    "${remaining} KB: heap ${heap} KB, linear ${linear} KB. With ${step} KB more remaining it would be "
                    "heap ${heap2} KB, linear ${linear2} KB, a cliff within 4 MB (source/ctr_memory.c LARGE_MEMORY, "
                    "FIXED_HEAP_SIZE, the AOT region). Move the threshold or the image so the split has room.")
        endif()
    endforeach()
    message(STATUS "memory split: ${CONSOLE} ${grant} KB granted, image ${image_kb} KB, heap ${heap} KB, linear ${linear} KB (no cliff within 4 MB)")
endforeach()
