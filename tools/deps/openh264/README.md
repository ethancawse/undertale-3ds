# openh264

Cisco's [openh264](https://github.com/cisco/openh264) H.264 decoder, BSD-2-Clause (see `LICENSE`, the release's own),
decodes the intro videos in software where there's no hardware decoder (`platform/3ds/source/ctr_video_sw.c`).

The source isn't in the repo. `build.sh` downloads the pinned release once, checks it, and cross-builds the decoder
with devkitARM into `build/deps/openh264/` (`libopenh264dec.a`, `include/`, `LICENSE`), where
`platform/3ds/CMakeLists.txt` finds it. `platform/3ds/build.sh` runs it before each build; it does nothing when the
library is already built from the same release and script, and needs no network once the archive is in
`build/deps/cache/`. Without the library the build still works and videos play without pictures where there's no
hardware decoder.

## Source

Release `v2.6.0`, the GitHub tag archive
(`https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz`, SHA-256
`558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69`).

## What is built, and what was changed

- Only the decoder: `codec/decoder/core`, `welsDecoderExt.cpp` and the parts of `codec/common` they use. No encoder,
  no console tools.
- Portable C++ only. openh264's ARM assembly is NEON, which the 3DS's ARM11 doesn't have.
- Built with the flags of the rest of the 3DS build (`-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O3`),
  without exceptions or RTTI. `WelsThreadLib.cpp` is built with `__Fuchsia__` defined, its branch that needs only
  pthreads and `sysconf`; the decoder runs single-threaded here.
- One source change, made by `build.sh` as it unpacks: `ExpandBsBuffer` and `ExpandBsLenBuffer` in
  `codec/decoder/core/src/decoder_core.cpp` are defined with an `int` parameter but declared with `int32_t`.
  devkitARM's `int32_t` is `long`, so the definitions became separate overloads and the link failed; the
  definitions are changed to `int32_t`.

The library is 0.8 MB; it adds 504 KB to a DELTARUNE .3dsx (UNDERTALE, which has no videos, goes without).
