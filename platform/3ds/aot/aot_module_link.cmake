# Links one AOT module so that it fits its region (aot_modules.cmake runs it as a build step; docs/aot.md).
#
# The generated files hold the functions in the selection's order, best first (gmlaot.py --layout reorders all but the
# last quarter of the files, which keep it), and aot_table.c refers to them weakly in module builds (AOT_FN_DECL in
# engine/src/aot/vm_aot.h). So a module too big for its region can be linked without its last files: their functions
# are null in the table and the interpreter runs that code. This links the module with every file, measures where it
# ends, and while it is over the region leaves out the last files (as many as their objects' sizes say, then one at a
# time), relinking each time. What it left out is logged and listed in <elf>.left-out.txt.
#
# Typed regions made from the player's data (gmlaot.py --typed all) come in files of their own,
# aot_typed_<n>, n ascending in priority order (loops by their function's rank, then straight regions by rank), and the
# generated code calls them through weak references with a null check: a region left out is null and its generated
# code runs instead. Those files count as the last ones, after every generated file, so they are left out first, the
# lowest-priority first, and a generated function is left out only when no typed region is left. Their regions are
# listed in <elf>.typed-left-out.txt. (aot_typed.c, from a typed list, and aot_typed_bind.c are always linked.)
#
# -D inputs: CC, ARCH, LDSCRIPT, REGION (bytes), MAIN_ELF, WRAPS, MAP, ELF, MODULE, OBJECTS, COMMON, TABLE_C, NM,
# OBJCOPY, SIZE, PART. List inputs are joined with "|".

foreach(v CC LDSCRIPT REGION MAIN_ELF MAP ELF MODULE OBJECTS COMMON TABLE_C NM OBJCOPY SIZE PART)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "aot_module_link.cmake: ${v} is not set")
    endif()
endforeach()
foreach(v ARCH WRAPS OBJECTS COMMON)
    string(REPLACE "|" ";" ${v} "${${v}}")
endforeach()

# The generated files in their order (aot_gen_<n>, n ascending), then the typed-region files in theirs (aot_typed_<n>);
# aot_table.c's object and any others go with COMMON. gen is both, in the order they may be left out from the end.
set(gen_numbered "")
set(typed_numbered "")
set(always ${COMMON})
foreach(obj IN LISTS OBJECTS)
    get_filename_component(name "${obj}" NAME)
    if(name MATCHES "^aot_(gen|typed)_([0-9]+)\\.")
        set(kind "${CMAKE_MATCH_1}")
        string(LENGTH "${CMAKE_MATCH_2}" len)
        math(EXPR pad "10 - ${len}")
        string(REPEAT "0" ${pad} zeros)
        list(APPEND ${kind}_numbered "${zeros}${CMAKE_MATCH_2}|${obj}")
    else()
        list(APPEND always "${obj}")
    endif()
endforeach()
list(SORT gen_numbered)
list(SORT typed_numbered)
set(gen "")
foreach(item IN LISTS gen_numbered typed_numbered)
    string(REGEX REPLACE "^[0-9]+\\|" "" obj "${item}")
    list(APPEND gen "${obj}")
endforeach()
list(LENGTH gen gen_count)
list(LENGTH gen_numbered gen_files)
list(LENGTH typed_numbered typed_files)

# Links with the first n generated objects; region is what the linker script's ASSERT checks. Sets module_end (the end of
# the module's bss, bytes from its start) or fails the build.
function(link_module n region)
    set(objs ${always})
    if(n GREATER 0)
        math(EXPR last "${n} - 1")
        foreach(i RANGE ${last})
            list(GET gen ${i} obj)
            list(APPEND objs "${obj}")
        endforeach()
    endif()
    # The objects go in a response file, as paths relative to the ELF's folder (the linker's working directory): a large
    # module's list is longer than a Windows command line may be, and devkitPro's MSYS2 shell translates the paths on
    # a command line for the compiler but not those in a file.
    get_filename_component(elf_dir "${ELF}" DIRECTORY)
    include("${elf_dir}/aot_numeric_string.cmake")
    set(numeric_import "")
    if(AOTM_NUMERIC_PARSER)
        list(APPEND numeric_import "-Wl,--defsym=${AOTM_NUMERIC_PARSER}=RValue_numericStrtod")
    endif()
    set(rsp "")
    foreach(obj IN LISTS objs)
        file(RELATIVE_PATH rel "${elf_dir}" "${obj}")
        string(APPEND rsp "\"${rel}\"\n")
    endforeach()
    file(WRITE "${ELF}.objects" "${rsp}")
    execute_process(
        COMMAND "${CC}" ${ARCH} -nostdlib -nostartfiles "-Wl,-T,${LDSCRIPT}" "-Wl,--defsym=__aotm_region_size=${region}"
                "-Wl,--just-symbols=${MAIN_ELF}" ${numeric_import} -Wl,--use-blx -Wl,--gc-sections ${WRAPS} "-Wl,-Map=${MAP}" -o "${ELF}"
                "@${ELF}.objects"
        WORKING_DIRECTORY "${elf_dir}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Linking AOT module ${PART} failed (${rc}):\n${out}")
    endif()
    execute_process(COMMAND "${NM}" "${ELF}" RESULT_VARIABLE rc OUTPUT_VARIABLE syms ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT syms MATCHES "([0-9a-fA-F]+) [A-Za-z] __aotm_bss_end[\r\n]")
        message(FATAL_ERROR "AOT module ${PART}: can't read __aotm_bss_end from ${ELF}: ${err}")
    endif()
    set(end_hex "${CMAKE_MATCH_1}")
    if(NOT syms MATCHES "([0-9a-fA-F]+) [A-Za-z] aotModuleHeader[\r\n]")
        message(FATAL_ERROR "AOT module ${PART}: can't read aotModuleHeader from ${ELF}")
    endif()
    math(EXPR end "0x${end_hex} - 0x${CMAKE_MATCH_1}")
    set(module_end ${end} PARENT_SCOPE)
endfunction()

# Text, data and bss of one object, from size's Berkeley output.
function(object_size obj outvar)
    execute_process(COMMAND "${SIZE}" "${obj}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_QUIET)
    set(bytes 0)
    if(rc EQUAL 0 AND out MATCHES "\n[ \t]*([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)")
        math(EXPR bytes "${CMAKE_MATCH_1} + ${CMAKE_MATCH_2} + ${CMAKE_MATCH_3}")
    endif()
    set(${outvar} ${bytes} PARENT_SCOPE)
endfunction()

# A trial link with room to spare, so the ASSERT doesn't stop it, measures the module.
math(EXPR unlimited "256 * 1024 * 1024")
set(keep ${gen_count})
link_module(${keep} ${unlimited})
set(full_end ${module_end})
while(module_end GREATER REGION AND keep GREATER 0)
    # Leave out as many of the last files as their objects say make up the excess; at least one.
    math(EXPR excess "${module_end} - ${REGION}")
    set(dropped 0)
    while(dropped LESS excess AND keep GREATER 0)
        math(EXPR keep "${keep} - 1")
        list(GET gen ${keep} obj)
        object_size("${obj}" bytes)
        math(EXPR dropped "${dropped} + ${bytes}")
    endwhile()
    link_module(${keep} ${unlimited})
endwhile()
# The objects' sizes are only an estimate (the link drops unused sections): take back files while the next still fits.
if(keep LESS gen_count)
    set(fits_end ${module_end})
    while(keep LESS gen_count)
        math(EXPR try "${keep} + 1")
        link_module(${try} ${unlimited})
        if(module_end GREATER REGION)
            break()
        endif()
        set(keep ${try})
        set(fits_end ${module_end})
    endwhile()
    set(module_end ${fits_end})
endif()
# The final link checks the region itself (the linker script's ASSERT).
link_module(${keep} ${REGION})

# What was left out: the functions the left-out files define, by name (from aot_table.c's entries), and the typed
# regions, by symbol (aott_ a loop, aots_ a straight region; the number is the function's rank, its aotf_ number).
set(report "${ELF}.left-out.txt")
set(typed_report "${ELF}.typed-left-out.txt")
set(typed_names "")
set(typed_count 0)
if(keep LESS gen_count)
    math(EXPR last "${gen_count} - 1")
    foreach(i RANGE ${keep} ${last})
        if(i LESS gen_files)
            continue()
        endif()
        list(GET gen ${i} obj)
        execute_process(COMMAND "${NM}" --defined-only "${obj}" OUTPUT_VARIABLE syms ERROR_QUIET)
        string(REGEX MATCHALL "[A-Za-z] aot[ts]_[0-9]+_[0-9A-F]+" defs "${syms}")
        foreach(def IN LISTS defs)
            string(REGEX REPLACE "^[A-Za-z] " "" region "${def}")
            list(APPEND typed_names "${region}")
            math(EXPR typed_count "${typed_count} + 1")
        endforeach()
    endforeach()
    math(EXPR typed_out "${gen_count} - ${keep}")
    if(typed_out GREATER typed_files)
        set(typed_out ${typed_files})
    endif()
    list(JOIN typed_names "\n" text)
    file(WRITE "${typed_report}" "${text}\n")
    if(typed_out GREATER 0)
        message(NOTICE "AOT module ${PART}: ${full_end} bytes with all its functions and typed regions, over its ${REGION}-byte "
                        "region. Left out the last ${typed_out} of ${typed_files} files of typed regions: ${typed_count} "
                        "regions, the lowest-priority (listed in ${typed_report}), whose generated code runs instead. Now "
                        "${module_end} bytes.")
    endif()
else()
    file(REMOVE "${typed_report}")
endif()
if(keep LESS gen_files)
    file(STRINGS "${TABLE_C}" entries REGEX ", aotf_[0-9]+, aotv_")
    foreach(entry IN LISTS entries)
        if(entry MATCHES "^ *{ \"([^\"]+)\".*, (aotf_[0-9]+), aotv_")
            set("name_${CMAKE_MATCH_2}" "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(names "")
    set(count 0)
    math(EXPR last "${gen_files} - 1")
    foreach(i RANGE ${keep} ${last})
        list(GET gen ${i} obj)
        execute_process(COMMAND "${NM}" --defined-only "${obj}" OUTPUT_VARIABLE syms ERROR_QUIET)
        string(REGEX MATCHALL "[A-Za-z] aotf_[0-9]+" defs "${syms}")
        foreach(def IN LISTS defs)
            string(REGEX REPLACE "^[A-Za-z] " "" fn "${def}")
            if(DEFINED "name_${fn}")
                list(APPEND names "${name_${fn}}")
            else()
                list(APPEND names "${fn}")
            endif()
            math(EXPR count "${count} + 1")
        endforeach()
    endforeach()
    list(JOIN names "\n" text)
    file(WRITE "${report}" "${text}\n")
    math(EXPR files "${gen_files} - ${keep}")
    message(NOTICE "AOT module ${PART}: ${full_end} bytes with all its functions, over its ${REGION}-byte region. Left out "
                    "the last ${files} of ${gen_files} files of generated code: ${count} functions, the lowest-ranked "
                    "(listed in ${report}), which run in the interpreter. Now ${module_end} bytes.")
else()
    file(REMOVE "${report}")
endif()

execute_process(COMMAND "${OBJCOPY}" -O binary "${ELF}" "${MODULE}" RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "objcopy of AOT module ${PART} failed: ${err}")
endif()
