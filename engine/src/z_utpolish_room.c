// A lazily loaded room's payload, read with one read the size of the room (utpolish).
//
// DataWin_loadRoomPayload parses the room through a 128 KB window of data.win, and the window's first fill read
// 128 KB from the card whatever the room's size. UNDERTALE's rooms are 9 KB at the median (28 KB at the 90th
// percentile, 108 KB at most), and a 128 KB read took about 28 ms of every room change on the New 3DS
// (docs/3ds-hardware.md, "A room change read 128 KB of data.win"). Here initRoom's load works out where the room's
// payload ends (where the next room's begins) and the window is filled with exactly that, through the descriptor so
// that the FILE's 128 KB buffer doesn't read the rest anyway. The parse is unchanged: anything it reads outside the
// span takes the window's usual path.
//
// All code and state is in utpolish's late sections (platform/3ds/late_reserve.ld), so nothing that existed moves.

#include "binary_reader.h"
#include "data_win.h"

#include <stdint.h>
#include <stdio.h>
#if defined(__3DS__)
#include <unistd.h>
#endif

#if defined(__GNUC__)
#define ROOM_LATE __attribute__((noinline, section(".text.late.utpolish")))
#define ROOM_LATE_BSS __attribute__((section(".bss.late.utpolish")))
#else
#define ROOM_LATE
#define ROOM_LATE_BSS
#endif

// The window DataWin_loadRoomPayload opens (data_win.c's ROOM_PAYLOAD_WINDOW_SIZE).
#define ROOM_WINDOW_BYTES (128u * 1024u)

void DataWin_loadRoomPayloadSized(DataWin* dw, int32_t roomIndex) ROOM_LATE;
void BinaryReader_beginRoomWindow(BinaryReader* reader, size_t capacity) ROOM_LATE;

#if defined(__3DS__)
// The span of data.win the next room window is filled with; 0 when there is none to use.
static size_t roomSpanStart ROOM_LATE_BSS;
static size_t roomSpanBytes ROOM_LATE_BSS;

// The lowest of a room's payload offsets (its lists come right after its header), or 0 for a room without one.
static uint32_t ROOM_LATE payloadStart(const Room* room) {
    uint32_t offsets[5] = {room->backgroundsFileOffset, room->viewsFileOffset, room->gameObjectsFileOffset, room->tilesFileOffset,
                           room->layersFileOffset};
    uint32_t start = 0;
    for (int i = 0; 5 > i; i++) {
        if (offsets[i] != 0 && (start == 0 || start > offsets[i])) start = offsets[i];
    }
    return start;
}

static uint32_t ROOM_LATE payloadLastList(const Room* room) {
    uint32_t offsets[5] = {room->backgroundsFileOffset, room->viewsFileOffset, room->gameObjectsFileOffset, room->tilesFileOffset,
                           room->layersFileOffset};
    uint32_t last = 0;
    for (int i = 0; 5 > i; i++) {
        if (offsets[i] > last) last = offsets[i];
    }
    return last;
}
#endif

void DataWin_loadRoomPayloadSized(DataWin* dw, int32_t roomIndex) {
#if defined(__3DS__)
    if (roomIndex >= 0 && dw->room.count > (uint32_t) roomIndex && !dw->room.rooms[roomIndex].payloadLoaded) {
        const Room* room = &dw->room.rooms[roomIndex];
        size_t start = payloadStart(room);
        size_t last = payloadLastList(room);
        // The payload ends where the next room's begins: the nearest room payload after this one's lists.
        size_t end = dw->fileSize;
        for (uint32_t i = 0; dw->room.count > i; i++) {
            if ((int32_t) i == roomIndex) continue;
            size_t other = payloadStart(&dw->room.rooms[i]);
            if (other > last && end > other) end = other;
        }
        if (start != 0 && end > last && ROOM_WINDOW_BYTES >= end - start) {
            roomSpanStart = start;
            roomSpanBytes = end - start;
        }
    }
#endif
    DataWin_loadRoomPayload(dw, roomIndex);
#if defined(__3DS__)
    roomSpanBytes = 0;
#endif
}

void BinaryReader_beginRoomWindow(BinaryReader* reader, size_t capacity) {
    BinaryReader_beginWindow(reader, capacity);
#if defined(__3DS__)
    size_t start = roomSpanStart;
    size_t bytes = roomSpanBytes;
    roomSpanBytes = 0;
    if (bytes == 0 || bytes > capacity || reader->window == nullptr || start > reader->fileSize || bytes > reader->fileSize - start) return;
    // As loadAudoBlob does: read the range through the descriptor and put its offset back, so the FILE's idea of
    // its position (and its buffer) stays valid.
    int fd = fileno(reader->file);
    if (0 > fd) return;
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (0 > saved) return;
    bool ok = lseek(fd, (off_t) start, SEEK_SET) == (off_t) start;
    size_t done = 0;
    while (ok && bytes > done) {
        ssize_t n = read(fd, reader->window + done, bytes - done);
        if (0 >= n) ok = false;
        else done += (size_t) n;
    }
    lseek(fd, saved, SEEK_SET);
    // A failed read leaves the window empty: its first read fills it the usual way.
    if (!ok) return;
    reader->windowBase = start;
    reader->windowLength = bytes;
#endif
}
