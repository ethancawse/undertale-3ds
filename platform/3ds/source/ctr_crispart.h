// DTCA v1: counted frames; future approvals add records without a format change.
#ifndef CTR_CRISPART_H
#define CTR_CRISPART_H
#include <stdbool.h>
#include <stdint.h>

#define CA_MAGIC 0x41435444u
#define CA_VERSION 1u
#define CA_ATLAS_WIDTH 128u
#define CA_MAX_SIZE (1024u * 1024u)
#define CA_COMMAND 1u

typedef struct {
    uint32_t magic;
    uint16_t version, headerBytes;
    uint32_t fileBytes, count, dataBytes, gameId;
    uint64_t timestamp;
    uint8_t digest[32];
    char part[32];
} CaHeader;

typedef struct {
    char name[32];
    uint32_t frame, tpag;
    uint16_t sourceW, sourceH;
    int16_t originX, originY;
    uint16_t w, h, x, y;
    uint32_t offset, bytes, flags, reserved;
} CaFrame;

bool Crispart_check(const uint8_t* data, uint32_t size, const char* digest, const char* part,
                   uint32_t dataBytes, uint32_t gameId, uint64_t timestamp);
// Validated pack only; texels is a zeroed CA_ATLAS_WIDTH x padded-height RGBA8 GPU texture.
void Crispart_writeAtlas(uint32_t* texels, const uint8_t* data);
#endif
