# Launcher dispatcher (tools/buildcache/launch_dispatch.py; docs/building-cia.md, "Launcher dispatcher"). Every ctrport
# compile runs the C_COMPILER_LAUNCHER chain built above: about 25 Python launchers, each started as its own process
# that runs the next one and waits for it. RULE_LAUNCH_COMPILE puts the dispatcher in front of that chain; it runs the
# chain's Python launchers in its one process, the same code making the same calls in the same order, and starts only
# the compiler, the assembler and the other tools as processes. The chain itself is not changed. OFF (or
# CTRPORT_LAUNCH_DISPATCH=0 in a compile's environment) runs every launcher as its own process again.
option(CTRPORT_LAUNCH_DISPATCH "Run each compile's launcher chain in one Python process (build.sh: ON for UNDERTALE)" OFF)
if(CTRPORT_LAUNCH_DISPATCH)
    set_property(TARGET ctrport PROPERTY RULE_LAUNCH_COMPILE
        "\"${CTRPORT_PYTHON_EXECUTABLE}\" -X utf8 -I -S \"${CMAKE_CURRENT_SOURCE_DIR}/../../tools/buildcache/launch_dispatch.py\"")
endif()
