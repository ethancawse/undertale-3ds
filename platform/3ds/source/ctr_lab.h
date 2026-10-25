// Lab mode: unattended test runs that end cleanly whatever happens. See the top of ctr_lab.c for how it works.
#ifndef CTRPORT_CTR_LAB_H
#define CTRPORT_CTR_LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* appName;    // "the port UNDERTALE"
    const char* buildId;    // the commit
    const char* hostPath;   // the lab host's SD card path ("sdmc:/3ds/x.3dsx" or "/3ds/x.3dsx"); nullptr: LAB_HOST_DEFAULT
    const char* reportPath; // where crash and hang reports go ("sdmc:/3ds/undertale-3ds/crash.txt")
    int hangSeconds;        // leave when progress hasn't moved for this long while watching; 0: never
    int deadlineSeconds;    // leave this long after CtrLab_start whatever happens; 0: never
    int bootSeconds;        // leave if CtrLab_booted() hasn't been called this long after CtrLab_start; 0: never
    // All optional, and called from the thread that leaves (maybe in an exception handler: they must not allocate or
    // wait on locks without a time limit).
    size_t (*logTail)(char* out, size_t size);          // the last lines logged, for the report
    void (*emergencyLog)(const char* text, int waitMs); // the report to the log and the PC; then sends what is left
    uint32_t (*progress)(void);                         // moves while the app is alive (the main loop's marks)
    bool (*watching)(void);                             // whether progress should be moving now
    const char* (*phase)(void);                         // what the main thread last said it was doing
    const char* (*threadName)(const void* tls);         // a thread's name, or nullptr
    const char* (*hangDetails)(void);                   // more for a hang report (the watchdog's), or nullptr
    const char* (*bootStep)(void);                      // how far the start got (before CtrLab_booted), or nullptr
    void (*trail)(const char* text);                    // one line for the boot trail (ctr_boot_trail.h) on leaving
} CtrLabConfig;

// Installs the exception handlers and the guard thread. Call it from the main thread, as early as possible, with a
// config that stays valid. Returns false (and does nothing) if called twice.
bool CtrLab_start(const CtrLabConfig* config);
// The start is over (the log is open and the watchdog running): bootSeconds no longer applies.
void CtrLab_booted(void);
bool CtrLab_active(void);
// Whether the lab host's file is there and loadable (else runs end in the Homebrew Launcher's menu, as without lab
// mode).
bool CtrLab_hostReady(void);

// Runs entry(arg) with this thread's own exception handler entry, so two threads that crash at once don't share one.
// ctr_memory.c's thread trampoline calls it for every thread; without lab mode it just calls entry.
void CtrLab_runThread(void (*entry)(void*), void* arg);

// What a normal exit after this means, for the lab host's screen and its next app ("exit" unless set: "error" after
// an error screen, ...). A string literal.
void CtrLab_setExitNote(const char* note);
// The app has started its own exit: hang detection stops, and if the process is still there this long afterwards
// it leaves as for a hang.
void CtrLab_exiting(int seconds);
// The exit is still getting somewhere (the run's files going to the PC, ctr_link_files.h): its time limit starts again.
// Any thread.
void CtrLab_exitProgress(void);
// Main loop: once a crash on another thread is being handled, the main thread stops here (and so leaves APT alone).
void CtrLab_poll(void);

// A failure the code itself noticed (abort(), say): reported and left as a crash is. Never returns.
void CtrLab_fail(const char* kind, const char* detail) __attribute__((noreturn));

// --lab-crash: crash on purpose ("data", "prefetch", "undef", "thread", "break", "abort", "hang"). Returns only for
// an unknown kind.
void CtrLab_crashTest(const char* kind);

// The guard thread's sleep (ctr_lab.c, late code): svcSleepThread, then parked in the kernel for good if this thread's
// stack has been unmapped meanwhile (libctru's exit frees the heap before the process ends).
void CtrLab_guardSleep(long long ns);

#endif
