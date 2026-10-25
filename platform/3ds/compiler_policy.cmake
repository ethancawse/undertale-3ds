# Shared eligibility gate for all compiler-dependent layout pins. Never cached:
# reconfiguring the same build directory with another compiler rechecks it.
function(release_compiler_policy contract_file)
    file(READ "${contract_file}" contract)
    foreach(key id version target)
        string(JSON tested_${key} GET "${contract}" tested_compiler "${key}")
    endforeach()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                    RESULT_VARIABLE target_result OUTPUT_VARIABLE actual_target
                    OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 10 ERROR_QUIET)
    set(enabled OFF)
    if(CMAKE_C_COMPILER_ID STREQUAL tested_id AND CMAKE_C_COMPILER_VERSION VERSION_EQUAL tested_version
       AND target_result EQUAL 0 AND actual_target STREQUAL tested_target)
        set(enabled ON)
    else()
        # STATUS is intentional: this is advice, not a compiler warning or a refusal.
        message(STATUS "Tested with devkitARM GCC ${tested_version} (${tested_target}); found ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${actual_target}), so layout pins are off and performance may differ slightly.")
    endif()
    set(CTRPORT_LAYOUT_PINS_ENABLED "${enabled}" PARENT_SCOPE)
endfunction()
