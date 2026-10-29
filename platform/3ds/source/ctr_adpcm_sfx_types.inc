// Shared resident SFX types; no engine ABI changes.
// A literal table keeps new C references out of GCC's original varpool walk.
// Direct references from these new bodies reordered existing utaudio BSS.
// These exact private symbols already remain live through the original
// feeder. The linker resolves them; the layout proof checks their targets.
typedef struct {
    UtaChan* channels;
    ndspWaveBuf (*buffers)[UTA_RING];
    uint8_t** tail;
    uint8_t** seam;
    LightLock* lock;
    volatile bool* active;
} AdSfxView;
extern const AdSfxView CtrAdpcm_sfxView;
#ifndef CTR_ADPCM_HOST
__asm__(".pushsection .rodata.late.adpcm,\"a\",%progbits\n"
        ".p2align 2\n.global CtrAdpcm_sfxView\n"
        ".type CtrAdpcm_sfxView,%object\nCtrAdpcm_sfxView:\n"
        ".word utaChan, utaWb, utaTailPcm, utaSeamPcm, utaLock, utaActive\n"
        ".size CtrAdpcm_sfxView, .-CtrAdpcm_sfxView\n.popsection\n");
#endif

typedef struct AdSfxMeta {
    struct AdSfxMeta* next;
    CachedSfx* cache;
    uint8_t* data;
    CtrAdpcmPcm initial;
    UtaSeamSound seam;
    int16_t first[2], prev[2], last[2];
    uint32_t originalBytes, originalLinearBytes;
    uint64_t scanTicks;
} AdSfxMeta;

typedef struct AdSfxRing {
    struct AdSfxRing* next;
    uint8_t* data;
    uint32_t frames, bytes;
} AdSfxRing;

typedef struct {
    AdSfxMeta* meta;
    CtrAdpcmPcm cursor;
    uint32_t start[2];
    AdSfxRing* ring;
    bool tailReady;
} AdSfxPhysical;

