#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Set once for the app session, including across chapter changes. Read with
// __atomic_load_n(..., __ATOMIC_ACQUIRE); writers call CtrSafeSave_failed().
extern bool CtrSave_failed;
void CtrSafeSave_failed(void);

// No overwrite of the live file: stage/flush/close, rotate old to .bak, promote.
// Byte-identical writes do nothing. Failure retains old bytes at path or .bak.
bool CtrSafeSave_write(const char* path, const uint8_t* data, size_t size);
// Restore a stranded .bak only when the live file is absent (interrupted rename).
bool CtrSafeSave_recover(const char* path);
// Before an intentional delete/rename, prevent recovery from undoing it.
bool CtrSafeSave_forgetBackup(const char* path);
