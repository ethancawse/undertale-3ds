#!/usr/bin/env python3
"""Tests for the builder app (tools/release/builder/) with fixture Steam/GOG trees, registry exports, fake hashes, a
mocked toolchain, a fake build process and a local mock FTP server. No game data, no network, nothing installed,
no console: py -3 -I tools/release/test_builder.py"""
import contextlib
import ftplib
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import socketserver
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "builder"))
sys.dont_write_bytecode = True
import builder as app  # noqa: E402
import deliver  # noqa: E402
import discover  # noqa: E402
import pipeline  # noqa: E402
import products  # noqa: E402
import texcache  # noqa: E402
import toolchain  # noqa: E402
from products import verify_game as vg  # noqa: E402

texcache.ALLOW_SOURCE_BUILD = False   # never compile texprep in these tests (test_texcache.py covers it)
FIX = os.path.join(HERE, "builder", "fixtures")
OWNER = "76561190000000001"
UT = b"fake UNDERTALE data.win for the builder tests" * 7
ALLOW = [("undertale", "root", len(UT), hashlib.sha256(UT).hexdigest(), "steam", "test 1.0")]


def fixture(name, **subs):
    with open(os.path.join(FIX, name), encoding="utf-8") as f:
        text = f.read()
    for k, v in subs.items():
        text = text.replace("{%s}" % k, v)
    return text


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode, **({} if isinstance(data, bytes) else {"encoding": "utf-8", "newline": "\n"})) as f:
        f.write(data)


def vdf_path(p):
    return p.replace("\\", "\\\\")


def game_folder(path, data=UT, oggs=3, credits=True):
    write(os.path.join(path, "data.win"), data)
    if credits:
        write(os.path.join(path, "credits.txt"), "names\n")
    for i in range(oggs):
        write(os.path.join(path, "mus_%d.ogg" % i), b"ogg" * (i + 1))
    write(os.path.join(path, "UNDERTALE.exe"), b"MZ")
    return path


class SteamTree:
    """A Steam client at <tmp>/Steam with a second library at <tmp>/Lib, built from the fixture files."""

    def __init__(self, tmp, state="4", owner=OWNER, install=True):
        self.root = os.path.join(tmp, "Steam")
        self.lib = os.path.join(tmp, "Lib")
        write(os.path.join(self.root, "steamapps", "libraryfolders.vdf"),
              fixture("libraryfolders.vdf", STEAM=vdf_path(self.root), LIB=vdf_path(self.lib)))
        write(os.path.join(self.root, "config", "loginusers.vdf"), fixture("loginusers.vdf"))
        write(os.path.join(self.lib, "steamapps", "appmanifest_391540.acf"),
              fixture("appmanifest_391540.acf", STATE=state, OWNER=owner))
        self.folder = os.path.join(self.lib, "steamapps", "common", "Undertale")
        self.save_dir = os.path.join(tmp, "pc-saves")
        if install:
            game_folder(self.folder)

    def probes(self, save_dir=None):
        return discover.Probes(steam_roots=[self.root], gog_dirs=[], registry=lambda *a: None, platform="win32",
                               save_dir=save_dir or self.save_dir)


# ---------------------------------------------------------------------------------------------------------------------

class RegistryExportTest(unittest.TestCase):
    def test_parse_values_and_wow64_view(self):
        reg = discover.registry_from_export(fixture("gog.reg", GOG_REG="C:\\\\GOG Games\\\\Undertale"))
        self.assertEqual(reg("HKEY_LOCAL_MACHINE", r"SOFTWARE\GOG.com\Games\1456487183", "path"),
                         "C:\\GOG Games\\Undertale")
        self.assertEqual(reg("HKEY_LOCAL_MACHINE", r"SOFTWARE\WOW6432Node\GOG.com\Games\1456487183", "PATH"),
                         "C:\\GOG Games\\Undertale")
        self.assertEqual(reg("HKEY_LOCAL_MACHINE", r"software\gog.com\games\1456487183", "buildId"), "12")
        self.assertIsNone(reg("HKEY_CURRENT_USER", r"SOFTWARE\GOG.com\Games\1456487183", "path"))

    def test_utf16_export_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "steam.reg")
            with open(p, "wb") as f:
                f.write(fixture("steam.reg", STEAM_FWD="d:/steam", STEAM_REG="D:\\\\Steam").encode("utf-16"))
            reg = discover.registry_from_export(discover.read_reg_file(p))
            self.assertEqual(reg("HKEY_CURRENT_USER", r"Software\Valve\Steam", "SteamPath"), "d:/steam")
            self.assertEqual(reg("HKEY_LOCAL_MACHINE", r"SOFTWARE\Valve\Steam", "InstallPath"), "D:\\Steam")


class MusicFixtures(unittest.TestCase):
    def setUp(self):
        entries = [dict(name='mus_%d.ogg' % i, size=3 * (i + 1),
                        sha256=hashlib.sha256(b'ogg' * (i + 1)).hexdigest()) for i in range(3)]
        patch = mock.patch.object(products, 'load_music_manifest', return_value=entries)
        patch.start()
        self.addCleanup(patch.stop)


class DiscoveryTest(MusicFixtures):
    def test_steam_library_from_fixture_vdf(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            copies = discover.find_copies("undertale", t.probes())
            self.assertEqual(len(copies), 1)
            c = copies[0]
            self.assertEqual((c.store, os.path.normcase(c.folder)), ("Steam", os.path.normcase(t.folder)))
            self.assertEqual(c.version, "1.08 (Steam build 4279148)")     # the allow-list label for that buildid
            self.assertEqual(c.note, "")

    def test_steam_root_from_registry_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            reg = discover.registry_from_export(fixture("steam.reg", STEAM_FWD=t.root.replace("\\", "/"),
                                                        STEAM_REG=vdf_path(t.root)))
            probes = discover.Probes(registry=reg, gog_dirs=[], platform="win32")
            self.assertIn(os.path.normcase(os.path.realpath(t.root)),
                          [os.path.normcase(os.path.realpath(r)) for r in probes.roots()])
            # The default Steam folders are probed too (this PC may have a real one), so look for ours.
            folders = [os.path.normcase(c.folder) for c in discover.find_copies("undertale", probes)]
            self.assertIn(os.path.normcase(t.folder), folders)

    def test_updating_install_is_listed_with_a_note(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp, state="6")
            c = discover.find_copies("undertale", t.probes())[0]
            self.assertIn("still installing", c.note)

    def test_nothing_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp, install=False)
            self.assertEqual(discover.find_copies("undertale", t.probes()), [])

    def test_demo_app_is_labelled(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.lib, "steamapps", "appmanifest_1690940.acf"),
                  fixture("appmanifest_1690940.acf", OWNER=OWNER))
            os.makedirs(os.path.join(t.lib, "steamapps", "common", "DELTARUNEdemo"))
            copies = discover.find_copies("deltarune", t.probes())
            self.assertEqual([c.store for c in copies], ["Steam (demo)"])

    def test_gog_from_registry_export_and_verify(self):
        with tempfile.TemporaryDirectory() as tmp:
            gog = game_folder(os.path.join(tmp, "GOG Games", "Undertale"))
            write(os.path.join(gog, "goggame-1456487183.info"), '{"gameId": "1456487183", "name": "Undertale"}')
            reg = discover.registry_from_export(fixture("gog.reg", GOG_REG=vdf_path(gog)))
            probes = discover.Probes(steam_roots=[], registry=reg, galaxy_db=os.path.join(tmp, "none.db"),
                                     platform="win32")
            copies = discover.find_copies("undertale", probes)
            self.assertEqual([(c.store, os.path.normcase(c.folder)) for c in copies], [("GOG", os.path.normcase(gog))])
            v = discover.verify_copy("undertale", gog, probes, allow_list=ALLOW)
            self.assertFalse(v.accepted)
            self.assertIn('GOG support will follow', v.message)
            self.assertEqual(v.result.store, "gog")

    def test_gog_galaxy_database(self):
        with tempfile.TemporaryDirectory() as tmp:
            gog = game_folder(os.path.join(tmp, "Galaxy", "Undertale"))
            write(os.path.join(gog, "goggame-1456487183.info"), '{"gameId": "1456487183"}')
            db = os.path.join(tmp, "galaxy-2.0.db")
            con = sqlite3.connect(db)
            con.execute("CREATE TABLE InstalledBaseProducts (productId INTEGER, installationPath TEXT)")
            con.execute("INSERT INTO InstalledBaseProducts VALUES (1456487183, ?)", (gog,))
            con.commit()
            con.close()
            probes = discover.Probes(steam_roots=[], registry=lambda *a: None, galaxy_db=db, platform="win32")
            self.assertEqual(len(discover.find_copies("undertale", probes)), 1)
            self.assertFalse(discover.verify_copy("undertale", gog, probes, allow_list=ALLOW).accepted)


class VerifyTest(MusicFixtures):
    def test_music_same_size_corruption_names_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.folder, 'mus_1.ogg'), b'badbad')
            verdict = discover.verify_copy('undertale', t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(verdict.accepted)
            self.assertIn('mus_1.ogg has the wrong SHA-256', verdict.message)
            self.assertIn('Verify integrity of game files', verdict.message)

    def test_music_wrong_size_names_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.folder, 'mus_1.ogg'), b'bad')
            verdict = discover.verify_copy('undertale', t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(verdict.accepted)
            self.assertIn('mus_1.ogg has the wrong size', verdict.message)

    def test_missing_music_names_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            os.remove(os.path.join(t.folder, 'mus_1.ogg'))
            verdict = discover.verify_copy('undertale', t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(verdict.accepted)
            self.assertIn('mus_1.ogg is missing', verdict.message)

    def test_extra_music_is_not_copied_unverified(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.folder, 'custom.ogg'), b'custom')
            verdict = discover.verify_copy('undertale', t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(verdict.accepted)
            self.assertIn('Unexpected music file custom.ogg', verdict.message)

    def test_official_steam_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            v = discover.verify_copy("undertale", t.folder, t.probes(), allow_list=ALLOW)
            self.assertTrue(v.accepted, v.message)
            self.assertIn("3 music files", v.message)
            self.assertEqual(v.store_links, ())

    def test_unknown_version_shows_store_links_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.folder, "data.win"), UT[:-1] + b"!")
            v = discover.verify_copy("undertale", t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(v.accepted)
            self.assertIn("don't match an official release", v.message)
            self.assertIn("store.steampowered.com", " ".join(v.store_links))

    def test_account_not_signed_in_here(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp, owner="76561190000000099")
            v = discover.verify_copy("undertale", t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(v.accepted)
            self.assertIn("has not signed in to Steam on this computer", v.message)

    def test_hand_picked_folder_without_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            loose = game_folder(os.path.join(tmp, "copy"))
            v = discover.verify_copy("undertale", loose, t.probes(), allow_list=ALLOW)
            self.assertFalse(v.accepted)
            self.assertIn("isn't a Steam or GOG installation", v.message)

    def test_missing_credits(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            os.remove(os.path.join(t.folder, "credits.txt"))
            v = discover.verify_copy("undertale", t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(v.accepted)
            self.assertIn("credits.txt is missing", v.message)

    def test_music_count_for_a_known_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = game_folder(os.path.join(tmp, "g"), oggs=3)
            ok, msg = products.music_check(products.PRODUCTS["undertale"], f, "1.08 (Steam build 4279148)")
            self.assertFalse(ok)
            self.assertIn("only 3 of the 212 music files", msg)

    def test_no_music(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = game_folder(os.path.join(tmp, "g"), oggs=0)
            ok, msg = products.music_check(products.PRODUCTS["undertale"], f, "test 1.0")
            self.assertFalse(ok)
            self.assertIn("music files (.ogg) are missing", msg)


# ---------------------------------------------------------------------------------------------------------------------

def fake_devkitpro(tmp, files=toolchain.REQUIRED_FILES, cia=True):
    root = os.path.join(tmp, "devkitPro")
    write(os.path.join(root, "msys2", "usr", "bin", "bash.exe"), b"")
    for f in files:
        write(os.path.join(root, f), b"")
    if cia:
        write(os.path.join(root, "tools", "bin", "makerom.exe"), b"")
        write(os.path.join(root, "tools", "bin", "bannertool.exe"), b"")
    return root


def pacman(packages):
    return lambda argv, **kw: (0, "".join("%s 1.0-1\n" % p for p in packages))


class Boom:
    def __call__(self, *a, **kw):
        raise AssertionError("the dry run must not call this")


class ToolchainTest(unittest.TestCase):
    ALL = toolchain.REQUIRED_PACKAGES + toolchain.RECOMMENDED_PACKAGES

    def test_everything_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_devkitpro(tmp)
            tc = toolchain.detect(os.path.join(tmp, "tools"), platform="win32", run=pacman(self.ALL), roots=[root])
            self.assertTrue(tc.can_build, tc.checks)
            self.assertTrue(tc.can_cia)
            self.assertEqual(toolchain.plan(tc, "t", "d", want_cia=True), [])

    def test_no_devkitpro(self):
        with tempfile.TemporaryDirectory() as tmp:
            tc = toolchain.detect(os.path.join(tmp, "tools"), platform="win32", run=Boom(),
                                  roots=[os.path.join(tmp, "nope")])
            self.assertFalse(tc.can_build)
            keys = [s.key for s in toolchain.plan(tc, "t", "d", want_cia=True)]
            self.assertEqual(keys, ["devkitpro", "packages", "makerom", "bannertool"])
            self.assertEqual([s.key for s in toolchain.plan(tc, "t", "d", want_cia=False)], ["devkitpro", "packages"])

    def test_missing_packages_and_cia_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_devkitpro(tmp, cia=False)
            have = [p for p in self.ALL if p not in ("citro3d", "3ds-libogg")]
            tc = toolchain.detect(os.path.join(tmp, "tools"), platform="win32", run=pacman(have), roots=[root])
            self.assertFalse(tc.can_build)
            self.assertEqual(tc.missing_packages, ["citro3d"])
            self.assertEqual(tc.missing_recommended, ["3ds-libogg"])
            self.assertEqual([s.key for s in toolchain.plan(tc, "t", "d", True)], ["packages", "makerom", "bannertool"])

    def test_cia_tools_in_builder_folder_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_devkitpro(tmp, cia=False)
            tools = os.path.join(tmp, "tools")
            write(os.path.join(tools, "makerom.exe"), b"")
            write(os.path.join(tools, "bannertool.exe"), b"")
            tc = toolchain.detect(tools, platform="win32", run=pacman(self.ALL), roots=[root])
            self.assertTrue(tc.can_cia)

    def test_installer_env_value_is_not_a_windows_path(self):
        self.assertEqual(toolchain.candidate_roots({"DEVKITPRO": "/opt/devkitpro"}, "win32"),
                         [os.path.normpath("C:/devkitPro")])
        self.assertEqual(toolchain.candidate_roots({"DEVKITPRO": "D:/dkp"}, "win32")[0], os.path.normpath("D:/dkp"))

    def test_dry_run_runs_nothing(self):
        logged = []
        actions = toolchain.Actions(download=Boom(), extract=Boom(), elevated=Boom(), run=Boom())
        for s in toolchain.full_plan("tools", "dl", platform="win32") + [toolchain.update_step(None, platform="win32")]:
            self.assertTrue(toolchain.run_step(s, logged.append, True, actions))
        text = "\n".join(logged)
        self.assertIn("devkitProUpdater-3.0.3.exe", text)
        self.assertIn("pacman -S --needed --noconfirm 3ds-dev 3ds-libvorbisidec 3ds-libogg cmake make", text)
        self.assertIn(toolchain.CIA_TOOLS["makerom"]["sha256"], text)
        self.assertIn("pacman -Syu", text)
        self.assertNotIn(" -s ", text)

    def test_dry_run_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            tc = toolchain.detect(os.path.join(tmp, "t"), platform="win32", run=Boom(), roots=[os.path.join(tmp, "x")])
            rep = toolchain.dry_run_report(tc, "t", "d")
        self.assertNotIn("NOT PINNED", rep)
        self.assertIn("download https://github.com/devkitPro/installer/releases/download/v3.0.3/"
                      "devkitProUpdater-3.0.3.exe", rep)
        self.assertIn("SHA-256 038a99dc84f1ca0b52e9e0e074a94a3b0672e6d7bf0988563f0ab0812dcbb38d", rep)
        self.assertIn("size 196304 bytes", rep)
        self.assertIn("needs administrator permission", rep)
        self.assertIn("tick 3DS Development", rep)
        self.assertIn("nothing was downloaded, installed or changed", rep)
        self.assertIn("github.com/3DSGuy/Project_CTR", rep)

    def test_unpinned_download_is_refused(self):
        with self.assertRaises(toolchain.Refused):
            toolchain.download("https://example.invalid/x.exe", "x.exe", None, opener=Boom())

    def test_download_checks_the_digest(self):
        body = b"pretend installer"

        class Resp(io.BytesIO):
            headers = {"Content-Length": str(len(body))}

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False
        with tempfile.TemporaryDirectory() as tmp:
            dest = os.path.join(tmp, "dl", "a.zip")
            toolchain.download("u", dest, hashlib.sha256(body).hexdigest(), opener=lambda u, timeout: Resp(body))
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), body)
            bad = os.path.join(tmp, "dl", "b.zip")
            with self.assertRaises(toolchain.Refused):
                toolchain.download("u", bad, "0" * 64, opener=lambda u, timeout: Resp(body))
            self.assertFalse(os.path.exists(bad) or os.path.exists(bad + ".part"))

    def test_pinned_installer_size_and_hash_mismatch_are_refused(self):
        pin = toolchain.DEVKITPRO_INSTALLER
        self.assertEqual((pin["version"], pin["size"], pin["sha256"]),
                         ("3.0.3", 196304, "038a99dc84f1ca0b52e9e0e074a94a3b0672e6d7bf0988563f0ab0812dcbb38d"))

        class Resp(io.BytesIO):
            headers = {}

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False
        right_size = b"\0" * pin["size"]                 # the right size, the wrong bytes
        with tempfile.TemporaryDirectory() as tmp:
            for body, why in ((b"\0" * (pin["size"] - 1), "196303 bytes"), (b"\0" * (pin["size"] + 5), "bytes"),
                              (right_size, "SHA-256")):
                dest = os.path.join(tmp, "dl", "devkitProUpdater-3.0.3.exe")
                with self.assertRaises(toolchain.Refused) as e:
                    toolchain.download(pin["url"], dest, pin["sha256"], opener=lambda u, timeout: Resp(body),
                                       size=pin["size"])
                self.assertIn(why, str(e.exception))
                self.assertFalse(os.path.exists(dest) or os.path.exists(dest + ".part"))
            # The real step passes the pinned size and hash through to the download.
            got = []
            step = [s for s in toolchain.full_plan("t", os.path.join(tmp, "dl"), platform="win32") if s.key == "devkitpro"][0]
            acts = toolchain.Actions(download=lambda url, dest, sha, size=None: got.append((url, sha, size)),
                                     elevated=lambda p: 0, run=Boom())
            toolchain.run_step(step, lambda s: None, False, acts)
            self.assertEqual(got, [(pin["url"], pin["sha256"], pin["size"])])

    def test_extract_and_real_step_with_mocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            z = os.path.join(tmp, "dl", "makerom-v0.18.4-win_x86_64.zip")
            os.makedirs(os.path.dirname(z))
            with zipfile.ZipFile(z, "w") as f:
                f.writestr("makerom-v0.18.4/makerom.exe", b"MZ fake")
            tools = os.path.join(tmp, "tools")
            step = [s for s in toolchain.full_plan(tools, os.path.join(tmp, "dl"), platform="win32") if s.key == "makerom"][0]
            got = []
            actions = toolchain.Actions(download=lambda url, dest, sha, size=None: got.append((url, sha)))
            toolchain.run_step(step, lambda s: None, False, actions)
            self.assertEqual(got, [(toolchain.CIA_TOOLS["makerom"]["url"], toolchain.CIA_TOOLS["makerom"]["sha256"])])
            self.assertTrue(os.path.isfile(os.path.join(tools, "makerom.exe")))

    def test_package_failure_message_and_admin_path(self):
        steps = {s.key: s for s in toolchain.full_plan("t", "d", platform="win32")}
        with self.assertRaises(toolchain.Refused) as e:
            toolchain.run_step(steps["packages"], lambda s: None, False, toolchain.Actions(run=lambda argv, log: 1))
        self.assertIn("couldn't install", str(e.exception))
        elevated = []
        acts = toolchain.Actions(download=lambda *a, **kw: None, elevated=lambda p: elevated.append(p) or 0, run=Boom())
        toolchain.run_step(steps["devkitpro"], lambda s: None, False, acts)
        self.assertEqual(len(elevated), 1)


# ---------------------------------------------------------------------------------------------------------------------

class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class ProgressTest(unittest.TestCase):
    def test_stages_and_estimate(self):
        clk = Clock()
        p = pipeline.Progress(products.PRODUCTS["undertale"], "n3ds", clk)
        self.assertEqual(p.estimate()[0], 147.2)
        p.feed("AOT: generating UNDERTALE code for n3ds from D:/x\n")
        clk.t = 15.0                                       # twice the measured 7.5 s
        p.feed("-- Configuring done (1.0s)\n")
        self.assertEqual((p.stage, p.aot_seconds, p.speed_factor()), (0, 15.0, 2.0))
        lo, hi = p.estimate()
        self.assertTrue(lo < 294.4 < hi)
        clk.t = 65.0
        p.feed("[ 50%] Building C object CMakeFiles/ctrport.dir/x.c.obj\n")
        self.assertEqual(p.percent, 50)
        self.assertTrue(p.estimate()[0] > 0)
        p.feed("/x/y.c:1:2: warning: unused\n")
        p.feed("[100%] Linking C executable undertale-3ds.elf\n")
        p.feed("built /w/src/build/3ds-undertale-n3ds/undertale-3ds-n3ds.3dsx (with AOT from /w)\n")
        self.assertEqual((p.stage, p.warnings, len(p.outputs)), (3, 1, 1))
        self.assertFalse(p.no_aot)
        p.feed("WARNING: building WITHOUT AOT: no UNDERTALE game data found.\n")
        self.assertTrue(p.no_aot)
        self.assertIn("left", pipeline.format_eta((100, 400)))

    def test_support_log_filter(self):
        home = os.path.expanduser("~")
        text = "AOT: generating\nAOT:   skip gml_Script_x\ncc gml_Object_y.c\n%s\\AppData\\x\nok\n" % home
        out = pipeline.filter_log(text)
        self.assertNotIn("gml_", out)
        self.assertNotIn(home, out)
        self.assertIn("%USERPROFILE%", out)
        self.assertIn("AOT: generating", out)
        self.assertEqual(out.count("[line removed: game identifiers]"), 2)


def fake_source(tmp):
    src = os.path.join(tmp, "src")
    write(os.path.join(src, "engine", "src", "vm.c"), "int x;\n")
    write(os.path.join(src, "engine", "LICENSE"), "AGPL\n")
    write(os.path.join(src, "platform", "3ds", "build.sh"), "#!/bin/sh\n")
    write(os.path.join(src, "platform", "3ds", "stray.win"), b"never copied")
    write(os.path.join(src, "platform", "3ds", "build", "junk.o"), b"x")
    write(os.path.join(src, "tools", "aot", "prepare.py"), "print()\n")
    write(os.path.join(src, "VERSION"), "0.1.0\n")     # what the release export puts there (readiness S8)
    with open(os.path.join(HERE, '../../docs/release/build-contract.json'), encoding='utf-8') as source:
        write(os.path.join(src, 'docs/release/build-contract.json'), source.read())
    for rel in pipeline.SOURCE_FILES + ('tools/release/release_contract.py', 'tools/release/verify_game.py',
                                       'tools/release/music-versions.json'):
        if not os.path.isfile(os.path.join(src, rel)):
            write(os.path.join(src, rel), 'fixture\n')
    descriptor = products.release_contract.source_descriptor('undertale', '0.1.0',
        products.release_contract.payload_digest(pipeline.source_payload(src, 'undertale')))
    descriptor['public_commit'] = 'a' * 40
    import json
    write(os.path.join(src, products.release_contract.DESCRIPTOR), json.dumps(descriptor))
    return src


class FakeProc:
    def __init__(self, lines, rc, on_start=None):
        self.stdout = iter(lines)
        self.rc = rc
        self.pid = 999999
        if on_start:
            on_start()

    def wait(self):
        return self.rc

    def poll(self):
        return self.rc


class BuildTest(unittest.TestCase):
    def setUp(self):
        compiler = mock.patch.object(toolchain, 'compiler_details', return_value={
            'path': '/fixture/gcc', 'version': '16.1.0', 'target': 'arm-none-eabi'})
        compiler.start()
        self.addCleanup(compiler.stop)
        self.tmp = tempfile.mkdtemp()
        self.src = fake_source(self.tmp)
        self.work = os.path.join(self.tmp, "work")
        self.tc = toolchain.Toolchain("C:/devkitPro", "C:/devkitPro/msys2/usr/bin/bash.exe", [], [], [], None, None,
                                      None, "win32")
        self.build_id = "archive-0.1.0+aot12"       # what relprep's build_id.cmake writes for this source

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def outputs(self, aot=True, kinds=("3dsx",)):
        bdir = os.path.join(self.work, "src", "build", "3ds-undertale-n3ds")
        write(os.path.join(bdir, "CMakeCache.txt"), "CTRPORT_AOT_DIR:PATH=%s\n" % ("/w/aot" if aot else ""))
        if self.build_id is not None:
            write(os.path.join(bdir, "generated", "ctrport_build_id.h"),
                  '#pragma once\n#define CTRPORT_COMMIT_TEXT "%s"\n' % self.build_id)
        for k in kinds:
            write(os.path.join(bdir, "undertale-3ds-n3ds." + k), b"3DSX" * 100)

    def make(self, lines, rc=0, aot=True, kinds=("3dsx",), outputs=("3dsx",)):
        seen = {}

        def popen(argv, **kw):
            seen["argv"], seen["env"] = argv, kw["env"]
            return FakeProc(lines, rc, on_start=lambda: rc == 0 and self.outputs(aot, kinds))
        b = pipeline.Build(products.PRODUCTS["undertale"], self.tc, self.work, "n3ds", "D:\\Games\\Undertale",
                           outputs=outputs, popen=popen, source_root=self.src, art_generator=lambda *a: {})
        return b, seen

    def test_successful_build(self):
        lines = ["AOT: generating UNDERTALE code for n3ds\n", "-- Configuring done\n", "[ 50%] x\n", "[100%] y\n",
                 "built /w/undertale-3ds-n3ds.3dsx (with AOT from /w/aot)\n"]
        b, seen = self.make(lines)
        arts, rec = b.run()
        self.assertEqual([a.kind for a in arts], ["3dsx"])
        self.assertEqual(arts[0].sha256, hashlib.sha256(b"3DSX" * 100).hexdigest())
        self.assertEqual((rec["version"], rec["build_id"]), ("0.1.0", "archive-0.1.0+aot12"))
        self.assertIn("data=D:/Games/Undertale", seen["argv"])
        self.assertIn("openh264=off", seen["argv"])
        self.assertEqual(seen["env"]["CTRPORT_CCACHE_DIR"], os.path.join(self.work, "ccache"))
        with open(os.path.join(self.work, "src", "VERSION"), "rb") as f:      # copied as is, never written
            self.assertEqual(f.read(), b"0.1.0\n")
        self.assertFalse(os.path.exists(os.path.join(self.work, "src", "platform", "3ds", "stray.win")))
        self.assertFalse(os.path.exists(os.path.join(self.work, "src", "platform", "3ds", "build")))
        with open(os.path.join(self.work, "bin", "python3"), encoding="utf-8") as f:
            self.assertTrue(f.read().startswith("#!/bin/sh\nexec "))

    def test_exact_source_overrides_ambient_url_and_is_recorded(self):
        b, seen = self.make(['[100%] y\n'])
        with mock.patch.dict(os.environ, {'CTRPORT_RELEASE_SOURCE_URL': 'https://example.invalid/wrong'}):
            _, record = b.run()
        expected = 'https://github.com/ethancawse/undertale-3ds/tree/' + 'a' * 40
        self.assertEqual(seen['env']['CTRPORT_RELEASE_SOURCE_URL'], expected)
        self.assertEqual(record['release_source']['source_url'], expected)
        self.assertEqual(record['compiler']['version'], '16.1.0')
        self.assertTrue(record['compiler']['layout_pins'])
        self.assertIn(expected, Path(b.log_path).read_text())

    def test_unset_or_changed_source_stops_before_build(self):
        path = Path(self.src) / products.release_contract.DESCRIPTOR
        original = path.read_text()
        for failure in ('missing', 'unset', 'game', 'version', 'source', 'added'):
            with self.subTest(failure=failure):
                path.write_text(original)
                descriptor = json.loads(original)
                if failure == 'missing':
                    path.unlink()
                elif failure in ('unset', 'game', 'version'):
                    descriptor[{'unset': 'public_commit', 'game': 'game', 'version': 'version'}[failure]] = {
                        'unset': None, 'game': 'deltarune', 'version': '0.2.0'}[failure]
                    path.write_text(json.dumps(descriptor))
                elif failure == 'source':
                    write(os.path.join(self.src, 'engine/src/vm.c'), 'changed fixture\n')
                else:
                    write(os.path.join(self.src, 'engine/src/added.c'), 'added fixture\n')
                b, seen = self.make([])
                with self.assertRaises(pipeline.BuildFailed):
                    b.run()
                self.assertNotIn('argv', seen)
                self.assertFalse(os.path.isdir(os.path.join(self.work, 'src')))
                write(os.path.join(self.src, 'engine/src/vm.c'), 'int x;\n')

    def test_other_compilers_warn_and_still_build_on_each_host(self):
        for platform in ('win32', 'linux', 'darwin'):
            for version in ('17.1.0', 'unknown'):
                with self.subTest(platform=platform, version=version):
                    self.tc.platform = platform
                    b, seen = self.make(['[100%] y\n'])
                    with mock.patch.object(toolchain, 'compiler_details', return_value={
                            'path': '/fixture/gcc', 'version': version, 'target': 'arm-none-eabi'}):
                        _, record = b.run()
                    self.assertIn('argv', seen)
                    self.assertFalse(record['compiler']['layout_pins'])
                    self.assertIn('16.1.0', record['compiler']['warning'])
                    self.assertIn('performance may differ slightly.', record['compiler']['warning'])

    def test_no_aot_is_refused(self):
        b, _ = self.make(["WARNING: building WITHOUT AOT: no UNDERTALE game data found\n", "[100%] y\n"])
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("fast (AOT) code", str(e.exception))
        self.assertTrue(b.cancelled)

    def test_finished_without_aot_config_is_refused(self):
        b, _ = self.make(["[100%] y\n"], aot=False)
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("without AOT", str(e.exception))

    def test_failed_build(self):
        b, _ = self.make(["error: something\n"], rc=2)
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("The build stopped", str(e.exception))
        self.assertIn("exit code 2", str(e.exception))
        self.assertTrue(os.path.isfile(e.exception.log_path))

    def test_missing_cia(self):
        b, _ = self.make(["[100%] y\n"], outputs=("3dsx", "cia"))
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("The .cia was not made", str(e.exception))

    def test_cancel_before_start(self):
        b, _ = self.make([])
        b.cancel()
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("cancelled", str(e.exception))

    def test_low_disk(self):
        b, _ = self.make([])
        with mock.patch.object(pipeline.shutil, "disk_usage", return_value=mock.Mock(free=10 << 20)):
            with self.assertRaises(pipeline.BuildFailed) as e:
                b.run()
        self.assertIn("MB free", str(e.exception))

    def test_missing_version_stops_before_anything_is_written(self):
        os.remove(os.path.join(self.src, "VERSION"))
        b, seen = self.make(["[100%] y\n"])
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("source archive is incomplete", str(e.exception))
        self.assertIn("no VERSION file", str(e.exception))
        self.assertNotIn("argv", seen)
        self.assertFalse(os.path.exists(os.path.join(self.work, "src")))

    def test_malformed_version_is_refused(self):
        for bad in ("0.1\n", "0.1.0-test\n", "v0.1.0\n", "\n", "1.2.3.4\n"):
            write(os.path.join(self.src, "VERSION"), bad)
            with self.assertRaises(pipeline.BuildFailed) as e:
                pipeline.release_version(self.src)
            self.assertIn("not major.minor.patch", str(e.exception))
        write(os.path.join(self.src, "VERSION"), "12.34.15\r\n")
        self.assertEqual(pipeline.release_version(self.src), "12.34.15")

    def test_beta_label_and_numeric_allocation(self):
        write(os.path.join(self.src, 'VERSION'), '1.0.0-beta.1\n')
        self.assertEqual(pipeline.release_version(self.src), '1.0.0-beta.1')
        contract = products.release_contract.read(self.src)
        self.assertEqual(contract['numeric_version'], '1.0.0')
        self.assertEqual(contract['cia_integer'], 1009)
        write(os.path.join(self.src, 'VERSION'), '1.0.0\n')
        self.assertEqual(products.release_contract.read(self.src)['cia_integer'], 1024)

    def test_version_too_long_for_the_build_id(self):
        # A smaller contract budget must be respected, rather than a copied builder constant.
        path = os.path.join(self.src, 'docs/release/build-contract.json')
        with open(path, encoding='utf-8') as source:
            contract = json.load(source)
        contract['build_id_storage_bytes'] = 20
        write(path, json.dumps(contract))
        write(os.path.join(self.src, 'VERSION'), '1.0.0-beta.1\n')
        with self.assertRaises(pipeline.BuildFailed) as e:
            pipeline.release_version(self.src)
        self.assertIn("too long", str(e.exception))

    def test_build_id_check(self):
        h = os.path.join(self.tmp, "ctrport_build_id.h")
        for good in ("archive-0.1.0+aot4", "archive-0.1.0+aot12", "archive-0.1.0+aot4ch", "archive-0.1.0+aot12ch"):
            write(h, '#define CTRPORT_COMMIT_TEXT "%s"\n' % good)
            self.assertEqual(pipeline.check_build_id(h, "0.1.0"), good)
        for bad in ("65726574+aot12", "65726574-dirty+aot12", "archive-0.1.0", "archive-0.2.0+aot12",
                    "archive-0.1.0+aot12 ", "archive-0.1.0+aot" + "1" * 30):
            write(h, '#define CTRPORT_COMMIT_TEXT "%s"\n' % bad)
            with self.assertRaises(pipeline.BuildFailed):
                pipeline.check_build_id(h, "0.1.0")
        with self.assertRaises(pipeline.BuildFailed):
            pipeline.check_build_id(os.path.join(self.tmp, "none.h"), "0.1.0")

    def test_git_stamped_build_is_not_delivered(self):
        self.build_id = "65726574+aot12"     # the source was a git checkout, not the release archive
        b, _ = self.make(["[100%] y\n"])
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("not archive-0.1.0+aot", str(e.exception))
        self.assertTrue(os.path.isfile(e.exception.log_path))
        self.assertFalse(os.path.exists(os.path.join(self.work, "out", "n3ds")))

    def test_package_stem_matches_build_sh(self):
        with open(os.path.join(pipeline.SOURCE_ROOT, "platform", "3ds", "build.sh"), encoding="utf-8") as f:
            script = f.read()
        self.assertIn('echo "built $build/$package_stem.3dsx', script)
        with open(os.path.join(pipeline.SOURCE_ROOT, "docs", "release", "build-contract.json"), encoding="utf-8") as f:
            contract = json.load(f)
        self.assertEqual(products.package_stem(products.PRODUCTS["undertale"], "o3ds"), "undertale-3ds-o3ds")
        contract['package_stem'] = '{game}-test-{model}'
        write(os.path.join(self.src, 'docs/release/build-contract.json'), json.dumps(contract))
        self.assertEqual(products.package_stem(products.PRODUCTS['undertale'], 'o3ds', self.src),
                         'undertale-test-o3ds')

    def test_mirror_is_incremental(self):
        dest = os.path.join(self.work, "src")
        c1, r1 = pipeline.mirror_source(self.src, dest)
        write(os.path.join(dest, "engine", "src", "old.c"), "stale\n")
        c2, r2 = pipeline.mirror_source(self.src, dest)
        self.assertGreater(c1, 0)
        self.assertEqual((c2, r2), (0, 1))

    def test_private_grid_step_and_reader_are_exported(self):
        for game in ("undertale", "deltarune", None):
            self.assertTrue(pipeline.source_wanted("tools/aot/loadscreen_art.py", game))
            self.assertEqual(pipeline.source_wanted("tools/aot/pixgrid.py", game), game != "deltarune")
            self.assertEqual(pipeline.source_wanted("tools/aot/btnprompt_build.py", game), game != "deltarune")
            self.assertEqual(pipeline.source_wanted("tools/aot/stglyphs.py", game), game != "deltarune")
            self.assertEqual(pipeline.source_wanted("tools/aot/stpack.py", game), game != "deltarune")
            self.assertEqual(pipeline.source_wanted("tools/aot/stmenu.py", game), game != "deltarune")
        manifest = Path(pipeline.SOURCE_ROOT, "tools/release/manifest-ut.txt").read_text(encoding="utf-8")
        for name in ("tools/aot/pixgrid.py", "tools/aot/loadscreen_art.py", "tools/aot/btnprompt_build.py",
                     "tools/aot/stglyphs.py", "tools/aot/stpack.py", "tools/aot/stmenu.py"):
            self.assertIn(name, manifest.splitlines())

    def test_work_dir(self):
        self.assertEqual(pipeline.default_work_dir({"LOCALAPPDATA": "C:\\Users\\Jo Smith\\AppData\\Local"}, "win32"),
                         "C:/u3ds")
        self.assertEqual(pipeline.default_work_dir({"LOCALAPPDATA": "C:\\Users\\jo\\AppData\\Local"}, "win32"),
                         os.path.join("C:\\Users\\jo\\AppData\\Local", "u3ds", "w"))

    def test_build_argv_shape(self):
        argv = pipeline.build_argv(self.tc, "C:/w", "undertale", "o3ds", "D:\\G", platform="win32")
        self.assertEqual(argv[:3], [self.tc.bash, "--login", "-c"])
        self.assertEqual(argv[-5:], ["undertale", "o3ds", "aot", "data=D:/G", "openh264=off"])
        argv = pipeline.build_argv(self.tc, "C:/w", "deltarune", "n3ds", "D:\\G", openh264=True, platform="win32")
        self.assertNotIn("openh264=off", argv)


# ---------------------------------------------------------------------------------------------------------------------

class Art:
    def __init__(self, path, kind):
        self.path, self.kind, self.size, self.sha256 = path, kind, os.path.getsize(path), "0" * 64


class DeliverTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = game_folder(os.path.join(self.tmp, "game"))
        self.sd = os.path.join(self.tmp, "sd")
        os.makedirs(os.path.join(self.sd, "3ds"))
        p = os.path.join(self.tmp, "undertale-3ds-n3ds.3dsx")
        write(p, b"3dsx")
        self.arts = [Art(p, "3dsx")]
        self.ut = products.PRODUCTS["undertale"]

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_copy_then_skip(self):
        items = deliver.plan_sd_copy(self.sd, self.ut, self.arts, self.game)
        dsts = sorted(i.dst for i in items)
        self.assertIn("3ds/undertale-3ds/undertale-3ds-n3ds.3dsx", dsts)
        self.assertIn("3ds/undertale-3ds/data.win", dsts)
        self.assertIn("3ds/undertale-3ds/credits.txt", dsts)
        self.assertNotIn("3ds/undertale-3ds/UNDERTALE.exe", dsts)
        self.assertEqual(sum(d.endswith(".ogg") for d in dsts), 3)
        copied, skipped, _ = deliver.copy_items(self.sd, items)
        self.assertEqual((copied, skipped), (len(items), 0))
        again = deliver.plan_sd_copy(self.sd, self.ut, self.arts, self.game)
        self.assertTrue(all(i.skip for i in again))
        self.assertTrue(deliver.dsp_missing(self.sd))
        write(os.path.join(self.sd, "3ds", "dspfirm.cdc"), b"x")
        self.assertFalse(deliver.dsp_missing(self.sd))

    def test_not_a_homebrew_card(self):
        os.rmdir(os.path.join(self.sd, "3ds"))
        with self.assertRaises(deliver.DeliverError) as e:
            deliver.plan_sd_copy(self.sd, self.ut, self.arts, self.game)
        self.assertIn("no 3ds folder", str(e.exception))

    def test_card_full(self):
        with mock.patch.object(deliver.shutil, "disk_usage", return_value=mock.Mock(free=10)):
            with self.assertRaises(deliver.DeliverError) as e:
                deliver.plan_sd_copy(self.sd, self.ut, self.arts, self.game)
        self.assertIn("This card has 0 MB free", str(e.exception))

    def test_cia_goes_to_cia_folder(self):
        p = os.path.join(self.tmp, "undertale-3ds-n3ds.cia")
        write(p, b"cia")
        items = deliver.plan_sd_copy(self.sd, self.ut, [Art(p, "cia")], self.game)
        self.assertIn("cia/undertale-3ds-n3ds.cia", [i.dst for i in items])

    def test_list_cards(self):
        other = os.path.join(self.tmp, "usb")
        os.makedirs(other)
        types = {self.sd: 2, other: 2, self.game: 3}
        cards = deliver.list_sd_cards("win32", drive_type=lambda r: types[r], roots=[self.sd, other, self.game])
        self.assertEqual([c.root for c in cards], [self.sd])

    def test_deltarune_files(self):
        dr = os.path.join(self.tmp, "DELTARUNE")
        for rel in ("data.win", "DELTARUNE.exe", "mus/a.ogg", "chapter1_windows/data.win",
                    "chapter3_windows/data.win", "chapter5_windows/x.dll"):
            write(os.path.join(dr, *rel.split("/")), b"x")
        rels = [r for _, r, _ in products.sd_files(products.PRODUCTS["deltarune"], dr, ["chapter1_windows"])]
        self.assertEqual(sorted(rels), ["chapter1_windows/data.win", "data.win", "mus/a.ogg"])

    def test_instructions(self):
        text = deliver.instructions(self.ut, ["n3ds"], ["3dsx"], "E:\\")
        self.assertIn("Homebrew Launcher", text)
        self.assertIn("Don't share it", text)

    def test_3dslink_is_send_only(self):
        self.assertEqual(deliver.threedslink_argv("3dslink", "a.3dsx"), ["3dslink", "a.3dsx"])
        self.assertEqual(deliver.threedslink_argv("3dslink", "a.3dsx", "10.0.0.5"),
                         ["3dslink", "-a", "10.0.0.5", "a.3dsx"])
        ran = []
        deliver.send_3dslink("3dslink", "a.3dsx", run=lambda argv: ran.append(argv) or 0, log=lambda s: None)
        self.assertNotIn("-s", ran[0])
        with self.assertRaises(deliver.DeliverError):
            deliver.send_3dslink("3dslink", "a.3dsx", run=lambda argv: 1, log=lambda s: None)

    def test_ftp_against_local_mock(self):
        root = os.path.join(self.tmp, "console")
        os.makedirs(root)
        server = MockFTP(root)
        try:
            items = [deliver.CopyItem(s, "3ds/undertale-3ds/" + r, n)
                     for s, r, n in products.sd_files(self.ut, self.game)]
            deliver.send_ftp("127.0.0.1", items, port=server.port, log=lambda s: None)
            with open(os.path.join(root, "3ds", "undertale-3ds", "data.win"), "rb") as f:
                self.assertEqual(f.read(), UT)
            stored = server.stores
            deliver.send_ftp("127.0.0.1", items, port=server.port, log=lambda s: None)
            self.assertEqual(server.stores, stored)             # same sizes: nothing re-sent
        finally:
            server.close()


class MockFTP:
    """Just enough of an FTP server on 127.0.0.1 (an ephemeral port) for ftplib's login, MKD, SIZE and STOR."""

    def __init__(self, root):
        self.root, self.stores = root, 0
        outer = self

        class Handler(socketserver.StreamRequestHandler):
            def reply(self, s):
                self.wfile.write((s + "\r\n").encode())

            def handle(self):
                self.reply("220 mock ftpd")
                pasv = None
                while True:
                    line = self.rfile.readline().decode().strip()
                    if not line:
                        return
                    cmd, _, arg = line.partition(" ")
                    cmd = cmd.upper()
                    path = os.path.join(outer.root, *[p for p in arg.split("/") if p])
                    if cmd == "USER":
                        self.reply("331 ok")
                    elif cmd == "PASS":
                        self.reply("230 ok")
                    elif cmd == "TYPE":
                        self.reply("200 ok")
                    elif cmd == "MKD":
                        if os.path.isdir(path):
                            self.reply("550 exists")
                        else:
                            os.makedirs(path)
                            self.reply("257 made")
                    elif cmd == "SIZE":
                        self.reply("213 %d" % os.path.getsize(path) if os.path.isfile(path) else "550 none")
                    elif cmd == "PASV":
                        pasv = socket.socket()
                        pasv.bind(("127.0.0.1", 0))
                        pasv.listen(1)
                        port = pasv.getsockname()[1]
                        self.reply("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (port >> 8, port & 255))
                    elif cmd == "STOR":
                        self.reply("150 send")
                        conn, _ = pasv.accept()
                        with open(path, "wb") as f:
                            while True:
                                b = conn.recv(65536)
                                if not b:
                                    break
                                f.write(b)
                        conn.close()
                        pasv.close()
                        outer.stores += 1
                        self.reply("226 done")
                    elif cmd == "QUIT":
                        self.reply("221 bye")
                        return
                    else:
                        self.reply("502 no")

        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        assert self.port != 17491
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


# ---------------------------------------------------------------------------------------------------------------------

def valid_ut_slot(name="Frisk", lv=1, lines=30):
    body = [name, str(lv)] + ["0" for _ in range(lines)]
    return "\r\n".join(body) + "\r\n"


def valid_ut_ini(name="Frisk", lv="1.000000", kills="0.000000"):
    return '[General]\r\nName="%s"\r\nLove="%s"\r\nKills="%s"\r\nRoom="6.000000"\r\n' % (name, lv, kills)


def valid_dr_ini(name="Kris", lv="2.000000"):
    return '[G0]\r\nName="%s"\r\nLevel="%s"\r\n' % (name, lv)


def valid_dr_slot(name="Kris", lines=40):
    return "\n".join([name] + ["1" for _ in range(lines)]) + "\n"


def folder_snapshot(folder):
    if not os.path.isdir(folder):
        return {}
    snap = {}
    for name in sorted(os.listdir(folder)):
        p = os.path.join(folder, name)
        if os.path.isfile(p):
            st = os.stat(p)
            with open(p, "rb") as f:
                data = f.read()
            snap[name] = (st.st_size, int(st.st_mtime), hashlib.sha256(data).hexdigest())
    return snap


class SaveImportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.pc_saves = os.path.join(self.tmp, "pc-saves")
        self.sd = os.path.join(self.tmp, "sd")
        os.makedirs(os.path.join(self.sd, "3ds"))
        self.ut = products.PRODUCTS["undertale"]
        self.dr = products.PRODUCTS["deltarune"]

    def tearDown(self):
        import shutil
        import stat
        for root, dirs, files in os.walk(self.tmp):
            for d in dirs:
                try:
                    os.chmod(os.path.join(root, d), stat.S_IWRITE | stat.S_IREAD | stat.S_IEXEC)
                except OSError:
                    pass
            for fn in files:
                try:
                    os.chmod(os.path.join(root, fn), stat.S_IWRITE | stat.S_IREAD)
                except OSError:
                    pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_empty_save_folder(self):
        # 1. Folder does not exist at all.
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        self.assertFalse(scan.exists)
        self.assertEqual(scan.files, [])
        self.assertEqual(scan.valid_files, [])
        with self.assertRaises(deliver.DeliverError) as e1:
            deliver.copy_pc_saves(self.sd, self.ut, scan)
        self.assertIn("No PC save files were found", str(e1.exception))
        self.assertFalse(os.path.exists(self.pc_saves))

        # 2. Folder exists and is empty.
        os.makedirs(self.pc_saves)
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        self.assertTrue(scan.exists)
        self.assertEqual(scan.files, [])

        # 3. Folder has only Steam cloud metadata, temporary/backup files, and unrelated files.
        write(os.path.join(self.pc_saves, "steam_autocloud.vdf"), '"steam_autocloud.vdf" {}')
        write(os.path.join(self.pc_saves, "file0.bak"), valid_ut_slot())
        write(os.path.join(self.pc_saves, "file0.tmp"), valid_ut_slot())
        write(os.path.join(self.pc_saves, "unrelated.txt"), "hello\n")
        before = folder_snapshot(self.pc_saves)
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        self.assertEqual(scan.files, [])
        with self.assertRaises(deliver.DeliverError) as e2:
            deliver.copy_pc_saves(self.sd, self.ut, scan)
        self.assertIn("No PC save files were found", str(e2.exception))
        self.assertEqual(folder_snapshot(self.pc_saves), before)

    def test_valid_undertale_saves_and_metadata_readonly(self):
        f0 = os.path.join(self.pc_saves, "file0")
        f9 = os.path.join(self.pc_saves, "file9")
        ini = os.path.join(self.pc_saves, "undertale.ini")
        cfg = os.path.join(self.pc_saves, "config.ini")
        sys962 = os.path.join(self.pc_saves, "system_information_962")
        write(f0, valid_ut_slot("Frisk", 1))
        write(f9, valid_ut_slot("Chara", 19))
        write(ini, valid_ut_ini("Frisk", "1.000000"))
        write(cfg, "[config]\nlang=0\n")
        write(sys962, "a")
        fixed_ts = 1729382400  # 2024-10-20 00:00:00 UTC
        for p in (f0, f9, ini, cfg, sys962):
            os.utime(p, (fixed_ts, fixed_ts))
        before = folder_snapshot(self.pc_saves)

        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        self.assertTrue(scan.exists)
        self.assertEqual(len(scan.valid_files), 5)
        self.assertEqual(scan.corrupt_files, [])
        by_name = {s.name: s for s in scan.files}
        self.assertEqual((by_name["file0"].player_name, by_name["file0"].lv), ("Frisk", 1))
        self.assertIn("Name: Frisk", by_name["file0"].summary)
        self.assertIn("LV 1", by_name["file0"].summary)
        self.assertRegex(by_name["file0"].mtime_text, r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$")
        self.assertEqual((by_name["file9"].player_name, by_name["file9"].lv), ("Chara", 19))
        self.assertEqual((by_name["undertale.ini"].player_name, by_name["undertale.ini"].lv), ("Frisk", 1))
        self.assertEqual(by_name["config.ini"].summary, "Settings")
        self.assertEqual(by_name["system_information_962"].summary, "System data")

        res = deliver.copy_pc_saves(self.sd, self.ut, scan)
        self.assertEqual((res.copied, res.backed_up), (5, 0))
        sd_saves = deliver.sd_save_dir(self.sd, self.ut)
        for name in ("file0", "file9", "undertale.ini", "config.ini", "system_information_962"):
            self.assertTrue(os.path.isfile(os.path.join(sd_saves, name)))
        with open(os.path.join(sd_saves, "file0"), "rb") as f_sd, open(f0, "rb") as f_pc:
            self.assertEqual(f_sd.read(), f_pc.read())
        # PC save folder must never be modified.
        self.assertEqual(folder_snapshot(self.pc_saves), before)

    def test_valid_deltarune_saves(self):
        write(os.path.join(self.pc_saves, "dr.ini"), valid_dr_ini("Kris", "2.000000"))
        write(os.path.join(self.pc_saves, "filech1_0"), valid_dr_slot("Kris"))
        write(os.path.join(self.pc_saves, "filech2_1"), valid_dr_slot("Noelle"))
        write(os.path.join(self.pc_saves, "config_0.ini"), "[controls]\n")
        write(os.path.join(self.pc_saves, "keyconfig_0.ini"), "40\n38\n39\n37\n90\n88\n67\n")
        write(os.path.join(self.pc_saves, "keyconfig_1.ini"), "[KEYCONFIG]\n0=\"40\"\n")
        scan = deliver.scan_saves(self.pc_saves, self.dr.key)
        self.assertEqual(len(scan.valid_files), 6)
        by_name = {s.name: s for s in scan.files}
        self.assertEqual((by_name["dr.ini"].player_name, by_name["dr.ini"].lv), ("Kris", 2))
        self.assertEqual(by_name["filech1_0"].player_name, "Kris")
        self.assertIn("Chapter 1", by_name["filech1_0"].summary)
        self.assertEqual(by_name["filech2_1"].player_name, "Noelle")
        self.assertEqual(by_name["keyconfig_0.ini"].summary, "Settings")
        self.assertEqual(by_name["keyconfig_1.ini"].summary, "Settings")
        res = deliver.copy_pc_saves(self.sd, self.dr, scan)
        self.assertEqual(res.copied, 6)
        self.assertTrue(os.path.isfile(os.path.join(deliver.sd_save_dir(self.sd, self.dr), "filech1_0")))
        self.assertTrue(os.path.isfile(os.path.join(deliver.sd_save_dir(self.sd, self.dr), "keyconfig_0.ini")))
        self.assertTrue(os.path.isfile(os.path.join(deliver.sd_save_dir(self.sd, self.dr), "keyconfig_1.ini")))

    def test_corrupt_saves_are_detected_and_refused(self):
        cases = [
            ("file0", b"", "empty save file"),
            ("file1", "Frisk\n", "truncated save slot"),
            ("file2", "Frisk\nnot_a_number\n0\n0\n", "name and LV could not be read"),
            ("file3", "Frisk\n0\n0\n0\n", "name and LV could not be read"),
            ("file4", "Frisk\n999\n0\n0\n", "name and LV could not be read"),
            ("file5", "Frisk\n1\nnot_a_number\n", "invalid save data"),
            ("file6", b"Frisk\x00\n1\n0\n", "name and LV could not be read"),
            ("file7", b"\xff\xfe\xfd\x00", "name and LV could not be read"),
            ("undertale.ini", "not_an_ini_section_without_equals\n", "invalid INI format"),
        ]
        corrupt_dir = os.path.join(self.tmp, "corrupt-cases")
        for name, data, expected_reason in cases:
            p = os.path.join(corrupt_dir, name)
            write(p, data)
            sf = deliver.inspect_save_file(p, self.ut.key)
            self.assertIsNotNone(sf, name)
            self.assertFalse(sf.valid, "%s should be marked corrupt (%s)" % (name, sf.summary))
            self.assertIn(expected_reason, sf.summary)

        # Oversized save file (> MAX_SAVE_BYTES) is rejected safely without reading the whole file.
        huge = os.path.join(corrupt_dir, "file8")
        write(huge, "0\n" * (deliver.MAX_SAVE_BYTES // 2 + 100))
        sf_huge = deliver.inspect_save_file(huge, self.ut.key)
        self.assertFalse(sf_huge.valid)
        self.assertIn("too large", sf_huge.summary)

        # Folder containing a corrupt save file: scan_saves reports it, and copy_pc_saves refuses so a corrupt save set
        # is never written to the 3DS SD card.
        write(os.path.join(self.pc_saves, "file0"), valid_ut_slot("Frisk", 3))
        write(os.path.join(self.pc_saves, "file9"), b"\x00\x01corrupt")
        write(os.path.join(self.pc_saves, "undertale.ini"), b"")
        before = folder_snapshot(self.pc_saves)
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        self.assertEqual([s.name for s in scan.valid_files], ["file0"])
        self.assertEqual(sorted(s.name for s in scan.corrupt_files), ["file9", "undertale.ini"])
        with self.assertRaises(deliver.DeliverError) as e:
            deliver.copy_pc_saves(self.sd, self.ut, scan)
        self.assertIn("Can't read", str(e.exception))
        self.assertIn("file9", str(e.exception))
        sd_saves = deliver.sd_save_dir(self.sd, self.ut)
        self.assertFalse(os.path.exists(os.path.join(sd_saves, "file0")))
        self.assertEqual(folder_snapshot(self.pc_saves), before)

    def test_existing_3ds_save_refused_without_overwrite_and_backed_up_with_overwrite(self):
        write(os.path.join(self.pc_saves, "file0"), valid_ut_slot("PCFrisk", 10))
        write(os.path.join(self.pc_saves, "undertale.ini"), valid_ut_ini("PCFrisk", "10.000000"))
        sd_saves = deliver.sd_save_dir(self.sd, self.ut)
        old_3ds_slot = valid_ut_slot("3DSPlayer", 4)
        old_3ds_slot8 = valid_ut_slot("3DSPlayer", 4)
        old_3ds_bak = valid_ut_slot("StaleBak", 2)
        write(os.path.join(sd_saves, "file0"), old_3ds_slot)
        write(os.path.join(sd_saves, "file8"), old_3ds_slot8)
        write(os.path.join(sd_saves, "file0.bak"), old_3ds_bak)

        self.assertTrue(deliver.has_existing_sd_saves(self.sd, self.ut))
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)

        # Without overwrite=True, copy_pc_saves refuses and leaves the 3DS saves untouched.
        with self.assertRaises(deliver.DeliverError) as e:
            deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=False)
        self.assertIn("Existing 3DS saves were found", str(e.exception))
        self.assertIn("3DSPlayer, LV 4", str(e.exception))
        with open(os.path.join(sd_saves, "file0"), encoding="utf-8") as f:
            self.assertIn("3DSPlayer", f.read())

        # With overwrite=True, existing 3DS saves (including .bak and file8) are moved into a timestamped backup
        # subfolder so the 3DS runtime does not preload stale slots or resurrect .bak files on boot.
        fixed_now = time.struct_time((2026, 10, 20, 14, 30, 5, 1, 293, 0))
        res = deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=True, now=fixed_now)
        self.assertEqual(res.copied, 2)
        self.assertEqual(res.backed_up, 3)
        self.assertEqual(os.path.basename(res.backup_dir), "backup-20261020-143005")
        self.assertTrue(os.path.isfile(os.path.join(res.backup_dir, "file0")))
        self.assertTrue(os.path.isfile(os.path.join(res.backup_dir, "file8")))
        self.assertTrue(os.path.isfile(os.path.join(res.backup_dir, "file0.bak")))
        with open(os.path.join(res.backup_dir, "file0"), encoding="utf-8") as f:
            self.assertIn("3DSPlayer", f.read())
        # Top-level saves/ now has only the newly imported PC saves and the backup subdirectory.
        self.assertFalse(os.path.exists(os.path.join(sd_saves, "file8")))
        self.assertFalse(os.path.exists(os.path.join(sd_saves, "file0.bak")))
        with open(os.path.join(sd_saves, "file0"), encoding="utf-8") as f:
            self.assertIn("PCFrisk", f.read())

    def test_read_only_sd_card(self):
        import stat
        write(os.path.join(self.pc_saves, "file0"), valid_ut_slot("Frisk", 1))
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        p = os.path.join(self.tmp, "undertale-3ds-n3ds.3dsx")
        write(p, b"3dsx")
        arts = [Art(p, "3dsx")]
        game = game_folder(os.path.join(self.tmp, "game"))

        # 1. On Windows, FILE_ATTRIBUTE_READONLY on a folder does not block writes (Explorer sets it on customized
        # folders), so check_sd_writable ignores folder read-only bits on win32 while enforcing them on POSIX.
        os.chmod(self.sd, stat.S_IREAD | stat.S_IEXEC)
        try:
            deliver.check_sd_writable(self.sd, platform="win32")
            with self.assertRaises(deliver.DeliverError) as e_posix:
                deliver.check_sd_writable(self.sd, platform="linux")
            self.assertIn("read-only", str(e_posix.exception))
            self.assertIn("Lock switch", str(e_posix.exception))
        finally:
            os.chmod(self.sd, stat.S_IWRITE | stat.S_IREAD | stat.S_IEXEC)

        # 2. A read-only target file on the SD card is caught by check_sd_writable on all platforms.
        sd_saves = deliver.sd_save_dir(self.sd, self.ut)
        sd_file0 = os.path.join(sd_saves, "file0")
        write(sd_file0, valid_ut_slot("Old", 1))
        os.chmod(sd_file0, stat.S_IREAD)
        try:
            with self.assertRaises(deliver.DeliverError) as e_file:
                deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=True)
            self.assertIn("read-only", str(e_file.exception))
            self.assertIn("Lock switch", str(e_file.exception))
        finally:
            os.chmod(sd_file0, stat.S_IWRITE | stat.S_IREAD)
            os.remove(sd_file0)

        # 3. Write failure (PermissionError / EROFS) during probe or copy_items produces the same clear message.
        items = deliver.plan_sd_copy(self.sd, self.ut, arts, game)
        with mock.patch.object(deliver.shutil, "copy2", side_effect=PermissionError("Write protected")):
            with self.assertRaises(deliver.DeliverError) as e3:
                deliver.copy_items(self.sd, items)
            self.assertIn("read-only", str(e3.exception))
            self.assertIn("Lock switch", str(e3.exception))


    def test_failed_copy_leaves_3ds_saves_unchanged(self):
        # A copy that fails part-way (card pulled, card full) must not leave a mix of PC and 3DS files: every incoming
        # file is staged before any 3DS save is replaced (argreview).
        import saves
        write(os.path.join(self.pc_saves, "file0"), valid_ut_slot("PCFrisk", 10))
        write(os.path.join(self.pc_saves, "undertale.ini"), valid_ut_ini("PCFrisk", "10.000000"))
        sd_saves = deliver.sd_save_dir(self.sd, self.ut)
        write(os.path.join(sd_saves, "file0"), valid_ut_slot("3DSPlayer", 4))
        write(os.path.join(sd_saves, "file8"), valid_ut_slot("3DSPlayer", 4))
        before = folder_snapshot(sd_saves)
        scan = deliver.scan_saves(self.pc_saves, self.ut.key)
        real_copy = saves._copy_file

        def flaky(src, dst):
            if os.path.basename(dst) == "undertale.ini.part":
                raise OSError(5, "I/O error")
            real_copy(src, dst)

        with mock.patch.object(saves, "_copy_file", side_effect=flaky):
            with self.assertRaises(deliver.DeliverError):
                deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=True)
        top_files = sorted(n for n in os.listdir(sd_saves) if os.path.isfile(os.path.join(sd_saves, n)))
        self.assertEqual(top_files, ["file0", "file8"])
        self.assertEqual(folder_snapshot(sd_saves), before)

        # A failure after files were replaced names the backup folder.
        with mock.patch.object(saves.os, "remove", side_effect=PermissionError(13, "denied")):
            with self.assertRaises(deliver.DeliverError) as e:
                deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=True)
        self.assertIn("previous 3DS saves are in", str(e.exception))
        self.assertIn("backup-", str(e.exception))

    def test_read_only_pc_save_is_copied_writable(self):
        # shutil.copy2 carried the read-only bit to the card, where it blocks the game's own saves and every later
        # import ("SD card is read-only"). The copy keeps the bytes and the date, not the bit (argreview).
        import stat
        f0 = os.path.join(self.pc_saves, "file0")
        write(f0, valid_ut_slot("Frisk", 1))
        os.utime(f0, (1729382400, 1729382400))
        os.chmod(f0, stat.S_IREAD)
        before = folder_snapshot(self.pc_saves)
        deliver.copy_pc_saves(self.sd, self.ut, deliver.scan_saves(self.pc_saves, self.ut.key))
        dst = os.path.join(deliver.sd_save_dir(self.sd, self.ut), "file0")
        self.assertTrue(os.stat(dst).st_mode & stat.S_IWRITE)
        self.assertEqual(int(os.stat(dst).st_mtime), 1729382400)
        self.assertEqual(folder_snapshot(self.pc_saves), before)
        self.assertFalse(os.stat(f0).st_mode & stat.S_IWRITE)
        # A second import over it works (no false "read-only" refusal).
        res = deliver.copy_pc_saves(self.sd, self.ut, deliver.scan_saves(self.pc_saves, self.ut.key), overwrite=True)
        self.assertEqual(res.copied, 1)


# ---------------------------------------------------------------------------------------------------------------------

class CliTest(MusicFixtures):
    def test_parse(self):
        # Default target is "both" (New 3DS and Old 3DS).
        d = app.parse([])
        self.assertEqual((d["mode"], d["target"], d["cia"], d["import_saves"]), ("gui", "both", False, False))
        o = app.parse(["--cli", "--work", "rel/w", "--target", "o3ds", "--cia", "--save-dir", "rel/s",
                       "--import-saves", "--overwrite-saves"])
        self.assertEqual((o["mode"], o["target"], o["cia"], o["import_saves"], o["overwrite_saves"]),
                         ("cli", "o3ds", True, True, True))
        self.assertTrue(os.path.isabs(o["work"]))
        self.assertTrue(os.path.isabs(o["save_dir"]))
        with contextlib.redirect_stderr(io.StringIO()):
            for bad in (["--target", "wii"], ["--game", "x"], ["--bogus"]):
                with self.assertRaises(SystemExit) as e:
                    app.parse(bad)
                self.assertEqual(e.exception.code, 2)

    def test_dry_run_changes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.save_dir, "file0"), valid_ut_slot("Frisk", 5))
            before_saves = folder_snapshot(t.save_dir)
            tc = toolchain.Toolchain(None, "C:/dkp/bash.exe", [toolchain.Check("devkitPro", False, "x", "build")],
                                     list(toolchain.REQUIRED_PACKAGES), [], None, None, None, "win32")
            out = []
            work = os.path.join(tmp, "w")
            with mock.patch.object(app, "probes_for", return_value=t.probes()), \
                    mock.patch.object(app.toolchain, "detect", return_value=tc):
                app.dry_run(dict(app.parse(["--dry-run", "--work", work])), out=out.append)
            text = "\n".join(out)
            self.assertIn("Copies found: 1", text)
            self.assertIn("PC saves (", text)
            self.assertIn("Name: Frisk, LV 5", text)
            self.assertIn("Steps this machine needs: 4", text)
            self.assertIn("Build command for n3ds", text)
            self.assertIn("Build command for o3ds", text)
            self.assertFalse(os.path.exists(work))
            self.assertEqual(folder_snapshot(t.save_dir), before_saves)

    def test_cli_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp, install=False)
            out = []
            with mock.patch.object(app, "probes_for", return_value=t.probes()):
                rc = app.cli(dict(app.parse(["--cli", "--work", os.path.join(tmp, "w")])), out=out.append)
            self.assertEqual(rc, 1)
            self.assertIn("was not found", out[0])

    def test_cli_import_saves_and_backup(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.save_dir, "file0"), valid_ut_slot("Frisk", 7))
            sd = os.path.join(tmp, "sd")
            os.makedirs(os.path.join(sd, "3ds"))
            sd_saves = deliver.sd_save_dir(sd, products.PRODUCTS["undertale"])
            write(os.path.join(sd_saves, "file0"), valid_ut_slot("Old3DS", 2))
            tc = toolchain.Toolchain("C:/dkp", "C:/dkp/bash.exe", [toolchain.Check("devkitPro", True, "ok", "build")],
                                     [], [], None, None, None)
            fake_art = os.path.join(tmp, "undertale-3ds-n3ds.3dsx")
            write(fake_art, b"3dsx")
            orig_verify = discover.verify_copy

            class FakeBuild:
                def __init__(self, product, tc, work, target, folder, **kw):
                    self.log_path = os.path.join(tmp, "build.log")

                def run(self):
                    return [Art(fake_art, "3dsx")], {"target": "n3ds", "seconds": 1.0, "compiler_warnings": 0}

            # 1. Without --overwrite-saves, CLI refuses to overwrite the existing 3DS save.
            out1 = []
            with mock.patch.object(app, "probes_for", return_value=t.probes()), \
                    mock.patch.object(app.discover, "verify_copy",
                                      side_effect=lambda g, f, p: orig_verify(g, f, p, allow_list=ALLOW)), \
                    mock.patch.object(app.toolchain, "detect", return_value=tc), \
                    mock.patch.object(app.pipeline, "Build", FakeBuild):
                rc1 = app.cli(dict(app.parse(["--cli", "--target", "n3ds", "--work", os.path.join(tmp, "w"),
                                              "--sd", sd, "--import-saves"])), out=out1.append)
            self.assertEqual(rc1, 1)
            self.assertTrue(any("Existing 3DS saves were found" in line for line in out1))
            self.assertTrue(any("--overwrite-saves" in line for line in out1))

            # 2. With --overwrite-saves, CLI backs up existing 3DS saves and copies PC saves.
            out2 = []
            with mock.patch.object(app, "probes_for", return_value=t.probes()), \
                    mock.patch.object(app.discover, "verify_copy",
                                      side_effect=lambda g, f, p: orig_verify(g, f, p, allow_list=ALLOW)), \
                    mock.patch.object(app.toolchain, "detect", return_value=tc), \
                    mock.patch.object(app.pipeline, "Build", FakeBuild):
                rc2 = app.cli(dict(app.parse(["--cli", "--target", "n3ds", "--work", os.path.join(tmp, "w"),
                                              "--sd", sd, "--import-saves", "--overwrite-saves"])), out=out2.append)
            self.assertEqual(rc2, 0)
            joined = "\n".join(out2)
            self.assertIn("Imported 1 PC save file(s)", joined)
            self.assertIn("Backed up existing 3DS saves", joined)
            self.assertIn("What to do now:", joined)


class GuiTest(unittest.TestCase):
    def setUp(self):
        import gui
        self.gui = gui
        self.tmp = tempfile.mkdtemp()
        try:
            self.root = gui.tk.Tk()
            self.root.withdraw()
        except gui.tk.TclError as e:
            self.skipTest("Tk display unavailable: %s" % e)

    def tearDown(self):
        import shutil
        try:
            for timer in self.root.tk.call("after", "info"):
                self.root.after_cancel(timer)
            self.root.destroy()
        except Exception:
            pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_wizard_defaults_and_saves_page_flows(self):
        t = SteamTree(self.tmp)
        pc_saves = t.save_dir
        sd = os.path.join(self.tmp, "sd")
        os.makedirs(os.path.join(sd, "3ds"))

        prompts = []
        opts = self.gui.Options(
            game="undertale",
            work=os.path.join(self.tmp, "work"),
            probes=t.probes(),
            confirm_overwrite=lambda title, msg: (prompts.append((title, msg)) or False),
        )
        wiz = self.gui.App(self.root, opts)
        # Sensible defaults: both New and Old 3DS, and .3dsx.
        self.assertEqual(len(wiz.pages), 9)
        self.assertEqual(wiz.state.targets, ["n3ds", "o3ds"])
        self.assertEqual(wiz.state.outputs, ["3dsx"])

        # Step 5: Options page defaults to "both" and ".3dsx".
        wiz.show(4)
        self.assertEqual(wiz.page.target.get(), "both")
        self.assertTrue(wiz.page.v3dsx.get())
        self.assertFalse(wiz.page.vcia.get())

        # Step 8 with an empty PC save folder: defaults to "skip" and copy radio is disabled.
        wiz.state.sd_root = sd
        wiz.state.delivered = True
        wiz.show(7)
        self.assertEqual(wiz.page.title, "Import PC saves")
        self.assertEqual(wiz.page.choice.get(), "skip")
        self.assertTrue(wiz.page.copy_radio.instate(["disabled"]))
        self.assertIn("No PC saves were found", wiz.page.notice.cget("text"))

        # Add a corrupt save file: SavesPage shows it as "Corrupt / unreadable", defaults to "skip", and refuses copy.
        write(os.path.join(pc_saves, "file0"), valid_ut_slot("Frisk", 8))
        write(os.path.join(pc_saves, "file9"), b"\x00corrupt")
        wiz.page.scan()
        self.assertEqual(wiz.page.choice.get(), "skip")
        rows = [wiz.page.tree.item(i, "values") for i in wiz.page.tree.get_children()]
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0][0], "file0")
        self.assertIn("Name: Frisk, LV 8", rows[0][2])
        self.assertEqual(rows[0][3], "OK")
        self.assertEqual(rows[1][0], "file9")
        self.assertEqual(rows[1][3], "Unreadable")
        wiz.page.choice.set("copy")
        wiz.next()
        self.assertEqual(wiz.index, 7)
        self.assertIn("Can't read", wiz.page.status.cget("text"))

        # Remove the corrupt file: SavesPage now defaults to "copy" when no 3DS save exists.
        os.remove(os.path.join(pc_saves, "file9"))
        wiz.page.scan()
        self.assertEqual(wiz.page.choice.get(), "copy")

        # Put an existing 3DS save on the SD card: SavesPage defaults to "skip" and warns about existing saves.
        sd_saves = deliver.sd_save_dir(sd, wiz.state.product)
        write(os.path.join(sd_saves, "file0"), valid_ut_slot("3DSHero", 3))
        wiz.page.scan()
        self.assertEqual(wiz.page.choice.get(), "skip")
        self.assertIn("already has 3DS saves", wiz.page.notice.cget("text"))

        # User selects "copy", clicks Next >, and declines the overwrite prompt -> stays on SavesPage, save untouched.
        wiz.page.choice.set("copy")
        wiz.next()
        self.assertEqual(wiz.index, 7)
        self.assertEqual(len(prompts), 1)
        self.assertIn("3DS saves kept", wiz.page.status.cget("text"))
        with open(os.path.join(sd_saves, "file0"), encoding="utf-8") as f:
            self.assertIn("3DSHero", f.read())

        # Read-only SD card when confirmed -> stays on SavesPage and displays the read-only lock switch guidance.
        opts.confirm_overwrite = lambda title, msg: (prompts.append((title, msg)) or True)
        with mock.patch.object(deliver, "check_sd_writable",
                               side_effect=deliver.DeliverError(deliver.READ_ONLY_MESSAGE % sd)):
            wiz.next()
        self.assertEqual(wiz.index, 7)
        self.assertIn("read-only", wiz.page.status.cget("text"))
        self.assertIn("Lock switch", wiz.page.status.cget("text"))

        # User confirms the overwrite prompt on writable SD card -> backs up 3DS save, copies PC save, and advances to
        # DonePage (Step 9).
        wiz.next()
        self.assertEqual(wiz.index, 8)
        self.assertEqual(wiz.page.title, "What to do now")
        self.assertIsNotNone(wiz.state.save_result)
        self.assertEqual(wiz.state.save_result.copied, 1)
        self.assertEqual(wiz.state.save_result.backed_up, 1)
        self.assertTrue(os.path.isfile(os.path.join(wiz.state.save_result.backup_dir, "file0")))
        with open(os.path.join(sd_saves, "file0"), encoding="utf-8") as f:
            self.assertIn("Frisk", f.read())
        self.assertEqual("UNDERTALE is ready.", wiz.page.headline.cget("text"))
        self.assertIn("Eject the SD card", wiz.page.steps_label.cget("text"))
        self.assertIn("backed up", wiz.page.summary_label.cget("text"))
        self.assertIn("Homebrew Launcher", wiz.page.steps_label.cget("text"))

    def test_undertale_theme_and_short_wording(self):
        t = SteamTree(self.tmp)
        opts = self.gui.Options(game="undertale", work=os.path.join(self.tmp, "work"), probes=t.probes())
        wiz = self.gui.App(self.root, opts)
        # Synthetic assets exercise the fun renderer without reading or bundling any game content.
        wiz.theme.runtime_loaded = True
        wiz.theme.apply()
        self.assertEqual(self.root.cget("bg"), "#000000")
        self.assertEqual(wiz.body.cget("bg"), "#000000")
        self.assertEqual(wiz.page.frame.cget("bg"), "#000000")
        self.assertEqual(wiz.page.frame.cget("highlightbackground"), "#ffffff")
        self.assertEqual(len(wiz.theme.soul_mask), 16)
        self.assertEqual(len(wiz.theme.soul_mask[0]), 16)
        # The fallback heart is original art. The hash is of an earlier mask that was a pixel copy of the game's
        # spr_heart (argreview); the game's own heart is only ever read from the player's data.win at runtime.
        fallback = "\n".join(self.gui.FALLBACK_SOUL_ROWS).encode()
        self.assertNotEqual(hashlib.sha256(fallback).hexdigest(),
                            "010dae23e7029483ab47b954c826c147ae33957d9f80307326e151c5d9cc49e7")
        btn_img = wiz.theme.render_button_image("Next >", active=True, show_soul=True)
        self.assertGreaterEqual(btn_img.width(), 48)
        self.assertGreaterEqual(btn_img.height(), 28)

        # Non-Latin characters in paths fall back to the Tk text font instead of rendering as "?" in the bitmap font.
        unicode_path = "PC folder: C:\\Users\\Zoë\\Юлия\\AppData\\Local\\UNDERTALE"
        self.assertFalse(wiz.theme.can_render_bitmap(unicode_path))
        lbl = self.gui.PixelLabel(wiz.page.frame, text=unicode_path, bold=True)
        self.assertEqual(lbl.cget("text"), unicode_path)
        self.assertEqual(str(lbl.cget("compound")), "none")
        self.assertEqual(str(lbl.cget("image")), "")
        lbl.config(text="PC folder: C:\\Games\\UNDERTALE")
        self.assertEqual(str(lbl.cget("compound")), "image")
        self.assertNotEqual(str(lbl.cget("image")), "")

        # Ensure flagged "vibe-coded" strings are completely absent across every page.
        wiz.work = lambda job, done, errors=False: None
        for idx in range(len(wiz.pages)):
            wiz.state.build_ok = True
            wiz.state.delivered = True
            wiz.show(idx)
            page_text = "\n".join(self.gui.text_of(wiz.page.frame))
            self.assertNotIn("Quitting keeps what's done", page_text)
            self.assertNotIn("Converting the game's art for the 3DS", page_text)
            self.assertNotIn("What is compiled so far is kept", page_text)

    def test_boring_switch_keeps_widgets_values_and_active_build(self):
        t = SteamTree(self.tmp)
        opts = self.gui.Options(work=os.path.join(self.tmp, "work"), probes=t.probes(), wifi=True)
        wiz = self.gui.App(self.root, opts)
        self.assertFalse(wiz.boring.get())
        self.assertTrue(wiz.theme.native)  # unreadable/missing assets
        wiz.theme.runtime_loaded = True
        wiz.theme.apply()
        wiz.work = lambda *args, **kw: None
        wiz.state.build_ok = True
        wiz.state.delivered = True
        scaling = self.root.tk.call("tk", "scaling")
        for index in range(9):
            wiz.show(index)
            page = wiz.page
            text = self.gui.text_of(page.frame)
            children = page.frame.winfo_children()
            for boring in (True, False):
                wiz.boring_toggle.invoke()
                self.assertEqual(wiz.boring.get(), boring)
                self.assertIs(wiz.page, page)
                self.assertEqual(page.frame.winfo_children(), children)
                self.assertEqual(self.gui.text_of(page.frame), text)
                self.assertEqual(self.root.tk.call("tk", "scaling"), scaling)
                self.assertEqual(str(wiz.next_btn.cget("style")), "TButton" if boring else "Ut.TButton")
                self.assertEqual(bool(wiz.boring_toggle.instate(["selected"])), boring)
        wiz.show(4)
        wiz.page.target.set("o3ds")
        wiz.boring_toggle.invoke()
        self.assertEqual(wiz.page.target.get(), "o3ds")
        # A worker remains blocked until after the switch, then its queued callback still runs.
        page = wiz.pages[5]
        page.running = True
        page.build = object()
        page.log.insert("end", "Build log\n")
        page.bar.config(value=43)
        wiz.show(5)
        build = page.build
        wiz.boring_toggle.invoke()
        self.assertTrue(page.running)
        self.assertIs(page.build, build)
        self.assertEqual(page.log.get("1.0", "end-1c"), "Build log\n")
        self.assertEqual(float(page.bar.cget("value")), 43)
        gate = threading.Event()
        done = threading.Event()
        worker = threading.Thread(target=lambda: (gate.wait(2), wiz.call(done.set)))
        worker.start()
        wiz.boring_toggle.invoke()
        gate.set()
        worker.join(3)
        wiz.poll()
        self.assertTrue(done.is_set())
        self.assertEqual(str(page.bar._overlay.place_info()), "{}")
        wiz.show(6)
        delivery = wiz.page
        delivery.how.set("wifi")
        delivery.mode()
        delivery.addr.insert(0, "test address")
        delivery.addr.icursor(4)
        delivery.addr.selection_range(0, 4)
        # Focus and keyboard traversal targets are properties of the original widgets.
        self.root.deiconify()
        self.root.update()
        delivery.addr.focus_force()
        self.root.update()
        next_focus = delivery.addr.tk_focusNext()
        for _ in range(2):
            wiz.boring_toggle.invoke()
            self.assertEqual(delivery.addr.get(), "test address")
            self.assertEqual(delivery.addr.index("insert"), 4)
            self.assertTrue(delivery.addr.selection_present())
            self.assertEqual(self.root.focus_get(), delivery.addr)
            self.assertEqual(delivery.addr.tk_focusNext(), next_focus)
        # Check that a cached dropdown also loses game colours and font when switched to native.
        popup = self.root.tk.call("ttk::combobox::PopdownWindow", delivery.card)
        self.root.tk.call(popup + ".f.l", "configure", "-font", "Courier 22")
        wiz.boring_toggle.invoke()
        wiz.boring_toggle.invoke()
        self.assertEqual(self.root.tk.call(popup + ".f.l", "cget", "-font"), "TkDefaultFont")
        # A fresh wizard reads the saved preference from this work folder only.
        other = self.gui.tk.Toplevel(self.root)
        restored = self.gui.App(other, opts)
        self.assertTrue(restored.boring.get())
        other.destroy()

    def test_boring_override_and_asset_failure(self):
        t = SteamTree(self.tmp)
        opts = self.gui.Options(work=os.path.join(self.tmp, "work"), probes=t.probes(), boring=True)
        wiz = self.gui.App(self.root, opts)
        with mock.patch.object(self.gui, "_extract_runtime_assets", return_value=(None, None)):
            wiz.theme.load_from_game_dir(t.folder)
        self.assertTrue(wiz.theme.native)
        wiz.boring_toggle.invoke()
        self.assertFalse(wiz.boring.get())
        self.assertTrue(wiz.theme.native)
        with mock.patch.object(self.gui, "_extract_runtime_assets",
                               return_value=(self.gui._fallback_glyphs(), self.gui._fallback_soul())):
            wiz.theme.load_from_game_dir(t.folder + "/other")
        self.assertFalse(wiz.theme.native)
        wiz.boring_toggle.invoke()
        self.assertTrue(wiz.theme.native)
        self.assertEqual(str(wiz.heading.cget("image")), "")
        self.assertEqual(str(wiz.heading.cget("font")), "TkDefaultFont")

    def test_native_clam_stays_clean_and_respects_system_font(self):
        # Linux uses clam too: game overrides must live in a separate child theme.
        style = self.gui.ttk.Style(self.root)
        style.theme_use("clam")
        baseline = style.configure("TFrame")
        opts = self.gui.Options(work=os.path.join(self.tmp, "work"))
        with mock.patch.object(self.gui.sys, "platform", "linux"):
            wiz = self.gui.App(self.root, opts)
        self.assertEqual(wiz.theme.native_theme, "clam")
        system_font = self.gui.tkfont.nametofont("TkDefaultFont", self.root)
        size = system_font.cget("size")
        try:
            system_font.configure(size=18)
            wiz.theme.runtime_loaded = True
            wiz.theme.apply()
            wiz.boring_toggle.invoke()
            self.assertEqual(style.theme_use(), "clam")
            self.assertEqual(style.configure("TFrame"), baseline)
            self.assertGreaterEqual(int(style.lookup("Treeview", "rowheight")), system_font.metrics("linespace"))
            self.assertEqual(str(wiz.pages[5].log.cget("font")), "TkDefaultFont")
        finally:
            system_font.configure(size=size)


class PreferencesTest(unittest.TestCase):
    def test_defaults_persistence_and_atomic_failure(self):
        import preferences
        self.assertTrue(app.parse(["--boring"])["boring"])
        self.assertIsNone(app.parse([])["boring"])
        with tempfile.TemporaryDirectory() as work:
            path = os.path.join(work, "ui-preferences.json")
            self.assertEqual(preferences.load(work), {"boring": False})
            for broken in ("{", "[]", '{"boring": "true"}', "null", "\ufffd", "[" * 2000 + "]" * 2000):
                write(path, broken)
                self.assertEqual(preferences.load(work), {"boring": False})
            self.assertTrue(preferences.save(work, True))
            self.assertEqual(preferences.load(work), {"boring": True})
            self.assertEqual(json.loads(Path(path).read_text()), {"boring": True})
            with mock.patch.object(preferences.os, "replace", side_effect=OSError("read only")):
                self.assertFalse(preferences.save(work, False))
            self.assertEqual(preferences.load(work), {"boring": True})
            self.assertEqual(os.listdir(work), ["ui-preferences.json"])
            self.assertTrue(preferences.save(work, False))
            self.assertEqual(preferences.load(work), {"boring": False})


class LicenceTest(unittest.TestCase):
    """The licence line and the per-step confirm text name each tool's licence exactly, with an official link."""

    def test_notice_names_each_licence(self):
        text, links = toolchain.licence_notice("win32")
        self.assertEqual(text, "By clicking Install or Update you agree to the licences of: devkitPro (each package's "
                               "own licence), makerom (MIT License), bannertool (MIT License).")
        self.assertEqual([n for n, _ in links], ["devkitPro", "makerom", "bannertool"])
        self.assertEqual(dict(links)["makerom"],
                         "https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.18.4/makerom/LICENSE")
        self.assertEqual(dict(links)["bannertool"],
                         "https://github.com/carstene1ns/3ds-bannertool/blob/1.2.3/LICENSE.txt")
        self.assertEqual([n for n, _ in toolchain.licence_notice("linux")[1]],
                         ["Debian packages", "devkitPro", "makerom", "bannertool"])
        self.assertEqual([n for n, _ in toolchain.licence_notice("darwin")[1]],
                         ["Apple Command Line Tools", "CMake", "devkitPro", "makerom", "bannertool"])
        self.assertIn("CMake (BSD 3-Clause License)", toolchain.licence_notice("darwin")[0])
        for platform in ("win32", "linux", "darwin"):
            for _, url in toolchain.licence_notice(platform)[1]:
                self.assertTrue(url.startswith("https://"), url)

    def test_every_step_has_a_licence_and_official_page(self):
        for platform in ("win32", "linux", "darwin"):
            steps = toolchain.full_plan("t", "d", platform=platform) + [toolchain.update_step(None, platform)]
            for s in steps:
                with self.subTest(platform=platform, step=s.key):
                    self.assertIsNotNone(s.licence)
                    name, licence, url = s.licence
                    text = s.confirm_text()
                    self.assertTrue(text.startswith(s.consent))
                    self.assertIn("you agree to the licence of %s (%s)" % (name, licence), text)
                    self.assertIn(url, text)
                    if s.downloads:
                        # Downloads come from the official project's own GitHub release page, never a mirror.
                        self.assertTrue(s.page and s.page.startswith("https://github.com/"), s.page)
                        for d in s.downloads:
                            self.assertTrue(d["url"].startswith("https://"), d["url"])
                            self.assertTrue(d["sha256"])

    def test_run_step_reports_each_stage(self):
        steps = {s.key: s for s in toolchain.full_plan("t", "d", platform="win32")}
        heard = []
        acts = toolchain.Actions(download=lambda *a, **kw: None, elevated=lambda p: 0, run=Boom())
        toolchain.run_step(steps["devkitpro"], lambda s: None, False, acts,
                           stage=lambda kind, text: heard.append((kind, text)))
        self.assertEqual(heard, [("download", "Downloading devkitProUpdater-3.0.3.exe..."),
                                 ("run", "Waiting for the installer...")])
        heard.clear()
        toolchain.run_step(steps["packages"], lambda s: None, False, toolchain.Actions(run=lambda argv, log: 0),
                           stage=lambda kind, text: heard.append((kind, text)))
        self.assertEqual(heard, [("run", "Installing...")])
        heard.clear()
        toolchain.run_step(steps["packages"], lambda s: None, True, toolchain.Actions(run=Boom()),
                           stage=lambda kind, text: heard.append((kind, text)))
        self.assertEqual(heard, [])


def fake_zip(member, body=b"MZ fake"):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(member, body)
    return buf.getvalue()


class FakeResponse(io.BytesIO):
    def __init__(self, body):
        super().__init__(body)
        self.headers = {"Content-Length": str(len(body)), "Content-Type": "application/zip"}

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class BuilderUiTest(unittest.TestCase):
    """The one-click setup buttons and the page layout rules from the UI review. Fake downloads only."""
    setUp = GuiTest.setUp
    tearDown = GuiTest.tearDown

    def wizard(self, **kw):
        t = SteamTree(self.tmp)
        kw.setdefault("toolchain_dry_run", False)
        opts = self.gui.Options(game="undertale", work=os.path.join(self.tmp, "work"), probes=t.probes(), **kw)
        wiz = self.gui.App(self.root, opts)

        def work(job, done, errors=False):
            try:
                res = job()
            except Exception as e:  # noqa: BLE001
                res = e
            done(res)
        wiz.work = work                               # runs each job at once, on this thread
        wiz.call = lambda fn, *a: fn(*a)
        return wiz

    def toolchain_fixture(self):
        tmp = os.path.join(self.tmp, "dkp")
        root = fake_devkitpro(tmp, cia=False)
        return toolchain.detect(os.path.join(tmp, "tools"), platform="win32",
                                run=pacman(ToolchainTest.ALL), roots=[root])

    def fake_network(self, bodies):
        """toolchain.download with its real pin checks, reading bytes from a dict instead of the internet."""
        real = toolchain.download
        fetched = []

        def opener(request, timeout):
            url = request.full_url if hasattr(request, "full_url") else request
            fetched.append(url)
            if url not in bodies:
                raise AssertionError("unexpected download %s" % url)
            return FakeResponse(bodies[url])
        patch = mock.patch.object(toolchain, "download",
                                  lambda url, dest, sha, progress=None, size=None:
                                  real(url, dest, sha, opener=opener, progress=progress, size=size))
        patch.start()
        self.addCleanup(patch.stop)
        return fetched

    def open_toolchain_page(self, wiz, tc):
        patch = mock.patch.object(self.gui.toolchain, "detect", return_value=tc)
        detect = patch.start()
        self.addCleanup(patch.stop)
        wiz.show(3)
        self.assertEqual(wiz.page.title, "Set up the 3DS compiler")
        return detect

    def box(self, page, key):
        return [b for b in page.boxes if b.step.key == key and b.box.winfo_exists()][0]

    def test_install_button_downloads_checks_and_reports_done(self):
        asked = []
        wiz = self.wizard(confirm_install=lambda title, text: asked.append((title, text)) or True)
        tc = self.toolchain_fixture()
        detect = self.open_toolchain_page(wiz, tc)
        page = wiz.page
        self.assertEqual([b.step.key for b in page.boxes], ["makerom", "bannertool"])
        body = fake_zip("makerom-v0.18.4/makerom.exe")
        pin = toolchain.CIA_TOOLS["makerom"]
        fetched = self.fake_network({pin["url"]: body})
        with mock.patch.dict(pin, sha256=hashlib.sha256(body).hexdigest()):
            wiz.page.detect()                          # rebuild the steps with the fixture's pin
            box = self.box(page, "makerom")
            self.assertEqual(str(box.go.cget("text")), "Install")
            self.assertEqual(box.show_btn.winfo_manager(), "")   # "Show command" only after a failure
            self.assertTrue(page.run_step(box))
        self.assertEqual(fetched, [pin["url"]])
        self.assertEqual(len(asked), 1)
        self.assertIn("MIT License", asked[0][1])
        self.assertIn("https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.18.4/makerom/LICENSE", asked[0][1])
        self.assertTrue(os.path.isfile(os.path.join(wiz.opts.tools_dir, "makerom.exe")))
        self.assertEqual(page.result.cget("text"), "Done: %s." % box.step.title)
        self.assertFalse(box.box.winfo_exists())         # the re-check rebuilt the list of steps
        self.assertGreaterEqual(detect.call_count, 3)    # checks again after the install
        self.assertFalse(page.running_step)

    def test_hash_mismatch_fails_and_keeps_the_manual_way(self):
        wiz = self.wizard(confirm_install=lambda title, text: True)
        self.open_toolchain_page(wiz, self.toolchain_fixture())
        page = wiz.page
        pin = toolchain.CIA_TOOLS["makerom"]
        self.fake_network({pin["url"]: fake_zip("makerom-v0.18.4/makerom.exe", b"not the pinned bytes")})
        with mock.patch.object(self.gui.webbrowser, "open") as browser:
            self.assertTrue(page.run_step(self.box(page, "makerom")))
            self.assertFalse(os.path.exists(os.path.join(wiz.opts.tools_dir, "makerom.exe")))
            self.assertIn("SHA-256", page.failures["makerom"])
            self.assertIn("Show command", page.result.cget("text"))
            # The re-check rebuilt the boxes; the new one still shows the failure and the manual buttons.
            box = self.box(page, "makerom")
            self.assertTrue(box.status.cget("text").startswith("Failed: "))
            self.assertEqual(box.show_btn.winfo_manager(), "pack")
            self.assertEqual(box.page_btn.winfo_manager(), "pack")
            self.assertEqual(box.bar.winfo_manager(), "")
            self.assertTrue(box.go.instate(["!disabled"]))
            box.show_command()
            self.assertIn(pin["url"], page.log.get("1.0", "end"))
            box.page_btn.invoke()
            browser.assert_called_once_with("https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.18.4")
            other = self.box(page, "bannertool")
            self.assertEqual(other.show_btn.winfo_manager(), "")

    def test_server_block_and_declined_consent(self):
        # devkitpro.org answers some clients with an HTML block page: that is a failure, never a retry trick.
        wiz = self.wizard(confirm_install=lambda title, text: False)
        self.open_toolchain_page(wiz, self.toolchain_fixture())
        page = wiz.page
        self.fake_network({})                          # any download would fail the test
        box = self.box(page, "makerom")
        self.assertFalse(page.run_step(box))           # declined: nothing runs
        self.assertEqual(box.status.winfo_manager(), "")
        self.assertEqual(page.failures, {})
        page.update()                                  # Update devkitPro, declined: no box is left behind
        self.assertIsNone(page.update_box)
        self.assertEqual(page.extra.winfo_children(), [])

    def test_html_block_page_is_a_clear_failure(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(toolchain.Refused) as e:
            toolchain.download("https://pkg.devkitpro.org/x.pkg.tar.zst", os.path.join(tmp, "x"), "0" * 64,
                               opener=lambda req, timeout: FakeResponse(b"<!DOCTYPE html><p>blocked"))
        self.assertIn("devkitPro", str(e.exception))

    def test_update_asks_once_and_runs_the_official_updater(self):
        asked, ran = [], []
        wiz = self.wizard(confirm_install=lambda title, text: asked.append(title) or True)
        tc = self.toolchain_fixture()
        self.open_toolchain_page(wiz, tc)
        page = wiz.page
        self.assertTrue(page.update_btn.instate(["!disabled"]))
        # Exercise each host's actual updater plan, with execution always mocked.
        for platform in ("win32", "linux", "darwin"):
            with self.subTest(platform=platform):
                asked.clear()
                ran.clear()
                step = toolchain.update_step(tc.root, platform=platform)
                with mock.patch.object(self.gui.toolchain, "update_step", return_value=step), \
                        mock.patch.object(self.gui, "stream", side_effect=lambda argv, log: ran.append(argv) or 0):
                    page.update()
                title = "Update devkitPro (pacman -Syu)" if platform == "win32" else "Update devkitPro"
                self.assertEqual(asked, [title])
                self.assertEqual(ran, step.commands)
                self.assertEqual(len(ran), 1)
                if platform == "win32":
                    self.assertEqual(ran[0][-1], "pacman -Syu --noconfirm")
                elif platform == "linux":
                    self.assertEqual(ran[0][0], "pkexec")
                    self.assertEqual(ran[0][-2:], ["-Syu", "--noconfirm"])
                else:
                    self.assertEqual(ran[0][0], "/usr/bin/osascript")
                    self.assertIn("with administrator privileges", ran[0][2])
                    self.assertTrue(ran[0][-1].endswith(" -Syu --noconfirm"))
                self.assertEqual(page.update_box.status.cget("text"), "Done.")

    def test_licence_line_and_links_on_the_page(self):
        wiz = self.wizard()
        self.open_toolchain_page(wiz, self.toolchain_fixture())
        page = wiz.page
        text, links = toolchain.licence_notice(sys.platform)
        self.assertEqual(page.notice.cget("text"), text)
        found = []

        def walk(w):
            for k in w.winfo_children():
                if getattr(k, "_url", None):
                    found.append(k)
                walk(k)
        walk(page.frame)
        self.assertEqual([k._url for k in found], [u for _, u in links])
        self.root.deiconify()
        self.root.update()
        with mock.patch.object(self.gui.webbrowser, "open") as browser:
            link = found[0]
            self.assertTrue(int(link.cget("takefocus")))
            keyboard_binding = link.bind("<Return>")
            self.assertTrue(keyboard_binding)
            # Dispatch the actual Return binding without depending on the desktop window manager's
            # asynchronous keyboard focus (notably WSLg on a shared Windows desktop).
            link.bind("<<TestLinkReturn>>", keyboard_binding)
            link.event_generate("<<TestLinkReturn>>")
            self.root.update()
            browser.assert_called_once_with(links[0][1])
            browser.reset_mock()
            link.event_generate("<Button-1>")
            self.root.update()
            browser.assert_called_once_with(links[0][1])

    def test_graphics_phase_is_named_and_updates_in_both_looks(self):
        wiz = self.wizard()
        page = wiz.pages[5]
        clock = Clock()
        progress = pipeline.Progress(wiz.state.product, 'n3ds', clock)
        progress.feed('Builder phase: Preparing graphics')
        wiz.index = 5
        page.running = True
        page.build = mock.Mock(progress=progress)
        for native in (True, False):
            wiz.theme.runtime_loaded = True
            wiz.theme.boring = native
            wiz.theme.apply()
            wiz.poll()  # No new subprocess line: the GUI clock still updates this phase.
            before = float(page.bar.cget('value'))
            clock.t += 7.0
            wiz.poll()
            self.assertIn('Preparing graphics', page.eta.cget('text'))
            self.assertGreater(float(page.bar.cget('value')), before)
        page.running = False

    def test_dry_run_buttons_only_show_commands(self):
        wiz = self.wizard(toolchain_dry_run=True, confirm_install=lambda *a: self.fail("no confirm in a dry run"))
        self.open_toolchain_page(wiz, self.toolchain_fixture())
        page = wiz.page
        self.fake_network({})
        box = self.box(page, "makerom")
        self.assertEqual(str(box.go.cget("text")), "Show command")
        self.assertTrue(page.run_step(box))
        self.assertIn("[dry run] would do", page.log.get("1.0", "end"))

    def test_deliver_page_shows_only_the_current_mode(self):
        wiz = self.wizard()
        wiz.state.build_ok = True
        wiz.state.artifacts = []
        with mock.patch.object(self.gui.deliver, "list_sd_cards", return_value=[]):
            wiz.show(6)
            page = wiz.page
            page.how.set("sd")
            page.mode()
        self.assertEqual(page.sd.winfo_manager(), "pack")
        self.assertIn("No 3DS SD card found", page.status.cget("text"))
        self.assertEqual(page.status.winfo_manager(), "pack")
        self.assertTrue(page.go.instate(["disabled"]))
        page.how.set("files")
        page.mode()
        # Files mode: no card list, no Copy button, no SD message and no progress bar.
        self.assertEqual(page.sd.winfo_manager(), "")
        self.assertEqual(page.status.winfo_manager(), "")
        self.assertEqual(page.bar.winfo_manager(), "")
        self.assertEqual(page.files.winfo_manager(), "pack")
        page.how.set("sd")
        page.mode()
        self.assertEqual(page.status.winfo_manager(), "pack")
        self.assertEqual(page.texts["files"], "")

    def test_buttons_keep_their_size_and_long_paths_wrap(self):
        wiz = self.wizard()
        wiz.theme.runtime_loaded = True
        wiz.theme.apply()
        theme = wiz.theme
        for text in ("< Back", "Next >", "Install"):
            idle = theme.render_button_image(text)
            hover = theme.render_button_image(text, active=True, show_soul=True)
            self.assertEqual((idle.width(), idle.height()), (hover.width(), hover.height()))
        path = "C:\\InventedLibrary\\BuilderFixtures\\LongFolderName\\MoreFixtureFolders\\Games\\steamapps"
        lines = theme.wrap_lines("Checking " + path, 300)
        self.assertGreater(len(lines), 1)
        for line in lines:
            self.assertLessEqual(theme.measure_line(line, 1), 300, line)
        self.assertEqual("".join(lines).replace(" ", ""), ("Checking" + path))

    def test_minimum_window_scrolls_and_tab_reaches_page_before_nav(self):
        wiz = self.wizard()
        wiz.theme.runtime_loaded = True
        wiz.theme.apply()
        clean = toolchain.Toolchain(None, None, [toolchain.Check("devkitPro", False, "not found", "build")],
                                    list(toolchain.REQUIRED_PACKAGES), list(toolchain.RECOMMENDED_PACKAGES),
                                    None, None, None, "win32")
        self.open_toolchain_page(wiz, clean)
        self.root.deiconify()
        w, h = self.root.minsize()
        self.root.geometry("%dx%d" % (w, h))
        for _ in range(3):
            self.root.update()
            wiz.page.fit()
        page = wiz.page
        self.assertTrue(page.scrollable())
        for lbl in page.wrappers:
            if lbl.winfo_exists():
                self.assertLessEqual(lbl._wrap, max(120, page.wrap_width(lbl)))
        self.root.geometry("1400x1100")
        wiz.show(0)
        for _ in range(3):
            self.root.update()
            wiz.page.fit()
        self.assertFalse(wiz.page.scrollable())
        # Tab order: every page control, then About, Boring mode, Back, Next.
        wiz.show(4)
        self.root.update()
        order, w = [], wiz.next_btn
        for _ in range(80):
            w = w.tk_focusNext()
            if w is None or w in order:
                break
            order.append(w)
        nav = [o for o in order if not str(o).startswith(str(wiz.page.box) + ".")]
        on_page = [o for o in order if str(o).startswith(str(wiz.page.box) + ".")]
        self.assertTrue(on_page)
        self.assertEqual(order[:len(on_page)], on_page)
        self.assertLess(nav.index(wiz.back_btn), nav.index(wiz.next_btn))
        self.assertEqual(nav[-1], wiz.next_btn)

    def test_all_pages_fit_at_100_and_200_percent_in_both_looks(self):
        wiz = self.wizard()
        wiz.work = lambda *args, **kw: None  # no real detection, builds or installers
        wiz.state.build_ok = wiz.state.delivered = True
        wiz.theme.runtime_loaded = True  # synthetic fallback bitmap font
        self.root.deiconify()
        with mock.patch.object(self.gui.deliver, "list_sd_cards", return_value=[]), \
                mock.patch.object(wiz.pages[5], "start"):
            for grow in (1, 2):
                self.root.tk.call("tk", "scaling", grow * 96 / 72)
                width = min(680 * grow, self.root.winfo_screenwidth() - 40)
                height = min(560 * grow, self.root.winfo_screenheight() - 80)
                self.root.minsize(width, height)
                self.root.geometry("%dx%d" % (width, height))
                for boring in (False, True):
                    wiz.theme.boring = boring
                    wiz.theme.apply()
                    for index in range(len(wiz.pages)):
                        with self.subTest(scale=grow, boring=boring, page=index + 1):
                            wiz.show(index)
                            for _ in range(3):
                                self.root.update()
                                wiz.page.fit()
                            page = wiz.page
                            self.assertEqual(wiz.theme.native, boring)
                            self.assertLessEqual(page.frame.winfo_width(), page.canvas.winfo_width())
                            for widget in page.wrappers:
                                self.assertLessEqual(widget._wrap, max(120, page.wrap_width(widget)))
                            self.assertLessEqual(wiz.back_btn.winfo_rootx() + wiz.back_btn.winfo_width(),
                                                 wiz.next_btn.winfo_rootx())
                            self.assertLessEqual(wiz.next_btn.winfo_rootx() + wiz.next_btn.winfo_width(),
                                                 self.root.winfo_rootx() + self.root.winfo_width())


if __name__ == "__main__":
    unittest.main()
