# Bench-only screen-move duration observer, standalone on dv8u6.
# Include BEFORE vine_delegate is captured: utwindow requires -c on its
# outside and delegates -S on its inside. No existing launcher is changed.
if(CTRPORT_GAME STREQUAL "undertale")
    target_sources(ctrport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_screenmovestamp.c")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_screenmovestamp.c"
        APPEND PROPERTY COMPILE_OPTIONS "-marm")
    get_target_property(sms_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/screenmovestamp_compile.py;${sms_delegate};--screenmovestamp-compiler")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_renderer.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/screenmovestamp_compile.py")
endif()
