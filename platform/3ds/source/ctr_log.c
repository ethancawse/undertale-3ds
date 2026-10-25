#include "ctr_log_fatalflush_layout.inc"
#include "ctr_log.h"
#include "ctr_exit.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "ctr_link_files.h"
#include "ctr_memory.h"
#include "ctr_startup_clock.h"
#include "log.h"

#define LOG_BUFFER_SIZE 1024

static FILE* logFile = nullptr;
static bool consoleEnabled = true;
// The file, written by the writer thread (or by whoever logs while it isn't running).
static LightLock logLock;
static u32 mainThreadId;
static volatile u32 warningCount;
static char lastWarning[128]; // the main thread's last warning

// ===[ Byte rings ]===
// head and tail count the bytes ever put in and taken out; head - tail are waiting.
typedef struct {
    char* data;
    u32 size;
    u32 head, tail;
} ByteRing;

static u32 ringUsed(const ByteRing* ring) {
    return ring->head - ring->tail;
}

static void ringPut(ByteRing* ring, const void* src, u32 length) {
    const char* from = src;
    while (length > 0) {
        u32 at = ring->head % ring->size;
        u32 run = ring->size - at;
        if (run > length) run = length;
        memcpy(ring->data + at, from, run);
        from += run;
        length -= run;
        ring->head += run;
    }
}

static void ringTake(ByteRing* ring, void* dst, u32 length) {
    char* to = dst;
    while (length > 0) {
        u32 at = ring->tail % ring->size;
        u32 run = ring->size - at;
        if (run > length) run = length;
        memcpy(to, ring->data + at, run);
        to += run;
        length -= run;
        ring->tail += run;
    }
}

// ===[ 3dslink stream ]===
// When the Homebrew Launcher's network loader started the app (3dslink, tools/remote/run3ds.sh), every log line
// also goes to the PC that sent it. Lines wait in a ring buffer for a low-priority thread that connects and sends
// them, so the game never waits on the network: the thread runs while the game thread sleeps (vsync), and lines
// that don't fit are dropped and counted rather than holding the game up.
// A connection that breaks mid-run (a wifi drop) is made again: lines keep collecting in the ring meanwhile, and the
// first line on the new connection says so. Only a connection the app closes itself ends with LINK_END_LINE, which
// is how the PC (tools/remote/linkserver.py, tour3ds.py) tells a finished run from a dropped connection. The first
// connection starts with LINK_HELLO_LINE, so the PC waits for a reconnect only from an app that makes one: apps from
// before this (and the lab host) just close, and the PC takes that close as the end of the run, as it always did.
#define LINK_RING_SIZE (32 * 1024)
// The SOC service's working memory; one TCP socket needs little of it.
#define LINK_SOC_BUFFER_SIZE 0x40000
#define LINK_STACK_SIZE (8 * 1024)
// With the run's files to send (ctr_link_files.h), whose stdio and formatting go deeper.
#define LINK_FILES_STACK_SIZE (16 * 1024)
#define LINK_PRIORITY 0x3F // the lowest an application thread can have
#define LINK_CONNECT_ATTEMPTS 30 // a second apart
#define LINK_RECONNECT_WAIT_MS 2000 // between attempts to connect again, for as long as the app runs
// A send that moves nothing for this long means the connection is gone even if the socket hasn't said so (a wifi
// drop can leave TCP retrying for minutes). The PC's receivers read as fast as the lines come.
#define LINK_STALL_MS 20000
// While the run's files go, the app's exit waits for the link as long as bytes keep moving; it stops waiting when
// nothing has moved for this long (longer than LINK_STALL_MS, after which the thread gives up by itself).
#define LINK_FILES_STALL_MS (LINK_STALL_MS + 5000)
#define LINK_END_LINE "[link] end of log\n"
#define LINK_HELLO_LINE "[link] log stream v2: reconnects after a drop, ends with \"[link] end of log\"\n"
// 3dslink's port: the PC that sent the app listens there. Test builds may point the link elsewhere.
#ifndef LINK_PORT
#define LINK_PORT 17491
#endif

static ByteRing linkRing;
static void (*exitProgress)(void); // CtrLog_setExitProgress
static u32 linkDropped; // bytes dropped since the last report
static LightLock linkLock;
static volatile bool linkOpen; // lines are wanted
static volatile bool linkStop;
static bool linkSending __attribute__((section(".bss.fatalflush"))); // chunk in socket-send buffer
static Thread linkThread;
static u32* linkSocBuffer;
static bool linkSocUp;

// Copies up to size waiting bytes out of the ring.
static size_t linkTake(char* out, size_t size) {
    LightLock_Lock(&linkLock);
    size_t n = 0;
    if (linkDropped > 0) {
        int written = snprintf(out, size, "[link] %lu bytes of log dropped here (the PC fell behind)\n", (unsigned long) linkDropped);
        n = written > 0 && (size_t) written < size ? (size_t) written : 0;
        linkDropped = 0;
    }
    u32 run = ringUsed(&linkRing);
    // Publish before removing bytes: an empty ring alone does not mean a
    // fatal summary has reached the socket. Only linkMain clears this flag.
    if (run > 0 || n > 0) __atomic_store_n(&linkSending, true, __ATOMIC_SEQ_CST);
    if (run > size - n) run = (u32) (size - n);
    ringTake(&linkRing, out + n, run);
    n += run;
    LightLock_Unlock(&linkLock);
    return n;
}

static void linkSleep(u32 ms) {
    for (u32 slept = 0; ms > slept && !linkStop; slept += 50) svcSleepThread(50 * 1000000LL);
}

// The connection libctru's link3dsConnectToHost makes (without handing it stdout: that is the bottom-screen
// console), non-blocking so that a stalled send can be noticed. attempts 0: until the app stops.
static int linkConnect(int attempts, u32 waitMs) {
    for (int attempt = 0; (attempts == 0 || attempts > attempt) && !linkStop; attempt++) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock >= 0) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(LINK_PORT);
            to.sin_addr = __3dslink_host;
            if (connect(sock, (struct sockaddr*) &to, sizeof(to)) == 0) {
                CtrExitFiles_connected(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
                return sock;
            }
            close(sock);
        }
        linkSleep(waitMs);
    }
    return -1;
}

// True once the thread has ended. threadJoin passes on svcWaitSynchronization's result, and its timeout (0x09401BFE,
// level "info") isn't negative, so R_SUCCEEDED would count a thread still running after the wait as joined.
static bool joinWithin(Thread thread, u64 ns) {
    return threadJoin(thread, ns) == 0;
}

static void linkClose(int sock) {
    CtrExitFiles_shutdownDrained(sock, SHUT_RDWR);
    close(sock);
}

// Sends data[*sent..n), advancing *sent. False when the connection is gone: an error, or nothing moved for
// LINK_STALL_MS.
static bool linkSend(int sock, const char* data, size_t n, size_t* sent) {
    u64 lastMoved = osGetTime();
    while (n > *sent) {
        ssize_t r = send(sock, data + *sent, n - *sent, 0);
        if (r > 0) {
            *sent += (size_t) r;
            lastMoved = osGetTime();
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (osGetTime() - lastMoved > LINK_STALL_MS) return false;
            struct pollfd ready = {.fd = sock, .events = POLLOUT, .revents = 0};
            poll(&ready, 1, 100);
            continue;
        }
        return false;
    }
    return true;
}

static bool linkSendFile(void* ctx, const char* data, size_t n) {
    size_t sent = 0;
    return linkSend(*(int*) ctx, data, n, &sent);
}

static __attribute__((noinline, section(".fatalflush"))) void linkMain(void* arg) {
    (void) arg;
    // The PC may start listening a little after the upload (run3ds.sh sends to several consoles in turn), so the
    // first connection gets a while; lines wait in the ring meanwhile. A PC that never answers isn't listening at all.
    char chunk[2048];
    size_t n = 0, sent = 0; // chunk[sent..n) is still to go
    u64 lostAt = 0;
    bool helloDone = false;
    for (int connection = 1;; connection++) {
        int sock = connection == 1 ? linkConnect(LINK_CONNECT_ATTEMPTS, 1000) : linkConnect(0, LINK_RECONNECT_WAIT_MS);
        if (sock < 0) break;
        bool up = true;
        if (!helloDone) {
            // Until the PC has the whole hello it doesn't know to wait for this app, so a reconnect sends it again.
            size_t helloSent = 0;
            up = helloDone = linkSend(sock, LINK_HELLO_LINE, strlen(LINK_HELLO_LINE), &helloSent);
        }
        if (up && connection > 1) {
            // Bytes the old connection had taken but not delivered are lost; whatever it never took follows this.
            char note[160];
            unsigned long awayMs = (unsigned long) (osGetTime() - lostAt);
            int length = snprintf(note, sizeof(note), "\n[link] connected again (connection %d), %lu.%lu s after a send failed; the last lines sent before that may be missing\n",
                                  connection, awayMs / 1000, awayMs % 1000 / 100);
            size_t noteSent = 0;
            up = length > 0 && linkSend(sock, note, (size_t) length, &noteSent);
        }
        while (up) {
            bool stopping = linkStop;
            if (sent == n) {
                n = linkTake(chunk, sizeof(chunk));
                sent = 0;
            }
            if (n == 0) {
                if (stopping) break;
                svcSleepThread(50 * 1000000LL);
                continue;
            }
            up = linkSend(sock, chunk, n, &sent);
            if (sent == n) __atomic_store_n(&linkSending, false, __ATOMIC_RELEASE);
        }
        // Every log line is sent and the app is stopping: the run's files go now (ctr_link_files.h), before the end
        // line. A connection that breaks meanwhile isn't made again (the app is ending): the PC sees the file cut off.
        if (up && CtrLinkFiles_pending()) up = CtrLinkFiles_send(&(CtrLinkFilesSink) {linkSendFile, osGetTime, &sock});
        if (up) {
            // Everything is sent and the app is stopping: say so, so the PC knows the run is over.
            size_t endSent = 0;
            linkSend(sock, LINK_END_LINE, strlen(LINK_END_LINE), &endSent);
            linkClose(sock);
            break;
        }
        linkClose(sock);
        if (linkStop) break; // the app is ending; don't wait for the network
        lostAt = osGetTime();
    }
    linkOpen = false;
}

static void linkWrite(const char* text) {
    if (!linkOpen) return;
    u32 length = (u32) strlen(text);
    LightLock_Lock(&linkLock);
    if (length > linkRing.size - ringUsed(&linkRing)) {
        linkDropped += length;
    } else {
        ringPut(&linkRing, text, length);
    }
    LightLock_Unlock(&linkLock);
}

__attribute__((noinline, section(".fatalflush"))) bool CtrLog_startLink(void) {
    if (__3dslink_host.s_addr == 0 || linkThread != nullptr) return false;
    linkSocBuffer = memalign(0x1000, LINK_SOC_BUFFER_SIZE);
    linkRing.data = malloc(LINK_RING_SIZE);
    if (linkSocBuffer == nullptr || linkRing.data == nullptr || R_FAILED(socInit(linkSocBuffer, LINK_SOC_BUFFER_SIZE))) {
        free(linkSocBuffer);
        free(linkRing.data);
        linkSocBuffer = nullptr;
        linkRing.data = nullptr;
        return false;
    }
    linkSocUp = true;
    LightLock_Init(&linkLock);
    linkRing.size = LINK_RING_SIZE;
    linkRing.head = linkRing.tail = 0;
    linkDropped = 0;
    linkStop = false;
    linkOpen = true;
    linkThread = threadCreate(linkMain, nullptr, CtrLinkFiles_enabled() ? LINK_FILES_STACK_SIZE : LINK_STACK_SIZE, LINK_PRIORITY, -2, false);
    if (linkThread == nullptr) {
        CtrLog_stopLink();
        return false;
    }
    return true;
}

const char* CtrLog_linkHost(void) {
    return __3dslink_host.s_addr != 0 ? inet_ntoa(__3dslink_host) : nullptr;
}

bool CtrLog_linkOpen(void) {
    return linkOpen;
}

__attribute__((noinline, section(".fatalflush"))) void CtrLog_stopLink(void) {
    if (linkThread != nullptr) {
        linkStop = true;
        // The thread sends what is left, then closes. Shutting the service down breaks a send stuck on a PC that
        // stopped reading, so the second wait ends.
        bool joined = joinWithin(linkThread, 5000000000ULL);
        // The run's files (ctr_link_files.h) take as long as they take, while bytes keep moving.
        u32 moved = CtrLinkFiles_progress();
        u64 movedAt = osGetTime();
        bool waitedForFiles = false;
        while (!joined && CtrLinkFiles_busy() && osGetTime() - movedAt < LINK_FILES_STALL_MS) {
            waitedForFiles = true;
            joined = joinWithin(linkThread, 1000000000ULL);
            if (CtrLinkFiles_progress() != moved) {
                moved = CtrLinkFiles_progress();
                movedAt = osGetTime();
                if (exitProgress != nullptr) exitProgress();
            }
        }
        // The files are through (or stuck): the end line gets the same time the log's last lines had.
        if (!joined && waitedForFiles) joined = joinWithin(linkThread, 5000000000ULL);
        if (!joined && linkSocUp) {
            socExit();
            linkSocUp = false;
            joined = joinWithin(linkThread, 2000000000ULL);
        }
        if (!joined) return; // leave its memory alone; the process is ending anyway
        threadFree(linkThread);
        linkThread = nullptr;
    }
    linkOpen = false;
    if (linkSocUp) socExit();
    linkSocUp = false;
    free(linkSocBuffer);
    free(linkRing.data);
    linkSocBuffer = nullptr;
    linkRing.data = nullptr;
}

// ===[ Writer ]===
// Log lines are formatted, written to log.txt, sent to the 3dslink PC and printed on the bottom-screen console by a
// low-priority thread, so a thread that logs neither waits for the SD card (a New 3DS took 20-50 ms for the
// once-a-second flush) nor spends its time formatting numbers. Each line goes into a ring as a record holding its
// format and arguments, strings copied, and the writer formats the records in the order they came. A thread that
// finds the ring full waits during boot. During play, ordinary lines leave a reserve for warnings/errors and may
// be dropped; warnings/errors always wait or take the synchronous fallback.
#define WRITER_RING_SIZE (64 * 1024)
#define WRITER_WARNING_RESERVE (8 * 1024)
#define WRITER_STACK_SIZE (32 * 1024)
// Below the game thread (0x30) and the audio decoder (0x2F), above the texture packing (0x3F).
#define WRITER_PRIORITY 0x3E
// A record's payload at most. Lines are cut at LOG_BUFFER_SIZE anyway; it stays small for threads with small stacks.
#define RECORD_MAX 2048
// How long a thread waits for the writer before giving up (a flush, or room in the ring).
#define WRITER_WAIT_NS 3000000000LL
#define WRITER_POLL_NS 1000000LL

enum { RECORD_LINE, RECORD_LINK, RECORD_FLUSH, RECORD_TEXT };
// A line's flags: print it on the console; leave its console selected afterwards.
#define LINE_CONSOLE 1u
#define LINE_SELECT 2u
#define LINE_NO_LINK 4u // emergency text already went directly to the TCP ring

typedef struct {
    u16 kind;
    u16 size; // of the payload that follows
} RecordHeader;

// RECORD_LINE's payload: this, the format (NUL-terminated), then the arguments.
typedef struct {
    u8 type; // logType
    u8 flags;
    u16 formatSize;
    PrintConsole* target; // nullptr: a log line, for the console selected, the file and the PC
    uint64_t elapsedUs; // captured by the producer, before queue waits or deferred formatting
} LineHeader;

static ByteRing writerRing;
static LightLock writerLock;
static LightEvent writerEvent;
static Thread writerThread;
static volatile bool writerStop;
static volatile bool writerRunning;
static bool writerFrameTimed; // under writerLock; false through first-room initialization, on each chapter load
static u32 writerThreadId;
static u32 flushAsked;          // under writerLock
static volatile u32 flushDone;  // the last flush the writer finished
static volatile u32 consoleQueued; // lines in the ring for the console
// Held while the writer prints, and by the main thread between CtrLog_beginConsole and CtrLog_endConsole.
static RecursiveLock consoleLock;
static int32_t consoleHeld; // by the main thread
// The framebuffer the writer last printed on, until it flushes it from the data cache.
static u16* printedFramebuffer;
// The bottom screen's, 240x320 RGB565.
#define CONSOLE_FRAMEBUFFER_BYTES (240 * 320 * 2)

static const char* textPrefixOf(logType type) {
    switch (type) {
        case LOG_TYPE_WARNING: return "Warning: ";
        case LOG_TYPE_ERROR: return "Error: ";
        case LOG_TYPE_DEBUG: return "Debug: ";
        default: return "";
    }
}

static const char* colourPrefixOf(logType type) {
    switch (type) {
        case LOG_TYPE_WARNING: return ANSI_COLOUR_CODE_BOLD_YELLOW;
        case LOG_TYPE_ERROR: return ANSI_COLOUR_CODE_BOLD_RED;
        case LOG_TYPE_DEBUG: return ANSI_COLOUR_CODE_BOLD_PURPLE;
        default: return "";
    }
}

static u32 currentThreadId(void) {
    u32 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id;
}

// --- Formats without the arguments at hand ---
// A conversion of a printf format: what follows the '%' up to `end`.
enum { LENGTH_NONE, LENGTH_LONG_LONG, LENGTH_LONG_DOUBLE };
typedef struct {
    const char* end;
    char conversion; // '\0' at the end of the format
    int length;
    bool widthStar, precisionStar;
} Conversion;

static const char* parseConversion(const char* p, Conversion* c) {
    memset(c, 0, sizeof(*c));
    while (*p != '\0' && strchr("-+ #0'", *p) != nullptr) p++;
    if (*p == '*') {
        c->widthStar = true;
        p++;
    } else {
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == '.') {
        p++;
        if (*p == '*') {
            c->precisionStar = true;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') p++;
        }
    }
    if (p[0] == 'l' && p[1] == 'l') {
        c->length = LENGTH_LONG_LONG;
        p += 2;
    } else if (*p == 'j') {
        c->length = LENGTH_LONG_LONG;
        p++;
    } else if (*p == 'L') {
        c->length = LENGTH_LONG_DOUBLE;
        p++;
    } else {
        while (*p == 'h' || *p == 'l' || *p == 'z' || *p == 't') p++; // all 32 bits or promoted to int
    }
    c->conversion = *p;
    if (*p != '\0') p++;
    c->end = p;
    return p;
}

static bool isFloatConversion(char c) {
    return c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G' || c == 'a' || c == 'A';
}

static bool isIntConversion(char c) {
    return c == 'd' || c == 'i' || c == 'u' || c == 'o' || c == 'x' || c == 'X' || c == 'c';
}

typedef struct {
    char* data;
    u32 size, used;
    bool full;
} Packer;

static void pack(Packer* p, const void* src, u32 length) {
    if (p->full || length > p->size - p->used) {
        p->full = true;
        return;
    }
    memcpy(p->data + p->used, src, length);
    p->used += length;
}

// Takes each conversion's arguments off va in the format's order: what formatArguments puts back.
static void packArguments(Packer* p, const char* format, va_list va) {
    for (const char* f = format; *f != '\0';) {
        if (*f++ != '%') continue;
        Conversion c;
        f = parseConversion(f, &c);
        if (c.widthStar) {
            int v = va_arg(va, int);
            pack(p, &v, sizeof(v));
        }
        if (c.precisionStar) {
            int v = va_arg(va, int);
            pack(p, &v, sizeof(v));
        }
        if (c.conversion == '\0') break;
        if (isIntConversion(c.conversion)) {
            if (c.length == LENGTH_LONG_LONG) {
                long long v = va_arg(va, long long);
                pack(p, &v, sizeof(v));
            } else {
                int v = va_arg(va, int);
                pack(p, &v, sizeof(v));
            }
        } else if (isFloatConversion(c.conversion)) {
            double v = c.length == LENGTH_LONG_DOUBLE ? (double) va_arg(va, long double) : va_arg(va, double);
            pack(p, &v, sizeof(v));
        } else if (c.conversion == 's') {
            const char* s = va_arg(va, const char*);
            if (s == nullptr) s = "(null)";
            // Lines are cut at LOG_BUFFER_SIZE, so no more of a string can show.
            u32 length = (u32) strnlen(s, LOG_BUFFER_SIZE - 1);
            u32 room = p->size - p->used;
            if (!p->full && length + 1 > room && room > 0) length = room - 1;
            pack(p, s, length);
            pack(p, "", 1);
        } else if (c.conversion == 'p') {
            void* v = va_arg(va, void*);
            pack(p, &v, sizeof(v));
        } else if (c.conversion == 'n') {
            (void) va_arg(va, void*); // writes nothing here
        }
    }
}

typedef struct {
    const char* data;
    u32 size, at;
} Unpacker;

static void unpack(Unpacker* u, void* dst, u32 length) {
    if (length > u->size - u->at) {
        memset(dst, 0, length);
        u->at = u->size;
        return;
    }
    memcpy(dst, u->data + u->at, length);
    u->at += length;
}

static void appendf(char* out, size_t size, size_t* n, const char* spec, ...) {
    if (*n + 1 >= size) return;
    va_list va;
    va_start(va, spec);
    int written = vsnprintf(out + *n, size - *n, spec, va);
    va_end(va);
    if (written > 0) *n = *n + (size_t) written < size ? *n + (size_t) written : size - 1;
}

// Formats format with the arguments packArguments packed, as vsnprintf would have.
static void formatArguments(char* out, size_t size, const char* format, Unpacker* u) {
    size_t n = 0;
    out[0] = '\0';
    for (const char* f = format; *f != '\0' && size > n + 1;) {
        if (*f != '%') {
            out[n++] = *f++;
            out[n] = '\0';
            continue;
        }
        const char* start = f++;
        Conversion c;
        f = parseConversion(f, &c);
        if (c.conversion == '\0') break;
        // The conversion's own text, with any '*' replaced by its number.
        char spec[48];
        size_t specLength = 0;
        for (const char* s = start; s < c.end && sizeof(spec) - 12 > specLength; s++) {
            if (*s == '*') {
                int v = 0;
                unpack(u, &v, sizeof(v));
                specLength += (size_t) snprintf(spec + specLength, sizeof(spec) - specLength, "%d", v);
            } else {
                spec[specLength++] = *s;
            }
        }
        spec[specLength] = '\0';
        if (c.conversion == '%') {
            appendf(out, size, &n, "%%");
        } else if (isIntConversion(c.conversion)) {
            if (c.length == LENGTH_LONG_LONG) {
                long long v = 0;
                unpack(u, &v, sizeof(v));
                appendf(out, size, &n, spec, v);
            } else {
                int v = 0;
                unpack(u, &v, sizeof(v));
                appendf(out, size, &n, spec, v);
            }
        } else if (isFloatConversion(c.conversion)) {
            double v = 0.0;
            unpack(u, &v, sizeof(v));
            if (c.length == LENGTH_LONG_DOUBLE) appendf(out, size, &n, spec, (long double) v);
            else appendf(out, size, &n, spec, v);
        } else if (c.conversion == 's') {
            const char* s = u->data + u->at;
            size_t length = strnlen(s, u->size - u->at);
            u->at += (u32) (length < u->size - u->at ? length + 1 : length);
            char text[LOG_BUFFER_SIZE];
            memcpy(text, s, length < sizeof(text) ? length : sizeof(text) - 1);
            text[length < sizeof(text) ? length : sizeof(text) - 1] = '\0';
            appendf(out, size, &n, spec, text);
        } else if (c.conversion == 'p') {
            void* v = nullptr;
            unpack(u, &v, sizeof(v));
            appendf(out, size, &n, spec, v);
        }
    }
}

// --- The ring ---
// Only ordinary/debug lines may be dropped, after boot. Counts are under writerLock.
static u32 droppedInfo, droppedDebug;

// The record that says how many lines were dropped: a warning line, its format, then the count as its argument.
#define DROPPED_FORMAT "%u log lines dropped (the log writer fell behind): %u info, %u debug, 0 warnings, 0 errors\n"
#define DROPPED_RECORD_SIZE (sizeof(RecordHeader) + sizeof(LineHeader) + sizeof(DROPPED_FORMAT) + 3 * sizeof(u32))

// Under writerLock, with enough room. Also called by the writer before it goes idle/stops, so the final count
// does not depend on a later producer having room for both a line and this notice.
static void putDroppedRecord(void) {
    char record[DROPPED_RECORD_SIZE];
    RecordHeader header = { RECORD_LINE, (u16) (DROPPED_RECORD_SIZE - sizeof(RecordHeader)) };
    LineHeader line = { (u8) LOG_TYPE_WARNING, 0, (u16) sizeof(DROPPED_FORMAT), nullptr, CtrStartup_elapsedUs() };
    char* at = record;
    memcpy(at, &header, sizeof(header));
    at += sizeof(header);
    memcpy(at, &line, sizeof(line));
    at += sizeof(line);
    memcpy(at, DROPPED_FORMAT, sizeof(DROPPED_FORMAT));
    at += sizeof(DROPPED_FORMAT);
    u32 counts[] = { droppedInfo + droppedDebug, droppedInfo, droppedDebug };
    memcpy(at, counts, sizeof(counts));
    ringPut(&writerRing, record, sizeof(record));
    droppedInfo = droppedDebug = 0;
}

// Boot/control/warning/error records wait for room; a timed-out caller writes synchronously instead. In play,
// ordinary records may use only the space outside the warning reserve. No change to the frame loop is needed.
static bool postRecord(u16 kind, const void* payload, u32 size, bool forConsole, bool droppable) {
    RecordHeader header = { kind, (u16) size };
    u32 total = (u32) sizeof(header) + size;
    s64 waited = 0;
    for (;;) {
        if (!writerRunning) return false;
        LightLock_Lock(&writerLock);
        if (!writerRunning) {
            LightLock_Unlock(&writerLock);
            return false;
        }
        bool mayDrop = droppable && writerFrameTimed;
        u32 reserve = mayDrop ? WRITER_WARNING_RESERVE : 0;
        u32 room = writerRing.size - ringUsed(&writerRing);
        if ((droppedInfo > 0 || droppedDebug > 0) && room >= total + reserve + (u32) DROPPED_RECORD_SIZE) {
            putDroppedRecord();
            room -= (u32) DROPPED_RECORD_SIZE;
        }
        // A pending drop notice must not prevent a warning from using its reserve.
        if (room >= total + reserve) {
            ringPut(&writerRing, &header, sizeof(header));
            ringPut(&writerRing, payload, size);
            if (forConsole) __atomic_add_fetch(&consoleQueued, 1, __ATOMIC_SEQ_CST);
            LightLock_Unlock(&writerLock);
            LightEvent_Signal(&writerEvent);
            return true;
        }
        if (mayDrop) {
            LineHeader line;
            memcpy(&line, payload, sizeof(line));
            if (line.type == LOG_TYPE_DEBUG) droppedDebug++;
            else droppedInfo++;
            LightLock_Unlock(&writerLock);
            return true;
        }
        LightLock_Unlock(&writerLock);
        if (currentThreadId() == writerThreadId || waited >= WRITER_WAIT_NS) return false;
        svcSleepThread(WRITER_POLL_NS);
        waited += WRITER_POLL_NS;
    }
}

// Asks the writer to flush the file once it has written everything before; returns that flush's number.
static bool postFlush(u32* number) {
    if (!writerRunning) return false;
    LightLock_Lock(&writerLock);
    u32 n = ++flushAsked;
    LightLock_Unlock(&writerLock);
    *number = n;
    return postRecord(RECORD_FLUSH, &n, sizeof(n), false, false);
}

static void waitFor(volatile u32* counter, bool (*done)(u32 value, u32 target), u32 target) {
    for (s64 waited = 0; writerRunning && !done(*counter, target) && WRITER_WAIT_NS > waited; waited += WRITER_POLL_NS) {
        svcSleepThread(WRITER_POLL_NS);
    }
}

static bool reached(u32 value, u32 target) {
    return 0 <= (int32_t) (value - target);
}

static bool isZero(u32 value, u32 target) {
    (void) target;
    return value == 0;
}

// --- The last lines logged, for crash reports (ctr_lab.h) ---
// Written by whoever writes lines (the writer, or a thread logging while it isn't there), read without a lock by
// CtrLog_tail: a report may get a garbled line, never a wait.
#define TAIL_SIZE 4096
static char tailRing[TAIL_SIZE];
static volatile u32 tailHead; // bytes ever put in

static void tailPut(const char* text) {
    u32 head = tailHead;
    for (const char* p = text; *p != '\0'; p++) tailRing[head++ % TAIL_SIZE] = *p;
    tailHead = head;
}

size_t CtrLog_tail(char* out, size_t size) {
    if (size == 0) return 0;
    u32 head = tailHead;
    u32 n = head < TAIL_SIZE ? head : TAIL_SIZE;
    if (n > size - 1) n = (u32) (size - 1);
    u32 start = head - n;
    size_t used = 0;
    for (u32 i = 0; n > i; i++) {
        char c = tailRing[(start + i) % TAIL_SIZE];
        out[used++] = c != '\0' ? c : '?';
    }
    out[used] = '\0';
    // From the first whole line.
    if (head > n) {
        char* newline = memchr(out, '\n', used);
        if (newline != nullptr && newline + 1 < out + used) {
            size_t skip = (size_t) (newline + 1 - out);
            memmove(out, out + skip, used - skip + 1);
            used -= skip;
        }
    }
    return used;
}

// --- The thread ---
static __attribute__((noinline, section(".fatalflush"))) void writeLine(logType type, u8 flags, PrintConsole* target, const char* text) {
    if (flags & LINE_CONSOLE) {
        RecursiveLock_Lock(&consoleLock);
        PrintConsole* previous = consoleSelect(target);
        if (target == nullptr) consoleSelect(previous); // the current one
        PrintConsole* printedOn = target != nullptr ? target : previous;
        if (target != nullptr) {
            fputs(text, stdout);
            if (!(flags & LINE_SELECT)) consoleSelect(previous);
        } else {
            const char* colour = colourPrefixOf(type);
            printf("%s%s%s", colour, text, *colour ? ANSI_COLOUR_CODE_RESET : "");
        }
        if (printedOn != nullptr) printedFramebuffer = printedOn->frameBuffer;
        RecursiveLock_Unlock(&consoleLock);
    }
    if (target != nullptr) return;
    tailPut(text);
    if (logFile != nullptr) {
        LightLock_Lock(&logLock);
        fputs(text, logFile);
        // Errors usually come right before a crash, so don't leave them in the buffer.
        if (type == LOG_TYPE_ERROR) fflush(logFile);
        LightLock_Unlock(&logLock);
    }
    if (!(flags & LINE_NO_LINK)) linkWrite(text);
    // Visible in Luma3DS's GDB stub and in emulator logs.
    svcOutputDebugString(text, (s32) strlen(text));
}

static __attribute__((noinline, section(".fatalflush"))) void writerMain(void* arg) {
    (void) arg;
    writerThreadId = currentThreadId();
    static char payload[RECORD_MAX];
    char text[LOG_BUFFER_SIZE];
    for (;;) {
        RecordHeader header;
        LightLock_Lock(&writerLock);
        if (ringUsed(&writerRing) == 0 && (droppedInfo > 0 || droppedDebug > 0)) putDroppedRecord();
        bool got = ringUsed(&writerRing) >= sizeof(header);
        if (got) {
            ringTake(&writerRing, &header, sizeof(header));
            ringTake(&writerRing, payload, header.size);
        }
        // Stops only with the ring empty, and under the lock, so no record is put in after the last look.
        bool stopping = !got && writerStop;
        if (stopping) writerRunning = false;
        LightLock_Unlock(&writerLock);
        if (stopping) break;
        if (!got) {
            // The main thread flushes the bottom screen each frame, but cache maintenance stays on the core that
            // asks for it, and this may run on another: flush what it printed from here.
            if (printedFramebuffer != nullptr) {
                GSPGPU_FlushDataCache(printedFramebuffer, CONSOLE_FRAMEBUFFER_BYTES);
                printedFramebuffer = nullptr;
            }
            LightEvent_WaitTimeout(&writerEvent, 100000000LL);
            continue;
        }
        if (header.kind == RECORD_LINE) {
            LineHeader line;
            memcpy(&line, payload, sizeof(line));
            const char* format = payload + sizeof(line);
            Unpacker u = { payload, header.size, (u32) sizeof(line) + line.formatSize };
            int written = snprintf(text, sizeof(text), "%s", textPrefixOf((logType) line.type));
            if (0 > written || written >= (int) sizeof(text)) written = 0;
            formatArguments(text + written, sizeof(text) - (size_t) written, format, &u);
            if (line.target == nullptr) CtrStartup_stamp(text, sizeof(text), line.elapsedUs);
            writeLine((logType) line.type, line.flags, line.target, text);
            if (line.flags & LINE_CONSOLE) __atomic_sub_fetch(&consoleQueued, 1, __ATOMIC_SEQ_CST);
        } else if (header.kind == RECORD_LINK) {
            payload[header.size > 0 ? header.size - 1 : 0] = '\0';
            linkWrite(payload);
        } else if (header.kind == RECORD_TEXT) {
            payload[header.size > 0 ? header.size - 1 : 0] = '\0';
            writeLine(LOG_TYPE_ERROR, LINE_NO_LINK, nullptr, payload);
        } else if (header.kind == RECORD_FLUSH) {
            u32 number;
            memcpy(&number, payload, sizeof(number));
            if (logFile != nullptr) {
                LightLock_Lock(&logLock);
                fflush(logFile);
                LightLock_Unlock(&logLock);
            }
            // Fatal callers may hold the heap lock. A flush must never free
            // number caches here; the trampoline releases them on thread exit.
            flushDone = number;
        }
    }
}

static void startWriter(void) {
    writerRing.data = malloc(WRITER_RING_SIZE);
    if (writerRing.data == nullptr) return;
    writerRing.size = WRITER_RING_SIZE;
    writerRing.head = writerRing.tail = 0;
    LightLock_Init(&writerLock);
    LightEvent_Init(&writerEvent, RESET_ONESHOT);
    RecursiveLock_Init(&consoleLock);
    writerStop = false;
    writerFrameTimed = false;
    droppedInfo = droppedDebug = 0;
    writerRunning = true;
    // The New 3DS's third core; elsewhere the game's, where it runs while the game thread waits for vblank or the GPU.
    // Only asked for there: an Old 3DS has no core 2, and Azahar's Old 3DS mode closes on the request.
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds) writerThread = threadCreate(writerMain, nullptr, WRITER_STACK_SIZE, WRITER_PRIORITY, 2, false);
    if (writerThread == nullptr) writerThread = threadCreate(writerMain, nullptr, WRITER_STACK_SIZE, WRITER_PRIORITY, -2, false);
    if (writerThread == nullptr) {
        writerRunning = false;
        free(writerRing.data);
        writerRing.data = nullptr;
    }
}

// Writes out everything queued, then stops the thread.
static void stopWriter(void) {
    if (writerThread == nullptr) return;
    writerStop = true;
    LightEvent_Signal(&writerEvent);
    bool joined = joinWithin(writerThread, 5000000000ULL);
    writerRunning = false;
    if (!joined) return; // leave its memory alone; the process is ending anyway
    threadFree(writerThread);
    writerThread = nullptr;
    free(writerRing.data);
    writerRing.data = nullptr;
}

// ===[ Log file and console ]===

__attribute__((noinline, section(".fatalflush"))) void CtrLog_openFile(const char* path) {
    LightLock_Init(&logLock);
    mainThreadId = currentThreadId();
    logFile = fopen(path, "w");
    if (logFile != nullptr) setvbuf(logFile, nullptr, _IOFBF, 16 * 1024);
    startWriter();
}

static void reportFileDecision(void* ctx, bool warning, const char* line) {
    (void) ctx;
    if (warning) logWarn("%s\n", line);
    else logInfo("%s\n", line);
}

void CtrLog_setExitProgress(void (*hook)(void)) {
    exitProgress = hook;
}

__attribute__((noinline, section(".fatalflush"))) void CtrLog_closeFile(void) {
    // The run's files go to the PC after the last log line (ctr_link_files.h): what stays behind is logged first.
    if (CtrLinkFiles_enabled()) {
        if (linkOpen) CtrLinkFiles_prepare(reportFileDecision, nullptr);
        else logWarn("Remote: no log connection to the PC, so the run's files stay on the SD card\n");
        if (exitProgress != nullptr && CtrLinkFiles_pending()) exitProgress();
    }
    stopWriter();
    CtrLog_stopLink();
    if (logFile == nullptr) return;
    fclose(logFile);
    logFile = nullptr;
}

void CtrLog_flush(void) {
    u32 number;
    if (currentThreadId() != writerThreadId && postFlush(&number)) {
        waitFor(&flushDone, reached, number);
        return;
    }
    if (logFile == nullptr) return;
    LightLock_Lock(&logLock);
    fflush(logFile);
    LightLock_Unlock(&logLock);
}

void CtrLog_flushSoon(void) {
    u32 number;
    if (!postFlush(&number)) CtrLog_flush();
}

void CtrLog_setConsoleEnabled(bool enabled) {
    consoleEnabled = enabled;
}

void CtrLog_setFrameTimed(bool frameTimed) {
    if (!writerRunning) return;
    LightLock_Lock(&writerLock);
    writerFrameTimed = frameTimed;
    LightLock_Unlock(&writerLock);
}

bool CtrLog_consoleEnabled(void) {
    return consoleEnabled;
}

__attribute__((noinline, section(".fatalflush"))) void CtrLog_beginConsole(void) {
    if (!writerRunning) return;
    waitFor(&consoleQueued, isZero, 0);
    RecursiveLock_Lock(&consoleLock);
    consoleHeld++;
}

__attribute__((noinline, section(".fatalflush"))) void CtrLog_endConsole(void) {
    if (0 >= consoleHeld) return;
    consoleHeld--;
    RecursiveLock_Unlock(&consoleLock);
}

extern bool CtrExit_postRecord(u16 kind, const void* payload, u32 size, bool forConsole, bool droppable);

static bool postLine(logType type, u8 flags, PrintConsole* target, uint64_t elapsedUs, const char* format, va_list va) {
    char payload[RECORD_MAX];
    LineHeader line = { (u8) type, flags, 0, target, elapsedUs };
    u32 formatLength = (u32) strnlen(format, LOG_BUFFER_SIZE - 1);
    line.formatSize = (u16) (formatLength + 1);
    memcpy(payload, &line, sizeof(line));
    memcpy(payload + sizeof(line), format, formatLength);
    payload[sizeof(line) + formatLength] = '\0';
    Packer p = { payload, sizeof(payload), (u32) sizeof(line) + line.formatSize, false };
    // The copy is what the writer formats, so arguments must match it even when cut.
    packArguments(&p, payload + sizeof(line), va);
    // Warnings/errors and dedicated consoles are never dropped. Ordinary lines can drop only after boot;
    // postRecord protects the warning reserve and accounts for their severity.
    bool droppable = target == nullptr && (type == LOG_TYPE_NORMAL || type == LOG_TYPE_DEBUG);
    return CtrExit_postRecord(RECORD_LINE, payload, p.used, (flags & LINE_CONSOLE) != 0, droppable);
}

void CtrLog_printTo(PrintConsole* console, const char* format, ...) {
    va_list va;
    va_start(va, format);
    bool queued = postLine(LOG_TYPE_NORMAL, LINE_CONSOLE, console, 0, format, va);
    va_end(va);
    if (queued) return;
    PrintConsole* previous = consoleSelect(console);
    va_start(va, format);
    vprintf(format, va);
    va_end(va);
    consoleSelect(previous);
}

static bool postSelect(PrintConsole* console, ...) {
    va_list va;
    va_start(va, console);
    bool queued = postLine(LOG_TYPE_NORMAL, LINE_CONSOLE | LINE_SELECT, console, 0, "", va);
    va_end(va);
    return queued;
}

void CtrLog_selectConsole(PrintConsole* console) {
    if (!postSelect(console)) consoleSelect(console);
}

void CtrLog_linkWrite(const char* text) {
    // Behind the lines before it, like everything else the PC gets.
    u32 length = (u32) strlen(text);
    if (RECORD_MAX > length && postRecord(RECORD_LINK, text, length + 1, false, false)) return;
    linkWrite(text);
}

// ===[ After a crash or hang (ctr_lab.h) ]===
// The thread calling may have crashed holding any lock, or other threads may have: locks are only tried, and every
// wait ends by the deadline.

static u64 msNow(void) {
    return svcGetSystemTick() / CPU_TICKS_PER_MSEC;
}

static bool postRecordBefore(u16 kind, const void* payload, u32 size, u64 deadline) {
    RecordHeader header = { kind, (u16) size };
    u32 total = (u32) sizeof(header) + size;
    while (writerRunning) {
        if (LightLock_TryLock(&writerLock) == 0) {
            bool room = writerRunning && writerRing.size - ringUsed(&writerRing) >= total;
            if (room) {
                ringPut(&writerRing, &header, sizeof(header));
                ringPut(&writerRing, payload, size);
            }
            LightLock_Unlock(&writerLock);
            if (room) {
                LightEvent_Signal(&writerEvent);
                return true;
            }
        }
        if (msNow() >= deadline) return false;
        svcSleepThread(WRITER_POLL_NS);
    }
    return false;
}

static void linkPutBefore(const char* text, u64 deadline) {
    u32 length = (u32) strlen(text);
    while (linkOpen && linkRing.data != nullptr) {
        if (LightLock_TryLock(&linkLock) == 0) {
            // The report matters more than whatever is still waiting.
            if (length > linkRing.size) {
                text += length - linkRing.size;
                length = linkRing.size;
            }
            u32 room = linkRing.size - ringUsed(&linkRing);
            if (length > room) linkRing.tail += length - room;
            ringPut(&linkRing, text, length);
            LightLock_Unlock(&linkLock);
            return;
        }
        if (msNow() >= deadline) return;
        svcSleepThread(WRITER_POLL_NS);
    }
}

static __attribute__((noinline, section(".fatalflush")))
void emergencyReport(const char* text, int waitMs, bool closeLink) {
    u64 deadline = msNow() + (u64) (waitMs > 0 ? waitMs : 0);
    // A spinning main thread (a hang) would keep these low-priority threads from ever running.
    if (writerThread != nullptr) svcSetThreadPriority(threadGetHandle(writerThread), 0x19);
    if (linkThread != nullptr) svcSetThreadPriority(threadGetHandle(linkThread), 0x19);

    // The SD writer may accept a record and then block in stdio or malloc.
    // Put the whole report on TCP first, independently of that acceptance.
    svcOutputDebugString(text, (s32) strlen(text));
    linkPutBefore(text, deadline);

    // The SD copy, in pieces that end at line ends. RECORD_TEXT does not
    // forward to TCP again, so a healthy writer doesn't duplicate the report.
    const char* rest = text;
    if (writerRunning && currentThreadId() != writerThreadId) {
        static char piece[RECORD_MAX];
        while (*rest != '\0') {
            size_t n = strlen(rest);
            if (n > RECORD_MAX - 64) {
                n = RECORD_MAX - 64;
                while (n > 1 && rest[n - 1] != '\n') n--;
                if (n <= 1) n = RECORD_MAX - 64;
            }
            memcpy(piece, rest, n);
            piece[n] = '\0';
            if (!postRecordBefore(RECORD_TEXT, piece, (u32) n + 1, deadline)) break;
            rest += n;
        }
        u32 number = 0;
        if (*rest == '\0' && LightLock_TryLock(&writerLock) == 0) {
            number = ++flushAsked;
            LightLock_Unlock(&writerLock);
            if (postRecordBefore(RECORD_FLUSH, &number, sizeof(number), deadline)) {
                while (writerRunning && !reached(flushDone, number) && deadline > msNow()) svcSleepThread(WRITER_POLL_NS);
            }
        }
    }
    // Fatal screens keep the connection for the heap walk and exit progress.
    // Lab crash/hang reports retain their existing close-after-report policy.
    if (linkThread != nullptr && linkRing.data != nullptr) {
        while (linkOpen && deadline > msNow() && ((writerRunning && ringUsed(&writerRing) > 0) || ringUsed(&linkRing) > 0 ||
                                                 __atomic_load_n(&linkSending, __ATOMIC_ACQUIRE))) {
            svcSleepThread(10 * 1000000LL);
        }
        if (closeLink) {
            linkStop = true;
            while (linkOpen && deadline > msNow()) svcSleepThread(10 * 1000000LL);
        }
    }
}

__attribute__((noinline, section(".fatalflush"))) void CtrLog_emergency(const char* text, int waitMs) {
    emergencyReport(text, waitMs, true);
}

__attribute__((noinline, section(".fatalflush")))
void CtrLog_emergencyKeepLink(const char* text, int waitMs) {
    emergencyReport(text, waitMs, false);
}

__attribute__((noinline, section(".fatalflush"))) void platformLog(const logType type, const char* format, va_list va) {
    uint64_t elapsedUs = CtrStartup_elapsedUs();
    u32 threadId = currentThreadId();
    if (type == LOG_TYPE_WARNING || type == LOG_TYPE_ERROR) {
        __atomic_add_fetch(&warningCount, 1, __ATOMIC_RELAXED);
        if (threadId == mainThreadId) {
            va_list copy;
            va_copy(copy, va);
            vsnprintf(lastWarning, sizeof(lastWarning), format, copy);
            va_end(copy);
        }
    }
    // The console belongs to the thread that opened the log, and not while it draws something else there.
    bool console = consoleEnabled && (logFile == nullptr || threadId == mainThreadId) && consoleHeld == 0;
    if (threadId != writerThreadId) {
        va_list copy;
        va_copy(copy, va);
        bool queued = postLine(type, console ? LINE_CONSOLE : 0, nullptr, elapsedUs, format, copy);
        va_end(copy);
        if (queued) {
            // Errors usually come right before a crash: wait until it is on the card.
            if (type == LOG_TYPE_ERROR) CtrLog_flush();
            return;
        }
    }

    char buffer[LOG_BUFFER_SIZE];
    int written = snprintf(buffer, sizeof(buffer), "%s", textPrefixOf(type));
    if (written >= 0 && written < (int) sizeof(buffer)) {
        vsnprintf(buffer + written, sizeof(buffer) - (size_t) written, format, va);
    }
    buffer[sizeof(buffer) - 1] = '\0';
    CtrStartup_stamp(buffer, sizeof(buffer), elapsedUs);
    writeLine(type, console ? LINE_CONSOLE : 0, nullptr, buffer);
}

uint32_t CtrLog_warningCount(void) {
    return __atomic_load_n(&warningCount, __ATOMIC_RELAXED);
}

const char* CtrLog_lastWarning(void) {
    return lastWarning;
}

// The writer's entry, for memory_report.c (heapfrag), which warms the writer's number caches before it starts. A
// pointer in the late constants, not an alias: an alias is a new sized symbol in the warm text, which
// check_layout.py rejects; this adds 4 B after the frozen rodata and moves nothing.
extern void (*const CtrLog_writerEntry)(void*);
__attribute__((section(".rodata.late.heapfrag"))) void (*const CtrLog_writerEntry)(void*) = writerMain;

// Assembly references keep GCC's existing private-BSS anchor groups intact.
// C initializers or new helpers touching these statics reorder their anchors
// and grow even unchanged flush functions. All new state access stays late.
// Keep the general body: assembly references alone are invisible to GCC's
// constant propagation once postLine delegates to the exit-aware producer.
__attribute__((used, section(".rodata.late.exithang")))
static bool (*const exitKeepPostRecord)(u16, const void*, u32, bool, bool) = postRecord;
__asm__(
    ".pushsection .rodata.late.exithang,\"a\",%progbits\n.balign 4\n"
    ".global CtrExit_logState\n.type CtrExit_logState,%object\n"
    "CtrExit_logState:\n"
    ".word writerRing,writerLock,writerEvent,writerThread,writerStop,writerRunning\n"
    ".word flushAsked,flushDone,consoleQueued,linkStop,logFile,postRecord,postRecordBefore\n"
    ".size CtrExit_logState,.-CtrExit_logState\n.popsection\n"
);

// Keep the original connector's call-graph facts and private storage unchanged.
__asm__(
    ".pushsection .rodata.late.exithang.files,\"a\",%progbits\n.balign 4\n"
    ".global CtrExit_filesLinkState\n.type CtrExit_filesLinkState,%object\n"
    "CtrExit_filesLinkState:\n.word linkThread,reportFileDecision,linkSocUp\n"
    ".size CtrExit_filesLinkState,.-CtrExit_filesLinkState\n.popsection\n"
);
