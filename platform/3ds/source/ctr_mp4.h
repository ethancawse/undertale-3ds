// A small MP4 (ISO base media file) demuxer for the games' intro videos:
// the first video and the first audio track of a file, their codec setup
// and where each of their samples is, from the moov box. Fragmented files
// (moof) aren't read. Samples are read from the file as they are needed, so
// only the index is kept in memory.
//
// Each user opens the file itself (the video and audio decoders run on
// different threads); a CtrMp4 does no locking.
#ifndef CTRPORT_CTR_MP4_H
#define CTRPORT_CTR_MP4_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CTR_MP4_FOURCC(a, b, c, d) (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 8) | (uint32_t) (d))

typedef struct {
    uint32_t codec;     // the sample entry's type: 'avc1', 'mp4a', ...; 0 when there is no such track
    uint32_t timescale; // ticks per second of the times below
    uint32_t count;     // samples, in decode order
    uint64_t* offsets;  // file offset of each sample
    uint32_t* sizes;
    // Presentation time of each sample in ticks, the edit list applied: the
    // first sample shown is at 0. Audio priming samples come out negative.
    int64_t* pts;
    uint8_t* sync; // nonzero for a sync sample (a keyframe)
    uint32_t maxSize;
    double duration; // seconds, from the edit list if there is one
    // Codec setup: the AVCDecoderConfigurationRecord (avcC) or the MPEG-4
    // AudioSpecificConfig (from esds).
    uint8_t* config;
    uint32_t configSize;
    uint32_t width, height;        // video
    uint32_t channels, sampleRate; // audio, from the sample entry
} CtrMp4Track;

typedef struct {
    FILE* file;
    CtrMp4Track video;
    CtrMp4Track audio;
    double duration; // seconds (the movie header's)
    // Samples are read through a window onto the file: a track's samples sit
    // between the other track's, so reading the span that holds the next
    // few takes one SD card read where reading each would take one apiece.
    uint8_t* window;
    uint32_t windowCap, windowLen;
    uint64_t windowStart;
} CtrMp4;

// Whether the first bytes look like an MP4 file ("ftyp" box).
bool CtrMp4_sniff(const uint8_t* data, uint32_t size);

// Opens and indexes a file, reading samples through a window of windowSize
// bytes (a sample larger than that is read on its own). False (with nothing
// left open) if it isn't an MP4 file with a moov box.
bool CtrMp4_open(CtrMp4* mp4, const char* path, uint32_t windowSize);
void CtrMp4_close(CtrMp4* mp4);

// Reads sample `index` of a track into out, which has room for its size.
bool CtrMp4_readSample(CtrMp4* mp4, const CtrMp4Track* track, uint32_t index, void* out);

#endif
