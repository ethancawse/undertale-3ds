// Performance test, started from the options menu: runs scenes from the game
// (rooms, boss fights) one after another, measures them and shows and saves
// the results.
//
// The scenes are scene scripts (engine/src/bench_script.h) compiled in from
// platform/3ds/bench/<game>.txt; a benchscenes.txt in the game folder replaces
// them, for trying scenes out without a rebuild. Each scene sets itself up the
// game's own way (loading a save it writes, starting a battle as the game's
// triggers do), then gets 3 s to settle and is measured for 20 s.
//
// The player's saves are never read or written during a test: the game's file
// system is switched to a sandbox (sandbox_file_system.h) that shows only the
// game folder's own files and keeps everything written in memory. Afterwards
// the sandbox is dropped and the game restarts from its title (UNDERTALE) or
// returns to the chapter it was in (DELTARUNE, whose scenes each start their
// chapter afresh with game_change()). Results are appended to
// <game folder>/benchmark.txt.
#ifndef CTRPORT_CTR_BENCHMARK_H
#define CTRPORT_CTR_BENCHMARK_H

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#include "ctr_display.h"
#include "file_system.h"
#include "runner.h"

// What the frontend's current game session is made of. The test outlives a
// session when DELTARUNE changes chapter, so it is told about each new one.
typedef struct {
    Runner* runner;
    Renderer* renderer;
    FileSystem* sandbox;    // the SandboxFileSystem the runner uses
    FileSystem* saves;      // the CtrFileSystem under it
    const char* bundleDir;  // the running data.win's folder
    const char* gameDir;    // CTRPORT_GAME_DIR
    char** gameArgs;        // stb_ds array, [0] is the program name
    CtrDisplaySettings display;
    bool isNew3ds;
} CtrBenchSession;

typedef enum {
    BENCH_FRAME_RUN,    // step and draw the game as usual
    BENCH_FRAME_PAUSE,  // a screen of the test has the bottom screen; keep the game paused
} CtrBenchFrame;

// Call once a game session's first room is ready. Enables the sandbox if a
// test is running.
void CtrBench_sessionStarted(const CtrBenchSession* session);

// The scene list on the bottom screen (console covers all of it), from the
// options menu. While it is open the game stays paused. display is what the
// results will say the test ran with.
void CtrBench_openPicker(PrintConsole* console, const CtrDisplaySettings* display);
bool CtrBench_pickerOpen(void);

// Test hook for automated runs: starts a test with the scenes a file lists
// (scene ids, or "all"; "done" also leaves the results screen by itself after
// 5 s, "shots" takes a screenshot halfway through each measurement). Returns
// false if the file doesn't exist or names no scenes.
//
// "tour" instead visits every room of the game (DELTARUNE: Chapters 1-5) in
// one run, tools/aot/corpus.py's room sweep's rooms: each chapter started
// afresh once, then room_goto to each room, 1 s to settle and 10 s measured.
// Every room's numbers are appended to <game folder>/tour.csv as soon as it is
// measured, and tour.pos says where the tour is. "tour=C:I" starts at chapter
// C's room I (data.win room index), "tour=C:I-C:I" also stops after a room,
// "tour=C-C" takes whole chapters, and "tour=resume" carries on after the room
// tour.pos names: a room the last run never finished (it crashed or hung
// there) gets a row saying so and is skipped.
//
// "tour=battles" is DELTARUNE's battle tour: every encounter id each
// chapter's scr_encountersetup defines, started with scr_battle (Chapter 1:
// obj_encounterbasic) in a quiet room of the chapter with the party's and the
// enemies' HP held and FIGHT picked, 1 s to settle, 10 s measured, then ended
// with scr_endcombat. Rows go to battles.csv (enemy names and the warnings
// logged while measuring too) and battles.pos; "tour=battles:SPEC" takes the
// forms above, I being an encounter id ("tour=battles:resume").
//
// "sweep" is the battle sweep: the "battle" entries of
// bench/sweep_<game>.txt (after the scenes in the same list), each a script
// that starts one battle plus a recipe of "sweep" lines, fought to the end or
// through every attack with scripted menu keys. Nobody's HP is held: hits
// land, but every party member is back at full HP as each enemy turn begins
// and whenever their HP falls to the recipe's floor, so nobody is downed. The
// recipe can pin variables to a value per turn (a boss's turn counter, say) so
// every attack is drawn, then lets the battle run on its own logic for a few
// turns. The battle is measured in segments: "enter" (from its start to the
// first enemy turn, which pays for the loading), each "player" turn, each
// "attack" and any "transition"; every segment is a row of
// <game folder>/sweep.csv, appended as it ends (attack label, pinned values,
// fps, 1% low, worst frame, step/draw/GPU times, texture loads, dropped draws,
// hits taken, the objects created most), and the log gets a summary line per
// battle (worst segment, lowest fps and 1% low). Frames whose work takes 50 ms
// or more are saved as sweep_shots/<battle>_tNN_<segment>.bmp (the
// application surface; at most 3 per segment, the frame that saves one left
// out of the numbers) unless "noshots" is given. sweep.pos says which battle
// is being fought. "sweep=SPEC" picks battles: ids separated by commas, each
// "id", "prefix*" or "id.." (that battle and every one after it); and
// "sweep=resume" carries on after the battle sweep.pos names, fighting a battle
// the last run never finished once more before leaving it out.
bool CtrBench_autorun(const char* path, PrintConsole* console, const CtrDisplaySettings* display);
// The same with the file's words given as text (launch arguments' --bench); source names them in the log.
bool CtrBench_autorunList(const char* words, const char* source, PrintConsole* console, const CtrDisplaySettings* display);

// True once after each test ends and its results are saved (to leave the app when --exit-after asked to).
bool CtrBench_takeFinished(void);

// True once a test has gone back to the game, since the app started.
bool CtrBench_returnedToGame(void);

// True from starting a test until its results are dismissed.
bool CtrBench_running(void);

// Every frame, after input is read and before the game would step: drives the
// picker, the scenes and the results screen. down is this frame's new presses,
// frameMs the time since the previous frame started.
CtrBenchFrame CtrBench_beginFrame(u32 down, double frameMs);

// After the frame (step, draw, room change) while a test is running.
void CtrBench_endFrame(double stepMs, double drawMs, bool skipped);

// Whether the test wants the bottom screen for its status (the frontend yields it).
bool CtrBench_wantsBottomScreen(void);

// utpolish (late code, z_utpolish_bench.c): CtrBench_sessionStarted, CtrBench_beginFrame and CtrBench_endFrame,
// and a log line for each frame over two vblanks (33.4 ms) while a test runs, with its Step, Draw, GPU and texture time.
void CtrBench_sessionStartedLate(const CtrBenchSession* session);
CtrBenchFrame CtrBench_beginFrameTimed(u32 down, double frameMs);
void CtrBench_endFrameTimed(double stepMs, double drawMs, bool skipped);

// Reporting-only hidden room-load observer, then the unchanged runner handler.
void CtrBench_handleRoomChange(Runner* runner);
// roomload's room-entry attribution trace: compiled into profile builds only (z_roomload_trace.c).
#ifdef CTRPORT_RENDER_TIMING
#include "z_roomload_trace.h"
#define ROOMLOAD_TRACE_FRAME(runner, renderer, frameStart, stepMs, drawMs) RoomloadTrace_frame((runner), (renderer), (frameStart), (stepMs), (drawMs))
#else
#define ROOMLOAD_TRACE_FRAME(runner, renderer, frameStart, stepMs, drawMs) ((void) 0)
#endif

#endif
