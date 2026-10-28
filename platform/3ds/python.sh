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

# A devkitPro login shell started from Git Bash (another MSYS runtime) gets no APPDATA, LOCALAPPDATA or USERPROFILE,
# though one started from PowerShell, cmd or Python keeps them. Windows Python then looks for its user site-packages
# in the wrong place, and a tool whose numpy/Pillow live there can't import them. Restore the missing ones from the
# logon session's own copy (HKCU\Volatile Environment). Values already set are kept; elsewhere this does nothing.
release_windows_profile_env() {
    local line name value
    command -v reg.exe > /dev/null 2>&1 || return 0
    if [ -n "${APPDATA:-}" ] && [ -n "${LOCALAPPDATA:-}" ] && [ -n "${USERPROFILE:-}" ]; then
        return 0
    fi
    while IFS= read -r line; do
        line=${line//$'\r'/}
        if [[ $line =~ ^[[:space:]]+(APPDATA|LOCALAPPDATA|USERPROFILE)[[:space:]]+REG_SZ[[:space:]]+(.+)$ ]]; then
            name=${BASH_REMATCH[1]}
            value=${BASH_REMATCH[2]}
            if [ -z "${!name:-}" ]; then
                export "$name=$value"
            fi
        fi
    done < <(reg.exe query 'HKCU\Volatile Environment' 2> /dev/null || true)
}

# A derivation the shipping game needs could not run ("<what>" "<why>", e.g. the import that failed). The build
# stops with exit 3 (the caller exits with it), so a build that looks fine never ships or tests the game without it.
# CTRPORT_DERIVATION_OPTIONAL=1 is for tools-only builds: they go on without it, with a note.
release_missing_derivation() {
    if [ "${CTRPORT_DERIVATION_OPTIONAL:-0}" = 1 ]; then
        printf '%s\n' "$1: $2; CTRPORT_DERIVATION_OPTIONAL=1, building without it" >&2
        return 0
    fi
    printf '%s\n' "ERROR: $1: $2." \
        'The shipping game needs it, so the build stops. Fix the Python environment (numpy/Pillow, APPDATA), or set' \
        'CTRPORT_DERIVATION_OPTIONAL=1 for a tools-only build without it.' >&2
    return 3
}
