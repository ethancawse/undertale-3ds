// Optional, exact-string PC-height glyph selection. All game text and glyphs
// remain in the pack derived from the player's copy (tools/aot/stasrtext.py).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ENABLE_WAD16)
#define ASR_TEXT __attribute__((noinline, optimize("Os"), section(".text.late.asrtext")))
static const uint8_t* asrMotion __attribute__((section(".bss.late.asrtext")));
static const uint8_t* asrRule __attribute__((section(".bss.late.asrtext")));

void Asrtext_load(const uint8_t* data, size_t size) ASR_TEXT;
const uint8_t* Asrtext_match(const char* text, size_t len, int32_t font) ASR_TEXT;
int Asrtext_motion(const char* text, size_t len, int32_t font) ASR_TEXT;

void Asrtext_load(const uint8_t* data, size_t size) {
    asrRule = NULL;
    asrMotion = NULL;
    if (data == NULL || size < 144 || data[0] != 'A' || data[1] != 'T' || data[2] != 'X' || data[3] != '1') return;
    uint32_t len;
    memcpy(&len, data + 8, 4);
    if (len > size - 144 || len == 0 || data[142] == 0) return;
    asrRule = data;
    data += 144 + len;
    size -= 144 + len;
    if (size < 12 || data[0] != 'A' || data[1] != 'S' || data[2] != 'Y' || data[3] != '1') return;
    memcpy(&len, data + 8, 4);
    if (len == size - 12 && len != 0) asrMotion = data;
}

const uint8_t* Asrtext_match(const char* text, size_t len, int32_t font) {
    const uint8_t* rule = asrRule;
    if (rule == NULL) return NULL;
    uint32_t wantedFont, wantedLen;
    memcpy(&wantedFont, rule + 4, 4);
    memcpy(&wantedLen, rule + 8, 4);
    if ((uint32_t) font != wantedFont || len != wantedLen || memcmp(text, rule + 144, len) != 0) return NULL;
    return rule + 12;
}

int Asrtext_motion(const char* text, size_t len, int32_t font) {
    const uint8_t* rule = asrMotion;
    if (rule == NULL) return 0;
    uint32_t wantedFont, wantedLen;
    memcpy(&wantedFont, rule + 4, 4);
    memcpy(&wantedLen, rule + 8, 4);
    return (uint32_t) font == wantedFont && len == wantedLen && memcmp(text, rule + 12, len) == 0;
}
#endif
