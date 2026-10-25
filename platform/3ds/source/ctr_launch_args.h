// Launch arguments for remote test runs.
//
// The Homebrew Launcher's network loader (3dslink, tools/remote/run3ds.sh) passes the arguments given after the
// .3dsx on the PC; argv[0] is the program's path (with 3dslink 0.6, "sdmc:/3ds/<name>.3dsx"). Started from the
// menu, a .3dsx gets no arguments and nothing here changes.
//
//   --bench <scene[,scene...]|all>  run the performance test with these scenes once the game is up (scene ids
//                                   from bench/<game>.txt, as benchrun.txt takes them); the results screen goes
//                                   away by itself; "tour", "tour=C:I[-C:I]" or "tour=resume" visits every
//                                   room instead (ctr_benchmark.h); "sweep" or "sweep=<battle[,battle...]>"
//                                   plays battles (the list keeps its commas)
//   --shots                         with --bench: a screenshot halfway through each measurement (bench shots,
//                                   these and a scene's "shot", end in the scene id: top_00941_ch3_rhythm.bmp)
//   --exit-after                    return to the Homebrew Launcher when the test is done (or, without --bench,
//                                   when the game would otherwise wait for a button: the error screen, or once
//                                   --flicker-probe has reported)
//   --timeout <seconds>             return to the Homebrew Launcher after this long whatever happens
//   --ini <key=value>               a settings.ini setting for this run only (repeatable; never saved: the options
//                                   menu saves only what it changes, over the file's own settings)
//   --lab                           lab mode (ctr_lab.h): a crash or hang is caught, reported and left cleanly, and
//                                   whatever ends the run goes on to the lab host (sdmc:/3ds/ctrport-lab.3dsx), which
//                                   waits for the next .3dsx
//   --lab-host <path>               lab mode with the lab host at this SD card path. The lab host passes this to every
//                                   .3dsx it starts (platform/3ds/labhost/labhost.c); nothing else needs to.
//   --lab-hang <seconds>            lab mode: leave when the game loop has been stuck this long (default 90; 0: never)
//   --lab-crash <kind>              lab mode test: crash on purpose at startup (data, prefetch, undef, thread, break,
//                                   abort, or hang: spin until --lab-hang ends it)
//   --profile                       the GML profiler on, as an empty profile.txt in the game folder does (profile
//                                   builds only)
//   --profile=step                  the same, timing only the Step half of each frame (as a profile.txt holding
//                                   "step" does): Draw events, draw builtins and the runner's drawing are left out
//   --knightdiag                    profile each Draw separately, logging selected frames (profile builds only);
//                                   enables the profiler without an SD card flag file
//   --gputrace <frame[,frame...]>   trace the GPU on these frames, as gputrace.txt listing them does (repeatable;
//                                   the file's frames count too)
//   --selftest <name>               run a self-test once the game is up, log its results and leave (ctr_renderer.h):
//                                  Debug builds also accept oom: fail one 128 MB stb array growth before frame 1.
//                                   "smart" (one-pixel patterns through each smoothing path), "fcram_target" (render
//                                   targets in linear memory against VRAM ones) or "mirror_cancel" (a dropped
//                                   mirror's queued fill never lands)
//   --flicker-probe <words>         count the application surface's text box per frame, as flickerprobe.txt holding
//                                   the words does (words separated by commas: base, nooverlap, slow, nomulti,
//                                   flush, all, unitsoff, eyes (Omega Flowey's eye instead),
//                                   frames=<first>-<last>;
//                                   ctr_renderer.h); the file wins
//   --fixed-seed[=<seed>]           start the random generator at this seed (default 0) and keep randomize() from
//                                   reseeding it, as fixedseed.txt in the game folder (empty, or holding the seed)
//                                   does, so random choices (a battle's attack order) repeat from run to run; the
//                                   game clock and measurements stay real, but bench video presentation follows
//                                   game frames for repeatability. Only the --fixed-seed=<seed> form takes a
//                                   seed.
//   --force-materialize             debug: every surface the game makes starts recorded, as when VRAM is full, and is
//                                   drawn from its quads into a texture at its first draw (materializeRecorded), as
//                                   forcematerialize.txt in the game folder does
//   --drawing-full-at <frame>       test: set the Drawing setting to Full at this frame (of each chapter load), as the
//                                   menu would, as drawingfullat.txt in the game folder holding the frame does
//   --slider-at <up>[,<down>]       test: the 3D slider reads as down before frame up, up (1.0) from it, and down again
//                                   from frame down if given, of each chapter load, as sliderat.txt in the game folder
//                                   holding the frames does; a 3D mode must be on for it to show
//   --no-surface-readers            test: forget which objects read surfaces back (VM_findSurfaceReaders), so their
//                                   surfaces can be native-scale too, as before that scan (DELTARUNE Chapter 4's titan
//                                   climb then places its surfaces as b7059c6 did), as nosurfacereaders.txt in the
//                                   game folder does
//   --autoshot <frame[,frame...]>   screenshot these frames (the top and bottom screens and the application surface:
//                                   top_<frame>.bmp, bottom_<frame>.bmp, shot_<frame>.bmp in the game folder), as
//                                   autoshot.txt listing them does (repeatable; the file's frames count too). Sends
//                                   the run's files to the PC (--send-files).
//   --statehash                     hash every frame's game state and draw calls into state_hash.log, on the virtual
//                                   clock, as statehash.txt in the game folder does. Sends the run's files to the PC
//                                   (--send-files).
//   --send-files[=<MB>]             when the run ends, send the files it wrote (the screenshots above, state_hash.log,
//                                   --shots and battle sweep screenshots) to the PC over the log connection, after the
//                                   last log line (ctr_link_files.h; tools/remote/linkserver.py saves them). At most
//                                   <MB> MB in all (default 16, up to 64); files past that stay on the SD card and the
//                                   log says which. Only the --send-files=<MB> form takes a size. Needs 3dslink.
//
// Every option also takes the --name=value form. Unknown options are reported and skipped.
// --video-suspend F:N fakes N seconds of suspension once at game frame F
// (1-100000000 frames, 1-120 seconds), for video clock tests without HOME.
//
// No 3DS headers: tools/remote/test_launch_args.sh builds this file for the PC with its unit tests.
#ifndef CTRPORT_CTR_LAUNCH_ARGS_H
#define CTRPORT_CTR_LAUNCH_ARGS_H

#include <stdbool.h>
#include <stddef.h>

#define LAUNCH_ARGS_BENCH_MAX 256
#define LAUNCH_ARGS_INI_MAX 16
#define LAUNCH_ARGS_INI_LENGTH 64
#define LAUNCH_ARGS_MESSAGES_MAX 512
#define LAUNCH_ARGS_GPU_TRACE_MAX 16
#define LAUNCH_ARGS_AUTOSHOT_MAX 64
#define LAUNCH_ARGS_SEND_FILES_DEFAULT_MB 16
#define LAUNCH_ARGS_SEND_FILES_MAX_MB 64

typedef struct {
    char bench[LAUNCH_ARGS_BENCH_MAX]; // scene ids separated by spaces, or "" for no test
    bool shots;
    bool exitAfter;
    int timeoutSeconds;                // 0: none
    char ini[LAUNCH_ARGS_INI_MAX][LAUNCH_ARGS_INI_LENGTH];
    int iniCount;
    bool lab;                          // --lab, --lab-host
    char labHost[128];                 // "" for the default; else "/3ds/x.3dsx"
    int labHangSeconds;
    bool labHangSet;
    char labCrash[16];                 // "" or the --lab-crash kind
    bool profile;                      // --profile
    bool profileStepOnly;              // --profile=step
    bool knightDiagnostic;             // --knightdiag
    bool fixedSeed;                    // --fixed-seed
    unsigned fixedSeedValue;           // its seed, 0 by default
    int gpuTraceFrames[LAUNCH_ARGS_GPU_TRACE_MAX]; // --gputrace
    int gpuTraceCount;
    char selftest[24];                 // --selftest name, or ""
    bool forceMaterialize;             // --force-materialize
    bool noSurfaceReaders;             // --no-surface-readers
    int drawingFullAt;                 // --drawing-full-at, 0: off
    int sliderUpAt, sliderDownAt;      // --slider-at up[,down], 0: off (down 0: stays up)
    int autoshotFrames[LAUNCH_ARGS_AUTOSHOT_MAX]; // --autoshot
    int autoshotCount;
    bool stateHash;                    // --statehash
    bool sendFiles;                    // --send-files (--autoshot and --statehash imply it: CtrLaunchArgs_sendFilesMB)
    int sendFilesMB;                   // its cap, 0: the default
    char flickerProbe[64];             // --flicker-probe's words, "" for none
    unsigned videoSuspendFrame, videoSuspendSeconds;
    int problems;                     // options that were wrong or unknown, described in messages
    char messages[LAUNCH_ARGS_MESSAGES_MAX];
} CtrLaunchArgs;

// Fills args from main's argc/argv (argv[0], the program, is skipped). Returns true if any option was given.
bool CtrLaunchArgs_parse(CtrLaunchArgs* args, int argc, char** argv);

// The arguments again, for the log ("--bench ruins --exit-after").
void CtrLaunchArgs_describe(const CtrLaunchArgs* args, char* out, size_t size);

// How many MB of the run's files go to the PC at the end: 0 unless --send-files, --autoshot or --statehash.
int CtrLaunchArgs_sendFilesMB(const CtrLaunchArgs* args);

#endif
