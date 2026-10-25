#!/usr/bin/env bash
# Fetch and cross-build openh264's H.264 decoder for the 3DS: the software video decoder for consoles without the New 3DS
# hardware decoder (platform/3ds/source/ctr_video_sw.c). See README.md here for what is built and why.
#
# usage: tools/deps/openh264/build.sh [--force]
#
# Output: build/deps/openh264/ with libopenh264dec.a, include/ and LICENSE, which platform/3ds/CMakeLists.txt picks up
# on its own. The release archive is downloaded once into build/deps/cache/ and checked against its SHA-256; after that
# the script needs no network, and when the output is already built from the same archive and the same version of
# this script it does nothing at all. --force rebuilds anyway.
#
# Needs devkitARM ($DEVKITPRO or $DEVKITARM), curl or wget, tar and sha256sum. On Windows, run it from the devkitPro
# MSYS2 shell, or from any shell as:
#   C:/devkitPro/msys2/usr/bin/bash.exe --login -c "cd <repo> && tools/deps/openh264/build.sh"
set -euo pipefail

version=2.6.0
url=https://github.com/cisco/openh264/archive/refs/tags/v$version.tar.gz
sha256=558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69

force=
for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        *) echo "usage: $0 [--force]" >&2; exit 2 ;;
    esac
done

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
deps="$repo/build/deps"
out="$deps/openh264"
archive="$deps/cache/openh264-$version.tar.gz"

# What the output was built from: the archive and this script (its flags and source list).
stamp="openh264 $version $sha256 script $(sha256sum < "$0" | cut -d' ' -f1)"
if [ -z "$force" ] && [ -f "$out/libopenh264dec.a" ] && [ "$(cat "$out/stamp" 2> /dev/null)" = "$stamp" ]; then
    echo "openh264 $version: up to date in $out"
    exit 0
fi

if [ -z "${DEVKITARM:-}" ]; then
    : "${DEVKITPRO:?neither DEVKITARM nor DEVKITPRO is set; install devkitPro and the 3ds-dev group}"
    DEVKITARM=$DEVKITPRO/devkitARM
fi
cxx=$DEVKITARM/bin/arm-none-eabi-g++
ar=$DEVKITARM/bin/arm-none-eabi-ar
[ -x "$cxx" ] || [ -x "$cxx.exe" ] || { echo "no $cxx; install devkitARM" >&2; exit 1; }

mkdir -p "$deps/cache"
if [ ! -f "$archive" ] || [ "$(sha256sum < "$archive" | cut -d' ' -f1)" != "$sha256" ]; then
    echo "openh264 $version: downloading $url"
    rm -f "$archive.part"
    if command -v curl > /dev/null 2>&1; then
        curl -fL --retry 3 -o "$archive.part" "$url"
    elif command -v wget > /dev/null 2>&1; then
        wget -O "$archive.part" "$url"
    else
        echo "neither curl nor wget found to download $url" >&2
        exit 1
    fi
    got=$(sha256sum < "$archive.part" | cut -d' ' -f1)
    if [ "$got" != "$sha256" ]; then
        echo "openh264 $version: the download's SHA-256 is $got, not $sha256" >&2
        rm -f "$archive.part"
        exit 1
    fi
    mv "$archive.part" "$archive"
fi

work="$deps/src"
src="$work/openh264-$version"
rm -rf "$src" "$out"
mkdir -p "$work" "$out/obj" "$out/include"
tar -xzf "$archive" -C "$work" "openh264-$version/codec" "openh264-$version/LICENSE"

# The one source change: two definitions take int where their declarations (decoder_core.h) take int32_t. They are
# the same type on most targets, but devkitARM's int32_t is long, so the definitions became overloads nothing calls
# and the link failed.
core="$src/codec/decoder/core/src/decoder_core.cpp"
for fn in ExpandBsBuffer:kiSrcLen ExpandBsLenBuffer:kiCurrLen; do
    name=${fn%%:*}
    param=${fn#*:}
    sed -i "s/^int32_t $name (PWelsDecoderContext pCtx, const int $param) {\$/int32_t $name (PWelsDecoderContext pCtx, const int32_t $param) {/" "$core"
    if ! grep -q "^int32_t $name (PWelsDecoderContext pCtx, const int32_t $param) {\$" "$core"; then
        echo "openh264 $version: couldn't patch $name in $core" >&2
        exit 1
    fi
done

# The decoder in portable C++ (openh264's ARM assembly is NEON, which the 3DS's ARM11 doesn't have), with the ABI and
# tuning of the rest of the 3DS build. The thread library is built as for Fuchsia, the platform branch of
# WelsThreadLib.cpp that only needs pthreads and sysconf (the decoder runs single-threaded here anyway).
flags=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O3 -fno-exceptions -fno-rtti
       -ffunction-sections -fdata-sections -D__3DS__ -w "-ffile-prefix-map=$src=openh264-$version")
includes=(-I"$src/codec/api" -I"$src/codec/api/wels" -I"$src/codec/common/inc" -I"$src/codec/decoder/core/inc"
          -I"$src/codec/decoder/plus/inc")
sources=("$src"/codec/decoder/core/src/*.cpp "$src/codec/decoder/plus/src/welsDecoderExt.cpp")
for f in common_tables copy_mb cpu crt_util_safe_x deblocking_common expand_pic intra_pred_common mc memory_align \
         sad_common utils welsCodecTrace WelsTaskThread WelsThread WelsThreadLib WelsThreadPool; do
    sources+=("$src/codec/common/src/$f.cpp")
done

echo "openh264 $version: building ${#sources[@]} files"
jobs=$(nproc 2> /dev/null || echo 4)
pids=()
objects=()
for s in "${sources[@]}"; do
    o="$out/obj/$(basename "$s" .cpp).o"
    objects+=("$o")
    extra=()
    case "$s" in *WelsThreadLib.cpp) extra=(-D__Fuchsia__) ;; esac
    "$cxx" "${flags[@]}" "${extra[@]}" "${includes[@]}" -c "$s" -o "$o" &
    pids+=($!)
    if [ "${#pids[@]}" -ge "$jobs" ]; then
        wait "${pids[0]}"
        pids=("${pids[@]:1}")
    fi
done
for pid in "${pids[@]}"; do
    wait "$pid"
done

# D: no timestamps or owners in the archive, so the same inputs give the same library.
"$ar" rcsD "$out/libopenh264dec.a" "${objects[@]}"
rm -rf "$out/obj"
cp "$src"/codec/api/wels/codec_api.h "$src"/codec/api/wels/codec_app_def.h "$src"/codec/api/wels/codec_def.h \
   "$src"/codec/api/wels/codec_ver.h "$out/include/"
cp "$src/LICENSE" "$out/LICENSE"
rm -rf "$src"
printf '%s\n' "$stamp" > "$out/stamp"
echo "openh264 $version: built $out/libopenh264dec.a"
