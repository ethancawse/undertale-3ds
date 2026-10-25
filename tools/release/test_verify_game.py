#!/usr/bin/env python3
"""Tests for verify_game.py with fake Steam and GOG trees (no game data needed): py -3 -I tools/release/test_verify_game.py"""
import contextlib
import hashlib
import io
import json
import os
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_game as vg  # noqa: E402

OWNER = "76561190000000001"
OTHER = "76561190000000002"


def blob(tag):
    return ("fake data.win " + tag).encode() * 3


def row(game, part, data, store="steam", label="test 1.0"):
    return (game, part, len(data), hashlib.sha256(data).hexdigest(), store, label)


UT = blob("ut")
DR = {p: blob("dr " + p) for p in vg.GAMES["deltarune"].parts}
ALLOW = [row("undertale", "root", UT)] + [row("deltarune", p, d) for p, d in DR.items()]


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w", **({} if isinstance(data, bytes) else {"encoding": "utf-8"})) as f:
        f.write(data)


class Tree:
    """A Steam client at <tmp>/Steam with a second library at <tmp>/Lib."""

    def __init__(self, tmp, old_format=False):
        self.tmp = tmp
        self.root = os.path.join(tmp, "Steam")
        self.lib = os.path.join(tmp, "Lib")
        os.makedirs(os.path.join(self.lib, "steamapps"), exist_ok=True)
        lib_vdf = self.lib.replace("\\", "\\\\")
        if old_format:
            vdf = '"LibraryFolders"\n{\n\t"TimeNextStatsReport"\t"1"\n\t"1"\t"%s"\n}\n' % lib_vdf
        else:
            vdf = ('"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n'
                   '\t\t"apps"\n\t\t{\n\t\t\t"391540"\t\t"1"\n\t\t}\n\t}\n}\n'
                   % (self.root.replace("\\", "\\\\"), lib_vdf))
        write(os.path.join(self.root, "steamapps", "libraryfolders.vdf"), vdf)
        self.users([OWNER])

    def users(self, ids):
        body = "".join('\t"%s"\n\t{\n\t\t"AccountName"\t\t"someone"\n\t\t"MostRecent"\t\t"1"\n\t}\n' % i for i in ids)
        write(os.path.join(self.root, "config", "loginusers.vdf"), '"users"\n{\n%s}\n' % body)

    def install(self, appid, installdir, files, owner=OWNER, flags=4, lib=None):
        lib = lib or self.lib
        write(os.path.join(lib, "steamapps", "appmanifest_%s.acf" % appid),
              '"AppState"\n{\n\t"appid"\t\t"%s"\n\t"StateFlags"\t\t"%d"\n\t"installdir"\t\t"%s"\n'
              '\t"buildid"\t\t"1"\n\t"LastOwner"\t\t"%s"\n}\n' % (appid, flags, installdir, owner))
        folder = os.path.join(lib, "steamapps", "common", installdir)
        for rel, data in files.items():
            write(os.path.join(folder, rel), data)
        return folder


def dr_files(parts=None, override=None):
    out = {}
    for p in parts or vg.GAMES["deltarune"].parts:
        out["data.win" if p == "root" else p + "/data.win"] = (override or {}).get(p, DR[p])
    return out


class VdfTests(unittest.TestCase):
    def test_nested_escapes_comments(self):
        d = vg.parse_vdf('// c\n"a"\n{\n "b" "x\\"y\\\\z" // tail\n "C" { "d" "1" }\n bare val\n}\n')
        self.assertEqual(vg.vget(d, "A", "b"), 'x"y\\z')
        self.assertEqual(vg.vget(d, "a", "c", "D"), "1")
        self.assertEqual(vg.vget(d, "a", "bare"), "val")

    def test_malformed(self):
        for bad in ('"a" {', '"a" "b" }', '"a"', '"a" "unterminated'):
            with self.assertRaises(ValueError):
                vg.parse_vdf(bad)


class SteamTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.t = Tree(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def run_ut(self, **kw):
        return vg.verify("undertale", steam_roots=[self.t.root], gog_dirs=[], allow_list=ALLOW, platform="win32", **kw)

    def test_ok_second_library(self):
        folder = self.t.install("391540", "Undertale", {"data.win": UT})
        r = self.run_ut()
        self.assertEqual(r.status, vg.OK, r.message)
        self.assertEqual(vg._norm(r.folder), vg._norm(folder))
        self.assertEqual(r.store, "steam")
        self.assertIn("test 1.0", r.message)

    def test_old_libraryfolders_format(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        t = Tree(td.name, old_format=True)
        t.install("391540", "Undertale", {"data.win": UT})
        r = vg.verify("undertale", steam_roots=[t.root], gog_dirs=[], allow_list=ALLOW, platform="win32")
        self.assertEqual(r.status, vg.OK, r.message)

    def test_library_in_steam_root(self):
        self.t.install("391540", "Undertale", {"data.win": UT}, lib=self.t.root)
        self.assertEqual(self.run_ut().status, vg.OK)

    def test_not_found(self):
        r = self.run_ut()
        self.assertEqual(r.status, vg.NOT_FOUND)
        self.assertIn("store.steampowered.com/app/391540", r.message)
        self.assertIn("gog.com", r.message)

    def test_files_without_manifest(self):
        write(os.path.join(self.t.lib, "steamapps", "common", "Undertale", "data.win"), UT)
        r = self.run_ut()
        self.assertEqual(r.status, vg.NO_RECORD)
        self.assertIn("no install record", r.message)

    def test_unlisted_library_is_ignored(self):
        stray = os.path.join(self.td.name, "Stray")
        self.t.install("391540", "Undertale", {"data.win": UT}, lib=stray)
        self.assertEqual(self.run_ut().status, vg.NOT_FOUND)

    def test_account_never_signed_in_here(self):
        self.t.install("391540", "Undertale", {"data.win": UT}, owner=OTHER)
        r = self.run_ut()
        self.assertEqual(r.status, vg.ACCOUNT)
        self.assertNotIn(OTHER, r.message + json.dumps(r.details))

    def test_owner_zero_refused(self):
        self.t.users(["0", OWNER])
        self.t.install("391540", "Undertale", {"data.win": UT}, owner="0")
        self.assertEqual(self.run_ut().status, vg.ACCOUNT)

    def test_missing_loginusers(self):
        os.remove(os.path.join(self.t.root, "config", "loginusers.vdf"))
        self.t.install("391540", "Undertale", {"data.win": UT})
        self.assertEqual(self.run_ut().status, vg.ACCOUNT)

    def test_updating(self):
        self.t.install("391540", "Undertale", {"data.win": UT}, flags=6)
        self.assertEqual(self.run_ut().status, vg.NOT_INSTALLED)

    def test_unknown_version(self):
        self.t.install("391540", "Undertale", {"data.win": UT + b"x"})
        r = self.run_ut()
        self.assertEqual(r.status, vg.UNKNOWN_VERSION)
        self.assertIn("Verify integrity", r.message)

    def test_same_size_different_bytes(self):
        self.t.install("391540", "Undertale", {"data.win": bytes(len(UT))})
        self.assertEqual(self.run_ut().status, vg.UNKNOWN_VERSION)

    def test_missing_data_win(self):
        self.t.install("391540", "Undertale", {"UNDERTALE.exe": b"x"})
        r = self.run_ut()
        self.assertEqual(r.status, vg.MISSING_FILES)

    def test_copy_outside_library_refused(self):
        self.t.install("391540", "Undertale", {"data.win": UT})
        copy = os.path.join(self.td.name, "Copy")
        write(os.path.join(copy, "data.win"), UT)
        r = self.run_ut(game_dir=copy)
        self.assertEqual(r.status, vg.NO_RECORD)
        self.assertEqual(r.accepted_parts, [])

    def test_explicit_dir_of_real_install(self):
        folder = self.t.install("391540", "Undertale", {"data.win": UT})
        self.assertEqual(self.run_ut(game_dir=folder + os.sep).status, vg.OK)

    def test_manifest_appid_mismatch_ignored(self):
        write(os.path.join(self.t.lib, "steamapps", "appmanifest_391540.acf"),
              '"AppState" { "appid" "1" "StateFlags" "4" "installdir" "Undertale" "LastOwner" "%s" }' % OWNER)
        write(os.path.join(self.t.lib, "steamapps", "common", "Undertale", "data.win"), UT)
        self.assertEqual(self.run_ut().status, vg.NO_RECORD)

    def test_cli_json(self):
        self.t.install("391540", "Undertale", {"data.win": UT})
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = vg.main(["x", "undertale", "--steam-root", self.t.root, "--gog-dir", os.path.join(self.td.name, "no"),
                          "--json"])
            self.assertEqual(vg.main(["x"]), 2)
            self.assertEqual(vg.main(["x", "undertale", "--bogus", "1"]), 2)
        self.assertEqual(rc, 1)   # the real allow-list refuses fake bytes
        first = json.JSONDecoder().raw_decode(out.getvalue())[0]
        self.assertEqual(first["status"], vg.UNKNOWN_VERSION)
        self.assertFalse(first["accepted"])


class DeltaruneTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.t = Tree(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def run_dr(self, **kw):
        return vg.verify("deltarune", steam_roots=[self.t.root], gog_dirs=[], allow_list=ALLOW, platform="win32", **kw)

    def test_full_game_ok(self):
        self.t.install("1671210", "DELTARUNE", dr_files())
        r = self.run_dr()
        self.assertEqual(r.status, vg.OK, r.message)
        self.assertEqual(len(r.accepted_parts), 6)

    def test_other_account_gets_free_chapters_only(self):
        self.t.install("1671210", "DELTARUNE", dr_files(), owner=OTHER)
        r = self.run_dr()
        self.assertEqual(r.status, vg.OK_FREE, r.message)
        self.assertEqual(set(r.accepted_parts), {"root", "chapter1_windows", "chapter2_windows"})
        self.assertEqual(set(r.refused_parts), {"chapter3_windows", "chapter4_windows", "chapter5_windows"})
        self.assertIn("sign in to Steam", r.message)
        self.assertTrue(r.accepted)

    def test_demo_app(self):
        self.t.install("1690940", "DELTARUNEdemo", dr_files(["root", "chapter1_windows", "chapter2_windows"]))
        r = self.run_dr()
        self.assertEqual(r.status, vg.OK, r.message)
        self.assertIn("Only the free chapters", r.message)

    def test_demo_folder_with_paid_chapters_copied_in(self):
        self.t.install("1690940", "DELTARUNEdemo", dr_files())
        r = self.run_dr()
        self.assertEqual(r.status, vg.OK_FREE)
        self.assertIn("demo includes only Chapters 1 and 2", r.message)

    def test_purchased_preferred_over_demo(self):
        self.t.install("1690940", "DELTARUNEdemo", dr_files(["root", "chapter1_windows", "chapter2_windows"]))
        self.t.install("1671210", "DELTARUNE", dr_files())
        self.assertEqual(self.run_dr().status, vg.OK)

    def test_free_chapters_from_unrecorded_folder(self):
        loose = os.path.join(self.td.name, "itch")
        for rel, data in dr_files(["root", "chapter1_windows", "chapter2_windows"]).items():
            write(os.path.join(loose, rel), data)
        r = self.run_dr(game_dir=loose)
        self.assertEqual(r.status, vg.OK, r.message)

    def test_paid_chapters_from_unrecorded_folder(self):
        loose = os.path.join(self.td.name, "copy")
        for rel, data in dr_files().items():
            write(os.path.join(loose, rel), data)
        r = self.run_dr(game_dir=loose)
        self.assertEqual(r.status, vg.OK_FREE)
        self.assertEqual(len(r.refused_parts), 3)

    def test_unknown_launcher(self):
        self.t.install("1671210", "DELTARUNE", dr_files(override={"root": b"other"}))
        self.assertEqual(self.run_dr().status, vg.UNKNOWN_VERSION)

    def test_one_unknown_chapter(self):
        self.t.install("1671210", "DELTARUNE", dr_files(override={"chapter4_windows": b"modded"}))
        r = self.run_dr()
        self.assertEqual(r.status, vg.OK_FREE)
        self.assertEqual(r.refused_parts, ["chapter4_windows"])
        self.assertIn("Chapter 4 does not match", r.message)


class GogTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.dir = os.path.join(self.td.name, "GOG Games", "Undertale")
        write(os.path.join(self.dir, "data.win"), UT)
        self.allow = [row("undertale", "root", UT, "gog", "1.08 GOG test")]

    def tearDown(self):
        self.td.cleanup()

    def info(self, product="1456487183"):
        write(os.path.join(self.dir, "goggame-1456487183.info"),
              json.dumps({"gameId": product, "rootGameId": product, "name": "Undertale"}))

    def run_gog(self, records, platform="win32", **kw):
        return vg.verify("undertale", steam_roots=[], gog_dirs=[self.dir], gog_records=lambda p: records,
                         allow_list=self.allow, platform=platform, **kw)

    def test_registry_record(self):
        self.info()
        r = self.run_gog([self.dir.replace("\\", "/") + "/"])
        self.assertEqual(r.status, vg.OK, r.message)
        self.assertEqual(r.store, "gog")

    def test_info_without_registry(self):
        self.info()
        self.assertEqual(self.run_gog([]).status, vg.NO_RECORD)

    def test_wrong_product(self):
        self.info("1")
        self.assertEqual(self.run_gog([self.dir]).status, vg.NO_RECORD)

    def test_linux_gameinfo(self):
        game = os.path.join(self.td.name, "lin", "Undertale", "game")
        write(os.path.join(game, "data.win"), UT)
        write(os.path.join(self.td.name, "lin", "Undertale", "gameinfo"), "Undertale\n1.08\n")
        r = vg.verify("undertale", steam_roots=[], gog_dirs=[game], allow_list=self.allow, platform="linux")
        self.assertEqual(r.status, vg.OK, r.message)

    def test_galaxy_db(self):
        db = os.path.join(self.td.name, "galaxy-2.0.db")
        con = sqlite3.connect(db)
        con.execute("CREATE TABLE InstalledBaseProducts (productId INTEGER, installationPath TEXT)")
        con.execute("INSERT INTO InstalledBaseProducts VALUES (1456487183, ?)", (self.dir,))
        con.commit()
        con.close()
        self.assertEqual(vg.galaxy_install_paths(db, "1456487183"), [self.dir])
        self.assertEqual(vg.galaxy_install_paths(db, "2"), [])
        self.assertEqual(vg.galaxy_install_paths(os.path.join(self.td.name, "none.db"), "1"), [])
        recs = vg.default_gog_records("1456487183", registry=lambda *a: None, galaxy_db=db, platform="win32")
        self.assertEqual(recs, [self.dir])

    def test_deltarune_has_no_gog(self):
        self.assertIsNone(vg.GAMES["deltarune"].gog_product)


MAC = blob("ut mac")
MAC_ALLOW = [row("undertale", "root", MAC, label="test 1.0 mac")]
BUNDLE = "UNDERTALE.app/Contents/Resources/"


class MacTests(unittest.TestCase):
    """UNDERTALE's macOS depot: game.ios in the bundle, its own rows, the same install record rule."""

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.t = Tree(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def run_ut(self, **kw):
        return vg.verify("undertale", steam_roots=[self.t.root], gog_dirs=[], allow_list=ALLOW,
                         macos_allow_list=MAC_ALLOW, platform="darwin", **kw)

    def test_mac_layout_accepted(self):
        folder = self.t.install("391540", "Undertale", {BUNDLE + "game.ios": MAC})
        r = self.run_ut()
        self.assertEqual(r.status, vg.OK, r.message)
        self.assertEqual((r.layout, r.version, r.store), ("macos", "test 1.0 mac", "steam"))
        self.assertEqual(vg._norm(r.data_dir), vg._norm(os.path.join(folder, *BUNDLE.split("/")[:3])))

    def test_bundle_name_case(self):
        self.t.install("391540", "Undertale", {"undertale.app/Contents/Resources/game.ios": MAC})
        self.assertEqual(self.run_ut().status, vg.OK)

    def test_pick_inside_bundle(self):
        folder = self.t.install("391540", "Undertale", {BUNDLE + "game.ios": MAC})
        for pick in ("UNDERTALE.app", "UNDERTALE.app/Contents", "UNDERTALE.app/Contents/Resources"):
            r = self.run_ut(game_dir=os.path.join(folder, *pick.split("/")))
            self.assertEqual(r.status, vg.OK, (pick, r.message))
            self.assertEqual(vg._norm(r.folder), vg._norm(folder))

    def test_windows_file_in_mac_layout_refused(self):
        self.t.install("391540", "Undertale", {BUNDLE + "game.ios": UT})
        r = self.run_ut()
        self.assertEqual((r.status, r.layout), (vg.UNKNOWN_VERSION, "macos"))

    def test_mac_file_as_data_win_refused(self):
        self.t.install("391540", "Undertale", {"data.win": MAC})
        r = self.run_ut()
        self.assertEqual((r.status, r.layout), (vg.UNKNOWN_VERSION, "windows"))

    def test_windows_files_next_to_bundle_checked_as_windows(self):
        self.t.install("391540", "Undertale", {"data.win": UT, BUNDLE + "game.ios": MAC})
        r = self.run_ut()
        self.assertEqual((r.status, r.layout, r.version), (vg.OK, "windows", "test 1.0"))

    def test_same_ownership_rule(self):
        self.t.install("391540", "Undertale", {BUNDLE + "game.ios": MAC}, owner=OTHER)
        self.assertEqual(self.run_ut().status, vg.ACCOUNT)
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        write(os.path.join(td.name, BUNDLE + "game.ios"), MAC)
        self.assertEqual(self.run_ut(game_dir=td.name).status, vg.NO_RECORD)

    def test_deltarune_has_no_mac_layout(self):
        self.t.install("1671210", "DELTARUNE", {BUNDLE + "game.ios": MAC})
        r = vg.verify("deltarune", steam_roots=[self.t.root], gog_dirs=[], allow_list=ALLOW,
                      macos_allow_list=MAC_ALLOW + [row("deltarune", "root", MAC)], platform="darwin")
        self.assertEqual(r.status, vg.MISSING_FILES)

    def test_real_rows_are_separate(self):
        win = {(s, d) for g, p, s, d, _, _ in vg.ALLOW_LIST}
        mac = {(s, d) for g, p, s, d, _, _ in vg.MACOS_ALLOW_LIST}
        self.assertTrue(mac)
        self.assertFalse(win & mac)


class DiscoveryTests(unittest.TestCase):
    def test_registry_and_linux_roots(self):
        with tempfile.TemporaryDirectory() as td:
            reg_root = os.path.join(td, "RegSteam")
            os.makedirs(os.path.join(reg_root, "steamapps"))
            roots = vg.default_steam_roots(registry=lambda h, k, v: reg_root if v == "SteamPath" else None,
                                           platform="win32")
            self.assertEqual(roots[0], reg_root)
            lin = os.path.join(td, ".local", "share", "Steam")
            os.makedirs(os.path.join(lin, "steamapps"))
            self.assertEqual([vg._norm(r) for r in vg.default_steam_roots(home=td, platform="linux")], [vg._norm(lin)])

    def test_allow_list_covers_compat_table(self):
        """Every version the 3DS build knows (platform/3ds/data_versions.inc) is an official release here."""
        import re
        inc = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "platform", "3ds", "data_versions.inc")
        with open(inc, encoding="utf-8") as f:
            text = f.read()
        rows = set(re.findall(r'DATA_VERSION\("([^"]+)", "([^"]+)", (\d+)ULL, "([0-9a-f]{64})"\)', text))
        mine = {(g, p, str(s), d) for g, p, s, d, _, _ in vg.ALLOW_LIST + vg.MACOS_ALLOW_LIST}
        self.assertTrue(rows)
        self.assertLessEqual(rows, mine)


if __name__ == "__main__":
    unittest.main()
