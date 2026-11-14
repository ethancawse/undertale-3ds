"""Builder key for UNDERTALE Old's covered-star skip (change asrfight, unit D; review decision 2026-10-30 19:0x).

When the big star of the Star Blazing attack bursts, one event makes its ring stars (a bullet object) at its centre
and, in the same event, a copy of the big star (a cover object, the burst object's sprite, drawn by the default draw)
at the same point. The stars' default depth is behind the cover's, and neither has stepped yet, so in that first
frame every new star is drawn upright (image_angle 0) and then drawn over by the cover's opaque core. Pictures are
the same with or without those draws. engine/src/runner.c's utkDrawRoom (UNDERTALE Old) leaves out the draws of
instances of the key's object at the key's depth whose image_angle is exactly 0. The stars' Step adds 4 to the angle
every frame, so 0 is their first frame only. The gun-arm attack's stars, the same object, are given depth -9000 and
never match. The proof (code, geometry and pictures) was done by asrfight.

The key is written only for data whose SHA-256 is one this proof was checked on, and only if the objects and sprites
found by the SHA-1 of their names still have the shapes the proof used. The cover's opaque core must reach past
every pixel a first-frame star can touch. Otherwise the include holds the "no key" values, and nothing is left out.

Run with -I:  py -3 -I tools/aot/dskip.py --out <private-build>/dskip_key.inc [--data <game folder>]
The output is a private build input (two numbers), never source or an export.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import data_versions  # noqa: E402
import gmlaot  # noqa: E402
import loadscreen_art as art  # noqa: E402

# The data.win / game.ios files the proof was checked on: Windows Steam 1.08, the macOS copy, and GOG 1.08 (its
# star, burst and cover code entries disassemble the same as Steam's, and the checks below hold on it).
CHECKED = {
    "b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7",
    "91a8acf1f40bd7c45d0bdb978de55187e7fb2cd76a1e2cda41c078dd44d3565e",
    "946c738c6d6182f683431320a0e32470bc3deb50f3e8887e61998df0c4e3abc8",
}
# SHA-1 of the names: the bullet object and its sprite, the cover object, the burst object and its sprite.
BULLET = "2864c86ecc608fda824c4be44caae4b6d96c7522"
BULLET_SPRITE = "598c3541dcfed1bd2647758edbb1758836c92e51"
COVER = "64382e3a19bdfe18b0ca669e86964fdf52de3c95"
BURST = "c68a9bbfc052b7283a7614d57212ffce1b3e5b04"
BURST_SPRITE = "00e57a0748d35d07aaec250a469040731d134cb3"
# The bullet sprite's size and origin as the proof measured them.
BULLET_SHAPE = (23, 23, 11, 11)
# How far a new star moves before its first draw: the burst's largest speed less its friction (1.6 + 0.3), rounded up.
MOTION = 2.0
# Beyond its ink, what a star's draw can reach and what the cover's draw needs opaque under it, in room pixels: the
# 2x2 block a half-resolution pixel covers (2 sqrt 2), Smart's averaging spread on the star (1), and on the cover its
# bilinear texels (sqrt 2) and its own averaging spread (1).
MARGIN = 2.0 * math.sqrt(2.0) + 1.0 + math.sqrt(2.0) + 1.0
NO_KEY = (-1, 0)


def sha1(text):
    return hashlib.sha1(text.encode("utf-8")).hexdigest()


def objects(wad):
    """(name, sprite, depth, parent) by object index (OBJT)."""
    out = []
    for p in wad.table("OBJT"):
        out.append((wad.cstr(wad.u32(p)), wad.i32(p + 4), wad.i32(p + 16), wad.i32(p + 24)) if p else ("", -1, 0, -100))
    return out


def sprite_info(wad, index):
    p = wad.table("SPRT")[index]
    name = wad.cstr(wad.u32(p))
    w, h = wad.u32(p + 4), wad.u32(p + 8)
    ox, oy = wad.i32(p + 48), wad.i32(p + 52)
    return name, w, h, ox, oy, art.sprite(wad, name)[4]


def alpha(wad, w, h, t):
    if (t["sw"], t["sh"]) != (t["tw"], t["th"]):
        raise ValueError("scaled frame")
    width, px = wad.page_rows(t["page"], t["sy"] + t["sh"])
    a = [[0] * w for _ in range(h)]
    for y in range(t["sh"]):
        row = ((t["sy"] + y) * width + t["sx"]) * 4
        for x in range(t["sw"]):
            a[t["ty"] + y][t["tx"] + x] = px[row + x * 4 + 3]
    return a


def ink_reach(a, ox, oy):
    """The farthest point of any inked texel from the origin; alpha must be 0 or 255."""
    reach = 0.0
    for y, row in enumerate(a):
        for x, v in enumerate(row):
            if v not in (0, 255):
                raise ValueError("partly transparent bullet texel")
            if v:
                reach = max(reach, math.hypot(max(abs(x - ox), abs(x + 1 - ox)), max(abs(y - oy), abs(y + 1 - oy))))
    return reach


def opaque_radius(a, w, h, ox, oy):
    """The distance from the origin to the nearest point of any texel that isn't fully opaque, or of the frame's edge."""
    best = float(min(ox, oy, w - ox, h - oy))
    for y, row in enumerate(a):
        for x, v in enumerate(row):
            if v != 255:
                dx = 0.0 if x <= ox <= x + 1 else min(abs(x - ox), abs(x + 1 - ox))
                dy = 0.0 if y <= oy <= y + 1 else min(abs(y - oy), abs(y + 1 - oy))
                best = min(best, math.hypot(dx, dy))
    return best


def derive(path):
    """(object index, depth) when every check holds, else NO_KEY, and why."""
    revision, known = data_versions.load_table()
    accepted, _, digest = data_versions.check_file(path, "undertale", "root", revision, known, {})
    if not accepted or digest not in CHECKED:
        return NO_KEY, "data not among the checked versions"
    wad = art.Wad(path)
    try:
        objs = objects(wad)
        index = {sha1(o[0]): i for i, o in enumerate(objs) if o[0]}
        if not all(k in index for k in (BULLET, COVER, BURST)):
            return NO_KEY, "objects not found"
        bullet, cover, burst = objs[index[BULLET]], objs[index[COVER]], objs[index[BURST]]
        if bullet[1] < 0 or burst[1] < 0 or cover[1] != -1 or cover[3] >= 0 or burst[3] >= 0:
            return NO_KEY, "object sprites or parents differ"
        if not bullet[2] > burst[2]:
            return NO_KEY, "the stars are not behind the cover"
        # No Draw event on the bullet, its parents or the cover: both are drawn by the default draw.
        chain, q = set(), index[BULLET]
        while 0 <= q < len(objs) and q not in chain:
            chain.add(q)
            q = objs[q][3]
        chain.add(index[COVER])
        names = {objs[i][0] for i in chain}
        dw = gmlaot.DataWin(str(path))
        for entry in dw.code:
            if entry is None or not entry["name"].startswith("gml_Object_"):
                continue
            body = entry["name"][len("gml_Object_"):]
            if "_Draw_" in body and body.rsplit("_Draw_", 1)[0] in names:
                return NO_KEY, "a Draw event on the stars or the cover"
        bname, bw, bh, box, boy, bframes = sprite_info(wad, bullet[1])
        if sha1(bname) != BULLET_SPRITE or (bw, bh, box, boy) != BULLET_SHAPE or len(bframes) != 1:
            return NO_KEY, "bullet sprite differs"
        reach = ink_reach(alpha(wad, bw, bh, bframes[0]), box, boy)
        cname, cw, ch, cox, coy, cframes = sprite_info(wad, burst[1])
        if sha1(cname) != BURST_SPRITE or not cframes:
            return NO_KEY, "cover sprite differs"
        core = min(opaque_radius(alpha(wad, cw, ch, t), cw, ch, cox, coy) for t in cframes)
        need = reach + MOTION + MARGIN
        if not core > need:
            return NO_KEY, "cover core %.2f does not reach %.2f" % (core, need)
        return (index[BULLET], bullet[2]), "core %.2f over %.2f" % (core, need)
    finally:
        wad.close()


def write_include(path, key, why):
    text = ("// PRIVATE: derived from the builder's game data; never redistribute.\n"
            "// tools/aot/dskip.py: %s\n#define DSKIP_OBJECT %d\n#define DSKIP_DEPTH %d\n" % (why, key[0], key[1]))
    path = Path(path)
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    temporary.replace(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--data", help="UNDERTALE folder (data.win or game.ios)")
    args = parser.parse_args(argv)
    key, why = NO_KEY, "no game data"
    try:
        import prepare
        folder = prepare.find_data("undertale", args.data)
        path = next((Path(folder) / n for n in ("data.win", "game.ios") if folder and (Path(folder) / n).is_file()), None)
        if path is not None:
            key, why = derive(path)
    except (OSError, ValueError, KeyError, IndexError, struct.error) as error:
        key, why = NO_KEY, "not checked (%s)" % error
    write_include(args.out, key, why)
    print("Covered stars: %s" % ("key written, " + why if key != NO_KEY else "no key, " + why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
