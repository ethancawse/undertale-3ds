"""Private output adapter for aotclean's UNDERTALE prompt-site generator.

The discovery implementation is tools/aot/btnprompt.py from aotclean.
This adapter emits STRG-index/hash guards and an interpreted-build fallback in
the earlier fixed BtnpromptTable layout. AOT uses the upstream-generated table.
Both AOT and interpreted builds run it; gmlaot separately applies the same generated
patches before compiling, using the same upstream implementation.

No game instructions or text are stored here. All generated files are private inputs
under the build directory, never source exports. Run with Python -I:
  py -3 -I tools/aot/btnprompt_build.py --data <player-folder> --out <private-build-dir>
"""
import argparse
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import btnprompt  # noqa: E402
import gmlaot  # noqa: E402
import pixgrid  # noqa: E402

OUTPUTS = ("btnprompt_code.inc", "btnprompt_strings.inc", "btnprompt_guards.inc", "btnprompt_fallback.c")


def initializers(table):
    """Keep dc60af6b's 360 B / 340 B table slots; discovery is entirely upstream."""
    if len(table["code"]) != 6 or len(table["strings"]) != 5:
        raise ValueError("prompt discovery does not fill the retained runtime table slots")
    if sorted(r[4] for r in table["strings"]) != list(range(5)):
        raise ValueError("prompt string slots are missing or duplicated")
    codes, strings = [], []
    for name, _, length, digest, offset, word, patched, flags in table["code"]:
        if len(name.encode("utf-8")) >= 36:
            raise ValueError("prompt entry name exceeds the retained field")
        codes.append("    { %s, %du, 0x%08Xu, 0x%Xu, 0x%08Xu, 0x%08Xu, %du }," %
                     (gmlaot.c_string(name), length, digest, offset, word, patched, flags))
    for name, _, length, offset, slot, text in table["strings"]:
        if len(name.encode("utf-8")) >= 36 or len(text.encode("utf-8")) >= 20:
            raise ValueError("prompt string exceeds the retained field")
        strings.append("    { %s, %du, 0x%Xu, %du, %s }," %
                       (gmlaot.c_string(name), length, offset, slot, gmlaot.c_string(text)))
    private = "// PRIVATE: generated from the builder's game data; never redistribute.\n"
    guards = table.get("guards")
    if guards is None or [r[0] for r in guards] != list(range(5)):
        raise ValueError("prompt index/hash guards are missing or unordered")
    guards = ["    { %du, 0x%08Xu }," % (index, digest) for _, index, digest in guards]
    return [private + "\n".join(rows) + "\n" for rows in (codes, strings, guards)]


def packed(table):
    """The original C records as bytes, for private equivalence checks only."""
    code = b"".join(struct.pack("<36s6I", n.encode(), length, digest, off, word, patch, flags)
                    for n, _, length, digest, off, word, patch, flags in table["code"])
    strings = b"".join(struct.pack("<36s3I20s", n.encode(), length, off, slot, text.encode())
                       for n, _, length, off, slot, text in table["strings"])
    return code, strings


def fallback_source(table):
    """Use upstream's fixed ABI, placing the newly added interpreter data late."""
    source = btnprompt.c_source(table, gmlaot.c_string)
    marker = 'BTNPROMPT_TABLE_SECTION'
    if source.count(marker) != 1:
        raise ValueError("upstream prompt-table declaration changed")
    return source.replace(marker, '__attribute__((section(".rodata.late.exportclean")))')


def generate(path):
    dw = gmlaot.DataWin(str(path))
    dw.patch_operands()
    table = btnprompt.generate(dw)
    table["guards"] = sorted((slot, dw.u32(dw.code_by_name[name]["abs"] + offset + 4),
                              btnprompt.fnv1a32(text.encode("utf-8")))
                             for name, _, _, offset, slot, text in table["strings"])
    if not table["sprite"]:
        raise ValueError("button-sprite entry is missing")
    initializers(table)
    return table


def write(path, text):
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    with temp.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    temp.replace(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        import prepare
        folder = prepare.find_data("undertale", args.data)
        if not folder:
            raise ValueError("the player's UNDERTALE data is required")
        path = next((Path(folder) / n for n in ("data.win", "game.ios") if (Path(folder) / n).is_file()), None)
        if path is None:
            raise ValueError("the game folder has no data.win or game.ios")
        # Same recognized input requirement as the grid step; discovery never
        # silently broadens the adaptation to a different game-file version.
        revision, known = pixgrid.data_versions.load_table()
        if not pixgrid.data_versions.check_file(path, "undertale", "root", revision, known, {})[0]:
            raise ValueError("UNDERTALE data version is not recognized")
        table = generate(path)
        text = [*initializers(table), fallback_source(table)]
        for name, body in zip(OUTPUTS, text):
            write(args.out / name, body)
    except (OSError, ValueError, KeyError, struct.error, gmlaot.Reject) as error:
        for name in OUTPUTS:
            (args.out / name).unlink(missing_ok=True)
        print("Button prompts: %s; no includes written" % error, file=sys.stderr)
        return 3
    print("Button prompts: %d branches / %d strings, private index/hash guards and interpreted table" %
          (len(table["code"]), len(table["strings"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
