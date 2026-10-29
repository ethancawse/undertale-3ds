#include "ctr_benchmark.h"

#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "bench_script.h"
#include "gettime.h"
#include "instance.h"
#include "log.h"
#include "sandbox_file_system.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

#include "ctr_audio_system.h"
#include "ctr_chapters.h"
#include "ctr_file_system.h"
#include "ctr_frame_pacer.h"
#include "ctr_gpu_queue.h"
#ifdef CTRPORT_HEAP_PROFILE
#include "ctr_heap_profile.h" // heap marks at the bench scene boundaries (heap builds only)
#endif
#line 27
#include "ctr_log.h"
#include "ctr_renderer.h"

#include "ctr_benchromfs.h"
#include "ctr_build_id.h" // CTRPORT_COMMIT
#include "ctr_benchmark_rows.h"
#line 32
static BenchScriptState benchPreTick(BenchScript*, Runner*); static void benchPreOpen(void); static BenchScriptState benchRoomsTick(BenchScript*, Runner*); static void benchRoomsOpen(void); static bool benchRoomsTakeShot(BenchScript*); static void benchfidRoomsOpen(void); static void benchfidRoomsCollect(Runner*); static void benchfidRoomsReport(void); static void benchshownRoomsOpen(void); static void benchshownRoomsCollect(Runner*); static void benchshownRoomsReport(void);
#define WARMUP_SECONDS 3.0
#define MEASURE_SECONDS 20.0
// With bench_frames (CtrDisplaySettings.benchFrames): game frames a scene settles for after it is ready, about the
// 3 s of WARMUP_SECONDS at 30 fps.
#define BENCH_SETTLE_FRAMES 90
// A room tour's rooms: the first second (the room's load) left out, then 10 s measured.
#define TOUR_WARMUP_SECONDS 1.0
#define TOUR_MEASURE_SECONDS 10.0
#define SCENE_FILE_NAME "benchscenes.txt"
#define RESULTS_FILE_NAME "benchmark.txt"
#define TOUR_FILE_NAME "tour.csv"
#define TOUR_POSITION_FILE_NAME "tour.pos"
// A battle tour's, the same way.
#define BATTLE_FILE_NAME "battles.csv"
#define BATTLE_POSITION_FILE_NAME "battles.pos"
// The highest encounter id a battle tour looks for (the chapters' go up to 777, a test battle).
#define BATTLE_LAST_ENCOUNTER 999
// After each battle's measurement: frames for the end of the battle (scr_endcombat, obj_endbattle) to play out.
#define BATTLE_END_FRAMES 60
// Results a tour keeps for the status screen; the rest are only in tour.csv.
#define TOUR_RESULTS_KEPT 16
// What the DELTARUNE launcher passes when it starts a chapter.
#define CHAPTER_LAUNCH_PARAMETERS "-game data.win launcher switch_-1 returning_0"
// Rows of the bottom screen console (40x30) used by lists.
#define LIST_ROWS 17
// The battle sweep (ctr_benchmark.h): its rows, where it is, and its screenshots, in the game folder.
#define SWEEP_FILE_NAME "sweep.csv"
#define SWEEP_POSITION_FILE_NAME "sweep.pos"
#define SWEEP_SHOT_DIR "sweep_shots"
// Pinned variables, attack labels and party members a battle can have, and keys in its player turn's sequence.
#define SWEEP_PINS 4
#define SWEEP_LABELS 3
#define SWEEP_PARTY 4
#define SWEEP_SEQUENCE 16
// A sequence's keys: held this many frames, one every SWEEP_KEY_PERIOD frames.
#define SWEEP_KEY_HOLD 3
#define SWEEP_KEY_PERIOD 8
// Turns played on the battle's own logic after the pinned ones, and the most turns a battle gets.
#define SWEEP_FREE_TURNS 2
#define SWEEP_TURN_CAP 40
// A party member's HP at or below this share of their maximum goes back to full.
#define SWEEP_HP_FLOOR 0.5
// A battle, or one segment, that takes longer is stuck.
#define SWEEP_BATTLE_SECONDS 1500.0
#define SWEEP_SEGMENT_SECONDS 180.0
// A frame whose work took this long is worth a screenshot (a new worst, at most this many per segment).
#define SWEEP_SHOT_MIN_MS 50.0
#define SWEEP_SHOTS_PER_SEGMENT 3
// Objects named in a row's top_objects, and the shortest segment the battle's summary counts.
#define SWEEP_TOP_OBJECTS 4
#define SWEEP_MIN_SUMMARY_FRAMES 15
// "sweep=resume" starts a battle that ended the last run again, but not one that ended this many.
#define SWEEP_MAX_TRIES 2

typedef enum {
    PHASE_IDLE,
    PHASE_PICKER,
    PHASE_NEXT,     // about to start the next scene in the queue
    PHASE_LOADING,  // waiting for the chapter a scene needs to load
    PHASE_SETUP,    // the scene script is working towards "measure"
    PHASE_WARMUP,
    PHASE_MEASURE,
    PHASE_RESULTS,
} Phase;

// The parts of a battle the sweep measures separately.
typedef enum {
    SEG_ENTER, // from the battle's start to its first change of state: pays for the loading
    SEG_PLAYER, // the player's turn: the menus, the player's action, the enemy's words
    SEG_ATTACK, // the enemy's attack
    SEG_TRANSITION, // a phase change or cutscene, as the recipe tells it
} SweepSegment;

typedef struct {
    char id[32];
    char title[48];
    char room[40];
    char status[96]; // why the scene has no numbers; empty when it was measured
    bool measured;
    int32_t frames;
    double seconds;
    double fpsAvg, fpsLow1, worstMs;
    double shownFps; // frames that reached the screen per second; fpsAvg is the game's speed
    // The same over frames taken two at a time (each pair's time halved), which fixed 2:1 frame skip shows as one:
    // its frames alternate between short (not shown, no wait) and long (shown, after the wait for both).
    double pairLow1, pairWorstMs;
    double stepAvg, stepMax, drawAvg, drawMax, gpuAvg, gpuMax;
    double smartBoxes, smartPixels; // smart smoothing's boxes and their surface pixels a measured frame
    uint32_t texLoads, texEvictions;
    uint32_t texImageLoads; // pages decoded from their images: the texture cache wasn't ready
    double texLoadMs;
    uint32_t underruns;
    double heapPeakMB, linearMinFreeMB, vramMinFreeKB;
    double nativeShare; // of the frames shown, those drawn at half resolution (native drawing)
    double setupSeconds;
    double loadMs; // the longest frame while settling: for a tour's room, its load
    uint32_t warnings; // warnings and errors logged while measuring
    char lastWarning[96];
    uint32_t drawsDropped; // draws on the application surface frame skip left out
    int32_t skippedFrames; // frames frame skip left unshown
    // While frames were skipped: the top screen's flips by how many vblanks each stayed up (CtrFlipStats), their mean
    // and standard deviation, the main thread's waits for the GPU a frame, and the skipped frames that queued GPU work
    // and were handed over behind the frame before (frame_skip_overlap).
    CtrFlipStats flips;
    double flipMean, flipSd, gpuWaitMs;
    uint32_t workFrames, workDraws, workFills, workCopies, behind;
} SceneResult;

// Room tour: every room of the game, one after another, in one run. Its rooms, skip list and start of each chapter
// are tools/aot/corpus.py's room sweep's (room_names, ROOM_SKIP, DR_ROOM_BOOT), so the two visit the same rooms.
// A battle tour (DELTARUNE) goes the same way through every encounter id each chapter's scr_encountersetup
// defines instead: room is then the encounter id.
static struct {
    bool on;
    bool battles;
    bool deltarune;
    int32_t chapter, room;           // the next room to visit (chapter 0: UNDERTALE)
    int32_t lastChapter, lastRoom;   // where to stop, inclusive (lastRoom -1: the chapter's last)
    bool booting;                    // the chapter's start steps are running
    bool booted;                     // they ran in the loaded game
    bool ending;                     // a battle is being ended after its measurement
    int32_t visited;
    uint32_t warningsAtStart;
    char enemies[96];                // the battle's enemies
    char script[1536];
} tour;

// Rooms the tour leaves out, as corpus.py's ROOM_SKIP (case ignored, name prefixes): debug and start-up rooms, and
// intros, endings, battles and shops, which only work when the game itself sends the player there.
static const char* const tourSkipPrefixes[] = {
    "room_gms_debug", "PLACE_", "ROOM_INITIALIZE", "room_intro", "room_legend", "room_ed", "room_gameover", "room_shop",
    "room_battle", "room_start", "room_end", "room_credits",
};

// How the tour starts each chapter afresh: a state from which room_goto works (corpus.py's DR_ROOM_BOOT; for
// UNDERTALE, its save in the Ruins and boot).
static const char* const tourBootSteps[] = {
    "file file0 lines \"Frisk\" 1 99 0 10 0 10 0 4 0 0 0 1 201 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 4 512*0 3 1 1 1 0 6 0\n"
    "restart\nwaitroom room_introstory\ncall scr_load\nwaitroom room_ruins1\n",
    "waitroom PLACE_CONTACT\n",
    "waitroom room_intro_ch2\nset global.char[0] 1\nset global.char[1] 2\nset global.char[2] 3\n",
    "wait 30\ncall scr_gamestart\ncall scr_gamestart_chapter_override\nset global.char[0] 1\nset global.char[1] 2\n"
    "set global.char[2] 3\n",
    "wait 30\n",
    "wait 30\n",
};
#define TOUR_LAST_CHAPTER ((int32_t) (sizeof(tourBootSteps) / sizeof(tourBootSteps[0])) - 1)

// Where the battle tour fights each chapter's battles: rooms the bench scenes' battles already use, quiet at a
// chapter's start (the room's own cutscene or chasers destroyed), gone to afresh for every battle.
static const char* const battleRooms[] = {
    nullptr,
    "goto room_field1\ndestroy obj_chaseenemy\n",
    "goto room_dw_city_berdly\ndestroy obj_ch2_city_berdly\n",
    "set global.plot 330\ngoto room_dw_snow_zone\n",
    "goto room_dw_churchc_encounter2\ndestroy obj_dw_church_organikchaser\n",
    "goto room_dw_cliff_kawkawdash\n",
};

static bool tourPastEnd(void);
static void tourBootEnded(const char* reason);
static void tourRecord(const SceneResult* r);
static void tourStartBoot(void);
static void tourBattleEnded(const char* reason);
static void tourStartBattleEnd(void);
static void computeResult(SceneResult* r); static void benchRoomsRelease(void* buffer);
static void logResult(const SceneResult* r);
static void sweepAbandonBattle(const char* reason);

static struct {
    Phase phase;
    CtrBenchSession session;
    PrintConsole* console;
    BenchScene* scenes; // stb_ds array, the listed scenes (helpers left out)
    int32_t cursor;     // picker: 0 = run all, 1.. = scenes; results: row
    int32_t scroll;
    int32_t* queue;     // stb_ds array of indexes into scenes
    int32_t queuePos;
    BenchScript* script;
    SceneResult* results; // stb_ds array
    uint64_t phaseStart;
    uint64_t lastStatus;
    int32_t readyFrame, measureStartFrame; // the game frames the scene was ready at and measuring began at
    bool changeRequested; // a game_change of ours is on its way
    // Measuring
    float* frameMs;       // stb_ds array
    int32_t measureFrames;
    int32_t skippedFrames; // frame skip left them unshown
    double stepSum, drawSum, gpuSum, stepMax, drawMax, gpuMax;
    CtrRenderStats statsAtStart;
    uint32_t droppedAtStart;
    CtrSkipStats skipAtStart;
    CtrFlipStats flipsAtStart;
    // Auto's levels at the scene's start (CtrPacer_logLevels), and when.
    CtrLevelTotals levelsAtStart;
    uint64_t levelsTick;
    uint64_t gpuWaitAtStart;
    uint32_t underrunsAtStart;
    uint32_t heapPeak, linearMinFree, vramMinFree;
    // Where to go back to afterwards
    char* originalBundleDir;
    char* originalParameters;
    char model[24];
    char startedAt[32];
    bool cancelled;
    bool saved;
    bool finished;    // a test ended and its results were saved; not yet taken by CtrBench_takeFinished
    bool returned;    // a test has gone back to the game since the app started
    bool autoDismiss; // benchrun.txt said "done": leave the results by themselves
    bool screenshots; // benchrun.txt said "shots": a screenshot halfway through each measurement
    bool shotTaken;
    aptHookCookie aptCookie;
    bool aptHooked;
    volatile bool interrupted; // the HOME Menu or sleep took over since the last frame
} bench;

// The battle sweep's state while a battle is fought: its recipe (the "sweep" lines of its script) and where it is.
typedef struct {
    BenchRef* ref;
    double* values; // stb_ds
} SweepPin;

typedef struct {
    int32_t turn;
    char* op;
} SweepAt;

static struct {
    bool on;
    char selection[256]; // what the sweep word asked for, for the log
    // The recipe
    SweepPin pins[SWEEP_PINS];
    int32_t pinCount;
    int32_t pinTurns; // the longest pin list
    BenchRef* labels[SWEEP_LABELS];
    int32_t labelCount;
    BenchRef* state; // the battle's state; the enemy attacks while it holds attackValue
    double attackValue;
    BenchRef* transition; // a phase change or cutscene while it holds transitionValue
    double transitionValue;
    BenchRef* endRef; // the sweep ends the battle once it holds endValue
    double endValue;
    BenchRef* follow; // a fight without turns: each new value of it is a new attack
    double followValue;
    bool followRead; // followValue holds a value
    int32_t aliveObject; // the battle goes on while an instance of it exists
    char aliveName[48];
    char* playerKeys[SWEEP_SEQUENCE]; // pressed one after another at the start of each player turn
    int32_t playerKeyCount;
    char playerMash[64]; // then this pattern
    char attackKeys[64];
    int32_t freeTurns, turnCap;
    double hpFloor;
    SweepAt* atOps; // stb_ds
    // The party's HP
    BenchRef* hp[SWEEP_PARTY];
    BenchRef* maxHp[SWEEP_PARTY];
    // DELTARUNE: a hit that takes a hero to 0 HP downs them (global.chardead, charmove and charcantarget of their
    // party slot), which HP alone doesn't undo: these are set back as scr_revive would, or nullptr.
    BenchRef* revive[SWEEP_PARTY][3];
    int32_t partyCount;
    double lastHp[SWEEP_PARTY];
    // Where the battle is
    bool begun; // it reached "measure"
    SweepSegment segment;
    int32_t turn; // enemy turns begun
    int32_t pinTurn; // the pin values in use (pinTurns or more: done)
    int32_t freeLeft; // unpinned turns still to play after the pins
    char label[96];
    char labelParts[SWEEP_LABELS][32]; // what each label read last in this attack
    char pinText[64];
    uint32_t hits;
    bool downed; // someone's HP reached 0 in this segment
    uint64_t battleStart, segmentStart;
    int32_t segmentFrames;
    int32_t keyStep, keyFrame; // the player turn's key sequence
    bool ending;
    char endReason[64];
    // Worst-frame shots
    int32_t shots;
    double shotWorkMs;
    char shotFile[64];
    int32_t excludeIn; // frames until the one whose time a shot spoiled is left out
    uint64_t frameBegan;
    uint32_t* created; // createCounts at the segment's start
    // The battle's summary
    int32_t rows;
    double minFps, minLow, worstMs;
    char worstSegment[96];
} sweep;

static const char* const sweepSegmentNames[] = { "enter", "player", "attack", "transition" };
static const char sweepColumns[] =
    "console,model,build,run,battle,title,chapter,turn,segment,attack,pin,status,frames,seconds,fps,fps_1low,shown_fps,worst_ms,"
    "step_avg,step_max,draw_avg,draw_max,gpu_avg,gpu_max,tex_loads,tex_image_loads,tex_ms,tex_evictions,draws_dropped,"
    "skipped_frames,underruns,heap_peak_mb,linear_min_mb,hits,top_objects,shot,shot_frame_ms,warnings,last_warning";
// How many times the first battle of a resumed sweep was started before (it ended those runs).
static int32_t sweepFirstTries;
static void sweepSavePosition(const char* state, int32_t tries);
static void sweepBegin(const char* script);
static void sweepFrame(void);
static void sweepEndFrame(bool skipped);
static void sweepCut(const char* status);
static void sweepFreeRecipe(void);
static void sweepDescribePins(void);
static void sweepDescribeAttack(void);

// ===[ Helpers ]===

static bool fileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static double secondsSince(uint64_t start) {
    return (double) (nowNanos() - start) / 1e9;
}

// The DELTARUNE chapter a data.win folder holds (…/chapter3_windows/), or 0 (the launcher, UNDERTALE).
static int32_t chapterOf(const char* bundleDir) {
    const char* c = bundleDir != nullptr ? strstr(bundleDir, "chapter") : nullptr;
    return c != nullptr ? atoi(c + 7) : 0;
}

static bool chapterAvailable(int32_t chapter) {
    if (chapter <= 0) return true;
    char path[256];
    snprintf(path, sizeof(path), "%s/chapter%ld_windows/data.win", bench.session.gameDir, (long) chapter);
    return fileExists(path);
}

static void copyText(char* dst, size_t size, const char* src) {
    snprintf(dst, size, "%s", src != nullptr ? src : "");
}

static const char* tourFileName(void) {
    return tour.battles ? BATTLE_FILE_NAME : TOUR_FILE_NAME;
}

static const char* tourPositionFileName(void) {
    return tour.battles ? BATTLE_POSITION_FILE_NAME : TOUR_POSITION_FILE_NAME;
}

// Whether an instance of the named object is in the room.
static bool objectExists(const char* name) {
    Runner* runner = bench.session.runner;
    const DataWin* dw = runner->dataWin;
    int32_t object = -1;
    for (uint32_t i = 0; dw->objt.count > i && 0 > object; i++) {
        if (dw->objt.objects[i].name != nullptr && strcmp(dw->objt.objects[i].name, name) == 0) object = (int32_t) i;
    }
    for (ptrdiff_t i = 0; 0 <= object && arrlen(runner->instances) > i; i++) {
        if (!runner->instances[i]->destroyed && runner->instances[i]->objectIndex == object) return true;
    }
    return false;
}

static double warmupSeconds(void) {
    return tour.on ? TOUR_WARMUP_SECONDS : WARMUP_SECONDS;
}

static double measureSeconds(void) {
    return tour.on ? TOUR_MEASURE_SECONDS : MEASURE_SECONDS;
}

// bench_frames' window: game frames to measure, or 0 for the timed window (always for tours and sweeps).
static int32_t frameWindow(void) {
    return tour.on || sweep.on ? 0 : bench.session.display.benchFrames;
}

// Asks the frontend to load another data.win, the way game_change() does.
static void requestGameChange(const char* workingDirectory, const char* parameters) {
    Runner* runner = bench.session.runner;
    free(runner->pendingWorkingDirectory);
    free(runner->pendingLaunchParameters);
    runner->pendingWorkingDirectory = safeStrdup(workingDirectory);
    runner->pendingLaunchParameters = safeStrdup(parameters);
    logInfo("Bench: game_change(\"%s\", \"%s\")\n", workingDirectory, parameters);
}

static const char* modelName(void) {
    u8 model = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        cfguExit();
    }
    switch (model) {
        case CFG_MODEL_3DS: return "Old 3DS";
        case CFG_MODEL_3DSXL: return "Old 3DS XL";
        case CFG_MODEL_N3DS: return "New 3DS";
        case CFG_MODEL_2DS: return "Old 2DS";
        case CFG_MODEL_N3DSXL: return "New 3DS XL";
        case CFG_MODEL_N2DSXL: return "New 2DS XL";
        default: return bench.session.isNew3ds ? "New 3DS (model?)" : "Old 3DS (model?)";
    }
}

// ===[ Scene list ]===

// The performance test's scenes, or with battles the battle sweep's battles.
static void loadScenes(bool battles) {
    BenchScene_freeList(bench.scenes);
    bench.scenes = nullptr;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", bench.session.gameDir, SCENE_FILE_NAME);
    char* text = nullptr;
    FILE* f = fopen(path, "rb");
    if (f != nullptr) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            text = safeMalloc((size_t) size + 1);
            size_t got = fread(text, 1, (size_t) size, f);
            text[got] = '\0';
            logInfo("Bench: scenes from %s\n", path);
        }
        fclose(f);
    }
    if (text == nullptr) {
        text = CtrBench_readRomfs();
        if (text == nullptr) return;

    }
    extern BenchScene* coverageParseScenes(const char* primary); BenchScene* all = coverageParseScenes(text);
    free(text);
    for (ptrdiff_t i = 0; arrlen(all) > i; i++) {
        if (all[i].id[0] == '_' || all[i].battle != battles) {
            free(all[i].id);
            free(all[i].title);
            free(all[i].script);
        } else {
            arrput(bench.scenes, all[i]);
        }
    }
    arrfree(all);
}

// ===[ Bottom screen ]===

static void drawPicker(void) {
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    printf("\x1b[0;0H\x1b[1;36m Performance test\x1b[0m\x1b[K\n");
    printf(" ---------------------------------------\n");
    int32_t count = (int32_t) arrlen(bench.scenes) + 1;
    if (bench.cursor < bench.scroll) bench.scroll = bench.cursor;
    if (bench.cursor >= bench.scroll + LIST_ROWS) bench.scroll = bench.cursor - LIST_ROWS + 1;
    for (int32_t row = 0; LIST_ROWS > row; row++) {
        int32_t i = bench.scroll + row;
        if (i >= count) {
            printf("\x1b[K\n");
            continue;
        }
        bool selected = i == bench.cursor;
        if (i == 0) {
            printf("%s%s Run all (%d scenes)\x1b[0m\x1b[K\n", selected ? "\x1b[1;33m" : "", selected ? ">" : " ", count - 1);
            continue;
        }
        const BenchScene* s = &bench.scenes[i - 1];
        bool available = chapterAvailable(s->chapter);
        char label[64];
        if (s->chapter > 0) snprintf(label, sizeof(label), "Ch%ld %s", (long) s->chapter, s->title);
        else snprintf(label, sizeof(label), "%s", s->title);
        printf("%s%s %-37.37s\x1b[0m\x1b[K\n", !available ? "\x1b[2m" : selected ? "\x1b[1;33m" : "", selected ? ">" : " ", label);
    }
    printf("\x1b[20;0H\x1b[2m");
    if (count == 1) printf(" No scenes for this game.\x1b[K\n");
    else printf(" Each scene: %.0f s to settle, %.0f s measured.\x1b[K\n", WARMUP_SECONDS, MEASURE_SECONDS);
    printf(" Your saves aren't read or changed; the\x1b[K\n");
    printf(" game restarts from its title afterwards.\x1b[K\n");
    printf(" START cancels a running test. Results\x1b[K\n");
    printf(" go to %s too.\x1b[0m\x1b[K\n", RESULTS_FILE_NAME);
    printf("\x1b[26;0H ---------------------------------------\n");
    printf(" Up/Down: choose   A: run\x1b[K\n");
    printf(" B or SELECT: back to the game\x1b[K");
    CtrLog_endConsole();
}

static void drawStatus(void) {
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    int32_t total = (int32_t) arrlen(bench.queue);
    const BenchScene* s = !tour.on && bench.queuePos < total ? &bench.scenes[bench.queue[bench.queuePos]] : nullptr;
    const SceneResult* current = arrlen(bench.results) > 0 ? &arrlast(bench.results) : nullptr;
    if (tour.on) {
        printf("\x1b[0;0H\x1b[1;36m %s tour\x1b[0m  chapter %ld, %s %ld (%ld done)\x1b[K\n", tour.battles ? "Battle" : "Room",
               (long) tour.chapter, tour.battles ? "encounter" : "room", (long) tour.room, (long) tour.visited);
    } else {
        printf("\x1b[0;0H\x1b[1;36m Performance test\x1b[0m  scene %d of %d\x1b[K\n", (int) bench.queuePos + 1, (int) total);
    }
    printf(" ---------------------------------------\n");
    printf(" %-38.38s\x1b[K\n", s != nullptr ? s->title : tour.on && tour.booting ? "Starting the chapter" : tour.on && tour.ending ? "Ending the battle" : tour.on && current != nullptr ? current->title : "");
    switch (bench.phase) {
        case PHASE_LOADING: printf(" Loading chapter %ld...\x1b[K\n", (long) (s != nullptr ? s->chapter : tour.chapter)); break;
        case PHASE_SETUP: printf(" Setting up (%.0f s)\x1b[K\n", secondsSince(bench.phaseStart)); break;
        case PHASE_WARMUP:
            if (frameWindow() > 0) printf(" Settling (%d game frames)\x1b[K\n", BENCH_SETTLE_FRAMES);
            else printf(" Settling (%.0f of %.0f s)\x1b[K\n", secondsSince(bench.phaseStart), warmupSeconds());
            break;
        case PHASE_MEASURE:
            if (frameWindow() > 0) printf(" Measuring %d game frames\x1b[K\n", (int) frameWindow());
            else printf(" Measuring for %.0f s\x1b[K\n", measureSeconds());
            break;
        default: printf("\x1b[K\n"); break;
    }
    printf("\x1b[K\n");
    // The last few results so far.
    int32_t n = (int32_t) arrlen(bench.results);
    int32_t first = n > 12 ? n - 12 : 0;
    for (int32_t i = first; n > i; i++) {
        const SceneResult* r = &bench.results[i];
        if (r->measured) printf(" %-20.20s %5.1f fps, 1%% low %5.1f\x1b[K\n", r->title, r->fpsAvg, r->fpsLow1);
        else printf(" %-20.20s \x1b[2m%-17.17s\x1b[0m\x1b[K\n", r->title, r->status);
    }
    printf("\x1b[28;0H\x1b[2m The bottom screen goes back to your\x1b[K\n setting while measuring. START: cancel\x1b[0m\x1b[K");
    CtrLog_endConsole();
}

static void drawResults(void) {
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    printf("\x1b[0;0H\x1b[1;36m Performance test\x1b[0m  %s%s\x1b[K\n", bench.model, bench.cancelled ? ", cancelled" : "");
    printf("\x1b[2mscene       fps 1%%lowworst step draw  gpu\x1b[0m\x1b[K\n");
    int32_t n = (int32_t) arrlen(bench.results);
    if (bench.cursor >= n) bench.cursor = n > 0 ? n - 1 : 0;
    if (bench.cursor < bench.scroll) bench.scroll = bench.cursor;
    if (bench.cursor >= bench.scroll + LIST_ROWS) bench.scroll = bench.cursor - LIST_ROWS + 1;
    for (int32_t row = 0; LIST_ROWS > row; row++) {
        int32_t i = bench.scroll + row;
        if (i >= n) {
            printf("\x1b[K\n");
            continue;
        }
        const SceneResult* r = &bench.results[i];
        const char* colour = i == bench.cursor ? "\x1b[1;33m" : "";
        if (r->measured) {
            printf("%s%-10.10s%5.1f%5.1f%5.0f%5.1f%5.1f%5.1f\x1b[0m\x1b[K\n", colour, r->id, r->fpsAvg, r->fpsLow1, r->worstMs, r->stepAvg,
                   r->drawAvg, r->gpuAvg);
        } else {
            printf("%s%-10.10s \x1b[2m%-29.29s\x1b[0m\x1b[K\n", colour, r->id, r->status);
        }
    }
    printf("\x1b[19;0H ---------------------------------------\n");
    if (n > 0) {
        const SceneResult* r = &bench.results[bench.cursor];
        printf(" \x1b[1m%-38.38s\x1b[0m\x1b[K\n", r->title);
        if (r->measured) {
            printf(" in %-35.35s\x1b[K\n", r->room);
            printf(" step %.1f/%.1f draw %.1f/%.1f gpu %.1f/%.1f\x1b[K\n", r->stepAvg, r->stepMax, r->drawAvg, r->drawMax, r->gpuAvg, r->gpuMax);
            // Pages decoded from their images stall the game for a second or
            // more each; that is a cache not built yet, not the scene.
            if (r->texImageLoads > 0) {
                printf(" \x1b[1;31mtexture cache not ready: %lu decoded\x1b[0m\x1b[K\n", (unsigned long) r->texImageLoads);
            } else if (r->shownFps < r->fpsAvg - 0.05) {
                printf(" %ld frames in %.1f s, shown %.1f fps\x1b[K\n", (long) r->frames, r->seconds, r->shownFps);
            } else {
                printf(" (ms, average/worst). %ld frames in %.1f s\x1b[K\n", (long) r->frames, r->seconds);
            }
            printf(" textures: %lu loads (%.0f ms), %lu evicted\x1b[K\n", (unsigned long) r->texLoads, r->texLoadMs,
                   (unsigned long) r->texEvictions);
            printf(" music underruns %lu, heap peak %.1f MB\x1b[K\n", (unsigned long) r->underruns, r->heapPeakMB);
            printf(" linear free %.1f MB at least\x1b[K\n", r->linearMinFreeMB);
        } else {
            // The reason can be long (a script error): wrap it over the detail lines.
            const char* t = r->status;
            for (int32_t line = 0; 6 > line; line++) {
                printf(" %-38.38s\x1b[K\n", t);
                t += strlen(t) > 38 ? 38 : strlen(t);
            }
        }
    }
    printf("\x1b[27;0H\x1b[2m %s\x1b[0m\x1b[K\n", bench.saved ? "Saved to " RESULTS_FILE_NAME " in the game folder." : "Could not save " RESULTS_FILE_NAME ".");
    printf(" Up/Down: scene   A or B: back to game\x1b[K");
    CtrLog_endConsole();
}

// ===[ Results file ]===

// One line of the results: to benchmark.txt (f, when it opened) and, when 3dslink started the app, to the PC as a
// "[result] " line (ctr_log.h).
static void emit(FILE* f, const char* format, ...) {
    char line[512];
    va_list va;
    va_start(va, format);
    vsnprintf(line, sizeof(line), format, va);
    va_end(va);
    if (f != nullptr) fputs(line, f);
    if (CtrLog_linkOpen()) {
        CtrLog_linkWrite("[result] ");
        CtrLog_linkWrite(line);
    }
}

static void saveResults(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", bench.session.gameDir, RESULTS_FILE_NAME);
    FILE* f = fopen(path, "a");
    bench.saved = f != nullptr;
    if (f == nullptr) {
        logWarn("Bench: could not write %s\n", path);
        if (!CtrLog_linkOpen()) return;
    }
    const CtrDisplaySettings* d = &bench.session.display;
    emit(f, "== 3DS %s performance test, %s, build %s%s\n", CTRPORT_GAME_TITLE, bench.startedAt, CTRPORT_COMMIT,
         bench.cancelled ? " (cancelled)" : "");
    emit(f, "console: %s, %s\n", bench.model, bench.session.isNew3ds ? "804 MHz with L2 cache (New 3DS speedup on)" : "268 MHz");
    emit(f, "display: top %s, drawing %s, 3D %s (slider %.2f), smoothing %s, bottom %s, frame skip %s\n", CtrDisplay_topModeName(d->topMode),
         CtrDisplay_drawingName(d->drawing), CtrDisplay_stereoModeName(d->stereoMode), (double) osGet3DSliderState(), CtrDisplay_smoothingName(d->smoothing),
         CtrDisplay_bottomModeName(d->bottomMode), CtrDisplay_frameSkipName(d->frameSkip));
    if (d->benchFrames > 0 && !tour.on && !sweep.on) {
        void benchfidEmitWindow(FILE*, const char*, int, int); benchfidEmitWindow(f, "window: %d game frames measured after %d settling (bench_frames), not 3 s and 20 s\n", (int) d->benchFrames, BENCH_SETTLE_FRAMES);
    }
    if (tour.on) {
        bool complete = tourPastEnd();
        emit(f, "%s tour: %ld %s(s) this run, %s; all their numbers are in %s (the last %d below)\n", tour.battles ? "battle" : "room",
             (long) tour.visited, tour.battles ? "battle" : "room",
             complete ? "finished" : tour.battles ? "stopped before its end (tour=battles:resume carries on)" : "stopped before its end (tour=resume carries on)",
             tourFileName(), TOUR_RESULTS_KEPT);
        logInfo("Tour: %s after %ld %s(s)\n", complete ? "finished" : "stopped", (long) tour.visited, tour.battles ? "battle" : "room");
    }
    if (sweep.on) {
        emit(f, "battle sweep (%s): each battle's lowest fps and 1%% low over its segments, and its worst frame; every segment's "
                "numbers are in %s\n",
             sweep.selection, SWEEP_FILE_NAME);
    }
    emit(f, "%-22s %5s %6s %6s %8s %13s %13s %13s %16s %9s %9s %10s %6s  %s\n", "scene", "fps", "1%low", "shown", "worst ms", "step avg/max",
         "draw avg/max", "gpu avg/max", "tex loads/ms/ev", "underruns", "heap peak", "linear min", "setup", "room");
    bool anyImageLoads = false;
    for (ptrdiff_t i = 0; arrlen(bench.results) > i; i++) {
        const SceneResult* r = &bench.results[i];
        if (!r->measured) {
            emit(f, "%-22s %s\n", r->id, r->status);
            continue;
        }
        if (r->texImageLoads > 0) anyImageLoads = true;
        char step[24], draw[24], gpu[24], tex[32];
        snprintf(step, sizeof(step), "%.1f/%.1f", r->stepAvg, r->stepMax);
        snprintf(draw, sizeof(draw), "%.1f/%.1f", r->drawAvg, r->drawMax);
        snprintf(gpu, sizeof(gpu), "%.1f/%.1f", r->gpuAvg, r->gpuMax);
        snprintf(tex, sizeof(tex), "%lu/%.0f/%lu", (unsigned long) r->texLoads, r->texLoadMs, (unsigned long) r->texEvictions);
        emit(f, "%-22s %5.1f %6.1f %6.1f %8.1f %13s %13s %13s %16s %9lu %6.1f MB %7.1f MB %4.0f s  %s%s\n", r->id, r->fpsAvg, r->fpsLow1,
             r->shownFps, r->worstMs, step, draw, gpu, tex, (unsigned long) r->underruns, r->heapPeakMB, r->linearMinFreeMB, r->setupSeconds,
             r->room, r->texImageLoads > 0 ? " *" : "");
    }
    if (anyImageLoads) {
        emit(f, "* decoded texture pages from their images while measuring: the texture cache wasn't ready, and each such page\n");
        emit(f, "  stalls the game for a second or more. Run the test again once the cache is built.\n");
    }
    emit(f, "\n");
    if (f == nullptr) return;
    fclose(f);
    logInfo("Bench: results appended to %s\n", path);
}

// ===[ Running ]===

static SceneResult* currentResult(void) {
    return arrlen(bench.results) > 0 ? &arrlast(bench.results) : nullptr;
}

static void dropScript(void) {
    if (bench.script == nullptr) return;
    if (bench.session.runner != nullptr) BenchScript_releaseKeys(bench.script, bench.session.runner);
    BenchScript_free(bench.script);
    bench.script = nullptr;
}

// Ends the current scene; reason is nullptr when it was measured.
static void endScene(const char* reason) {
    if (tour.booting) {
        tourBootEnded(reason);
        return;
    }
    if (tour.ending) {
        tourBattleEnded(reason);
        return;
    }
    SceneResult* r = currentResult();
    if (r != nullptr) {
        if (reason != nullptr) {
            copyText(r->status, sizeof(r->status), reason);
            logInfo("Bench: %s: %s\n", r->id, reason);
            if (sweep.on) sweepAbandonBattle(reason);
        } else if (sweep.on) {
            // sweepFinishBattle has filled r in with the battle's summary.
        } else {
            computeResult(r);
            // A battle that ended by itself (won, lost, run from, or a scripted end) was partly measured outside it.
            if (r->measured && tour.on && tour.battles && !objectExists("obj_battlecontroller")) {
                copyText(r->status, sizeof(r->status), "the battle ended before the measurement did");
            }
            logResult(r);
        }
        if (tour.on) tourRecord(r);
    }
    dropScript();
    ((void) (bench.frameMs ? benchRoomsRelease(stbds_header(bench.frameMs)) : (void)0), bench.frameMs = nullptr);
    bench.frameMs = nullptr;
    bench.queuePos++;
    bench.phase = PHASE_NEXT;
    if (tour.on && tour.battles && tour.booted) tourStartBattleEnd();
}

static int compareSlowestFirst(const void* a, const void* b) {
    float x = *(const float*) a, y = *(const float*) b;
    return x < y ? 1 : x > y ? -1 : 0;
}

// The numbers of what was measured since beginMeasuring (or resetMeasuring), into r.
static void computeResult(SceneResult* r) {
    int32_t n = (int32_t) arrlen(bench.frameMs);
    double sum = 0.0, worst = 0.0;
    for (int32_t i = 0; n > i; i++) {
        sum += bench.frameMs[i];
        if (bench.frameMs[i] > worst) worst = bench.frameMs[i];
    }
    // 1% low: the frame rate over the slowest 1% of frames.
    float* sorted = safeMalloc(sizeof(float) * (size_t) (n > 0 ? n : 1));
    if (n > 0) memcpy(sorted, bench.frameMs, sizeof(float) * (size_t) n);
    qsort(sorted, (size_t) n, sizeof(float), compareSlowestFirst);
    int32_t slow = n / 100 > 0 ? n / 100 : 1;
    if (slow > n) slow = n;
    double slowSum = 0.0;
    for (int32_t i = 0; slow > i; i++) slowSum += sorted[i];
    int32_t pairs = n / 2;
    for (int32_t i = 0; pairs > i; i++) sorted[i] = (bench.frameMs[2 * i] + bench.frameMs[2 * i + 1]) / 2.0f;
    qsort(sorted, (size_t) pairs, sizeof(float), compareSlowestFirst);
    int32_t slowPairs = pairs / 100 > 0 ? pairs / 100 : 1;
    double slowPairSum = 0.0;
    for (int32_t i = 0; pairs >= slowPairs && slowPairs > i; i++) slowPairSum += sorted[i];
    r->pairLow1 = slowPairSum > 0.0 ? 1000.0 * slowPairs / slowPairSum : 0.0;
    r->pairWorstMs = pairs > 0 ? 2.0 * sorted[0] : 0.0;
    free(sorted);
    int32_t frames = bench.measureFrames > 0 ? bench.measureFrames : 1;
    CtrRenderStats rs = CtrRenderer_getStats(bench.session.renderer);
    r->measured = n > 0;
    r->frames = n;
    r->seconds = sum / 1000.0;
    r->fpsAvg = sum > 0.0 ? 1000.0 * n / sum : 0.0;
    r->fpsLow1 = slowSum > 0.0 ? 1000.0 * slow / slowSum : 0.0;
    r->shownFps = r->fpsAvg * (double) (bench.measureFrames - bench.skippedFrames) / frames;
    r->worstMs = worst;
    r->stepAvg = bench.stepSum / frames;
    r->stepMax = bench.stepMax;
    r->drawAvg = bench.drawSum / frames;
    r->drawMax = bench.drawMax;
    r->gpuAvg = bench.measureFrames > bench.skippedFrames ? bench.gpuSum / (bench.measureFrames - bench.skippedFrames) : 0.0;
    r->gpuMax = bench.gpuMax;
    r->texLoads = rs.textureLoads - bench.statsAtStart.textureLoads;
    r->texImageLoads = rs.textureImageLoads - bench.statsAtStart.textureImageLoads;
    r->texEvictions = rs.textureEvictions - bench.statsAtStart.textureEvictions;
    r->texLoadMs = (double) (rs.textureLoadMs - bench.statsAtStart.textureLoadMs);
    r->smartBoxes = (double) (rs.smartBoxes - bench.statsAtStart.smartBoxes) / frames;
    r->smartPixels = (double) (rs.smartPixels - bench.statsAtStart.smartPixels) / frames;
    r->underruns = CtrAudioSystem_underruns((CtrAudioSystem*) bench.session.runner->audioSystem) - bench.underrunsAtStart;
#ifdef CTRPORT_HEAP_PROFILE
    CtrHeapProfile_mark("measure end");
#endif
#line 789
    r->heapPeakMB = (double) bench.heapPeak / (1024.0 * 1024.0);
    r->linearMinFreeMB = (double) bench.linearMinFree / (1024.0 * 1024.0);
    r->vramMinFreeKB = (double) bench.vramMinFree / 1024.0;
    uint32_t nativeFrames = rs.nativeFrames - bench.statsAtStart.nativeFrames, fullFrames = rs.fullFrames - bench.statsAtStart.fullFrames;
    r->nativeShare = nativeFrames + fullFrames > 0 ? (double) nativeFrames / (double) (nativeFrames + fullFrames) : 0.0;
    r->warnings = CtrLog_warningCount() - tour.warningsAtStart;
    if (r->warnings > 0) copyText(r->lastWarning, sizeof(r->lastWarning), CtrLog_lastWarning());
    r->drawsDropped = CtrRenderer_getSkipTotals(bench.session.renderer).dropped - bench.droppedAtStart;
    r->skippedFrames = bench.skippedFrames;
    CtrSkipStats skipNow = CtrRenderer_getSkipTotals(bench.session.renderer);
    r->workFrames = skipNow.workFrames - bench.skipAtStart.workFrames;
    r->workDraws = skipNow.workDraws - bench.skipAtStart.workDraws;
    r->workFills = skipNow.workFills - bench.skipAtStart.workFills;
    r->workCopies = skipNow.workCopies - bench.skipAtStart.workCopies;
    r->behind = skipNow.behind - bench.skipAtStart.behind;
    CtrFlipStats flipsNow;
    CtrPacer_getFlipTotals(&flipsNow);
    memset(&r->flips, 0, sizeof(r->flips));
    for (int32_t i = 0; CTR_FLIP_BUCKETS > i; i++) r->flips.count[i] = flipsNow.count[i] - bench.flipsAtStart.count[i];
    r->flips.flips = flipsNow.flips - bench.flipsAtStart.flips;
    r->flips.sum = flipsNow.sum - bench.flipsAtStart.sum;
    r->flips.sumSq = flipsNow.sumSq - bench.flipsAtStart.sumSq;
    r->flipMean = r->flips.flips > 0 ? (double) r->flips.sum / r->flips.flips : 0.0;
    double flipVar = r->flips.flips > 0 ? (double) r->flips.sumSq / r->flips.flips - r->flipMean * r->flipMean : 0.0;
    r->flipSd = flipVar > 0.0 ? sqrt(flipVar) : 0.0;
    r->gpuWaitMs = (double) (CtrGpu_waitTotalTicks() - bench.gpuWaitAtStart) / CPU_TICKS_PER_MSEC / frames;
    CtrPacer_logLevels(r->id, &bench.levelsAtStart, (double) (svcGetSystemTick() - bench.levelsTick) / CPU_TICKS_PER_MSEC / 1000.0);
    if (!r->measured) copyText(r->status, sizeof(r->status), "no frames measured");
}

static void logResult(const SceneResult* r) {
    logInfo("Bench: %s: %.1f fps (1%% low %.1f, %.1f shown), worst %.1f ms, step %.2f/%.2f, draw %.2f/%.2f, gpu %.2f/%.2f ms, "
            "textures %lu loads (%lu from images) %.0f ms %lu evicted, underruns %lu, heap peak %.1f MB, linear min %.1f MB, vram min %.0f KB, "
            "native %.0f%%, in %s\n",
            r->id, r->fpsAvg, r->fpsLow1, r->shownFps, r->worstMs, r->stepAvg, r->stepMax, r->drawAvg, r->drawMax, r->gpuAvg, r->gpuMax,
            (unsigned long) r->texLoads, (unsigned long) r->texImageLoads, r->texLoadMs, (unsigned long) r->texEvictions,
            (unsigned long) r->underruns, r->heapPeakMB,
            r->linearMinFreeMB, r->vramMinFreeKB, 100.0 * r->nativeShare, r->room);
    if (0.0 < r->smartPixels) { void CtrBench_lateSmartBoxesLine(const char* fmt, ...); CtrBench_lateSmartBoxesLine("Bench: %s: smart smoothing averaged %.1f boxes, %.0f surface pixels a frame\n", r->id, r->smartBoxes, r->smartPixels); }
    logInfo("Bench: %s: frame pairs: 1%% low %.1f fps, worst pair %.1f ms, %ld skipped\n", r->id, r->pairLow1, r->pairWorstMs,
            (long) r->skippedFrames);
    if (r->skippedFrames > 0) {
        // 59.83 vblanks a second on the top screen.
        const double vblankMs = 1000.0 / 59.83;
        logInfo("Bench: %s: shown frames: %lu flips, up for 1-7/8+ vblanks %lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu, mean %.2f vblanks (%.1f ms) "
                "sd %.2f (%.1f ms); waited for the GPU %.2f ms a frame; %lu skipped frames queued GPU work (%lu draws, %lu fills, %lu copies), %lu behind\n",
                r->id, (unsigned long) r->flips.flips, (unsigned long) r->flips.count[0], (unsigned long) r->flips.count[1],
                (unsigned long) r->flips.count[2], (unsigned long) r->flips.count[3], (unsigned long) r->flips.count[4],
                (unsigned long) r->flips.count[5], (unsigned long) r->flips.count[6], (unsigned long) r->flips.count[7], r->flipMean,
                r->flipMean * vblankMs, r->flipSd, r->flipSd * vblankMs, r->gpuWaitMs, (unsigned long) r->workFrames,
                (unsigned long) r->workDraws, (unsigned long) r->workFills, (unsigned long) r->workCopies,
                (unsigned long) r->behind);
    }
}

static void finish(void) {
    if (sweep.on) sweepFreeRecipe();
    dropScript();
    bench.phase = PHASE_RESULTS;
    bench.phaseStart = nowNanos();
    bench.cursor = 0;
    bench.scroll = 0;
    benchRowsSave();
    bench.finished = true;
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    consoleClear();
    CtrLog_endConsole();
    drawResults();
}

// Back to the player's game: the sandbox goes, and the game starts again from where the test found it.
static void restore(void) {
    SandboxFileSystem_setEnabled(bench.session.sandbox, false);
    bool sameData = bench.originalBundleDir != nullptr && strcmp(bench.originalBundleDir, bench.session.bundleDir) == 0;
    if (sameData) {
        bench.session.runner->pendingRoom = ROOM_RESTARTGAME;
    } else {
        int32_t from = chapterOf(bench.session.bundleDir);
        int32_t to = chapterOf(bench.originalBundleDir);
        char dir[64];
        if (to > 0) snprintf(dir, sizeof(dir), "%schapter%ld_windows", from > 0 ? "/../" : "/", (long) to);
        else snprintf(dir, sizeof(dir), "%s", from > 0 ? "/../" : "/");
        requestGameChange(dir, bench.originalParameters != nullptr && bench.originalParameters[0] != '\0' ? bench.originalParameters : "-game data.win");
    }
    free(bench.originalBundleDir);
    free(bench.originalParameters);
    bench.originalBundleDir = nullptr;
    bench.originalParameters = nullptr;
    arrfree(bench.results);
    bench.results = nullptr;
    arrfree(bench.queue);
    bench.queue = nullptr;
    bench.phase = PHASE_IDLE;
    tour.on = false;
    sweep.on = false;
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    consoleClear();
    CtrLog_endConsole();
    bench.returned = true;
    logInfo("Bench: done, back to the game\n");
}

static void startScript(const char* text) {
    char* error = nullptr;
    bench.script = BenchScript_parse(text, &error);
    if (bench.script == nullptr) {
        char reason[96];
        snprintf(reason, sizeof(reason), "script error: %s", error != nullptr ? error : "?");
        free(error);
        endScene(reason);
        return;
    }
    bench.phase = PHASE_SETUP;
    bench.phaseStart = nowNanos();
}

// ===[ Room tour ]===

static void tourPath(char* out, size_t size, const char* name) {
    snprintf(out, size, "%s/%s", bench.session.gameDir, name);
}

static const char* tourRoomName(int32_t index) {
    const RoomChunk* rooms = &bench.session.runner->dataWin->room;
    return 0 <= index && (int32_t) rooms->count > index && rooms->rooms[index].name != nullptr ? rooms->rooms[index].name : "?";
}

// Whether DELTARUNE's global.darkzone is set on the way into a room, as play would have it there: the Dark World is
// drawn at twice the Light World's scale, so its rooms' views are 640x480 where the Light World's are 320x240. The
// names say so only from Chapter 2 on (room_dw_...): Chapter 1's Card Castle, fields and forest are room_cc_...,
// room_field... and so on. A room without a view goes by its name. Reads the room's payload, which room_goto
// would read anyway.
static bool tourRoomIsDark(int32_t index, const char* name) {
    DataWin* dataWin = bench.session.runner->dataWin;
    DataWin_loadRoomPayload(dataWin, index);
    const Room* room = &dataWin->room.rooms[index];
    if (room->views != nullptr) {
        for (int32_t i = 0; 8 > i; i++) {
            if (room->views[i].enabled) return room->views[i].viewWidth >= 640;
        }
    }
    return strncmp(name, "room_dw", 7) == 0 || strstr(name, "_dw_") != nullptr || strncmp(name, "room_cc_", 8) == 0;
}

// tour.pos: the room being visited ("started") or the last one visited ("done"), and where the tour stops, so that
// "tour=resume" carries on after it, also when that room ended the run.
static void tourSavePosition(const char* state, const char* roomName) {
    char path[256];
    tourPath(path, sizeof(path), tourPositionFileName());
    FILE* f = fopen(path, "w");
    if (f == nullptr) return;
    fprintf(f, "%ld %ld %s %ld %ld %s\n", (long) tour.chapter, (long) tour.room, state, (long) tour.lastChapter, (long) tour.lastRoom,
            roomName);
    fclose(f);
}

static bool tourSkipped(const char* name) {
    for (size_t i = 0; sizeof(tourSkipPrefixes) / sizeof(tourSkipPrefixes[0]) > i; i++) {
        if (strncasecmp(name, tourSkipPrefixes[i], strlen(tourSkipPrefixes[i])) == 0) return true;
    }
    return false;
}

// One row of tour.csv, written at once so that a room that ends the run loses none of the rows before it, and logged
// ("Tour: ") for the PC. r is nullptr for a room that ended an earlier run.
static void tourWriteRow(const char* roomName, const SceneResult* r) {
    char status[96];
    snprintf(status, sizeof(status), "%s", r == nullptr ? "the run ended here" : r->status[0] != '\0' ? r->status : r->measured ? "ok" : "?");
    for (char* c = status; *c != '\0'; c++) {
        if (*c == ',' || *c == '"' || *c == '\n') *c = ';';
    }
    char row[512];
    int n = snprintf(row, sizeof(row), "%s,%s,%s,%ld,%ld,%s,%s,%s", bench.session.isNew3ds ? "n3ds" : "o3ds", bench.model, CTRPORT_COMMIT,
                     (long) tour.chapter, (long) tour.room, roomName, r != nullptr && r->room[0] != '\0' ? r->room : "-", status);
    if (r != nullptr && r->measured && 0 < n && (int) sizeof(row) > n) {
        snprintf(row + n, sizeof(row) - (size_t) n, ",%.1f,%.1f,%.1f,%.1f,%.2f,%.1f,%.2f,%.1f,%.2f,%.1f,%lu,%lu,%.0f,%lu,%lu,%.1f,%.1f,%.0f",
                 r->fpsAvg, r->fpsLow1, r->shownFps, r->worstMs, r->stepAvg, r->stepMax, r->drawAvg, r->drawMax, r->gpuAvg, r->gpuMax,
                 (unsigned long) r->texLoads, (unsigned long) r->texImageLoads, r->texLoadMs, (unsigned long) r->texEvictions,
                 (unsigned long) r->underruns, r->heapPeakMB, r->linearMinFreeMB, r->loadMs);
    } else if (0 < n && (int) sizeof(row) > n) {
        snprintf(row + n, sizeof(row) - (size_t) n, ",,,,,,,,,,,,,,,,,,");
    }
    // A battle's also has the warnings logged while it was measured, and the last one's text.
    n = (int) strlen(row);
    if (tour.battles && (int) sizeof(row) > n) {
        char warning[96];
        snprintf(warning, sizeof(warning), "%s", r != nullptr ? r->lastWarning : "");
        for (char* c = warning; *c != '\0'; c++) {
            if (*c == ',' || *c == '"' || *c == '\n' || *c == '\r') *c = ';';
        }
        if (r != nullptr && r->measured) snprintf(row + n, sizeof(row) - (size_t) n, ",%lu,%s", (unsigned long) r->warnings, warning);
        else snprintf(row + n, sizeof(row) - (size_t) n, ",,");
    }
    char path[256];
    tourPath(path, sizeof(path), tourFileName());
    struct stat st;
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE* f = fopen(path, "a");
    if (f != nullptr) {
        if (fresh && tour.battles) {
            fputs("console,model,build,chapter,encounter,enemies,measured_room,status,fps,fps_1low,shown_fps,worst_ms,step_avg,step_max,"
                  "draw_avg,draw_max,gpu_avg,gpu_max,tex_loads,tex_image_loads,tex_ms,tex_evictions,underruns,heap_peak_mb,linear_min_mb,"
                  "load_ms,warnings,last_warning\n",
                  f);
        } else if (fresh) {
            fputs("console,model,build,chapter,index,room,measured_room,status,fps,fps_1low,shown_fps,worst_ms,step_avg,step_max,draw_avg,"
                  "draw_max,gpu_avg,gpu_max,tex_loads,tex_image_loads,tex_ms,tex_evictions,underruns,heap_peak_mb,linear_min_mb,load_ms\n",
                  f);
        }
        fputs(row, f);
        fputc('\n', f);
        fclose(f);
    } else {
        logWarn("Tour: could not write %s\n", path);
    }
    logInfo("Tour: %s\n", row);
}

static void tourRecord(const SceneResult* r) {
    const char* name = tour.battles ? tour.enemies : tourRoomName(tour.room);
    tourWriteRow(name, r);
    tourSavePosition("done", name);
    tour.room++;
    tour.visited++;
    // Only the last few stay for the status screen; the array was made big enough for them when the test began.
    if (arrlen(bench.results) >= TOUR_RESULTS_KEPT) arrdel(bench.results, 0);
}

// Reads tour.pos into tour; false when there is none. A room left "started" ended the run: it gets its row now.
static bool tourLoadPosition(void) {
    char path[256];
    tourPath(path, sizeof(path), tourPositionFileName());
    FILE* f = fopen(path, "r");
    if (f == nullptr) return false;
    long chapter = 0, room = 0, lastChapter = 0, lastRoom = -1;
    char state[16] = "", name[96] = "?";
    int got = fscanf(f, "%ld %ld %15s %ld %ld %95s", &chapter, &room, state, &lastChapter, &lastRoom, name);
    fclose(f);
    if (5 > got) return false;
    tour.chapter = (int32_t) chapter;
    tour.room = (int32_t) room;
    tour.lastChapter = (int32_t) lastChapter;
    tour.lastRoom = (int32_t) lastRoom;
    if (strcmp(state, "started") == 0) {
        logWarn("Tour: chapter %ld %s %ld (%s) ended the last run\n", chapter, tour.battles ? "encounter" : "room", room, name);
        tourWriteRow(name, nullptr);
        tourSavePosition("done", name);
    }
    if (strcmp(state, "cancelled") != 0) tour.room++;
    return true;
}

// Sets the tour up from a "tour" word: "tour" (everything), "tour=C:I" (from chapter C's room I to the end),
// "tour=C:I-C:I" (up to that room), "tour=C-C" (whole chapters) or "tour=resume" (after the room tour.pos names).
static bool tourParse(const char* spec) {
    memset(&tour, 0, sizeof(tour));
    tour.deltarune = chapterAvailable(1);
    // "battles", "battles:SPEC": a battle tour, its SPEC as a room tour's (I an encounter id).
    if (spec != nullptr && strncmp(spec, "battles", 7) == 0 && (spec[7] == '\0' || spec[7] == ':')) {
        if (!tour.deltarune) return false;
        tour.battles = true;
        spec += spec[7] == ':' ? 8 : 7;
    }
    tour.chapter = tour.deltarune ? 1 : 0;
    tour.lastChapter = tour.deltarune ? TOUR_LAST_CHAPTER : 0;
    tour.lastRoom = -1;
    if (spec == nullptr || *spec == '\0') return true;
    if (strcmp(spec, "resume") == 0) {
        if (tourLoadPosition()) return true;
        logInfo("Tour: no %s, starting from the first %s\n", tourPositionFileName(), tour.battles ? "battle" : "room");
        return true;
    }
    char* end;
    tour.chapter = (int32_t) strtol(spec, &end, 10);
    if (end == spec) return false;
    if (*end == ':') tour.room = (int32_t) strtol(end + 1, &end, 10);
    if (*end == '-') {
        tour.lastChapter = (int32_t) strtol(end + 1, &end, 10);
        if (*end == ':') tour.lastRoom = (int32_t) strtol(end + 1, &end, 10);
    }
    return *end == '\0' && (tour.battles ? 1 : 0) <= tour.chapter && TOUR_LAST_CHAPTER >= tour.lastChapter && 0 <= tour.room;
}

static bool tourPastEnd(void) {
    if (tour.chapter > tour.lastChapter) return true;
    return tour.chapter == tour.lastChapter && 0 <= tour.lastRoom && tour.room > tour.lastRoom;
}

static void tourStartBoot(void) {
    logInfo("Tour: starting chapter %ld\n", (long) tour.chapter);
    tour.booting = true;
    snprintf(tour.script, sizeof(tour.script), "%smeasure\n", tourBootSteps[tour.chapter]);
    startScript(tour.script);
}

static void tourBootEnded(const char* reason) {
    dropScript();
    tour.booting = false;
    bench.phase = PHASE_NEXT;
    if (reason == nullptr) {
        tour.booted = true;
        return;
    }
    logWarn("Tour: chapter %ld could not start (%s), going on with the next\n", (long) tour.chapter, reason);
    tour.chapter++;
    tour.room = 0;
}

// After a battle's measurement: ends it the way the game does (scr_endcombat, run by obj_battlecontroller), so that
// its music and objects go, then lets the end play out. A battle that has already ended needs nothing.
static void tourStartBattleEnd(void) {
    tour.ending = true;
    snprintf(tour.script, sizeof(tour.script), "%swait %d\nmeasure\n",
             objectExists("obj_battlecontroller") ? "callas obj_battlecontroller scr_endcombat\n" : "", BATTLE_END_FRAMES);
    startScript(tour.script);
}

static void tourBattleEnded(const char* reason) {
    dropScript();
    tour.ending = false;
    bench.phase = PHASE_NEXT;
    if (reason != nullptr) logWarn("Tour: could not end the battle (%s); the next one starts from its room anyway\n", reason);
}

// The script for the battle tour's next battle, encounter tour.room.
static void tourBattleScript(void) {
    char start[512];
    if (tour.chapter == 1) {
        // Chapter 1 has no scr_battle: the overworld enemies start the music and create obj_encounterbasic.
        snprintf(start, sizeof(start),
                 "callas obj_mainchara snd_init \"battle.ogg\"\nset global.batmusic[0] obj_mainchara._mystream\nset global.flag[9] 1\n"
                 "set global.specialbattle 0\nset global.encounterno %ld\ncreate obj_encounterbasic 0 0\n",
                 (long) tour.room);
    } else {
        // scr_battle starts the battle music itself unless flag[9] is 2.
        snprintf(start, sizeof(start), "set global.flag[9] 0\ncall scr_battle %ld 0 -4 -4 -4\n", (long) tour.room);
    }
    snprintf(tour.script, sizeof(tour.script),
             "log BATTLE %ld %s\ncall snd_free_all\nset global.darkzone 1\nset global.char[0] 1\nset global.char[1] 2\n"
             "set global.char[2] 3\n%swait 5\n%swaitobj obj_battlecontroller\nkeep set global.inv 30\n"
             "keep set global.hp[1] global.maxhp[1]\nkeep set global.hp[2] global.maxhp[2]\nkeep set global.hp[3] global.maxhp[3]\n"
             "keep set global.monsterhp[0] global.monstermaxhp[0]\nkeep set global.monsterhp[1] global.monstermaxhp[1]\n"
             "keep set global.monsterhp[2] global.monstermaxhp[2]\nkeys X Z/4:2\nwait 60\nmeasure\n",
             (long) tour.room, tour.enemies, battleRooms[tour.chapter], start);
}

// Starts the tour's next room, the chapter it is in first when that isn't running yet.
static void tourNext(void) {
    for (;;) {
        if (tourPastEnd()) {
            finish();
            return;
        }
        if (!tour.booted) {
            if (!tour.deltarune) {
                tourStartBoot();
                return;
            }
            if (!chapterAvailable(tour.chapter)) {
                logWarn("Tour: chapter %ld is not on the SD card\n", (long) tour.chapter);
                tour.chapter++;
                tour.room = 0;
                continue;
            }
            // Each chapter starts afresh, as the launcher starts it.
            char dir[64];
            snprintf(dir, sizeof(dir), "%schapter%ld_windows", chapterOf(bench.session.bundleDir) > 0 ? "/../" : "/", (long) tour.chapter);
            requestGameChange(dir, CHAPTER_LAUNCH_PARAMETERS);
            bench.changeRequested = true;
            bench.phase = PHASE_LOADING;
            bench.phaseStart = nowNanos();
            return;
        }
        if (tour.battles) {
            int32_t last = tour.chapter == tour.lastChapter && 0 <= tour.lastRoom ? tour.lastRoom : BATTLE_LAST_ENCOUNTER;
            int32_t found = BenchScript_findEncounter(bench.session.runner, tour.room, last, tour.enemies, sizeof(tour.enemies));
            if (0 > found) {
                tour.chapter++;
                tour.room = 0;
                tour.booted = false;
                continue;
            }
            tour.room = found;
            SceneResult r = {0};
            snprintf(r.id, sizeof(r.id), "c%ldb%ld", (long) tour.chapter, (long) tour.room);
            copyText(r.title, sizeof(r.title), tour.enemies);
            arrput(bench.results, r);
            logInfo("Bench: scene %s (%s)\n", r.id, tour.enemies);
            tourBattleScript();
            tourSavePosition("started", tour.enemies);
            startScript(tour.script);
            return;
        }
        if (tour.room >= (int32_t) bench.session.runner->dataWin->room.count) {
            tour.chapter++;
            tour.room = 0;
            tour.booted = false;
            continue;
        }
        const char* name = tourRoomName(tour.room);
        if (tourSkipped(name)) {
            tour.room++;
            continue;
        }
        SceneResult r = {0};
        snprintf(r.id, sizeof(r.id), "c%ldr%ld", (long) tour.chapter, (long) tour.room);
        copyText(r.title, sizeof(r.title), name);
        arrput(bench.results, r);
        logInfo("Bench: scene %s (%s)\n", r.id, name);
        // As corpus.py's sweep: a marker for the log, the room's Dark World flag, then room_goto.
        if (tour.deltarune) {
            bool dark = tourRoomIsDark(tour.room, name);
            snprintf(tour.script, sizeof(tour.script), "log SWEEPROOM %ld %s\nset global.darkzone %d\ncall room_goto %s\nmeasure\n",
                     (long) tour.room, name, dark ? 1 : 0, name);
        } else {
            snprintf(tour.script, sizeof(tour.script), "log SWEEPROOM %ld %s\ncall room_goto %s\nmeasure\n", (long) tour.room, name, name);
        }
        tourSavePosition("started", name);
        startScript(tour.script);
        return;
    }
}

static void startNextScene(void) {
    if (tour.on) {
        tourNext();
        return;
    }
    if (bench.queuePos >= (int32_t) arrlen(bench.queue)) {
        finish();
        return;
    }
    const BenchScene* scene = &bench.scenes[bench.queue[bench.queuePos]];
    SceneResult r = {0};
    copyText(r.id, sizeof(r.id), scene->id);
    copyText(r.title, sizeof(r.title), scene->title);
    arrput(bench.results, r);
#ifdef CTRPORT_HEAP_PROFILE
    CtrHeapProfile_mark("scene start");
#endif
#line 1228
    void CtrBench_lateSceneStart(const char* format, ...); CtrBench_lateSceneStart("Bench: scene %s (%s)\n", scene->id, scene->title);
    if (sweep.on) sweepSavePosition("started", bench.queuePos == 0 ? sweepFirstTries + 1 : 1);
    if (scene->chapter > 0) {
        if (!chapterAvailable(scene->chapter)) {
            char reason[64];
            snprintf(reason, sizeof(reason), "chapter %ld is not on the SD card", (long) scene->chapter);
            endScene(reason);
            return;
        }
        // Every DELTARUNE scene starts its chapter afresh, as the launcher does.
        char dir[64];
        snprintf(dir, sizeof(dir), "%schapter%ld_windows", chapterOf(bench.session.bundleDir) > 0 ? "/../" : "/", (long) scene->chapter);
        requestGameChange(dir, CHAPTER_LAUNCH_PARAMETERS);
        bench.changeRequested = true;
        bench.phase = PHASE_LOADING;
        bench.phaseStart = nowNanos();
        return;
    }
    startScript(scene->script);
}

// Starts the numbers afresh (the battle sweep at each segment, too).
static void resetMeasuring(void) {
    Runner* runner = bench.session.runner;
    arrsetlen(bench.frameMs, 0);
    bench.measureFrames = 0;
    bench.skippedFrames = 0;
    bench.stepSum = bench.drawSum = bench.gpuSum = 0.0;
    bench.stepMax = bench.drawMax = bench.gpuMax = 0.0;
    bench.statsAtStart = CtrRenderer_getStats(bench.session.renderer);
    bench.droppedAtStart = CtrRenderer_getSkipTotals(bench.session.renderer).dropped;
    bench.skipAtStart = CtrRenderer_getSkipTotals(bench.session.renderer);
    CtrPacer_getFlipTotals(&bench.flipsAtStart);
    CtrPacer_getLevelTotals(&bench.levelsAtStart);
    bench.levelsTick = svcGetSystemTick();
    bench.gpuWaitAtStart = CtrGpu_waitTotalTicks();
    bench.underrunsAtStart = CtrAudioSystem_underruns((CtrAudioSystem*) runner->audioSystem);
    struct mallinfo mi = mallinfo();
    bench.heapPeak = (uint32_t) mi.uordblks;
#ifdef CTRPORT_HEAP_PROFILE
    CtrHeapProfile_mark("measure start");
#endif
#line 1267
    bench.linearMinFree = linearSpaceFree();
    bench.vramMinFree = vramSpaceFree();
    tour.warningsAtStart = CtrLog_warningCount();
}

static void beginMeasuring(void) {
    Runner* runner = bench.session.runner;
    SceneResult* r = currentResult();
    copyText(r->room, sizeof(r->room), runner->currentRoom != nullptr ? runner->currentRoom->name : "?");
    benchshownRoomsOpen();
    bench.phase = PHASE_MEASURE;
    bench.phaseStart = nowNanos();
    bench.measureStartFrame = runner->frameCount;
    bench.shotTaken = false;
    // The bottom screen goes back to the player's setting, so the numbers are for the way they play.
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    consoleClear();
    CtrLog_endConsole();
}

static void onAptEvent(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) bench.interrupted = true;
}

// Starts the scenes in bench.queue.
static __attribute__((noinline)) void beginTest(void) {
    if (!bench.aptHooked) {
        aptHook(&bench.aptCookie, onAptEvent, nullptr);
        bench.aptHooked = true;
    }
    bench.interrupted = false;
    bench.queuePos = 0;
    arrfree(bench.results);
    bench.results = nullptr;
    // Made whole now: grown scene by scene, each copy would land mid-game and outlive the chapter.
    arrsetcap(bench.results, tour.on ? TOUR_RESULTS_KEPT : arrlen(bench.queue));
    bench.cancelled = false;
    bench.saved = false;
    copyText(bench.model, sizeof(bench.model), modelName());
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (tm != nullptr) strftime(bench.startedAt, sizeof(bench.startedAt), "%Y-%m-%d %H:%M", tm);
    else copyText(bench.startedAt, sizeof(bench.startedAt), "?");

    free(bench.originalBundleDir);
    free(bench.originalParameters);
    bench.originalBundleDir = safeStrdup(bench.session.bundleDir);
    char parameters[512] = "";
    for (ptrdiff_t i = 1; arrlen(bench.session.gameArgs) > i; i++) {
        size_t used = strlen(parameters);
        snprintf(parameters + used, sizeof(parameters) - used, "%s%s", i > 1 ? " " : "", bench.session.gameArgs[i]);
    }
    bench.originalParameters = safeStrdup(parameters);

    // Whatever the game queued for the card goes first; from here on nothing reaches it.
    CtrFileSystem_flush(bench.session.saves);
    SandboxFileSystem_setEnabled(bench.session.sandbox, true);
    logInfo("Bench: starting %d scene(s) on %s\n", (int) arrlen(bench.queue), bench.model);
    CtrLog_beginConsole();
    consoleSelect(bench.console);
    consoleClear();
    CtrLog_endConsole();
    bench.phase = PHASE_NEXT;
}

// ===[ Battle sweep ]===


static void sweepFreeRecipe(void) {
    for (int32_t i = 0; sweep.pinCount > i; i++) {
        BenchRef_free(sweep.pins[i].ref);
        arrfree(sweep.pins[i].values);
    }
    for (int32_t i = 0; sweep.labelCount > i; i++) BenchRef_free(sweep.labels[i]);
    for (int32_t i = 0; sweep.partyCount > i; i++) {
        BenchRef_free(sweep.hp[i]);
        BenchRef_free(sweep.maxHp[i]);
        for (int32_t j = 0; 3 > j; j++) BenchRef_free(sweep.revive[i][j]);
    }
    for (int32_t i = 0; sweep.playerKeyCount > i; i++) free(sweep.playerKeys[i]);
    for (ptrdiff_t i = 0; arrlen(sweep.atOps) > i; i++) free(sweep.atOps[i].op);
    arrfree(sweep.atOps);
    BenchRef_free(sweep.state);
    BenchRef_free(sweep.transition);
    BenchRef_free(sweep.endRef);
    BenchRef_free(sweep.follow);
    Runner* runner = bench.session.runner;
    if (runner != nullptr && runner->createCounts == sweep.created) runner->createCounts = nullptr;
    free(sweep.created);
    if (runner != nullptr && runner->createCounts != nullptr) {
        free(runner->createCounts);
        runner->createCounts = nullptr;
    }
    bool on = sweep.on;
    char selection[sizeof(sweep.selection)];
    memcpy(selection, sweep.selection, sizeof(selection));
    memset(&sweep, 0, sizeof(sweep));
    sweep.on = on;
    memcpy(sweep.selection, selection, sizeof(selection));
}

// Pin values: numbers, "a..b" (every whole number from a to b) and "N*v" (v N times).
static bool sweepParseValues(const char* p, double** out) {
    char word[48];
    int consumed = 0;
    while (sscanf(p, "%47s%n", word, &consumed) == 1) {
        p += consumed;
        char* dots = strstr(word, "..");
        char* star = strchr(word, '*');
        if (dots != nullptr) {
            long from = strtol(word, nullptr, 10), to = strtol(dots + 2, nullptr, 10);
            for (long v = from; to >= v && 1000 > arrlen(*out); v++) arrput(*out, (double) v);
        } else if (star != nullptr) {
            long count = strtol(word, nullptr, 10);
            double v = strtod(star + 1, nullptr);
            for (long i = 0; count > i && 1000 > arrlen(*out); i++) arrput(*out, v);
        } else {
            char* end = nullptr;
            double v = strtod(word, &end);
            if (end == word) return false;
            arrput(*out, v);
        }
    }
    return true;
}

static int32_t findObjectIndex(const char* name) {
    const DataWin* dw = bench.session.runner->dataWin;
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        if (dw->objt.objects[i].name != nullptr && strcmp(dw->objt.objects[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

// slot: the member's DELTARUNE party slot (0-2), whose downed flags are undone with the HP; -1 for none.
static bool sweepAddParty(const char* hp, const char* maxHp, int32_t slot) {
    if (sweep.partyCount >= SWEEP_PARTY) return false;
    BenchRef* a = BenchRef_parse(hp);
    BenchRef* b = BenchRef_parse(maxHp);
    if (a == nullptr || b == nullptr) {
        BenchRef_free(a);
        BenchRef_free(b);
        return false;
    }
    sweep.hp[sweep.partyCount] = a;
    sweep.maxHp[sweep.partyCount] = b;
    if (slot >= 0) {
        static const char* const flags[3] = { "global.chardead", "global.charmove", "global.charcantarget" };
        for (int32_t j = 0; 3 > j; j++) {
            char ref[48];
            snprintf(ref, sizeof(ref), "%s[%ld]", flags[j], (long) slot);
            sweep.revive[sweep.partyCount][j] = BenchRef_parse(ref);
        }
    }
    sweep.partyCount++;
    return true;
}

// Reads the "sweep" lines of a battle's script. Returns false (with a warning) on a line it can't read.
static bool sweepParseRecipe(const char* script) {
    sweepFreeRecipe();
    bool deltarune = chapterAvailable(1);
    sweep.attackValue = 2.0;
    sweep.freeTurns = SWEEP_FREE_TURNS;
    sweep.turnCap = SWEEP_TURN_CAP;
    sweep.hpFloor = SWEEP_HP_FLOOR;
    copyText(sweep.playerMash, sizeof(sweep.playerMash), "Z/6:2");
    copyText(sweep.aliveName, sizeof(sweep.aliveName), "obj_battlecontroller");
    bool ok = true;
    bool partySet = false;
    const char* s = script;
    while (*s != '\0') {
        const char* end = s;
        while (*end != '\0' && *end != '\n') end++;
        char line[512];
        size_t length = (size_t) (end - s) < sizeof(line) - 1 ? (size_t) (end - s) : sizeof(line) - 1;
        memcpy(line, s, length);
        line[length] = '\0';
        if (length > 0 && line[length - 1] == '\r') line[length - 1] = '\0';
        s = *end == '\n' ? end + 1 : end;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "sweep ", 6) != 0) continue;
        p += 6;
        char word[24] = "", a[96] = "", b[96] = "";
        int consumed = 0;
        sscanf(p, "%23s%n", word, &consumed);
        char* rest = p + consumed;
        while (*rest == ' ' || *rest == '\t') rest++;
        bool good = true;
        if (strcmp(word, "pin") == 0 && sweep.pinCount < SWEEP_PINS) {
            int n = 0;
            good = sscanf(rest, "%95s%n", a, &n) == 1;
            SweepPin* pin = &sweep.pins[sweep.pinCount];
            pin->ref = good ? BenchRef_parse(a) : nullptr;
            good = pin->ref != nullptr && sweepParseValues(rest + n, &pin->values) && arrlen(pin->values) > 0;
            if (good) {
                if (arrlen(pin->values) > sweep.pinTurns) sweep.pinTurns = (int32_t) arrlen(pin->values);
                sweep.pinCount++;
            } else {
                BenchRef_free(pin->ref);
                arrfree(pin->values);
                memset(pin, 0, sizeof(*pin));
            }
        } else if (strcmp(word, "label") == 0) {
            int n = 0;
            while (sweep.labelCount < SWEEP_LABELS && sscanf(rest, "%95s%n", a, &n) == 1) {
                rest += n;
                BenchRef* ref = BenchRef_parse(a);
                if (ref == nullptr) good = false;
                else sweep.labels[sweep.labelCount++] = ref;
            }
        } else if (strcmp(word, "state") == 0 || strcmp(word, "transition") == 0 || strcmp(word, "end") == 0) {
            double value = 0.0;
            good = sscanf(rest, "%95s %lf", a, &value) == 2;
            BenchRef* ref = good ? BenchRef_parse(a) : nullptr;
            good = ref != nullptr;
            if (word[0] == 's') {
                BenchRef_free(sweep.state);
                sweep.state = ref;
                sweep.attackValue = value;
            } else if (word[0] == 't') {
                BenchRef_free(sweep.transition);
                sweep.transition = ref;
                sweep.transitionValue = value;
            } else {
                BenchRef_free(sweep.endRef);
                sweep.endRef = ref;
                sweep.endValue = value;
            }
        } else if (strcmp(word, "follow") == 0) {
            good = sscanf(rest, "%95s", a) == 1;
            BenchRef_free(sweep.follow);
            sweep.follow = good ? BenchRef_parse(a) : nullptr;
            good = sweep.follow != nullptr;
        } else if (strcmp(word, "alive") == 0) {
            good = sscanf(rest, "%47s", sweep.aliveName) == 1;
        } else if (strcmp(word, "player") == 0) {
            // sweep player [KEY KEY ...] [then PATTERN...]
            for (int32_t i = 0; sweep.playerKeyCount > i; i++) free(sweep.playerKeys[i]);
            sweep.playerKeyCount = 0;
            int n = 0;
            while (sscanf(rest, "%95s%n", a, &n) == 1) {
                rest += n;
                if (strcmp(a, "then") == 0) {
                    while (*rest == ' ') rest++;
                    copyText(sweep.playerMash, sizeof(sweep.playerMash), rest);
                    break;
                }
                if (sweep.playerKeyCount < SWEEP_SEQUENCE) sweep.playerKeys[sweep.playerKeyCount++] = safeStrdup(a);
            }
        } else if (strcmp(word, "attack") == 0) {
            copyText(sweep.attackKeys, sizeof(sweep.attackKeys), rest);
        } else if (strcmp(word, "free") == 0 || strcmp(word, "turns") == 0) {
            long n = 0;
            good = sscanf(rest, "%ld", &n) == 1 && n >= 0;
            if (good && word[0] == 'f') sweep.freeTurns = (int32_t) n;
            else if (good) sweep.turnCap = (int32_t) n;
        } else if (strcmp(word, "hpfloor") == 0) {
            good = sscanf(rest, "%lf", &sweep.hpFloor) == 1;
        } else if (strcmp(word, "party") == 0) {
            long slot = -1;
            good = sscanf(rest, "%95s %95s %ld", a, b, &slot) >= 2 && sweepAddParty(a, b, (int32_t) slot);
            partySet = true;
        } else if (strcmp(word, "at") == 0) {
            long turn = 0;
            int n = 0;
            good = sscanf(rest, "%ld%n", &turn, &n) == 1;
            if (good) {
                rest += n;
                while (*rest == ' ') rest++;
                SweepAt at = { (int32_t) turn, safeStrdup(rest) };
                arrput(sweep.atOps, at);
            }
        } else {
            good = false;
        }
        if (!good) {
            logWarn("Sweep: can't read 'sweep %s'\n", p);
            ok = false;
        }
    }
    if (sweep.state == nullptr) sweep.state = BenchRef_parse("global.mnfight");
    sweep.aliveObject = findObjectIndex(sweep.aliveName);
    if (!partySet) {
        if (deltarune) {
            // The party as global.char has it now; each member's HP is global.hp[their id].
            BenchRef* member = nullptr;
            for (int32_t slot = 0; 3 > slot; slot++) {
                char ref[48];
                snprintf(ref, sizeof(ref), "global.char[%ld]", (long) slot);
                member = BenchRef_parse(ref);
                double id = 0.0;
                if (member != nullptr && BenchRef_getReal(member, bench.session.runner, &id) && id > 0) {
                    char hp[48], maxHp[48];
                    snprintf(hp, sizeof(hp), "global.hp[%ld]", (long) id);
                    snprintf(maxHp, sizeof(maxHp), "global.maxhp[%ld]", (long) id);
                    sweepAddParty(hp, maxHp, slot);
                }
                BenchRef_free(member);
            }
        } else {
            sweepAddParty("global.hp", "global.maxhp", -1);
        }
    }
    return ok;
}

static void sweepPath(char* out, size_t size, const char* name) {
    snprintf(out, size, "%s/%s", bench.session.gameDir, name);
}

// sweep.pos: the battle being fought ("started", with the times it was started) or the last one finished ("done"),
// so that "sweep=resume" carries on there.
static void sweepSavePosition(const char* state, int32_t tries) {
    char path[256];
    sweepPath(path, sizeof(path), SWEEP_POSITION_FILE_NAME);
    FILE* f = fopen(path, "w");
    if (f == nullptr) return;
    const BenchScene* scene = &bench.scenes[bench.queue[bench.queuePos]];
    fprintf(f, "%s %s %ld\n", scene->id, state, (long) tries);
    fclose(f);
}

static void csvClean(char* text) {
    for (char* c = text; *c != '\0'; c++) {
        if (*c == ',' || *c == '"' || *c == '\n' || *c == '\r') *c = ';';
    }
}

// The objects created most during the segment, as "name*count" words.
static void sweepTopObjects(char* out, size_t size) {
    out[0] = '\0';
    Runner* runner = bench.session.runner;
    if (runner->createCounts == nullptr || sweep.created == nullptr) return;
    const DataWin* dw = runner->dataWin;
    int32_t picked[SWEEP_TOP_OBJECTS];
    uint32_t counts[SWEEP_TOP_OBJECTS];
    int32_t pickedCount = 0;
    // The SWEEP_TOP_OBJECTS largest counts, kept sorted (the first object wins a tie).
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        uint32_t n = runner->createCounts[i] - sweep.created[i];
        if (n == 0) continue;
        int32_t at = pickedCount;
        while (at > 0 && n > counts[at - 1]) at--;
        if (at >= SWEEP_TOP_OBJECTS) continue;
        int32_t last = pickedCount < SWEEP_TOP_OBJECTS ? pickedCount : SWEEP_TOP_OBJECTS - 1;
        for (int32_t j = last; j > at; j--) {
            picked[j] = picked[j - 1];
            counts[j] = counts[j - 1];
        }
        picked[at] = (int32_t) i;
        counts[at] = n;
        if (pickedCount < SWEEP_TOP_OBJECTS) pickedCount++;
    }
    size_t used = 0;
    for (int32_t j = 0; pickedCount > j; j++) {
        const char* name = dw->objt.objects[picked[j]].name != nullptr ? dw->objt.objects[picked[j]].name : "?";
        int n = snprintf(out + used, size - used, "%s%s*%lu", used > 0 ? " " : "", name, (unsigned long) counts[j]);
        if (0 > n || (size_t) n >= size - used) break;
        used += (size_t) n;
    }
}

// One row of sweep.csv, appended and closed at once so that a crash loses at most the segment it was in.
static void sweepWriteRowFor(const BenchScene* scene, const SceneResult* r, const char* segment, const char* status, const char* objects) {
    char label[96], pin[64], statusText[96], warning[96], title[64];
    copyText(label, sizeof(label), sweep.label);
    copyText(pin, sizeof(pin), sweep.pinText);
    copyText(statusText, sizeof(statusText), status);
    copyText(warning, sizeof(warning), r != nullptr ? r->lastWarning : "");
    copyText(title, sizeof(title), scene->title);
    csvClean(label);
    csvClean(pin);
    csvClean(statusText);
    csvClean(warning);
    csvClean(title);
    char row[1024];
    int n = snprintf(row, sizeof(row), "%s,%s,%s,%s,%s,%s,%ld,%ld,%s,%s,%s,%s", bench.session.isNew3ds ? "n3ds" : "o3ds", bench.model,
                     CTRPORT_COMMIT, bench.startedAt, scene->id, title, (long) scene->chapter, (long) sweep.turn, segment, label, pin,
                     statusText);
    if (r != nullptr && r->measured && 0 < n && (int) sizeof(row) > n) {
        n += snprintf(row + n, sizeof(row) - (size_t) n,
                      ",%ld,%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%.1f,%.2f,%.1f,%.2f,%.1f,%lu,%lu,%.0f,%lu,%lu,%ld,%lu,%.1f,%.1f,%lu,%s,%s,%.0f,%lu,%s",
                      (long) r->frames, r->seconds, r->fpsAvg, r->fpsLow1, r->shownFps, r->worstMs, r->stepAvg, r->stepMax, r->drawAvg,
                      r->drawMax, r->gpuAvg, r->gpuMax, (unsigned long) r->texLoads, (unsigned long) r->texImageLoads, r->texLoadMs,
                      (unsigned long) r->texEvictions, (unsigned long) r->drawsDropped, (long) r->skippedFrames,
                      (unsigned long) r->underruns, r->heapPeakMB, r->linearMinFreeMB, (unsigned long) sweep.hits,
                      objects != nullptr ? objects : "", sweep.shots > 0 ? sweep.shotFile : "", sweep.shots > 0 ? sweep.shotWorkMs : 0.0,
                      (unsigned long) r->warnings, warning);
    } else if (0 < n && (int) sizeof(row) > n) {
        snprintf(row + n, sizeof(row) - (size_t) n, ",,,,,,,,,,,,,,,,,,,,,,,,,,,");
    }
    char path[256];
    sweepPath(path, sizeof(path), SWEEP_FILE_NAME);
    struct stat st;
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE* f = fopen(path, "a");
    if (f != nullptr) {
        if (fresh) {
            fputs(sweepColumns, f);
            fputc('\n', f);
        }
        fputs(row, f);
        fputc('\n', f);
        fclose(f);
    } else {
        logWarn("Sweep: could not write %s\n", path);
    }
    // The log has every row too (tools/remote/run3ds.sh makes a sweep.csv of them).
    logInfo("Sweep row: %s\n", row);
    sweep.rows++;
}

// The same for the battle being fought.
static void sweepWriteRow(const SceneResult* r, const char* segment, const char* status, const char* objects) {
    sweepWriteRowFor(&bench.scenes[bench.queue[bench.queuePos]], r, segment, status, objects);
}

// Ends the segment: its row, and the battle's summary so far. The numbers start afresh.
static void sweepCut(const char* status) {
    // An attack is named once it is over: the enemies pick and create it a few frames into their turn. (A followed
    // fight's attack is named through it: when it is cut, the next one has begun.)
    if (sweep.segment == SEG_ATTACK && sweep.follow == nullptr) sweepDescribeAttack();
    SceneResult r = {0};
    copyText(r.id, sizeof(r.id), currentResult()->id);
    computeResult(&r);
    char objects[160];
    sweepTopObjects(objects, sizeof(objects));
    const char* name = sweepSegmentNames[sweep.segment];
    sweepWriteRow(&r, name, status != nullptr ? status : r.measured ? (sweep.downed ? "ok (someone was downed)" : "ok") : r.status, objects);
    // The battle's entry pays for its loading: not a segment to compare.
    if (r.measured && sweep.segment != SEG_ENTER && r.frames >= SWEEP_MIN_SUMMARY_FRAMES) {
        if (sweep.minFps == 0.0 || r.fpsAvg < sweep.minFps) sweep.minFps = r.fpsAvg;
        if (sweep.minLow == 0.0 || r.fpsLow1 < sweep.minLow) sweep.minLow = r.fpsLow1;
        if (r.worstMs > sweep.worstMs) {
            sweep.worstMs = r.worstMs;
            snprintf(sweep.worstSegment, sizeof(sweep.worstSegment), "turn %ld %s%s%s", (long) sweep.turn, name,
                     sweep.label[0] != '\0' ? " " : "", sweep.label);
        }
    }
    resetMeasuring();
    Runner* runner = bench.session.runner;
    if (runner->createCounts != nullptr && sweep.created != nullptr) {
        memcpy(sweep.created, runner->createCounts, sizeof(uint32_t) * runner->dataWin->objt.count);
    }
    sweep.hits = 0;
    sweep.downed = false;
    sweep.shots = 0;
    sweep.shotWorkMs = 0.0;
    sweep.shotFile[0] = '\0';
    sweep.segmentStart = nowNanos();
    sweep.segmentFrames = 0;
    // This frame carries the time the row took to write: the next segment leaves it out.
    sweep.excludeIn = 1;
}

static void sweepSetKeys(const char* keys) {
    if (!BenchScript_setKeys(bench.script, bench.session.runner, keys)) logWarn("Sweep: bad keys '%s'\n", keys);
}

// Party member i back to full HP, and no longer downed.
static void sweepRestore(int32_t i, double maxHp) {
    Runner* runner = bench.session.runner;
    BenchRef_setReal(sweep.hp[i], runner, maxHp);
    sweep.lastHp[i] = maxHp;
    double dead = 0.0;
    if (sweep.revive[i][0] != nullptr && BenchRef_getReal(sweep.revive[i][0], runner, &dead) && dead != 0.0) {
        BenchRef_setReal(sweep.revive[i][0], runner, 0.0);
        BenchRef_setReal(sweep.revive[i][1], runner, 1.0);
        BenchRef_setReal(sweep.revive[i][2], runner, 1.0);
    }
}

static void sweepTopUpParty(void) {
    for (int32_t i = 0; sweep.partyCount > i; i++) {
        double maxHp = 0.0;
        if (BenchRef_getReal(sweep.maxHp[i], bench.session.runner, &maxHp)) sweepRestore(i, maxHp);
    }
}

// Called when the battle reached "measure".
static void sweepBegin(const char* script) {
    if (!sweepParseRecipe(script)) logWarn("Sweep: %s's recipe has lines it couldn't read (see above); going on without them\n", currentResult()->id);
    Runner* runner = bench.session.runner;
    uint32_t objects = runner->dataWin->objt.count;
    free(runner->createCounts);
    runner->createCounts = safeCalloc(objects > 0 ? objects : 1, sizeof(uint32_t));
    sweep.created = safeCalloc(objects > 0 ? objects : 1, sizeof(uint32_t));
    sweep.begun = true;
    sweep.segment = SEG_ENTER;
    sweep.turn = 0;
    sweep.pinTurn = 0;
    sweep.freeLeft = sweep.freeTurns;
    sweep.battleStart = sweep.segmentStart = nowNanos();
    copyText(sweep.label, sizeof(sweep.label), "");
    sweepDescribePins();
    sweepTopUpParty();
    sweepSetKeys(sweep.playerMash);
    char mkdirPath[256];
    sweepPath(mkdirPath, sizeof(mkdirPath), SWEEP_SHOT_DIR);
    mkdir(mkdirPath, 0777);
    logInfo("Sweep: %s (%s) begins: %ld pinned turn(s), %ld free, at most %ld turns, %ld party member(s)\n", currentResult()->id,
            currentResult()->title, (long) sweep.pinTurns, (long) sweep.freeTurns, (long) sweep.turnCap, (long) sweep.partyCount);
}

static bool sweepRefIs(BenchRef* ref, double value) {
    double v = 0.0;
    return ref != nullptr && BenchRef_getReal(ref, bench.session.runner, &v) && v == value;
}

static void sweepApplyPins(void) {
    if (sweep.pinTurn >= sweep.pinTurns) return;
    for (int32_t i = 0; sweep.pinCount > i; i++) {
        SweepPin* pin = &sweep.pins[i];
        if (arrlen(pin->values) == 0) continue;
        int32_t at = sweep.pinTurn < (int32_t) arrlen(pin->values) ? sweep.pinTurn : (int32_t) arrlen(pin->values) - 1;
        BenchRef_setReal(pin->ref, bench.session.runner, pin->values[at]);
    }
}

static void sweepDescribePins(void) {
    sweep.pinText[0] = '\0';
    if (sweep.pinTurn >= sweep.pinTurns) {
        if (sweep.pinCount > 0) copyText(sweep.pinText, sizeof(sweep.pinText), "free");
        return;
    }
    size_t used = 0;
    for (int32_t i = 0; sweep.pinCount > i; i++) {
        SweepPin* pin = &sweep.pins[i];
        int32_t at = sweep.pinTurn < (int32_t) arrlen(pin->values) ? sweep.pinTurn : (int32_t) arrlen(pin->values) - 1;
        int n = snprintf(sweep.pinText + used, sizeof(sweep.pinText) - used, "%s%g", used > 0 ? " " : "", pin->values[at]);
        if (0 > n || (size_t) n >= sizeof(sweep.pinText) - used) break;
        used += (size_t) n;
    }
}

// Reads the labels into the attack's name. Called through the attack, since the enemies pick it a few frames in and
// may forget it before the turn ends: a label that reads nothing now ("-": its object is gone, or blank) keeps what
// it read before.
static void sweepDescribeAttack(void) {
    sweep.label[0] = '\0';
    size_t used = 0;
    for (int32_t i = 0; sweep.labelCount > i; i++) {
        char value[48];
        BenchRef_format(sweep.labels[i], bench.session.runner, value, sizeof(value));
        bool empty = strcmp(value, "-") == 0 || strspn(value, " ") == strlen(value);
        if (!empty || sweep.labelParts[i][0] == '\0') copyText(sweep.labelParts[i], sizeof(sweep.labelParts[i]), empty ? "-" : value);
        int n = snprintf(sweep.label + used, sizeof(sweep.label) - used, "%s%s", used > 0 ? "/" : "", sweep.labelParts[i]);
        if (0 > n || (size_t) n >= sizeof(sweep.label) - used) break;
        used += (size_t) n;
    }
}

// The battle is over for the sweep: the last segment's row, the summary, then the next battle.
static void sweepFinishBattle(const char* why) {
    // The segment the battle ended in (none when it ended as one did).
    if (sweep.segmentFrames > 1) sweepCut(nullptr);
    SceneResult* r = currentResult();
    r->measured = sweep.minFps > 0.0;
    r->fpsAvg = sweep.minFps;
    r->fpsLow1 = sweep.minLow;
    r->shownFps = sweep.minFps;
    r->worstMs = sweep.worstMs;
    r->seconds = secondsSince(sweep.battleStart);
    if (!r->measured) copyText(r->status, sizeof(r->status), why);
    logInfo("Sweep: %s (%s) done (%s) after %ld turn(s), %.0f s, %ld rows: worst segment %s at %.1f ms; lowest fps %.1f, lowest 1%% low %.1f\n",
            r->id, r->title, why, (long) sweep.turn, r->seconds, (long) sweep.rows, sweep.worstSegment[0] != '\0' ? sweep.worstSegment : "-",
            sweep.worstMs, sweep.minFps, sweep.minLow);
    sweepSavePosition("done", 0);
    sweepFreeRecipe();
    endScene(nullptr);
}

// Fills bench.queue (bench.scenes holding the battles) from a "sweep" word's SPEC: empty for every battle, "resume"
// to carry on where sweep.pos says, or ids separated by commas, each "id", "prefix*" or "id.." (that battle and every
// one after it). False when it names none.
static bool sweepSelect(const char* spec) {
    arrfree(bench.queue);
    bench.queue = nullptr;
    sweepFirstTries = 0;
    int32_t count = (int32_t) arrlen(bench.scenes);
    if (strcmp(spec, "resume") == 0) {
        char path[256], id[64] = "", state[16] = "";
        long tries = 0;
        sweepPath(path, sizeof(path), SWEEP_POSITION_FILE_NAME);
        FILE* f = fopen(path, "r");
        int got = f != nullptr ? fscanf(f, "%63s %15s %ld", id, state, &tries) : 0;
        if (f != nullptr) fclose(f);
        int32_t from = 0;
        if (got >= 2) {
            int32_t at = -1;
            for (int32_t i = 0; count > i; i++) {
                if (strcmp(bench.scenes[i].id, id) == 0) at = i;
            }
            if (0 > at) {
                logWarn("Sweep: %s names battle %s, which the list doesn't have; starting from the first\n", SWEEP_POSITION_FILE_NAME, id);
            } else if (strcmp(state, "started") == 0 && tries < SWEEP_MAX_TRIES) {
                // That battle ended the last run: it starts again.
                logWarn("Sweep: %s ended the last run (started %ld time(s)); trying it again\n", id, tries);
                from = at;
                sweepFirstTries = (int32_t) tries;
            } else if (strcmp(state, "started") == 0) {
                logWarn("Sweep: %s ended the last %ld runs; going on without it\n", id, tries);
                char status[96];
                snprintf(status, sizeof(status), "the run ended here %ld times; left out", tries);
                copyText(bench.model, sizeof(bench.model), modelName());
                sweepWriteRowFor(&bench.scenes[at], nullptr, "-", status, nullptr);
                from = at + 1;
            } else {
                from = at + 1;
            }
        } else {
            logInfo("Sweep: no %s, starting from the first battle\n", SWEEP_POSITION_FILE_NAME);
        }
        for (int32_t i = from; count > i; i++) arrput(bench.queue, i);
        return arrlen(bench.queue) > 0;
    }
    for (int32_t i = 0; count > i; i++) {
        bool wanted = spec[0] == '\0';
        const char* p = spec;
        while (!wanted && *p != '\0') {
            size_t length = strcspn(p, ",");
            char item[64];
            snprintf(item, sizeof(item), "%.*s", (int) length, p);
            size_t itemLength = strlen(item);
            if (itemLength > 0 && item[itemLength - 1] == '*') {
                wanted = strncmp(bench.scenes[i].id, item, itemLength - 1) == 0;
            } else if (itemLength > 2 && strcmp(item + itemLength - 2, "..") == 0) {
                item[itemLength - 2] = '\0';
                for (int32_t j = 0; i >= j && !wanted; j++) wanted = strcmp(bench.scenes[j].id, item) == 0;
            } else {
                wanted = strcmp(bench.scenes[i].id, item) == 0;
            }
            p += length;
            if (*p == ',') p++;
        }
        if (wanted) arrput(bench.queue, i);
    }
    return arrlen(bench.queue) > 0;
}

// The battle couldn't go on (its script failed, the game closed, ...): a row that says why, then the next battle.
static void sweepAbandonBattle(const char* reason) {
    if (sweep.begun) sweepCut(reason);
    else sweepWriteRow(nullptr, "setup", reason, nullptr);
    logWarn("Sweep: %s abandoned after %ld turn(s): %s\n", currentResult()->id, (long) sweep.turn, reason);
    sweepSavePosition("done", 0);
    sweepFreeRecipe();
}

// Each measured frame, after its time went in: follows the battle from segment to segment and plays it.
static void sweepFrame(void) {
    Runner* runner = bench.session.runner;
    sweep.segmentFrames++;
    // A party wiped out by one hit ends in a game over (room_gameover in both games), which also ends the battle.
    // The segment's row says so, however short it was, and the sweep goes on to the next battle, which loads its own
    // save or chapter.
    bool gameOver = runner->currentRoom != nullptr && runner->currentRoom->name != nullptr && strstr(runner->currentRoom->name, "gameover") != nullptr;
    if (gameOver) {
        static const char why[] = "game over: the party was wiped out";
        sweepCut(why);
        sweepFinishBattle(why);
        return;
    }
    bool alive = sweep.aliveObject < 0 || objectExists(sweep.aliveName);
    if (!alive) {
        sweepFinishBattle(sweep.downed ? "the battle ended right after someone was downed (a game over?)" : "the battle ended");
        return;
    }
    if (secondsSince(sweep.battleStart) > SWEEP_BATTLE_SECONDS) {
        sweepFinishBattle("the battle took too long");
        return;
    }
    if (secondsSince(sweep.segmentStart) > SWEEP_SEGMENT_SECONDS) {
        sweepCut("stuck: the segment took too long");
        sweepFinishBattle("stuck");
        return;
    }
    SweepSegment now = sweepRefIs(sweep.transition, sweep.transitionValue) ? SEG_TRANSITION
                       : sweepRefIs(sweep.state, sweep.attackValue)       ? SEG_ATTACK
                                                                          : SEG_PLAYER;
    bool followChanged = false;
    if (sweep.follow != nullptr && now != SEG_TRANSITION) {
        // A fight without turns (a real-time one): an attack lasts while the followed value holds; the entry lasts
        // until it first changes.
        double v = 0.0;
        if (BenchRef_getReal(sweep.follow, runner, &v)) {
            followChanged = sweep.followRead && v != sweep.followValue;
            sweep.followValue = v;
            sweep.followRead = true;
        }
        now = sweep.segment == SEG_ENTER && !followChanged ? SEG_ENTER : SEG_ATTACK;
    } else if (sweep.segment == SEG_ENTER && now == SEG_PLAYER) {
        // The entry lasts until the battle first leaves the state it began in.
        now = SEG_ENTER;
    }
    if (now != sweep.segment || followChanged) {
        bool attackBegins = now == SEG_ATTACK && (sweep.segment != SEG_ATTACK || followChanged);
        bool attackEnds = sweep.segment == SEG_ATTACK && (now != SEG_ATTACK || followChanged);
        sweepCut(nullptr);
        sweep.segment = now;
        if (attackBegins) {
            sweep.turn++;
            sweepTopUpParty();
            sweep.label[0] = '\0';
            memset(sweep.labelParts, 0, sizeof(sweep.labelParts));
            sweepSetKeys(sweep.attackKeys);
            // A followed fight has no player turns: its "at" operations run as its attacks begin (1: the first).
            for (ptrdiff_t i = 0; sweep.follow != nullptr && arrlen(sweep.atOps) > i; i++) {
                if (sweep.atOps[i].turn == sweep.turn) BenchScript_runNow(bench.script, runner, sweep.atOps[i].op);
            }
        }
        if (attackEnds) {
            // What the next turn pins.
            if (sweep.pinTurn < sweep.pinTurns) sweep.pinTurn++;
            else if (sweep.freeLeft > 0) sweep.freeLeft--;
            bool pinsDone = sweep.pinTurn >= sweep.pinTurns;
            bool done = sweep.turn >= sweep.turnCap || (pinsDone && sweep.freeLeft <= 0) || (pinsDone && sweep.pinCount == 0 && sweep.freeTurns == 0);
            if (sweep.endRef != nullptr && sweepRefIs(sweep.endRef, sweep.endValue)) done = true;
            if (done) {
                sweepFinishBattle(sweep.turn >= sweep.turnCap ? "turn limit" : "every turn played");
                return;
            }
        }
        if (now == SEG_PLAYER) {
            sweepDescribePins();
            copyText(sweep.label, sizeof(sweep.label), "");
            sweep.keyStep = 0;
            sweep.keyFrame = 0;
            sweepSetKeys(sweep.playerKeyCount > 0 ? "" : sweep.playerMash);
            for (ptrdiff_t i = 0; arrlen(sweep.atOps) > i; i++) {
                if (sweep.atOps[i].turn == sweep.turn) BenchScript_runNow(bench.script, runner, sweep.atOps[i].op);
            }
        }
    }
    if (sweep.endRef != nullptr && sweep.segment != SEG_ATTACK && sweepRefIs(sweep.endRef, sweep.endValue)) {
        sweepFinishBattle("its end condition");
        return;
    }
    if (sweep.segment == SEG_ATTACK && (sweep.follow != nullptr || sweep.segmentFrames % 8 == 0)) sweepDescribeAttack();
    // The player's turn: the pins hold until the enemy's turn begins, and the keys play the menu.
    if (sweep.segment == SEG_PLAYER || sweep.segment == SEG_ENTER) {
        sweepApplyPins();
        if (sweep.keyStep < sweep.playerKeyCount) {
            int32_t f = sweep.keyFrame++;
            if (f == 0) sweepSetKeys(sweep.playerKeys[sweep.keyStep]);
            else if (f == SWEEP_KEY_HOLD) sweepSetKeys("");
            else if (f >= SWEEP_KEY_PERIOD) {
                sweep.keyStep++;
                sweep.keyFrame = 0;
                if (sweep.keyStep >= sweep.playerKeyCount) sweepSetKeys(sweep.playerMash);
            }
        }
    }
}

// After each measured frame: the party's HP, and whether the frame is worth a screenshot.
static void sweepEndFrame(bool skipped) {
    Runner* runner = bench.session.runner;
    for (int32_t i = 0; sweep.partyCount > i; i++) {
        double hp = 0.0, maxHp = 0.0;
        if (!BenchRef_getReal(sweep.hp[i], runner, &hp) || !BenchRef_getReal(sweep.maxHp[i], runner, &maxHp)) continue;
        if (hp < sweep.lastHp[i]) sweep.hits++;
        if (hp <= 0.0) sweep.downed = true;
        // Nobody may fall: at the floor, back to full (and up again if a single hit downed them).
        if (hp <= maxHp * sweep.hpFloor || hp <= 1.0) sweepRestore(i, maxHp);
        else sweep.lastHp[i] = hp;
    }
    double workMs = (double) (nowNanos() - sweep.frameBegan) / 1e6;
    if (!skipped && bench.screenshots && sweep.segment != SEG_ENTER && sweep.excludeIn == 0 && sweep.shots < SWEEP_SHOTS_PER_SEGMENT &&
        workMs >= SWEEP_SHOT_MIN_MS && workMs > sweep.shotWorkMs * 1.1) {
        snprintf(sweep.shotFile, sizeof(sweep.shotFile), "%s_t%02ld_%s.bmp", currentResult()->id, (long) sweep.turn,
                 sweepSegmentNames[sweep.segment]);
        char path[256];
        snprintf(path, sizeof(path), "%s/%s/%s", bench.session.gameDir, SWEEP_SHOT_DIR, sweep.shotFile);
        CtrRenderer_requestSurfaceShot(bench.session.renderer, path);
        sweep.shotWorkMs = workMs;
        sweep.shots++;
        // This frame keeps its time; the next one carries the shot's, and is left out.
        sweep.excludeIn = 2;
    }
}

// ===[ API ]===

void CtrBench_sessionStarted(const CtrBenchSession* session) {
    CtrDisplaySettings display = bench.session.display;
    bench.session = *session;
    // A chapter loaded for the test reads the same settings file, but keep what the test started with.
    if (bench.phase != PHASE_IDLE && bench.phase != PHASE_PICKER) bench.session.display = display;
    if (bench.phase == PHASE_IDLE || bench.phase == PHASE_PICKER) return;
    SandboxFileSystem_setEnabled(session->sandbox, true);
    if (bench.phase == PHASE_LOADING) {
        bench.changeRequested = false;
        if (tour.on) tourStartBoot();
        else startScript(bench.scenes[bench.queue[bench.queuePos]].script);
    }
}

void CtrBench_openPicker(PrintConsole* console, const CtrDisplaySettings* display) {
    bench.console = console;
    bench.session.display = *display;
    loadScenes(false);
    tour.on = false;
    sweep.on = false;
    bench.phase = PHASE_PICKER;
    bench.cursor = 0;
    bench.scroll = 0;
    CtrLog_beginConsole();
    consoleSelect(console);
    consoleClear();
    CtrLog_endConsole();
    drawPicker();
}

bool CtrBench_autorunList(const char* words, const char* source, PrintConsole* console, const CtrDisplaySettings* display) {
    bench.console = console;
    bench.session.display = *display;
    loadScenes(false);
    arrfree(bench.queue);
    bench.queue = nullptr;
    bench.autoDismiss = false;
    bench.screenshots = false;
    tour.on = false;
    sweep.on = false;
    bool tourWanted = false;
    bool sweepWanted = false;
    bool noShots = false;
    char sweepSpec[256] = "";
    const char* p = words;
    for (;;) {
        // Long enough for a sweep word that lists battles one by one.
        char word[256];
        int consumed = 0;
        if (sscanf(p, "%255s%n", word, &consumed) != 1) break;
        p += consumed;
        if (strcmp(word, "done") == 0) {
            bench.autoDismiss = true;
            continue;
        }
        if (strcmp(word, "tour") == 0 || strncmp(word, "tour=", 5) == 0) {
            if (!tourParse(word[4] == '=' ? word + 5 : "")) {
                logWarn("Bench: %s: can't read '%s' (tour, tour=C:I, tour=C:I-C:I, tour=C-C or tour=resume; tour=battles or "
                        "tour=battles:<the same> for DELTARUNE's battles)\n", source, word);
                return false;
            }
            tourWanted = true;
            continue;
        }
        if (strcmp(word, "shots") == 0) {
            bench.screenshots = true;
            continue;
        }
        if (strcmp(word, "noshots") == 0) {
            noShots = true;
            continue;
        }
        if (strcmp(word, "sweep") == 0 || strncmp(word, "sweep=", 6) == 0) {
            sweepWanted = true;
            copyText(sweepSpec, sizeof(sweepSpec), word[5] == '=' ? word + 6 : "");
            continue;
        }
        bool found = false;
        for (int32_t i = 0; (int32_t) arrlen(bench.scenes) > i; i++) {
            if (strcmp(word, "all") == 0 || strcmp(word, bench.scenes[i].id) == 0) {
                arrput(bench.queue, i);
                found = true;
            }
        }
        if (!found) logWarn("Bench: %s names scene '%s', which this game's list doesn't have\n", source, word);
    }
    if (sweepWanted) {
        // The sweep replaces the scenes (and a tour) with its battles.
        loadScenes(true);
        if (!sweepSelect(sweepSpec)) {
            logWarn("Bench: %s: 'sweep%s%s' names no battles (%d in this game's list)\n", source, sweepSpec[0] != '\0' ? "=" : "", sweepSpec,
                    (int) arrlen(bench.scenes));
            loadScenes(false);
            return false;
        }
        sweep.on = true;
        // Worst-frame screenshots unless "noshots".
        bench.screenshots = !noShots;
        copyText(sweep.selection, sizeof(sweep.selection), sweepSpec[0] != '\0' ? sweepSpec : "all");
        logInfo("Bench: %s asks for a battle sweep (%s): %d battle(s), from %s\n", source, sweep.selection, (int) arrlen(bench.queue),
                bench.scenes[bench.queue[0]].id);
        logInfo("Sweep columns: %s\n", sweepColumns);
        benchRowsBegin();
        return true;
    }
    if (tourWanted) {
        // A tour replaces the scenes.
        arrfree(bench.queue);
        bench.queue = nullptr;
        tour.on = true;
        logInfo("Bench: %s asks for a %s tour from chapter %ld %s %ld to chapter %ld %s %ld%s\n", source, tour.battles ? "battle" : "room",
                (long) tour.chapter, tour.battles ? "encounter" : "room", (long) tour.room, (long) tour.lastChapter,
                tour.battles ? "encounter" : "room", (long) tour.lastRoom, 0 > tour.lastRoom ? " (its last)" : "");
        benchRowsBegin();
        return true;
    }
    if (arrlen(bench.queue) == 0) {
        logWarn("Bench: %s names no scenes\n", source);
        return false;
    }
    logInfo("Bench: %s asks for %d scene(s)\n", source, (int) arrlen(bench.queue));
    benchRowsBegin();
    return true;
}

bool CtrBench_autorun(const char* path, PrintConsole* console, const CtrDisplaySettings* display) {
    FILE* f = fopen(path, "r");
    if (f == nullptr) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* words = safeMalloc((size_t) (size > 0 ? size : 0) + 1);
    size_t got = size > 0 ? fread(words, 1, (size_t) size, f) : 0;
    words[got] = '\0';
    fclose(f);
    bool started = CtrBench_autorunList(words, path, console, display);
    free(words);
    return started;
}

bool CtrBench_takeFinished(void) {
    bool finished = bench.finished;
    bench.finished = false;
    return finished;
}

bool CtrBench_returnedToGame(void) {
    return bench.returned;
}

bool CtrBench_pickerOpen(void) {
    return bench.phase == PHASE_PICKER;
}

bool CtrBench_running(void) {
    return bench.phase != PHASE_IDLE && bench.phase != PHASE_PICKER;
}

bool CtrBench_wantsBottomScreen(void) {
    return bench.phase != PHASE_IDLE && bench.phase != PHASE_MEASURE;
}

CtrBenchFrame CtrBench_beginFrame(u32 down, double frameMs) {
    switch (bench.phase) {
        case PHASE_IDLE: return BENCH_FRAME_RUN;
        case PHASE_PICKER: {
            int32_t count = (int32_t) arrlen(bench.scenes) + 1;
            if (down & (KEY_B | KEY_SELECT)) {
                bench.phase = PHASE_IDLE;
                CtrLog_beginConsole();
                consoleSelect(bench.console);
                consoleClear();
                CtrLog_endConsole();
                return BENCH_FRAME_RUN;
            }
            bool redraw = false;
            if (down & (KEY_DUP | KEY_CPAD_UP)) {
                bench.cursor = (bench.cursor + count - 1) % count;
                redraw = true;
            }
            if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) {
                bench.cursor = (bench.cursor + 1) % count;
                redraw = true;
            }
            if ((down & KEY_A) && count > 1) {
                arrfree(bench.queue);
                bench.queue = nullptr;
                if (bench.cursor == 0) {
                    for (int32_t i = 0; count - 1 > i; i++) arrput(bench.queue, i);
                } else {
                    arrput(bench.queue, bench.cursor - 1);
                }
                bench.autoDismiss = false;
                bench.screenshots = false;
                benchRowsBegin();
                return BENCH_FRAME_PAUSE;
            }
            if (redraw) drawPicker();
            return BENCH_FRAME_PAUSE;
        }
        case PHASE_RESULTS: {
            int32_t n = (int32_t) arrlen(bench.results);
            bool redraw = false;
            if ((down & (KEY_DUP | KEY_CPAD_UP)) && n > 0) {
                bench.cursor = (bench.cursor + n - 1) % n;
                redraw = true;
            }
            if ((down & (KEY_DDOWN | KEY_CPAD_DOWN)) && n > 0) {
                bench.cursor = (bench.cursor + 1) % n;
                redraw = true;
            }
            if ((down & (KEY_A | KEY_B | KEY_START)) || (bench.autoDismiss && secondsSince(bench.phaseStart) > 5.0)) {
                restore();
                return BENCH_FRAME_RUN;
            }
            if (redraw) drawResults();
            return BENCH_FRAME_PAUSE;
        }
        default: break;
    }

    // Time spent in the HOME Menu or asleep isn't the game's: settle and measure again.
    if (bench.interrupted) {
        bench.interrupted = false;
        if (sweep.on && bench.phase == PHASE_MEASURE) {
            // A battle can't be measured again: the segment says so instead.
            logInfo("Sweep: %s was interrupted by the HOME Menu or sleep\n", currentResult()->id);
            sweepCut("interrupted by the HOME Menu or sleep");
        } else if (bench.phase == PHASE_WARMUP || bench.phase == PHASE_MEASURE) {
            logInfo("Bench: %s was interrupted by the HOME Menu or sleep, measuring again\n", currentResult()->id);
            bench.phase = PHASE_WARMUP;
            bench.phaseStart = nowNanos();
        }
    }
    if ((down & KEY_START) && bench.phase != PHASE_LOADING) {
        bench.cancelled = true;
        SceneResult* r = currentResult();
        if (r != nullptr && !r->measured && r->status[0] == '\0') copyText(r->status, sizeof(r->status), "cancelled");
        // tour=resume visits the room again.
        if (tour.on && !tour.booting && !tour.ending && tour.booted) {
            tourSavePosition("cancelled", tour.battles ? tour.enemies : tourRoomName(tour.room));
        }
        // sweep=resume fights the battle again.
        if (sweep.on && sweep.begun) sweepCut("cancelled");
        finish();
        return BENCH_FRAME_PAUSE;
    }
    if (bench.phase == PHASE_NEXT) {
        startNextScene();
        if (bench.phase == PHASE_RESULTS || bench.phase == PHASE_LOADING) return BENCH_FRAME_PAUSE;
        if (bench.phase == PHASE_NEXT) return BENCH_FRAME_PAUSE; // that scene couldn't start; the next one goes next frame
    }
    if (bench.phase == PHASE_LOADING) {
        if (nowNanos() - bench.lastStatus > 500000000ULL) {
            bench.lastStatus = nowNanos();
            drawStatus();
        }
        return BENCH_FRAME_PAUSE;
    }

    Runner* runner = bench.session.runner;
    BenchScriptState state = benchRowsTick(bench.script, runner);
    if (benchRoomsTakeShot(bench.script)) CtrRenderer_requestScreenshot(bench.session.renderer);
    // Full captures happen before this frame's slot wait. Only the incoming
    // interval carries their I/O; Step/Draw/GPU are timed after the capture.
    // Observe the actual save, covering manual, scheduled and benchmark shots.
    bool captured = CtrRenderer_screenshotCaptured(bench.session.renderer);
    bool captureTiming = CtrRenderer_screenshotTimingAffected(bench.session.renderer);
    if (captured) {
        logInfo("Bench: screenshot capture before frame %d; %.2f ms interval and notice intervals excluded from timing\n", runner->frameCount, frameMs);
    }
    if (state == BENCH_SCRIPT_FAILED) {
        char reason[96];
        snprintf(reason, sizeof(reason), "failed: %s", BenchScript_error(bench.script));
        endScene(reason);
        return BENCH_FRAME_RUN;
    }
    if (bench.phase == PHASE_SETUP && state == BENCH_SCRIPT_MEASURING && tour.booting) {
        tourBootEnded(nullptr);
    } else if (bench.phase == PHASE_SETUP && state == BENCH_SCRIPT_MEASURING && tour.ending) {
        tourBattleEnded(nullptr);
    } else if (bench.phase == PHASE_SETUP && state == BENCH_SCRIPT_MEASURING) {
        currentResult()->setupSeconds = secondsSince(bench.phaseStart);
        // The desktop runner's --bench-script logs the same frame, so a screenshot can be matched to its frame there.
        logInfo("Bench: %s ready at frame %d in %s\n", currentResult()->id, runner->frameCount, runner->currentRoom->name);
        bench.readyFrame = runner->frameCount;
        if (sweep.on) {
            // A battle is measured from here, its loading included (its "enter" segment).
            beginMeasuring();
            sweepBegin(bench.scenes[bench.queue[bench.queuePos]].script);
        } else {
            bench.phase = PHASE_WARMUP;
            bench.phaseStart = nowNanos();
        }
    } else if (bench.phase == PHASE_MEASURE && sweep.on) {
        if (bench.measureFrames > 0 && sweep.excludeIn != 1 && !captureTiming) arrput(bench.frameMs, (float) frameMs);
        if (sweep.excludeIn > 0) sweep.excludeIn--;
        sweepFrame();
        // The sweep's own work (a row written to the card) isn't the game's: left out of the shot's measure here, and
        // of the numbers by sweepCut.
        sweep.frameBegan = nowNanos();
    } else if (bench.phase == PHASE_WARMUP && (frameWindow() > 0 ? runner->frameCount - bench.readyFrame >= BENCH_SETTLE_FRAMES
                                                                 : secondsSince(bench.phaseStart) >= warmupSeconds())) {
        beginMeasuring();
#ifdef CTRPORT_RENDER_TIMING
        CtrRenderer_logPackedSet(bench.session.renderer, "start");
#endif
    } else if (bench.phase == PHASE_WARMUP) {
        if (!captureTiming && frameMs > currentResult()->loadMs) currentResult()->loadMs = frameMs;
    } else if (bench.phase == PHASE_MEASURE) {
        // frameMs is the frame before this one, which was measured unless this is the first measured frame.
        if (bench.measureFrames > 0 && !captureTiming) arrput(bench.frameMs, (float) frameMs);
        int32_t window = frameWindow(), measured = runner->frameCount - bench.measureStartFrame;
        bool half = window > 0 ? measured >= window / 2 : secondsSince(bench.phaseStart) >= measureSeconds() / 2;
        if (bench.screenshots && !bench.shotTaken && half) {
            bench.shotTaken = true;
            CtrRenderer_requestScreenshot(bench.session.renderer);
            logInfo("Bench: screenshot of %s at frame %d\n", currentResult()->id, runner->frameCount);
        }
        if (window > 0 ? measured >= window : secondsSince(bench.phaseStart) >= measureSeconds()) {
            // Which game frames the numbers cover, so two runs can be compared over the same ones.
            logInfo("Bench: %s measured frames %d-%d\n", currentResult()->id, bench.measureStartFrame, runner->frameCount - 1);
#ifdef CTRPORT_RENDER_TIMING
            CtrRenderer_logPackedSet(bench.session.renderer, "end");
#endif
            endScene(nullptr);
        }
    }
    if (bench.phase != PHASE_MEASURE && nowNanos() - bench.lastStatus > 1000000000ULL) {
        bench.lastStatus = nowNanos();
        drawStatus();
    }
    return BENCH_FRAME_RUN;
}

void CtrBench_endFrame(double stepMs, double drawMs, bool skipped) {
    if (bench.phase != PHASE_SETUP && bench.phase != PHASE_WARMUP && bench.phase != PHASE_MEASURE) return;
    Runner* runner = bench.session.runner;
    if (bench.phase == PHASE_MEASURE) {
        bench.stepSum += stepMs;
        bench.drawSum += drawMs;
        if (stepMs > bench.stepMax) bench.stepMax = stepMs;
        if (drawMs > bench.drawMax) bench.drawMax = drawMs;
        if (skipped) {
            bench.skippedFrames++;
        } else {
            double gpuMs = (double) CtrRenderer_getStats(bench.session.renderer).gpuMs;
            bench.gpuSum += gpuMs;
            if (gpuMs > bench.gpuMax) bench.gpuMax = gpuMs;
        }
        if (bench.measureFrames % 15 == 0) {
            struct mallinfo mi = mallinfo();
            if ((uint32_t) mi.uordblks > bench.heapPeak) bench.heapPeak = (uint32_t) mi.uordblks;
            uint32_t linearFree = linearSpaceFree();
            if (linearFree < bench.linearMinFree) bench.linearMinFree = linearFree;
            uint32_t vramFree = vramSpaceFree();
            if (vramFree < bench.vramMinFree) bench.vramMinFree = vramFree;
        }
        bench.measureFrames++;
        if (sweep.on && sweep.begun) sweepEndFrame(skipped);
    }
    // The game must not end itself or change chapter in the middle of a test (a gameend build lets it end).
    if (runner->shouldExit && !CTRPORT_LET_GAME_END) {
        runner->shouldExit = false;
        endScene("the game closed itself");
    } else if (CtrChapters_changeRequested(runner) && !bench.changeRequested) {
        logInfo("Bench: the game asked for game_change(\"%s\", \"%s\"); ignored\n", runner->pendingWorkingDirectory, runner->pendingLaunchParameters);
        free(runner->pendingWorkingDirectory);
        free(runner->pendingLaunchParameters);
        runner->pendingWorkingDirectory = nullptr;
        runner->pendingLaunchParameters = nullptr;
        endScene("the game tried to change chapter");
    }
}

// minigames: the running bench scene's id, which the renderer puts in screenshot names (ctr_shot_name.h), or
// nullptr outside a scene. Late code, appended after everything else: a declaration added above existing code here
// changes gcc's code for unrelated functions (computeResult, requestGameChange), so this file has no new include.
__attribute__((noinline, section(".text.late.minigames")))
const char* CtrBench_lateSceneId(void) {
    if (bench.phase != PHASE_SETUP && bench.phase != PHASE_WARMUP && bench.phase != PHASE_MEASURE) return nullptr;
    SceneResult* result = currentResult();
    return result != nullptr ? result->id : nullptr;
}

// Main 65726574's Release loadScenes slot is 576 B on all four targets. Subsection 1 follows its code and
// literal pool, so none of this zero padding executes. Keep the sized symbol too, for the full layout guard.
// Emit this after loadScenes (as in the other retained slots), without a scheduling barrier in its body.
#include "ctr_benchmark_benchromfs_layout.inc"
#if defined(__3DS__) && defined(NDEBUG)
static void __attribute__((used, noinline, section(".text.late.benchromfs"))) benchRomfsSlot(void) {
    __asm__(".pushsection .text.loadScenes, 1\n.space 576-(.-loadScenes),0\n.size loadScenes,576\n.popsection");
}
#endif

// minigames: the bench's sandbox, which each scene start clears (ctr_bench_scene_start.h). Late code, appended
// after everything else so nothing above it moves.
__attribute__((noinline, section(".text.late.sandbox")))
FileSystem* CtrBench_lateSandbox(void) {
    return bench.session.sandbox;
}

#include "ctr_benchmark_prewindow.inc"

// Preserve every existing source line: diagnostics expand __LINE__ into ARM
// immediates, whose encoded size can change and shift following warm code.
// The block-scope loader declaration shares its original call-site line.
__attribute__((noinline, noclone, section(".text.late.coverage")))
const char* coverageGameDir(void) {
    return bench.session.gameDir;
}

#include "ctr_benchmark_rooms.inc"

#include "ctr_benchmark_rooms_pool.inc"

// Read-only view of the ready boundary; option logic lives in its own unit.
#include "ctr_benchfid_window.h"
BenchfidWindowHook __attribute__((noinline, noclone, section(".text.late.benchfid")))
benchfidWindowHook(void) {
    return (BenchfidWindowHook) {
        .ready = bench.phase == PHASE_WARMUP && !tour.on && !sweep.on &&
            bench.session.display.benchFrames > 0 && bench.session.runner != nullptr &&
            bench.session.runner->frameCount == bench.readyFrame,
        .open = beginMeasuring,
    };
}

#include "ctr_benchfid_rooms.inc"
// logResult's smart line (smartcut). Its arguments are those of the
// logInfo it replaced: the "averaged %.1f boxes" format, the scene's id
// (SceneResult's first member, so the scene's result itself), its boxes and
// surface pixels a frame. Native frames note boxes only for a screenshot
// (smartNoteGate, ctr_renderer.c), so for a scene with any the count is not
// given; the line is as before otherwise.
static const char smartBoxesUncollected[] __attribute__((section(".rodata.smartcut_in_text"))) =
    "Bench: %s: smart boxes not collected, %.0f surface pixels a frame\n";
void CtrBench_lateSmartBoxesLine(const char* fmt, ...);
__attribute__((noinline, noipa, section(".text.late.smartcut"))) void CtrBench_lateSmartBoxesLine(const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    const char* id = __builtin_va_arg(args, const char*);
    double boxes = __builtin_va_arg(args, double), pixels = __builtin_va_arg(args, double);
    __builtin_va_end(args);
    const SceneResult* r = (const SceneResult*) (const void*) id;
#ifndef CTRPORT_SMART_BOXES
    if (0.0 < r->nativeShare) {
        logInfo(smartBoxesUncollected, id, pixels);
        return;
    }
#endif
    if (0.0 < boxes) logInfo(fmt, id, boxes, pixels);
}

#include "ctr_benchmark_rows.inc"

// Held-frame lines (stallfix rev 3): late code only, hooked by link-time wraps, so it goes last. Not in
// DELTARUNE Old builds (CMakeLists.txt sets CTRPORT_BENCH_HELD with the wraps).
#ifdef CTRPORT_BENCH_HELD
#include "ctr_benchheld.inc"
#endif

#ifdef CTRPORT_BENCHFIX_PROBE
// benchfix probe (ruling 2026-10-28; build.sh ... bfprobe, its own build directory, never a release). On every
// console sweep the main thread loses 0.3-0.9 s between "[prewindow] <sweep>" and "Sweep: <sweep> ... begins" while
// the battle music's stream opens on the audio thread; Azahar loses 3 ms. This times, on the main thread only and only
// in a sweep (its setup and window), every wait it can make: CtrLog_beginConsole (the log writer's drain; harness
// only in play), contended LightLock/RecursiveLock, LightEvent and svcWaitSynchronization waits (vblank and GPU waits
// land here too), svcSleepThread, and SD/RomFS calls (FSUSER/FSFILE/FSDIR). One "[bfprobe] sweep start" line splits
// the ready frame's bench call around CtrLog_beginConsole, with a CPU canary (a fixed loop; slower than in the setup
// frames = the main thread lost its core). In the window, one "[bfprobe] frame" line for each frame with main-thread
// FS, lock or condition-variable waits of 2 ms or more, or any console wait of 1 ms (at most 400 lines). Only call
// targets change. Rev 2 (10-28, a hunch: the main thread waits on something queued behind a stream open):
// the main thread never waits on an audio loader/opener/worker request itself (it queues them and polls; no audio thread
// holds a lock across a read or an open), so that can only show as an FS wait (the same fs:USER session or card), a
// lock or condition-variable wait (CondVar: the save writer's idle wait, the read-ahead), or a slow canary. For each
// frame line and the sweep start, the frame's longest such wait is named (call, ms, caller address and object: map the
// address with addr2line on the build's .elf) with every other named thread's last watchdog note at its start (the
// loader's, opener's, decode thread's... current job).
// Its code is too big for UNDERTALE o3DS's late code reserve, so it sits in ordinary text after
// ctr_benchmark.o's: a probe build's later text moves (its timings are for this probe only). Its constants sit in
// ordinary .rodata with it (round 6, argstage47 F3: DELTARUNE Old's late constants had 410 B left after
// integrate21's merges, and the probe's 867 B no longer fit).
#define BFP_LATE __attribute__((noinline, noclone, section(".text.bfprobe")))
#define BFP_RO __attribute__((section(".rodata.bfprobe")))
enum { BFP_CONSOLE, BFP_LOCK, BFP_EVENT, BFP_SLEEP, BFP_SYNC, BFP_FS, BFP_COND, BFP_KINDS };
#define BFP_THREADS 16
// One wait, with what the other named threads last noted when it began.
typedef struct {
    u64 ticks;
    const char* what;
    const char* mainPhase; // the main thread's watchdog phase (step, draw, audio update, room change...)
    u32 caller, object, threads;
    const char* names[BFP_THREADS];
    const char* states[BFP_THREADS];
} BfpWait;
typedef struct { u64 ticks[BFP_KINDS]; u64 max[BFP_KINDS]; u32 count[BFP_KINDS]; BfpWait longest; } BfpSum;
static struct {
    BfpSum frame; // since the last benchfidBeginFrame (or the sweep start line)
    u64 canaryBase, canaryConsole, consoleEnter, consoleLeave;
    void* mainTls; // the main thread's TLS block
    u32 lines, suppressed;
    bool armed, reporting, starting;
    char text[720];
} bfp __attribute__((section(".data.late.zz_benchfix_probe"))) = {0};
int32_t CtrWatchdog_bfpNotes(const char** names, const char** states, int32_t max); // ctr_watchdog.c, probe builds
const char* CtrWatchdog_phase(void);
// Every file opened on any thread (probe builds, from boot): its handle and the tail of its path, so a wait on a file
// handle names the file (the cache file a shed closes, an audio group, a tile pack...). Ordinary BSS (probe builds only).
#define BFP_FILES 96
static struct { Handle handle; char name[44]; } bfpFiles[BFP_FILES];
static u32 bfpFileNext;
static const char bfpStartFormat[] BFP_RO =
    "[bfprobe] sweep start frame %lu: bench call %.1f ms = before console %.1f + console wait %.1f + after %.1f; "
    "canary us setup-best %.1f, at console %.1f, at end %.1f\n";
static const char bfpWaitFormat[] BFP_RO =
    "[bfprobe] %s %lu: fs %lu/%.1f/%.1f lock %lu/%.1f/%.1f console %lu/%.1f sleep %lu/%.1f/%.1f sync %lu/%.1f/%.1f "
    "event %lu/%.1f/%.1f cond %lu/%.1f/%.1f (count/ms/max ms, main thread; %lu lines suppressed)\n";
static const char bfpLongestFormat[] BFP_RO =
    "[bfprobe] longest wait %s %lu: %s %.1f ms, called from %08lx, object %08lx%s%s, main thread in %s; other threads "
    "at its start: %s\n";
static const char bfpFileWord[] BFP_RO = " file ";
static const char bfpThreadFormat[] BFP_RO = "%s%s=%s";
static const char bfpSeparator[] BFP_RO = "; ";
static const char bfpNone[] BFP_RO = "";
static const char bfpConsoleName[] BFP_RO = "CtrLog_beginConsole";
static const char bfpLightLockName[] BFP_RO = "LightLock_Lock";
static const char bfpRecursiveLockName[] BFP_RO = "RecursiveLock_Lock";
static const char bfpCondName[] BFP_RO = "CondVar_Wait";
static const char bfpCondTimeoutName[] BFP_RO = "CondVar_WaitTimeout";
static const char bfpStartName[] BFP_RO = "sweep start waits, frame";
static const char bfpFrameName[] BFP_RO = "frame";

// The thread test reads the TLS register only: threadGetCurrent would svcBreak on ctr_exit.c's guard, a thread made
// with svcCreateThread (no libctru thread block), which sleeps through these wraps.
static inline __attribute__((always_inline)) bool bfpOn(void) {
    return bfp.armed && !bfp.reporting && getThreadLocalStorage() == bfp.mainTls;
}

static BFP_LATE u64 bfpAdd(int kind, u64 start) {
    u64 d = svcGetSystemTick() - start;
    bfp.frame.ticks[kind] += d;
    bfp.frame.count[kind]++;
    if (d > bfp.frame.max[kind]) bfp.frame.max[kind] = d;
    return d;
}

// At a wait's start: what it is and what every other named thread last noted (pointers only).
static BFP_LATE void bfpSnap(BfpWait* w, const char* what, const void* caller, const void* object) {
    w->what = what;
    w->mainPhase = CtrWatchdog_phase();
    w->caller = (u32) caller;
    w->object = (u32) object;
    w->threads = (u32) CtrWatchdog_bfpNotes(w->names, w->states, BFP_THREADS);
}

// At its end: timed as kind, and kept if it is the frame's longest FS, lock, condition-variable or console wait.
static BFP_LATE void bfpEnd(int kind, u64 start, BfpWait* w) {
    u64 d = bfpAdd(kind, start);
    if (d > bfp.frame.longest.ticks) {
        w->ticks = d;
        bfp.frame.longest = *w;
    }
}

static BFP_LATE double bfpMs(u64 ticks) {
    return (double) ticks / (SYSCLOCK_ARM11 / 1000.0);
}

// Any thread: a file was opened as handle h.
static BFP_LATE void bfpFileOpened(Handle h, FS_Path path) {
    u32 i = __atomic_fetch_add(&bfpFileNext, 1, __ATOMIC_RELAXED) % BFP_FILES;
    char* d = bfpFiles[i].name;
    const u32 cap = sizeof(bfpFiles[i].name) - 1;
    u32 n = 0;
    if (path.type == PATH_UTF16 && path.data != nullptr) {
        const u16* s = (const u16*) path.data;
        u32 chars = path.size / 2;
        while (chars > 0 && s[chars - 1] == 0) chars--;
        for (u32 k = chars > cap ? chars - cap : 0; k < chars; k++) d[n++] = s[k] < 0x80 ? (char) s[k] : '?';
    } else if (path.type == PATH_ASCII && path.data != nullptr) {
        const char* s = (const char*) path.data;
        u32 chars = (u32) strnlen(s, path.size);
        for (u32 k = chars > cap ? chars - cap : 0; k < chars; k++) d[n++] = s[k];
    }
    d[n] = '\0';
    bfpFiles[i].handle = h;
}

// The newest file opened as handle h, or "".
static BFP_LATE const char* bfpFileName(u32 h) {
    if (h == 0) return bfpNone;
    u32 next = bfpFileNext;
    for (u32 k = 1; k <= BFP_FILES && k <= next; k++) {
        u32 i = (next - k) % BFP_FILES;
        if (bfpFiles[i].handle == h) return bfpFiles[i].name;
    }
    return bfpNone;
}

static BFP_LATE void bfpLongest(const char* name, unsigned long frame, const BfpWait* w) {
    if (w->what == nullptr) return;
    size_t n = 0;
    bfp.text[0] = '\0';
    for (u32 i = 0; i < w->threads && n + 1 < sizeof(bfp.text); i++) {
        int k = snprintf(bfp.text + n, sizeof(bfp.text) - n, bfpThreadFormat, i > 0 ? bfpSeparator : bfpNone, w->names[i],
                         w->states[i] != nullptr ? w->states[i] : bfpNone);
        if (k < 0) break;
        n += (size_t) k;
    }
    const char* file = bfpFileName(w->object);
    bfp.reporting = true;
    logInfo(bfpLongestFormat, name, frame, w->what, bfpMs(w->ticks), (unsigned long) w->caller, (unsigned long) w->object,
            file[0] != '\0' ? bfpFileWord : bfpNone, file, w->mainPhase != nullptr ? w->mainPhase : bfpNone, bfp.text);
    bfp.reporting = false;
}

static BFP_LATE u64 bfpCanary(void) {
    u64 t = svcGetSystemTick();
    for (volatile u32 i = 0; i < 4096; i++) {}
    return svcGetSystemTick() - t;
}

static BFP_LATE void bfpWaits(const char* name, unsigned long frame, const BfpSum* s) {
    bfp.reporting = true;
    logInfo(bfpWaitFormat, name, frame,
            (unsigned long) s->count[BFP_FS], bfpMs(s->ticks[BFP_FS]), bfpMs(s->max[BFP_FS]),
            (unsigned long) s->count[BFP_LOCK], bfpMs(s->ticks[BFP_LOCK]), bfpMs(s->max[BFP_LOCK]),
            (unsigned long) s->count[BFP_CONSOLE], bfpMs(s->ticks[BFP_CONSOLE]),
            (unsigned long) s->count[BFP_SLEEP], bfpMs(s->ticks[BFP_SLEEP]), bfpMs(s->max[BFP_SLEEP]),
            (unsigned long) s->count[BFP_SYNC], bfpMs(s->ticks[BFP_SYNC]), bfpMs(s->max[BFP_SYNC]),
            (unsigned long) s->count[BFP_EVENT], bfpMs(s->ticks[BFP_EVENT]), bfpMs(s->max[BFP_EVENT]),
            (unsigned long) s->count[BFP_COND], bfpMs(s->ticks[BFP_COND]), bfpMs(s->max[BFP_COND]),
            (unsigned long) bfp.suppressed);
    bfp.reporting = false;
    bfpLongest(name, frame, &s->longest);
}

// Called by __wrap_benchfidBeginFrame before the bench's own frame call: the last frame's line, then arming.
static BFP_LATE void bfpBefore(const Runner* runner) {
    BfpSum last = bfp.frame;
    memset(&bfp.frame, 0, sizeof(bfp.frame));
    bfp.mainTls = getThreadLocalStorage();
    if (bfp.armed && sweep.on && bench.phase == PHASE_MEASURE && runner != nullptr &&
        (last.ticks[BFP_FS] >= SYSCLOCK_ARM11 / 500 || last.ticks[BFP_LOCK] >= SYSCLOCK_ARM11 / 500 ||
         last.ticks[BFP_COND] >= SYSCLOCK_ARM11 / 500 || last.ticks[BFP_CONSOLE] >= SYSCLOCK_ARM11 / 1000)) {
        if (bfp.lines < 400) {
            bfp.lines++;
            bfpWaits(bfpFrameName, (unsigned long) (runner->frameCount - 1), &last);
        } else {
            bfp.suppressed++;
        }
    }
    bfp.armed = sweep.on && (bench.phase == PHASE_SETUP || bench.phase == PHASE_MEASURE);
    bfp.starting = bfp.armed && bench.phase == PHASE_SETUP;
    if (bfp.starting) {
        u64 c = bfpCanary();
        if (bfp.canaryBase == 0 || c < bfp.canaryBase) bfp.canaryBase = c;
        bfp.consoleEnter = bfp.consoleLeave = bfp.canaryConsole = 0;
    }
}

// After it: on the frame the sweep's window opened, the split of that call.
static BFP_LATE void bfpAfter(const Runner* runner, u64 t0) {
    if (!bfp.starting || bench.phase != PHASE_MEASURE || runner == nullptr) return;
    u64 t1 = svcGetSystemTick();
    u64 c1 = bfpCanary();
    u64 enter = bfp.consoleEnter != 0 ? bfp.consoleEnter : t1, leave = bfp.consoleLeave != 0 ? bfp.consoleLeave : t1;
    BfpSum s = bfp.frame;
    memset(&bfp.frame, 0, sizeof(bfp.frame));
    bfp.starting = false;
    bfp.reporting = true;
    logInfo(bfpStartFormat, (unsigned long) runner->frameCount, bfpMs(t1 - t0), bfpMs(enter - t0), bfpMs(leave - enter),
            bfpMs(t1 - leave), bfpMs(bfp.canaryBase) * 1000.0, bfpMs(bfp.canaryConsole) * 1000.0, bfpMs(c1) * 1000.0);
    bfp.reporting = false;
    bfpWaits(bfpStartName, (unsigned long) runner->frameCount, &s);
}

void __real_CtrLog_beginConsole(void);
BFP_LATE void __wrap_CtrLog_beginConsole(void) {
    if (!bfpOn()) {
        __real_CtrLog_beginConsole();
        return;
    }
    bool first = bfp.starting && bfp.consoleEnter == 0;
    if (first) bfp.canaryConsole = bfpCanary();
    BfpWait w;
    bfpSnap(&w, bfpConsoleName, __builtin_return_address(0), nullptr);
    u64 t = svcGetSystemTick();
    __real_CtrLog_beginConsole();
    if (first) {
        bfp.consoleEnter = t;
        bfp.consoleLeave = svcGetSystemTick();
    }
    bfpEnd(BFP_CONSOLE, t, &w);
}

void __real_LightLock_Lock(LightLock* lock);
BFP_LATE void __wrap_LightLock_Lock(LightLock* lock) {
    if (!bfpOn()) {
        __real_LightLock_Lock(lock);
        return;
    }
    if (LightLock_TryLock(lock) == 0) return;
    BfpWait w;
    bfpSnap(&w, bfpLightLockName, __builtin_return_address(0), lock);
    u64 t = svcGetSystemTick();
    __real_LightLock_Lock(lock);
    bfpEnd(BFP_LOCK, t, &w);
}

void __real_RecursiveLock_Lock(RecursiveLock* lock);
BFP_LATE void __wrap_RecursiveLock_Lock(RecursiveLock* lock) {
    if (!bfpOn()) {
        __real_RecursiveLock_Lock(lock);
        return;
    }
    if (RecursiveLock_TryLock(lock) == 0) return;
    BfpWait w;
    bfpSnap(&w, bfpRecursiveLockName, __builtin_return_address(0), lock);
    u64 t = svcGetSystemTick();
    __real_RecursiveLock_Lock(lock);
    bfpEnd(BFP_LOCK, t, &w);
}

// A condition variable: the main thread waiting for a worker (the save writer's idle wait, the read-ahead).
void __real_CondVar_Wait(CondVar* cv, LightLock* lock);
BFP_LATE void __wrap_CondVar_Wait(CondVar* cv, LightLock* lock) {
    if (!bfpOn()) {
        __real_CondVar_Wait(cv, lock);
        return;
    }
    BfpWait w;
    bfpSnap(&w, bfpCondName, __builtin_return_address(0), cv);
    u64 t = svcGetSystemTick();
    __real_CondVar_Wait(cv, lock);
    bfpEnd(BFP_COND, t, &w);
}

int __real_CondVar_WaitTimeout(CondVar* cv, LightLock* lock, s64 timeout);
BFP_LATE int __wrap_CondVar_WaitTimeout(CondVar* cv, LightLock* lock, s64 timeout) {
    if (!bfpOn()) return __real_CondVar_WaitTimeout(cv, lock, timeout);
    BfpWait w;
    bfpSnap(&w, bfpCondTimeoutName, __builtin_return_address(0), cv);
    u64 t = svcGetSystemTick();
    int r = __real_CondVar_WaitTimeout(cv, lock, timeout);
    bfpEnd(BFP_COND, t, &w);
    return r;
}

void __real_LightEvent_Wait(LightEvent* event);
BFP_LATE void __wrap_LightEvent_Wait(LightEvent* event) {
    if (!bfpOn()) {
        __real_LightEvent_Wait(event);
        return;
    }
    u64 t = svcGetSystemTick();
    __real_LightEvent_Wait(event);
    bfpAdd(BFP_EVENT, t);
}

int __real_LightEvent_WaitTimeout(LightEvent* event, s64 timeout);
BFP_LATE int __wrap_LightEvent_WaitTimeout(LightEvent* event, s64 timeout) {
    if (!bfpOn()) return __real_LightEvent_WaitTimeout(event, timeout);
    u64 t = svcGetSystemTick();
    int r = __real_LightEvent_WaitTimeout(event, timeout);
    bfpAdd(BFP_EVENT, t);
    return r;
}

void __real_svcSleepThread(s64 ns);
BFP_LATE void __wrap_svcSleepThread(s64 ns) {
    if (!bfpOn()) {
        __real_svcSleepThread(ns);
        return;
    }
    u64 t = svcGetSystemTick();
    __real_svcSleepThread(ns);
    bfpAdd(BFP_SLEEP, t);
}

Result __real_svcWaitSynchronization(Handle handle, s64 ns);
BFP_LATE Result __wrap_svcWaitSynchronization(Handle handle, s64 ns) {
    if (!bfpOn()) return __real_svcWaitSynchronization(handle, ns);
    u64 t = svcGetSystemTick();
    Result r = __real_svcWaitSynchronization(handle, ns);
    bfpAdd(BFP_SYNC, t);
    return r;
}

Result __real_svcWaitSynchronizationN(s32* out, const Handle* handles, s32 n, bool all, s64 ns);
BFP_LATE Result __wrap_svcWaitSynchronizationN(s32* out, const Handle* handles, s32 n, bool all, s64 ns) {
    if (!bfpOn()) return __real_svcWaitSynchronizationN(out, handles, n, all, ns);
    u64 t = svcGetSystemTick();
    Result r = __real_svcWaitSynchronizationN(out, handles, n, all, ns);
    bfpAdd(BFP_SYNC, t);
    return r;
}

// The SD card and RomFS calls (newlib's sdmc and romfs devices and the engine's own file code all end in these). The
// object is the file or directory handle (0 for the FSUSER calls).
#define BFP_FS_WRAP(name, params, args, object) \
    static const char bfpName_##name[] BFP_RO = #name; \
    Result __real_##name params; \
    BFP_LATE Result __wrap_##name params { \
        if (!bfpOn()) return __real_##name args; \
        BfpWait w; \
        bfpSnap(&w, bfpName_##name, __builtin_return_address(0), (const void*) (object)); \
        u64 t = svcGetSystemTick(); \
        Result r = __real_##name args; \
        bfpEnd(BFP_FS, t, &w); \
        return r; \
    }
// The two opens also note every thread's opened file (handle -> path tail), from boot.
static const char bfpName_FSUSER_OpenFile[] BFP_RO = "FSUSER_OpenFile";
Result __real_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 flags, u32 attributes);
BFP_LATE Result __wrap_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 flags, u32 attributes) {
    if (!bfpOn()) {
        Result r = __real_FSUSER_OpenFile(out, archive, path, flags, attributes);
        if (R_SUCCEEDED(r) && out != nullptr) bfpFileOpened(*out, path);
        return r;
    }
    BfpWait w;
    bfpSnap(&w, bfpName_FSUSER_OpenFile, __builtin_return_address(0), nullptr);
    u64 t = svcGetSystemTick();
    Result r = __real_FSUSER_OpenFile(out, archive, path, flags, attributes);
    // A failed open (stat's probe of a missing path) is noted as handle ffffffff, the main thread's newest such.
    w.object = R_SUCCEEDED(r) && out != nullptr ? (u32) *out : 0xFFFFFFFFu;
    bfpFileOpened((Handle) w.object, path);
    bfpEnd(BFP_FS, t, &w);
    return r;
}
static const char bfpName_FSUSER_OpenFileDirectly[] BFP_RO = "FSUSER_OpenFileDirectly";
Result __real_FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID id, FS_Path archivePath, FS_Path filePath, u32 flags,
                                      u32 attributes);
BFP_LATE Result __wrap_FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID id, FS_Path archivePath, FS_Path filePath,
                                               u32 flags, u32 attributes) {
    if (!bfpOn()) {
        Result r = __real_FSUSER_OpenFileDirectly(out, id, archivePath, filePath, flags, attributes);
        if (R_SUCCEEDED(r) && out != nullptr) bfpFileOpened(*out, filePath);
        return r;
    }
    BfpWait w;
    bfpSnap(&w, bfpName_FSUSER_OpenFileDirectly, __builtin_return_address(0), nullptr);
    u64 t = svcGetSystemTick();
    Result r = __real_FSUSER_OpenFileDirectly(out, id, archivePath, filePath, flags, attributes);
    w.object = R_SUCCEEDED(r) && out != nullptr ? (u32) *out : 0xFFFFFFFFu;
    bfpFileOpened((Handle) w.object, filePath);
    bfpEnd(BFP_FS, t, &w);
    return r;
}
static const char bfpName_FSUSER_OpenDirectory[] BFP_RO = "FSUSER_OpenDirectory";
Result __real_FSUSER_OpenDirectory(Handle* out, FS_Archive archive, FS_Path path);
BFP_LATE Result __wrap_FSUSER_OpenDirectory(Handle* out, FS_Archive archive, FS_Path path) {
    if (!bfpOn()) return __real_FSUSER_OpenDirectory(out, archive, path);
    BfpWait w;
    bfpSnap(&w, bfpName_FSUSER_OpenDirectory, __builtin_return_address(0), nullptr);
    u64 t = svcGetSystemTick();
    Result r = __real_FSUSER_OpenDirectory(out, archive, path);
    w.object = R_SUCCEEDED(r) && out != nullptr ? (u32) *out : 0xFFFFFFFFu;
    bfpFileOpened((Handle) w.object, path);
    bfpEnd(BFP_FS, t, &w);
    return r;
}
BFP_FS_WRAP(FSUSER_CreateDirectory, (FS_Archive archive, FS_Path path, u32 attributes), (archive, path, attributes), 0)
BFP_FS_WRAP(FSUSER_DeleteFile, (FS_Archive archive, FS_Path path), (archive, path), 0)
BFP_FS_WRAP(FSUSER_RenameFile, (FS_Archive a, FS_Path p, FS_Archive b, FS_Path q), (a, p, b, q), 0)
BFP_FS_WRAP(FSFILE_Read, (Handle h, u32* got, u64 offset, void* buffer, u32 size), (h, got, offset, buffer, size), h)
BFP_FS_WRAP(FSFILE_Write, (Handle h, u32* put, u64 offset, const void* buffer, u32 size, u32 flags),
            (h, put, offset, buffer, size, flags), h)
BFP_FS_WRAP(FSFILE_GetSize, (Handle h, u64* size), (h, size), h)
BFP_FS_WRAP(FSFILE_Flush, (Handle h), (h), h)
BFP_FS_WRAP(FSFILE_Close, (Handle h), (h), h)
BFP_FS_WRAP(FSDIR_Read, (Handle h, u32* got, u32 count, FS_DirectoryEntry* entries), (h, got, count, entries), h)
#endif

// benchfix (2026-10-28): a tour room whose warp frame alone outlasts the 1 s warm-up (a cold first room, such as
// c0r145's 1455.6 ms and c0r112's 1286.3 ms on the New 3DS) opened its window on the very next frame, before the flip
// that ends the old room's hold: room presentation 1 counted that hold (shown_3vb=1, shown_max_vb=89) and loadMs
// missed the load. Its warm-up now starts after the warp, as it already does for a room whose warp is shorter, so
// those rows change and no other does. Linked as --wrap=benchfidBeginFrame (z_utpolish_bench.c's one call), in a
// section of its own after all other late text, so nothing above moves.
//
// benchfix (2026-10-28): the sweep-start stall (0.3-0.9 s on every console between "[prewindow] <sweep>" and
// "Sweep: ... begins", Azahar ~20 ms). sweepBegin -> sweepParseRecipe asks chapterAvailable(1), a stat of
// "<gameDir>/chapter1_windows/data.win" on the SD card (in UNDERTALE a path that never exists: libctru's archive_stat
// then makes an FSUSER_OpenFile and an FSUSER_OpenDirectory), in the very frame the battle music's stream opens on the
// opener thread, so the main thread's calls wait behind the stream's card reads (bfprobe: FSUSER_OpenDirectory from
// archive_stat, the sweep start's longest wait). The answer is now taken once, on the bench's first frame (before its
// first scene's window), and while a bench runs a stat of that exact path answers from it (the file's presence does
// not change in a run). Linked as --wrap=_stat_r (newlib's stat()'s one call), so only that call target moves.
static struct {
    bool filled;
    int result, error;
    struct stat st;
    char path[128];
} bfStat __attribute__((section(".data.benchfix_late"))) = {0};

// In .text.benchfix_stat_late, after __wrap_benchfidBeginFrame, so its place holds (CtrBench_beginFrameTimed's call stays).
static void bfStatFill(void);
// In .text.benchfix_lines_late, after those.
static void bfWindowLines(double frameMs);
static void bfHeldAfter(void);

// Round 6 (10-28, argstage47 F1): these wraps run in every player's game, where no bench is active, so each now
// tests the bench phase first (one load and a branch) and otherwise goes straight to the real call. The bench's own
// work moved to .text.benchfix_r6_late (the end of this file; late_reserve.ld, last), and each wrap keeps its stage
// slot (cand24, 353db7f54): its body is padded to the old size with a fill reached only by a phase that never occurs
// (BF_SLOT_PHASE), so no late code after it moves (check_layout.py --full against the stage: 0 failures).
#define BF_SLOT_PHASE 255
#define BF_SLOT_FILL(bytes) do { __asm__ volatile(".space " bytes); __builtin_unreachable(); } while (0)
#ifdef CTRPORT_BENCHFIX_DRAWWAIT
#define BF_FILL_BEGIN "192" // the stage body: 228 B with the held lines' bfHeldAfter call
#else
#define BF_FILL_BEGIN "180" // 216 B (DELTARUNE Old)
#endif
#define BF_FILL_STAT "112" // 148 B
#define BF_FILL_FRAME "104" // bfBeginFrameBench without the held lines: 236 B
CtrBenchFrame bfBeginFrameBench(u32 down, double frameMs);
int bfStatAnswer(struct _reent* r, const char* path, struct stat* st);

CtrBenchFrame __real_benchfidBeginFrame(u32 down, double frameMs);
__attribute__((noinline, noclone, section(".text.benchfix_late")))
CtrBenchFrame __wrap_benchfidBeginFrame(u32 down, double frameMs) {
    if (__builtin_expect(bench.phase == PHASE_IDLE, 1)) return __real_benchfidBeginFrame(down, frameMs);
    if (__builtin_expect(bench.phase != BF_SLOT_PHASE, 1)) return bfBeginFrameBench(down, frameMs);
    BF_SLOT_FILL(BF_FILL_BEGIN);
}

int __real__stat_r(struct _reent* r, const char* path, struct stat* st);
__attribute__((noinline, noclone, section(".text.benchfix_stat_late")))
int __wrap__stat_r(struct _reent* r, const char* path, struct stat* st) {
    if (__builtin_expect(bench.phase == PHASE_IDLE, 1)) return __real__stat_r(r, path, st);
    if (__builtin_expect(bench.phase != BF_SLOT_PHASE, 1)) return bfStatAnswer(r, path, st);
    BF_SLOT_FILL(BF_FILL_STAT);
}

// The bench's first frame: chapterAvailable(1)'s path, asked once.
__attribute__((noinline, noclone, section(".text.benchfix_stat_late")))
static void bfStatFill(void) {
    if (bfStat.filled || bench.session.gameDir == nullptr) return;
    int n = snprintf(bfStat.path, sizeof(bfStat.path), "%s/chapter%ld_windows/data.win", bench.session.gameDir, 1L);
    if (n <= 0 || n >= (int) sizeof(bfStat.path)) return;
    struct _reent* r = _REENT;
    int saved = r->_errno;
    bfStat.result = __real__stat_r(r, bfStat.path, &bfStat.st);
    bfStat.error = r->_errno;
    r->_errno = saved;
    bfStat.filled = true;
}

// benchfix (2026-10-28, gap 1): utpolish's "Frame over 33.4 ms" lines (z_utpolish_bench.c) stop after
// SLOW_ROOM_LINES (16) a room visit, and its count starts again only at a room change. A scene whose room intro
// already spent them (all four drsweep22 Knight logs: 16 lines by frame 60-63, before the window) logged no per-frame
// line inside its measured window. While the bench measures, each frame now starts with utpolish's count at zero, so
// every late frame (>= 41.8 ms, slowlog; legacy prefix unchanged) gets its line, up to BF_WINDOW_LINES a room visit
// in that window (20 s is at most 599 frames; a sweep spans its rows, so the budget is per room visit). After the
// window, utpolish's own count comes back as it was, so lines outside windows are as before. The line is utpolish's
// (same text, same place: CtrBench_beginFrameTimed, right after this wrapper returns), queued to the log writer as
// before. This call runs first in CtrBench_beginFrameTimed, so a frame whose interval arrives while the phase is
// still MEASURE (the window's frames, its last one included) is in the window.
#include "z_utpolish_bench.h"
#define BF_WINDOW_LINES 600
static struct {
    bool on;
    int32_t savedRoom, savedLines; // utpolish's count when the window began
    int32_t start, room; // the window (its first frame) and room visit the budget is for
    uint32_t lines;
} bfWin __attribute__((no_reorder, section(".data.benchfix_lines_late"))) = {0};

__attribute__((noinline, noclone, section(".text.benchfix_lines_late")))
static void bfWindowLines(double frameMs) {
    if (bench.phase != PHASE_MEASURE) {
        if (bfWin.on) {
            bfWin.on = false;
            utSlowFrame.linesRoom = bfWin.savedRoom;
            utSlowFrame.lines = bfWin.savedLines;
        }
        return;
    }
    if (!bfWin.on) {
        bfWin.on = true;
        bfWin.savedRoom = utSlowFrame.linesRoom;
        bfWin.savedLines = utSlowFrame.lines;
        bfWin.start = -1;
    }
    if (bfWin.start != bench.measureStartFrame || bfWin.room != utSlowFrame.room) {
        bfWin.start = bench.measureStartFrame;
        bfWin.room = utSlowFrame.room;
        bfWin.lines = 0;
    }
    utSlowFrame.linesRoom = utSlowFrame.room;
    if (bfWin.lines >= BF_WINDOW_LINES) {
        utSlowFrame.lines = INT32_MAX; // spent: utpolish logs no more in this visit's window
        return;
    }
    // utpolish's test for a line (its runner, renderer and CtrBench_running() checks hold whenever the phase is MEASURE).
    if (utSlowFrame.valid && frameMs >= 41.8) bfWin.lines++;
    utSlowFrame.lines = 0;
}

// benchfix (2026-10-28, gaps 2 and 3): the held_frame lines' gpu and gpuwait (ctr_benchheld.inc, stallfix,
// which must be included above this point: bfHeldAfter reads its benchHeld).
// gpu: heldRead gives a frame its GPU time one read later (as frame F begins, the GPU's last finished frame is taken
// to be F-2), but a hold whose late flip is frame F-1's is listed in the read where F-1's entry is made, before its
// time is known, and kept 0.0 (302 of 340 movebench Knight lines). After each held read, a listed frame F-2 now takes
// the time its ring entry just got.
// gpuwait: CtrGpu_waitTotalTicks counts CtrGpu_waitIdle only; CtrGpu_waitDraws (the spare-texture fence,
// ctr_renderer.c initLinearTextureForCpu) slept without adding to it. Its waits (--wrap=CtrGpu_waitDraws, the one
// call) are now added to the gpuwait of the frame they fell in, in the ring and in a listed frame. CtrGpu_waitTotalTicks
// is unchanged, so the result line's gpu wait and the pacer's "waited" keep their meaning.
// Both run after the held read, in bfHeldAfter after the bench's frame call, and (round 6, argstage47 F2) also right
// before heldReport: the call that closes a window reads its last frame and prints the held lines inside the same
// bench call (endScene -> computeResult -> CtrPacer_logLevels), so the after-call pass came too late for that read
// (its frame B-1 kept gpu=0.0 and frame B lacked its waitDraws time). The wrap is on wherever the held lines are
// (CMakeLists.txt); without them only the wrap's counter runs.
#ifdef CTRPORT_BENCHFIX_DRAWWAIT
static uint32_t bfDrawWait __attribute__((no_reorder, section(".data.benchfix_lines_late"))) = 0; // ticks (low word)

#define BF_FILL_DRAWS "12" // 48 B
void __real_CtrGpu_waitDraws(void);
void __wrap_CtrGpu_waitDraws(void);
void bfDrawWaitTimed(void);
__attribute__((noinline, noclone, section(".text.benchfix_lines_late")))
void __wrap_CtrGpu_waitDraws(void) {
    if (__builtin_expect(bench.phase == PHASE_IDLE, 1)) {
        __real_CtrGpu_waitDraws();
        return;
    }
    if (__builtin_expect(bench.phase != BF_SLOT_PHASE, 1)) {
        bfDrawWaitTimed();
        return;
    }
    BF_SLOT_FILL(BF_FILL_DRAWS);
}
#endif

#if defined(CTRPORT_BENCH_HELD) && defined(CTRPORT_BENCHFIX_DRAWWAIT)
static uint32_t bfDrawWaitSeen __attribute__((no_reorder, section(".data.benchfix_lines_late"))) = 0;

__attribute__((noinline, noclone, section(".text.benchfix_lines_late")))
static void bfHeldAfter(void) {
    uint32_t drawWait = bfDrawWait - bfDrawWaitSeen; // since the last frame's begin: the frame just ended (F-1)
    bfDrawWaitSeen = bfDrawWait;
    const Runner* runner = bench.session.runner;
    if (runner == nullptr || !benchHeld.open || !benchHeld.started) return;
    uint32_t now = (uint32_t) runner->frameCount;
    BenchHeld* f = &benchHeld.ring[(now - 1) & 3];
    // Only after a held read in this call (it made frame F-1's entry).
    if (f->frame != now - 1 || now < 3) return;
    float ms = (float) drawWait * (float) (1.0 / CPU_TICKS_PER_MSEC);
    f->t[4] += ms;
    const BenchHeld* g = &benchHeld.ring[(now - 2) & 3];
    for (uint32_t i = benchHeld.listed; i > 0; i--) {
        BenchHeld* r = &benchHeld.list[i - 1];
        if (r->frame + 3 < now) break;
        if (r->frame == now - 1) r->t[4] += ms;
        else if (r->frame == now - 2 && g->frame == now - 2) r->t[3] = g->t[3];
    }
}
#else
static void bfHeldAfter(void) {
}
#endif

// ===== benchfix round 6 (10-28): .text.benchfix_r6_late and .data.benchfix_r6_late (late_reserve.ld, last).
// no_reorder (round 7): kept in source order, their cand24 order, whatever calls them (bfWin, bfDrawWait,
// bfDrawWaitSeen and bfHeldSnap too: GCC emits statics in first-use order otherwise, and round 7 swapped two).
#define BF_R6 __attribute__((noinline, noclone, no_reorder, optimize("Os"), section(".text.benchfix_r6_late")))

// c0r148 (round 6): a hold before the window. The window's first held read sees the flips still pending at its
// start; a late one is a frame from before the window (c0r148 o3ds: held f987 for window 988-1287,
// integ24-o3c-uthot-3B), and both the held lines and the presentation counts (shown_3vb, shown_max_vb, which the
// boards judge) took it against the scene. Frames before the window's first measured frame never count: after the
// bench's frame call, a hold heldAdd listed in it for such a frame (play or black; a hidden hold is presentation's own
// case) leaves the list, its class count, and the visit's presentation counts it went into (a snapshot of the visit
// from before the call restores shown_max_vb). Scenes with no such hold are unchanged. Same build condition as the
// held lines.
#if defined(CTRPORT_BENCH_HELD) && defined(CTRPORT_BENCHFIX_DRAWWAIT)
static struct {
    bool on;
    uint16_t listed;
    uint32_t visit;
    int32_t start;
    BenchShownVisit v;
} bfHeldSnap __attribute__((no_reorder, section(".data.benchfix_r6_late"))) = {0};

// Round 7 (argbenchr6 M1-M4): the snapshot and the boundary are bfHeldBefore7 and bfHeldBoundary7 (the end of this
// file). These keep their round 6 slots (cand24): padded to the old size with a fill reached only by a phase that never
// occurs, so no late code after them moves.
static void bfHeldBefore7(void);
static void bfHeldBoundary7(void);
#define BF_FILL_BEFORE "152"     // 176 B
#define BF_FILL_BOUNDARY "524" // 548 B
BF_R6 static void bfHeldBefore(void) {
    if (__builtin_expect(bench.phase != BF_SLOT_PHASE, 1)) {
        bfHeldBefore7();
        return;
    }
    BF_SLOT_FILL(BF_FILL_BEFORE);
}

BF_R6 static void bfHeldBoundary(void) {
    if (__builtin_expect(bench.phase != BF_SLOT_PHASE, 1)) {
        bfHeldBoundary7();
        return;
    }
    BF_SLOT_FILL(BF_FILL_BOUNDARY);
}

// ctr_benchheld.inc's logLevels wrap, at a scene's result: the window's last read completed, then the held lines.
BF_R6 static void bfHeldReport(const char* id) {
    bfHeldAfter();
    heldReport(id);
}
#else
// Without the held lines (DELTARUNE Old): the tour warm-up's restart alone (bfTourBefore, the end of this file).
static void bfTourBefore(void);
static inline void bfHeldBefore(void) {
    bfTourBefore();
}
static inline void bfHeldBoundary(void) {
}
#ifdef CTRPORT_BENCH_HELD
BF_R6 static void bfHeldReport(const char* id) {
    heldReport(id);
}
#endif
#endif

// The bench's frame work (the frame wrap's body until round 6), when a bench is active.
BF_R6 CtrBenchFrame bfBeginFrameBench(u32 down, double frameMs) {
    const Runner* runner = bench.session.runner;
    if (!bfStat.filled && bench.phase != PHASE_PICKER) bfStatFill();
    bfWindowLines(frameMs);
#if defined(CTRPORT_BENCH_HELD) && defined(CTRPORT_BENCHFIX_DRAWWAIT)
    if (tour.on && bench.phase == PHASE_WARMUP && runner != nullptr && runner->frameCount == bench.readyFrame + 1 &&
        nowNanos() - bench.phaseStart >= (uint64_t) (TOUR_WARMUP_SECONDS * 1e9)) {
        bench.phaseStart = nowNanos();
    }
#else
    // Without the held lines (DELTARUNE Old), bfHeldBefore's bfTourBefore restarts the warm-up at the frame after
    // ready, this test's case among them. The fill (a phase that never occurs) keeps this function's cand24 size,
    // 236 B, so no late code after it moves.
    (void) runner;
    if (__builtin_expect(bench.phase == BF_SLOT_PHASE, 0)) BF_SLOT_FILL(BF_FILL_FRAME);
#endif
    bfHeldBefore();
#ifdef CTRPORT_BENCHFIX_PROBE
    bfpBefore(runner);
    u64 t0 = svcGetSystemTick();
    CtrBenchFrame result = __real_benchfidBeginFrame(down, frameMs);
    bfpAfter(runner, t0);
#else
    CtrBenchFrame result = __real_benchfidBeginFrame(down, frameMs);
#endif
    bfHeldBoundary();
    bfHeldAfter();
    // The bench ended in this call: what the next calls (idle, the wrap's first test) would have done first.
    if (bench.phase == PHASE_IDLE) {
        bfWindowLines(frameMs);
#ifdef CTRPORT_BENCHFIX_PROBE
        bfpBefore(runner);
#endif
    }
    return result;
}

// The stat wrap's answer (its body until round 6), when a bench is active.
BF_R6 int bfStatAnswer(struct _reent* r, const char* path, struct stat* st) {
    if (bfStat.filled && bench.phase != PHASE_PICKER && strcmp(path, bfStat.path) == 0) {
        *st = bfStat.st;
        if (bfStat.result != 0) r->_errno = bfStat.error;
        return bfStat.result;
    }
    return __real__stat_r(r, path, st);
}

#ifdef CTRPORT_BENCHFIX_DRAWWAIT
// The fence wrap's timing (its body until round 6), when a bench is active.
BF_R6 void bfDrawWaitTimed(void) {
    uint32_t start = (uint32_t) svcGetSystemTick();
    __real_CtrGpu_waitDraws();
    bfDrawWait += (uint32_t) svcGetSystemTick() - start;
}
#endif
#undef BF_R6

// ===== benchfix round 7 (10-28, argbenchr6 M1-M4): .text.benchfix_r7_late and .bss.benchfix_r7_late
// (late_reserve.ld, last). The held lines' window boundary, as round 6's with four edge cases fixed (bench only):
// - M1: the window's first frame S had no ring entry (the read that opens the window sets no time baselines), so a
//   hold that S's own slowness caused, seen at read S+1, went to frame S-1 and left as a hold before the window. The
//   call that opens the window now sets the baselines the next read takes its times from: frame S is recorded.
// - M2: a room change in frame S: round 6 skipped the boundary when the call opened a new room visit, but the read
//   (before benchRoomsVisit) always adds to the snapshot's visit, so the boundary now runs whatever the call did after.
// - M3: a window that closes in the call after its first read reports inside that call: the logLevels wrap
//   (ctr_benchheld.inc) runs the boundary before the report.
// - M4: a same-room restart (a restart of the room, or a goto to the room it is in) opens no room visit, so heldAdd took the
//   visit's first frame for the room's start: the hidden load's hold went to frame first-1 with no times (the
//   class=hidden frame=S-1 lines of integ24-o3-utcv-2B and integ24-o3-utnx-2B), and the room's first 3 frames after
//   it counted as play. A room change seen as a call begins marks that frame as the room's start; holds listed after
//   it are classed as heldAdd classes a new visit's start (hidden or entry, the same rules and times).
// Rev 2 (argbenchr7, 10-28):
// - F1.1: the call that opens the window also records frame S-1's interval in the ring (a read in the window would
//   have), so the read at S+1 weighs S against S-1 as heldAdd does everywhere else; with S-1's slot empty, any S over
//   a slot took S-1's hold (a hold from before the window kept as play). And a frame that changed room is never shown
//   (main.c skips its present): a hold heldAdd gave it (not the hidden one) is the frame before's.
// - F1.2: a window's start clears the room start (DELTARUNE starts frameCount again each scene, so an earlier scene's
//   restart could match a later window's frames and class its play holds entry).
// - F1.4: holds past HELD_MAX are only counted; the boundary now classes them too (their frames are heldAdd's, from
//   the ring), so the counts the board judges get the same classes as the lines.
// Rev 3 (argbenchr7b, 10-28):
// - R7B-1.1, R7B-1.2: the window's flip baseline is read as frame S's call begins. A frame's flip normally comes
//   between its own call and the next one, so frame S-1's is already in the baseline; when no flip came since S-1's
//   call began, S-1's is still to come (its GPU work ran past S's slot start), or S-1 changed room outside the window
//   (no hiddenPending outside PHASE_MEASURE) and was never shown. Room changed between the two calls: the window's
//   first flip ends that hidden load (benchshownRead's skipNext, as a room change in the window sets it: hidden at
//   S-1, or entry at S when S took over 2 vblanks of Step + Draw). Same room: the first hold read after the window
//   opens, if that read is the first to see a flip, is S-1's and leaves as a hold before the window (with its
//   presentation counts), whatever the intervals say. Not seen: S-1's flip pending while S-2's also came after
//   S-1's call began (two late frames in a row before the window): S-1's hold is then classed as before.
// - R7B-1.3: the room start before the latest is kept too: a hold of a frame before the latest start (read up to two
//   calls later, as heldAdd reads them) is classed from the start it came after, and the hold that ends a hidden
//   load is classed from its own load's start when another room change came before it was read.
// - R7B-2.1: holds only counted (past HELD_MAX) of a frame before the window leave the counts and cut as the listed
//   ones leave. Their presentation counts stay (the read does not say which flip was theirs); a list full in the
//   window's first reads takes 25 holds of 3+ vblanks in one read, which 1 flip a vblank cannot give.
// - The tour's warm-up (argdtm F8): a tour room's warm-up starts again as the first frame presented after its
//   room change begins (the frame after ready, or a later room change in the warm-up's first BF_TOUR_REDIRECT
//   frames: a room that warps on), after the hidden load and the rooms.dtm write of the change frame. Before, it
//   started on the change frame itself, and from round 6 again on the frame after only when the change frame alone
//   had run out the warm-up (DELTARUNE Old, without the held lines, gets the same restart from bfTourBefore below).
#if defined(CTRPORT_BENCH_HELD) && defined(CTRPORT_BENCHFIX_DRAWWAIT)
#define BF_R7 __attribute__((noinline, noclone, no_reorder, optimize("Os"), section(".text.benchfix_r7_late")))
#define BF_HELD_SLOT 33.4f // ctr_benchheld.inc's HELD_SLOT
#define BF_TOUR_REDIRECT 90 // frames after ready in which a tour room's room change starts its warm-up again
static struct {
    bool hidden;        // the call's read ends a hidden load's hold (benchshownRead's skipNext as the call began)
    bool change;        // the frame before the call changed room (it was not shown)
    uint16_t counts[4]; // benchHeld.counts as the call began
    int32_t restartIn;  // the window (its first frame) the room start below is in; -1: none in this window
    uint32_t restart;   // the frame a room change last made a room's first
    bool pending;       // rev 3: the window's first hold is frame S-1's (no flip came between S-1's call and S's)
    int32_t restartPrevIn; // rev 3: the room start before restart, and its window
    uint32_t restartPrev;
    uint32_t openSum;   // rev 3: the flip baseline (vblanks) the window opened with
} bfHeld7 __attribute__((no_reorder, section(".bss.benchfix_r7_late")));
// Rev 3: the flips' vblank total and the room as each bench call began (any phase): this call's and the one before's.
static struct {
    uint32_t sum[2];
    int32_t room[2];
} bfHeldSeen __attribute__((no_reorder, section(".bss.benchfix_r7_late")));

BF_R7 static void bfHeldBefore7(void) {
    const Runner* runner = bench.session.runner;
    CtrFlipStats t;
    __real_CtrPacer_getFlipTotals(&t); // the pacer's totals, not the held read's wrap
    bfHeldSeen.sum[0] = bfHeldSeen.sum[1];
    bfHeldSeen.sum[1] = (uint32_t) t.sum;
    bfHeldSeen.room[0] = bfHeldSeen.room[1];
    bfHeldSeen.room[1] = runner != nullptr ? runner->currentRoomIndex : -1;
    if (tour.on && bench.phase == PHASE_WARMUP && runner != nullptr &&
        (runner->frameCount == bench.readyFrame + 1 ||
         (bfHeldSeen.room[1] != bfHeldSeen.room[0] && runner->frameCount > bench.readyFrame &&
          runner->frameCount - bench.readyFrame <= BF_TOUR_REDIRECT))) {
        bench.phaseStart = nowNanos();
    }
    bfHeldSnap.on = bench.phase == PHASE_MEASURE && benchHeld.open && benchRooms.count > 0 && !benchRooms.overflow;
    if (!bfHeldSnap.on) return;
    bfHeldSnap.listed = benchHeld.listed;
    bfHeldSnap.visit = benchRooms.count - 1;
    bfHeldSnap.start = bench.measureStartFrame;
    bfHeldSnap.v = benchfidShown.visits[bfHeldSnap.visit];
    bfHeld7.hidden = benchfidShown.skipNext;
    bfHeld7.change = benchRooms.hiddenPending;
    memcpy(bfHeld7.counts, benchHeld.counts, sizeof(bfHeld7.counts));
    // The frame before changed room (CtrBench_handleRoomChange): this frame is the room's first, same room or not.
    if (benchRooms.hiddenPending) {
        bfHeld7.restartPrevIn = bfHeld7.restartIn;
        bfHeld7.restartPrev = bfHeld7.restart;
        bfHeld7.restartIn = bench.measureStartFrame;
        bfHeld7.restart = (uint32_t) runner->frameCount;
    }
}

// A listed hold given another frame and class: that frame's ring times if the ring has it (as heldAdd copies them).
BF_R7 static void bfHeldMove(BenchHeld* r, uint32_t want, uint8_t cls) {
    benchHeld.counts[r->cls]--;
    benchHeld.counts[cls]++;
    uint8_t vb = r->vb, visit = r->visit;
    *r = benchHeld.ring[want & 3];
    if (r->frame != want) {
        memset(r, 0, sizeof(*r));
        r->frame = want;
    }
    r->vb = vb;
    r->cls = cls;
    r->visit = visit;
}

// A room start's class for the hold that ends its hidden load (heldAdd's rule): entry when the room's first frame
// (start) took over 2 vblanks of Step + Draw, else hidden.
BF_R7 static bool bfHeldSlowStart(uint32_t start) {
    const BenchHeld* e = &benchHeld.ring[start & 3];
    return e->frame == start && e->t[1] + e->t[2] > BF_HELD_SLOT;
}

BF_R7 static void bfHeldBoundary7(void) {
    Runner* runner = bench.session.runner;
    // M1: the call that opened the window (its read cleared benchHeld): heldRead's baselines, so that the next read
    // records frame S, and (F1.1) frame S-1's interval, as the read would have recorded it. F1.2: no room start yet.
    if (benchHeld.open && !benchHeld.started && bench.phase == PHASE_MEASURE && runner != nullptr &&
        runner->frameCount == bench.measureStartFrame) {
        memset(&bfHeld7, 0, sizeof(bfHeld7));
        bfHeld7.restartIn = bfHeld7.restartPrevIn = -1;
        if (benchRooms.active && !sweep.on) {
            benchHeld.started = true;
            benchHeld.wait = (uint32_t) CtrGpu_waitTotalTicks();
            benchHeld.snd = runner->audioSystem != nullptr ? (uint32_t) CtrAudioSystem_heldLoadTicks(runner->audioSystem) : 0;
            benchHeld.texMs = CtrRenderer_getStats(bench.session.renderer).textureLoadMs;
            BenchHeld* p = &benchHeld.ring[(uint32_t) (bench.measureStartFrame - 1) & 3];
            p->frame = (uint32_t) (bench.measureStartFrame - 1);
            p->t[0] = (float) runner->deltaTime * 0.001f;
            // Rev 3 (R7B-1.1, R7B-1.2): no flip since frame S-1's call began: S-1's flip is still to come, or S-1
            // changed room and was never shown.
            if (benchfidShown.lastSum == bfHeldSeen.sum[0]) {
                if (bfHeldSeen.room[1] != bfHeldSeen.room[0]) benchfidShown.skipNext = true;
                else bfHeld7.pending = true;
            }
            bfHeld7.openSum = benchfidShown.lastSum;
        }
    }
    if (!bfHeldSnap.on) return;
    bfHeldSnap.on = false;
    // Rev 3: the pending S-1 hold is the first read's that sees a flip, or none.
    bool pend = bfHeld7.pending;
    if (pend && benchfidShown.lastSum != bfHeld7.openSum) bfHeld7.pending = false;
    // The same window (M2: whichever room visit the call ends in), and a call that added holds (F1.4: listed or not).
    if (!benchHeld.open || bench.phase != PHASE_MEASURE || bench.measureStartFrame != bfHeldSnap.start) return;
    uint16_t more[4]; // the call's holds by class, then less the listed ones: those past HELD_MAX
    uint32_t added = 0;
    for (uint32_t c = 0; c < 4; c++) {
        if (benchHeld.counts[c] < bfHeld7.counts[c]) return;
        more[c] = (uint16_t) (benchHeld.counts[c] - bfHeld7.counts[c]);
        added += more[c];
    }
    if (added == 0) return;
    for (uint32_t i = bfHeldSnap.listed; i < benchHeld.listed; i++)
        if (more[benchHeld.list[i].cls] > 0) more[benchHeld.list[i].cls]--;
    uint32_t now = (uint32_t) runner->frameCount, visit = bfHeldSnap.visit;
    uint32_t first = (uint32_t) benchRooms.visits[visit].firstFrame;
    // heldAdd's frame for the call's holds that are not the hidden one (the same for each: from the ring).
    const BenchHeld* f2 = &benchHeld.ring[(now - 2) & 3];
    const BenchHeld* f1 = &benchHeld.ring[(now - 1) & 3];
    uint32_t want = now - 2;
    if (f1->frame == now - 1 && f1->t[0] > BF_HELD_SLOT + 0.5f && (f2->frame != now - 2 || f1->t[0] > f2->t[0])) want = now - 1;
    // F1.1: the frame before changed room, so was never shown: the flip read now is the frame before it's.
    uint32_t to = bfHeld7.change && want == now - 1 ? now - 2 : want;
    // M4: a room start after the visit's first frame (a same-room restart): heldAdd's classes from that start. R7B-1.3:
    // the start the frame came after (a frame before the latest start: the one before); the hidden load's hold: its
    // own load's start (the one before when this call saw another room change).
    uint32_t start = bfHeld7.restart, hstart = bfHeld7.change ? bfHeld7.restartPrev : bfHeld7.restart;
    int32_t in = bfHeld7.restartIn, hin = bfHeld7.change ? bfHeld7.restartPrevIn : bfHeld7.restartIn;
    if (to < start) {
        start = bfHeld7.restartPrev;
        in = bfHeld7.restartPrevIn;
    }
    bool m4 = in == bfHeldSnap.start && start > first, hm4 = hin == bfHeldSnap.start && hstart > first;
    // The class heldAdd would give a hold of frame `to` that it classed c at frame want (black kept, unless entry).
    uint8_t cls[4];
    for (uint32_t c = 0; c < 4; c++) {
        uint8_t k = (uint8_t) c;
        if (c != 2 && to != want) k = visit > 0 && to >= first && first + 3 > to ? 1 : c == 1 ? 0 : (uint8_t) c;
        if (m4 && k != 1 && k != 2 && to >= start && start + 3 > to) k = 1;
        cls[c] = k;
    }
    bool hidden = bfHeld7.hidden;
    for (uint32_t i = bfHeldSnap.listed; i < benchHeld.listed; i++) {
        BenchHeld* r = &benchHeld.list[i];
        if (hidden) {
            hidden = false;
            if (hm4) {
                bool slow = bfHeldSlowStart(hstart);
                bfHeldMove(r, slow ? hstart : hstart - 1, slow ? 1 : 2);
            }
        } else if (pend) {
            // R7B-1.1: frame S-1's (it leaves below, a play or black hold before the window).
            pend = false;
            bfHeld7.pending = false;
            bfHeldMove(r, (uint32_t) bfHeldSnap.start - 1, r->cls);
        } else if (to != want) {
            bfHeldMove(r, to, cls[r->cls]);
        } else if (cls[r->cls] != r->cls) {
            benchHeld.counts[r->cls]--;
            benchHeld.counts[cls[r->cls]]++;
            r->cls = cls[r->cls];
        }
    }
    // F1.4: the holds only counted (the list was full), the same way.
    if (hidden) {
        uint8_t was = bfHeldSlowStart(first) ? 1 : 2, now7 = hm4 ? (bfHeldSlowStart(hstart) ? 1 : 2) : was;
        if (more[was] > 0) {
            more[was]--;
            benchHeld.counts[was]--;
            benchHeld.counts[now7]++;
        }
    }
    // R7B-2.1: of a frame before the window, they leave (the counts and cut).
    bool before = (int32_t) to < bfHeldSnap.start;
    for (uint32_t c = 0; c < 4; c++) {
        if (c == 2 || (cls[c] == c && !before)) continue;
        benchHeld.counts[c] -= more[c];
        if (before) benchHeld.cut -= more[c];
        else benchHeld.counts[cls[c]] += more[c];
    }
    if (benchHeld.listed <= bfHeldSnap.listed) return;
    BenchShownVisit* s = &benchfidShown.visits[bfHeldSnap.visit];
    const uint32_t L = CTR_FLIP_BUCKETS - 1;
    uint32_t kept = bfHeldSnap.listed;
    bool dropped = false;
    for (uint32_t i = bfHeldSnap.listed; i < benchHeld.listed; i++) {
        BenchHeld r = benchHeld.list[i];
        if ((int32_t) r.frame >= bfHeldSnap.start || r.cls == 2) {
            benchHeld.list[kept++] = r;
            continue;
        }
        benchHeld.counts[r.cls]--;
        uint32_t b = r.vb > L ? L : r.vb - 1u;
        if (s->count[b] > bfHeldSnap.v.count[b]) {
            s->count[b]--;
            if (b == L) s->longSum = s->longSum > r.vb ? s->longSum - r.vb : 0;
        }
        dropped = true;
    }
    benchHeld.listed = (uint16_t) kept;
    if (!dropped) return;
    // The visit's longest: as before this call, or a flip of this call still counted (an 8+ one keeps the read's).
    uint32_t top = 0;
    for (uint32_t b = 0; b < CTR_FLIP_BUCKETS; b++)
        if (s->count[b] > bfHeldSnap.v.count[b]) top = b + 1;
    if (top == CTR_FLIP_BUCKETS) return;
    s->maxVb = bfHeldSnap.v.maxVb > top ? bfHeldSnap.v.maxVb : top;
}
#undef BF_R7
#undef BF_HELD_SLOT
#else
// The tour's warm-up without the held lines (DELTARUNE Old; 10-28, droldcensus's tour benches): as bfHeldBefore7
// does with them, a tour room's warm-up starts again as the first frame presented after its room change begins (the
// frame after ready, or a later room change in the warm-up's first BF_TOUR_REDIRECT frames), after the change frame's
// hidden load and rooms.dtm write. The room each call began in is kept in any phase, as bfHeldSeen keeps it.
#define BF_R7 __attribute__((noinline, noclone, no_reorder, optimize("Os"), section(".text.benchfix_r7_late")))
#define BF_TOUR_REDIRECT 90 // frames after ready in which a tour room's room change starts its warm-up again
static int32_t bfTourRoom __attribute__((no_reorder, section(".bss.benchfix_r7_late")));
BF_R7 static void bfTourBefore(void) {
    const Runner* runner = bench.session.runner;
    int32_t was = bfTourRoom;
    bfTourRoom = runner != nullptr ? runner->currentRoomIndex : -1;
    if (tour.on && bench.phase == PHASE_WARMUP && runner != nullptr &&
        (runner->frameCount == bench.readyFrame + 1 ||
         (bfTourRoom != was && runner->frameCount > bench.readyFrame &&
          runner->frameCount - bench.readyFrame <= BF_TOUR_REDIRECT))) {
        bench.phaseStart = nowNanos();
    }
}
#undef BF_R7
#endif

#ifdef CTRPORT_FRAMETRACE
// frametrace (z_frametrace.c; build.sh ... frametrace, test builds only): the bench's state as a frame's Step begins.
__attribute__((section(".text.frametrace_late"))) const CtrBenchSession* CtrBench_ftState(bool* measuring, int32_t* start,
                                                                                   const char** id) {
    const SceneResult* r = currentResult();
    *measuring = bench.phase == PHASE_MEASURE && r != nullptr;
    *start = bench.measureStartFrame;
    *id = r != nullptr ? r->id : nullptr;
    return &bench.session;
}
#endif
