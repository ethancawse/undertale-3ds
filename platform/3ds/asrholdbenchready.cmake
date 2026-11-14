# Generic benchmark ready-clear, UNDERTALE dev and release. No test switch.
# Install before noticescroll's object-routing pass, retaining its outer role.
if(CTRPORT_GAME STREQUAL "undertale")
    get_target_property(br_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/asrholdbenchready_compile.py;${br_delegate};--asrhold-benchready-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_benchmark.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/asrholdbenchready/ctr_benchmark.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/asrholdbenchready/ctr_benchready.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/asrholdbenchready_compile.py")
endif()
