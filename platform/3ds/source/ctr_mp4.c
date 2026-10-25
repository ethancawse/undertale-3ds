#include "ctr_mp4.h"

#include <stdlib.h>
#include <string.h>

// ===[ Reading boxes ]===

static uint32_t be16(const uint8_t* p) {
    return ((uint32_t) p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t* p) {
    return ((uint64_t) be32(p) << 32) | be32(p + 4);
}

// A box inside a parsed buffer: its type and payload.
typedef struct {
    uint32_t type;
    const uint8_t* data;
    uint32_t size;
} Box;

// The next box of [*p, end), advancing *p past it. False at the end or on a
// box that doesn't fit.
static bool nextBox(const uint8_t** p, const uint8_t* end, Box* box) {
    if (end - *p < 8) return false;
    uint64_t size = be32(*p);
    uint32_t header = 8;
    box->type = be32(*p + 4);
    if (size == 1) {
        if (end - *p < 16) return false;
        size = be64(*p + 8);
        header = 16;
    } else if (size == 0) {
        size = (uint64_t) (end - *p);
    }
    if (size < header || size > (uint64_t) (end - *p)) return false;
    box->data = *p + header;
    box->size = (uint32_t) (size - header);
    *p += size;
    return true;
}

// The first child of type `type` in a container's payload.
static bool findBox(const uint8_t* data, uint32_t size, uint32_t type, Box* out) {
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    Box box;
    while (nextBox(&p, end, &box)) {
        if (box.type == type) {
            *out = box;
            return true;
        }
    }
    return false;
}

#define FOURCC CTR_MP4_FOURCC

// ===[ Sample tables ]===

typedef struct {
    Box stsd, stts, ctts, stsc, stsz, stco, stss;
    bool hasCtts, hasStss, co64;
    uint32_t handler;
    uint32_t timescale;
    uint64_t mediaDuration;
    // Edit list: the media time shown first, and a delay before it (movie
    // timescale), from a single edit or an empty one followed by another.
    bool hasEdit;
    int64_t editMediaTime;
    uint64_t editDelay, editDuration;
} TrakInfo;

static bool parseElst(const Box* elst, TrakInfo* t) {
    if (elst->size < 8) return false;
    uint32_t version = elst->data[0];
    uint32_t count = be32(elst->data + 4);
    uint32_t entrySize = version == 1 ? 20 : 12;
    if (count > (elst->size - 8) / entrySize) return false;
    const uint8_t* e = elst->data + 8;
    for (uint32_t i = 0; count > i; i++, e += entrySize) {
        uint64_t duration = version == 1 ? be64(e) : be32(e);
        int64_t mediaTime = version == 1 ? (int64_t) be64(e + 8) : (int64_t) (int32_t) be32(e + 4);
        if (mediaTime == -1) {
            t->editDelay += duration;
            continue;
        }
        // Only the first edit that shows media is followed; the games'
        // videos have one.
        t->hasEdit = true;
        t->editMediaTime = mediaTime;
        t->editDuration = duration;
        break;
    }
    return true;
}

static bool parseTrak(const Box* trak, TrakInfo* t) {
    memset(t, 0, sizeof(*t));
    Box edts, elst, mdia, mdhd, hdlr, minf, stbl;
    if (findBox(trak->data, trak->size, FOURCC('e', 'd', 't', 's'), &edts) && findBox(edts.data, edts.size, FOURCC('e', 'l', 's', 't'), &elst)) {
        parseElst(&elst, t);
    }
    if (!findBox(trak->data, trak->size, FOURCC('m', 'd', 'i', 'a'), &mdia)) return false;
    if (!findBox(mdia.data, mdia.size, FOURCC('m', 'd', 'h', 'd'), &mdhd) || mdhd.size < 24) return false;
    if (mdhd.data[0] == 1) {
        if (mdhd.size < 32) return false;
        t->timescale = be32(mdhd.data + 20);
        t->mediaDuration = be64(mdhd.data + 24);
    } else {
        t->timescale = be32(mdhd.data + 12);
        t->mediaDuration = be32(mdhd.data + 16);
    }
    if (!findBox(mdia.data, mdia.size, FOURCC('h', 'd', 'l', 'r'), &hdlr) || hdlr.size < 12) return false;
    t->handler = be32(hdlr.data + 8);
    if (!findBox(mdia.data, mdia.size, FOURCC('m', 'i', 'n', 'f'), &minf)) return false;
    if (!findBox(minf.data, minf.size, FOURCC('s', 't', 'b', 'l'), &stbl)) return false;
    if (!findBox(stbl.data, stbl.size, FOURCC('s', 't', 's', 'd'), &t->stsd)) return false;
    if (!findBox(stbl.data, stbl.size, FOURCC('s', 't', 't', 's'), &t->stts)) return false;
    if (!findBox(stbl.data, stbl.size, FOURCC('s', 't', 's', 'c'), &t->stsc)) return false;
    if (!findBox(stbl.data, stbl.size, FOURCC('s', 't', 's', 'z'), &t->stsz)) return false;
    if (!findBox(stbl.data, stbl.size, FOURCC('s', 't', 'c', 'o'), &t->stco)) {
        if (!findBox(stbl.data, stbl.size, FOURCC('c', 'o', '6', '4'), &t->stco)) return false;
        t->co64 = true;
    }
    t->hasCtts = findBox(stbl.data, stbl.size, FOURCC('c', 't', 't', 's'), &t->ctts);
    t->hasStss = findBox(stbl.data, stbl.size, FOURCC('s', 't', 's', 's'), &t->stss);
    return t->timescale != 0;
}

static void freeTrack(CtrMp4Track* track) {
    free(track->offsets);
    free(track->sizes);
    free(track->pts);
    free(track->sync);
    free(track->config);
    memset(track, 0, sizeof(*track));
}

// MPEG-4 descriptor: tag, then a length of up to four 7-bit groups.
static bool readDescriptor(const uint8_t** p, const uint8_t* end, uint32_t* tag, uint32_t* length) {
    if (end - *p < 2) return false;
    *tag = *(*p)++;
    *length = 0;
    for (int i = 0; 4 > i; i++) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        *length = (*length << 7) | (b & 0x7Fu);
        if ((b & 0x80u) == 0) break;
    }
    return *length <= (uint32_t) (end - *p);
}

// The AudioSpecificConfig in an esds box's ES_Descriptor.
static bool parseEsds(const Box* esds, CtrMp4Track* track) {
    if (esds->size < 4) return false;
    const uint8_t* p = esds->data + 4; // version and flags
    const uint8_t* end = esds->data + esds->size;
    uint32_t tag, length;
    if (!readDescriptor(&p, end, &tag, &length) || tag != 0x03 || length < 3) return false;
    end = p + length;
    uint32_t flags = p[2];
    p += 3; // ES_ID, flags
    if (flags & 0x80u) p += 2;                     // dependsOn_ES_ID
    if (flags & 0x40u) p += p < end ? 1u + *p : 0; // URL
    if (flags & 0x20u) p += 2;                     // OCR_ES_Id
    if (p > end || !readDescriptor(&p, end, &tag, &length) || tag != 0x04 || length < 13) return false;
    end = p + length;
    p += 13; // objectTypeIndication, streamType, bufferSizeDB, maxBitrate, avgBitrate
    if (!readDescriptor(&p, end, &tag, &length) || tag != 0x05 || length == 0) return false;
    track->config = malloc(length);
    if (track->config == NULL) return false;
    memcpy(track->config, p, length);
    track->configSize = length;
    return true;
}

// The first sample entry: codec, sizes and codec setup.
static bool parseStsd(const Box* stsd, CtrMp4Track* track) {
    if (stsd->size < 8 || be32(stsd->data + 4) == 0) return false;
    const uint8_t* p = stsd->data + 8;
    Box entry;
    if (!nextBox(&p, stsd->data + stsd->size, &entry)) return false;
    track->codec = entry.type;
    if (entry.type == FOURCC('a', 'v', 'c', '1') || entry.type == FOURCC('a', 'v', 'c', '3')) {
        // VisualSampleEntry: 78 bytes, then its boxes.
        if (entry.size < 78) return false;
        track->width = be16(entry.data + 24);
        track->height = be16(entry.data + 26);
        Box avcC;
        if (!findBox(entry.data + 78, entry.size - 78, FOURCC('a', 'v', 'c', 'C'), &avcC) || avcC.size < 7) return false;
        track->config = malloc(avcC.size);
        if (track->config == NULL) return false;
        memcpy(track->config, avcC.data, avcC.size);
        track->configSize = avcC.size;
        return true;
    }
    if (entry.type == FOURCC('m', 'p', '4', 'a')) {
        // AudioSampleEntry: 28 bytes, then its boxes.
        if (entry.size < 28) return false;
        track->channels = be16(entry.data + 16);
        track->sampleRate = be32(entry.data + 24) >> 16;
        Box esds;
        if (!findBox(entry.data + 28, entry.size - 28, FOURCC('e', 's', 'd', 's'), &esds)) return false;
        return parseEsds(&esds, track);
    }
    return true; // a codec nobody here decodes: indexed all the same
}

static bool buildTrack(const TrakInfo* t, uint32_t movieTimescale, CtrMp4Track* track) {
    memset(track, 0, sizeof(*track));
    if (!parseStsd(&t->stsd, track)) goto fail;
    track->timescale = t->timescale;

    // Sizes.
    if (t->stsz.size < 12) goto fail;
    uint32_t fixedSize = be32(t->stsz.data + 4);
    uint32_t count = be32(t->stsz.data + 8);
    if (fixedSize == 0 && count > (t->stsz.size - 12) / 4) goto fail;
    if (count == 0 || count > 1000000) goto fail;
    track->count = count;
    track->offsets = malloc(count * sizeof(uint64_t));
    track->sizes = malloc(count * sizeof(uint32_t));
    track->pts = malloc(count * sizeof(int64_t));
    track->sync = malloc(count);
    if (track->offsets == NULL || track->sizes == NULL || track->pts == NULL || track->sync == NULL) goto fail;
    for (uint32_t i = 0; count > i; i++) {
        track->sizes[i] = fixedSize != 0 ? fixedSize : be32(t->stsz.data + 12 + 4 * i);
        if (track->sizes[i] > track->maxSize) track->maxSize = track->sizes[i];
    }

    // Offsets: chunks from stco/co64, samples per chunk from stsc.
    if (t->stco.size < 8 || t->stsc.size < 8) goto fail;
    uint32_t chunks = be32(t->stco.data + 4);
    uint32_t chunkEntry = t->co64 ? 8u : 4u;
    if (chunks > (t->stco.size - 8) / chunkEntry) goto fail;
    uint32_t runs = be32(t->stsc.data + 4);
    if (runs == 0 || runs > (t->stsc.size - 8) / 12) goto fail;
    uint32_t sample = 0;
    for (uint32_t r = 0; runs > r && count > sample; r++) {
        const uint8_t* e = t->stsc.data + 8 + 12 * r;
        uint32_t first = be32(e), perChunk = be32(e + 4);
        uint32_t last = r + 1 < runs ? be32(e + 12) : chunks + 1; // one past this run's chunks
        if (first == 0 || last <= first) continue;
        for (uint32_t c = first; last > c && chunks >= c && count > sample; c++) {
            const uint8_t* ce = t->stco.data + 8 + chunkEntry * (c - 1);
            uint64_t offset = t->co64 ? be64(ce) : be32(ce);
            for (uint32_t k = 0; perChunk > k && count > sample; k++, sample++) {
                track->offsets[sample] = offset;
                offset += track->sizes[sample];
            }
        }
    }
    if (sample != count) goto fail;

    // Decode times from stts, presentation times from ctts and the edit list.
    if (t->stts.size < 8) goto fail;
    uint32_t sttsRuns = be32(t->stts.data + 4);
    if (sttsRuns > (t->stts.size - 8) / 8) goto fail;
    int64_t dts = 0;
    sample = 0;
    for (uint32_t r = 0; sttsRuns > r && count > sample; r++) {
        uint32_t n = be32(t->stts.data + 8 + 8 * r), delta = be32(t->stts.data + 12 + 8 * r);
        for (uint32_t k = 0; n > k && count > sample; k++, sample++) {
            track->pts[sample] = dts;
            dts += delta;
        }
    }
    for (; count > sample; sample++) track->pts[sample] = dts; // a short table
    if (t->hasCtts && t->ctts.size >= 8) {
        uint32_t cttsRuns = be32(t->ctts.data + 4);
        if (cttsRuns > (t->ctts.size - 8) / 8) cttsRuns = (t->ctts.size - 8) / 8;
        sample = 0;
        for (uint32_t r = 0; cttsRuns > r && count > sample; r++) {
            uint32_t n = be32(t->ctts.data + 8 + 8 * r);
            // Unsigned in version 0, but encoders write negative ones there too.
            int64_t offset = (int32_t) be32(t->ctts.data + 12 + 8 * r);
            for (uint32_t k = 0; n > k && count > sample; k++, sample++) track->pts[sample] += offset;
        }
    }
    int64_t shift = 0;
    if (t->hasEdit) shift -= t->editMediaTime;
    if (movieTimescale != 0) shift += (int64_t) (t->editDelay * t->timescale / movieTimescale);
    for (uint32_t i = 0; count > i; i++) track->pts[i] += shift;
    if (t->hasEdit && t->editDuration != 0 && movieTimescale != 0) {
        track->duration = (double) (t->editDelay + t->editDuration) / (double) movieTimescale;
    } else {
        track->duration = (double) t->mediaDuration / (double) t->timescale;
    }

    // Sync samples: all of them when there's no stss.
    memset(track->sync, t->hasStss ? 0 : 1, count);
    if (t->hasStss && t->stss.size >= 8) {
        uint32_t n = be32(t->stss.data + 4);
        if (n > (t->stss.size - 8) / 4) n = (t->stss.size - 8) / 4;
        for (uint32_t i = 0; n > i; i++) {
            uint32_t s = be32(t->stss.data + 8 + 4 * i);
            if (s >= 1 && count >= s) track->sync[s - 1] = 1;
        }
    }
    return true;
fail:
    freeTrack(track);
    return false;
}

// ===[ Public API ]===

bool CtrMp4_sniff(const uint8_t* data, uint32_t size) {
    return data != NULL && size >= 8 && memcmp(data + 4, "ftyp", 4) == 0;
}

// Finds the top-level moov box and reads it into memory.
static uint8_t* readMoov(FILE* f, uint32_t* sizeOut) {
    uint64_t pos = 0;
    for (;;) {
        uint8_t header[16];
        if (fseek(f, (long) pos, SEEK_SET) != 0 || fread(header, 1, 8, f) != 8) return NULL;
        uint64_t size = be32(header);
        uint32_t headerSize = 8;
        if (size == 1) {
            if (fread(header + 8, 1, 8, f) != 8) return NULL;
            size = be64(header + 8);
            headerSize = 16;
        }
        if (be32(header + 4) == FOURCC('m', 'o', 'o', 'v')) {
            if (size == 0 || size < headerSize || size - headerSize > 16u * 1024u * 1024u) return NULL;
            uint32_t bytes = (uint32_t) (size - headerSize);
            uint8_t* moov = malloc(bytes);
            if (moov == NULL) return NULL;
            if (fread(moov, 1, bytes, f) != bytes) {
                free(moov);
                return NULL;
            }
            *sizeOut = bytes;
            return moov;
        }
        if (size == 0 || size < headerSize) return NULL; // the last box, and not moov
        pos += size;
    }
}

bool CtrMp4_open(CtrMp4* mp4, const char* path, uint32_t windowSize) {
    memset(mp4, 0, sizeof(*mp4));
    mp4->file = fopen(path, "rb");
    if (mp4->file == NULL) return false;
    // The window does the buffering: reads go straight to the file.
    setvbuf(mp4->file, NULL, _IONBF, 0);
    mp4->window = windowSize != 0 ? malloc(windowSize) : NULL;
    mp4->windowCap = mp4->window != NULL ? windowSize : 0;
    uint8_t head[8];
    uint32_t moovSize = 0;
    uint8_t* moov = NULL;
    if (fread(head, 1, 8, mp4->file) == 8 && CtrMp4_sniff(head, 8)) moov = readMoov(mp4->file, &moovSize);
    if (moov == NULL) {
        CtrMp4_close(mp4);
        return false;
    }
    uint32_t movieTimescale = 0;
    Box mvhd;
    if (findBox(moov, moovSize, FOURCC('m', 'v', 'h', 'd'), &mvhd) && mvhd.size >= 20) {
        if (mvhd.data[0] == 1 && mvhd.size >= 32) {
            movieTimescale = be32(mvhd.data + 20);
            if (movieTimescale != 0) mp4->duration = (double) be64(mvhd.data + 24) / (double) movieTimescale;
        } else {
            movieTimescale = be32(mvhd.data + 12);
            if (movieTimescale != 0) mp4->duration = (double) be32(mvhd.data + 16) / (double) movieTimescale;
        }
    }
    const uint8_t* p = moov;
    Box box;
    while (nextBox(&p, moov + moovSize, &box)) {
        if (box.type != FOURCC('t', 'r', 'a', 'k')) continue;
        TrakInfo t;
        if (!parseTrak(&box, &t)) continue;
        CtrMp4Track* track = NULL;
        if (t.handler == FOURCC('v', 'i', 'd', 'e') && mp4->video.count == 0) track = &mp4->video;
        if (t.handler == FOURCC('s', 'o', 'u', 'n') && mp4->audio.count == 0) track = &mp4->audio;
        if (track != NULL) buildTrack(&t, movieTimescale, track);
    }
    free(moov);
    if (mp4->video.count == 0 && mp4->audio.count == 0) {
        CtrMp4_close(mp4);
        return false;
    }
    return true;
}

void CtrMp4_close(CtrMp4* mp4) {
    freeTrack(&mp4->video);
    freeTrack(&mp4->audio);
    if (mp4->file != NULL) fclose(mp4->file);
    free(mp4->window);
    mp4->file = NULL;
    mp4->window = NULL;
    mp4->windowCap = mp4->windowLen = 0;
}

bool CtrMp4_readSample(CtrMp4* mp4, const CtrMp4Track* track, uint32_t index, void* out) {
    if (mp4->file == NULL || index >= track->count) return false;
    uint64_t offset = track->offsets[index];
    uint32_t size = track->sizes[index];
    if (offset >= mp4->windowStart && offset + size <= mp4->windowStart + mp4->windowLen) {
        memcpy(out, mp4->window + (offset - mp4->windowStart), size);
        return true;
    }
    if (fseek(mp4->file, (long) offset, SEEK_SET) != 0) return false;
    if (size > mp4->windowCap) return fread(out, 1, size, mp4->file) == size;
    // The window moves on to start at this sample.
    mp4->windowStart = offset;
    mp4->windowLen = (uint32_t) fread(mp4->window, 1, mp4->windowCap, mp4->file);
    if (mp4->windowLen < size) return false;
    memcpy(out, mp4->window, size);
    return true;
}
