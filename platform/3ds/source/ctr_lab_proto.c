#include "ctr_lab_proto.h"

#include <string.h>

size_t LabProto_packArgv(uint8_t out[LAB_ARGV_BUFFER_SIZE], const char* const* args, int count) {
    memset(out, 0, LAB_ARGV_BUFFER_SIZE);
    if (count < 0) return 0;
    size_t used = 4;
    for (int i = 0; count > i; i++) {
        size_t length = strlen(args[i]) + 1;
        if (length > LAB_ARGV_BUFFER_SIZE - used) {
            memset(out, 0, LAB_ARGV_BUFFER_SIZE);
            return 0;
        }
        memcpy(out + used, args[i], length);
        used += length;
    }
    uint32_t n = (uint32_t) count;
    out[0] = (uint8_t) n;
    out[1] = (uint8_t) (n >> 8);
    out[2] = (uint8_t) (n >> 16);
    out[3] = (uint8_t) (n >> 24);
    return used;
}

int LabProto_splitArgs(char* data, size_t length, char** out, int max) {
    int count = 0;
    size_t at = 0;
    data[length] = '\0'; // the last string's terminator, if it came without one
    while (length > at && max > count) {
        char* start = data + at;
        size_t n = strlen(start);
        if (n > 0) out[count++] = start;
        at += n + 1;
    }
    return count;
}

static bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool LabProto_isLinkToken(const char* arg) {
    if (arg == NULL || strlen(arg) != 17 || strcmp(arg + 8, "_3DSLINK_") != 0) return false;
    for (int i = 0; 8 > i; i++) {
        if (!isHex(arg[i])) return false;
    }
    return true;
}

bool LabProto_safeName(const char* name) {
    size_t length = strlen(name);
    if (length < 6 || length > LAB_NAME_MAX || name[0] == '.') return false;
    if (strcmp(name + length - 5, ".3dsx") != 0) return false;
    for (size_t i = 0; length > i; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+';
        if (!ok) return false;
    }
    return strstr(name, "..") == NULL;
}

static uint32_t read32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t read16(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

bool LabProto_check3dsx(const uint8_t* header, size_t headerLength, uint64_t fileSize) {
    if (headerLength < 32 || memcmp(header, "3DSX", 4) != 0) return false;
    uint32_t headerSize = read16(header + 4);
    uint32_t relocHeaderSize = read16(header + 6);
    uint32_t codeSize = read32(header + 16);
    uint32_t rodataSize = read32(header + 20);
    uint32_t dataSize = read32(header + 24);
    uint32_t bssSize = read32(header + 28);
    if (headerSize < 32 || headerSize > 0x100) return false;
    if (relocHeaderSize < 4 || relocHeaderSize > 0x40 || relocHeaderSize % 4 != 0) return false;
    if (codeSize == 0 || bssSize > dataSize) return false;
    // A .3dsx bigger than the largest application memory could never load.
    const uint64_t limit = 256u * 1024u * 1024u;
    if (codeSize > limit || rodataSize > limit || dataSize > limit || fileSize > limit) return false;
    uint64_t needed = (uint64_t) headerSize + 3u * (uint64_t) relocHeaderSize + codeSize + rodataSize + (dataSize - bssSize);
    return needed <= fileSize;
}

bool LabProto_sdPath(const char* path, char* out, size_t size) {
    if (size == 0) return false;
    out[0] = '\0';
    if (path == NULL) return false;
    char built[LAB_PATH_MAX + 16];
    if (strncmp(path, "sdmc:", 5) == 0) {
        path += 5;
    } else if (strncmp(path, "3dslink:/", 9) == 0) {
        // 3dslink before 0.6 named argv[0] this way; the loader saved the file in sdmc:/3ds/.
        if (strlen(path + 9) >= LAB_PATH_MAX) return false;
        strcpy(built, "/3ds/");
        strcat(built, path + 9);
        path = built;
    }
    size_t length = strlen(path);
    if (path[0] != '/' || length >= LAB_PATH_MAX || length >= size || strstr(path, "..") != NULL || strstr(path, "//") != NULL) return false;
    if (length < 6 || strcmp(path + length - 5, ".3dsx") != 0) return false;
    memcpy(out, path, length + 1);
    return true;
}

static bool asksForLab(const char* arg) {
    return strcmp(arg, "--lab") == 0 || strcmp(arg, "--lab-host") == 0 || strncmp(arg, "--lab-host=", 11) == 0;
}

int LabProto_chainArgs(char* const* in, int count, const char* hostPath, const char** out, int max) {
    int n = 0;
    int last = count;
    const char* token = NULL;
    if (count > 1 && LabProto_isLinkToken(in[count - 1])) {
        token = in[count - 1];
        last = count - 1;
    }
    bool lab = false;
    for (int i = 0; last > i && max > n; i++) {
        if (i > 0 && asksForLab(in[i])) lab = true;
        out[n++] = in[i];
    }
    if (n == 0 && max > 0) out[n++] = "sdmc:/3ds/app.3dsx"; // argv[0] must be there
    if (!lab && hostPath != NULL && max >= n + 2) {
        out[n++] = "--lab-host";
        out[n++] = hostPath;
    }
    if (token != NULL && max > n) out[n++] = token;
    return n;
}

void LabProto_append(char* buffer, size_t size, const char* text) {
    if (size == 0) return;
    size_t used = 0;
    while (size > used && buffer[used] != '\0') used++;
    if (used >= size) return;
    while (*text != '\0' && size - 1 > used) buffer[used++] = *text++;
    buffer[used] = '\0';
}

void LabProto_appendHex(char* buffer, size_t size, uint32_t value, int digits) {
    char text[9];
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    for (int i = 0; digits > i; i++) {
        uint32_t nibble = (value >> ((digits - 1 - i) * 4)) & 0xF;
        text[i] = (char) (nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    text[digits] = '\0';
    LabProto_append(buffer, size, text);
}

void LabProto_appendDec(char* buffer, size_t size, int32_t value) {
    char text[12];
    int at = (int) sizeof(text) - 1;
    text[at] = '\0';
    uint32_t magnitude = value < 0 ? (uint32_t) 0 - (uint32_t) value : (uint32_t) value;
    do {
        text[--at] = (char) ('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude > 0);
    if (value < 0) text[--at] = '-';
    LabProto_append(buffer, size, text + at);
}
