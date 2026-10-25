#!/usr/bin/env python3
"""Profile corpus for AOT selection (docs/aot.md, "Byte-budget selection").

Runs YOUR game data in the desktop runner with the GML profiler and writes one profile per bench scene, per room and
per encounter. tools/aot/select.py ranks code entries from them. Nothing here leaves your machine: the profiles name
the game's code entries and count their instructions, so keep them out of the repository like everything else
derived from the game.

usage:
  corpus.py scenes  --game G --data DIR --exe RUNNER --out CORPUS [--frames 3000] [--window 60] [--jobs 4]
  corpus.py rooms   --game G --data DIR --exe RUNNER --out CORPUS [--frames 90] [--part KEY]
  corpus.py battles --game G --data DIR --exe RUNNER --out CORPUS [--first 1] [--last N] [--jobs 4] [--part KEY]
  corpus.py distinct --game G --out CORPUS

  --game G     undertale or deltarune
  --data DIR   the game folder: UNDERTALE's (data.win) or DELTARUNE's (chapterN_windows/data.win); the PC install or
               the copy for the SD card, which has the same layout
  --exe RUNNER the desktop runner built with the GML profiler and without AOT:
               cmake -S engine -B build/desktop-prof -DPLATFORM=cli -DBACKEND=noop -DAUDIO_BACKEND=none
                     -DVIDEO_BACKEND=none -DENABLE_VM_TRACING=OFF -DENABLE_VM_GML_PROFILER=ON
  --out CORPUS where the profiles go: CORPUS/<part>/scenes|rooms|battles/*.log, <part> being the data.win's folder
               (chapter1_windows ... chapter5_windows; "root" for UNDERTALE's data.win)
  --part KEY   only this part (DELTARUNE: chapter3_windows, say)
  --scenes IDS scenes only: comma-separated bench ids, to extend an existing corpus without replacing its profiles
  --bench FILE scenes only: use this bench catalog instead of platform/3ds/bench/<game>.txt

scenes: every scene of platform/3ds/bench/<game>.txt, 3,000 frames each, profiled after "Bench: measuring".
rooms:  every room of each data.win in one run (room_goto, --frames frames each, a log marker per room), restarting after
        a room that ends the run; the profiler windows are then split by room.
battles: one run per encounter id (DELTARUNE: scr_battle, or obj_encounterbasic in Chapter 1; UNDERTALE:
        global.battlegroup), the party's HP held, Z and X mashed, 300 frames profiled from 90 frames in. Then
        "distinct" keeps one run per distinct set of entries (and, for DELTARUNE, only runs with enemy code) in
        battles/kept.txt.
"""
import concurrent.futures as cf
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gmlaot  # noqa: E402

DR_ARGS = "-game data.win launcher switch_-1 returning_0"
# UNDERTALE: a save in the Ruins (as the "ruins" bench scene) and one late in the game for battles (as "undyne").
UT_SAVE_EARLY = ('file file0 lines "Frisk" 1 99 0 10 0 10 0 4 0 0 0 1 201 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 4 512*0 3 1 1 1 0 6 0')
UT_SAVE_LATE = ('file file0 lines "Frisk" 15 99 0 38 0 13 0 4 0 200 0 1 201 1 0 1 0 1 0 1 0 1 0 0 0 0 0 3 4 512*0 120 1 1 1 0 '
                '134 108000')
UT_BOOT = ["restart", "waitroom room_introstory", "call scr_load"]

# How a room sweep starts each DELTARUNE chapter: a state from which room_goto works.
DR_ROOM_BOOT = {
    1: ["waitroom PLACE_CONTACT"],
    2: ["waitroom room_intro_ch2", "set global.char[0] 1", "set global.char[1] 2", "set global.char[2] 3"],
    3: ["wait 30", "call scr_gamestart", "call scr_gamestart_chapter_override", "set global.char[0] 1", "set global.char[1] 2",
        "set global.char[2] 3"],
    4: ["wait 30"],
    5: ["wait 30"],
}
# Where a DELTARUNE encounter sweep starts each battle from.
DR_BATTLE_SETUP = {
    1: ["waitroom PLACE_CONTACT", "set global.darkzone 1", "goto room_field2", "wait 10"],
    2: ["waitroom room_intro_ch2", "set global.darkzone 1", "set global.plot 250", "goto room_dw_castle_area_2_transformed", "wait 10"],
    3: ["wait 30", "call scr_gamestart", "call scr_gamestart_chapter_override", "set global.darkzone 1", "set global.plot 330",
        "goto room_dw_snow_zone", "wait 10"],
    4: ["wait 30", "set global.darkzone 1", "goto room_dw_churchc_pretitan", "destroy obj_ch4_DCC01", "destroy obj_dw_churchc_pretitan",
        "wait 5"],
    5: ["wait 30", "set global.darkzone 1", "goto room_dw_fcastle_flowery", "wait 10"],
}
ROOM_SKIP = re.compile(r"^(room_gms_debug|PLACE_|ROOM_INITIALIZE|room_intro|room_legend|room_ed|room_gameover|room_shop|"
                       r"room_battle|room_start|room_end|room_credits)", re.I)


def parts(game, data):
    """[(part key, data.win path, chapter number or 0)] for the game's data.wins that exist."""
    if game == "undertale":
        p = os.path.join(data, "data.win")
        return [("root", p, 0)] if os.path.isfile(p) else []
    out = []
    for ch in range(1, 10):
        p = os.path.join(data, "chapter%d_windows" % ch, "data.win")
        if os.path.isfile(p):
            out.append(("chapter%d_windows" % ch, p, ch))
    return out


def room_names(path):
    d = open(path, "rb").read()
    pos, end = 8, 8 + struct.unpack_from("<I", d, 4)[0]
    while pos + 8 <= end:
        name, size = d[pos:pos + 4], struct.unpack_from("<I", d, pos + 4)[0]
        if name == b"ROOM":
            start = pos + 8
            out = []
            for i in range(struct.unpack_from("<I", d, start)[0]):
                p = struct.unpack_from("<I", d, start + 4 + 4 * i)[0]
                sp = struct.unpack_from("<I", d, p)[0]
                out.append(d[sp:d.index(0, sp)].decode("utf-8", "replace"))
            return out
        pos += 8 + size
    return []


def run_runner(o, data_win, ch, script, scene, frames, window, log, save, timeout):
    os.makedirs(save, exist_ok=True)
    empty = os.path.join(o.out, "empty.json")
    if not os.path.exists(empty):
        open(empty, "w").write("{}\n")
    # The desktop runner's game-facing paths must use forward slashes, including paths os.path.join builds on Windows.
    # Backslashes load data.win but leave the Chapter 1 choice writer unable to reach its choice control sequence.
    data_win = data_win.replace("\\", "/")
    cmd = [o.exe, data_win, "--headless", "--disable-log-colours", "--save-folder", save, "--playback-inputs", empty,
           "--exit-at-frame", str(frames), "--seed", "1", "--bench-script", script, "--bench-scene", scene,
           "--profile-gml-scripts", str(window)]
    if ch:
        cmd[2:2] = ["--game-args", DR_ARGS]
    with open(log, "w") as f:
        try:
            return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout,
                                  env=dict(os.environ, BS_PROFILE_TOP="0")).returncode
        except subprocess.TimeoutExpired:
            return "timeout"


def write_script(path, lines):
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def bench_scenes(game, bench=None):
    """[(scene id, chapter or 0)] of platform/3ds/bench/<game>.txt, helpers (ids starting with _) left out."""
    out, cur = [], None
    for l in open(bench or os.path.join(REPO, "platform", "3ds", "bench", game + ".txt"), encoding="utf-8"):
        m = re.match(r"scene (\S+)", l)
        if m:
            cur = m.group(1)
            if not cur.startswith("_"):
                out.append([cur, 0])
            continue
        m = re.match(r"chapter (\d+)", l)
        if m and out and out[-1][0] == cur:
            out[-1][1] = int(m.group(1))
    return [tuple(x) for x in out]


def cmd_scenes(o):
    ps = {ch: (key, p) for key, p, ch in parts(o.game, o.data)}
    bench = o.bench or os.path.join(REPO, "platform", "3ds", "bench", o.game + ".txt")
    scenes = bench_scenes(o.game, bench)
    ids = [name for name, _ in scenes]
    if len(ids) != len(set(ids)):
        raise SystemExit("duplicate bench scene ids: " + ", ".join(sorted(n for n in set(ids) if ids.count(n) > 1)))
    if o.scenes is not None:
        unknown = o.scenes - {name for name, _ in scenes}
        if unknown:
            raise SystemExit("unknown bench scenes: " + ", ".join(sorted(unknown)))
        scenes = [sc for sc in scenes if sc[0] in o.scenes]

    def one(sc):
        scene, ch = sc
        if ch not in ps:
            return scene, "no data.win for chapter %d" % ch
        key, data_win = ps[ch]
        d = os.path.join(o.out, key, "scenes")
        os.makedirs(d, exist_ok=True)
        log = os.path.join(d, scene + ".log")
        t0 = time.time()
        rc = run_runner(o, data_win, ch, bench, scene, o.frames, o.window, log,
                        os.path.join(o.out, "saves", "scene_" + scene), 3600)
        text = open(log, encoding="utf-8", errors="replace").read()
        ok = "Bench: measuring" in text and "Bench: scene failed" not in text and bool(gmlaot.profile_ops(log))
        return scene, "rc=%s %.0f s %s" % (rc, time.time() - t0, "ok" if ok else "INVALID PROFILE")

    failed = []
    with cf.ThreadPoolExecutor(o.jobs) as ex:
        for scene, msg in ex.map(one, scenes):
            print("%-12s %s" % (scene, msg), flush=True)
            if not msg.startswith("rc=0 ") or not msg.endswith(" ok"):
                failed.append(scene)
    if failed:
        raise SystemExit("scene profiling failed: " + ", ".join(failed))


def room_boot(o, ch):
    if o.game == "undertale":
        return [UT_SAVE_EARLY] + UT_BOOT + ["waitroom room_ruins1"]
    return ["chapter %d" % ch] + DR_ROOM_BOOT[ch]


def cmd_rooms(o):
    for key, data_win, ch in parts(o.game, o.data):
        if o.part and key != o.part:
            continue
        rooms = room_names(data_win)
        todo = [i for i, r in enumerate(rooms) if not ROOM_SKIP.match(r)]
        work = os.path.join(o.out, "work", key, "rooms")
        dest = os.path.join(o.out, key, "rooms")
        os.makedirs(work, exist_ok=True)
        os.makedirs(dest, exist_ok=True)
        print("%s: %d rooms, %d to sweep" % (key, len(rooms), len(todo)), flush=True)
        logs, pos, t0 = [], 0, time.time()
        while pos < len(todo):
            lines = ["scene sweep Room sweep"] + room_boot(o, ch) + ["measure"]
            for i in todo[pos:]:
                r = rooms[i]
                lines += ["log SWEEPROOM %d %s" % (i, r)]
                if ch:
                    lines += ["set global.darkzone %d" % (1 if r.startswith("room_dw") or "_dw_" in r else 0)]
                lines += ["call room_goto %s" % r, "wait %d" % o.frames]
            lines.append("log SWEEPROOM -1 end")
            script = os.path.join(work, "run%d.txt" % pos)
            write_script(script, lines)
            log = os.path.join(work, "run%d.log" % pos)
            rc = run_runner(o, data_win, ch, script, "sweep", 2000 + o.frames * (len(todo) - pos) + 200, 30, log,
                            os.path.join(o.out, "saves", key + "_rooms"), 3600)
            logs.append(log)
            seen = [int(m.group(1)) for m in re.finditer(r"SWEEPROOM (-?\d+) ", open(log, encoding="utf-8", errors="replace").read())]
            if -1 in seen:
                print("  run from %d: rc=%s, finished" % (pos, rc), flush=True)
                break
            if not seen:
                print("  run from %d: rc=%s, no room reached; stopping" % (pos, rc), flush=True)
                break
            print("  run from %d: rc=%s, ended in room %d %s; restarting after it" % (pos, rc, seen[-1], rooms[seen[-1]]), flush=True)
            pos = todo.index(seen[-1]) + 1
        count = split_rooms(logs, dest, 30)
        print("%s: %d room profiles in %.0f s" % (key, count, time.time() - t0), flush=True)


def split_rooms(logs, dest, window):
    """A profiler window belongs to a room when the room's marker came before the previous window ended and no other
    marker came since, so it holds only that room's frames."""
    count = 0
    for log in logs:
        cur, clean, windows, win = None, False, {}, None
        for l in open(log, encoding="utf-8", errors="replace"):
            m = re.search(r"SWEEPROOM (-?\d+) (\S+)", l)
            if m:
                cur, clean = (None if m.group(1) == "-1" else m.group(2)), False
                continue
            if l.startswith("GML Profiler"):
                win = [] if (cur is not None and clean) else None
                if cur is not None and not clean:
                    clean = True
                if win is not None:
                    windows.setdefault(cur, []).append(win)
                continue
            if win is not None:
                if l.startswith("total") or not re.match(r"[\d.]+ms ", l):
                    win = None
                else:
                    win.append(l)
        for room, wins in windows.items():
            with open(os.path.join(dest, room + ".log"), "w") as f:
                f.write("Bench: measuring sweep\n")
                for w in wins:
                    f.write("GML Profiler (avg %d frames)\n" % window)
                    f.writelines(w)
                    f.write("total\n")
            count += 1
    return count


def battle_lines(o, ch, e):
    if o.game == "undertale":
        return [UT_SAVE_LATE] + UT_BOOT + ["wait 20",
            "set global.battlegroup %d" % e, "set global.mercy 1", "set global.flag[15] 0", "create obj_battler 0 0",
            "waitroom room_battle", "keep set global.hp 99", "keys Z/6:2"]
    lines = ["chapter %d" % ch] + DR_BATTLE_SETUP[ch] + ["set global.char[0] 1", "set global.char[1] 2", "set global.char[2] 3"]
    if ch == 1:
        lines += ["set global.encounterno %d" % e, "create obj_encounterbasic 0 0"]
    else:
        lines += ["call scr_battle %d 0 -4 -4 -4" % e]
    return lines + ["wait 5", "keep set global.inv 30", "keep set global.hp[1] global.maxhp[1]",
                    "keep set global.hp[2] global.maxhp[2]", "keep set global.hp[3] global.maxhp[3]", "keys X Z/4:2"]


def cmd_battles(o):
    for key, data_win, ch in parts(o.game, o.data):
        if o.part and key != o.part:
            continue
        work = os.path.join(o.out, "work", key, "battles")
        dest = os.path.join(o.out, key, "battles")
        os.makedirs(work, exist_ok=True)
        os.makedirs(dest, exist_ok=True)

        def one(e):
            lines = ["scene b Battle %d" % e] + battle_lines(o, ch, e) + [
                "wait 90", "log BATTLESTART", "measure", "wait 300", "log BATTLEEND"]
            script = os.path.join(work, "e%d.txt" % e)
            write_script(script, lines)
            raw = os.path.join(work, "e%d.raw" % e)
            run_runner(o, data_win, ch, script, "b", 1600, 30, raw, os.path.join(work, "e%d.save" % e), 600)
            text = open(raw, encoding="utf-8", errors="replace").read()
            if "BATTLESTART" not in text or "Bench: measuring" not in text:
                return False
            body = text.split("BATTLESTART", 1)[1].split("BATTLEEND", 1)[0]
            with open(os.path.join(dest, "e%d.log" % e), "w") as f:
                f.write("Bench: measuring battle\n")
                f.write(body[body.find("GML Profiler"):] if "GML Profiler" in body else "")
            return True

        t0 = time.time()
        with cf.ThreadPoolExecutor(o.jobs) as ex:
            n = sum(ex.map(one, range(o.first, o.last + 1)))
        print("%s: encounters %d-%d, %d reached the battle, %.0f s" % (key, o.first, o.last, n, time.time() - t0), flush=True)
    cmd_distinct(o)


def cmd_distinct(o):
    """battles/kept.txt: runs with game code (DELTARUNE: enemy code), one per distinct set of entries over 1% share."""
    for key in sorted(os.listdir(o.out)):
        d = os.path.join(o.out, key, "battles")
        if not os.path.isdir(d):
            continue
        kept, sigs, runs, with_code = [], set(), 0, 0
        for f in sorted((f for f in os.listdir(d) if f.endswith(".log")), key=lambda f: int(re.sub(r"\D", "", f) or 0)):
            runs += 1
            ops = gmlaot.profile_ops(os.path.join(d, f))
            tot = sum(ops.values())
            if not tot or (o.game == "deltarune" and not any("enemy" in k.lower() for k in ops)):
                continue
            with_code += 1
            sig = frozenset(k for k, v in ops.items() if v / tot >= 0.01)
            if sig in sigs:
                continue
            sigs.add(sig)
            kept.append(f[:-4])
        with open(os.path.join(d, "kept.txt"), "w") as f:
            f.write("".join(k + "\n" for k in kept))
        print("%s: %d runs, %d with %s code, %d distinct" % (key, runs, with_code,
                                                          "enemy" if o.game == "deltarune" else "game", len(kept)), flush=True)


def main(argv):
    if len(argv) < 2 or argv[1] not in ("scenes", "rooms", "battles", "distinct"):
        print(__doc__)
        return 2

    class O:
        pass
    o = O()
    o.cmd, o.game, o.data, o.exe, o.out, o.part = argv[1], None, None, None, None, None
    o.jobs, o.first, o.last = 4, 1, None
    o.frames = None
    o.scenes = None
    o.bench = None
    o.window = 60
    args = argv[2:]
    for i in range(0, len(args), 2):
        k, v = args[i], args[i + 1] if i + 1 < len(args) else None
        if v is None:
            raise SystemExit("%s needs a value" % k)
        if k == "--game":
            o.game = v
        elif k == "--data":
            o.data = v
        elif k == "--exe":
            o.exe = v
        elif k == "--out":
            o.out = v
        elif k == "--part":
            o.part = v
        elif k == "--jobs":
            o.jobs = int(v)
        elif k == "--first":
            o.first = int(v)
        elif k == "--last":
            o.last = int(v)
        elif k == "--frames":
            o.frames = int(v)
        elif k == "--scenes":
            o.scenes = set(v.split(","))
        elif k == "--bench":
            o.bench = v
        elif k == "--window":
            o.window = int(v)
        else:
            raise SystemExit("unknown argument " + k)
    if o.game not in ("undertale", "deltarune") or not o.out:
        raise SystemExit("--game undertale|deltarune and --out are needed")
    os.makedirs(o.out, exist_ok=True)
    if o.cmd == "distinct":
        cmd_distinct(o)
        return 0
    if not o.data or not o.exe or not parts(o.game, o.data):
        raise SystemExit("--data (a folder with the game's data.win) and --exe are needed")
    if o.cmd == "scenes":
        o.frames = o.frames or 3000
        cmd_scenes(o)
    elif o.cmd == "rooms":
        o.frames = o.frames or 90
        cmd_rooms(o)
    else:
        o.last = o.last or (300 if o.game == "undertale" else 260)
        cmd_battles(o)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
