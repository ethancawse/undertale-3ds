// Private binding between the renderer's existing storage and the separate frame gate.
#ifndef CTR_BONEGATE_H
#define CTR_BONEGATE_H
#include "renderer.h"
#include "runner.h"
#define BONEGATE_OBJECTS 6
typedef void (*BoneGateDraw)(void);
typedef struct {
    RendererVtable* vtable;
    int16_t* objects;
    const char (*names)[17];
    uint32_t skipOffset;
    struct {
        uint32_t offset;
        BoneGateDraw draw[2][2]; // [plain][skipped]
    } draws[BONEGATE_OBJECTS];
} BoneGateBindings;
extern const BoneGateBindings Bonegate_bindings;
#endif
