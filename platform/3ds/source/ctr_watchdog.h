#pragma once

// Hang reports for testing on hardware and in emulators.
//
// A high-priority thread wakes once a second. When the main loop hasn't
// passed a CtrWatchdog_mark() for HANG_SECONDS it writes what the main thread
// was doing to <gameDir>/hang.txt: the last phase marked, the room and frame,
// and a few samples of the VM's current code, instruction pointer and call
// stack (so a GML infinite loop shows a moving ip, native code a frozen one).
// It also flushes the main log, whose buffered tail would otherwise be lost
// when the app is killed. The report itself is written without the heap or
// stdio, so a main thread stuck holding their locks can't block it. Time spent in the HOME Menu or sleep mode doesn't
// count, and those transitions are logged. Reads of the runner are unsynchronised and only
// ever happen while the main thread looks stuck; strings are range-checked
// before they are printed.

#include "runner.h"

void CtrWatchdog_start(const char* reportPath);
void CtrWatchdog_stop(void);

// The game the main thread is running, or nullptr while it loads/unloads one.
void CtrWatchdog_watch(Runner* runner);

// Called by the main loop as it moves between phases of a frame; phase must
// be a string literal.
void CtrWatchdog_mark(const char* phase);

// Worker threads name themselves once and note what they are doing as they
// go (waiting for work, decoding, reading a file...), so a hang report shows
// each thread's last state and how long it has kept it, and which thread
// holds the heap lock if one does. A thread started again under the same
// name takes over its predecessor's line. Both strings must be literals.
// A note is two stores, but it belongs at state changes, not in inner loops.
void CtrWatchdog_nameThread(const char* name);
void CtrWatchdog_note(const char* state);

// For lab mode's hang detection and reports (ctr_lab.h), readable from any thread without locks: a count that moves
// with every mark, whether it should be moving (a game is running and the app isn't in the HOME Menu or asleep), the
// last phase marked, a named thread's name (nullptr if it never named itself), and the last hang report (nullptr if
// none was written).
uint32_t CtrWatchdog_progress(void);
bool CtrWatchdog_watching(void);
const char* CtrWatchdog_phase(void);
const char* CtrWatchdog_threadNameOf(const void* tls);
const char* CtrWatchdog_lastReport(void);
