#ifndef CTRPORT_CTR_EXIT_H
#define CTRPORT_CTR_EXIT_H

#include <stdbool.h>
#include <stdint.h>

// Exit-only replacements. Existing teardown order and function slots stay put.
// The deadline trigger is created before gameplay, without a heap trampoline.
// Word-sized so this late-BSS object's final flag cannot leave the 3DSX data
// segment with a non-word-sized p_memsz (3dsxtool rejects that segment).
extern uint32_t CtrExit_started;
void CtrExit_openLog(const char* path);
void CtrExit_begin(int seconds);
void CtrExit_logBegin(const char* format, ...);
void CtrExit_flush(void);
void CtrExit_closeLog(void);

// Exit-only file delivery: a connected peer gets one bounded batch.
int CtrExitFiles_connected(int sock, int command, int flags);
int CtrExitFiles_shutdown(int sock, int how);
int CtrExitFiles_shutdownDrained(int sock, int how);
void CtrExit_closeFilesLog(void);
void CtrExitFiles_openLog(const char* path);
void CtrExitFiles_exitDeadline(void) __attribute__((noreturn));
void CtrExitFiles_closeFailed(void) __attribute__((noreturn));
void CtrExitFiles_labExit(void) __attribute__((noreturn));
void CtrExitFiles_startFailed(void) __attribute__((noreturn));

#endif
