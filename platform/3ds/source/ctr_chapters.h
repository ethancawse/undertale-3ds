#pragma once

// Which data.win runs, and switching between them with game_change().
//
// DELTARUNE ships a launcher data.win plus one data.win per chapter in
// chapterN_windows/ folders. The launcher starts a chapter with
// game_change("/chapter1_windows", "-game data.win <tokens>") and a chapter
// goes back with game_change("/../", ...). The original runner starts a new
// process; here the frontend unloads everything and loads the next data.win,
// keeping the same save directory, and forwards the launch parameters to the
// next game's parameter_string(). Undertale never calls game_change(), so for
// it this is just "run <game dir>/data.win".

#include <stdbool.h>

#include "runner.h"

// The strings are the session's own (static storage, replaced by the next
// game_change()); copy them to keep them.
typedef struct {
    const char* dataWinPath; // full path of the data.win to run
    const char* bundleDir;   // its directory: working_directory, external audio, lang files
    char** gameArgs;         // stb_ds array for parameter_string(); [0] is the program name
    int32_t loadCount; // how many data.win files this session has started (1 = first boot)
} CtrGameSession;

// Picks the first data.win: <gameDir>/data.win, unless <gameDir>/boot.txt asks
// for another one (a test hook; see docs/deltarune.md).
void CtrChapters_init(CtrGameSession* session, const char* gameDir);
// Return to the launcher, ignoring the optional boot.txt shortcut.
void CtrChapters_returnToLauncher(CtrGameSession* session, const char* gameDir);

// True once game code has called game_change(); the frame loop should stop.
bool CtrChapters_changeRequested(const Runner* runner);

// Turns the runner's pending game_change() into the next session: new
// data.win path and launch parameters. Returns false (and leaves the session
// alone) when there is no request or it can't be understood.
bool CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner);

// Frees everything the running game owns (audio, renderer, runner, file
// system, VM, data.win) so the next one can load. Call outside a GPU frame.
void CtrChapters_unloadGame(Runner* runner);

void CtrChapters_free(CtrGameSession* session);
