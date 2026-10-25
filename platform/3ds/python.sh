# Sourced by build.sh. Keep the chosen command as an array, including py's -3.
# An explicit interpreter survives a devkitPro login shell's PATH reset.
release_find_python() {
    python=()
    if [ -n "${CTRPORT_PYTHON:-}" ]; then
        if "$CTRPORT_PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 8))' > /dev/null 2>&1; then
            python=("$CTRPORT_PYTHON")
        else
            printf '%s\n' 'ERROR: CTRPORT_PYTHON must name a working Python 3.8+ executable.' >&2
            return 2
        fi
    else
        for p in python3 python; do
            if command -v "$p" > /dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' > /dev/null 2>&1; then
                python=("$p")
                break
            fi
        done
        if [ "${#python[@]}" = 0 ] && command -v py > /dev/null 2>&1 && py -3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' > /dev/null 2>&1; then
            python=(py -3)
        fi
    fi
    if [ "${#python[@]}" = 0 ]; then
        printf '%s\n' 'ERROR: Python 3.8+ is required; no interpreter was found.' \
            'Set CTRPORT_PYTHON to its full executable path, or put python3/python/py on PATH.' \
            'The devkitPro login shell may reset PATH. No build or no-AOT fallback was started.' >&2
        return 2
    fi
}

# CMake/make run in MSYS, while sys.executable from Windows Python uses a drive
# prefix. Passing C:/... as FILEPATH to MSYS CMake makes it relative to the repo.
release_python_executable() {
    local executable
    executable=$("${python[@]}" -c 'import sys; print(sys.executable)')
    executable=${executable//$'\r'/}
    if command -v cygpath > /dev/null 2>&1; then
        cygpath -u "$executable"
    else
        printf '%s\n' "$executable"
    fi
}
