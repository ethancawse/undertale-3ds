#ifndef CTRPORT_CTR_LOG_H
#define CTRPORT_CTR_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <3ds/console.h>

// Engine log output goes to the bottom-screen console (when one is active) and
// to a log file on the SD card. From CtrLog_openFile to CtrLog_closeFile a
// low-priority thread formats and writes the lines, in the order they were
// logged. Boot and warnings/errors may wait for the card; ordinary play-time
// messages may drop, with counts, to leave queue space for diagnostics.
void CtrLog_openFile(const char* path);
// Writes out every line still queued first.
void CtrLog_closeFile(void);
// Log lines are buffered; call this at points where losing the tail of the log
// to a crash would hurt (after init stages, before a fatal screen). Returns once
// every line logged before it is on the card.
void CtrLog_flush(void);
// The same without waiting: the writer flushes once it has written the lines
// before (the main loop's once-a-second flush).
void CtrLog_flushSoon(void);
// False during each chapter's startup, true after its first room is ready.
// Boot messages block instead of dropping. During play ordinary/debug lines
// leave an 8 KB warning/error reserve. Warnings/errors are never dropped from
// the writer queue. The separate best-effort network stream can still lose data.
void CtrLog_setFrameTimed(bool frameTimed);
// Whether log lines also print on the bottom screen's log console. The main
// loop sets it each frame: on only while that console is shown (the stats and
// log bottom mode, not the magnifier, whose screen the renderer owns).
void CtrLog_setConsoleEnabled(bool enabled);
bool CtrLog_consoleEnabled(void);
// Prints to console through the writer, behind the log lines before it, and not
// to the file (the bottom screen's stats). Arguments are copied, strings too.
void CtrLog_printTo(PrintConsole* console, const char* format, ...) __attribute__((format(printf, 2, 3)));
// Selects console in turn with the lines before and after: the console log
// lines print on from then on.
void CtrLog_selectConsole(PrintConsole* console);
// Around drawing on a console directly (menus, the performance test's screens,
// fatal screens): waits for the writer to print the lines queued for the
// console, then keeps it from printing until CtrLog_endConsole. Log lines
// logged meanwhile go to the file only. Main thread only; they nest.
void CtrLog_beginConsole(void);
void CtrLog_endConsole(void);

// When the Homebrew Launcher's network loader started the app (3dslink sets __3dslink_host), also sends every log
// line to that PC on port 17491 (tools/remote/run3ds.sh listens there). Returns false, and does nothing, otherwise.
// Lines are sent from a low-priority thread and dropped (with a note) if the PC falls 32 KB behind. A connection
// that breaks (a wifi drop) is made again, lines waiting meanwhile. CtrLog_closeFile sends what is left, then
// "[link] end of log", and stops it. With --send-files (ctr_link_files.h, enabled before this is called), the files the
// run wrote go between the last line and the end line, and the exit waits for them as long as bytes keep moving.
bool CtrLog_startLink(void);
// Called (from CtrLog_closeFile, on the calling thread) when the run's files start going to the PC and every second they
// move: lab mode's exit time limit starts again (CtrLab_exitProgress).
void CtrLog_setExitProgress(void (*hook)(void));
void CtrLog_stopLink(void);
// The PC's address as text, or nullptr when 3dslink didn't start the app.
const char* CtrLog_linkHost(void);
// True while lines are wanted: from CtrLog_startLink until the first connection never comes or the link stops.
bool CtrLog_linkOpen(void);
// Sends text to the PC only, not to log.txt or the screen (the performance test's results table).
void CtrLog_linkWrite(const char* text);

// For crash and hang reports (ctr_lab.h), safe to call from an exception handler or with other threads stuck: the
// last lines logged (up to size - 1 bytes, from a line start; returns the length), and a report written to log.txt
// and sent to the PC, after which the log connection is closed. Locks are only tried and no wait lasts past waitMs.
size_t CtrLog_tail(char* out, size_t size);
void CtrLog_emergency(const char* text, int waitMs);
// Same bounded direct TCP delivery, retaining the connection for later
// diagnostics and exit progress. Only for a fatal screen that will shut down.
void CtrLog_emergencyKeepLink(const char* text, int waitMs);

// Warnings and errors logged since the app started, from any thread, and the text of the main thread's last one
// (empty before it has one). The performance test's battle tour counts those logged in each battle.
uint32_t CtrLog_warningCount(void);
const char* CtrLog_lastWarning(void);

#endif
