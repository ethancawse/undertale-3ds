// The parts of lab mode (ctr_lab.h) and the lab host (platform/3ds/labhost/) that are plain data handling: the
// Homebrew Launcher's argument block, the network loader's file names and argument strings, 3DSX headers, and the
// arguments the lab host hands the next app.
//
// No 3DS headers: tools/remote/test_lab_proto.sh builds this file for the PC with its unit tests.
#ifndef CTRPORT_CTR_LAB_PROTO_H
#define CTRPORT_CTR_LAB_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The argument block Luma3DS's hb:ldr takes (HBLDR_SetArgv) and gives the next .3dsx: a little-endian u32 argc,
// then argc NUL-terminated strings. libctru's __system_initArgv reads it.
#define LAB_ARGV_BUFFER_SIZE 0x400
// Where the lab host lives unless --lab-host says otherwise: an SD card path as hb:ldr takes it (no "sdmc:").
#define LAB_HOST_DEFAULT "/3ds/ctrport-lab.3dsx"
// The longest SD card path hb:ldr gets from us (its own limit is PATH_MAX, 255 UTF-16 units).
#define LAB_PATH_MAX 128
// The longest file name the lab host takes from the network.
#define LAB_NAME_MAX 96

// Packs args into out as the argument block above. Returns the bytes used, or 0 when they don't fit. The rest of
// out is zeroed.
size_t LabProto_packArgv(uint8_t out[LAB_ARGV_BUFFER_SIZE], const char* const* args, int count);

// Splits the network loader's argument data (NUL-separated strings, the last NUL maybe missing) in place; data must
// have room for length + 1 bytes. Empty strings are skipped. Returns how many pointers went into out (at most max).
int LabProto_splitArgs(char* data, size_t length, char** out, int max);

// The marker 3ds-hbmenu's network loader adds as the last argument: the sender's IPv4 address as 8 hex digits, then
// "_3DSLINK_". libctru takes it off argv and sets __3dslink_host from it.
bool LabProto_isLinkToken(const char* arg);

// Whether name is a file name the lab host may save under sdmc:/3ds/: a plain name (no directories) of letters,
// digits and ._-+ ending in ".3dsx", at most LAB_NAME_MAX characters, not starting with a dot.
bool LabProto_safeName(const char* name);

// Whether a file starting with header (headerLength bytes of it, at least 32) and fileSize bytes long looks like a
// .3dsx Luma3DS's loader can take: the "3DSX" magic, a sane header and relocation header size, and segments that fit
// in the file. Luma3DS's loader stops the console (a panic) when the file it is told to load is not one, so this is
// checked before every HBLDR_SetTarget.
bool LabProto_check3dsx(const uint8_t* header, size_t headerLength, uint64_t fileSize);

// Turns a path as argv[0] or --lab-host give it ("sdmc:/3ds/x.3dsx", "/3ds/x.3dsx", or 3dslink's older
// "3dslink:/x.3dsx") into the SD card path hb:ldr takes ("/3ds/x.3dsx"). False if it isn't one (relative, "..",
// too long, not a .3dsx).
bool LabProto_sdPath(const char* path, char* out, size_t size);

// The arguments the lab host gives the next app: in (as the network loader sent them, argv[0] first) with
// "--lab-host <hostPath>" added after the app's own unless they already ask for lab mode (--lab, --lab-host,
// --lab-host=...), and a 3dslink marker kept last. Pointers go into out (at most max; the strings are in's, the
// literal "--lab-host" and hostPath). Returns how many.
int LabProto_chainArgs(char* const* in, int count, const char* hostPath, const char** out, int max);

// Appends text to a buffer of size bytes holding a NUL-terminated string, as much as fits. No printf: the crash
// report is put together in an exception handler.
void LabProto_append(char* buffer, size_t size, const char* text);
// The same for a number: hexadecimal with `digits` digits (8 for a register), or decimal.
void LabProto_appendHex(char* buffer, size_t size, uint32_t value, int digits);
void LabProto_appendDec(char* buffer, size_t size, int32_t value);

#endif
