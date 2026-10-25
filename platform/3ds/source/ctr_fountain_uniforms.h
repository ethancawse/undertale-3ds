#ifndef CTRPORT_CTR_FOUNTAIN_UNIFORMS_H
#define CTRPORT_CTR_FOUNTAIN_UNIFORMS_H
#include <stdint.h>

// Four values per shader in a namespace disjoint from base uniform indices
// and sampler slots. Slot 3 is reserved and never issued. No room/object keys.
#define FOUNTAIN_HANDLE_FIRST 0x4000
#define FOUNTAIN_HANDLE_SLOTS 4
#define FOUNTAIN_HANDLE_SHADERS 256
#define FOUNTAIN_HANDLE_LIMIT (FOUNTAIN_HANDLE_FIRST + FOUNTAIN_HANDLE_SLOTS * FOUNTAIN_HANDLE_SHADERS)

typedef struct CtrFountainUniforms {
    struct CtrFountainUniforms* next;
    int32_t shader;
    uint32_t issued;
    float values[3][4];
} CtrFountainUniforms;

#endif
