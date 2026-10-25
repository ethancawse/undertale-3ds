#include "ctr_audio_decoder.h"

#include <stdlib.h>
#include <string.h>

#include "ctr_mp4.h"
// The API header expects this from PacketVideo's OSCL library.
#define OSCL_IMPORT_REF
#include "pvmp4audiodecoder_api.h"

#ifndef CTRPORT_HAVE_TREMOR
// Fallback when the devkitPro 3ds-libvorbisidec package is not installed. It
// works, but stb_vorbis decodes in floating point and is noticeably slower than
// Tremor on the ARM11.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshadow"
#include "stb_vorbis.c"
#pragma GCC diagnostic pop
#endif

#define FILE_BUFFER_SIZE (16 * 1024)

// ===[ Byte source ]===

static size_t srcRead(CtrDecoder* d, void* dst, size_t n) {
    if (d->readAhead != NULL || d->file != NULL) {
        if (d->sectioned && n > d->secSize - d->secPos) n = d->secSize - d->secPos;
        size_t got = d->readAhead != NULL ? CtrReadAhead_read(d->readAhead, dst, n) : fread(dst, 1, n, d->file);
        d->secPos += (uint32_t) got;
        return got;
    }
    size_t avail = d->memSize - d->memPos;
    if (n > avail) n = avail;
    memcpy(dst, d->mem + d->memPos, n);
    d->memPos += (uint32_t) n;
    return n;
}

static bool srcSeek(CtrDecoder* d, long offset, int whence) {
    if ((d->readAhead != NULL || d->file != NULL) && d->sectioned) {
        long base = whence == SEEK_CUR ? (long) d->secPos : whence == SEEK_END ? (long) d->secSize : 0;
        long target = base + offset;
        if (target < 0 || target > (long) d->secSize) return false;
        long absolute = d->secBase + target;
        bool ok = d->readAhead != NULL ? CtrReadAhead_seek(d->readAhead, absolute, SEEK_SET) : fseek(d->file, absolute, SEEK_SET) == 0;
        if (ok) d->secPos = (uint32_t) target;
        return ok;
    }
    if (d->readAhead != NULL) return CtrReadAhead_seek(d->readAhead, offset, whence);
    if (d->file != NULL) return fseek(d->file, offset, whence) == 0;
    long base = 0;
    if (whence == SEEK_CUR) base = (long) d->memPos;
    else if (whence == SEEK_END) base = (long) d->memSize;
    long target = base + offset;
    if (target < 0 || target > (long) d->memSize) return false;
    d->memPos = (uint32_t) target;
    return true;
}

static long srcTell(CtrDecoder* d) {
    if ((d->readAhead != NULL || d->file != NULL) && d->sectioned) return (long) d->secPos;
    if (d->readAhead != NULL) return CtrReadAhead_tell(d->readAhead);
    if (d->file != NULL) return ftell(d->file);
    return (long) d->memPos;
}

static uint32_t readLe32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t readLe16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

// ===[ Sections ]===

#define SECTION_PREFIX "section:"

char* CtrDecoder_sectionPath(const char* path, uint32_t offset, uint32_t size, bool sizeBefore) {
    size_t length = strlen(path) + 48;
    char* spec = (char*) malloc(length);
    if (spec != NULL) snprintf(spec, length, SECTION_PREFIX "%lu:%lu:%d:%s", (unsigned long) offset, (unsigned long) size, sizeBefore ? 1 : 0, path);
    return spec;
}

// Opens the file a path or section names (with buffer, if not NULL, as its
// stdio buffer). A section is left at its start with *base and *size set.
static FILE* openSource(const char* spec, char* buffer, bool* sectioned, long* base, uint32_t* size) {
    *sectioned = false;
    *base = 0;
    *size = 0;
    const size_t prefix = sizeof(SECTION_PREFIX) - 1;
    bool section = strncmp(spec, SECTION_PREFIX, prefix) == 0;
    unsigned long offset = 0, length = 0;
    int before = 0, consumed = 0;
    if (section && (sscanf(spec + prefix, "%lu:%lu:%d:%n", &offset, &length, &before, &consumed) != 3 || consumed == 0)) return NULL;
    FILE* f = fopen(section ? spec + prefix + consumed : spec, "rb");
    if (f == NULL) return NULL;
    if (buffer != NULL) setvbuf(f, buffer, _IOFBF, FILE_BUFFER_SIZE);
    if (!section) return f;
    if (before) {
        uint8_t word[4];
        if (offset < 4 || fseek(f, (long) offset - 4, SEEK_SET) != 0 || fread(word, 1, 4, f) != 4) {
            fclose(f);
            return NULL;
        }
        uint32_t exact = readLe32(word);
        if (exact < length) length = exact;
    }
    if (fseek(f, (long) offset, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    *sectioned = true;
    *base = (long) offset;
    *size = (uint32_t) length;
    return f;
}

// ===[ WAV ]===

static bool wavOpen(CtrDecoder* d) {
    uint8_t header[12];
    if (!srcSeek(d, 0, SEEK_SET) || srcRead(d, header, 12) != 12) return false;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return false;

    uint16_t formatTag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool haveFormat = false;
    for (;;) {
        uint8_t chunk[8];
        if (srcRead(d, chunk, 8) != 8) return false;
        uint32_t length = readLe32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0 && length >= 16) {
            uint8_t fmt[16];
            if (srcRead(d, fmt, 16) != 16) return false;
            formatTag = readLe16(fmt);
            channels = readLe16(fmt + 2);
            rate = readLe32(fmt + 4);
            bits = readLe16(fmt + 14);
            haveFormat = true;
            if (!srcSeek(d, (long) (length - 16 + (length & 1)), SEEK_CUR)) return false;
        } else if (memcmp(chunk, "data", 4) == 0) {
            d->wavDataStart = (uint32_t) srcTell(d);
            d->wavDataBytes = length;
            uint32_t sourceBytes = d->file == NULL ? d->memSize : d->sectioned ? d->secSize : UINT32_MAX;
            if (d->wavDataBytes > sourceBytes - d->wavDataStart) d->wavDataBytes = sourceBytes - d->wavDataStart;
            break;
        } else if (!srcSeek(d, (long) (length + (length & 1)), SEEK_CUR)) {
            return false;
        }
    }

    if (!haveFormat || formatTag != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16) || rate == 0) return false;
    d->kind = CTR_DECODER_WAV;
    d->channels = channels;
    d->sampleRate = rate;
    d->wavBits = bits;
    d->totalFrames = d->wavDataBytes / (channels * (bits / 8u));
    return true;
}

static int32_t wavRead(CtrDecoder* d, int16_t* out, uint32_t maxFrames) {
    uint64_t remaining = d->totalFrames - d->positionFrames;
    uint32_t frames = maxFrames < remaining ? maxFrames : (uint32_t) remaining;
    if (frames == 0) return 0;
    uint32_t samples = frames * d->channels;

    uint32_t got;
    if (d->wavBits == 16) {
        got = (uint32_t) srcRead(d, out, samples * 2u) / 2u;
    } else {
        // Read the unsigned bytes into the top half of the output and widen in
        // place; walking forward never overwrites a byte that is still unread.
        uint8_t* bytes = (uint8_t*) out + samples;
        got = (uint32_t) srcRead(d, bytes, samples);
        for (uint32_t i = 0; i < got; i++) out[i] = (int16_t) (((int) bytes[i] - 128) << 8);
    }
    uint32_t gotFrames = got / d->channels;
    d->positionFrames += gotFrames;
    return (int32_t) gotFrames;
}

static bool wavSeek(CtrDecoder* d, uint64_t frame) {
    if (frame > d->totalFrames) frame = d->totalFrames;
    uint32_t frameBytes = d->channels * (d->wavBits / 8u);
    if (!srcSeek(d, (long) (d->wavDataStart + frame * frameBytes), SEEK_SET)) return false;
    d->positionFrames = frame;
    return true;
}

// ===[ Vorbis ]===

#ifdef CTRPORT_HAVE_TREMOR

static size_t tremorRead(void* ptr, size_t size, size_t nmemb, void* source) {
    if (size == 0) return 0;
    return srcRead((CtrDecoder*) source, ptr, size * nmemb) / size;
}

static int tremorSeek(void* source, ogg_int64_t offset, int whence) {
    return srcSeek((CtrDecoder*) source, (long) offset, whence) ? 0 : -1;
}

static int tremorClose(void* source) {
    (void) source; // The FILE is closed by CtrDecoder_close.
    return 0;
}

static long tremorTell(void* source) {
    return srcTell((CtrDecoder*) source);
}

static bool vorbisOpen(CtrDecoder* d, const uint8_t* initial, size_t initialBytes) {
    ov_callbacks callbacks = { tremorRead, tremorSeek, tremorClose, tremorTell };
    // Feed the format probe to Tremor instead of seeking back over it. Newlib
    // can discard the stdio buffer on rewind, costing another 16 KB SD read.
    // The source is already positioned immediately after initial, including
    // for sections and memory sources.
    if (ov_open_callbacks(d, &d->vf, (const char*) initial, (long) initialBytes, callbacks) != 0) return false;
    d->vorbisOpen = true;
    vorbis_info* info = ov_info(&d->vf, -1);
    if (info == NULL || info->channels < 1 || info->channels > 2 || info->rate <= 0) return false;
    d->kind = CTR_DECODER_VORBIS;
    d->channels = (uint32_t) info->channels;
    d->sampleRate = (uint32_t) info->rate;
    ogg_int64_t total = ov_pcm_total(&d->vf, -1);
    d->totalFrames = total > 0 ? (uint64_t) total : 0;
    return true;
}

static int32_t vorbisRead(CtrDecoder* d, int16_t* out, uint32_t maxFrames) {
    int bytesWanted = (int) (maxFrames * d->channels * sizeof(int16_t));
    for (int attempt = 0; attempt < 8; attempt++) {
        int bitstream = 0;
        long got = ov_read(&d->vf, (char*) out, bytesWanted, &bitstream);
        if (got == OV_HOLE) continue; // Gap in the data; decoding resumes after it.
        if (got < 0) return -1;
        int32_t frames = (int32_t) (got / (long) (d->channels * sizeof(int16_t)));
        d->positionFrames += (uint64_t) frames;
        return frames;
    }
    return -1;
}

static bool vorbisSeek(CtrDecoder* d, uint64_t frame) {
    if (ov_pcm_seek(&d->vf, (ogg_int64_t) frame) != 0) {
        if (frame != 0 || ov_raw_seek(&d->vf, 0) != 0) return false;
    }
    d->positionFrames = frame;
    return true;
}

static void vorbisClose(CtrDecoder* d) {
    if (d->vorbisOpen) ov_clear(&d->vf);
    d->vorbisOpen = false;
}

const char* CtrDecoder_vorbisBackendName(void) {
    return "Tremor";
}

#else // stb_vorbis

static bool vorbisOpen(CtrDecoder* d, const uint8_t* initial, size_t initialBytes) {
    (void) initial;
    (void) initialBytes;
    int error = 0;
    if (d->file != NULL) {
        srcSeek(d, 0, SEEK_SET);
        // stb_vorbis reads the FILE itself; a section starts where the FILE is.
        if (d->sectioned) d->stb = stb_vorbis_open_file_section(d->file, 0, &error, NULL, d->secSize);
        else d->stb = stb_vorbis_open_file(d->file, 0, &error, NULL);
    } else {
        d->stb = stb_vorbis_open_memory(d->mem, (int) d->memSize, &error, NULL);
    }
    if (d->stb == NULL) return false;
    d->vorbisOpen = true;
    stb_vorbis_info info = stb_vorbis_get_info(d->stb);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0) return false;
    d->kind = CTR_DECODER_VORBIS;
    d->channels = (uint32_t) info.channels;
    d->sampleRate = info.sample_rate;
    d->totalFrames = stb_vorbis_stream_length_in_samples(d->stb);
    return true;
}

static int32_t vorbisRead(CtrDecoder* d, int16_t* out, uint32_t maxFrames) {
    int frames = stb_vorbis_get_samples_short_interleaved(d->stb, (int) d->channels, out, (int) (maxFrames * d->channels));
    if (frames < 0) return -1;
    d->positionFrames += (uint64_t) frames;
    return frames;
}

static bool vorbisSeek(CtrDecoder* d, uint64_t frame) {
    int ok = frame == 0 ? stb_vorbis_seek_start(d->stb) : stb_vorbis_seek(d->stb, (unsigned int) frame);
    if (!ok) return false;
    d->positionFrames = frame;
    return true;
}

static void vorbisClose(CtrDecoder* d) {
    if (d->stb != NULL) stb_vorbis_close(d->stb);
    d->stb = NULL;
    d->vorbisOpen = false;
}

const char* CtrDecoder_vorbisBackendName(void) {
    return "stb_vorbis";
}

#endif

// ===[ MP4 (AAC) ]===

// Samples are read through a window this large (see CtrMp4): the Chapter 3
// video's AAC frames sit between its video frames, a few KB apart.
#define AAC_WINDOW_BYTES (64 * 1024)
// An AAC-LC frame's samples per channel.
#define AAC_FRAME 1024

struct CtrMp4Aac {
    CtrMp4 mp4;
    tPVMP4AudioDecoderExternal ext;
    void* mem;
    uint8_t* sample;
    uint32_t next; // the next sample to decode
    // The frames of the last decoded sample not yet read (stereo).
    int16_t pcm[AAC_FRAME * 2];
    uint32_t pcmPos, pcmCount;
};

// A presentation time in output frames.
static int64_t aacFrames(const CtrMp4Track* t, uint32_t sampleRate, int64_t ticks) {
    if (t->timescale == sampleRate) return ticks;
    return ticks * (int64_t) sampleRate / (int64_t) t->timescale;
}

// The output frames the track holds, priming ones (before time 0) left out.
static uint64_t aacTotalFrames(const CtrMp4Track* t, uint32_t sampleRate) {
    int64_t start = aacFrames(t, sampleRate, t->pts[0]);
    int64_t total = (int64_t) t->count * AAC_FRAME + (start < 0 ? start : 0);
    return total > 0 ? (uint64_t) total : 0;
}

static void aacFree(CtrMp4Aac* a) {
    if (a == NULL) return;
    CtrMp4_close(&a->mp4);
    free(a->mem);
    free(a->sample);
    free(a);
}

static bool mp4Open(CtrDecoder* d, const char* path) {
    CtrMp4Aac* a = (CtrMp4Aac*) calloc(1, sizeof(CtrMp4Aac));
    if (a == NULL) return false;
    if (!CtrMp4_open(&a->mp4, path, AAC_WINDOW_BYTES)) {
        free(a);
        return false;
    }
    d->aac = a;
    const CtrMp4Track* t = &a->mp4.audio;
    if (t->count == 0 || t->codec != CTR_MP4_FOURCC('m', 'p', '4', 'a') || t->configSize < 2) return false;
    a->mem = malloc(PVMP4AudioDecoderGetMemRequirements());
    a->sample = (uint8_t*) malloc(t->maxSize != 0 ? t->maxSize : 1);
    if (a->mem == NULL || a->sample == NULL) return false;
    a->ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
    a->ext.desiredChannels = 2; // mono comes out on both
    a->ext.aacPlusEnabled = false;
    if (PVMP4AudioDecoderInitLibrary(&a->ext, a->mem) != 0) return false;
    a->ext.pInputBuffer = t->config;
    a->ext.inputBufferCurrentLength = (Int) t->configSize;
    a->ext.inputBufferMaxLength = 0;
    if (PVMP4AudioDecoderConfig(&a->ext, a->mem) != MP4AUDEC_SUCCESS || a->ext.samplingRate <= 0) return false;
    d->kind = CTR_DECODER_MP4;
    d->channels = 2;
    d->sampleRate = (uint32_t) a->ext.samplingRate;
    d->totalFrames = aacTotalFrames(t, d->sampleRate);
    return true;
}

// Decodes sample a->next into a->pcm. A frame that fails to decode comes out
// silent, so what follows keeps its time. False at the end of the track.
static bool aacDecodeNext(CtrDecoder* d) {
    CtrMp4Aac* a = d->aac;
    const CtrMp4Track* t = &a->mp4.audio;
    if (a->next >= t->count) return false;
    uint32_t index = a->next++;
    bool ok = CtrMp4_readSample(&a->mp4, t, index, a->sample);
    if (ok) {
        a->ext.pInputBuffer = a->sample;
        a->ext.inputBufferCurrentLength = (Int) t->sizes[index];
        a->ext.inputBufferMaxLength = 0;
        a->ext.inputBufferUsedLength = 0;
        a->ext.remainderBits = 0;
        a->ext.pOutputBuffer = a->pcm;
        a->ext.pOutputBuffer_plus = NULL;
        a->ext.repositionFlag = false;
        ok = PVMP4AudioDecodeFrame(&a->ext, a->mem) == MP4AUDEC_SUCCESS && a->ext.frameLength == AAC_FRAME;
    }
    if (!ok) memset(a->pcm, 0, sizeof(a->pcm));
    // Priming frames, before time 0, are left out.
    int64_t start = aacFrames(t, d->sampleRate, t->pts[index]);
    a->pcmPos = start < 0 ? (uint32_t) (-start < AAC_FRAME ? -start : AAC_FRAME) : 0;
    a->pcmCount = AAC_FRAME;
    return true;
}

static int32_t mp4Read(CtrDecoder* d, int16_t* out, uint32_t maxFrames) {
    CtrMp4Aac* a = d->aac;
    while (a->pcmPos >= a->pcmCount) {
        if (!aacDecodeNext(d)) return 0;
    }
    uint32_t frames = a->pcmCount - a->pcmPos;
    if (frames > maxFrames) frames = maxFrames;
    memcpy(out, a->pcm + a->pcmPos * 2u, frames * 2u * sizeof(int16_t));
    a->pcmPos += frames;
    d->positionFrames += frames;
    return (int32_t) frames;
}

static bool mp4Seek(CtrDecoder* d, uint64_t frame) {
    CtrMp4Aac* a = d->aac;
    const CtrMp4Track* t = &a->mp4.audio;
    if (frame > d->totalFrames) frame = d->totalFrames;
    // The sample holding the frame: the last one starting at or before it.
    uint32_t index = 0;
    while (index + 1 < t->count && aacFrames(t, d->sampleRate, t->pts[index + 1]) <= (int64_t) frame) index++;
    PVMP4AudioDecoderResetBuffer(a->mem);
    // A frame's second half is only right after the one before it has been
    // decoded (the transform overlaps them), so that one is decoded first.
    a->next = index > 0 ? index - 1 : 0;
    if (index > 0) aacDecodeNext(d);
    a->pcmPos = a->pcmCount = 0;
    if (aacDecodeNext(d)) {
        int64_t into = (int64_t) frame - aacFrames(t, d->sampleRate, t->pts[index]);
        if (into > (int64_t) a->pcmPos) a->pcmPos = into < AAC_FRAME ? (uint32_t) into : AAC_FRAME;
    }
    d->positionFrames = frame;
    return true;
}

// ===[ Probing ]===

// Ogg page header: "OggS", version, flags, granule (8), serial (4), sequence
// (4), CRC (4), segment count, then the segment table.
#define OGG_PAGE_HEADER_BYTES 27
// Bytes read from the end of a file to find its last page; a page is at most
// 65307 bytes, and real ones are a few KB, so try a small tail first.
#define PROBE_TAIL_SMALL (8 * 1024)
#define PROBE_TAIL_LARGE (66 * 1024)
#define PROBE_HEAD_BYTES 512

static uint64_t readLe64(const uint8_t* p) {
    return (uint64_t) readLe32(p) | ((uint64_t) readLe32(p + 4) << 32);
}

// Channels and rate from the identification header on the first page.
static bool oggProbeHead(const uint8_t* head, uint32_t size, CtrAudioInfo* info, uint32_t* serial) {
    if (size < OGG_PAGE_HEADER_BYTES || memcmp(head, "OggS", 4) != 0) return false;
    uint32_t segments = head[26];
    uint32_t packet = OGG_PAGE_HEADER_BYTES + segments;
    // Packet type 1, "vorbis", version (4), channels (1), rate (4).
    if (packet + 16 > size || head[packet] != 1 || memcmp(head + packet + 1, "vorbis", 6) != 0) return false;
    uint32_t channels = head[packet + 11];
    uint32_t rate = readLe32(head + packet + 12);
    if (channels < 1 || channels > 2 || rate == 0) return false;
    info->kind = CTR_DECODER_VORBIS;
    info->channels = channels;
    info->sampleRate = rate;
    *serial = readLe32(head + 14);
    return true;
}

// The granule position of the last page of the stream in tail, which is the
// total frame count for a stream that starts at zero (as encoders write them).
static bool oggProbeTail(const uint8_t* tail, uint32_t size, uint32_t serial, uint64_t* frames) {
    if (size < OGG_PAGE_HEADER_BYTES) return false;
    for (uint32_t i = size - OGG_PAGE_HEADER_BYTES + 1; i-- > 0;) {
        const uint8_t* p = tail + i;
        if (p[0] != 'O' || memcmp(p, "OggS", 4) != 0 || p[4] != 0 || readLe32(p + 14) != serial) continue;
        uint64_t granule = readLe64(p + 6);
        if (granule == UINT64_MAX) continue; // no packet ends on this page
        *frames = granule;
        return true;
    }
    return false;
}

CtrDecoderKind CtrDecoder_sniff(const uint8_t* data, uint32_t size) {
    if (data == NULL || size < 4) return CTR_DECODER_NONE;
    if (memcmp(data, "OggS", 4) == 0) return CTR_DECODER_VORBIS;
    if (memcmp(data, "RIFF", 4) == 0) return CTR_DECODER_WAV;
    if (CtrMp4_sniff(data, size)) return CTR_DECODER_MP4;
    return CTR_DECODER_NONE;
}

static void infoFromDecoder(const CtrDecoder* dec, CtrAudioInfo* info) {
    info->kind = dec->kind;
    info->channels = dec->channels;
    info->sampleRate = dec->sampleRate;
    info->totalFrames = dec->totalFrames;
}

bool CtrDecoder_probeMemory(const uint8_t* data, uint32_t size, CtrAudioInfo* info) {
    memset(info, 0, sizeof(*info));
    info->sourceBytes = size;
    CtrDecoderKind kind = CtrDecoder_sniff(data, size);
    if (kind == CTR_DECODER_WAV) {
        CtrDecoder dec;
        if (!CtrDecoder_openMemory(&dec, data, size)) return false;
        infoFromDecoder(&dec, info);
        CtrDecoder_close(&dec);
        return true;
    }
    if (kind != CTR_DECODER_VORBIS) return false;
    uint32_t serial = 0;
    if (!oggProbeHead(data, size, info, &serial)) return false;
    uint32_t tail = size < PROBE_TAIL_LARGE ? size : PROBE_TAIL_LARGE;
    if (!oggProbeTail(data + size - tail, tail, serial, &info->totalFrames)) info->totalFrames = 0;
    return true;
}

bool CtrDecoder_probeFile(const char* path, CtrAudioInfo* info) {
    memset(info, 0, sizeof(*info));
    bool sectioned;
    long base;
    uint32_t sectionSize;
    FILE* f = openSource(path, NULL, &sectioned, &base, &sectionSize);
    if (f == NULL) return false;
    uint8_t head[PROBE_HEAD_BYTES];
    size_t headBytes = fread(head, 1, sectioned && sectionSize < sizeof(head) ? sectionSize : sizeof(head), f);
    long fileSize = sectioned ? (long) sectionSize : fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    CtrDecoderKind kind = CtrDecoder_sniff(head, (uint32_t) headBytes);
    bool ok = false;
    if (fileSize > 0 && kind == CTR_DECODER_VORBIS) {
        uint32_t serial = 0;
        ok = oggProbeHead(head, (uint32_t) headBytes, info, &serial);
        uint8_t* tail = ok ? (uint8_t*) malloc(PROBE_TAIL_LARGE) : NULL;
        bool found = false;
        for (int pass = 0; tail != NULL && pass < 2 && !found; pass++) {
            long want = pass == 0 ? PROBE_TAIL_SMALL : PROBE_TAIL_LARGE;
            if (want > fileSize) want = fileSize;
            if (fseek(f, base + fileSize - want, SEEK_SET) != 0) break;
            size_t got = fread(tail, 1, (size_t) want, f);
            found = oggProbeTail(tail, (uint32_t) got, serial, &info->totalFrames);
            if (want == fileSize) break;
        }
        free(tail);
    }
    fclose(f);
    if (kind == CTR_DECODER_MP4) {
        // The index alone (no sample window, no decoder).
        CtrMp4 mp4;
        if (!CtrMp4_open(&mp4, path, 0)) return false;
        const CtrMp4Track* t = &mp4.audio;
        ok = t->count != 0 && t->codec == CTR_MP4_FOURCC('m', 'p', '4', 'a') && t->sampleRate != 0;
        if (ok) {
            info->kind = CTR_DECODER_MP4;
            info->channels = 2;
            info->sampleRate = t->sampleRate;
            info->totalFrames = aacTotalFrames(t, t->sampleRate);
        }
        CtrMp4_close(&mp4);
    }
    if (kind == CTR_DECODER_WAV) {
        CtrDecoder dec;
        if (!CtrDecoder_openFile(&dec, path)) return false;
        infoFromDecoder(&dec, info);
        CtrDecoder_close(&dec);
        ok = true;
    }
    if (ok) info->sourceBytes = fileSize > 0 ? (uint32_t) fileSize : 0;
    return ok;
}

// ===[ Public API ]===

static bool openCommon(CtrDecoder* d, const char* path) {
    uint8_t head[8];
    size_t headBytes = srcRead(d, head, sizeof(head));
    bool ok = false;
    if (headBytes >= 4) {
        if (memcmp(head, "RIFF", 4) == 0) ok = wavOpen(d);
        else if (memcmp(head, "OggS", 4) == 0) ok = vorbisOpen(d, head, headBytes);
        else if (path != NULL && !d->sectioned && CtrMp4_sniff(head, (uint32_t) headBytes)) {
            // CtrMp4 reads the file through its own handle.
            fclose(d->file);
            free(d->fileBuffer);
            d->file = NULL;
            d->fileBuffer = NULL;
            ok = mp4Open(d, path);
        }
    }
    if (!ok) CtrDecoder_close(d);
    return ok;
}

bool CtrDecoder_openFile(CtrDecoder* d, const char* path) {
    memset(d, 0, sizeof(*d));
    d->fileBuffer = (char*) malloc(FILE_BUFFER_SIZE);
    d->file = openSource(path, d->fileBuffer, &d->sectioned, &d->secBase, &d->secSize);
    if (d->file == NULL) {
        free(d->fileBuffer);
        d->fileBuffer = NULL;
        return false;
    }
    return openCommon(d, path);
}

bool CtrDecoder_openMemory(CtrDecoder* d, const uint8_t* data, uint32_t size) {
    memset(d, 0, sizeof(*d));
    if (data == NULL || size == 0) return false;
    d->mem = data;
    d->memSize = size;
    return openCommon(d, NULL);
}

bool CtrDecoder_startReadAhead(CtrDecoder* d) {
    if (d->file == NULL || d->readAhead != NULL || d->kind == CTR_DECODER_NONE) return false;
#ifndef CTRPORT_HAVE_TREMOR
    if (d->kind == CTR_DECODER_VORBIS) return false; // stb_vorbis reads the FILE itself
#endif
    CtrReadAhead* ra = CtrReadAhead_attachLate(d->file, d->fileBuffer, ftell(d->file));
    if (ra == NULL) return false;
    d->readAhead = ra;
    d->file = NULL;
    d->fileBuffer = NULL;
    return true;
}

int32_t CtrDecoder_read(CtrDecoder* d, int16_t* out, uint32_t maxFrames) {
    if (maxFrames == 0) return 0;
    switch (d->kind) {
        case CTR_DECODER_WAV: return wavRead(d, out, maxFrames);
        case CTR_DECODER_VORBIS: return vorbisRead(d, out, maxFrames);
        case CTR_DECODER_MP4: return mp4Read(d, out, maxFrames);
        default: return -1;
    }
}

bool CtrDecoder_seek(CtrDecoder* d, uint64_t frame) {
    switch (d->kind) {
        case CTR_DECODER_WAV: return wavSeek(d, frame);
        case CTR_DECODER_VORBIS: return vorbisSeek(d, frame);
        case CTR_DECODER_MP4: return mp4Seek(d, frame);
        default: return false;
    }
}

void CtrDecoder_close(CtrDecoder* d) {
    vorbisClose(d);
    aacFree(d->aac);
    d->aac = NULL;
    CtrReadAhead_detach(d->readAhead); // closes the file it took over
    d->readAhead = NULL;
    if (d->file != NULL) fclose(d->file);
    free(d->fileBuffer);
    d->file = NULL;
    d->fileBuffer = NULL;
    d->mem = NULL;
    d->kind = CTR_DECODER_NONE;
}
