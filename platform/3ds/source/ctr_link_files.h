// The run's files back to the PC over the 3dslink log connection (--send-files, --autoshot, --statehash).
//
// While the game runs, whatever writes a file the PC should get (the screenshots, state_hash.log) notes its path here:
// only files this run wrote go, nothing else on the SD card. When the app exits, CtrLinkFiles_prepare (on the main
// thread, while the log is still open) orders them, applies the size cap and logs what stays behind. Then, after the
// last log line, the log stream's thread (ctr_log.c) sends them with CtrLinkFiles_send, a fixed-size chunk at a time
// read from the file, so no image is ever held whole in memory and nothing happens on the timed path. Without
// CtrLinkFiles_enable nothing here does anything.
//
// The stream (version 1). Every line is text and starts with "[link] ", the prefix of the link's own lines, so a plain
// log reader never takes file data for a log line; tools/remote/linkserver.py takes the data lines out of the log it
// writes and saves the files.
//   [link] files v1: <count> files, <bytes> bytes                  what follows (after every log line)
//   [link] file begin <name> <size> <crc32>                         size in bytes (decimal), CRC-32 as 8 hex digits
//   [link] data <base64>                                            up to CTR_LINK_FILES_CHUNK bytes of the file each
//   [link] file end <name> <size> <crc32>                           the size and CRC-32 of the data actually sent
//   [link] file skipped <name>: <reason>                            instead of begin..end: it could not be read
//   [link] files done: <sent> of <count> files, <bytes> bytes in <ms> ms
// then the log's own end line ("[link] end of log"). The CRC-32 is zlib's (reflected 0xEDB88320). A file is whole only
// when its end line arrives and matches its begin line and the data received: a connection that breaks, or any other
// line, before the end line means the file was cut off. Names are 1-63 of [A-Za-z0-9._-], not starting with a dot.
//
// No 3DS headers: tools/remote/test_link_files.sh builds this file for the PC with its unit tests.
#ifndef CTRPORT_CTR_LINK_FILES_H
#define CTRPORT_CTR_LINK_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTR_LINK_FILES_MAX 256      // files noted in one run; more are counted and reported, not sent
#define CTR_LINK_FILES_NAME_MAX 64  // with the terminator
#define CTR_LINK_FILES_CHUNK 3072   // file bytes per data line: 4096 base64 characters

// Starts noting files, for at most capBytes in all. Call once, early (before anything worth sending is written).
void CtrLinkFiles_enable(uint32_t capBytes);
bool CtrLinkFiles_enabled(void);
// Main thread: a file this run wrote and the PC should get (the same path again counts once). No-op unless enabled.
void CtrLinkFiles_note(const char* path);

// Main thread, at the exit, while the log is open: picks what will be sent and reports one line per decision through
// report (warning: a file stays behind). state_hash.log goes first, then the top screen's shots, the application
// surface's, the bottom screen's and anything else, each in the order written, while they fit in the cap. Returns how
// many files will be sent.
int CtrLinkFiles_prepare(void (*report)(void* ctx, bool warning, const char* line), void* ctx);
// Whether prepared files are waiting for CtrLinkFiles_send.
bool CtrLinkFiles_pending(void);

typedef struct {
    bool (*send)(void* ctx, const char* data, size_t length); // false: the connection is gone
    uint64_t (*nowMs)(void);                                   // a clock for the summary line; may be nullptr
    void* ctx;
} CtrLinkFilesSink;

// The log stream's thread, after the last log line: sends the prepared files in the format above. Returns false when
// a send failed (the connection is gone; the PC sees the file cut off). Sends nothing the second time.
bool CtrLinkFiles_send(const CtrLinkFilesSink* sink);
// File bytes sent so far: moves while CtrLinkFiles_send makes progress (read from another thread).
uint32_t CtrLinkFiles_progress(void);
// Whether CtrLinkFiles_send is between its first line and its last.
bool CtrLinkFiles_sending(void);
// Whether files are prepared and not yet all sent: pending or sending (read from another thread).
bool CtrLinkFiles_busy(void);

// Forgets everything and turns noting off (the tests).
void CtrLinkFiles_reset(void);

// zlib's CRC-32 of data, continuing from crc (0 to start).
uint32_t CtrLinkFiles_crc32(uint32_t crc, const void* data, size_t length);
// Standard base64 with padding; out needs (length + 2) / 3 * 4 + 1 bytes. Returns the characters written.
size_t CtrLinkFiles_base64(char* out, const uint8_t* data, size_t length);

#endif
