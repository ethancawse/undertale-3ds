# PacketVideo AAC decoder

Decodes the AAC-LC audio track of DELTARUNE Chapter 3's intro video
(`ctr_audio_decoder.c`, through `ctr_mp4.c`). Fixed-point, so it suits the
3DS's ARM11, and Apache-2.0 licensed (see `LICENSE` and `NOTICE`).

## Source

The Android Open Source Project's `frameworks/base`, tag `android-4.0.4_r2.1`,
directory `media/libstagefright/codecs/aacdec`: PacketVideo's decoder from
OpenCORE, the one Android used before it moved to Fraunhofer's.

## What was changed

- Only the files plain AAC-LC decoding needs are here: the 67 sources the
  decoder and `PVMP4AudioDecoderResetBuffer` (used when seeking) link without
  `AAC_PLUS` (no SBR or parametric stereo, so no HE-AAC), and the headers they
  include. The SBR and PS headers stay because the
  decoder's state structures refer to them.
- The sources are renamed from `.cpp` to `.c` and built as C (C++ isn't
  needed; the code is C apart from using `bool`, which the build supplies with
  `-include stdbool.h`).
- `src/get_audio_specific_config.c`: one cast names the enum's typedef instead
  of its tag, so that it compiles as C. The line is marked with a `the port:`
  comment.

Nothing else is changed. Multiplies use the portable C versions
(`fxp_mul32_c_equivalent.h`).

## Checked

On a PC, the Chapter 3 track decoded with these files matched FFmpeg's decode
sample for sample in timing and within 0.1 dB in every frame's level. The
waveforms differ only in the frames where the encoder used perceptual noise
substitution, whose noise each decoder makes up itself.
