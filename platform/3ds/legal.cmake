# Full source texts plus prewrapped pages stay in RomFS, outside the ELF.
set(CTRPORT_PYTHON_EXECUTABLE "" CACHE FILEPATH "Host Python selected by build.sh/the builder")
if(NOT CTRPORT_PYTHON_EXECUTABLE)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(CTRPORT_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
endif()
set(CTRPORT_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${CTRPORT_ROMFS_DIR}/legal")
get_filename_component(RELEASE_LEGAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RELEASE_LEGAL_FILES "")
foreach(name LICENSE THIRD_PARTY_NOTICES)
    list(APPEND RELEASE_LEGAL_FILES "${CTRPORT_ROMFS_DIR}/legal/${name}" "${CTRPORT_ROMFS_DIR}/legal/${name}.pages")
endforeach()
add_custom_command(OUTPUT ${RELEASE_LEGAL_FILES}
    COMMAND "${CTRPORT_PYTHON_EXECUTABLE}" -I "${RELEASE_LEGAL_ROOT}/tools/release/prepare_legal.py"
            --root "${RELEASE_LEGAL_ROOT}" --output "${CTRPORT_ROMFS_DIR}/legal"
    DEPENDS "${RELEASE_LEGAL_ROOT}/LICENSE" "${RELEASE_LEGAL_ROOT}/THIRD_PARTY_NOTICES"
            "${RELEASE_LEGAL_ROOT}/tools/release/prepare_legal.py"
            "${RELEASE_LEGAL_ROOT}/docs/release/build-contract.json"
    COMMENT "Bundling full licences, third-party notices and console pages"
    VERBATIM)
add_custom_target(legal_texts DEPENDS ${RELEASE_LEGAL_FILES})
