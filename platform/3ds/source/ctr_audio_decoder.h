#ifndef CTRPORT_CTR_AUDIO_DECODER_H
#define CTRPORT_CTR_AUDIO_DECODER_H

// Pull decoder for the audio streams: Ogg Vorbis (Tremor, or stb_vorbis when the
// build has no Tremor) and PCM WAV, read from a file or from memory, and the
// AAC-LC audio track of an MP4 video file (ctr_mp4.c and the vendored
// PacketVideo decoder; from a file only). Output is always interleaved signed
// 16-bit at the source rate, with 1 or 2 channels.
//
// A decoder is used by one thread at a time; it does no locking of its own.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ctr_read_ahead.h"

#ifdef CTRPORT_HAVE_TREMOR
#include <tremor/ivorbisfile.h>
#endif

typedef enum {
    CTR_DECODER_NONE,
    CTR_DECODER_VORBIS,
    CTR_DECODER_WAV,
    CTR_DECODER_MP4,
} CtrDecoderKind;

typedef struct CtrMp4Aac CtrMp4Aac;

typedef struct {
    CtrDecoderKind kind;

    // Byte source: a file, the file read ahead (which then owns it), or a
    // memory block (not owned). A file source may be a section of the file
    // (CtrDecoder_sectionPath): positions are then relative to secBase.
    FILE* file;
    char* fileBuffer;
    CtrReadAhead* readAhead;
    const uint8_t* mem;
    uint32_t memSize;
    uint32_t memPos;
    bool sectioned;
    long secBase;
    uint32_t secSize;
    uint32_t secPos;

    uint32_t channels;
    uint32_t sampleRate;
    uint64_t totalFrames; // 0 when unknown
    uint64_t positionFrames;

    // WAV
    uint32_t wavBits;
    uint32_t wavDataStart;
    uint32_t wavDataBytes;

    // MP4
    CtrMp4Aac* aac;

    // Vorbis
    bool vorbisOpen;
#ifdef CTRPORT_HAVE_TREMOR
    OggVorbis_File vf;
#else
    struct stb_vorbis* stb;
#endif
} CtrDecoder;

// What a probe finds out about a sound without setting up a decoder.
typedef struct {
    CtrDecoderKind kind;
    uint32_t channels;
    uint32_t sampleRate;
    uint64_t totalFrames; // 0 when unknown
    uint32_t sourceBytes; // size of the file or blob
} CtrAudioInfo;

// The name of a sound kept inside a bigger file (an embedded sound in data.win
// or an audiogroup .dat), which CtrDecoder_probeFile and CtrDecoder_openFile
// take in place of a path: size bytes at offset of path. With sizeBefore, size
// is only an upper bound and the exact size is the 32-bit word in front of the
// data. Returns a malloc'd string.
char* CtrDecoder_sectionPath(const char* path, uint32_t offset, uint32_t size, bool sizeBefore);

// Format, channels, rate and length of a sound. For Ogg Vorbis this reads only
// the identification header and the last page's granule position, which is far
// cheaper than opening a decoder (no codebook setup, no bisection seeks).
// False if the data is not Ogg Vorbis or PCM WAV the decoder can play.
bool CtrDecoder_probeFile(const char* path, CtrAudioInfo* info);
bool CtrDecoder_probeMemory(const uint8_t* data, uint32_t size, CtrAudioInfo* info);
// Format from the first bytes alone: CTR_DECODER_NONE if not "OggS", "RIFF" or
// an MP4 "ftyp" box.
CtrDecoderKind CtrDecoder_sniff(const uint8_t* data, uint32_t size);

// Both return false (with the decoder closed) if the data is not Ogg Vorbis,
// 8/16-bit PCM WAV with 1 or 2 channels, or (from a whole file) an MP4 file
// with an AAC-LC track. path may name a section of a file.
bool CtrDecoder_openFile(CtrDecoder* dec, const char* path);
bool CtrDecoder_openMemory(CtrDecoder* dec, const uint8_t* data, uint32_t size);
// For an open file decoder: hand the file to the read-ahead thread, so SD
// card stalls don't reach the decoder. False when no ring is free; the
// decoder keeps reading the file itself.
bool CtrDecoder_startReadAhead(CtrDecoder* dec);
// Decodes up to maxFrames frames into out (room for maxFrames * channels
// samples). Usually returns one Vorbis packet's worth, so callers loop. Returns
// 0 at the end of the data and -1 on a decode error.
int32_t CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
bool CtrDecoder_seek(CtrDecoder* dec, uint64_t frame);
void CtrDecoder_close(CtrDecoder* dec);
const char* CtrDecoder_vorbisBackendName(void);

#endif
