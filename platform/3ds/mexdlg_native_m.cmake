# Native-M writer adapter, included directly by CMakeLists.txt.
# Dev and release default ON (review MF-4); cached overrides remain valid.
option(CTRPORT_MEXDLG_NATIVE_M "Native writer M escape with checked ordered stores" ON)
if(CTRPORT_MEXDLG_NATIVE_M AND CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_TARGET STREQUAL "o3ds")
    get_target_property(mexdlg_native_delegate ctrport C_COMPILER_LAUNCHER)
    set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER
        "${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/mexdlg_native_compile.py;${mexdlg_native_delegate};--mexdlg-native-compiler")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/source/z_mexdlg_native.c" PROPERTIES
        COMPILE_OPTIONS "-Os;-fno-merge-constants;-fno-tree-switch-conversion;-fno-jump-tables"
        COMPILE_DEFINITIONS CTRPORT_MEXDLG_NATIVE_M)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/z_writerfront.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/z_mexdlg_native.c" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/mexdlg_native_compile.py")
endif()
