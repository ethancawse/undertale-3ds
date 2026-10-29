// ===[ sfxcopy (aotprof): buildSfx's dual-mono check and PCM copy, a word at a time (late code, both games) ]===
// The first play of a sound not in the SFX cache copies its WAV into linear memory on the game thread
// (prepareAudoSource -> buildSfx), after the loader thread has read it. 223 of UNDERTALE's 227 WAVs have their samples
// at 2 mod 4 (an 18-byte fmt chunk) and linearAlloc's buffer is aligned, so newlib's memcpy took its byte loop
// (4 instructions a byte); a stereo file's dual-mono check compared two bytes per sample and the dual-mono copy moved
// one byte at a time (17 instructions a frame). On Azahar Old a 213 KB sound's copy took 3.3 ms (aotprof run
// flaz4); on the Old consoles a copy takes 5-9 ms, 10-13 copies in Flowey's fight as its attacks bring shed sounds
// back (flowend), 12.7 ms for a text sound in cont_snow (freebies). Here a 16-bit frame is one word: a stereo frame's
// channels match when its two halves do, the dual-mono copy packs two frames' left halves into one word, and a copy
// from 2 mod 4 joins the halves of neighbouring words. Same decision, same bytes in the same buffer, same allocation
// (buildSfx still checks first, then allocates, then copies). Sources 0 or 2 mod 4 into an aligned buffer take the
// word loops (0 mod 4 copies stay memcpy's); 1 or 3 mod 4, the last few frames and 8-bit data take the byte loops,
// unchanged. Proven byte-identical on every WAV in UNDERTALE 1.08 and DELTARUNE (tools/analysis/sfxcopy_test.c, run
// by tools/analysis/sfxcopy_check.py).
// Shared with that host test, so plain C: no libctru here.
#ifndef SFXCOPY_LATE
#define SFXCOPY_LATE __attribute__((noinline, section(".text.sfxcopy_late")))
#endif

typedef uint32_t __attribute__((may_alias)) SfxWord;

SFXCOPY_LATE bool sfxIsDualMono(const uint8_t* data, uint32_t frames, uint32_t bytesPerSample);
SFXCOPY_LATE void sfxCopyPcm(uint8_t* pcm, const uint8_t* src, uint32_t frames, uint32_t bytesPerSample, bool dualMono,
                             uint32_t bytes);

SFXCOPY_LATE bool sfxIsDualMono(const uint8_t* data, uint32_t frames, uint32_t bytesPerSample) {
    uint32_t i = 0;
    if (bytesPerSample == 2 && ((uintptr_t) data & 3u) == 0) {
        // Little-endian frame word: left in the low half, right in the high half.
        const SfxWord* w = (const SfxWord*) data;
        for (; i + 4 <= frames; i += 4) {
            uint32_t a = w[i], b = w[i + 1], c = w[i + 2], d = w[i + 3];
            if (((a ^ (a >> 16)) | (b ^ (b >> 16)) | (c ^ (c >> 16)) | (d ^ (d >> 16))) << 16) return false;
        }
    } else if (bytesPerSample == 2 && ((uintptr_t) data & 3u) == 2) {
        // Data 2 mod 4 (a WAV whose fmt chunk is 18 bytes: most of UNDERTALE's): word k of the aligned words from 2
        // bytes before holds frame k-1's right channel in its low half and frame k's left in its high half. Frame i
        // matches when word i's high half equals word i+1's low half. Words 0..frames-1 lie in the blob: the caller
        // must have the 2 bytes before data readable (buildSfx: the WAV header's last 2 bytes). The last frame, whose
        // right half would need word `frames`, is left to the byte loop, and so is a sound of 4 frames or fewer (no
        // word is read then).
        if (frames > 4u) {
            const SfxWord* w = (const SfxWord*) (data - 2);
            uint32_t prev = w[0];
            for (; i + 4 < frames; i += 4) {
                uint32_t a = w[i + 1], b = w[i + 2], c = w[i + 3], d = w[i + 4];
                if (((a ^ (prev >> 16)) | (b ^ (a >> 16)) | (c ^ (b >> 16)) | (d ^ (c >> 16))) << 16) return false;
                prev = d;
            }
        }
    }
    if (bytesPerSample == 2) {
        for (data += i * 4; i < frames; i++, data += 4) {
            if (data[0] != data[2] || data[1] != data[3]) return false;
        }
    } else {
        for (; i < frames; i++, data += 2) {
            if (data[0] != data[1]) return false;
        }
    }
    return true;
}

SFXCOPY_LATE void sfxCopyPcm(uint8_t* pcm, const uint8_t* src, uint32_t frames, uint32_t bytesPerSample, bool dualMono,
                             uint32_t bytes) {
    if (bytesPerSample == 2) {
        uint32_t i = 0;
        if (!dualMono) {
            if (((uintptr_t) pcm & 3u) == 0 && ((uintptr_t) src & 3u) == 2) {
                SfxWord* o = (SfxWord*) pcm;
                // Output word j is bytes 4j..4j+3: the high half of word j and the low half of word j+1 of the aligned
                // words from 2 bytes before. Word j+1 must end inside the data: j + 1 < bytes / 4 at most.
                const SfxWord* w = (const SfxWord*) (src - 2);
                uint32_t words = bytes / 4u, j = 0;
                for (; j + 5 <= words; j += 4) {
                    uint32_t a = w[j], b = w[j + 1], c = w[j + 2], d = w[j + 3], e = w[j + 4];
                    o[j] = (a >> 16) | (b << 16);
                    o[j + 1] = (b >> 16) | (c << 16);
                    o[j + 2] = (c >> 16) | (d << 16);
                    o[j + 3] = (d >> 16) | (e << 16);
                }
                i = j * 4u;
            }
            memcpy(pcm + i, src + i, bytes - i);
            return;
        }
        if (((uintptr_t) pcm & 3u) == 0 && ((uintptr_t) src & 3u) == 0) {
            SfxWord* o = (SfxWord*) pcm;
            const SfxWord* w = (const SfxWord*) src;
            for (; i + 4 <= frames; i += 4) {
                uint32_t a = w[i], b = w[i + 1], c = w[i + 2], d = w[i + 3];
                o[i / 2] = (a & 0xFFFFu) | (b << 16);
                o[i / 2 + 1] = (c & 0xFFFFu) | (d << 16);
            }
        } else if (((uintptr_t) pcm & 3u) == 0 && ((uintptr_t) src & 3u) == 2) {
            // As in sfxIsDualMono: frame k's left is the high half of word k from 2 bytes before.
            SfxWord* o = (SfxWord*) pcm;
            const SfxWord* w = (const SfxWord*) (src - 2);
            for (; i + 4 <= frames; i += 4) {
                uint32_t a = w[i], b = w[i + 1], c = w[i + 2], d = w[i + 3];
                o[i / 2] = (a >> 16) | (b & 0xFFFF0000u);
                o[i / 2 + 1] = (c >> 16) | (d & 0xFFFF0000u);
            }
        }
        for (; i < frames; i++) {
            pcm[i * 2] = src[i * 4];
            pcm[i * 2 + 1] = src[i * 4 + 1];
        }
        return;
    }
    // WAV 8-bit is unsigned; NDSP PCM8 is signed.
    uint32_t step = dualMono ? 2 : 1;
    uint32_t samples = bytes;
    for (uint32_t i = 0; i < samples; i++) pcm[i] = src[i * step] ^ 0x80;
}
