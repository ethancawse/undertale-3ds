#ifndef _BS_BENCH_SCRIPT_H_
#define _BS_BENCH_SCRIPT_H_

// Scene scripts for performance tests: a short list of operations that put a
// running game into a given room, battle or boss phase without playing up to
// it, using the game's own scripts, rooms and variables. The 3DS performance
// test runs them from its scene list; the desktop runner runs one with
// --bench-script so a scene can be checked quickly.
//
// One operation per line; '#' starts a comment. Values are numbers, "strings"
// (with \n and \" escapes), true/false, asset names (rooms, objects, sprites,
// sounds, fonts, backgrounds, paths and scripts, resolved to their index) or
// variable references: global.name, global.name[i], object.name and
// object.name[i] (the first instance of the object, or all of them for set).
//
//   restart                 game_restart(); continues in the restarted game's first frame
//   wait <frames>
//   waitroom <room>         until the room is running (fails after 60 s, like every wait)
//   waitobj <object>        until an instance of the object exists
//   waitvar <ref> <value>   until the variable holds the value
//   goto <room>             room_goto(), then waits for the room
//   set <ref> <value>
//   call <function> [args]  a script (with or without its gml_Script_ prefix) or a
//                           builtin, run as the first instance in the room
//   callas <object> <function> [args]   the same, run as the first instance of object
//   create <object> <x> <y>
//   destroy <object>        every instance, with its Destroy event
//   file <name> <text>      writes a file through the game's file system
//   file <name> lines <values...>   the same, one value per line ("N*value" repeats it N times)
//   keys <keys...>          scripted input from now on: KEY to hold it, KEY/period to
//                           tap it every period frames, KEY/period:hold to hold each
//                           tap for hold frames; "keys" alone releases everything.
//                           KEY is Z, X, C, ENTER, SHIFT, CTRL, ESC, UP, DOWN, LEFT or RIGHT.
//   keep <operation>        runs the operation again every frame from now on
//   measure                 the scene is ready: measuring starts here. Operations after
//                           it run during the measurement (later phases, say).
//   log <text>
//   shot                    a screenshot of this frame (where the platform takes them)

#include "common.h"
#include "runner.h"

typedef struct BenchScript BenchScript;

typedef enum {
    BENCH_SCRIPT_SETTING_UP, // still working through the operations before "measure"
    BENCH_SCRIPT_MEASURING, // "measure" was reached (or the end of the script)
    BENCH_SCRIPT_FAILED, // see BenchScript_error
} BenchScriptState;

// Returns nullptr and sets *error (owned by the caller) when text has a syntax error.
BenchScript* BenchScript_parse(const char* text, char** error);
void BenchScript_free(BenchScript* script);

// Runs the script for one frame. Call it once per frame after the platform's input and before Runner_step.
// It presses and releases keys on runner->keyboard as its "keys" operation says.
BenchScriptState BenchScript_tick(BenchScript* script, Runner* runner);

// Observation only: the same tick and game state, with activity tags for its frame. SET keeps (HP/phase guards)
// are reported separately; other writes, warps and actual key presses/releases make a setup-active frame.
enum {
    BENCH_ACTIVITY_SEEK = 1, BENCH_ACTIVITY_WARP = 2, BENCH_ACTIVITY_WRITE = 4,
    BENCH_ACTIVITY_INPUT = 8, BENCH_ACTIVITY_GUARD = 16,
};
void BenchScript_observeBefore(const BenchScript* script);
unsigned BenchScript_observeAfter(const BenchScript* script);

// Releases every key the script holds.
void BenchScript_releaseKeys(BenchScript* script, Runner* runner);

// Replaces the key patterns, as a "keys" operation with these words would ("" releases everything). False when
// the words don't parse.
bool BenchScript_setKeys(BenchScript* script, Runner* runner, const char* keys);

// Runs one operation now (set, call, callas, create, destroy, file, keys or log), outside the script's own list.
// False, with a warning logged, when it doesn't parse or fails.
bool BenchScript_runNow(BenchScript* script, Runner* runner, const char* line);

// Why the script failed, or nullptr.
const char* BenchScript_error(const BenchScript* script);

// Whether a "shot" operation ran since the last call (which clears it).
bool BenchScript_takeShot(BenchScript* script);

// Frames since "measure" was reached (0 before).
int32_t BenchScript_framesMeasured(const BenchScript* script);

// DELTARUNE battles: the first encounter id from from to last (inclusive) that the game's scr_encountersetup
// defines, or -1 (also when the game has no such script). It runs scr_encountersetup for each id, which sets the
// enemy slots' globals (global.monstertype, global.monsterinstancetype, ...) and the battle's opening text, which
// starting a battle sets again. names gets
// the encounter's enemy object names, joined with '+'.
int32_t BenchScript_findEncounter(Runner* runner, int32_t from, int32_t last, char* names, size_t size);

// ===[ Variable references ]===
// A variable reference as the operations take it (global.name, global.name[i], object.name, object.name[i]), for
// code that reads or sets one itself. The object is looked up when first used.

typedef struct BenchRef BenchRef;

// nullptr when text isn't a variable reference.
BenchRef* BenchRef_parse(const char* text);
void BenchRef_free(BenchRef* ref);
// The value (owned by the caller), from the object's first instance; false when there is none.
bool BenchRef_get(BenchRef* ref, Runner* runner, RValue* out);
// The value as a number; false when there is no instance or the value isn't a number.
bool BenchRef_getReal(BenchRef* ref, Runner* runner, double* out);
// The value as text ("-" when there is no instance).
void BenchRef_format(BenchRef* ref, Runner* runner, char* out, size_t size);
// Sets the variable on every instance of the object; false when there is none.
bool BenchRef_setReal(BenchRef* ref, Runner* runner, double value);

// ===[ Scene lists ]===
// A scene file holds several scripts:
//
//   scene <id> <title...>
//   chapter <n>          optional: the DELTARUNE chapter whose data.win the scene needs
//   <operations>
//   include <id>         the operations of an earlier scene, in place
//
// Lines before the first "scene" are ignored. Scenes whose id starts with '_' are
// only there to be included and aren't listed.
//
//   battle <id> <title...>
//
// starts one of the battle sweep's battles instead (ctr_benchmark.h): a scene
// whose script may also hold "sweep ..." lines, which the sweep reads and
// BenchScript_parse skips.

typedef struct {
    char* id;
    char* title;
    int32_t chapter; // 0 = any
    bool battle; // a battle sweep battle ("battle" rather than "scene")
    char* script; // the scene's operations, as text
} BenchScene;

// Returns an stb_ds array of scenes. Free it with BenchScene_freeList.
BenchScene* BenchScene_parseList(const char* text);
void BenchScene_freeList(BenchScene* scenes);

#endif
