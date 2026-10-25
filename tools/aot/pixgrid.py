"""Derive ctr_pixgrid.c's fixed-format opaque runs from the builder's own UNDERTALE data.

Run with Python -I. The output is a private build input, never source or an export:
  py -3 -I tools/aot/pixgrid.py --out <private-build>/pixgrid_runs.inc [--data <game-folder>]

This step deliberately has no embedded fallback geometry. Missing or unrecognized data fails
without leaving a stale include. Both AOT and interpreted builds run this private build step.
"""
import argparse
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import data_versions  # noqa: E402
import loadscreen_art as art  # noqa: E402

SPRITE = "spr_battlebg"
RUN_COUNT = 23
FRAME_SIZES = ((609, 236), (639, 253))


def pack(x, y, length, row=False):
    """The existing runtime word: x:10, y:8, length-minus-one:8, orientation:1."""
    if not (0 <= x < 1024 and 0 <= y < 256 and 1 <= length <= 256):
        raise ValueError("opaque run does not fit the runtime format")
    return x | y << 10 | (length - 1) << 18 | int(row) << 26


def opaque_runs(mask):
    """Maximal columns, x then y, followed by maximal rows, y then x.

    Intersections belong to both runs, as in the renderer's current format. Single
    texels are excluded; the coverage check below rejects any ink they would lose.
    """
    height = len(mask)
    width = len(mask[0]) if height else 0
    if not width or any(len(line) != width for line in mask):
        raise ValueError("empty or ragged opaque mask")
    runs, covered = [], set()
    for row in (False, True):
        for axis in range(height if row else width):
            pos, limit = 0, width if row else height
            while pos < limit:
                x, y = (pos, axis) if row else (axis, pos)
                if not mask[y][x]:
                    pos += 1
                    continue
                end = pos + 1
                while end < limit and (mask[axis][end] if row else mask[end][axis]):
                    end += 1
                if end - pos > 1:
                    runs.append(pack(x, y, end - pos, row))
                    covered.update((p, axis) if row else (axis, p) for p in range(pos, end))
                pos = end
    if any(value and (x, y) not in covered for y, line in enumerate(mask) for x, value in enumerate(line)):
        raise ValueError("opaque ink is not covered by orthogonal runs")
    return runs


def corner(mask):
    """Frame zero's whole-length grid needs only its shared top-left corner record."""
    if not mask or not mask[0] or not mask[0][0]:
        raise ValueError("whole-length grid has no top-left corner")
    rows = [all(line) for line in mask]
    cols = [all(line[x] for line in mask) for x in range(len(mask[0]))]
    if not rows[0] or not cols[0] or any(
            bool(value) != (rows[y] or cols[x]) for y, line in enumerate(mask) for x, value in enumerate(line)):
        raise ValueError("first frame is not a whole-length grid")
    return pack(0, 0, 1)


def frame_mask(wad, item):
    if (item["sw"], item["sh"]) != (item["tw"], item["th"]):
        raise ValueError("scaled battle grid frame")
    width, pixels = wad.page_rows(item["page"], item["sy"] + item["sh"])
    if item["sx"] + item["sw"] > width or len(pixels) < width * (item["sy"] + item["sh"]) * 4:
        raise ValueError("battle grid lies outside its texture page")
    mask, colours = [], set()
    for y in range(item["sh"]):
        line = []
        for x in range(item["sw"]):
            offset = ((item["sy"] + y) * width + item["sx"] + x) * 4
            rgba = pixels[offset:offset + 4]
            opaque = rgba[3] == 255
            if opaque:
                colours.add(bytes(rgba[:3]))
            line.append(opaque)
        mask.append(line)
    if len(colours) != 1:
        raise ValueError("opaque grid ink is not one colour")
    return mask


def derive(path):
    revision, known = data_versions.load_table()
    accepted, _, _ = data_versions.check_file(path, "undertale", "root", revision, known, {})
    if not accepted:
        raise ValueError("UNDERTALE data version is not recognized")
    wad = art.Wad(path)
    try:
        frames = art.sprite(wad, SPRITE)[4]
        if len(frames) < 2 or tuple((t["sw"], t["sh"]) for t in frames[:2]) != FRAME_SIZES:
            raise ValueError("unexpected battle grid frame sizes")
        runs = [corner(frame_mask(wad, frames[0]))] + opaque_runs(frame_mask(wad, frames[1]))
    finally:
        wad.close()
    if len(runs) != RUN_COUNT:
        raise ValueError("battle grid run count differs from the retained runtime slot")
    return runs


def packed(runs):
    return struct.pack("<%dI" % len(runs), *runs)


def write_include(path, runs):
    """Only initializer words: the declaration, alignment and section stay in ctr_pixgrid.c."""
    text = "// PRIVATE: derived from the builder's game data; never redistribute.\n"
    text += "\n".join("    " + " ".join("0x%08Xu," % n for n in runs[i:i + 8])
                      for i in range(0, len(runs), 8)) + "\n"
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
    try:
        import prepare
        folder = prepare.find_data("undertale", args.data)
        if not folder:
            raise ValueError("the player's UNDERTALE data is required")
        path = next((Path(folder) / n for n in ("data.win", "game.ios") if (Path(folder) / n).is_file()), None)
        if path is None:
            raise ValueError("the game folder has no data.win or game.ios")
        runs = derive(path)
        write_include(args.out, runs)
    except (OSError, ValueError, KeyError, struct.error) as error:
        args.out.unlink(missing_ok=True)
        print("Battle grid: %s; no include written" % error, file=sys.stderr)
        return 3
    import hashlib
    print("Battle grid: %d runs, %d B, SHA-256 %s" % (len(runs), len(packed(runs)), hashlib.sha256(packed(runs)).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
