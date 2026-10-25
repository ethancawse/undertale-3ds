#ifndef _BS_SANDBOX_FILE_SYSTEM_H_
#define _BS_SANDBOX_FILE_SYSTEM_H_

#include "common.h"
#include "file_system.h"

// A file system that can shut the game off from the player's saves for a
// while, for performance tests that load rooms and battles the game's own way
// (which saves, writes ini files and reads save slots).
//
// Disabled, every call goes straight to the real file system. Enabled:
// * writes, deletes and renames only change files kept in memory;
// * reads see those files first, then the bundle (the game folder's own files,
//   read through bundleOnly), never the save area, so the game starts from no
//   saves and settings at all unless something writes them into the sandbox.
// Disabling forgets everything written in the sandbox.
//
// Handles from binaryOpen belong to this file system whichever mode it was in
// when they were opened, so a handle left open across a switch still works.

// Neither file system is owned. bundleOnly may be nullptr (reads of files not
// written in the sandbox then find nothing).
FileSystem* SandboxFileSystem_create(FileSystem* real, FileSystem* bundleOnly);
void SandboxFileSystem_destroy(FileSystem* fs);

void SandboxFileSystem_setEnabled(FileSystem* fs, bool enabled);
bool SandboxFileSystem_isEnabled(FileSystem* fs);
FileSystem* SandboxFileSystem_real(FileSystem* fs);

#endif /* _BS_SANDBOX_FILE_SYSTEM_H_ */
