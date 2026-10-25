#!/usr/bin/env python3
"""AOT selection lists from a profile corpus (docs/aot.md, "Byte-budget selection").

  select.py rank     --game G --data DIR --corpus CORPUS [--out FILE] [--hints FILE] [--max-mb 16] [--per-part] [--weigh ops]
  select.py evaluate --game G --data DIR --corpus CORPUS [--hints FILE] [--per-part] [--weigh ops]
  select.py layout   --game G --data DIR --corpus CORPUS [--selection FILE] [--out FILE] [--exclude [PART:]PREFIX]...

  --game G         undertale or deltarune
  --data DIR       the game folder the corpus was made from (tools/aot/corpus.py --data)
  --corpus CORPUS  the profiles tools/aot/corpus.py wrote (--out)
  --hints FILE     entries to take first whatever their rank, "<part> <entry>" per line (default:
                   tools/aot/selection/<game>-hints.txt when it exists): code that is hot on the 3DS but not in
                   headless desktop runs. "<part> <entry> extra" takes it on top of the budget instead of in it, so
                   nothing ranked makes room for it; the list keeps the mark
  --out FILE       default tools/aot/selection/<game>.txt
  --max-mb MB      the largest budget the list has to serve (default 16)
  --bytes-per-instr N  code size an instruction (default 85, measured on the o3DS build)
  --max-size N     never take an entry of more than N instructions (default 20000)
  --added-weight W  nominal total weight of an optional added-scenes/ corpus group relative to each original
                   kind (default 1); scenes/ retains the original bench profiles. The ops cap still applies.
  --budgets MBS    evaluate these comma-separated budgets in MB (default 4,8,12,16)
  --per-part       rank each part (DELTARUNE chapter) on its own profiles, --max-mb for each, into
                   tools/aot/selection/<game>-parts.txt: the lists for builds that load one chapter's code at a time
                   (platform/3ds/build.sh aot=chapters); evaluate then chooses each part's set on its own too
  --weigh ops|share  what a profile weighs (default ops): ops weighs it by its GML instructions a frame, up to
                   --ops-cap (default 33000, about a frame of Old 3DS interpreter time), so an entry's gain is the
                   instructions a frame it runs; share weighs every profile the same, so a light room's code counts
                   as much as a heavy battle's (the committed deltarune.txt was made with share)

rank: every code entry the corpus runs, ranked by gain per instruction of code; an entry's gain is its share of each
profile's instructions, weighted and summed. Each bench scene weighs 1; the rooms together weigh as much as all the
scenes, and so do the encounters; with --weigh ops each profile's weight is then scaled by its instructions a frame
over the cap (at most 1). The hints come first. The list keeps what a --max-mb budget takes (in order,
skipping what doesn't fit), "<part> <entry>" per line; gmlaot.py --selection walks it the same way for a smaller
budget (--target o3ds: 4 MB, n3ds: 12 MB). It names code entries only: the code is generated from the player's own
data.win at build time.

layout: the order to place each part's generated functions in, so that what runs together sits together in the
module (fewer pages and cache lines a frame). Made from the rooms and encounters only (no bench scenes, so the scenes
measure it out of sample); each profile's hot entries are those taking --hot-share percent (default 0.2) of its
instructions. Entries hot in profiles weighing --common percent (default 25) of all of them come first, hottest first
(rooms and encounters weigh the same in all, as in rank); then a profile at a time, the one with the most heat not
placed yet first, its unplaced hot entries hottest first. Names break every tie, so the same corpus always gives the
same list. --exclude leaves out the profiles whose names start with PREFIX (in PART only, with PART:), to hold scenes'
rooms or encounters out of it. Writes "<part> <entry>" lines for the entries of --selection (default
tools/aot/selection/<game>-parts.txt) into --out (default tools/aot/selection/<game>-layout.txt), for gmlaot.py
--layout (platform/3ds/build.sh aot=chapters uses it when it is there).

evaluate: coverage out of sample. The selection is made from half of each corpus (every other scene, room and
encounter of each part) and scored on the other half, both ways round, at 4, 8, 12 and 16 MB: the share of each
profile's instructions it covers and the GML instructions a frame it leaves to the interpreter, each a mean per kind.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import corpus  # noqa: E402
import gmlaot  # noqa: E402

KINDS = ("scenes", "rooms", "battles", "added-scenes")
SCENE_KINDS = ("scenes", "added-scenes")


def load(o):
    """profiles: [(part, kind, name, {entry: ops a frame})]; dws: {part: DataWin}"""
    dws = {}
    for part, path, _ in corpus.parts(o.game, o.data):
        dws[part] = gmlaot.DataWin(path)
        dws[part].patch_operands()
    profiles = []
    for part in sorted(dws):
        for kind in KINDS:
            d = os.path.join(o.corpus, part, kind)
            if not os.path.isdir(d):
                continue
            if kind == "battles":
                kept = os.path.join(d, "kept.txt")
                names = [l.strip() for l in open(kept)] if os.path.exists(kept) else []
            else:
                names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".log"))
            for n in names:
                if n:
                    path = os.path.join(d, n + ".log")
                    if kind in SCENE_KINDS:
                        text = open(path, encoding="utf-8", errors="replace").read()
                        if "Bench: measuring" not in text or "Bench: scene failed" in text:
                            raise SystemExit("invalid scene profile (measurement missing or scene failed): " + path)
                    ops = gmlaot.profile_ops(path)
                    if kind in SCENE_KINDS and not sum(ops.values()):
                        raise SystemExit("invalid scene profile (no measured GML instructions): " + path)
                    if sum(ops.values()):
                        profiles.append((part, kind, n, ops))
    return profiles, dws


def read_list(path, extra=None):
    """[(part, entry)]; the entries marked extra go into the set extra."""
    out = []
    for l in open(path, encoding="utf-8"):
        l = l.split("#")[0].split()
        if len(l) == 3 and l[2] == "extra":
            l = l[:2]
            if extra is not None:
                extra.add((l[0], l[1]))
        if len(l) == 2:
            out.append((l[0], l[1]))
    return out


class Sizes(dict):
    """Instructions of each (part, entry)'s generated function, None for one that can't be generated."""
    def __init__(self, dws):
        super().__init__()
        self.dws = dws

    def __missing__(self, key):
        part, name = key
        v = gmlaot.entry_size(self.dws[part], name) if part in self.dws else None
        self[key] = v
        return v


def profile_weights(o, profiles):
    """Equal nominal totals for original scenes, rooms and battles; added scenes have their own total."""
    n_scenes = sum(1 for p in profiles if p[1] == "scenes") or 1
    count = {k: sum(1 for p in profiles if p[1] == k) for k in KINDS}
    weights = [1.0 if k == "scenes" else n_scenes / count[k] for _, k, _, _ in profiles]
    weights = [w * getattr(o, "added_weight", 1.0) if p[1] == "added-scenes" else w
               for w, p in zip(weights, profiles)]
    if o.weigh == "ops":
        # A profile weighs in proportion to its GML instructions a frame, up to --ops-cap: an entry's gain is then the
        # instructions a frame it runs, and a place that runs more than the cap counts as if it ran the cap.
        weights = [w * min(1.0, sum(ops.values()) / o.ops_cap) for w, (_, _, _, ops) in zip(weights, profiles)]
    return weights


def choose(o, profiles, sizes, hints, mb):
    """The ordered selection a budget of mb megabytes takes from these profiles (o.extra on top of it)."""
    weights = profile_weights(o, profiles)
    keyed = [(part, ops) for part, _, _, ops in profiles]
    for part, ops in keyed:
        for name in ops:
            sizes[(part, name)]
    # A hint is taken whatever it is.
    order = hints + gmlaot.rank_by_gain(keyed, {k: v for k, v in sizes.items() if v}, weights)
    limit = int(mb * 1e6 / o.bytes)
    used, taken, seen = 0, [], set()
    for key in order:
        s = sizes[key]
        if key in seen or not s or s > o.max_size or (key not in o.extra and used + s > limit):
            continue
        seen.add(key)
        taken.append(key)
        if key not in o.extra:
            used += s
    return taken, used


def coverage(profiles, chosen):
    chosen = set(chosen)
    return [sum(v for n, v in ops.items() if (part, n) in chosen) / (sum(ops.values()) or 1) for part, _, _, ops in profiles]


def left(profiles, chosen):
    """Per profile, the GML instructions a frame left to the interpreter."""
    chosen = set(chosen)
    return [sum(v for n, v in ops.items() if (part, n) not in chosen) for part, _, _, ops in profiles]


def summary(profiles, chosen):
    cov = coverage(profiles, chosen)
    lft = left(profiles, chosen)
    out = []
    for k in KINDS:
        xs = [c for p, c in zip(profiles, cov) if p[1] == k]
        ls = [x for p, x in zip(profiles, lft) if p[1] == k]
        if xs:
            out.append("%s %d: mean %.1f%%, worst %.1f%%, %.0f instructions a frame left" % (
                k, len(xs), 100 * sum(xs) / len(xs), 100 * min(xs), sum(ls) / len(ls)))
    return "; ".join(out)


def cmd_rank(o):
    profiles, dws = load(o)
    sizes = Sizes(dws)
    hints = read_list(o.hints, o.extra) if o.hints else []
    if o.per_part:
        # Each part on its own: its profiles weighed as select.py weighs a whole game, its hints first.
        groups = [(part, [p for p in profiles if p[0] == part], [h for h in hints if h[0] == part]) for part in sorted(dws)]
        groups = [g for g in groups if g[1]]
    else:
        groups = [(None, profiles, hints)]
    chosen = []
    for part, profs, hh in groups:
        taken, used = choose(o, profs, sizes, hh, o.max_mb)
        chosen.append((part, profs, taken, used))
    counts = {k: sum(1 for p in profiles if p[1] == k) for k in KINDS}
    with open(o.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# AOT selection for %s: code entries, best gain per byte of code first. Made by tools/aot/select.py from\n"
                "# %d bench scenes, %d rooms and %d encounters; it fills %g MB%s at %g bytes an instruction. Names only: the\n"
                "# code is generated from your own data.win at build time (gmlaot.py --selection, platform/3ds/build.sh %s).\n"
                % (o.game.upper(), counts["scenes"] + counts["added-scenes"], counts["rooms"], counts["battles"], o.max_mb,
                   " for each part" if o.per_part else "", o.bytes, "aot=chapters" if o.per_part else "aot"))
        if o.weigh == "ops":
            f.write("# Gain is GML instructions a frame (select.py --weigh ops, each profile counted up to %d a frame).\n"
                    % o.ops_cap)
        if counts["added-scenes"]:
            f.write("# Added scenes: %d profiles, nominal total weight %g times each original corpus kind.\n"
                    % (counts["added-scenes"], o.added_weight))
        f.write("# <data.win folder> <code entry>\n")
        for _, _, taken, _ in chosen:
            for part, name in taken:
                f.write("%s %s%s\n" % (part, name, " extra" if (part, name) in o.extra else ""))
    for part, profs, taken, used in chosen:
        print("%s%s: %d entries, %d instructions (%.1f MB)" % (o.out, " " + part if part else "", len(taken), used,
                                                              used * o.bytes / 1e6))
        for target, mb in sorted(gmlaot.TARGET_BUDGET_MB.items()):
            sub, u = walk(o, taken, sizes, mb)
            print("  %s %g MB: %d entries, %d instructions; in sample: %s" % (target, mb, len(sub), u, summary(profs, sub)))


def walk(o, lst, sizes, mb):
    """What gmlaot.py --selection takes from lst for a budget of mb megabytes."""
    limit = int(mb * 1e6 / o.bytes)
    used, out = 0, []
    for key in lst:
        s = sizes[key]
        if s and s <= o.max_size and (key in o.extra or used + s <= limit):
            out.append(key)
            used += 0 if key in o.extra else s
    return out, used


def cmd_evaluate(o):
    profiles, dws = load(o)
    sizes = Sizes(dws)
    hints = read_list(o.hints, o.extra) if o.hints else []
    halves = ([], [])
    for part in sorted(dws):
        for k in KINDS:
            sub = [p for p in profiles if p[0] == part and p[1] == k]
            halves[0].extend(sub[0::2])
            halves[1].extend(sub[1::2])
    def pick(profs, mb):
        # --per-part: each part chosen on its own profiles with the whole budget, as rank --per-part makes the list
        if not o.per_part:
            return choose(o, profs, sizes, hints, mb)[0]
        out = []
        for part in sorted(dws):
            sub = [p for p in profs if p[0] == part]
            if sub:
                out += choose(o, sub, sizes, [h for h in hints if h[0] == part], mb)[0]
        return out
    print("out of sample: chosen on half of each corpus, scored on the other half (mean of both ways round)%s, weighted by %s"
          % (", each part on its own" if o.per_part else "", "share" if o.weigh == "share" else
             "instructions a frame up to %d" % o.ops_cap))
    print("each cell: share of the instructions covered, mean; GML instructions a frame left to the interpreter, mean")
    shown = [k for k in KINDS if any(p[1] == k for p in profiles)]
    print("%6s %12s %s | in sample: all scenes" % ("budget", "instructions", " ".join("%16s" % k for k in shown)))
    for mb in getattr(o, "budgets", (4, 8, 12, 16)):
        res = {k: [] for k in KINDS}
        for tr, te in ((halves[0], halves[1]), (halves[1], halves[0])):
            ch = pick(tr, mb)
            for k in KINDS:
                xs = [(c, l) for p, c, l in zip(te, coverage(te, ch), left(te, ch)) if p[1] == k]
                if xs:
                    res[k].append((sum(c for c, _ in xs) / len(xs), sum(l for _, l in xs) / len(xs)))
        ch = pick(profiles, mb)
        ins = [(c, l) for p, c, l in zip(profiles, coverage(profiles, ch), left(profiles, ch)) if p[1] in SCENE_KINDS]
        cell = lambda k: ("%8.1f%% %6.0f" % (100 * sum(c for c, _ in res[k]) / len(res[k]), sum(l for _, l in res[k]) / len(res[k]))
                          if res[k] else "%16s" % "-")
        print("%4g MB %12d %s | %.1f%% %.0f" % (mb, int(mb * 1e6 / o.bytes), " ".join(cell(k) for k in shown),
                                                     100 * sum(c for c, _ in ins) / max(1, len(ins)),
                                                     sum(l for _, l in ins) / max(1, len(ins))))


def layout_order(profiles, hot_share, common_weight):
    """The order to place one part's functions in: [entry]. profiles: [(kind, name, {entry: ops a frame})]."""
    count = {k: sum(1 for p in profiles if p[0] == k) for k in KINDS}
    weights = [1.0 / count[k] for k, _, _ in profiles]
    total_weight = sum(weights)
    shares = []
    for _, _, ops in profiles:
        t = float(sum(ops.values()))
        shares.append({n: v / t for n, v in ops.items() if v / t >= hot_share})
    heat, spread = {}, {}
    for w, sh in zip(weights, shares):
        for n, s in sh.items():
            heat[n] = heat.get(n, 0.0) + w * s
            spread[n] = spread.get(n, 0.0) + w
    # What most of the part runs, hottest first; then one profile at a time, the one with the most heat not yet placed
    # first, its entries hottest first; names break every tie.
    order = sorted((n for n in heat if spread[n] >= common_weight * total_weight), key=lambda n: (-heat[n], n))
    placed = set(order)
    left = list(range(len(profiles)))
    while left:
        gain = {i: sum(weights[i] * s for n, s in shares[i].items() if n not in placed) for i in left}
        best = max(left, key=lambda i: (gain[i], profiles[i][0], profiles[i][1]))
        if gain[best] <= 0:
            break
        for n in sorted((n for n in shares[best] if n not in placed), key=lambda n: (-shares[best][n], n)):
            order.append(n)
            placed.add(n)
        left.remove(best)
    return order


def cmd_layout(o):
    profiles, _ = load(o)

    def held_out(part, name):
        for x in o.exclude:
            xp, _, prefix = x.rpartition(":")
            if (not xp or xp == part) and name.startswith(prefix):
                return True
        return False
    used = [p for p in profiles if p[1] not in SCENE_KINDS and not held_out(p[0], p[2])]
    left_out = sorted("%s:%s" % (p[0], p[2]) for p in profiles if p[1] not in SCENE_KINDS and held_out(p[0], p[2]))
    selection = read_list(o.selection)
    chosen = {}
    for part, name in selection:
        chosen.setdefault(part, []).append(name)
    with open(o.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# AOT code layout for %s: the order gmlaot.py --layout places generated functions in, each part's functions\n"
                "# that run together next to each other. Made by tools/aot/select.py layout from %d rooms and %d encounters\n"
                "# (no bench scenes; %d profiles held out). Names only: the code is generated from your own data.win at build\n"
                "# time. Functions it doesn't list follow in the selection's order.\n"
                "# <data.win folder> <code entry>\n" % (o.game.upper(), sum(1 for p in used if p[1] == "rooms"),
                                                       sum(1 for p in used if p[1] == "battles"), len(left_out)))
        for part in sorted(chosen):
            sel = set(chosen[part])
            order = [n for n in layout_order([(k, n, ops) for pt, k, n, ops in used if pt == part], o.hot_share, o.common)
                     if n in sel]
            for n in order:
                f.write("%s %s\n" % (part, n))
            print("%s %s: %d of the selection's %d entries placed" % (o.out, part, len(order), len(sel)))
    if left_out:
        print("held out: " + " ".join(left_out))


def main(argv):
    if len(argv) < 2 or argv[1] not in ("rank", "evaluate", "layout"):
        print(__doc__)
        return 2

    class O:
        pass
    o = O()
    o.cmd, o.game, o.data, o.corpus, o.out, o.hints = argv[1], None, None, None, None, None
    o.extra = set()
    o.max_mb, o.bytes, o.max_size = 16.0, 85.0, 20000
    o.selection, o.exclude, o.hot_share, o.common = None, [], 0.002, 0.25
    o.weigh, o.ops_cap = "ops", 33000.0
    o.added_weight = 1.0
    o.budgets = (4, 8, 12, 16)
    o.per_part = "--per-part" in argv
    args = [a for a in argv[2:] if a != "--per-part"]
    for i in range(0, len(args), 2):
        k, v = args[i], args[i + 1] if i + 1 < len(args) else None
        if v is None:
            raise SystemExit("%s needs a value" % k)
        if k == "--game":
            o.game = v
        elif k == "--data":
            o.data = v
        elif k == "--corpus":
            o.corpus = v
        elif k == "--out":
            o.out = v
        elif k == "--hints":
            o.hints = v
        elif k == "--max-mb":
            o.max_mb = float(v)
        elif k == "--bytes-per-instr":
            o.bytes = float(v)
        elif k == "--max-size":
            o.max_size = int(v)
        elif k == "--selection":
            o.selection = v
        elif k == "--exclude":
            o.exclude.append(v)
        elif k == "--hot-share":
            o.hot_share = float(v) / 100
        elif k == "--common":
            o.common = float(v) / 100
        elif k == "--weigh":
            if v not in ("ops", "share"):
                raise SystemExit("--weigh ops|share")
            o.weigh = v
        elif k == "--ops-cap":
            o.ops_cap = float(v)
        elif k == "--added-weight":
            o.added_weight = float(v)
            if not 0 <= o.added_weight < float("inf"):
                raise SystemExit("--added-weight must be finite and nonnegative")
        elif k == "--budgets":
            o.budgets = tuple(float(mb) for mb in v.split(","))
            if not o.budgets or not all(0 < mb < float("inf") for mb in o.budgets):
                raise SystemExit("--budgets must contain positive finite MB values")
        else:
            raise SystemExit("unknown argument " + k)
    if o.game not in ("undertale", "deltarune") or not o.data or not o.corpus:
        raise SystemExit("--game undertale|deltarune, --data and --corpus are needed")
    sel = os.path.join(HERE, "selection")
    if o.hints is None and os.path.exists(os.path.join(sel, o.game + "-hints.txt")):
        o.hints = os.path.join(sel, o.game + "-hints.txt")
    if o.cmd == "layout":
        o.selection = o.selection or os.path.join(sel, o.game + "-parts.txt")
        o.out = o.out or os.path.join(sel, o.game + "-layout.txt")
        cmd_layout(o)
        return 0
    o.out = o.out or os.path.join(sel, o.game + ("-parts.txt" if o.per_part else ".txt"))
    if o.cmd == "rank":
        cmd_rank(o)
    else:
        cmd_evaluate(o)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
