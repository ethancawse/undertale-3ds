// Existing feeder state, in the baseline object's exact allocation order.
// New call edges can alter GCC's varpool walk. no_reorder keeps these static
// definitions and alignments deterministic without changing their C linkage.
// The music layout verifier pins every original symbol and section extent.
#define MM_STATE UTA_BSS __attribute__((no_reorder))
static volatile bool utaMapped MM_STATE;
static int8_t utaMap[UTA_CHANNELS] MM_STATE;
static CtrAudioSystem* utaCa MM_STATE;
static UtaSeamSound utaSeamSounds[UTA_SEAM_SOUNDS] MM_STATE;
static uint8_t utaSeamNext MM_STATE;
static void (*utaPost)(CtrAudioSystem*, const char*, ...) MM_STATE;
static UtaSeam utaSeams[STREAM_CONTEXTS] MM_STATE;
static UtaChan utaChan[UTA_CHANNELS] MM_STATE;
static u64 utaLastLog MM_STATE;
static uint32_t utaTails MM_STATE;
static uint32_t utaLoggedTails MM_STATE;
static LightLock utaLock MM_STATE;
static uint32_t utaCbCount MM_STATE;
static u64 utaCbTicks MM_STATE;
static u64 utaCbMaxTicks MM_STATE;
static uint8_t utaPeak MM_STATE;
static uint8_t utaPeakHeld MM_STATE;
static uint8_t utaPeakAll MM_STATE;
static uint32_t utaYields MM_STATE;
static uint32_t utaPieces MM_STATE;
static uint32_t utaStarved MM_STATE;
static uint32_t utaHard MM_STATE;
static uint32_t utaUnstarted MM_STATE;
static uint32_t utaMoves MM_STATE;
static volatile bool utaActive MM_STATE;
static uint8_t* utaSeamPcm MM_STATE;
static uint8_t* utaTailPcm MM_STATE;
static uint32_t utaCb MM_STATE;
static int8_t utaInv[UTA_CHANNELS] MM_STATE;
static ndspCallback utaEngineCallback MM_STATE;
static ndspWaveBuf utaWb[UTA_CHANNELS][UTA_RING] MM_STATE;
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
static uint32_t utaProbe MM_STATE;
#endif
#undef MM_STATE
