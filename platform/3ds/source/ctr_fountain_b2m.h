// Cached source-patch state. New uses it when the source is resident;
// Old remains disabled pending its memory decision.
// This path allocates no GML-visible handles or shader state.
#ifndef CTRPORT_CTR_FOUNTAIN_B2M_H
#define CTRPORT_CTR_FOUNTAIN_B2M_H

typedef struct {
    bool checked, enabled, active, maskReady, patchReady;
    C3D_Tex mask, patch;
    CtrPage* page;
    CtrTile* tiles;
    int32_t region[4], grid[4];
    struct {
        void* data;
        uint32_t checksum;
        GPU_TEXCOLOR format;
        bool valid;
    } source[4];
    float tc2[4];
    uint32_t builds, fallbacks, vertices, batches;
    uint64_t creationTicks;
} CtrFountainB2m;

#endif
