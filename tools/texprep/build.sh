#!/usr/bin/env bash
# Builds texprep for the machine it runs on: build.sh [output] (default: tools/texprep/build/texprep[.exe]).
# Needs a C compiler (CC, default cc) with pthreads. Windows: MSYS2 UCRT64 gcc (C:/msys64/ucrt64/bin).
# Extra compiler flags (a target, -arch, -mmacosx-version-min) go in TEXPREP_CFLAGS.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="$root/platform/3ds/source"
eng="$root/engine"
out="${1:-$here/build/texprep}"
# Windows: static, and no link time in the PE header, so the same sources and compiler give the same bytes.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe=.exe; link=(-static -Wl,--no-insert-timestamp) ;; *) exe=; link=(${TEXPREP_LDFLAGS:-}) ;; esac
[[ "$out" == *.exe || -z "$exe" ]] || out="$out$exe"
mkdir -p "$(dirname "$out")"
objdir="$(dirname "$out")/obj-$(basename "$out")"
rm -rf "$objdir"
mkdir -p "$objdir"
cc="${CC:-cc}"
# shellcheck disable=SC2206
extra=(${TEXPREP_CFLAGS:-})
# GCC 10/11 and older Apple clang lack this optional flag. Published cache headers/padding are explicitly cleared.
autozero=()
if "$cc" "${extra[@]}" -Werror -ftrivial-auto-var-init=zero -x c -c /dev/null -o "$objdir/zero-probe.o" >/dev/null 2>&1; then
    autozero=(-ftrivial-auto-var-init=zero)
elif "$cc" "${extra[@]}" -Werror -ftrivial-auto-var-init=zero -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang \
        -x c -c /dev/null -o "$objdir/zero-probe.o" >/dev/null 2>&1; then
    autozero=(-ftrivial-auto-var-init=zero -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang)
fi
base=(-std=gnu2x -O2 "${autozero[@]}" -ffile-prefix-map="$root"=. -DCTRPORT_GAME_DIR='"."' -DENABLE_WAD16 -DENABLE_WAD17
      -I"$here/shim" -I"$here" -I"$src" -I"$eng/src" -I"$eng/src/image"
      -isystem "$eng/vendor/stb/ds" -isystem "$eng/vendor/stb/image" -isystem "$eng/vendor/bzip2")
common=("${extra[@]}" "${base[@]}")
if [ -n "$exe" ]; then
    base+=(-include "$here/windows_utf8.h")
    common+=(-include "$here/windows_utf8.h")
    link+=(-municode)
fi
# Ours and the console's sources: warnings on, as in the console build.
ours=("$here/texprep.c" "$here/host_platform.c"
      "$src/ctr_png.c" "$src/ctr_inflate.c" "$src/ctr_qoi.c" "$src/ctr_bz2.c" "$src/ctr_lz.c" "$src/ctr_cache_publish.c"
      "$eng/src/image/image_decoder.c" "$eng/src/gettime.c" "$eng/src/log.c")
# Vendored libraries, as the console builds them.
vendor=("$src/stb_impl.c" "$eng/vendor/bzip2/bz_internal_error.c" "$eng/vendor/bzip2/bzlib.c" "$eng/vendor/bzip2/crctable.c"
        "$eng/vendor/bzip2/decompress.c" "$eng/vendor/bzip2/huffman.c" "$eng/vendor/bzip2/randtable.c")
# The sources digest (tools/texprep/sources.py): every file inside the repository that any object reads, and this
# script. A preprocessing pass lists them; the binary carries the digest and the list (texprep --sources).
py="${PYTHON:-}"
if [[ -z "$py" ]]; then
    if command -v python3 >/dev/null 2>&1; then py=python3; elif command -v py >/dev/null 2>&1; then py="py -3"; else
        echo "build.sh: Python 3 is needed for the sources digest (set PYTHON)" >&2; exit 1; fi
fi
deps=()
for f in "${ours[@]}" "${vendor[@]}"; do
    d="$objdir/$(basename "${f%.c}").d"
    "$cc" "${base[@]}" -DTEXPREP_DEPS_PASS -w -M -MT x -MF "$d" "$f"
    deps+=("$d")
done
# shellcheck disable=SC2086
$py -I "$here/sources.py" header "$root" "$objdir/texprep_sources.h" "${deps[@]}" >/dev/null
common+=(-I"$objdir")
objs=()
for f in "${ours[@]}"; do
    o="$objdir/$(basename "${f%.c}").o"
    "$cc" "${common[@]}" -Wall -Wextra -Wno-format-truncation -c "$f" -o "$o"
    objs+=("$o")
done
for f in "${vendor[@]}"; do
    o="$objdir/v_$(basename "${f%.c}").o"
    "$cc" "${common[@]}" -w -c "$f" -o "$o"
    objs+=("$o")
done
"$cc" "${extra[@]}" "${objs[@]}" -o "$out" "${link[@]}" -lpthread -lm
rm -rf "$objdir"
echo "built $out"
