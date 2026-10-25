#pragma once

#include <stdbool.h>

// Keeps a list of the files and folders open on the SD card, by path, and
// logs it with the process's kernel handle count. The FS service and the
// kernel limit how many can be open at once; past that every open fails
// (music, sounds, a chapter's data.win). Caches that keep files open after
// use (the texture cache's page files, the tile reader's) register here: they
// keep one more only while few are open, and close theirs when an open fails,
// which is then tried again.

// Puts the SD card's open and close calls through the list. Once, at startup.
void CtrOpenFiles_track(void);

// Kept-open files close once this many are open (the open_files_max setting;
// 0 keeps the default). For testing, opens past fakeMax fail as the FS
// service's do (open_files_fake_max; 0: off).
void CtrOpenFiles_setLimit(int limit, int fakeMax);

// Whether a cache may keep one more file open after use.
bool CtrOpenFiles_mayKeep(void);

// A cache of kept-open files, owned by the calling thread: shed closes them,
// on that thread only, at its CtrOpenFiles_poll (or within an open there that
// failed). wake, if not null, is called from other threads to have the owner
// poll soon. Returns an id for CtrOpenFiles_removeKeeper, or -1.
int CtrOpenFiles_addKeeper(void (*shed)(void* owner), void (*wake)(void* owner), void* owner);
void CtrOpenFiles_removeKeeper(int id);

// Closes the calling thread's caches' files if an open elsewhere asked for it.
// Call where none of them is in use.
void CtrOpenFiles_poll(void);

// Around opening a cache file: a failure there doesn't close the calling
// thread's caches (its caller may be using one of their files), so the
// caller closes those it can and tries again.
void CtrOpenFiles_beginCacheOpen(void);
void CtrOpenFiles_endCacheOpen(void);

// Whether an open that failed with errno err found nothing there, rather than
// too many files open.
bool CtrOpenFiles_missing(int err);

// One log line: files and folders open, and kernel handles in use (and the
// most at once). With list, also every path still open.
void CtrOpenFiles_log(const char* where, bool list);
