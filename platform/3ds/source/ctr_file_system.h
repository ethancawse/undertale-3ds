#pragma once

// The game's file system on the 3DS: the engine's overlay file system (saves
// over the bundle folder) behind two speedups for the SD card, where a small
// write takes 100 ms or more and checking that a file exists several ms.
//
// - Whole-file writes (ini_close, file_text_close, buffer_save) are queued and
//   done by a background thread in order, so the game never waits on them.
//   Reading a file that is still queued returns the queued contents.
// - fileExists answers are remembered until a change through this file
//   system could affect them. Nothing else writes the game folders while the
//   game runs.
// - The save folder is listed once at start and the listing kept up to date,
//   so checking for a save file costs nothing, and a plain name missing from
//   the save folder is looked for in the bundle folder only. When the writer
//   has nothing to write it reads the save files (up to 1 MB in all) into
//   memory, so loading a save doesn't wait on the card either. Opening a file
//   on the card costs 35-75 ms in Azahar.
//
// Writable file_bin handles buffer their changes until close, then enter the
// same queue. Writes stage and flush a temporary file before renaming, keeping
// one .bak of the previous changed contents. A stranded .bak is restored at
// startup when the live file is absent. Read-only file_bin access still streams.
//
// Every other operation (deletes, renames, file_bin open, folder
// operations) first waits for the queue to empty, so the order of changes on
// the card always matches the order the game made them.

#include "file_system.h"

FileSystem* CtrFileSystem_create(const char* bundlePath, const char* savePath);
// Waits for every queued write, then frees everything.
void CtrFileSystem_destroy(FileSystem* fs);
// Waits for every queued write to reach the SD card.
void CtrFileSystem_flush(FileSystem* fs);
