// dwkeep (change loadclass 10, UNDERTALE): data.win is looked up in the game folder once; every later open of it to read
// is a sub-file of that one handle, which the FS service opens without a lookup.
//
// Why: an open on the SD card finds its name by reading the folder's entries in order, deleted ones included. Each
// big sound inside data.win (the breaking SAVE's explosion, Flowey's laugh, the chime, Mettaton's heart burst) opens
// data.win again as it starts, and so do the audio loader and the boot. On the lab cards that lookup is 5-15 ms; on a
// card whose folder an installer rewrote (a new data.win lands behind the other names and the deleted entries) it was
// 425-800 ms a sound on the o3DS of the 11-08 play (loadclass 10a).
//
// How: a layer over libctru's sdmc device, installed under ctr_open_files' (CtrOpenFiles_track's chain link, before
// the real one: ctr_wrap_chain.h), so its list counts every open made here. The first open of a path ending in
// "/data.win" to read opens that path once more through the whole stack and keeps it (the keeper: one open file for
// the run, listed as data.win). That open and every later one of the same path to read is answered with
// FS's OpenSubFile over the keeper's handle, the whole file from offset 0: a session of its own, with its own
// position, put into the caller's file state as libctru's own open would (archive_file_t: handle, flags, offset).
// Reads, seeks, fstat, the stream read-ahead (its descriptor's lseek and read) and close are libctru's, unchanged; the
// bytes read are the file's. Any other open passes straight down.
//
// Azahar (its File::Close closes the file's one backend whatever session asks, so the first sub-file closed would
// close the keeper and every other sub-file): there, and only there, a sub-file this layer opened is closed by its
// session handle alone, which Azahar takes as that client leaving; at most DK_SUBS at once (more go to the card). On a
// console a close is libctru's (FS Close, then the handle), as for any file.
//
// Fallbacks: the keeper's open failing (a crowded card) leaves this open to the card and the next one tries again. A
// sub-file open failing with OutOfResource is passed down (the card's open fails the same way, and ctr_open_files has
// the caches close files and tries again, through this layer). Any other sub-file failure turns the layer off for the
// run (one log line): every open goes to the card. The keeper closes, but on Azahar stays (live sub-files share it).
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef DWKEEP_HOST_TEST
#include <3ds.h>
#include <sys/iosupport.h>
#include <unistd.h>
#include "log.h"
#include "ctr_wrap_chain.h"
#define DK_CODE __attribute__((noinline, section(".text_dwkeep")))
#define DK_RO __attribute__((aligned(4), section(".rodata.dwkeep_in_text")))
#define DK_BSS __attribute__((section(".bss.dwkeep_late")))
#define DK_OPEN open
#define DK_CLOSE close
#define DK_SUBFILE dkOpenSubFile
bool CtrVramTouch_emulator(void); // ctr_vram_touch.c's vramTouchOn (an alias): svcGetSystemInfo(0x20000, 0), cached
#define DK_EMULATOR CtrVramTouch_emulator
#define DK_CLOSE_SESSION svcCloseHandle
#endif

#define DK_PATH 192
#define DK_SUBS 16
#define DK_OUT_OF_RESOURCE 620 // FS's description: the SD open allowance is used up (docs/3ds-hardware.md)

// libctru's sdmc file state (archive_dev.c's archive_file_t; its open stores these three, flags masked as here).
typedef struct {
    Handle handle;
    int flags;
    uint64_t offset;
} DkFile;

void CTR_OPENFILES_AFTER_DWKEEP(void);

static struct {
    devoptab_t tab;          // this layer: the device below with open_r replaced
    const devoptab_t* real;  // libctru's sdmc device
    LightLock lock;
    uint8_t state;           // 0 no keeper, 1 kept, 2 the keeper being opened, 3 off for the run
    int fd;                  // the keeper's descriptor (opened through the whole stack, never closed while kept)
    Handle handle;           // its FS handle
    uint64_t size;
    uint32_t served;         // opens answered with a sub-file
    Handle subs[DK_SUBS];    // Azahar only: the sub-files open now (0: a free slot)
    char path[DK_PATH];
} dk DK_BSS;

static const char dkDevice[] DK_RO = "sdmc";
static const char dkName[] DK_RO = "/data.win";
static const char dkKept[] DK_RO = "[dwkeep] keeps %s open (%lu ms); its later opens to read are sub-files of it\n";
static const char dkOff[] DK_RO = "[dwkeep] off for this run: a sub-file open failed (%08lx, %lu served)\n";

#ifndef DWKEEP_HOST_TEST
// FS's file command OpenSubFile (0x0801), as libctru's FSFILE_OpenSubFile sends it, here in late text: libctru's own
// copy would be linked into its fs.o text and move every function after it.
static DK_CODE Result dkOpenSubFile(Handle handle, Handle* subFile, uint64_t offset, uint64_t size) {
    uint32_t* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x801, 4, 0);
    cmd[1] = (uint32_t) offset;
    cmd[2] = (uint32_t) (offset >> 32);
    cmd[3] = (uint32_t) size;
    cmd[4] = (uint32_t) (size >> 32);
    Result rc = svcSendSyncRequest(handle);
    if (R_FAILED(rc)) return rc;
    *subFile = cmd[3];
    return (Result) cmd[1];
}
#endif

// Opens and keeps the keeper (state 2 on entry, without the lock). 1 kept, 0 not.
static DK_CODE int dkKeep(const char* path) {
    uint64_t start = svcGetSystemTick();
    int fd = DK_OPEN(path, O_RDONLY);
    __handle* h = fd >= 0 ? __get_handle(fd) : NULL;
    uint64_t size = 0;
    Handle handle = h != NULL ? ((DkFile*) h->fileStruct)->handle : 0;
    bool kept = handle != 0 && R_SUCCEEDED(FSFILE_GetSize(handle, &size));
    if (!kept && fd >= 0) DK_CLOSE(fd);
    LightLock_Lock(&dk.lock);
    dk.state = kept ? 1 : 0;
    if (kept) {
        dk.fd = fd;
        dk.handle = handle;
        dk.size = size;
    }
    LightLock_Unlock(&dk.lock);
    if (kept) logInfo(dkKept, path, (unsigned long) ((svcGetSystemTick() - start) / 268111u));
    return kept;
}

static DK_CODE int dkOpen(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
    size_t n = strlen(path);
    const size_t name = sizeof(dkName) - 1;
    if ((flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) != O_RDONLY || name >= n || n >= DK_PATH ||
        memcmp(path + n - name, dkName, name) != 0)
        return dk.real->open_r(r, fileStruct, path, flags, mode);
    bool emulator = DK_EMULATOR();
    LightLock_Lock(&dk.lock);
    if (dk.state == 0) {
        dk.state = 2;
        memcpy(dk.path, path, n + 1);
        LightLock_Unlock(&dk.lock);
        int err = r->_errno;
        dkKeep(path); // (its open comes back here in state 2 and goes to the card)
        r->_errno = err;
        LightLock_Lock(&dk.lock);
    }
    Result failed = 0;
    int slot = 0;
    if (emulator)
        while (DK_SUBS > slot && dk.subs[slot] != 0) slot++;
    if (dk.state == 1 && strcmp(path, dk.path) == 0 && DK_SUBS > slot) {
        Handle sub = 0;
        Result rc = DK_SUBFILE(dk.handle, &sub, 0, dk.size);
        if (R_SUCCEEDED(rc)) {
            DkFile* f = (DkFile*) fileStruct;
            f->handle = sub;
            f->flags = flags & (O_ACCMODE | O_APPEND | O_SYNC);
            f->offset = 0;
            dk.served++;
            if (emulator) dk.subs[slot] = sub;
            LightLock_Unlock(&dk.lock);
            return 0;
        }
        if (R_DESCRIPTION(rc) != DK_OUT_OF_RESOURCE) {
            failed = rc;
            dk.state = 3;
        }
    }
    LightLock_Unlock(&dk.lock);
    if (failed != 0) {
        // No new open uses the keeper (state 3, under the lock); on Azahar its close would end the live sub-files.
        if (!emulator) DK_CLOSE(dk.fd);
        logInfo(dkOff, (unsigned long) failed, (unsigned long) dk.served);
    }
    return dk.real->open_r(r, fileStruct, path, flags, mode);
}

// A close: on Azahar, a sub-file this layer opened leaves by its session handle alone (above); anything else is
// libctru's close.
static DK_CODE int dkClose(struct _reent* r, void* fileStruct) {
    Handle h = ((DkFile*) fileStruct)->handle;
    int slot = 0;
    LightLock_Lock(&dk.lock);
    while (DK_SUBS > slot && (h == 0 || dk.subs[slot] != h)) slot++;
    if (DK_SUBS > slot) dk.subs[slot] = 0;
    LightLock_Unlock(&dk.lock);
    if (DK_SUBS > slot) return R_SUCCEEDED(DK_CLOSE_SESSION(h)) ? 0 : (r->_errno = EIO, -1);
    return dk.real->close_r(r, fileStruct);
}

// CtrOpenFiles_track's chain link (ctr_wrap_chain.h): this layer goes over libctru's device before the real track puts
// ctr_open_files' over it.
DK_CODE void CtrOpenFiles_chainDwkeep(void) {
    const devoptab_t* real = GetDeviceOpTab(dkDevice);
    if (real != NULL && real->open_r != NULL && dk.real == NULL) {
        dk.tab = *real;
        dk.tab.open_r = dkOpen;
        dk.tab.close_r = dkClose;
        LightLock_Init(&dk.lock);
        dk.real = real;
        // Replaces the device of the same name in place.
        if (AddDevice(&dk.tab) < 0) dk.real = NULL;
    }
    CTR_OPENFILES_AFTER_DWKEEP();
}
