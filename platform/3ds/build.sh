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
#                              [offdraw] (offdraw: instance census and offdraw.txt kill/hash-skip experiments; separate build directory)
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
#                              [layout=<audited absolute linker-script path>]
#                              [inputperiod=<0|8|16>] [inputstall=<ms>] (input sampler diagnostics; separate build directory)
#                              [spikerec] (spike recorder for playtests: every frame that misses its slot, written to
#                              spikes.txt at room changes; source/z_spikerec.c; separate build directory, never a release)
#                              [freewins] (main-loop segment timers and off switches, freewins; source/z_freewins.c;
#                              separate build directory, never a release)
#                              [bfprobe] (main-thread waits in a sweep, [bfprobe] log lines; ctr_benchmark.c's end;
#                              separate build directory, never a release)
#                              [asgprobe] (UNDERTALE test only: ARM11 counters and per-frame drawSprite ticks, source/ctr_asgprobe.c;
#                              asgframes.txt / asgtime.txt / asgdump.txt in the game folder; separate build directory, never a release)
#                              [frametrace] (every frame of a bench window: phases, pacer, GPU waits, SD calls, decodes,
#                              tile loads, written to frametrace.txt after the window; source/z_frametrace.c;
#                              separate build directory, never a release)
#                              [flowdiag] (UNDERTALE: every draw-time tile load and floor guard decision with free
#                              linear and heap memory and plan membership; source/z_flowdiag.c; flowstream round 3;
#                              separate build directory, never a release)
#                              [exittrail] (the exit's steps after the last log line, each in labboot.txt with its thread;
#                              source/z_exittrail.c; o2DS kernel panic diagnosis, separate build directory, never a release)
#                              [nowriterfront|writerfront-fallback|writerfront-count] (UNDERTALE Old: the dialogue writer's
#                              hand front off, every Draw through its fallback, or front/fallback counts in the log;
#                              source/z_writerfront.c; separate build directories, test builds only)
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
# "bubblecheck" (UNDERTALE, with smalltext) logs every Crisp line, any font, that comes closer to its bubble's or the
# battle box's right edge than PC's ink allows, leaves the screen where PC's stays on it, or comes closer to the line
# below it than PC's (source/z_smalltext.c; tools/analysis/bubblecheck_report.py makes the table), for tours and
# benches, and builds into build/3ds-<game>-<target>-bubblecheck/; never a Release build.
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
# run as typed C code. "typed=<list>" takes any other list. "typed=off" builds without them, generating into
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
audiometrics=OFF
adpcmprobe=OFF
adpcmsfx=OFF
adpcmmusic=OFF
adpcmtestmtime=OFF
adpcmpack=     # ON, OFF, or empty: the default (ON in UNDERTALE Old, OFF elsewhere)
adpcmfault=0   # TEST ONLY: 1 tail, 2 seam, 3 pool, 4 ring (the allocation that fails)
audio_convert=OFF
surfcensus=OFF
offdraw=OFF
halfcensus=OFF
pklindiag=OFF
pklinstress=OFF
halfart=        # point, box or balanced: the half-resolution art prototype (flowhalf)
natsurf6wide=OFF
cia80=OFF
knightnew_history=   # ON, OFF, or empty: the default (ON in DELTARUNE New, no effect elsewhere)
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
bubblecheck=OFF
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
freewins=OFF
bfprobe=OFF
frametrace=OFF
asgprobe=OFF
flowdiag=OFF
exittrail=OFF
writerfront=ON
repack_test= # test builds: a folder of <data.win folder>/ repack files for the RomFS (ctr_repack.inc)
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
        bubblecheck) bubblecheck=ON ;;
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
        freewins) freewins=ON ;;
        bfprobe) bfprobe=ON ;;
        frametrace) frametrace=ON ;;
        asgprobe) asgprobe=ON ;;
        flowdiag) flowdiag=ON ;;
        exittrail) exittrail=ON ;;
        nowriterfront) writerfront=OFF ;;
        writerfront-fallback) writerfront=FALLBACK ;;
        writerfront-count) writerfront=COUNT ;;
        typed-census) typed_census=ON ;;
        typed-framelog) typed_framelog=ON ;;
        typed) typed=ON ;;
        typed=off) typed=OFF ;;
        typed=*) typed=ON; typed_list=${arg#typed=} ;;
        typed-stats) typed_stats=ON ;;
        typed-stress=*) typed_stress=${arg#typed-stress=} ;;
        late-numeric=*) late_numeric_entry=${arg#late-numeric=} ;;
        aot-region=*) aot_region=${arg#aot-region=} ;;
        repacktest=*) repack_test=${arg#repacktest=} ;;
        *)
            echo "unknown argument '$arg'" >&2
            echo "usage: $0 [undertale|deltarune] [n3ds|o3ds] [Release|RelWithDebInfo|Debug|MinSizeRel] [profile] [heap] [aot|aot=chapters|aot=single|aot=off|aot=<dir>] [data=<game folder>] [aot-stress] [memcap=<KB>] [openh264=off] [reachgate] [shots] [layout=<script>] [typed|typed=off|typed=all|typed=<list>] [typed-stats] [typed-stress=<n>]" >&2
            exit 2
            ;;
    esac
done
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
        if [ "$game" = deltarune ]; then typed_list=all; else typed_list="$repo/tools/aot/selection/typed-$game.txt"; fi
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
if [ "$bubblecheck" = ON ]; then
    build="$build-bubblecheck"
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
if [ "$freewins" = ON ]; then
    build="$build-freewins"
fi
if [ "$bfprobe" = ON ]; then
    build="$build-bfprobe"
fi
if [ "$asgprobe" = ON ]; then
    build="$build-asgprobe"
fi
if [ "$frametrace" = ON ]; then
    build="$build-frametrace"
fi
if [ "$exittrail" = ON ]; then
    build="$build-exittrail"
fi
if [ "$flowdiag" = ON ]; then
    build="$build-flowdiag"
fi
case "$writerfront" in
    OFF) build="$build-nowriterfront" ;;
    FALLBACK) build="$build-writerfront-fallback" ;;
    COUNT) build="$build-writerfront-count" ;;
esac
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
python_executable=$(release_python_executable)
# prepare.py checks the same version table before using even an up-to-date AOT
# result. Check non-AOT/custom-code builds too, without generating anything.
if [ "$aot_mode" != auto ] && [ -n "${python:-}" ]; then
    version_args=(--check-only --game "$game" --target "$target" --out "$(native_path "$repo/build/data-versions-$game")")
    if [ -n "$data_dir" ]; then version_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -I "$(native_path "$repo/tools/aot/prepare.py")" "${version_args[@]}"
    rc=$?
    set -e
    if [ "$rc" != 0 ] && [ "$rc" != 3 ]; then exit "$rc"; fi
fi
if [ "$aot_mode" = auto ]; then
    echo "AOT: using ${python[*]} ($(command -v "${python[0]}"))"
    gen="$repo/build/aot-$game-$target${chapters:+-chapters}$typed_suffix$late_numeric_suffix"
    prep_args=(--game "$game" --target "$target" --out "$(native_path "$gen")" ${chapters:+--chapters})
    if [ -n "$late_numeric_entry" ]; then prep_args+=(--late-numeric "$late_numeric_entry"); fi
    if [ "$typed" = ON ]; then
        if [ "$typed_list" = all ]; then prep_args+=(--typed all); else prep_args+=(--typed "$(native_path "$typed_list")"); fi
    fi
    if [ -n "$data_dir" ]; then
        prep_args+=(--data "$(native_path "$data_dir")")
    fi
    set +e
    "${python[@]}" -I "$(native_path "$repo/tools/aot/prepare.py")" "${prep_args[@]}"
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
    "${python[@]}" -I "$(native_path "$repo/tools/aot/pixgrid.py")" "${grid_args[@]}"
    pixgrid_runs=$grid
    prompt_args=(--out "$(native_path "$repo/build/pixgrid-undertale")")
    if [ -n "$data_dir" ]; then prompt_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -I "$(native_path "$repo/tools/aot/btnprompt_build.py")" "${prompt_args[@]}"
fi

# The loading screens' art (z_loadscreen.c), cut from the player's own files (tools/aot/loadscreen_art.py) into the
# RomFS: both games' first time setup line, and DELTARUNE's loading text and Chapter 3's dog. Without known game files
# the loading screen is a bar alone.
loadscreen_art=
if [ -n "${python:-}" ]; then
    art="$repo/build/loadscreen-$game/loadscreen.bin"
    art_args=(--out "$(native_path "$art")" --game "$game")
    if [ -n "$data_dir" ]; then art_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -I "$(native_path "$repo/tools/aot/loadscreen_art.py")" "${art_args[@]}" || true
    if [ -f "$art" ]; then loadscreen_art=$art; fi
fi

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
    elif ! crispart_check=$("${python[@]}" -I "$(native_path "$repo/tools/analysis/crispart_pack.py")" --check-tools 2>&1); then
        crispart_why="tools/analysis/crispart_pack.py can't import its tools ($(printf '%s\n' "$crispart_check" | tr -d '\r' | tail -n 1))"
    else
        crispart_out="$repo/build/crispart-deltarune"
        crispart_args=(--out "$(native_path "$crispart_out")")
        if [ -n "$data_dir" ]; then crispart_args+=(--data "$(native_path "$data_dir")"); fi
        if "${python[@]}" -I "$(native_path "$repo/tools/analysis/crispart_pack.py")" "${crispart_args[@]}"; then
            crispart_dir=$crispart_out
        else
            crispart_why="tools/analysis/crispart_pack.py failed (its output is above)"
        fi
    fi
    if [ -n "$crispart_why" ]; then
        release_missing_derivation "Crisp art" "$crispart_why" || exit 3
    fi
fi

# The half-resolution art prototype (halfart=point|box|balanced, flowhalf; tools/analysis/halfart_pack.py): the
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
    "${python[@]}" -I "$(native_path "$repo/tools/analysis/halfart_pack.py")" "${ha_args[@]}"
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
    plan_args=(--game "$game" --out "$(native_path "$plan_out")")
    if [ -n "$data_dir" ]; then plan_args+=(--data "$(native_path "$data_dir")"); fi
    if [ -n "$halfart_remap" ]; then plan_args+=(--halfart "$(native_path "$halfart_remap")"); fi
    set +e
    "${python[@]}" -I "$(native_path "$repo/tools/roomplan/build_plan.py")" "${plan_args[@]}"
    rc=$?
    set -e
    if [ "$rc" = 0 ] && [ "$game" = deltarune ]; then
        roomplan_dir=$plan_out
        # drresid: the fights' track plan (tools/roomplan/build_tracks.py), New 3DS only; exit 3 builds without one.
        if [ "$target" = n3ds ] && [ "$roomplan_rec" != ON ]; then
            track_args=(--out "$(native_path "$plan_out")")
            if [ -n "$data_dir" ]; then track_args+=(--data "$(native_path "$data_dir")"); fi
            set +e
            "${python[@]}" -I "$(native_path "$repo/tools/roomplan/build_tracks.py")" "${track_args[@]}"
            rc=$?
            set -e
            if [ "$rc" = 2 ]; then
                echo "Track plan: tools/roomplan/build_tracks.py failed" >&2
                exit 2
            fi
            rc=0
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
# leaves it out too. DELTARUNE only (ruling, 2026-10-25).
phaseplan_file=
if [ "$game" = deltarune ] && [ "$roomplan" = ON ] && [ -n "${python:-}" ]; then
    phase_out="$repo/build/roomplan-$game/phases.dtph"
    phase_args=(--game "$game" --out "$(native_path "$phase_out")")
    if [ -n "$data_dir" ]; then phase_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -I "$(native_path "$repo/tools/roomplan/build_phases.py")" "${phase_args[@]}"
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
    sfx_args=(--out "$(native_path "$sfx_out")")
    if [ -n "$data_dir" ]; then sfx_args+=(--data "$(native_path "$data_dir")"); fi
    set +e
    "${python[@]}" -I "$(native_path "$repo/tools/roomplan/build_sfxplan.py")" "${sfx_args[@]}"
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

if [ "$audio_convert" = ON ]; then
    if [ -z "$data_dir" ] || [ -z "${python:-}" ]; then
        echo "audio-convert needs the player's data folder and the builder's Python" >&2
        exit 2
    fi
    audio_out="$repo/build/audio-adpcm-$game"
    "${python[@]}" -I "$(native_path "$repo/tools/audio/prepare.py")" \
        --data "$(native_path "$data_dir")" --out "$(native_path "$audio_out")"
    audio_sd_out="$repo/build/audio-sd-$game/audio-adpcm"
    "${python[@]}" -I "$(native_path "$repo/tools/audio/stage.py")" \
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
        "${python[@]}" "$repo/tools/aot/chapter_manifest.py" "${chapter_args[@]}"
    else
        rm -f "$build/romfs/chapters.txt"
    fi
fi

# The compiler cache (tools/buildcache/compile_cache.py): every C compile goes through it unless
# CTRPORT_CCACHE=0. Shared by every worktree of the repository, in <main checkout>/scratch/ccache unless
# $CTRPORT_CCACHE_DIR says otherwise. Reuse the already qualified interpreter.
launcher=
if [ "${CTRPORT_CCACHE:-1}" != 0 ]; then
    cc_python=$("${python[@]}" -c 'import sys; print(sys.executable)' | tr -d '\r')
    if command -v cygpath > /dev/null 2>&1; then cc_python=$(cygpath -u "$cc_python"); fi
    if [ -n "$cc_python" ]; then
        cache_dir=${CTRPORT_CCACHE_DIR:-}
        if [ -z "$cache_dir" ]; then
            # The main checkout is the parent of the repository's common .git, which every worktree shares.
            main_checkout=$repo
            common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2> /dev/null | tr -d '
')
            if [ "${common##*/}" = .git ]; then main_checkout=${common%/.git}; fi
            cache_dir="$main_checkout/scratch/ccache"
        fi
        mkdir -p "$cache_dir"
        launcher="$cc_python;-I;-S;$(native_path "$repo/tools/buildcache/compile_cache.py");--cache-dir=$(native_path "$cache_dir")"
        export CTRPORT_CCACHE_LOG="$(native_path "$build")/compile_cache.log"
    else
        echo "compile cache: no Python 3.8 or later found (tried python3, python and py -3); building uncached" >&2
    fi
fi

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
    gog_compat=$("${python[@]}" -I "$(native_path "$repo/tools/aot/gog_smart.py")" "${gog_args[@]}" | tr -d '\r')
fi

if [ "$flowcode" = ON ]; then
    if [ "$game" != deltarune ] || [ "$target" != n3ds ] || [ "$roomplan_rec" = ON ]; then
        echo "flowcode=lazy requires normal DELTARUNE New (no recorder)" >&2; exit 2
    fi
    if [ -z "$flowcode_plan" ]; then
        flowcode_plan="$repo/build/roomplan-deltarune/chapter5-flowcode.bin"
        flowcode_args=(--record "$(native_path "$repo/tools/roomplan/records/deltarune-steam-0.0.253-chapter5.json")" --record "$(native_path "$repo/tools/roomplan/flowcode-records/chapter5-flowery.json")" --record "$(native_path "$repo/tools/roomplan/flowcode-records/chapter5-dr44.json")" --out "$(native_path "$flowcode_plan")")
        if [ -n "$data_dir" ]; then flowcode_args+=(--data "$(native_path "$data_dir")"); fi
        "${python[@]}" -I "$(native_path "$repo/tools/roomplan/build_flowcode.py")" "${flowcode_args[@]}"
    fi
    # MSYS CMake needs its own absolute path, including for a native Windows
    # flowcode-plan path supplied by a detached Python driver.
    if command -v cygpath > /dev/null 2>&1; then flowcode_plan=$(cygpath -au "$flowcode_plan"); fi
    if [ ! -f "$flowcode_plan" ]; then echo "Flowcode plan not found: $flowcode_plan" >&2; exit 2; fi
fi

cmake -S "$here" -B "$build" -G "Unix Makefiles" \
    ${prefix_map[@]+"${prefix_map[@]}"} \
    ${release_source[@]+"${release_source[@]}"} \
    -DCTRPORT_PYTHON_EXECUTABLE="$python_executable" \
    -DCMAKE_C_COMPILER_LAUNCHER="$launcher" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
    -DCMAKE_BUILD_TYPE="$config" \
    -DCTRPORT_FLOWCODE="$flowcode" -DCTRPORT_FLOWCODE_PLAN="$flowcode_plan" \
    -DCTRPORT_GAME="$game" \
    -DCTRPORT_GOG_COMPAT="$gog_compat" \
    -DCTRPORT_TARGET="$target" \
    -DCTRPORT_PROFILE="$profile" \
    -DCTRPORT_DEBUG_RECORD_FAIL_AFTER="$record_fail_after" \
    -DCTRPORT_SLOT_DIAGNOSTICS="$slotdiag" \
    -DCTRPORT_LET_GAME_END="$gameend" \
    -DCTRPORT_STATE_DUMP_FRAME="$statedump" \
    -DCTRPORT_AUDIO_CAPTURE="$audiocap" \
    -DCTRPORT_AUDIO_PCM_HASH="$pcmhash" \
    -DCTRPORT_AUDIO_METRICS="$audiometrics" \
    -DCTRPORT_ADPCM_PROBE="$adpcmprobe" \
    -DCTRPORT_ADPCM_SFX="$adpcmsfx" \
    -DCTRPORT_ADPCM_MUSIC="$adpcmmusic" \
    -DCTRPORT_ADPCM_TEST_UNKNOWN_MTIME="$adpcmtestmtime" \
    -DCTRPORT_ADPCM_SFXPACK="$adpcmpack" \
    -DCTRPORT_ADPCM_FAULT="$adpcmfault" \
    -DCTRPORT_SURFACE_CENSUS="$surfcensus" \
    -DCTRPORT_OFFDRAW="$offdraw" \
    -DCTRPORT_HALF_CENSUS="$halfcensus" -DCTRPORT_HALFART_FILE="$halfart_file" \
    -DCTRPORT_PKLIN_DIAG="$pklindiag" -DCTRPORT_PKLIN_STRESS="$pklinstress" \
    -DCTRPORT_NATSURF6_WIDE="$natsurf6wide" \
    -DCTRPORT_KNIGHTNEW_HISTORY="$knightnew_history" \
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
    -DCTRPORT_PLAYER_SHOTS="$shots" -DCTRPORT_ROSALINA_PROBE="$rosaprobe" -DCTRPORT_BUBBLE_CHECK="$bubblecheck" \
    -DCTRPORT_PACER_OBSERVE="$pacer_observe" -DCTRPORT_PACER_R2R3A="$pacer_rule" \
    -DCTRPORT_PACER_CSV_DIAG="$pacer_csv_diag" \
    -DCTRPORT_ROOMPLAN_FILE="$roomplan_file" -DCTRPORT_SFXPLAN_FILE="$sfxplan_file" -DCTRPORT_PHASEPLAN_FILE="$phaseplan_file" -DCTRPORT_ROOMPLAN_DIR="$roomplan_dir" -DCTRPORT_ROOMPLAN_RECORDER="$roomplan_rec" \
    -DCTRPORT_PHASE_LAYOUT="$phase_layout" \
    -DCTRPORT_SPIKEREC="$spikerec" -DCTRPORT_FREEWINS="$freewins" \
    -DCTRPORT_BENCHFIX_PROBE="$bfprobe" -DCTRPORT_FRAMETRACE="$frametrace" -DCTRPORT_ASG_PROBE="$asgprobe" -DCTRPORT_FLOWSTREAM_DIAG="$flowdiag" \
    -DCTRPORT_EXITTRAIL="$exittrail" -DCTRPORT_WRITERFRONT_MODE="$writerfront" \
    "${input_cmake[@]}" \
    -DCTRPORT_TYPED_CENSUS="$typed_census" -DCTRPORT_TYPED_FRAMELOG="$typed_framelog" \
    -DCTRPORT_TYPED_AOT="$typed" -DCTRPORT_TYPED_STATS="$typed_stats" -DCTRPORT_TYPED_STRESS="$typed_stress" \
    ${aot_region:+-DCTRPORT_AOT_REGION_MB="$aot_region"}
if [ -n "$launcher" ]; then
    : > "$build/compile_cache.log"
fi
# The renderer's per-font and per-item Smart smoothing verdicts, worked out from the game data here instead of at each
# font's or item's first draw on the console (tools/aot/fontverdict.py, tools/aot/itemverdict.py):
# romfs/fonts/<data.win folder>.dtfv and .dtiv. Without game data or Python the console analyses them itself, as before.
if [ -n "${python:-}" ]; then
    echo "Builder phase: Preparing graphics"
    font_args=(--game "$game" --target "$target" --romfs "$(native_path "$build/romfs")")
    if [ -n "$data_dir" ]; then font_args+=(--data "$(native_path "$data_dir")"); fi
    "${python[@]}" -I "$(native_path "$repo/tools/aot/fontverdict.py")" "${font_args[@]}" || {
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
    "${python[@]}" -I "$(native_path "$repo/tools/aot/credits.py")" "${credits_args[@]}" ||
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
    "${python[@]}" -I "$(native_path "$repo/tools/aot/stpack.py")" "${st_args[@]}" ||
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
    "${python[@]}" -I "$(native_path "$repo/tools/aot/offkill.py")" "${offkill_args[@]}" ||
        echo "offkill: tools/aot/offkill.py failed; no early destroy in this build" >&2
fi
# Every RomFS producer has finished. Repackage both formats even when a file was removed or was
# generated after CMake configured (fonts, chapter selection and room plans).
cmake -E touch "$build/romfs-content.stamp"
echo "Builder phase: Compiling"
cmake --build "$build" -j "$(nproc)"
cmake --build "$build" --target cia
if [ -n "$launcher" ]; then
    "$cc_python" -I -S "$(native_path "$repo/tools/buildcache/compile_cache.py")" --summary "$(native_path "$build")/compile_cache.log"
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
