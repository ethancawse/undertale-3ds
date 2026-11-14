#!/usr/bin/env bash
# Build the 3DS frontend: the .3dsx, and the .cia when makerom and bannertool are installed.
#
# usage: platform/3ds/build.sh [undertale|deltarune] [n3ds|o3ds] [Release|RelWithDebInfo|Debug|MinSizeRel] [profile] [heap]
#                              [aot|aot=chapters|aot=single|aot=off|aot=<dir>] [data=<game folder>] [aot-stress] [memcap=<KB>]
#                              [openh264=off] [reachgate] [shots] [roomplan=off] [roomplan-rec] [smalltext|smalltext=off]
#                              [cia80] (DR Old installed CIA only, +8 MiB linear; separate directory)
#                              [slotdiag] (per-room dynamic-slot/heap counters, separate build directory)
#                              [recordfail=<n>] (force surface-record growth failure after n ops; diagnostic build directory)
#                              [gameend] (a test may end in game_end and the game closes, which the exit path's
#                              check needs; separate build directory, never a release)
#                              [statedump=<frame>] (state hash: dump every variable's hash at that frame, separate build directory)
#                              [audiocap] (records the DSP mix and channel events to the SD card, separate build directory)
#                              [pcmhash] (logs a hash of every stream's PCM, to compare builds; separate build directory)
#                              [loadaudio-control] (UT pcmhash only: matched observer, spare first-buffer preparation disabled)
#                              [audiometrics] (decoder allocation bytes and ticks per frame; Release diagnostic directory)
#                              [adpcmprobe] (UT native DSP block/reference check before audio startup; diagnostic only)
#                              [adpcmsfx] (compressed SFX cache/PCM feeder prototype; separate directory)
#                              [adpcmmusic] (native music adapter prototype; separate directory)
#                              [adpcmtestmtime] (TEST ONLY, requires adpcmmusic; emulator unknown-mtime gate)
#                              [adpcmpack|adpcmpack=off] (UT mono sound effects as DSP-ADPCM from the builder's
#                              adpcm-sfx.dtsp; default in UNDERTALE Old builds)
#                              [adpcmfault=tail|seam|pool|ring] (TEST ONLY, needs the pack: that linear allocation
#                              of the sound feeder or the pack fails; separate directory)
#                              [audio-convert] (private DSP-ADPCM objects and SD indexes; runtime adapter selection pending)
#                              [surfcensus] (logs how every game surface is made, bound and drawn out; separate build directory)
#                              [vramcensus] (per-bank VRAM free and largest gap at the bench's own samples, diag/
#                              ctr_vram_census.c; separate build directory, never a release)
#                              [offdraw] (change offdraw: instance census and offdraw.txt kill/hash-skip experiments; separate build directory)
#                              [halfcensus] (logs the largest scale each texture item is drawn at in one room; test only,
#                              separate build directory)
#                              [pklindiag] (UNDERTALE o3ds: logs a linear free-block census and the linear copies' accounting at
#                              each room load; test only, separate build directory)
#                              [pklinstress] (UNDERTALE o3ds: promotion reads into linear memory wait 250 ms each piece and
#                              every room load stops them; test only, separate build directory)
#                              [halfart|halfart=box|point|balanced[N]] (UNDERTALE o3ds only: the art tools/analysis/halfart/
#                              undertale.txt lists (Omega Flowey) stored at half resolution, box-filtered ("halfart" = box,
#                              the fallback if the full set can't fit); point: no filter (test); balanced[N]: box, only the
#                              frames undertale-balanced[N].txt lists (comparison); separate build directory, own SD cache folder)
#                              [natsurf6wide] (native-scale surfaces with lossy draw classes too; candidate, separate build directory)
#                              [knightnew-history|knightnew-history=off] (Knight GPU-1: trims the Knight afterimage spill
#                              copies' unused columns; ON by default in DELTARUNE New, a no-op in other builds; "=off" builds
#                              without it in its own build directory)
#                              [gpukick=off|gpukick|gpukick=multi|gpukick=multi-force] (DELTARUNE New, change pacer: hand
#                              the GPU the commands recorded so far while the frame still records. Default multi with
#                              the rev3 gate: only after 3 frames in a row whose main + GPU passed the slot while each
#                              fit it; off = the control without it; gpukick (single prefix) is gone since KM rev 5; multi-force =
#                              every frame, Azahar identity diagnostic only; a no-op in other builds)
#                              [gpukick-trace] (DELTARUNE New, KM rev 5: the per-frame KT trace between bench windows,
#                              for console row builds; off by default and in release)
#                              [knightgpu-texenv=off] (knightgpu T1, cheaper blend and fog switches: ON in DELTARUNE
#                              New, a no-op elsewhere; "=off" builds O9 alone in its own build directory, the forced-off
#                              control)
#                              [knightgpu-compact=off|force] (knightgpu A, VRAM compaction for hot spilled surfaces: ON in
#                              DELTARUNE New, a no-op elsewhere; "=off" builds without it and "=force" the forced control
#                              (moves every few frames; tests only), each in its own build directory)
#                              [knightgpu-dspe=on|scalar|off] (knightgpu dspe, typed regions' draw_surface_part_ext with
#                              real arguments and a draw session: ON in DELTARUNE New, OFF elsewhere ("=on" and "=scalar"
#                              are for DELTARUNE New only; "=scalar" without the session); "=off" and "=scalar" share the
#                              ON build's directories)
#                              [layout=<audited absolute linker-script path>]
#                              [inputperiod=<0|8|16>] [inputstall=<ms>] (input sampler diagnostics; separate build directory)
#                              [spikerec] (spike recorder for playtests: every frame that misses its slot, written to
#                              spikes.txt at room changes; source/z_spikerec.c; separate build directory, never a release)
#                              [homeaudioprobe|homeaudioauto|homeaudiofake] (UNDERTALE, when the sound and the picture stop on HOME: real
#                              presses, or the emulator's own timed jumps; diag/ctr_homeaudio_probe.c; never a release)
#                              [freewins] (main-loop segment timers and off switches, change freewins; source/z_freewins.c;
#                              separate build directory, never a release)
#                              [bfprobe] (main-thread waits in a sweep, [bfprobe] log lines; ctr_benchmark.c's end;
#                              separate build directory, never a release)
#                              [dimlightdiag] (UT sandboxed Options/FPS/late faults and optical markers;
#                              separate build directory, never a release)
#                              [arenalog] (UNDERTALE o3ds: the audio arena with peak, fallback and heap-lock census lines;
#                              separate build directory, never a release)
#                              [asgprobe] (UNDERTALE test only: ARM11 counters and per-frame drawSprite ticks, source/ctr_asgprobe.c;
#                              asgframes.txt / asgtime.txt / asgdump.txt in the game folder; separate build directory, never a release)
#                              [frametrace] (every frame of a bench window: phases, pacer, GPU waits, SD calls, decodes,
#                              tile loads, written to frametrace.txt after the window; source/z_frametrace.c;
#                              separate build directory, never a release)
#                              [flowdiag] (UNDERTALE: every draw-time tile load and floor guard decision with free
#                              linear and heap memory and plan membership; source/z_flowdiag.c; change flowstream round 3;
#                              separate build directory, never a release)
#                              [titandiag] (per-frame Step/Draw/other split and the costliest events of slow bench frames,
#                              in log.txt as [td] lines; source/z_titandiag.c; separate build directory, never a release)
#                              [exittrail] (the exit's steps after the last log line, each in labboot.txt with its thread;
#                              source/z_exittrail.c; o2DS kernel panic diagnosis, separate build directory, never a release)
#                              [ballast] (UNDERTALE: --ini ballast_* keys hold the second-screen features' heap, linear,
#                              CPU and GPU budget; source/z_ballast.c, dsplan B10.2; separate build directory, test builds
#                              only, never a release)
#                              [nowriterfront|writerfront-fallback|writerfront-count] (UNDERTALE Old: the dialogue writer's
#                              hand front off, every Draw through its fallback, or front/fallback counts in the log;
#                              source/z_writerfront.c; separate build directories, test builds only)
#                              [towerprof] (tower draws' CPU per frame as [tprof] lines; source/vine_new_retain.inc;
#                              test builds only, change flowerhw)
#                              [towerforce1|towerforce2] (tower strips without coverage, and with every cut 7d draw
#                              refused as well; same pictures; test builds only, change flowerhw)
#                              [nol2] [ppforce] (DELTARUNE New: flowerhw L2 off (the control), and the phase plan's
#                              eviction forced, with L2's counts as "Phase plan L2:" lines; source/z_phaseplan.c;
#                              separate build directories, test builds only, change flowerhw)
#                              [typed|typed=off|typed=all|typed=<list>] [typed-stats] [typed-stress=<n>]
#                              [late-numeric=<entry>|late-numeric=off] (UNDERTALE lava copy on by default)
#
# Arguments may come in any order; the defaults are undertale, n3ds and Release. Output goes to
# build/3ds-<game>-<target>/: <game>-3ds-<target>.3dsx and <game>-3ds-<target>.cia.
# "reachgate" compiles in the reach-mode identity gate, configured by the words in the game
# folder's statehash.txt, and builds into build/3ds-<game>-<target>-reachgate/; never a Release build.
# "shots" keeps the player's L+SELECT screenshot combo for debug and development (release builds have it off),
# and builds into build/3ds-<game>-<target>-shots/. Test screenshots (--autoshot, bench shots) work in
# every build.
# "rosaprobe" logs each pause and backlight switch of the Rosalina watcher (source/ctr_rosalina.c), for hardware
# test kits, and builds into build/3ds-<game>-<target>-rosaprobe/.
# "rosalow=arm" and "rosalow=script" (UNDERTALE Old, tests only; source/z_rosalow.c): the combo lowering's arm byte, which
# tools/analysis/rosalow_arm.py sets in a copy of the .3dsx, and its Azahar script. They build into
# build/3ds-<game>-<target>-rosalow<word>/.
# "utfadeprobe" (UNDERTALE, tests only) writes the game screen's picture during each room change's load to the game
# folder (source/z_utfade.c), and builds into build/3ds-<game>-<target>-utfadeprobe/; never a Release build.
# "bsplace" (UNDERTALE, tests only): L switches the split battle's preset A between the box centred with the whole
# bottom (b) and the box centred with the HP line and buttons left at the bottom (c) (source/z_battlesplit.c), and
# builds into build/3ds-<game>-<target>-bsplace/; never a Release build.
# "labcrash" (UNDERTALE, tests only; 11-11, devdiet): labguard's own fault injection, --labguard-crash=<kind> and
# --lab-crash stall-<step> (source/z_labguard.c's LG_CRASHTEST), for labguard's console tests; the plain dev build has
# neither. Builds into build/3ds-<game>-<target>-labcrash/; never a Release build (no labguard there).
# "bubblecheck" (UNDERTALE, with smalltext) logs every Crisp line, any font, that comes closer to its bubble's or the
# battle box's right edge than PC's ink allows, leaves the screen where PC's stays on it, or comes closer to the line
# below it than PC's (source/z_smalltext.c; tools/analysis/bubblecheck_report.py makes the table), for tours and
# benches, and builds into build/3ds-<game>-<target>-bubblecheck/; never a Release build.
# "touchtest" (UNDERTALE, tests only; source/z_battletouch.c): touchscript.txt in the game folder drives the stylus in
# battles ("<frame> pen <x> <y>" / "<frame> up", frames from the battle's first), even during a test, and each stylus
# frame goes to the log as a [touch] line; builds into build/3ds-<game>-<target>-touchtest/; never a Release build.
# "touchqueue" enables touchtest with packaged RomFS scene/tape reads, for self-contained shared-queue rows.
# "nametrace" opts naming campaigns into per-frame naming records; general touchtest/queue builds omit them.
# The room plan (tools/roomplan/build_plan.py): each room's first tiles, from the committed records resolved against the
# game data, go into the RomFS (romfs:/roomplan/undertale.dtp; DELTARUNE: <chapter folder>.dtp for each chapter with a
# record). Without game data or a record for it the build goes on without one. "roomplan=off" builds without it into build/3ds-<game>-<target>-noplan/.
# "roomplan-rec" compiles in the room plan recorder (n3ds only; roomplan-rec.txt in the game folder) and builds into
# build/3ds-<game>-<target>-rprec/; never a release build.
# "profile" compiles in the GML script profiler and builds into build/3ds-<game>-<target>-profile/;
# put an empty profile.txt in the game folder to turn it on. It also logs the renderer's own CPU time
# ([render] lines in log.txt).
# "heap" compiles in the heap profiler (source/ctr_heap_profile.c) and builds into build/3ds-<game>-<target>-heap/;
# the log then lists the call stacks holding the most heap at each [mem] line and every 30 s.
# "memcap=<KB>" splits only that much memory between the heap and linear memory, the way a console that grants that
# much would, for testing in an emulator that grants more (Azahar gives an Old 3DS .3dsx 89 MB). It caps what is left
# after the load image: on an Old 3DS that is 65528 KB less the image (tools/analysis/console_memcap.py prints it for a
# build); Azahar's New 3DS grant already matches the console's. It builds into build/3ds-<game>-<target>[-heap]-cap<KB>/.
# "aot" compiles the game's hottest GML code to C from the committed lists in tools/aot/selection/ and
# your own game files, 4 MB of code for o3ds and 12 MB for n3ds, and the build goes into the usual folder. It needs
# Python 3 and the game folder: data=<folder>, $CTRPORT_UNDERTALE_DATA / $CTRPORT_DELTARUNE_DATA, or the game's
# folder in a Steam library. Missing game data warns and builds without AOT. Missing Python is a fatal error.
# CTRPORT_PYTHON can name the builder's interpreter explicitly, surviving the login shell's PATH reset.
# "aot=off" builds without it, quietly. "aot" is the default for both games on both targets.
# UNDERTALE still needs recognized player data with aot=off: its battle-grid geometry is derived privately at build time.
# For DELTARUNE "aot" means "aot=chapters": each chapter gets the whole budget. tools/aot/prepare.py --chapters
# generates one set per chapter into build/aot-deltarune-<target>-chapters/ from tools/aot/selection/deltarune-parts.txt,
# each is linked into a module of its own in the romfs, and a chapter's module is loaded when the chapter starts.
# For UNDERTALE it means "aot=single": prepare.py generates one set into build/aot-<game>-<target>/ from
# tools/aot/selection/<game>.txt, linked into the program. "aot=single" also builds DELTARUNE that way (one budget for
# the whole game), into build/3ds-deltarune-<target>-single/.
# "aot=<dir>" compiles in code tools/aot/gmlaot.py generated into <dir> and builds into build/3ds-<game>-<target>-aot/.
# "aot-stress" also makes that code hand over to the interpreter every few blocks, a test of the fallback, and builds
# into build/3ds-<game>-<target>-aot-stress/.
# Typed regions (tools/aot/typed.py) are on by default wherever "aot" generates the code. For
# DELTARUNE ("typed=all") typed.py makes them in every code entry the AOT compiles, wherever its rules accept a region,
# from the player's data. For UNDERTALE the loops of the code entries listed in tools/aot/selection/typed-undertale.txt
# run as typed C code, or those of typed-undertale-<target>.txt where that file exists (UNDERTALE Old: the credits
# bullets' Draw, creddodge lever 2). "typed=<list>" takes any other list. "typed=off" builds without them, generating into
# build/aot-<game>-<target>[-chapters]-tyoff/ and building into build/3ds-<game>-<target>[-single]-tyoff/. With
# "aot=<dir>" they are off unless "typed" is given (the directory then needs prepare.py --typed's aot_typed.c), and that
# build goes into -aot-ty/. "typed-stats" also counts each region's entries, guard failures, hand-overs and exits for
# the log (-tystats); "typed-stress=<n>" fails one region entry in n and hands over at one block in n (-tystress<n>).
#
# The software video decoder, openh264, is built first by tools/deps/openh264/build.sh into build/deps/openh264/: the
# first time it downloads the pinned release (cached in build/deps/cache/), afterwards it does nothing. If that fails
# (no network the first time, say) the build goes on without it and warns: the videos then have no pictures where
# there's no hardware decoder. "openh264=off" builds without it, quietly. UNDERTALE, which has no videos, never has it.
#
# Needs devkitPro with the 3ds-dev group, plus makerom and bannertool for the .cia.
# 3ds-libvorbisidec and 3ds-libogg are optional: music is decoded with Tremor when they are installed and
# with the slower stb_vorbis otherwise.
# On Windows, run it from the devkitPro MSYS2 shell, or from any shell as:
#   C:/devkitPro/msys2/usr/bin/bash.exe --login -c "cd <repo> && platform/3ds/build.sh"
set -euo pipefail

flowcode=OFF
flowcode_plan=
game=undertale
target=n3ds
config=Release
profile=OFF
heap=OFF
slotdiag=OFF
gameend=OFF
statedump=
audiocap=OFF
pcmhash=OFF
loadaudio_control=OFF # loadaudio-control requires UT pcmhash; matched observer without spare first-buffer preparation
audiometrics=OFF
adpcmprobe=OFF
adpcmsfx=OFF
adpcmmusic=OFF
adpcmtestmtime=OFF
adpcmpack=     # ON, OFF, or empty: the default (ON in UNDERTALE Old, OFF elsewhere)
adpcmfault=0   # TEST ONLY: 1 tail, 2 seam, 3 pool, 4 ring (the allocation that fails)
audio_convert=OFF
surfcensus=OFF
vramcensus=OFF
offdraw=OFF
halfcensus=OFF
pklindiag=OFF
pklinstress=OFF
halfart=        # point, box or balanced: the half-resolution art prototype (change flowhalf)
natsurf6wide=OFF
cia80=OFF
knightnew_history=   # ON, OFF, or empty: the default (ON in DELTARUNE New, no effect elsewhere)
gpu_kick=ON   # gpukick words: takes effect in DELTARUNE New only; gpu_kick_mode single, multi or multi-force
gpu_kick_mode=multi
gpu_kick_trace=OFF   # gpukick-trace: KM rev 5's per-frame trace (row builds)
knightgpu_texenv=ON
knightgpu_compact=ON
knightgpu_compact_force=OFF
sharebuf=ON        # sharebuf=off: no shared surface buffers (DELTARUNE New only)
sharebuf_test=OFF  # sharebuf=test: force one sharebuf violation (a test build)
sharebuf_fulltest=OFF # sharebuf=fulltest: every whole clear of a shared scratch fills it all, each cut checked (a test build)
sharebuf_g2=ON     # sharebuf=g1: sharebuf without G2 (the base of check_sharebuf_layout.py --g2)
knightgpu_dspe=   # SESSION, SCALAR, OFF, or empty: the default (SESSION in DELTARUNE New, OFF elsewhere)
smalltext=     # ON, OFF, or empty: the game's default (UNDERTALE and DELTARUNE New ON, DELTARUNE Old OFF)
aot_mode=      # off, auto (the "aot" argument) or a directory; empty: the game and target's default
data_dir=
aot_dir=
aot_stress=OFF
aot_skip=      # why an "aot" build went on without AOT
memcap=        # KB, empty: all the system grants
openh264=ON
typed_census=OFF
typed_framelog=OFF
spdcount=OFF  # spdcount (change creddodge, test word): per-frame [spd] counts of the speed/direction memo
typed=         # typed AOT regions: ON, OFF, or empty: ON where "aot" generates the code
typed_list=    # their selection list or "all"; empty: all for DELTARUNE, tools/aot/selection/typed-undertale.txt
typed_stats=OFF
typed_stress=
late_numeric_entry=auto # WAD16 numeric lava copy for generated single-table UNDERTALE
aot_region=
openh264_skip= # set when the openh264 build failed
reachgate=OFF
shots=OFF
rosaprobe=OFF
rosalow=
utfadeprobe=OFF
bsplace=OFF
labcrash=OFF
bubblecheck=OFF
touchtest=OFF
touchqueue=OFF
nametrace=OFF
roomplan=ON
roomplan_rec=OFF
phase_layout= # opt-in audited linker script, empty: standard toolchain layout
input_period=16
input_stall=0
pacer_observe=OFF
pacer_rule=OFF
pacer_csv_diag=OFF
record_fail_after= # empty: no allocation injection
spikerec=OFF
homeaudioprobe=OFF
freewins=OFF
uttc_mode=     # uttc (change uthunt T1, test words): empty, OFF (uttc=off) or FORCE (uttc-force)
dprank_mode=   # dprank (change uthunt, test words): empty, OFF (dprank=off: always the scan) or DIAG (dprank-diag)
testpage=OFF   # testpage: one more 4 KiB page of UNDERTALE late code (measurement builds only, never a release)
bfprobe=OFF
dimlightdiag=OFF
arenalog=OFF
frametrace=OFF
asgprobe=OFF
flowdiag=OFF
titandiag=OFF
exittrail=OFF
ballast=OFF
writerfront=ON
repack_test= # test builds: a folder of <data.win folder>/ repack files for the RomFS (ctr_repack.inc)
repack_plans=OFF # repackplans: with repacktest, the plans leave out the tiles repack remaps (measured option A)
towerprof=OFF
towerforce=0
ppl2=ON
ppforce=OFF
for arg in "$@"; do
    case "$arg" in
        undertale|deltarune) game=$arg ;;
        n3ds|o3ds) target=$arg ;;
        Release|RelWithDebInfo|Debug|MinSizeRel) config=$arg ;;
        profile) profile=ON ;;
        heap) heap=ON ;;
        slotdiag) slotdiag=ON ;;
        recordfail=*)
            record_fail_after=${arg#recordfail=}
            if [[ ! "$record_fail_after" =~ ^(0|[1-9][0-9]*)$ ]] || (( ${#record_fail_after} > 5 )) || (( record_fail_after > 32767 )); then
                echo "recordfail needs an operation count from 0 through 32767" >&2
                exit 2
            fi
            ;;
        gameend) gameend=ON ;;
        statedump=*) statedump="${arg#statedump=}" ;;
        audiocap) audiocap=ON ;;
        pcmhash) pcmhash=ON ;;
        loadaudio-control) loadaudio_control=ON ;;
        audiometrics) audiometrics=ON ;;
        adpcmprobe) adpcmprobe=ON ;;
        adpcmsfx) adpcmsfx=ON ;;
        adpcmmusic) adpcmmusic=ON ;;
        adpcmtestmtime) adpcmtestmtime=ON ;;
        adpcmpack) adpcmpack=ON ;;
        adpcmpack=off) adpcmpack=OFF ;;
        adpcmfault=tail) adpcmfault=1 ;;
        adpcmfault=seam) adpcmfault=2 ;;
        adpcmfault=pool) adpcmfault=3 ;;
        adpcmfault=ring) adpcmfault=4 ;;
        audio-convert) audio_convert=ON ;;
        surfcensus) surfcensus=ON ;;
        vramcensus) vramcensus=ON ;;
        offdraw) offdraw=ON ;;
        halfcensus) halfcensus=ON ;;
        pklindiag) pklindiag=ON ;;
        pklinstress) pklinstress=ON ;;
        halfart) halfart=box ;;
        halfart=point|halfart=box|halfart=balanced|halfart=balanced[0-9]*) halfart=${arg#halfart=} ;;
        flowcode=lazy) flowcode=ON ;;
        flowcode-plan=*) flowcode_plan=${arg#flowcode-plan=} ;;
        natsurf6wide) natsurf6wide=ON ;;
        cia80) cia80=ON ;;
        knightnew-history) knightnew_history=ON ;;
        knightnew-history=off) knightnew_history=OFF ;;
        gpukick) gpu_kick=ON; gpu_kick_mode=single ;;
        gpukick=multi) gpu_kick=ON; gpu_kick_mode=multi ;;
        gpukick=multi-force) gpu_kick=ON; gpu_kick_mode=multi-force ;;
        gpukick=off) gpu_kick=OFF; gpu_kick_mode=multi ;;
        gpukick-trace) gpu_kick_trace=ON ;;
        knightgpu-texenv=off) knightgpu_texenv=OFF ;;
        knightgpu-compact=off) knightgpu_compact=OFF ;;
        knightgpu-compact=force) knightgpu_compact_force=ON ;;
        sharebuf=off) sharebuf=OFF ;;
        sharebuf=test) sharebuf=ON; sharebuf_test=ON ;;
        sharebuf=fulltest) sharebuf=ON; sharebuf_fulltest=ON ;;
        sharebuf=g1) sharebuf=ON; sharebuf_g2=OFF ;;
        knightgpu-dspe=on) knightgpu_dspe=SESSION ;;
        knightgpu-dspe=scalar) knightgpu_dspe=SCALAR ;;
        knightgpu-dspe=off) knightgpu_dspe=OFF ;;
        aot|aot=auto) aot_mode=auto ;;
        aot=chapters) aot_mode=chapters ;;
        aot=single) aot_mode=single ;;
        aot=off) aot_mode=off ;;
        aot=*) aot_mode=dir; aot_dir=${arg#aot=} ;;
        data=*) data_dir=${arg#data=} ;;
        aot-stress) aot_stress=ON ;;
        memcap=*) memcap=${arg#memcap=} ;;
        openh264=off) openh264=OFF ;;
        reachgate) reachgate=ON ;;
        shots) shots=ON ;;
        rosaprobe) rosaprobe=ON ;;
        rosalow=*) rosalow=${arg#rosalow=} ;;
        utfadeprobe) utfadeprobe=ON ;;
        bsplace) bsplace=ON ;;
        labcrash) labcrash=ON ;;
        bubblecheck) bubblecheck=ON ;;
        touchtest) touchtest=ON ;;
        touchqueue) touchtest=ON; touchqueue=ON ;;
        nametrace) touchtest=ON; nametrace=ON ;;
        pacer-observe) pacer_observe=ON ;;
        pacer) pacer_observe=ON; pacer_rule=ON ;;
        pacer-csv-diag) pacer_csv_diag=ON ;;
        roomplan=off) roomplan=OFF ;;
        roomplan-rec) roomplan_rec=ON ;;
        smalltext) smalltext=ON ;;
        smalltext=off) smalltext=OFF ;;
        layout=*) phase_layout=${arg#layout=} ;;
        inputperiod=*) input_period=${arg#inputperiod=} ;;
        inputstall=*) input_stall=${arg#inputstall=} ;;
        spikerec) spikerec=ON ;;
        homeaudioprobe) homeaudioprobe=ON ;;
        homeaudioauto) homeaudioprobe=AUTO ;;
        homeaudiofake) homeaudioprobe=FAKE ;;
        freewins) freewins=ON ;;
        uttc=off) uttc_mode=OFF ;;
        uttc-force) uttc_mode=FORCE ;;
        testpage) testpage=ON ;;
        dprank=off) dprank_mode=OFF ;;
        dprank-diag) dprank_mode=DIAG ;;
        bfprobe) bfprobe=ON ;;
        dimlightdiag) dimlightdiag=ON ;;
        arenalog) arenalog=ON ;;
        frametrace) frametrace=ON ;;
        asgprobe) asgprobe=ON ;;
        flowdiag) flowdiag=ON ;;
        titandiag) titandiag=ON ;;
        exittrail) exittrail=ON ;;
        ballast) ballast=ON ;;
        nowriterfront) writerfront=OFF ;;
        writerfront-fallback) writerfront=FALLBACK ;;
        writerfront-count) writerfront=COUNT ;;
        towerprof) towerprof=ON ;;
        towerforce1) towerforce=1 ;;
        towerforce2) towerforce=2 ;;
        nol2) ppl2=OFF ;;
        ppforce) ppforce=ON ;;
        typed-census) typed_census=ON ;;
        typed-framelog) typed_framelog=ON ;;
        spdcount) spdcount=ON ;;
        typed) typed=ON ;;
        typed=off) typed=OFF ;;
        typed=*) typed=ON; typed_list=${arg#typed=} ;;
        typed-stats) typed_stats=ON ;;
        typed-stress=*) typed_stress=${arg#typed-stress=} ;;
        late-numeric=*) late_numeric_entry=${arg#late-numeric=} ;;
        aot-region=*) aot_region=${arg#aot-region=} ;;
        repacktest=*) repack_test=${arg#repacktest=} ;;
        repackplans) repack_plans=ON ;;
        *)
            echo "unknown argument '$arg'" >&2
            echo "usage: $0 [undertale|deltarune] [n3ds|o3ds] [Release|RelWithDebInfo|Debug|MinSizeRel] [profile] [heap] [aot|aot=chapters|aot=single|aot=off|aot=<dir>] [data=<game folder>] [aot-stress] [memcap=<KB>] [openh264=off] [reachgate] [shots] [layout=<script>] [typed|typed=off|typed=all|typed=<list>] [typed-stats] [typed-stress=<n>]" >&2
            exit 2
            ;;
    esac
done
if [ "$loadaudio_control" = ON ] && { [ "$pcmhash" != ON ] || [ "$game" != undertale ]; }; then
    echo "loadaudio-control requires undertale pcmhash (diagnostic only)" >&2
    exit 2
fi
if [ -n "$halfart" ] && { [ "$game" != undertale ] || [ "$target" != o3ds ]; }; then
    echo "halfart=$halfart is for UNDERTALE on o3ds only (Omega Flowey's art on the Old 3DS); this is $game $target" >&2
    exit 2
fi
case "$input_period" in
    0|8|16) ;;
    *) echo "inputperiod must be 0, 8 or 16 ms" >&2; exit 2 ;;
esac
if ! [[ "$input_stall" =~ ^(0|[1-9][0-9]{0,2}|1000)$ ]]; then
    echo "inputstall must be 0..1000 ms" >&2
    exit 2
fi
input_cmake=()
if [ "$game" = undertale ]; then
    input_cmake=(-DCTRPORT_INPUTLATE_PERIOD_MS="$input_period" -DCTRPORT_INPUTLATE_STALL_MS="$input_stall")
elif [ "$input_period" != 16 ] || [ "$input_stall" != 0 ]; then
    echo "input sampler diagnostics require undertale" >&2
    exit 2
fi
# AOT is on by default: the hardware A/Bs confirmed it for both games on both targets. DELTARUNE gets a
# module per chapter, UNDERTALE (one data.win) a single set.
if [ -z "$aot_mode" ]; then
    aot_mode=auto
fi
if [ "$aot_mode" = auto ]; then
    if [ "$game" = deltarune ]; then aot_mode=chapters; else aot_mode=single; fi
fi
# Typed regions are on by default where prepare.py generates the code.
if [ -z "$typed" ]; then
    case "$aot_mode" in
        chapters|single) typed=ON ;;
        *) typed=OFF ;;
    esac
fi
if [ "$late_numeric_entry" = auto ]; then
    if [ "$game" = undertale ] && [ "$aot_mode" = single ] && [ "$typed" = ON ]; then
        late_numeric_entry=gml_Script_scr_true_lavawaver
    else
        late_numeric_entry=
    fi
elif [ "$late_numeric_entry" = off ]; then
    late_numeric_entry=
fi
if [ -n "$late_numeric_entry" ] && { [ "$game" != undertale ] || [ "$aot_mode" != single ] || [ "$typed" != ON ]; }; then
    echo 'late-numeric requires UNDERTALE with generated single-table typed AOT' >&2
    exit 2
fi
late_numeric_suffix=
if [ "$game" = undertale ] && [ "$aot_mode" = single ] && [ "$typed" = ON ] && [ -z "$late_numeric_entry" ]; then
    late_numeric_suffix=-lnumoff
fi

: "${DEVKITPRO:?DEVKITPRO is not set; install devkitPro and the 3ds-dev group}"
if [ -f /etc/profile.d/devkit-env.sh ]; then
    source /etc/profile.d/devkit-env.sh
fi

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build="$repo/build/3ds-$game-$target"
if [ "$input_period" != 16 ] || [ "$input_stall" != 0 ]; then
    build="$build-inputlate-p${input_period}-s${input_stall}"
fi
if [ "$pacer_rule" = ON ]; then
    build="$build-pacer"
elif [ "$pacer_observe" = ON ]; then
    build="$build-pacer-observe"
fi
if [ -n "$record_fail_after" ]; then
    build="$build-recordfail$record_fail_after"
fi
if [ -n "$phase_layout" ]; then
    build="$build-phase-layout"
fi
if [ "$pacer_csv_diag" = ON ]; then
    build="$build-pacer-csv-diag"
fi
if [ "$profile" = ON ]; then
    build="$build-profile"
fi
if [ "$heap" = ON ]; then
    build="$build-heap"
fi
if [ "$slotdiag" = ON ]; then
    build="$build-slotdiag"
fi
if [ "$gameend" = ON ]; then
    build="$build-gameend"
fi
if [ "$typed_census" = ON ]; then
    build="$build-census"
fi
if [ "$typed_framelog" = ON ]; then
    build="$build-ft"
fi
if [ "$spdcount" = ON ]; then
    build="$build-spdcount"
fi
if [ "$typed_stats" = ON ] || [ -n "$typed_stress" ]; then
    if [ "$typed" != ON ]; then
        echo "typed-stats and typed-stress=<n> need typed" >&2
        exit 2
    fi
fi
typed_suffix=  # of the build and generated-code folders: -tyoff without typed regions where they are the default
if [ "$typed" = ON ]; then
    if [ "$aot_mode" = dir ]; then typed_suffix=-ty; fi
    if [ -z "$typed_list" ]; then
        if [ "$game" = deltarune ]; then typed_list=all
        elif [ -f "$repo/tools/aot/selection/typed-$game-$target.txt" ]; then typed_list="$repo/tools/aot/selection/typed-$game-$target.txt"
        else typed_list="$repo/tools/aot/selection/typed-$game.txt"; fi
    fi
    if [ "$typed_stats" = ON ]; then build="$build-tystats"; fi
    if [ -n "$typed_stress" ]; then build="$build-tystress$typed_stress"; fi
elif [ "$aot_mode" = chapters ] || [ "$aot_mode" = single ]; then
    typed_suffix=-tyoff
fi
if [ -n "$statedump" ]; then
    build="$build-statedump"
fi
if [ "$audiocap" = ON ]; then
    build="$build-audiocap"
fi
if [ "$pcmhash" = ON ]; then
    build="$build-pcmhash"
fi
if [ "$loadaudio_control" = ON ]; then
    build="$build-loadaudio-control"
fi
if [ "$audiometrics" = ON ]; then
    build="$build-audiometrics"
fi
if [ "$adpcmprobe" = ON ]; then
    build="$build-adpcmprobe"
fi
if [ "$adpcmsfx" = ON ]; then
    build="$build-adpcmsfx"
fi
if [ "$adpcmmusic" = ON ]; then
    build="$build-adpcmmusic"
fi
# The UNDERTALE sound-effect pack (ctr_adpcm_pack.inc; the builder writes adpcm-sfx.dtsp beside data.win) is part of
# UNDERTALE Old builds unless "adpcmpack=off" or an ADPCM prototype word (adpcmprobe/adpcmsfx/adpcmmusic, which own
# the audio factory wrapper) is given; "adpcmpack" adds it to UNDERTALE New. Only a non-default choice names the
# build directory (-adpcmpack, -noadpcmpack).
adpcmpack_default=OFF
if [ "$game" = undertale ] && [ "$target" = o3ds ] && [ "$adpcmprobe$adpcmsfx$adpcmmusic" = OFFOFFOFF ]; then
    adpcmpack_default=ON
fi
if [ -z "$adpcmpack" ]; then
    adpcmpack=$adpcmpack_default
elif [ "$adpcmpack" != "$adpcmpack_default" ]; then
    if [ "$adpcmpack" = ON ]; then build="$build-adpcmpack"; else build="$build-noadpcmpack"; fi
fi
if [ "$adpcmfault" != 0 ]; then
    if [ "$adpcmpack" != ON ]; then
        echo "ERROR: adpcmfault requires the sound pack (adpcmpack); test builds only" >&2
        exit 2
    fi
    build="$build-adpcmfault$adpcmfault"
fi
if [ "$adpcmtestmtime" = ON ]; then
    if [ "$adpcmmusic" != ON ]; then
        echo "ERROR: adpcmtestmtime requires adpcmmusic; test builds only" >&2
        exit 2
    fi
    build="$build-adpcmtestmtime"
fi
if [ "$audio_convert" = ON ]; then
    build="$build-audioconvert"
fi
if [ "$surfcensus" = ON ]; then
    build="$build-surfcensus"
fi
if [ "$vramcensus" = ON ]; then
    build="$build-vramcensus"
fi
if [ "$offdraw" = ON ]; then
    build="$build-offdraw"
fi
if [ "$halfcensus" = ON ]; then
    build="$build-halfcensus"
fi
if [ "$pklindiag" = ON ]; then
    build="$build-pklindiag"
fi
if [ "$pklinstress" = ON ]; then
    build="$build-pklinstress"
fi
if [ -n "$halfart" ]; then
    build="$build-halfart$halfart"
fi
if [ "$natsurf6wide" = ON ]; then
    build="$build-ns6wide"
fi
if [ "$cia80" = ON ]; then
    if [ "$game" != deltarune ] || [ "$target" != o3ds ]; then
        echo "cia80 requires deltarune o3ds; its .3dsx is not the ordinary HBL artifact" >&2
        exit 2
    fi
    build="$build-cia80"
fi
if [ "$flowcode" = ON ]; then build="$build-flowcode"; fi
# Knight GPU-1 (z_knightnew_history.c) is part of DELTARUNE New builds unless "knightnew-history=off".
if [ -z "$knightnew_history" ]; then
    if [ "$game" = deltarune ] && [ "$target" = n3ds ]; then knightnew_history=ON; else knightnew_history=OFF; fi
elif [ "$knightnew_history" = OFF ] && [ "$game" = deltarune ] && [ "$target" = n3ds ]; then
    build="$build-noknighthist"
fi
if [ "$knightgpu_texenv" = OFF ] && [ "$game" = deltarune ] && [ "$target" = n3ds ]; then
    build="$build-nokte"
fi
if [ "$knightgpu_compact" = OFF ] && [ "$game" = deltarune ] && [ "$target" = n3ds ]; then
    build="$build-nokgc"
elif [ "$knightgpu_compact_force" = ON ] && [ "$game" = deltarune ] && [ "$target" = n3ds ]; then
    build="$build-kgcforce"
fi
if [ "$game" = deltarune ] && [ "$target" = n3ds ]; then
    if [ "$sharebuf" = OFF ]; then build="$build-nosb"; elif [ "$sharebuf_test" = ON ]; then build="$build-sbtest"; elif [ "$sharebuf_fulltest" = ON ]; then build="$build-sbfull"; elif [ "$sharebuf_g2" = OFF ]; then build="$build-sbg1"; fi
fi
# knightgpu-dspe=off/scalar share the build and generated-code directories with the ON build (an A/B that differs
# only by the draw_surface_part_ext path); switching regenerates the AOT code.
if [ -n "$knightgpu_dspe" ] && [ "$knightgpu_dspe" != OFF ] && { [ "$game" != deltarune ] || [ "$target" != n3ds ]; }; then
    echo "knightgpu-dspe=on|scalar is for DELTARUNE New" >&2
    exit 2
fi
if [ -z "$knightgpu_dspe" ]; then
    if [ "$game" = deltarune ] && [ "$target" = n3ds ]; then knightgpu_dspe=SESSION; else knightgpu_dspe=OFF; fi
fi
if [ -n "$memcap" ]; then
    build="$build-cap$memcap"
fi
if [ "$reachgate" = ON ]; then
    build="$build-reachgate"
fi
if [ "$shots" = ON ]; then
    build="$build-shots"
fi
if [ "$rosaprobe" = ON ]; then
    build="$build-rosaprobe"
fi
if [ -n "$rosalow" ]; then
    build="$build-rosalow$rosalow"
fi
if [ "$utfadeprobe" = ON ]; then
    build="$build-utfadeprobe"
fi
if [ "$bsplace" = ON ]; then
    build="$build-bsplace"
fi
if [ "$labcrash" = ON ]; then
    if [ "$game" != undertale ] || [ -n "${CTRPORT_RELEASE_SOURCE_URL:-}" ] || [ "$exittrail" = ON ]; then
        echo "ERROR: labcrash is an UNDERTALE test build: never a release (CTRPORT_RELEASE_SOURCE_URL) or with exittrail" >&2
        exit 2
    fi
    build="$build-labcrash"
fi
if [ "$bubblecheck" = ON ]; then
    build="$build-bubblecheck"
fi
if [ "$touchtest" = ON ]; then
    build="$build-touchtest"
fi
if [ "$touchqueue" = ON ]; then
    build="$build-touchqueue"
fi
if [ "$nametrace" = ON ]; then
    build="$build-nametrace"
fi
if [ "$roomplan" = OFF ]; then
    build="$build-noplan"
fi
if [ "$roomplan_rec" = ON ]; then
    build="$build-rprec"
fi
if [ "$spikerec" = ON ]; then
    build="$build-spikerec"
fi
if [ "$homeaudioprobe" != OFF ]; then
    build="$build-homeaudio$homeaudioprobe"
fi
if [ "$freewins" = ON ]; then
    build="$build-freewins"
fi
if [ "$uttc_mode" = OFF ]; then
    build="$build-nouttc"
elif [ "$uttc_mode" = FORCE ]; then
    build="$build-uttcforce"
fi
if [ "$testpage" = ON ]; then
    build="$build-testpage"
fi
if [ "$dprank_mode" = OFF ]; then
    build="$build-nodprank"
elif [ "$dprank_mode" = DIAG ]; then
    build="$build-dprankdiag"
fi
if [ "$bfprobe" = ON ]; then
    build="$build-bfprobe"
fi
if [ "$dimlightdiag" = ON ]; then
    if [ "$game" != undertale ]; then
        echo "ERROR: dimlightdiag requires undertale" >&2
        exit 2
    fi
    build="$build-dimlightdiag"
fi
if [ "$arenalog" = ON ]; then
    build="$build-arenalog"
fi
if [ "$asgprobe" = ON ]; then
    build="$build-asgprobe"
fi
if [ "$frametrace" = ON ]; then
    build="$build-frametrace"
fi
if [ "$titandiag" = ON ]; then
    build="$build-titandiag"
fi
if [ "$exittrail" = ON ]; then
    build="$build-exittrail"
fi
if [ "$ballast" = ON ]; then
    if [ "$game" != undertale ] || [ -n "${CTRPORT_RELEASE_SOURCE_URL:-}" ] || [ "$dimlightdiag" = ON ]; then
        echo "ERROR: ballast is an UNDERTALE test build: never a release (CTRPORT_RELEASE_SOURCE_URL) or with dimlightdiag" >&2
        exit 2
    fi
    build="$build-ballast"
fi
if [ "$flowdiag" = ON ]; then
    build="$build-flowdiag"
fi
case "$writerfront" in
    OFF) build="$build-nowriterfront" ;;
    FALLBACK) build="$build-writerfront-fallback" ;;
    COUNT) build="$build-writerfront-count" ;;
esac
if [ "$towerprof" = ON ]; then
    build="$build-towerprof"
fi
if [ "$towerforce" != 0 ]; then
    build="$build-towerforce$towerforce"
fi
if [ "$ppl2" = OFF ]; then
    build="$build-nol2"
fi
if [ "$ppforce" = ON ]; then
    build="$build-ppforce"
fi
chapters=
if [ "$aot_mode" = chapters ]; then
    if [ "$game" != deltarune ]; then
        echo "aot=chapters is for DELTARUNE (one module per chapter)" >&2
        exit 2
    fi
    aot_mode=auto
    chapters=1
elif [ "$aot_mode" = single ]; then
    aot_mode=auto
    if [ "$game" = deltarune ]; then
        build="$build-single"
    fi
fi
if [ "$aot_mode" = auto ]; then
    build="$build$typed_suffix$late_numeric_suffix"
fi
# A native path for Python on Windows (the py launcher is a Windows program); a no-op elsewhere.
native_path() {
    if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}
# Said when an "aot" build goes on without AOT: before the build, and again after it so it isn't lost in the output.
aot_warn() {
    {
        echo
        echo "WARNING: ${1:-building} WITHOUT AOT: $aot_skip."
        echo "WARNING: GML then runs in the interpreter only, slower in heavy scenes."
        echo "WARNING: aot=off builds without AOT and without this warning."
        echo
    } >&2
}
source "$here/python.sh"
release_windows_profile_env
release_find_python
# Keep native support and generated event bindings in agreement. Other targets
# retain their original generated code and native image.
if [ "$game" = deltarune ] && [ "$target" = n3ds ] &&
    { [ -n "$chapters" ] || { [ "$aot_mode" = dir ] && [ -f "$aot_dir/aot_modules.txt" ]; }; }; then
    export CTRPORT_WEIRDSHD_PINWHEEL="${CTRPORT_WEIRDSHD_PINWHEEL:-1}"
else
    export CTRPORT_WEIRDSHD_PINWHEEL=0
fi
python_executable=$(release_python_executable)
# datarefuse (playblocker #8): UNDERTALE builds only from an unmodified Steam or GOG data.win; the console supports
# only that file and warns about any other (CTRPORT_DATA_SIZE, CMakeLists.txt). Anything else stops here with the
# reason; no data folder found (exit 3) leaves the size empty, as before.
data_size=
if [ "$game" = undertale ]; then
    input_args=(--build-input undertale --cache "$(native_path "$repo/build/data-versions-$game")")
    if [ -n "$data_dir" ]; then input_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    data_size=$("${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/data_versions.py")" "${input_args[@]}")
    rc=$?
    set -e
    data_size=$(printf '%s' "$data_size" | tr -d '\r')
    if [ "$rc" = 3 ]; then data_size=
    elif [ "$rc" != 0 ]; then exit "$rc"
    fi
    # hitcheck: UT omits the PC region specialization. FUNC covers every GM1.4
    # builtin call, including interpreted code; stop if future data needs it.
    region_args=()
    if [ -n "$data_dir" ]; then region_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/hitcheck_guard.py")" "${region_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ]; then :
    elif [ "$rc" != 3 ]; then exit "$rc";
    fi
fi
# prepare.py checks the same version table before using even an up-to-date AOT
# result. Check non-AOT/custom-code builds too, without generating anything.
if [ "$aot_mode" != auto ] && [ -n "${python:-}" ]; then
    version_args=(--check-only --game "$game" --target "$target" --out "$(native_path "$repo/build/data-versions-$game")")
    if [ -n "$data_dir" ]; then version_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/prepare.py")" "${version_args[@]}"
    rc=$?
    set -e
    if [ "$rc" != 0 ] && [ "$rc" != 3 ]; then exit "$rc"; fi
fi
if [ "$aot_mode" = auto ]; then
    echo "AOT: using ${python[*]} ($(command -v "${python[0]}"))"
    gen="$repo/build/aot-$game-$target${chapters:+-chapters}$typed_suffix$late_numeric_suffix"
    # knightgpu dspe: tools/aot/typed.py's draw_surface_part_ext calls (DELTARUNE New); prepare.py's fingerprint counts it.
    case "$knightgpu_dspe" in
        SESSION) export CTRPORT_AOT_DSPE=session ;;
        SCALAR) export CTRPORT_AOT_DSPE=scalar ;;
        *) unset CTRPORT_AOT_DSPE ;;
    esac
    # knightgpu dspe's room gate: every room hash in vine_titancpu.inc's knGate must name its intended room in this game
    # data (tools/aot/check_dspe_gate.py), so a wrong constant fails the build; exit 3 (no game data) goes on.
    if [ "$knightgpu_dspe" = SESSION ]; then
        gate_args=(--source "$(native_path "$here/source/vine_titancpu.inc")")
        if [ -n "$data_dir" ]; then gate_args+=(--data "$(native_path "$data_dir")"); fi
        set +e
        "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/check_dspe_gate.py")" "${gate_args[@]}"
        rc=$?
        set -e
        if [ "$rc" = 2 ]; then
            echo "dspe gate: tools/aot/check_dspe_gate.py failed" >&2
            exit 2
        fi
        rc=0
    fi
    prep_args=(--game "$game" --target "$target" --out "$(native_path "$gen")" ${chapters:+--chapters})
    if [ -n "$late_numeric_entry" ]; then prep_args+=(--late-numeric "$late_numeric_entry"); fi
    if [ "$typed" = ON ]; then
        if [ "$typed_list" = all ]; then prep_args+=(--typed all); else prep_args+=(--typed "$(native_path "$typed_list")"); fi
    fi
    if [ -n "$data_dir" ]; then
        prep_args+=(--data "$(native_path "$data_dir")")
    fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/prepare.py")" "${prep_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ]; then
        aot_dir=$gen
    elif [ "$rc" = 3 ]; then
        GAME=$(printf '%s' "$game" | tr '[:lower:]' '[:upper:]')
        aot_skip="no $GAME game data found (pass data=<game folder>, or set \$CTRPORT_${GAME}_DATA)"
        aot_warn
    else
        echo "AOT: tools/aot/prepare.py failed (exit $rc); aot=off builds without AOT" >&2
        exit "$rc"
    fi
    if [ "$aot_stress" = ON ]; then
        echo "aot-stress needs aot=<dir>" >&2
        exit 2
    fi
elif [ "$aot_mode" = dir ]; then
    aot_dir=$(cd "$aot_dir" && pwd)
    build="$build-aot$typed_suffix"
    if [ "$aot_stress" = ON ]; then
        build="$build-stress"
    fi
elif [ "$aot_stress" = ON ]; then
    echo "aot-stress needs aot=<dir>" >&2
    exit 2
fi

# Preparation may continue without game data. Only chapter modules can call
# the native event wrappers; single-AOT and interpreted builds leave them off.
if [ -z "$aot_dir" ] || [ ! -f "$aot_dir/aot_modules.txt" ]; then
    export CTRPORT_WEIRDSHD_PINWHEEL=0
fi

# UNDERTALE's battle-grid runs: private build input in both AOT and interpreted builds.
# The fixed runtime slot and draw path are unchanged; no sprite coordinates are kept in source.
pixgrid_runs=
if [ "$game" = undertale ]; then
    if [ -z "${python:-}" ]; then
        echo "Battle grid: Python and the player's UNDERTALE data are required" >&2
        exit 3
    fi
    grid="$repo/build/pixgrid-undertale/pixgrid_runs.inc"
    grid_args=(--out "$(native_path "$grid")")
    if [ -n "$data_dir" ]; then grid_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/pixgrid.py")" "${grid_args[@]}"
    pixgrid_runs=$grid
    prompt_args=(--out "$(native_path "$repo/build/pixgrid-undertale")")
    if [ -n "$data_dir" ]; then prompt_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/btnprompt_build.py")" "${prompt_args[@]}"
    # UNDERTALE Old's covered-star key (tools/aot/dskip.py; the no-key values when the data isn't a checked version).
    skip_args=(--out "$(native_path "$repo/build/pixgrid-undertale/dskip_key.inc")")
    if [ -n "$data_dir" ]; then skip_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/dskip.py")" "${skip_args[@]}"
fi

# The loading screens' art (z_loadscreen.c), cut from the player's own files (tools/aot/loadscreen_art.py) into the
# RomFS: both games' first time setup line, and DELTARUNE's loading text and Chapter 3's dog. Without known game files
# the loading screen is a bar alone.
loadscreen_art=
if [ -n "${python:-}" ]; then
    art="$repo/build/loadscreen-$game/loadscreen.bin"
    art_args=(--out "$(native_path "$art")" --game "$game")
    if [ -n "$data_dir" ]; then art_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/loadscreen_art.py")" "${art_args[@]}" || true
    if [ -f "$art" ]; then loadscreen_art=$art; fi
fi

# Frame is withdrawn from 1.0: no private area art or SAVE metadata is derived.

# Approved Crisp battle art, generated from verified game files only (never committed). Release kits intentionally
# omit the derivation script and its numpy/Pillow dependencies, and build with the base art. Where the script is here
# (this repository), the shipping game needs the art: if its tools don't import (the message names the import and the
# user site-packages Python looked in) or the derivation fails, the build stops (release_missing_derivation;
# CTRPORT_DERIVATION_OPTIONAL=1 lets a tools-only build go on with the base art). Publish the path only after
# successful generation, so a partial failure cannot package stale art.
crispart_dir=
if [ "$game" = deltarune ] && [ -f "$repo/tools/analysis/crispart_pack.py" ]; then
    crispart_why=
    if [ -z "${python:-}" ]; then
        crispart_why="no Python"
    elif ! crispart_check=$("${python[@]}" -X utf8 -I "$(native_path "$repo/tools/analysis/crispart_pack.py")" --check-tools 2>&1); then
        crispart_why="tools/analysis/crispart_pack.py can't import its tools ($(printf '%s\n' "$crispart_check" | tr -d '\r' | tail -n 1))"
    else
        crispart_out="$repo/build/crispart-deltarune"
        crispart_args=(--out "$(native_path "$crispart_out")")
        if [ -n "$data_dir" ]; then crispart_args+=(--data "$(native_path "$data_dir")"); fi
        if "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/analysis/crispart_pack.py")" "${crispart_args[@]}"; then
            crispart_dir=$crispart_out
        else
            crispart_why="tools/analysis/crispart_pack.py failed (its output is above)"
        fi
    fi
    if [ -n "$crispart_why" ]; then
        release_missing_derivation "Crisp art" "$crispart_why" || exit 3
    fi
fi

# The half-resolution art prototype (halfart=point|box|balanced, change flowhalf; tools/analysis/halfart_pack.py): the
# sprites listed in tools/analysis/halfart/<game>.txt (balanced: the frames <game>-balanced.txt lists, box filter) at half resolution, in romfs:/halfart/<game>.dthr, and the room plan
# below made for their new places. The lists hold hashes of the sprite names, matched against this data.win. The
# .pages file beside the patch holds the patched pages for the release builder's texture preparation on the PC
# (tools/release/builder/texcache.py). Exit 3: no game data; the build stops (the build asked for it).
halfart_remap=
halfart_file=
if [ -n "$halfart" ]; then
    if [ -z "${python:-}" ]; then
        echo "halfart=$halfart needs Python" >&2
        exit 1
    fi
    # UNDERTALE's release source has it (the builder's halfart setting); DELTARUNE's does not.
    if [ ! -f "$repo/tools/analysis/halfart_pack.py" ]; then
        echo "halfart=$halfart needs tools/analysis/halfart_pack.py (not in this source tree)" >&2
        exit 2
    fi
    ha_dir="$repo/build/halfart-$game"
    ha_list=$repo/tools/analysis/halfart/$game.txt
    ha_filter=$halfart
    case $halfart in balanced*) ha_list=$repo/tools/analysis/halfart/$game-$halfart.txt; ha_filter=box ;; esac
    ha_args=(--game "$game" --list "$(native_path "$ha_list")" --filter "$ha_filter"
             --out "$(native_path "$ha_dir/$game-$halfart.dthr")" --remap "$(native_path "$ha_dir/$game-$halfart.json")"
             --report "$(native_path "$ha_dir/$game-$halfart-report.json")" --pages "$(native_path "$ha_dir/$game-$halfart.pages")")
    if [ -n "$data_dir" ]; then ha_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/analysis/halfart_pack.py")" "${ha_args[@]}"
    halfart_file=$ha_dir/$game-$halfart.dthr
    halfart_remap=$ha_dir/$game-$halfart.json
fi

# The room plan: exit 3 means no game data or no record made for it.
roomplan_file=
roomplan_dir=
if [ "$roomplan" = ON ] && [ -n "${python:-}" ]; then
    plan_out="$repo/build/roomplan-$game/$game.dtp"
    if [ "$game" = deltarune ]; then
        plan_out="$repo/build/roomplan-$game"
        rm -f "$plan_out"/*.dtp
    fi
    if [ -n "$halfart_remap" ]; then plan_out="$repo/build/roomplan-$game/$game-halfart$halfart.dtp"; fi
    # UNDERTALE New's plan has no boot rooms rule (build_plan.py BOOT_AHEAD_MODELS), so it gets a file of its own.
    if [ "$game" = undertale ] && [ "$target" = n3ds ]; then plan_out="${plan_out%.dtp}-n3ds.dtp"; fi
    plan_args=(--game "$game" --out "$(native_path "$plan_out")")
    if [ "$game" = undertale ]; then plan_args+=(--model "$target"); fi
    if [ -n "$data_dir" ]; then plan_args+=(--data "$(native_path "$data_dir")"); fi
    if [ -n "$halfart_remap" ]; then plan_args+=(--halfart "$(native_path "$halfart_remap")"); fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/roomplan/build_plan.py")" "${plan_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ] && [ "$game" = deltarune ]; then
        roomplan_dir=$plan_out
        # drresid: the fights' track plan (tools/roomplan/build_tracks.py), New 3DS only; exit 3 builds without one.
        if [ "$target" = n3ds ] && [ "$roomplan_rec" != ON ]; then
            track_args=(--out "$(native_path "$plan_out")")
            if [ -n "$data_dir" ]; then track_args+=(--data "$(native_path "$data_dir")"); fi
            # repacktest + repackplans: the tracks leave out the tiles repack draws from new tiles in its rooms
            # (build_tracks.py). Without repackplans they stay as today (repacka: identity-exact, 10-28).
            if [ -n "$repack_test" ] && [ "$repack_plans" = ON ]; then track_args+=(--repack "$(native_path "$repack_test")"); fi
            set +e
            "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/roomplan/build_tracks.py")" "${track_args[@]}"
            rc=$?
            set -e
            if [ "$rc" = 2 ]; then
                echo "Track plan: tools/roomplan/build_tracks.py failed" >&2
                exit 2
            fi
            rc=0
            # sharebuf: the objects whose surfaces share one buffer (tools/roomplan/build_sharebuf.py), for the exact
            # data.win their code was read on; exit 3 builds without a list (nothing shares).
            if [ "$sharebuf" = ON ]; then
                share_args=(--out "$(native_path "$plan_out")")
                if [ -n "$data_dir" ]; then share_args+=(--data "$(native_path "$data_dir")"); fi
                set +e
                "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/roomplan/build_sharebuf.py")" "${share_args[@]}"
                rc=$?
                set -e
                if [ "$rc" = 2 ]; then
                    echo "Shared surfaces: tools/roomplan/build_sharebuf.py failed" >&2
                    exit 2
                fi
                rc=0
            fi
        fi
    elif [ "$rc" = 0 ]; then
        roomplan_file=$plan_out  # CMake (msys) takes the POSIX path
    elif [ "$rc" = 3 ]; then
        echo "Room plan: none for this game data; building without one" >&2
    else
        echo "Room plan: tools/roomplan/build_plan.py failed (exit $rc); roomplan=off builds without one" >&2
        exit "$rc"
    fi
fi

# The phase plan (phaseplan; tools/roomplan/build_phases.py): a long room's tiles in phases, from the committed phase
# records (tools/roomplan/phases/) resolved against the game data, one table per data.win with a record, in
# romfs:/roomplan/phases.dtph. Exit 3: no game data or no record for it; the build goes on without one. "roomplan=off"
# leaves it out too. DELTARUNE only (review decision, 2026-10-25). A file per target: a room's record may be for one model
# only (build_phases.py --model).
phaseplan_file=
if [ "$game" = deltarune ] && [ "$roomplan" = ON ] && [ -n "${python:-}" ]; then
    phase_out="$repo/build/roomplan-$game/phases-$target.dtph"
    phase_args=(--game "$game" --model "$target" --out "$(native_path "$phase_out")")
    if [ -n "$data_dir" ]; then phase_args+=(--data "$(native_path "$data_dir")"); fi
    # repacktest + repackplans: the phases leave out the tiles repack draws from new tiles in its rooms
    # (build_phases.py).
    if [ -n "$repack_test" ] && [ "$repack_plans" = ON ]; then phase_args+=(--repack "$(native_path "$repack_test")"); fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/roomplan/build_phases.py")" "${phase_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ]; then
        phaseplan_file=$phase_out
    elif [ "$rc" = 3 ]; then
        echo "Phase plan: none for this game data; building without one" >&2
    else
        echo "Phase plan: tools/roomplan/build_phases.py failed (exit $rc); roomplan=off builds without one" >&2
        exit "$rc"
    fi
fi

# The sound plan (UNDERTALE, utsfx; tools/roomplan/build_sfxplan.py): each scene's sounds, from the committed sound
# record made on this game data, copied into the sound cache at the room change. Exit 3: no game data or no record for
# it; the build goes on without one. "roomplan=off" leaves it out too.
sfxplan_file=
if [ "$roomplan" = ON ] && [ "$game" = undertale ] && [ -n "${python:-}" ]; then
    sfx_out="$repo/build/roomplan-$game/$game-sfx.dtp"
    sfx_args=()
    # UNDERTALE Old also gets the record's stream scenes (change omegaflowey L2), in a file of its own.
    if [ "$target" = o3ds ]; then
        sfx_out="$repo/build/roomplan-$game/$game-sfx-o3ds.dtp"
        sfx_args=(--streams)
    fi
    sfx_args+=(--out "$(native_path "$sfx_out")")
    if [ -n "$data_dir" ]; then sfx_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/roomplan/build_sfxplan.py")" "${sfx_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ]; then
        sfxplan_file=$sfx_out
    elif [ "$rc" = 3 ]; then
        echo "Sound plan: none for this game data; building without one" >&2
    else
        echo "Sound plan: tools/roomplan/build_sfxplan.py failed (exit $rc); roomplan=off builds without one" >&2
        exit "$rc"
    fi
fi

# The scene table (UNDERTALE, change layout S3; tools/layout/build_scenes.py): the committed names-only spec
# (tools/layout/undertale-scenes.json) resolved against the player's data.win, as romfs:/layout/undertale.lst. Exit 3:
# no game data, or an every-room name this data.win lacks; the build goes on without (every battle Original).
layout_scenes_file=
if [ "$game" = undertale ] && [ -n "${python:-}" ]; then
    scenes_out="$repo/build/layout-$game/$game.lst"
    scenes_args=(--out "$(native_path "$scenes_out")")
    if [ -n "$data_dir" ]; then scenes_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/layout/build_scenes.py")" "${scenes_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ]; then
        layout_scenes_file=$scenes_out
    elif [ "$rc" = 3 ]; then
        echo "Scene table: none for this game data; building without one (every battle Original)" >&2
    else
        echo "Scene table: tools/layout/build_scenes.py failed (exit $rc)" >&2
        exit "$rc"
    fi
fi

if [ "$audio_convert" = ON ]; then
    if [ -z "$data_dir" ] || [ -z "${python:-}" ]; then
        echo "audio-convert needs the player's data folder and the builder's Python" >&2
        exit 2
    fi
    audio_out="$repo/build/audio-adpcm-$game"
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/audio/prepare.py")" \
        --data "$(native_path "$data_dir")" --out "$(native_path "$audio_out")"
    audio_sd_out="$repo/build/audio-sd-$game/audio-adpcm"
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/audio/stage.py")" \
        --data "$(native_path "$data_dir")" --prepared "$(native_path "$audio_out")" \
        --out "$(native_path "$audio_sd_out")"
    echo "ADPCM assets staged at $audio_sd_out; game adapter selection remains disabled"
fi

openh264_warn() {
    {
        echo
        echo "WARNING: ${1:-building} WITHOUT openh264: tools/deps/openh264/build.sh failed (see above)."
        echo "WARNING: The intro videos then have no pictures without the New 3DS's hardware decoder."
        echo "WARNING: openh264=off builds without it and without this warning."
        echo
    } >&2
}
# UNDERTALE plays no videos.
if [ "$game" = undertale ]; then
    openh264=OFF
fi
if [ "$openh264" = ON ] && ! bash "$repo/tools/deps/openh264/build.sh"; then
    openh264_skip=1
    openh264_warn
    if [ -n "${CTRPORT_RELEASE_SOURCE_URL:-}" ]; then
        echo "The video decoder could not be built. Try again, or turn off intro videos in the builder." >&2
        exit 1
    fi
fi

# Only DR New has the compact chapter guard; Old keeps the qualified no-guard runtime.
if [ "$game" = deltarune ]; then
    if [ "$target" = n3ds ]; then
        chapter_args=(--out "$(native_path "$build/romfs/chapters.txt")")
        if [ -n "$data_dir" ]; then chapter_args+=(--data "$(native_path "$data_dir")"); fi
        "${python[@]}" -X utf8 "$repo/tools/aot/chapter_manifest.py" "${chapter_args[@]}"
    else
        rm -f "$build/romfs/chapters.txt"
    fi
fi

# The compiler cache (tools/buildcache/compile_cache.py): every C compile goes through it unless
# CTRPORT_CCACHE=0. Shared by every worktree of the repository, in <main checkout>/scratch/ccache unless
# $CTRPORT_CCACHE_DIR says otherwise. Reuse the already qualified interpreter.
launcher=
if [ "${CTRPORT_CCACHE:-1}" != 0 ]; then
    cc_python=$("${python[@]}" -X utf8 -c 'import sys; print(sys.executable)' | tr -d '\r')
    if command -v cygpath > /dev/null 2>&1; then cc_python=$(cygpath -u "$cc_python"); fi
    if [ -n "$cc_python" ]; then
        cache_dir=${CTRPORT_CCACHE_DIR:-}
        if [ -z "$cache_dir" ]; then
            # The main checkout is the parent of the repository's common .git, which every worktree shares.
            # Without a .git of its own (a source zip, even one unpacked inside a checkout) this tree keeps its own
            # cache; so does a checkout git can't read, or a machine with no git. Neither stops the build.
            main_checkout=$repo
            if [ -e "$repo/.git" ] || [ -L "$repo/.git" ]; then
                common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2> /dev/null | tr -d '
') || common=
                if [ "${common##*/}" = .git ]; then main_checkout=${common%/.git}; fi
            fi
            cache_dir="$main_checkout/scratch/ccache"
        fi
        mkdir -p "$cache_dir"
        launcher="$cc_python;-X;utf8;-I;-S;$(native_path "$repo/tools/buildcache/compile_cache.py");--cache-dir=$(native_path "$cache_dir")"
        export CTRPORT_CCACHE_LOG="$(native_path "$build")/compile_cache.log"
    else
        echo "compile cache: no Python 3.8 or later found (tried python3, python and py -3); building uncached" >&2
    fi
fi

# The launcher dispatcher (platform/3ds/launch_dispatch.cmake) runs each compile's whole launcher chain in one
# Python process; CTRPORT_LAUNCH_DISPATCH=OFF starts every launcher as its own process, as before. Set on every
# configure, so an OFF build never sticks. Its log times each compile (summary printed after the build).
# ON by default for UNDERTALE, where its identity is proven (docs/building-cia.md); DELTARUNE keeps the chain until then.
if [ "$game" = undertale ]; then launch_dispatch=${CTRPORT_LAUNCH_DISPATCH:-ON}; else launch_dispatch=${CTRPORT_LAUNCH_DISPATCH:-OFF}; fi
export CTRPORT_LAUNCH_DISPATCH_LOG="$(native_path "$build")/launch_dispatch.log"

# CTRPORT_FILE_PREFIX_MAP=OFF builds with the checkout's absolute paths (CMakeLists.txt); unset keeps the build
# directory's setting, ON by default.
prefix_map=()
if [ -n "${CTRPORT_FILE_PREFIX_MAP:-}" ]; then prefix_map=(-DCTRPORT_FILE_PREFIX_MAP="$CTRPORT_FILE_PREFIX_MAP"); fi
release_source=()
if [ -n "${CTRPORT_RELEASE_SOURCE_URL:-}" ]; then release_source=(-DCTRPORT_RELEASE_SOURCE_URL="$CTRPORT_RELEASE_SOURCE_URL"); fi
gog_compat=OFF
if [ "$game" = undertale ]; then
    gog_args=(--build-flag)
    if [ -n "$data_dir" ]; then gog_args+=(--data "$(native_path "$data_dir")"); fi
    gog_compat=$("${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/gog_smart.py")" "${gog_args[@]}" | tr -d '\r')
fi

if [ "$flowcode" = ON ]; then
    if [ "$game" != deltarune ] || [ "$target" != n3ds ] || [ "$roomplan_rec" = ON ]; then
        echo "flowcode=lazy requires normal DELTARUNE New (no recorder)" >&2; exit 2
    fi
    if [ -z "$flowcode_plan" ]; then
        flowcode_plan="$repo/build/roomplan-deltarune/chapter5-flowcode.bin"
        flowcode_args=(--record "$(native_path "$repo/tools/roomplan/records/deltarune-steam-0.0.253-chapter5.json")" --record "$(native_path "$repo/tools/roomplan/flowcode-records/chapter5-flowery.json")" --record "$(native_path "$repo/tools/roomplan/flowcode-records/chapter5-dr44.json")" --out "$(native_path "$flowcode_plan")")
        if [ -n "$data_dir" ]; then flowcode_args+=(--data "$(native_path "$data_dir")"); fi
        "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/roomplan/build_flowcode.py")" "${flowcode_args[@]}"
    fi
    # MSYS CMake needs its own absolute path, including for a native Windows
    # flowcode-plan path supplied by a detached Python driver.
    if command -v cygpath > /dev/null 2>&1; then flowcode_plan=$(cygpath -au "$flowcode_plan"); fi
    if [ ! -f "$flowcode_plan" ]; then echo "Flowcode plan not found: $flowcode_plan" >&2; exit 2; fi
fi

cmake -S "$here" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_INTERMEDIATE_DIR_STRATEGY=FULL \
    ${prefix_map[@]+"${prefix_map[@]}"} \
    ${release_source[@]+"${release_source[@]}"} \
    -DCTRPORT_PYTHON_EXECUTABLE="$python_executable" \
    -DCMAKE_C_COMPILER_LAUNCHER="$launcher" \
    -DCTRPORT_LAUNCH_DISPATCH="$launch_dispatch" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
    -DCMAKE_BUILD_TYPE="$config" \
    -DCTRPORT_FLOWCODE="$flowcode" -DCTRPORT_FLOWCODE_PLAN="$flowcode_plan" \
    -DCTRPORT_GAME="$game" \
    -DCTRPORT_GOG_COMPAT="$gog_compat" \
    -DCTRPORT_DATA_SIZE="$data_size" \
    -DCTRPORT_TARGET="$target" \
    -DCTRPORT_PROFILE="$profile" \
    -DCTRPORT_DEBUG_RECORD_FAIL_AFTER="$record_fail_after" \
    -DCTRPORT_SLOT_DIAGNOSTICS="$slotdiag" \
    -DCTRPORT_LET_GAME_END="$gameend" \
    -DCTRPORT_STATE_DUMP_FRAME="$statedump" \
    -DCTRPORT_AUDIO_CAPTURE="$audiocap" \
    -DCTRPORT_AUDIO_PCM_HASH="$pcmhash" \
    -DCTRPORT_LOADAUDIO_CONTROL="$loadaudio_control" \
    -DCTRPORT_AUDIO_METRICS="$audiometrics" \
    -DCTRPORT_ADPCM_PROBE="$adpcmprobe" \
    -DCTRPORT_ADPCM_SFX="$adpcmsfx" \
    -DCTRPORT_ADPCM_MUSIC="$adpcmmusic" \
    -DCTRPORT_ADPCM_TEST_UNKNOWN_MTIME="$adpcmtestmtime" \
    -DCTRPORT_ADPCM_SFXPACK="$adpcmpack" \
    -DCTRPORT_ADPCM_FAULT="$adpcmfault" \
    -DCTRPORT_SURFACE_CENSUS="$surfcensus" -DCTRPORT_VRAM_CENSUS="$vramcensus" \
    -DCTRPORT_OFFDRAW="$offdraw" \
    -DCTRPORT_HALF_CENSUS="$halfcensus" -DCTRPORT_HALFART_FILE="$halfart_file" \
    -DCTRPORT_PKLIN_DIAG="$pklindiag" -DCTRPORT_PKLIN_STRESS="$pklinstress" \
    -DCTRPORT_NATSURF6_WIDE="$natsurf6wide" \
    -DCTRPORT_KNIGHTNEW_HISTORY="$knightnew_history" \
    -DCTRPORT_GPU_KICK="$gpu_kick" -DCTRPORT_GPU_KICK_MODE="$gpu_kick_mode" -DCTRPORT_GPU_KICK_TRACE="$gpu_kick_trace" \
    -DCTRPORT_KNIGHTGPU_TEXENV="$knightgpu_texenv" \
    -DCTRPORT_KNIGHTGPU_COMPACT="$knightgpu_compact" -DCTRPORT_KNIGHTGPU_COMPACT_FORCE="$knightgpu_compact_force" \
    -DCTRPORT_SHAREBUF="$sharebuf" -DCTRPORT_SHAREBUF_TEST="$sharebuf_test" -DCTRPORT_SHAREBUF_FULLTEST="$sharebuf_fulltest" -DCTRPORT_SHAREBUF_G2="$sharebuf_g2" \
    -DCTRPORT_KNIGHTGPU_DSPE="$knightgpu_dspe" \
    -DCTRPORT_REPACK_TEST_DIR="$repack_test" \
    -DCTRPORT_HEAP_PROFILE="$heap" \
    -DCTRPORT_MEMORY_CAP_KB="$memcap" \
    -DCTRPORT_DR_OLD_CIA80_TILES="$cia80" \
    -DCTRPORT_AOT_DIR="$aot_dir" \
    -DCTRPORT_PIXGRID_RUNS="$pixgrid_runs" \
    -DCTRPORT_LOADSCREEN_ART="$loadscreen_art" \
    -DCTRPORT_CRISPART_DIR="$crispart_dir" \
    -DCTRPORT_AOT_STRESS="$aot_stress" \
    -DCTRPORT_VIDEO_OPENH264="$openh264" \
    -DCTRPORT_REACH_GATE="$reachgate" \
    -DCTRPORT_PLAYER_SHOTS="$shots" -DCTRPORT_ROSALINA_PROBE="$rosaprobe" -DCTRPORT_ROSALOW_TEST="$rosalow" -DCTRPORT_UTFADE_PROBE="$utfadeprobe" -DCTRPORT_BS_PLACESWITCH="$bsplace" -DCTRPORT_LABCRASH="$labcrash" -DCTRPORT_BUBBLE_CHECK="$bubblecheck" -DCTRPORT_TOUCH_TEST="$touchtest" \
    -DCTRPORT_TOUCH_QUEUE="$touchqueue" \
    -DCTRPORT_TOUCH_NAME_TRACE="$nametrace" \
    -DCTRPORT_PACER_OBSERVE="$pacer_observe" -DCTRPORT_PACER_R2R3A="$pacer_rule" \
    -DCTRPORT_PACER_CSV_DIAG="$pacer_csv_diag" \
    -DCTRPORT_ROOMPLAN_FILE="$roomplan_file" -DCTRPORT_SFXPLAN_FILE="$sfxplan_file" -DCTRPORT_LAYOUT_SCENES_FILE="$layout_scenes_file" -DCTRPORT_PHASEPLAN_FILE="$phaseplan_file" -DCTRPORT_ROOMPLAN_DIR="$roomplan_dir" -DCTRPORT_ROOMPLAN_RECORDER="$roomplan_rec" \
    -DCTRPORT_PHASE_LAYOUT="$phase_layout" \
    -DCTRPORT_HOMEAUDIO_PROBE="$homeaudioprobe" -DCTRPORT_SPIKEREC="$spikerec" -DCTRPORT_TITANDIAG="$titandiag" -DCTRPORT_FREEWINS="$freewins" \
    -DCTRPORT_UTTC_MODE="$uttc_mode" -DCTRPORT_DPRANK_MODE="$dprank_mode" -DCTRPORT_TEST_LATE_PAGE="$testpage" \
    -DCTRPORT_BENCHFIX_PROBE="$bfprobe" -DCTRPORT_PACER_ARENA_LOG="$arenalog" -DCTRPORT_FRAMETRACE="$frametrace" -DCTRPORT_ASG_PROBE="$asgprobe" -DCTRPORT_FLOWSTREAM_DIAG="$flowdiag" \
    -DCTRPORT_EXITTRAIL="$exittrail" -DCTRPORT_BALLAST="$ballast" -DCTRPORT_WRITERFRONT_MODE="$writerfront" \
    -DCTRPORT_DIMLIGHT_DIAG="$dimlightdiag" \
    -DCTRPORT_FLOWERHW_TPROF="$towerprof" -DCTRPORT_FLOWERHW_FORCE="$towerforce" \
    -DCTRPORT_FLOWERHW_L2="$ppl2" -DCTRPORT_FLOWERHW_PPFORCE="$ppforce" \
    "${input_cmake[@]}" \
    -DCTRPORT_TYPED_CENSUS="$typed_census" -DCTRPORT_TYPED_FRAMELOG="$typed_framelog" -DCTRPORT_SPDCOUNT="$spdcount" \
    -DCTRPORT_TYPED_AOT="$typed" -DCTRPORT_TYPED_STATS="$typed_stats" -DCTRPORT_TYPED_STRESS="$typed_stress" \
    ${aot_region:+-DCTRPORT_AOT_REGION_MB="$aot_region"}
if [ -n "$launcher" ]; then
    : > "$build/compile_cache.log"
fi
: > "$build/launch_dispatch.log"
# The renderer's per-font and per-item Smart smoothing verdicts, worked out from the game data here instead of at each
# font's or item's first draw on the console (tools/aot/fontverdict.py, tools/aot/itemverdict.py):
# romfs/fonts/<data.win folder>.dtfv and .dtiv. Without game data or Python the console analyses them itself, as before.
if [ -n "${python:-}" ]; then
    echo "Builder phase: Preparing graphics"
    font_args=(--game "$game" --target "$target" --romfs "$(native_path "$build/romfs")")
    if [ -n "$data_dir" ]; then font_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/fontverdict.py")" "${font_args[@]}" || {
        echo "Could not prepare graphics. Check the build log." >&2
        exit 1
    }
fi
# The options menu's Credits page (source/ctr_legal.c): the game's own credits, read from the player's data, then this
# port's (docs/release/credits-port.txt), in romfs/credits.txt (tools/aot/credits.py). Without game data or Python the
# page has this port's credits only, or none.
if [ -n "${python:-}" ]; then
    credits_args=(--game "$game" --romfs "$(native_path "$build/romfs")")
    if [ -n "$data_dir" ]; then credits_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/credits.py")" "${credits_args[@]}" ||
        echo "credits: tools/aot/credits.py failed; the Credits page is empty in this build" >&2
fi
# Crisp text's glyph pack (source/z_smalltext.c): the derived native glyphs of the player's fonts
# (tools/aot/stpack.py, plain Python, so the release builder makes it too; byte-identical to the numpy reference
# tools/analysis/smalltext_pack.py) in romfs/fonts/<data.win folder>.dtst. The runtime reads it only when a Crisp key is
# on. UNDERTALE and DELTARUNE New builds make it by default ("smalltext=off" leaves it out; DELTARUNE New reads one pack
# per chapter, z_smalltext_dr.c); DELTARUNE Old, which has no Crisp text code, only with the word "smalltext".
# UNDERTALE's also gets the options menu's Smoothing data beside it (<folder>.dtsm: the Crisp soul, the sprites Bones and
# Numbers keep sharp, the Custom page's previews; tools/aot/stmenu.py). Without it any pack left from an earlier build
# is removed. The program is the same either way; only the RomFS
# differs, so bench_scenes.txt (a dependency of the package) is touched to repackage it when the packs change.
if [ -z "$smalltext" ]; then
    if [ "$game" = undertale ] || [ "$target" = n3ds ]; then smalltext=ON; else smalltext=OFF; fi
fi
st_packs=$(ls "$build/romfs/fonts/"*.dts[tmb] 2>/dev/null || true)
if [ "$smalltext" = ON ] && [ -n "${python:-}" ]; then
    st_args=(--game "$game" --romfs "$(native_path "$build/romfs")")
    if [ -n "$data_dir" ]; then st_args+=(--data "$(native_path "$data_dir")"); fi
    st_before=$( (cat "$build/romfs/fonts/"*.dts[tmb] 2>/dev/null || true) | cksum)
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/stpack.py")" "${st_args[@]}" ||
        echo "smalltext: tools/aot/stpack.py failed; Crisp text has no glyphs in this build" >&2
    if [ "$st_before" != "$( (cat "$build/romfs/fonts/"*.dts[tmb] 2>/dev/null || true) | cksum)" ]; then
        touch "$build/romfs/bench_scenes.txt"
    fi
elif [ -n "$st_packs" ]; then
    rm -f "$build/romfs/fonts/"*.dts[tmb]
    touch "$build/romfs/bench_scenes.txt"
fi
# The early destroy of bullets past the view (source/z_offkill.c, UNDERTALE only): the objects the PC code proves are
# done with once they leave the view, resolved by name against the player's data (tools/aot/offkill.py), in
# romfs/offkill.bin. Without game data or Python there is no file and the runtime destroys nothing early.
if [ "$game" = undertale ] && [ -n "${python:-}" ]; then
    offkill_args=(--game "$game" --romfs "$(native_path "$build/romfs")")
    if [ -n "$data_dir" ]; then offkill_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/offkill.py")" "${offkill_args[@]}" ||
        echo "offkill: tools/aot/offkill.py failed; no early destroy in this build" >&2
fi
# The dialogue script's case table (source/z_textcase.c, UNDERTALE Old only): its switch's constants sorted, with each
# case's address, from the player's data (tools/aot/textcase.py), in romfs/textcase.bin. Without game data or Python
# there is no file and the script's switch runs its compare chain as before.
if [ "$game" = undertale ] && [ "$target" = o3ds ] && [ -n "${python:-}" ]; then
    textcase_args=(--game "$game" --romfs "$(native_path "$build/romfs")")
    if [ -n "$data_dir" ]; then textcase_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -X utf8 -I "$(native_path "$repo/tools/aot/textcase.py")" "${textcase_args[@]}" ||
        echo "textcase: tools/aot/textcase.py failed; no case table in this build" >&2
fi
# Every RomFS producer has finished. Repackage both formats even when a file was removed or was
# generated after CMake configured (fonts, chapter selection and room plans).
cmake -E touch "$build/romfs-content.stamp"
echo "Builder phase: Compiling"
cmake --build "$build" -j "$(nproc)"
cmake --build "$build" --target cia
if [ -n "$launcher" ]; then
    "$cc_python" -X utf8 -I -S "$(native_path "$repo/tools/buildcache/compile_cache.py")" --summary "$(native_path "$build")/compile_cache.log"
fi
if [ -s "$build/launch_dispatch.log" ]; then
    "${python[@]}" -X utf8 -I -S "$(native_path "$repo/tools/buildcache/launch_dispatch.py")" --summary "$(native_path "$build")/launch_dispatch.log"
fi

package_stem=$(<"$build/package-stem.txt")
echo "built $build/$package_stem.3dsx${aot_dir:+ (with AOT from $aot_dir)}${aot_skip:+ (WITHOUT AOT)}"
if [ -f "$build/$package_stem.cia" ]; then
    echo "built $build/$package_stem.cia"
fi
if [ -n "$aot_skip" ]; then
    aot_warn built
fi
if [ -n "$openh264_skip" ]; then
    openh264_warn built
fi

if [ "$cia80" = ON ]; then
    echo "CIA80 palette/tiles: install the .cia; use the ordinary build for HBL."
fi
