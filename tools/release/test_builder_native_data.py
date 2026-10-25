#!/usr/bin/env python3
"""Maintainer-only native-data compatibility preflight; never changes the builder allow-list.

Run with -I. Inputs: Windows data.win and Linux game.unx or Mac game.ios from your own Steam depots.
Output: only sizes, digests, chunk inventory and parser counts. No extracted assets or game identifiers.
This is a preparation gate; actual gameplay/AOT equivalence still needs the campaign in the companion doc.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "aot"))
import gmlaot


def inspect(path):
    data = path.read_bytes()
    if len(data) < 8 or data[:4] != b"FORM" or struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        raise ValueError("Invalid FORM header or size")
    chunks = []
    pos = 8
    while pos < len(data):
        if pos + 8 > len(data):
            raise ValueError("Truncated chunk header")
        tag = data[pos:pos+4].decode("ascii")
        size = struct.unpack_from("<I", data, pos+4)[0]
        if pos + 8 + size > len(data):
            raise ValueError("Chunk exceeds file size")
        body = data[pos+8:pos+8+size]
        chunks.append({"tag": tag, "bytes": size, "sha256": hashlib.sha256(body).hexdigest()})
        pos += 8 + size
    result = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "chunks": chunks}
    try:
        parsed = gmlaot.DataWin(str(path))
        result["parser"] = {"ok": True, "wad": parsed.wad, "code_entries": len(parsed.code),
                            "variables": len(parsed.vars), "functions": len(parsed.funcs)}
    except (Exception, SystemExit) as exc:
        # Exceptions can contain file paths or GML names: record the class only.
        result["parser"] = {"ok": False, "error_type": type(exc).__name__}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--windows", type=Path, required=True)
    p.add_argument("--native", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    o = p.parse_args()
    if o.native.suffix not in (".unx", ".ios"):
        p.error("--native must be game.unx or game.ios")
    if o.output.resolve() in (o.windows.resolve(), o.native.resolve()):
        p.error("output must not overwrite either input")
    windows, native = inspect(o.windows), inspect(o.native)
    result = {"schema": 1, "windows": windows, "native": native,
              "identical": windows["sha256"] == native["sha256"],
              "parser_counts_match": windows["parser"]["ok"] and native["parser"]["ok"] and
                                     windows["parser"] == native["parser"],
              "approved_for_builder": False}
    o.output.parent.mkdir(parents=True, exist_ok=True)
    o.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Wrote native compatibility preflight. Builder approval remains false; follow the runtime campaign.")
    return 0 if windows["parser"]["ok"] and native["parser"]["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
