#!/usr/bin/env python3
"""Tests for texture preparation on the PC (tools/release/builder/texcache.py, tools/texprep/sources.py).

    py -3 -I tools/release/test_texcache.py

No game data: a synthetic data.win (two PNG pages drawn here) and a fake texprep for the card tests. The tests that
run the real tool need a texprep binary: TEXPREP_TEST_BINARY=<path>, or the shipped one under
tools/release/builder/bin/; without one they are skipped.
"""
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
import zlib
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(HERE, "builder"))
sys.dont_write_bytecode = True
import builder as app  # noqa: E402
import deliver  # noqa: E402
import products  # noqa: E402
import texcache  # noqa: E402
import sources  # noqa: E402  (tools/texprep, on the path through texcache)

UT = products.PRODUCTS["undertale"]
DR = products.PRODUCTS["deltarune"]


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def read(path):
    with open(path, "rb") as f:
        return f.read()


# ---------------------------------------------------------------------------------------------------------------------
# A synthetic data.win: FORM, GEN8 (the cache key) and TXTR with PNG pages, nothing else the texture code reads.

def png(width, height, seed):
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            a = 0 if (x + y + seed) % 7 == 0 else (128 if (x * y + seed) % 5 == 0 else 255)
            row += bytes(((x * 4 + seed) & 255, (y * 4) & 255, (x ^ y ^ seed) & 255, a))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def data_win(pages=((64, 48), (40, 300)), game_id=12345, timestamp=1700000000):
    gen8 = bytearray(200)
    gen8[1] = 17
    struct.pack_into("<I", gen8, 20, game_id)
    struct.pack_into("<II", gen8, 44, 2022, 1)
    struct.pack_into("<Q", gen8, 92, timestamp)
    gen8_chunk = b"GEN8" + struct.pack("<I", len(gen8)) + bytes(gen8)
    txtr_at = 8 + len(gen8_chunk)
    body = txtr_at + 8                                  # TXTR payload starts here
    count = len(pages)
    entries_at = body + 4 + 4 * count
    blobs_at = entries_at + 8 * count
    blobs = [png(w, h, i) for i, (w, h) in enumerate(pages)]
    payload = bytearray(struct.pack("<I", count))
    for i in range(count):
        payload += struct.pack("<I", entries_at + 8 * i)
    off = blobs_at
    for b in blobs:
        payload += struct.pack("<II", 1, off)
        off += len(b)
    for b in blobs:
        payload += b
    txtr_chunk = b"TXTR" + struct.pack("<I", len(payload)) + bytes(payload)
    form = gen8_chunk + txtr_chunk
    return b"FORM" + struct.pack("<I", len(form)) + form


def real_binary():
    p = os.environ.get("TEXPREP_TEST_BINARY")
    if p and os.path.isfile(p):
        return os.path.abspath(p)
    pin = texcache.PINNED.get((sys.platform, texcache.machine()))
    if pin and os.path.isfile(os.path.join(texcache.BIN_DIR, pin[0])):
        return os.path.join(texcache.BIN_DIR, pin[0])
    return None


# ---------------------------------------------------------------------------------------------------------------------
# A fake texprep: page files whose bytes depend on data.win and the page number, as the real one's do.

def fake_page(dw_bytes, i, pad=b"\0\0\0\0"):
    h = hashlib.sha256(dw_bytes + bytes([i])).digest()
    return b"DTX1" + h[:48] + pad + h * 4


def fake_run(argv):
    data = read(argv[1])
    out = argv[2]
    n = texcache.page_count(argv[1])
    for i in range(n):
        write(os.path.join(out, texcache.page_name(i)), fake_page(data, i))
    write(os.path.join(out, texcache.MARKER), b"XDTE" + hashlib.sha256(data).digest()[:28])
    return types.SimpleNamespace(returncode=0, stdout="texprep: fake, %d pages, complete" % n, stderr="")


FAKE_TOOL = texcache.Tool("fake-texprep", "pinned", "fake")


class Recorder(texcache.FileOps):
    """Real card writes, with their order recorded; fail_on raises OSError at that write."""

    def __init__(self, fail_on=None):
        self.ops = []
        self.fail_on = fail_on

    def _note(self, op, path):
        self.ops.append((op, os.path.basename(path)))
        if self.fail_on and self.fail_on == (op, os.path.basename(path)):
            raise OSError(5, "simulated card error")

    def copy(self, src, dst):
        self._note("copy", dst)
        super().copy(src, dst)

    def replace(self, src, dst):
        self._note("replace", dst)
        super().replace(src, dst)

    def remove(self, path):
        self._note("remove", path)
        super().remove(path)


def listing(d):
    out = {}
    for n in sorted(os.listdir(d)):
        out[n] = hashlib.sha256(read(os.path.join(d, n))).hexdigest()
    return out


class Fixture:
    def __init__(self, tmp, pages=((64, 48), (40, 300), (16, 16))):
        self.tmp = tmp
        self.game = os.path.join(tmp, "game")
        self.dw = data_win(pages)
        write(os.path.join(self.game, "data.win"), self.dw)
        self.work = os.path.join(tmp, "work")
        self.sd = os.path.join(tmp, "sd")
        os.makedirs(os.path.join(self.sd, "3ds"))
        self.card_cache = os.path.join(self.sd, "3ds", "undertale-3ds", "cache")

    def prepare(self):
        sets, problems = texcache.prepare(FAKE_TOOL, UT, self.game, self.work, run=fake_run, log=lambda s: None)
        assert not problems, problems
        return sets


class SourcesDigest(unittest.TestCase):
    def test_depfile_and_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(os.path.join(tmp, "a", "x.c"), b"int x;\r\n")
            write(os.path.join(tmp, "a", "y.h"), b"int y;\n")
            write(os.path.join(tmp, "tools", "texprep", "build.sh"), b"#!/bin/sh\n")
            dep = os.path.join(tmp, "x.d")
            with open(dep, "w") as f:
                f.write("x: %s \\\n %s /usr/include/stdio.h\n" % (os.path.join(tmp, "a", "x.c"),
                                                                  os.path.join(tmp, "a", "y.h")))
            rels = sources.relative_inputs(tmp, [dep])
            self.assertEqual(rels, ["a/x.c", "a/y.h", "tools/texprep/build.sh"])
            d1 = sources.digest(tmp, rels)
            write(os.path.join(tmp, "a", "x.c"), b"int x;\n")          # line endings don't count
            self.assertEqual(sources.digest(tmp, rels), d1)
            write(os.path.join(tmp, "a", "y.h"), b"int y2;\n")
            self.assertNotEqual(sources.digest(tmp, rels), d1)

    def test_check_mismatch_and_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(os.path.join(tmp, "f.c"), b"one\n")
            good = sources.digest(tmp, ["f.c"])

            def run_with(text, rc=0):
                return lambda argv: types.SimpleNamespace(returncode=rc, stdout=text, stderr="")
            self.assertTrue(sources.check(tmp, "t", run_with(good + "\nf.c\n"))[0])
            ok, why = sources.check(tmp, "t", run_with(sources.SCHEME + ":" + "0" * 64 + "\nf.c\n"))
            self.assertFalse(ok)
            self.assertIn("other sources", why)
            self.assertFalse(sources.check(tmp, "t", run_with(good + "\ng.c\n"))[0])     # file not in this tree
            self.assertFalse(sources.check(tmp, "t", run_with(good + "\n../f.c\n"))[0])  # never outside the tree
            self.assertFalse(sources.check(tmp, "t", run_with("usage", 2))[0])          # an older texprep

    @unittest.skipUnless(real_binary(), "no texprep binary (TEXPREP_TEST_BINARY)")
    def test_real_binary_matches_this_tree(self):
        ok, why = sources.check(ROOT, real_binary())
        self.assertTrue(ok, why)


class FindTool(unittest.TestCase):
    def test_pin_checks(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(os.path.join(tmp, "tp.exe"), b"binary")
            sha = hashlib.sha256(b"binary").hexdigest()
            pins = {("win32", "x86_64"): ("tp.exe", 6, sha)}
            self.assertEqual(texcache.pinned_tool("win32", "AMD64", tmp, pins)[0], os.path.join(tmp, "tp.exe"))
            bad = {("win32", "x86_64"): ("tp.exe", 6, "0" * 64)}
            self.assertIsNone(texcache.pinned_tool("win32", "AMD64", tmp, bad)[0])
            self.assertIsNone(texcache.pinned_tool("darwin", "arm64", tmp, pins)[0])
            unpinned = {("win32", "x86_64"): ("tp.exe", None, None)}
            self.assertIsNone(texcache.pinned_tool("win32", "AMD64", tmp, unpinned)[0])

    def test_digest_mismatch_skips_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            write(os.path.join(tmp, "bin", "tp.exe"), b"binary")
            pins = {("win32", "x86_64"): ("tp.exe", 6, hashlib.sha256(b"binary").hexdigest())}
            tool = texcache.find_tool(fx.work, platform="win32", arch="AMD64", bin_dir=os.path.join(tmp, "bin"),
                                      pins=pins, check=lambda p: (False, "built from other sources"))
            self.assertFalse(tool.ok)
            self.assertIn("other sources", tool.message)
            write(os.path.join(fx.card_cache, "rooms.dtm"), b"rooms")
            before = listing(fx.card_cache)
            ran = []
            res = texcache.deliver_textures(fx.sd, UT, fx.game, fx.work, tool=tool, run=lambda a: ran.append(a),
                                            log=lambda s: None)
            self.assertFalse(res.used)
            self.assertIn("3DS will prepare", res.message)
            self.assertEqual(ran, [])
            self.assertEqual(listing(fx.card_cache), before)

    def test_unix_falls_back_to_source_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(texcache, "ALLOW_SOURCE_BUILD", True):
                tool = texcache.find_tool(tmp, platform="linux", arch="x86_64", bin_dir=tmp, pins={},
                                          check=lambda p: (p == "built-tp", "digest"),
                                          build=lambda: ("built-tp", "texprep built here with cc"))
            self.assertEqual((tool.path, tool.how), ("built-tp", "built"))
            with mock.patch.object(texcache, "ALLOW_SOURCE_BUILD", True):
                tool = texcache.find_tool(tmp, platform="linux", arch="x86_64", bin_dir=tmp, pins={},
                                          check=lambda p: (True, "digest"), build=lambda: (None, "no C compiler"))
            self.assertFalse(tool.ok)
            self.assertIn("no C compiler", tool.message)
            tool = texcache.find_tool(tmp, platform="win32", arch="x86_64", bin_dir=tmp, pins={},
                                      check=lambda p: (True, "digest"), build=lambda: ("never", "never"))
            self.assertFalse(tool.ok)                                   # Windows never builds from source


class Card(unittest.TestCase):
    def test_fresh_card_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            sets = fx.prepare()
            self.assertEqual([(s.sd_dir, s.pages) for s in sets], [("3ds/undertale-3ds/cache", 3)])
            rec = Recorder()
            plans = texcache.plan(fx.sd, sets)
            written, kept, _ = texcache.write_plans(fx.sd, plans, rec)
            self.assertEqual((written, kept), (1, 0))
            self.assertEqual(rec.ops, [("copy", "page000.dtx.part"), ("replace", "page000.dtx"),
                                       ("copy", "page001.dtx.part"), ("replace", "page001.dtx"),
                                       ("copy", "page002.dtx.part"), ("replace", "page002.dtx"),
                                       ("copy", "complete.part"), ("replace", "complete")])
            self.assertEqual(listing(fx.card_cache), listing(sets[0].local_dir))

    def test_existing_matching_cache_is_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            sets = fx.prepare()
            texcache.write_plans(fx.sd, texcache.plan(fx.sd, sets))
            # A console-made page from before the header fix: only the padding differs.
            p1 = os.path.join(fx.card_cache, "page001.dtx")
            data = bytearray(read(p1))
            data[52:56] = b"\xf2\xc6\xd5\x00"
            write(p1, bytes(data))
            write(os.path.join(fx.card_cache, "rooms.dtm"), b"rooms")
            before = listing(fx.card_cache)
            rec = Recorder()
            plans = texcache.plan(fx.sd, sets)
            self.assertTrue(plans[0].keep)
            self.assertEqual(texcache.write_plans(fx.sd, plans, rec)[:2], (0, 1))
            self.assertEqual(rec.ops, [])
            self.assertEqual(listing(fx.card_cache), before)

    def test_stale_key_is_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            # The card holds a cache for another data.win (another game version): 5 pages and its own marker.
            old = data_win(((64, 48), (40, 300), (16, 16), (8, 8), (8, 8)), game_id=999)
            for i in range(5):
                write(os.path.join(fx.card_cache, texcache.page_name(i)), fake_page(old, i))
            write(os.path.join(fx.card_cache, "complete"), b"XDTE" + hashlib.sha256(old).digest()[:28])
            for keep in ("rooms.dtm", "bytecode.bin", "data-version.txt"):
                write(os.path.join(fx.card_cache, keep), keep.encode())
            sets = fx.prepare()
            rec = Recorder()
            plans = texcache.plan(fx.sd, sets)
            self.assertFalse(plans[0].keep)
            self.assertEqual(plans[0].remove, ["page003.dtx", "page004.dtx"])
            texcache.write_plans(fx.sd, plans, rec)
            ops = rec.ops
            self.assertEqual(ops[0], ("remove", "complete"))                         # the marker goes first
            self.assertEqual(ops[-2:], [("copy", "complete.part"), ("replace", "complete")])  # and comes back last
            self.assertLess(ops.index(("replace", "page002.dtx")), ops.index(("remove", "page003.dtx")))
            want = listing(sets[0].local_dir)
            got = listing(fx.card_cache)
            for keep in ("rooms.dtm", "bytecode.bin", "data-version.txt"):
                self.assertEqual(read(os.path.join(fx.card_cache, keep)), keep.encode())
                del got[keep]
            self.assertEqual(got, want)

    def test_corrupt_page_is_rewritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            sets = fx.prepare()
            texcache.write_plans(fx.sd, texcache.plan(fx.sd, sets))
            p2 = os.path.join(fx.card_cache, "page002.dtx")
            data = bytearray(read(p2))
            data[100] ^= 0x40
            write(p2, bytes(data))
            rec = Recorder()
            plans = texcache.plan(fx.sd, sets)
            self.assertFalse(plans[0].keep)
            self.assertEqual(plans[0].write_pages, [2])        # the good pages are already there byte for byte
            texcache.write_plans(fx.sd, plans, rec)
            self.assertEqual(rec.ops, [("remove", "complete"), ("copy", "page002.dtx.part"),
                                       ("replace", "page002.dtx"), ("copy", "complete.part"), ("replace", "complete")])
            self.assertEqual(listing(fx.card_cache), listing(sets[0].local_dir))

    def test_interrupted_write_leaves_no_marker_and_recovers(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            sets = fx.prepare()
            texcache.write_plans(fx.sd, texcache.plan(fx.sd, sets))
            # A newer data.win: every page changes. The card fails while writing page 1.
            fx.dw = data_win(((64, 48), (40, 300), (16, 16)), timestamp=1800000000)
            write(os.path.join(fx.game, "data.win"), fx.dw)
            sets = fx.prepare()
            rec = Recorder(fail_on=("copy", "page001.dtx.part"))
            with self.assertRaises(OSError):
                texcache.write_plans(fx.sd, texcache.plan(fx.sd, sets), rec)
            names = os.listdir(fx.card_cache)
            self.assertNotIn("complete", names)
            self.assertNotIn("page001.dtx.part", names)
            # Page 0 is new, page 1 still old: without the marker the console checks each page's header.
            self.assertEqual(read(os.path.join(fx.card_cache, "page000.dtx")),
                             read(os.path.join(sets[0].local_dir, "page000.dtx")))
            # A pulled card can leave a .part behind; the next run removes it.
            write(os.path.join(fx.card_cache, "page001.dtx.part"), b"half")
            plans = texcache.plan(fx.sd, sets)
            self.assertIn("page001.dtx.part", plans[0].remove)
            self.assertEqual(plans[0].write_pages, [1, 2])
            texcache.write_plans(fx.sd, plans)
            self.assertEqual(listing(fx.card_cache), listing(sets[0].local_dir))

    def test_deterministic_delivery(self):
        with tempfile.TemporaryDirectory() as tmp:
            outs = []
            for n in range(2):
                fx = Fixture(os.path.join(tmp, str(n)))
                texcache.deliver_textures(fx.sd, UT, fx.game, fx.work, tool=FAKE_TOOL, run=fake_run,
                                          log=lambda s: None)
                outs.append(listing(fx.card_cache))
            self.assertEqual(outs[0], outs[1])

    def test_deltarune_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "DELTARUNE")
            write(os.path.join(game, "data.win"), data_win(((16, 16), (16, 16))))
            write(os.path.join(game, "chapter1_windows", "data.win"), data_win(((16, 16), (8, 8)), game_id=1))
            write(os.path.join(game, "chapter2_windows", "data.win"), data_win(((16, 16), (8, 8)), game_id=2))
            write(os.path.join(game, "mus", "x.ogg"), b"ogg")
            chapters = [p for p in DR.game.parts if p != "root"]
            self.assertIn("chapter1_windows", chapters)
            rels = [r for _, r in texcache.data_wins(DR, game, ["chapter1_windows"])]
            self.assertEqual(rels, ["", "chapter1_windows"])         # chapter 2 not chosen
            self.assertEqual(texcache.sd_cache_dir(DR, "chapter1_windows"),
                             "3ds/deltarune-3ds/chapter1_windows/cache")
            self.assertEqual(texcache.sd_cache_dir(DR, ""), "3ds/deltarune-3ds/cache")

    def test_failed_preparation_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)

            def failing(argv):
                write(os.path.join(argv[2], "page000.dtx"), b"partial")
                return types.SimpleNamespace(returncode=1, stdout="INCOMPLETE", stderr="")
            res = texcache.deliver_textures(fx.sd, UT, fx.game, fx.work, tool=FAKE_TOOL, run=failing,
                                            log=lambda s: None)
            self.assertFalse(res.used)
            self.assertFalse(os.path.exists(fx.card_cache))

    def test_copy_with_textures_maps_card_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            sets = fx.prepare()
            with mock.patch.object(texcache, "prepare", return_value=(sets, [])):
                with mock.patch.object(texcache, "write_plans", side_effect=OSError(28, "No space left")), \
                        self.assertRaises(deliver.DeliverError):
                    deliver.copy_with_textures(fx.sd, [], UT, fx.game, fx.work, tool=FAKE_TOOL)
            with mock.patch.object(texcache, "prepare", return_value=(sets, [])):
                res = deliver.copy_with_textures(fx.sd, [], UT, fx.game, fx.work, tool=FAKE_TOOL)
            self.assertEqual(res[:3], (0, 0, 0))
            self.assertTrue(res[3].used)
            # No data.win was copied to this card, so no data-version.txt either (DataVersion tests).
            self.assertEqual(listing(fx.card_cache), listing(sets[0].local_dir))


class Package(unittest.TestCase):
    def test_package_takes_only_a_pinned_binary(self):
        sys.path.insert(0, HERE)
        import package_builder
        data = b"\x7fELF fake texprep"
        pins = {("linux", "x86_64"): ("texprep-linux-x64", len(data), hashlib.sha256(data).hexdigest())}
        with mock.patch.dict(texcache.PINNED, pins, clear=True):
            self.assertEqual(package_builder.texprep_entry("linux", data),
                             ("tools/release/builder/bin/texprep-linux-x64", data))
            with self.assertRaises(ValueError):
                package_builder.texprep_entry("linux", data + b"x")
            with self.assertRaises(ValueError):
                package_builder.texprep_entry("macos", data)


class DryRun(unittest.TestCase):
    def test_dry_run_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            lines = texcache.dry_run_lines(UT, fx.game, fx.work, tool=FAKE_TOOL)
            self.assertIn("Texture preparation on the PC: fake", lines[0])
            self.assertIn("data.win (3 pages) -> sd:/3ds/undertale-3ds/cache/", lines[1])
            self.assertIn("delete complete", lines[1])
            self.assertIn("write complete last", lines[1])
            off = texcache.dry_run_lines(UT, fx.game, fx.work, tool=texcache.Tool(None, "", "no shipped texprep"))
            self.assertIn("off (no shipped texprep)", off[0])
            self.assertEqual(os.listdir(fx.sd), ["3ds"])                       # nothing written
            self.assertFalse(os.path.exists(os.path.join(fx.work, "texcache")))

    def test_builder_dry_run_prints_texture_line(self):
        out = []
        with mock.patch.object(texcache, "dry_run_lines", return_value=["Texture preparation on the PC: X"]):
            with mock.patch.object(app, "discover") as disc, mock.patch.object(app, "toolchain") as tc, \
                    mock.patch.object(app, "deliver") as dl:
                disc.find_copies.return_value = []
                disc.Probes.return_value.roots.return_value = []
                dl.scan_saves.return_value = types.SimpleNamespace(folder="x", files=[])
                tc.detect.return_value = types.SimpleNamespace(bash=None)
                tc.dry_run_report.return_value = "toolchain"
                with mock.patch.object(app.sys, "platform", "win32"):
                    app.dry_run(dict(app.parse(["--dry-run"])), out=out.append)
        self.assertIn("Texture preparation on the PC: X", out)


# A data-version.txt the console wrote: a real o3DS's, fetched by FTP 2026-10-21 23:35, byte-identical to Azahar's
# (texprep; mtime 0 on both, docs/3ds-hardware.md): fprintf("%u %llu %lld %s\n%s\n") with libctru's st_mtime 0. Sizes and digests only (the same
# as platform/3ds/data_versions.inc), no game content.
CONSOLE_UT = (b"1 62921978 0 b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7\n"
              b"sdmc:/3ds/undertale-3ds/data.win\n")


class DataVersion(unittest.TestCase):
    def card_file(self, fx):
        return os.path.join(fx.card_cache, texcache.DATA_VERSION)

    def expected(self, fx, revision=None):
        revision = texcache.data_versions_revision() if revision is None else revision
        return ("%d %d 0 %s\nsdmc:/3ds/undertale-3ds/data.win\n"
                % (revision, len(fx.dw), hashlib.sha256(fx.dw).hexdigest())).encode()

    def put(self, fx):
        """The builder's copy of data.win on the card (copy2 keeps the time, as copy_items does)."""
        dst = os.path.join(fx.sd, "3ds", "undertale-3ds", "data.win")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(fx.game, "data.win"), dst)
        return dst

    def deliver(self, fx, fs=None, put=True, **kw):
        if put:
            self.put(fx)
        return texcache.deliver_data_versions(fx.sd, UT, fx.game, fx.work, fs=fs, log=lambda s: None, **kw)

    def test_format_matches_a_console_written_file(self):
        f = texcache.parse_data_version(CONSOLE_UT)
        self.assertEqual(f, (1, 62921978, 0, "b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7",
                             "sdmc:/3ds/undertale-3ds/data.win"))
        self.assertEqual(texcache.data_version_text(1, 62921978, f[3], texcache.console_data_win_path(UT, "")),
                         CONSOLE_UT)
        self.assertEqual(texcache.console_data_win_path(DR, ""), "sdmc:/3ds/deltarune-3ds/data.win")
        self.assertEqual(texcache.console_data_win_path(DR, "chapter1_windows"),
                         "sdmc:/3ds/deltarune-3ds/chapter1_windows/data.win")
        with open(os.path.join(ROOT, "platform", "3ds", "data_versions.inc"), encoding="utf-8") as inc:
            self.assertIn("#define DATA_VERSIONS_REVISION %d\n" % texcache.data_versions_revision(), inc.read())
        for bad in (CONSOLE_UT.replace(b"\n", b"\r\n"), CONSOLE_UT[:-1], b"1 2 0 abc\nx\n", b"\xff" * 10):
            self.assertIsNone(texcache.parse_data_version(bad))

    def test_fresh_card_writes_part_then_renames(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            rec = Recorder()
            res = self.deliver(fx, rec)
            self.assertEqual((res.used, res.written, res.kept), (True, 1, 0))
            self.assertEqual(rec.ops, [("copy", "data-version.txt.part"), ("replace", "data-version.txt")])
            self.assertEqual(read(self.card_file(fx)), self.expected(fx))
            self.assertEqual(sorted(os.listdir(fx.card_cache)), ["data-version.txt"])
            self.assertEqual(os.listdir(os.path.join(fx.work, "texcache")), [])     # no temporary left

    def test_matching_file_is_kept_whatever_its_mtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            self.deliver(fx)
            rec = Recorder()
            self.assertEqual(self.deliver(fx, rec).kept, 1)
            self.assertEqual(rec.ops, [])
            # A console that records a real mtime wrote its own file: still this data.win, so it stays.
            own = self.expected(fx).replace(b" 0 ", b" 1729000000 ", 1)
            write(self.card_file(fx), own)
            rec = Recorder()
            self.assertEqual((self.deliver(fx, rec).kept, rec.ops), (1, []))
            self.assertEqual(read(self.card_file(fx)), own)

    def test_stale_or_mismatching_file_is_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            good = self.expected(fx)
            f = texcache.parse_data_version(good)
            stale = [
                texcache.data_version_text(f[0], f[1] + 1, f[3], f[4]),                  # size mismatch
                texcache.data_version_text(f[0], f[1], "0" * 64, f[4]),                  # another data.win
                texcache.data_version_text(f[0] + 1, f[1], f[3], f[4]),                  # another table revision
                texcache.data_version_text(f[0], f[1], f[3], "sdmc:/3ds/other/data.win"),  # another path
                good[:-1],                                                               # cut short
                b"garbage",
            ]
            for old in stale:
                write(self.card_file(fx), old)
                rec = Recorder()
                res = self.deliver(fx, rec)
                self.assertEqual((res.written, res.kept), (1, 0), old)
                self.assertEqual(rec.ops, [("copy", "data-version.txt.part"), ("replace", "data-version.txt")])
                self.assertEqual(read(self.card_file(fx)), good)

    def test_interrupted_write_keeps_old_file_and_no_part(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            write(self.card_file(fx), b"garbage")
            with self.assertRaises(OSError):
                self.deliver(fx, Recorder(fail_on=("replace", "data-version.txt")))
            self.assertEqual(sorted(os.listdir(fx.card_cache)), ["data-version.txt"])
            self.assertEqual(read(self.card_file(fx)), b"garbage")     # the console rejects it and hashes
            self.assertEqual(self.deliver(fx).written, 1)
            self.assertEqual(read(self.card_file(fx)), self.expected(fx))

    def test_card_data_win_not_the_copy_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            res = self.deliver(fx, put=False)                                    # no data.win on the card
            self.assertFalse(res.used)
            self.assertFalse(os.path.exists(self.card_file(fx)))
            card_dw = self.put(fx)
            st = os.stat(card_dw)
            # Even matching dates cannot turn different bytes into a verified copy.
            write(card_dw, bytes(b ^ 1 for b in fx.dw))
            os.utime(card_dw, (st.st_atime, st.st_mtime))
            rec = Recorder()
            res = self.deliver(fx, rec, put=False)
            self.assertEqual((res.used, rec.ops), (False, []))
            self.assertFalse(os.path.exists(self.card_file(fx)))
            self.put(fx)
            os.utime(card_dw, (st.st_atime, st.st_mtime + 60))                  # Same bytes, dates don't matter.
            self.assertEqual(self.deliver(fx, put=False).written, 1)

    def test_no_revision_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            res = self.deliver(fx, source_root=os.path.join(tmp, "no-tree"))
            self.assertFalse(res.used)
            self.assertFalse(os.path.exists(fx.card_cache))
            self.assertIn("off", texcache.data_version_dry_run_lines(UT, fx.game, source_root=tmp)[0])

    def test_deltarune_one_file_per_data_win(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "game")
            dws = {"": data_win(game_id=1), "chapter1_windows": data_win(game_id=2)}
            sd = os.path.join(tmp, "sd")
            for rel, dw in dws.items():
                write(os.path.join(game, rel, "data.win"), dw)
                card = os.path.join(sd, "3ds", "deltarune-3ds", rel, "data.win")
                os.makedirs(os.path.dirname(card), exist_ok=True)
                shutil.copy2(os.path.join(game, rel, "data.win"), card)
            res = texcache.deliver_data_versions(sd, DR, game, os.path.join(tmp, "work"), parts=["chapter1_windows"],
                                                 log=lambda s: None)
            self.assertEqual(res.written, 2)
            for rel, dw in dws.items():
                path = texcache.console_data_win_path(DR, rel)
                got = read(os.path.join(sd, *texcache.sd_cache_dir(DR, rel).split("/"), "data-version.txt"))
                self.assertEqual(texcache.parse_data_version(got)[1:], (len(dw), 0, hashlib.sha256(dw).hexdigest(),
                                                                       path))

    def test_reuses_the_version_checks_digest(self):
        import verify_game
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            calls = []
            real = verify_game.sha256_file
            with mock.patch.object(verify_game, "sha256_file", side_effect=lambda p: calls.append(p) or real(p)):
                verify_game.data_digest(os.path.join(fx.game, "data.win"))
                self.deliver(fx)
                self.deliver(fx)
            self.assertEqual(len(calls), 1)
            write(os.path.join(fx.game, "data.win"), fx.dw + b"x")          # a changed file is hashed again
            self.assertEqual(self.deliver(fx).written, 1)

    def test_copy_with_textures_writes_it_after_the_textures(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            order = []
            real_tex, real_ver = texcache.deliver_textures, texcache.deliver_data_versions
            with mock.patch.object(texcache, "deliver_textures",
                                   side_effect=lambda *a, **k: order.append("tex") or real_tex(*a, **k)), \
                    mock.patch.object(texcache, "deliver_data_versions",
                                      side_effect=lambda *a, **k: order.append("ver") or real_ver(*a, **k)), \
                    mock.patch.object(texcache, "find_tool", return_value=texcache.Tool(None, "", "test")):
                items = deliver.plan_sd_copy(fx.sd, UT, [], fx.game)
                _, _, _, res = deliver.copy_with_textures(fx.sd, items, UT, fx.game, fx.work)
            self.assertEqual(order, ["tex", "ver"])
            self.assertIn("Version check prepared: 1 file(s) written", res.message)
            self.assertEqual(read(self.card_file(fx)), self.expected(fx))
            _, _, _, res = deliver.copy_with_textures(fx.sd, [], UT, fx.game, fx.work, textures=False)
            # Whole-set SD transactions verify receipts in the fresh staged layout before publication.
            self.assertIn("1 file(s) written", res.message)
            os.remove(self.card_file(fx))
            deliver.copy_with_textures(fx.sd, [], UT, fx.game, fx.work, textures=False, data_version=False)
            self.assertFalse(os.path.exists(self.card_file(fx)))

    def test_dry_run_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            lines = texcache.data_version_dry_run_lines(UT, fx.game)
            self.assertIn("revision %d" % texcache.data_versions_revision(), lines[0])
            self.assertIn("data.win -> sd:/3ds/undertale-3ds/cache/data-version.txt", lines[1])
            self.assertIn('"sdmc:/3ds/undertale-3ds/data.win"', lines[1])
            self.assertEqual(os.listdir(fx.sd), ["3ds"])


class HalfArt(unittest.TestCase):
    """products.HALFART: the half-resolution art build's own folder, from the build's patch and patched pages."""

    def build_files(self, fx, page_hash=None, dw_size=None):
        blob = png(40, 300, 1)                          # Fixture's page 1
        off = fx.dw.find(blob)
        h = page_hash if page_hash is not None else texcache._fnv1a(fx.dw[off:off + 4096])
        patch = (texcache.HALFART_HEADER.pack(b"DTHR", 1, dw_size or len(fx.dw), 12345, 1700000000, 0, 1, 0, 0) +
                 struct.pack("<4I", 1, h, 0, 0))
        # The patched page: the same size, its top half cleared (as a patch's clears do).
        rows = zlib.decompress(blob[blob.index(b"IDAT") + 4:-16])
        stride = 1 + 40 * 4
        raw = bytes(stride * 150) + rows[stride * 150:]

        idat = zlib.compress(raw, 9)
        start = blob.index(b"IDAT") - 4
        new = (blob[:start] + struct.pack(">I", len(idat)) + b"IDAT" + idat +
               struct.pack(">I", zlib.crc32(b"IDAT" + idat) & 0xffffffff) + blob[-12:])
        self.assertLessEqual(len(new), len(blob))
        pages = (struct.pack("<4sII", b"DTHP", 1, 1) + hashlib.sha256(patch).digest() +
                 struct.pack("<4I", 1, off, len(blob), len(new)) + new)
        base = os.path.join(fx.work, "src", *texcache.HALFART_PATCH)
        write(base + ".dthr", patch)
        write(base + ".pages", pages)
        patched = fx.dw[:off] + new + bytes(len(blob) - len(new)) + fx.dw[off + len(blob):]
        return patch, patched

    def test_off_by_default(self):
        self.assertFalse(products.HALFART)
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            self.build_files(fx)
            self.assertEqual([s.sd_dir for s in fx.prepare()], ["3ds/undertale-3ds/cache"])

    def test_own_folder_from_patched_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            patch, patched = self.build_files(fx)
            n = int(hashlib.sha256(patch).hexdigest()[:8], 16) | 0x80000000
            with mock.patch.object(products, "HALFART", True):
                sets, problems = texcache.prepare(FAKE_TOOL, UT, fx.game, fx.work, run=fake_run, log=lambda s: None)
            self.assertEqual(problems, [])
            self.assertEqual([s.sd_dir for s in sets], ["3ds/undertale-3ds/cache", "3ds/undertale-3ds/cache_lt_%d" % n])
            half = sets[1].local_dir
            self.assertEqual(read(os.path.join(half, "page001.dtx")), fake_page(patched, 1))
            self.assertEqual(read(os.path.join(half, "page000.dtx")), fake_page(patched, 0))
            self.assertNotEqual(read(os.path.join(half, "page001.dtx")), read(os.path.join(sets[0].local_dir, "page001.dtx")))
            self.assertEqual(sets[1].data_win, os.path.join(fx.game, "data.win"))
            self.assertFalse(os.path.exists(os.path.join(fx.work, "texcache", "halfart", "data.win")))   # copy removed
            # Both folders go to the card; the player's data.win is untouched.
            self.assertEqual(texcache.write_plans(fx.sd, texcache.plan(fx.sd, sets))[0], 2)
            self.assertEqual(read(os.path.join(fx.game, "data.win")), fx.dw)
            # DELTARUNE never gets one.
            with mock.patch.object(products, "HALFART", True):
                self.assertFalse(products.halfart("deltarune"))

    def test_wrong_inputs_leave_it_to_the_console(self):
        for kw in (dict(page_hash=1), dict(dw_size=7)):
            with tempfile.TemporaryDirectory() as tmp:
                fx = Fixture(tmp)
                self.build_files(fx, **kw)
                sets, problems = texcache.prepare(FAKE_TOOL, UT, fx.game, fx.work, run=fake_run, log=lambda s: None,
                                                  halfart=True)
                self.assertEqual([s.sd_dir for s in sets], ["3ds/undertale-3ds/cache"])
                self.assertEqual(len(problems), 1)
                self.assertIn("half-resolution art", problems[0])
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)                           # no build files at all
            sets, problems = texcache.prepare(FAKE_TOOL, UT, fx.game, fx.work, run=fake_run, log=lambda s: None,
                                              halfart=True)
            self.assertEqual(len(sets), 1)
            self.assertEqual(len(problems), 1)


@unittest.skipUnless(real_binary(), "no texprep binary (TEXPREP_TEST_BINARY)")
class RealTool(unittest.TestCase):
    def test_unicode_and_spaces_produce_identical_cache_bytes(self):
        tool = texcache.Tool(real_binary(), "pinned", "test")
        with tempfile.TemporaryDirectory() as tmp:
            outputs = []
            for name in ('ascii', '日本 source and work %'):
                fx = Fixture(os.path.join(tmp, name))
                sets, problems = texcache.prepare(tool, UT, fx.game, fx.work, log=lambda s: None)
                self.assertEqual(problems, [])
                outputs.append(listing(sets[0].local_dir))
            self.assertEqual(outputs[0], outputs[1])

    def test_deterministic_and_complete(self):
        tool = texcache.Tool(real_binary(), "pinned", "test")
        with tempfile.TemporaryDirectory() as tmp:
            outs = []
            for n in range(2):
                fx = Fixture(os.path.join(tmp, str(n)))
                sets, problems = texcache.prepare(tool, UT, fx.game, fx.work, log=lambda s: None)
                self.assertEqual(problems, [])
                self.assertEqual(sets[0].pages, 3)
                outs.append(listing(sets[0].local_dir))
            self.assertEqual(outs[0], outs[1])
            self.assertEqual(sorted(outs[0]), ["complete", "page000.dtx", "page001.dtx", "page002.dtx"])
            page = read(os.path.join(sets[0].local_dir, "page001.dtx"))
            self.assertEqual(page[:4], b"DTX1")
            self.assertEqual(page[52:56], b"\0\0\0\0")                     # the header padding is zero


if __name__ == "__main__":
    unittest.main()
