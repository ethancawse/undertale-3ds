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
GOG_ALLOW = [ALLOW[0][:-2] + ('gog', 'test GOG 1.0')]


def fixture(name, **subs):
    with open(os.path.join(FIX, name), encoding="utf-8") as f:
        text = f.read()
    for k, v in subs.items():
        text = text.replace("{%s}" % k, v)
    return text


def fake_exe(body=b"invented program body"):
    """The smallest Windows program header toolchain.complete_windows_exe reads: one section holding `body`."""
    header = bytearray(0x80)
    header[:2] = b"MZ"
    header[0x3C:0x40] = (0x40).to_bytes(4, "little")
    header[0x40:0x44] = b"PE\x00\x00"
    header[0x44:0x46] = (0x8664).to_bytes(2, "little")
    header[0x46:0x48] = (1).to_bytes(2, "little")  # One section; no optional header.
    section = 0x58
    header[section:section + 5] = b".text"
    header[section + 16:section + 20] = len(body).to_bytes(4, "little")
    header[section + 20:section + 24] = (0x80).to_bytes(4, "little")
    return bytes(header) + body


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


def fixture_music_manifest():
    entries = [dict(name='mus_%d.ogg' % i, size=3 * (i + 1),
                    sha256=hashlib.sha256(b'ogg' * (i + 1)).hexdigest()) for i in range(3)]
    return {'schema': 2, 'game': 'undertale', 'releases': {'steam': {'windows': {
        'test 1.0': {'files': entries}}}, 'gog': {'windows': {'test GOG 1.0': {'files': entries}}}}}


class MusicFixtures(unittest.TestCase):
    def setUp(self):
        entries = [dict(name='mus_%d.ogg' % i, size=3 * (i + 1),
                        sha256=hashlib.sha256(b'ogg' * (i + 1)).hexdigest()) for i in range(3)]
        patch = mock.patch.object(products, 'load_music_manifest', return_value=entries)
        patch.start()
        self.addCleanup(patch.stop)
        patch = mock.patch.object(products, 'load_credits_manifest', return_value=dict(
            name='credits.txt', size=len(b'names\n'), sha256=hashlib.sha256(b'names\n').hexdigest()))
        patch.start()
        self.addCleanup(patch.stop)
        manifest = mock.patch.object(products, 'read_music_manifest', return_value=fixture_music_manifest())
        manifest.start()
        self.addCleanup(manifest.stop)


class DiscoveryTest(MusicFixtures):
    def test_steam_library_from_fixture_vdf(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            copies = discover.find_copies("undertale", t.probes())
            self.assertEqual(len(copies), 1)
            c = copies[0]
            self.assertEqual((c.store, os.path.normcase(c.folder)), ("Steam", os.path.normcase(t.folder)))
            self.assertEqual(c.version, "Not checked yet")
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
            self.assertEqual(c.note, "")

    def test_nothing_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp, install=False)
            self.assertEqual(discover.find_copies("undertale", t.probes()), [])

    def test_empty_demo_folder_is_not_offered(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.lib, "steamapps", "appmanifest_1690940.acf"),
                  fixture("appmanifest_1690940.acf", OWNER=OWNER))
            os.makedirs(os.path.join(t.lib, "steamapps", "common", "DELTARUNEdemo"))
            copies = discover.find_copies("deltarune", t.probes())
            self.assertEqual(copies, [])

    def test_gog_from_registry_export_and_verify(self):
        with tempfile.TemporaryDirectory() as tmp:
            gog = game_folder(os.path.join(tmp, "GOG Games", "Undertale"))
            write(os.path.join(gog, "goggame-1456487183.info"), '{"gameId": "1456487183", "name": "Undertale"}')
            reg = discover.registry_from_export(fixture("gog.reg", GOG_REG=vdf_path(gog)))
            probes = discover.Probes(steam_roots=[], registry=reg, platform="win32")
            copies = discover.find_copies("undertale", probes)
            self.assertEqual([(c.store, os.path.normcase(c.folder)) for c in copies], [("GOG", os.path.normcase(gog))])
            v = discover.verify_copy("undertale", gog, probes, allow_list=GOG_ALLOW)
            self.assertTrue(v.accepted, v.message)
            self.assertEqual(v.result.store, "gog")

    def test_gog_copy_needs_no_galaxy_database(self):
        with tempfile.TemporaryDirectory() as tmp:
            gog = game_folder(os.path.join(tmp, "Galaxy", "Undertale"))
            probes = discover.Probes(steam_roots=[], gog_dirs=[], registry=lambda *a: None, platform="win32")
            with mock.patch.object(sqlite3, 'connect', side_effect=AssertionError('store database read')):
                self.assertEqual(discover.find_copies("undertale", probes), [])
                self.assertTrue(discover.verify_copy("undertale", gog, probes, allow_list=GOG_ALLOW).accepted)

    def test_manual_gog_folder_needs_only_known_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            gog = game_folder(os.path.join(tmp, "manual"))
            probes = discover.Probes(steam_roots=[], gog_dirs=[], registry=lambda *a: None, platform="win32")
            allow = [ALLOW[0][:-2] + ('gog', 'GOG fixture')]
            for metadata in (None, '{"gameId": "wrong product"}', 'malformed'):
                if metadata is not None:
                    write(os.path.join(gog, 'goggame-1456487183.info'), metadata)
                verdict = discover.verify_copy("undertale", gog, probes, allow_list=allow)
                self.assertTrue(verdict.accepted, verdict.message)
                self.assertEqual(verdict.result.store, "gog")
                self.assertEqual(verdict.platform, "Windows")
                self.assertEqual(products.load_music_manifest.call_args.kwargs, {'store': 'gog', 'platform': 'windows'})


class VerifyTest(MusicFixtures):
    def test_music_same_size_corruption_names_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.folder, 'mus_1.ogg'), b'badbad')
            verdict = discover.verify_copy('undertale', t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(verdict.accepted)
            self.assertIn('mus_1.ogg does not match the supported original', verdict.message)
            self.assertIn('Restore the original music files', verdict.message)

    def test_music_wrong_size_names_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            write(os.path.join(t.folder, 'mus_1.ogg'), b'bad')
            verdict = discover.verify_copy('undertale', t.folder, t.probes(), allow_list=ALLOW)
            self.assertFalse(verdict.accepted)
            self.assertIn('mus_1.ogg does not match the supported original', verdict.message)

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
            self.assertTrue(verdict.accepted, verdict.message)
            self.assertNotIn('custom.ogg', [name for _, name, _ in products.sd_files(products.PRODUCTS['undertale'], t.folder)])

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
            self.assertTrue(v.accepted, v.message)

    def test_hand_picked_folder_without_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            loose = game_folder(os.path.join(tmp, "copy"))
            v = discover.verify_copy("undertale", loose, t.probes(), allow_list=ALLOW)
            self.assertTrue(v.accepted, v.message)

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
            entries = [dict(name='mus_%d.ogg' % i, size=3, sha256='0' * 64) for i in range(212)]
            with mock.patch.object(products, 'load_music_manifest', return_value=entries):
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
        write(os.path.join(root, "tools", "bin", "makerom.exe"), fake_exe())
        write(os.path.join(root, "tools", "bin", "bannertool.exe"), fake_exe())
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
            write(os.path.join(tools, "makerom.exe"), fake_exe())
            write(os.path.join(tools, "bannertool.exe"), fake_exe())
            tc = toolchain.detect(tools, platform="win32", run=pacman(self.ALL), roots=[root])
            self.assertTrue(tc.can_cia)

    def test_installer_env_value_is_not_a_windows_path(self):
        with mock.patch.object(toolchain, 'windows_install_roots', return_value=[]):
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
                              (right_size, "expected file")):
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
        self.assertIn("The 3DS tools could not be installed", str(e.exception))
        elevated = []
        acts = toolchain.Actions(download=lambda *a, **kw: None, elevated=lambda p: elevated.append(p) or 0, run=Boom())
        toolchain.run_step(steps["devkitpro"], lambda s: None, False, acts)
        self.assertEqual(len(elevated), 1)

    def test_update_failure_names_the_update_on_every_platform(self):
        for platform in ('win32', 'darwin', 'linux'):
            with self.subTest(platform=platform):
                step = toolchain.update_step('fixture-devkitpro', platform=platform)
                run = mock.Mock(return_value=1)
                actions = toolchain.Actions(run=run, download=Boom(), elevated=Boom())
                with self.assertRaises(toolchain.Refused) as caught:
                    toolchain.run_step(step, lambda line: None, False, actions)
                self.assertEqual(str(caught.exception), 'The devkitPro update did not finish. '
                    'Check the error details, then try this step again.')
                run.assert_called_once()


# ---------------------------------------------------------------------------------------------------------------------

class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class ProgressTest(unittest.TestCase):
    def test_parallel_compile_percent_never_moves_backwards(self):
        p = pipeline.Progress(products.PRODUCTS['undertale'], 'o3ds', Clock())
        percentages, displayed = [], []
        for percent in (80, 79, 80, 98, 100, 40):
            p.feed('[%3d%%] Building C object fixture.c.obj\n' % percent)
            percentages.append(p.percent)
            displayed.append(p.completion())
        self.assertEqual(percentages, [80, 80, 80, 98, 100, 100])
        self.assertEqual(displayed, sorted(displayed))
        p.feed('[100%] Building CIA fixture.cia\n')
        self.assertEqual(p.activity, 'Making .cia')

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
        p.feed("AOT:   skip gml_Script_x: entry missing or instruction analysis failed "
               "before generation\n")                   # one function left to the interpreter, the build keeps AOT
        self.assertFalse(p.no_aot)
        p.feed("WARNING: building WITHOUT AOT: no UNDERTALE game data found.\n")
        self.assertTrue(p.no_aot)
        self.assertIn("left", pipeline.format_eta((100, 400)))
        q = pipeline.Progress(products.PRODUCTS["deltarune"], "n3ds", clk)
        q.feed("AOT: tools/aot/prepare.py failed (exit 1); aot=off builds without AOT\n")
        self.assertTrue(q.no_aot)

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


class CapturedEnv(dict):
    """A child environment a fake popen captured. Every repr (unittest's failure messages, pprint diffs) shows only how
    many variables it holds, so no failed assertion prints a value from the developer's environment."""
    def __repr__(self):
        return '<captured environment, %d variables>' % len(self)

    __str__ = __repr__


class CapturedEnvTest(unittest.TestCase):
    def test_a_failed_assertion_never_prints_a_captured_environment(self):
        canary = 'invented-canary-value-7f3a'
        seen = {'argv': ['bash'], 'env': CapturedEnv({'INVENTED_API_KEY': canary})}
        checks = {
            'whole capture equal': lambda: self.assertEqual(seen, {}),
            'whole capture dict equal': lambda: self.assertDictEqual(seen, {}),
            'env equal': lambda: self.assertEqual(seen['env'], {}),
            'env dict equal': lambda: self.assertDictEqual(seen['env'], {}),
            'env not in': lambda: self.assertNotIn('env', seen),
            'key in env': lambda: self.assertIn('PATH', seen['env']),
            'env is none': lambda: self.assertIsNone(seen['env']),
            'env falsy': lambda: self.assertFalse(seen['env']),
        }
        for name, check in checks.items():
            with self.subTest(name):
                with self.assertRaises(AssertionError) as caught:
                    check()
                self.assertNotIn(canary, str(caught.exception))
        self.assertEqual(seen['env']['INVENTED_API_KEY'], canary)


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
    def test_build_time_game_file_refusals_are_visible_and_return_to_selection_on_each_host(self):
        import gui
        paths = {'win32': r'C:\Games\UNDERTALE\data.win',
                 'linux': '/opt/steam/steamapps/common/UNDERTALE/data.win',
                 'darwin': '/Applications/UNDERTALE.app/Contents/Resources/data.win'}
        for platform, path in paths.items():
            diagnostics = (
                ("Refusing to build: %s is not an unmodified Steam or GOG copy of UNDERTALE "
                 "(SHA-256 %s)." % (path, 'f' * 64),
                 "These UNDERTALE files don't match an official release this builder knows."),
                ('Refusing to build: could not read %s (Permission denied).' % path,
                 'Could not read the UNDERTALE game files.'),
                ('Refusing to build: could not read %s (file changed while checking).' % path,
                 'The UNDERTALE game files changed while they were being checked.'),
                ('Could not find the game files.',
                 'Could not find the UNDERTALE game files.'),
            )
            for diagnostic, expected in diagnostics:
                with self.subTest(platform=platform, diagnostic=diagnostic):
                    self.tc.platform = platform
                    b, seen = self.make([diagnostic + '\n'], rc=2)
                    with self.assertRaises(pipeline.GameFilesRefused) as caught:
                        b.run()
                    self.assertIn('argv', list(seen))  # Refusal happens in the child after wizard verification.
                    self.assertIn(diagnostic, Path(b.log_path).read_text(encoding='utf-8'))
                    page = gui.BuildPage.__new__(gui.BuildPage)
                    page.app = mock.Mock(state=gui.State('undertale'), PAGES=gui.App.PAGES)
                    for attr in ('packaging', 'details', 'intro', 'status', 'cancel_btn', 'retry_btn', 'copy_btn'):
                        setattr(page, attr, mock.Mock())
                    page.finished(caught.exception)
                    text = page.status.config.call_args.kwargs['text']
                    self.assertIn(expected, text)
                    self.assertIn('Go back to choose the game files.', text)
                    self.assertNotIn('Try again', text)
                    self.assertNotIn('build.sh exit', text)
                    self.assertFalse(page.can_next())
                    page.retry_btn.config.assert_called_with(text='Choose game files', command=page.choose_game_files)
                    page.retry_btn.state.assert_called_with(['!disabled'])
                    page.app.state.verdict = mock.Mock(accepted=True)
                    page.app.state.folder = path
                    page.app.state.artifacts, page.app.state.records = ['old'], ['old']
                    before = page.st.inputs
                    page.retry_btn.config.call_args.kwargs['command']()
                    page.app.show.assert_called_once_with(gui.App.PAGES.index(gui.FindPage))
                    self.assertIsNone(page.st.verdict)
                    self.assertEqual(page.st.folder, path)
                    self.assertEqual((page.st.artifacts, page.st.records), ([], []))
                    self.assertEqual(page.st.inputs, before + 1)
                    self.assertFalse(page.st.build_ok)

    def test_other_exit_two_failures_keep_the_build_retry(self):
        import gui
        b, _ = self.make(['fixture.c:1: error: ordinary failure\n'], rc=2)
        with self.assertRaises(pipeline.BuildFailed) as caught:
            b.run()
        self.assertNotIsInstance(caught.exception, pipeline.GameFilesRefused)
        self.assertIn('Try again', str(caught.exception))
        page = gui.BuildPage.__new__(gui.BuildPage)
        page.failed, page.running, page.log_path = caught.exception, False, b.log_path
        page.cancel_btn, page.retry_btn, page.copy_btn = mock.Mock(), mock.Mock(), mock.Mock()
        page.buttons()
        page.retry_btn.config.assert_called_with(text='Try again', command=page.start)

    def test_choose_game_files_does_nothing_while_a_build_is_running(self):
        import gui
        page = gui.BuildPage.__new__(gui.BuildPage)
        page.app = mock.Mock(state=gui.State('undertale'))
        page.running = True
        state = page.app.state
        state.verdict, state.build_ok = object(), True
        state.artifacts, state.records = ['kept'], ['kept']
        before = vars(state).copy()
        page.choose_game_files()
        page.app.show.assert_not_called()
        self.assertEqual(vars(state), before)

    @unittest.skipIf(app.FIXED_GAME != "deltarune" and not Path(HERE, "builder", "chapters.py").is_file(),
                     "DELTARUNE-only chapters.py is not included in this UNDERTALE source tree")
    def test_deltarune_staging_errors_keep_the_exact_cause_on_each_host(self):
        import chapters
        import errno
        game = Path(self.tmp) / 'game'
        source_file = str(game / 'chapter3_windows/data.win')
        work_file = str(Path(self.work) / 'data/deltarune/chapter3_windows/data.win')
        action = ' Go back to choose the game files.'
        space = 'Not enough space for the selected chapters. Free up space and try again.'
        work = ('Could not prepare the selected chapters in the work folder. '
                'Check free space and folder permissions. '
                'Click Try again, or click Copy log to report the error.')
        cases = [
            (chapters.UnknownVersion(chapters.UNKNOWN_MESSAGE),
             "These DELTARUNE files don't match an official release this builder knows. "
             'Restore the complete original game folder, or try a newer builder.' + action, True),
            (OSError(errno.ENOSPC, 'raw disk full', work_file), space, False),
            # Disk full must win over both ambiguous-copy error shapes.
            (OSError(errno.ENOSPC, 'raw fast-copy disk full', source_file, None, work_file), space, False),
            (chapters.StageCopyError(errno.ENOSPC, 'raw wrapped disk full'), space, False),
            (PermissionError(errno.EACCES, 'raw source denied', source_file),
             'Could not read the DELTARUNE game files. Check that the game folder is readable '
             'and close anything that is using the files.' + action, True),
            (PermissionError(errno.EACCES, 'raw destination denied', work_file), work, False),
            (OSError('raw error without a filename'), work, False),
        ]
        copy = ('Could not copy the selected chapters. Check that the game folder is readable '
                'and that the work folder has free space and allows these files. '
                'Click Try again, or click Copy log to report the error.')
        for code in (errno.EDQUOT, errno.EFBIG, errno.EIO, errno.EACCES):
            # CPython fast-copy: filename points to source even for a destination error.
            cases.append((OSError(code, 'raw fast-copy failure', source_file, None, work_file), copy, False))
            # Windows copy: no filenames. The operation wrapper retains both sides.
            cases.append((chapters.StageCopyError(code, 'raw copy failure'), copy, False))
        for message in (space, 'Choose a work folder outside the game folder.',
                        'The staged files are shared and read-only. Choose another work folder.'):
            cases.append((ValueError(message), message, False))
        for message in ('Choose at least one installed chapter.', 'The launcher is missing.',
                        'Chapter 3 is missing or unreadable.',
                        'These chapters are from different versions. Restore the complete original game folder, then try again.',
                        'Missing or unreadable audio: fixture.ogg. Restore the complete original game folder, then try again.'):
            cases.append((chapters.GameFilesError(message), message + action, True))
        for host in ('win32', 'linux', 'darwin'):
            for phase in ('selection', 'staging'):
                for error, expected, select_again in cases:
                    with self.subTest(host=host, phase=phase, error=str(error)):
                        self.tc.platform = host
                        b, seen = self.make([])
                        b.product, b.data_dir = products.PRODUCTS['deltarune'], str(game)
                        if phase == 'staging':
                            prepare = mock.patch.object(b, 'prepare', return_value=('0.1.0', 0, 0))
                            failure = mock.patch.object(chapters, 'stage', side_effect=error)
                        else:
                            prepare = mock.patch.object(chapters, 'stage')
                            failure = mock.patch.object(chapters, 'runtime_selection', side_effect=error)
                        with prepare, failure, self.assertRaises(pipeline.BuildFailed) as caught:
                            b.run()
                        self.assertEqual(str(caught.exception), expected)
                        self.assertEqual(isinstance(caught.exception, pipeline.GameFilesRefused), select_again)
                        self.assertNotIn('GOG', str(caught.exception))
                        self.assertIn(str(error), Path(b.log_path).read_text(encoding='utf-8'))
                        self.assertNotIn('argv', seen)

    @unittest.skipIf(app.FIXED_GAME != "deltarune" and not Path(HERE, "builder", "chapters.py").is_file(),
                     "DELTARUNE-only chapters.py is not included in this UNDERTALE source tree")
    def test_unwritable_work_folder_does_not_hide_the_original_staging_cause(self):
        import chapters
        b, seen = self.make([])
        b.product = products.PRODUCTS['deltarune']
        b.log_path = str(Path(self.tmp) / 'blocked/log.txt')
        (Path(self.tmp) / 'blocked').write_text('fixture', encoding='ascii')
        message = 'Choose a work folder outside the game folder.'
        with mock.patch.object(chapters, 'runtime_selection', side_effect=ValueError(message)), \
                self.assertRaises(pipeline.BuildFailed) as caught:
            b.run()
        self.assertEqual(str(caught.exception), message)
        self.assertNotIsInstance(caught.exception, pipeline.GameFilesRefused)
        self.assertIsNone(caught.exception.log_path)
        self.assertNotIn('argv', seen)

    def test_art_failures_keep_details_in_the_log_and_offer_file_selection(self):
        for host in ('win32', 'linux', 'darwin'):
            with self.subTest(host=host):
                self.tc.platform = host
                b, seen = self.make([])
                b.art_generator = mock.Mock(side_effect=pipeline.home_art.ArtError('Missing player asset: fixture'))
                with self.assertRaises(pipeline.GameFilesRefused) as caught:
                    b.run()
                text = str(caught.exception)
                self.assertEqual(text, 'Could not prepare HOME Menu art for UNDERTALE. ''Click Copy log to report the error. Go back to choose the game files.')
                self.assertNotIn('may have changed', text)
                self.assertIn('Go back to choose the game files.', text)
                self.assertNotIn('Missing player asset', text)
                self.assertNotIn('Try again', text)
                self.assertIn('Missing player asset: fixture', Path(b.log_path).read_text(encoding='utf-8'))
                self.assertNotIn('argv', seen)

    def test_missing_optional_cia_tools_delivers_fresh_3dsx_on_each_host(self):
        for platform in ('win32', 'linux', 'darwin'):
            for requested in (('3dsx', 'cia'), ('cia',)):
                with self.subTest(platform=platform, requested=requested):
                    self.tc.platform = platform
                    b, _ = self.make(['[100%] y\n'], kinds=('3dsx', 'cia'), outputs=requested)
                    # A prior successful CIA still present must never be delivered.
                    lines = []
                    b.on_line = lines.append
                    arts, record = b.run()
                    self.assertEqual([a.kind for a in arts], ['3dsx'])
                    self.assertEqual(record['notices'], [pipeline.CIA_NOT_MADE])
                    self.assertIn(pipeline.CIA_NOT_MADE, lines)
                    self.assertIn(pipeline.CIA_NOT_MADE, Path(b.log_path).read_text())
                    self.assertEqual(Path(arts[0].path).read_bytes(), b'3DSX' * 100)

    def test_missing_3dsx_still_fails_without_cia_tools(self):
        b, _ = self.make(['[100%] y\n'], kinds=(), outputs=('3dsx', 'cia'))
        with self.assertRaisesRegex(pipeline.BuildFailed, 'finished game file is missing'):
            b.run()

    def test_no_cia_was_built_but_the_3dsx_is_still_usable(self):
        b, _ = self.make(['[100%] y\n'], outputs=('3dsx', 'cia'))
        arts, record = b.run()
        self.assertEqual([a.kind for a in arts], ['3dsx'])
        self.assertEqual(record['notices'], [pipeline.CIA_NOT_MADE])

    def test_available_cia_tools_deliver_both_requested_formats(self):
        self.tc.makerom, self.tc.bannertool = '/fixture/makerom', '/fixture/bannertool'
        b, _ = self.make(['[100%] y\n'], kinds=('3dsx', 'cia'), outputs=('3dsx', 'cia'))
        arts, record = b.run()
        self.assertEqual([a.kind for a in arts], ['3dsx', 'cia'])
        self.assertEqual(record['notices'], [])

    def setUp(self):
        compiler = mock.patch.object(toolchain, 'compiler_details', return_value={
            'path': '/fixture/gcc', 'version': '16.1.0', 'target': 'arm-none-eabi',
            'vendor': 'devkitARM', 'package': '16.1.0-1'})
        compiler.start()
        self.addCleanup(compiler.stop)
        self.tmp = tempfile.mkdtemp()
        self.src = fake_source(self.tmp)
        self.work = os.path.join(self.tmp, "work")
        self.game = os.path.join(self.tmp, 'game', 'Undertale')
        write(os.path.join(self.game, 'data.win'), UT)
        self.tc = toolchain.Toolchain("C:/devkitPro", "C:/devkitPro/msys2/usr/bin/bash.exe", [], [], [], None, None,
                                      None, "win32")
        selected = mock.patch.object(pipeline.library_pins, 'select', return_value={'packages': [], 'shims': None})
        selected.start()
        self.addCleanup(selected.stop)
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
            seen["argv"], seen["env"] = argv, CapturedEnv(kw["env"])
            return FakeProc(lines, rc, on_start=lambda: rc == 0 and self.outputs(aot, kinds))
        b = pipeline.Build(products.PRODUCTS["undertale"], self.tc, self.work, "n3ds", self.game,
                           outputs=outputs, popen=popen, source_root=self.src, art_generator=lambda *a, **k: {})
        return b, seen

    def test_successful_build(self):
        lines = ["AOT: generating UNDERTALE code for n3ds\n", "-- Configuring done\n", "[ 50%] x\n", "[100%] y\n",
                 "built /w/undertale-3ds-n3ds.3dsx (with AOT from /w/aot)\n"]
        b, seen = self.make(lines)
        arts, rec = b.run()
        self.assertEqual([a.kind for a in arts], ["3dsx"])
        self.assertEqual(arts[0].sha256, hashlib.sha256(b"3DSX" * 100).hexdigest())
        self.assertEqual((rec["version"], rec["build_id"]), ("0.1.0", "archive-0.1.0+aot12"))
        self.assertIn('data=' + str(Path(self.work) / 'data/Undertale').replace('\\', '/'), seen['argv'])
        self.assertEqual((Path(self.work) / 'data/Undertale/data.win').read_bytes(), UT)
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
                self.assertNotIn('argv', list(seen))
                self.assertFalse(os.path.isdir(os.path.join(self.work, 'src')))
                write(os.path.join(self.src, 'engine/src/vm.c'), 'int x;\n')

    def test_qualified_compiler_builds_without_exposing_captured_environment(self):
        for platform in ('win32', 'linux', 'darwin'):
            with self.subTest(platform=platform):
                self.tc.platform = platform
                b, seen = self.make(['[100%] y\n'])
                with mock.patch.object(toolchain, 'compiler_details', return_value={
                        'path': '/fixture/gcc', 'version': '16.1.0', 'target': 'arm-none-eabi',
                        'vendor': 'devkitARM', 'package': '16.1.0-1'}):
                    arts, record = b.run()
                    self.assertIn('argv', list(seen))
                    self.assertTrue(record['compiler']['layout_pins'])
                    self.assertEqual([a.kind for a in arts], ['3dsx'])

    def test_other_compilers_stop_before_any_build_work_on_each_host(self):
        for platform in ('win32', 'linux', 'darwin'):
            for field, value in (('version', '17.1.0'), ('version', 'unknown'), ('target', 'other'),
                                 ('vendor', 'unknown'), ('package', '16.1.0-2'), ('package', 'unknown')):
                with self.subTest(platform=platform, field=field, value=value):
                    self.tc.platform = platform
                    compiler = dict(path='/fixture/gcc', version='16.1.0', target='arm-none-eabi',
                                    vendor='devkitARM', package='16.1.0-1')
                    compiler[field] = value
                    b, seen = self.make([])
                    with mock.patch.object(toolchain, 'compiler_details', return_value=compiler), \
                            mock.patch.object(b, 'prepare') as prepare, \
                            mock.patch.object(products, 'aot_data_dir') as stage, \
                            self.assertRaises(pipeline.BuildFailed) as caught:
                        b.run()
                    prepare.assert_not_called()
                    stage.assert_not_called()
                    self.assertNotIn('argv', list(seen))
                    self.assertFalse(os.path.exists(os.path.join(self.work, 'src')))
                    self.assertIsNone(caught.exception.log_path)
                    for text in ('r68', '16.1.0-1', 'build has not started'):
                        self.assertIn(text, str(caught.exception))
                    self.assertNotIn('unknown', str(caught.exception))
                    self.assertNotIn('pacman -Syu', str(caught.exception))
                    if value != 'unknown':
                        self.assertIn('https://github.com/ethancawse/undertale-3ds/releases', str(caught.exception))
                    else:
                        self.assertIn("Couldn't check", str(caught.exception))

    def test_no_aot_is_refused(self):
        b, _ = self.make(["WARNING: building WITHOUT AOT: no UNDERTALE game data found\n", "[100%] y\n"])
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("The game code could not be prepared.", str(e.exception))
        self.assertNotIsInstance(e.exception, pipeline.GameFilesRefused)
        self.assertEqual(str(e.exception), 'The game code could not be prepared.\n'
                         'Click Try again, or click Copy log to report the error.')
        self.assertTrue(b.cancelled)

    def test_missing_input_message_precedes_the_no_aot_fallback(self):
        b, _ = self.make(['Could not find the game files.\n',
                          'WARNING: building WITHOUT AOT: no UNDERTALE game data found\n'], rc=3)
        with self.assertRaises(pipeline.GameFilesRefused) as caught:
            b.run()
        text = str(caught.exception)
        self.assertIn('Could not find the UNDERTALE game files.', text)
        self.assertIn('Go back to choose the game files.', text)
        self.assertNotIn('code could not be prepared', text)
        self.assertNotIn('repaired builder', text)

    def test_prepare_failure_reports_a_builder_step_failure_and_keeps_retry(self):
        import gui
        diagnostic = 'AOT: tools/aot/prepare.py failed (exit 1); aot=off builds without AOT'
        for host in ('win32', 'linux', 'darwin'):
            with self.subTest(host=host):
                self.tc.platform = host
                b, _ = self.make([diagnostic + '\n'], rc=1)
                with self.assertRaises(pipeline.BuildFailed) as caught:
                    b.run()
                self.assertNotIsInstance(caught.exception, pipeline.GameFilesRefused)
                self.assertEqual(str(caught.exception), 'The game code could not be prepared.\n'
                                 'Click Try again, or click Copy log to report the error.')
                self.assertIn(diagnostic, Path(b.log_path).read_text(encoding='utf-8'))
                page = gui.BuildPage.__new__(gui.BuildPage)
                page.failed, page.running, page.log_path = caught.exception, False, b.log_path
                page.cancel_btn, page.retry_btn, page.copy_btn = mock.Mock(), mock.Mock(), mock.Mock()
                page.buttons()
                page.retry_btn.config.assert_called_with(text='Try again', command=page.start)
                self.assertIn('report the error with the log named below', app.cli_text(str(caught.exception)))

    def test_aot_function_skip_is_not_refused(self):
        # DELTARUNE chapter 5: gmlaot interprets two functions it cannot analyse; the build has AOT (integrate21 stage73).
        skip = "AOT:   skip gml_Script_x: entry missing or instruction analysis failed before generation\n"
        b, _ = self.make([skip, "[100%] y\n"])
        try:
            b.run()
        except pipeline.BuildFailed as e:
            self.assertNotIn("The game code could not be prepared.", str(e))
        self.assertFalse(b.progress.no_aot)
        self.assertFalse(b.cancelled)

    def test_finished_without_aot_config_is_refused(self):
        b, _ = self.make(["[100%] y\n"], aot=False)
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("missing prepared game code", str(e.exception))

    def test_failed_build(self):
        b, _ = self.make(["error: something\n"], rc=2)
        with self.assertRaises(pipeline.BuildFailed) as e:
            b.run()
        self.assertIn("The build stopped", str(e.exception))
        self.assertIn("exit code 2", str(e.exception))
        self.assertTrue(os.path.isfile(e.exception.log_path))

    def test_missing_cia(self):
        self.tc.makerom, self.tc.bannertool = "/fixture/makerom", "/fixture/bannertool"
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

    def test_undertale_space_gate_matches_options_allowance_before_mirroring_on_each_model(self):
        import gui
        for model in ('o3ds', 'n3ds'):
            # A second calibration proves both consumers read the shared setting.
            for allowance in (1000, 1234):
                for free in (350000000, 640000000, allowance * 10**6 - 1, allowance * 10**6):
                    with self.subTest(model=model, allowance=allowance, free=free):
                        b, seen = self.make([])
                        b.target = model
                        page = gui.OptionsPage.__new__(gui.OptionsPage)
                        page.app = mock.Mock(state=gui.State('undertale'))
                        page.app.opts.work = self.work
                        page.target = mock.Mock(get=lambda: model)
                        page.v3dsx = mock.Mock(get=lambda: True)
                        page.vcia = mock.Mock(get=lambda: False)
                        page.part_vars = {}
                        page.estimate = mock.Mock()
                        with mock.patch.object(products, 'UNDERTALE_WORK_MB', allowance), \
                                mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=free)), \
                                mock.patch.object(pipeline, 'mirror_source', return_value=(0, 0)) as mirror, \
                                mock.patch.object(pipeline, 'write_python_shims') as shims:
                            page.changed()
                            self.assertIn('Allow about %d MB' % allowance, page.estimate.config.call_args.kwargs['text'])
                            self.assertIn('Delivery copies need extra space', page.estimate.config.call_args.kwargs['text'])
                            if free < allowance * 10**6:
                                with self.assertRaises(pipeline.BuildFailed) as caught:
                                    b.prepare()
                                self.assertEqual(str(caught.exception),
                                    '%s has %d MB free; %d MB needed.\nFree up disk space and try again.' %
                                    (self.work, free // 10**6, allowance))
                                mirror.assert_not_called()
                                shims.assert_not_called()
                            else:
                                b.prepare()
                                mirror.assert_called_once()
                                shims.assert_called_once()
                        self.assertNotIn('argv', seen)

    def test_undertale_allowance_stays_above_measured_work(self):
        self.assertGreaterEqual(products.UNDERTALE_WORK_MB * 10**6, 637_000_000)

    def test_two_prepares_credit_the_first_models_existing_work(self):
        # Real reusable files, two distinct Build objects, shrinking free space.
        allowance = products.UNDERTALE_WORK_MB * 10**6
        for first, second in (('o3ds', 'n3ds'), ('n3ds', 'o3ds')):
            with self.subTest(first=first):
                work = Path(self.work) / first
                a, seen_a = self.make([])
                a.work, a.target = str(work), first
                with mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=allowance)):
                    a.prepare()
                write(str(work / 'src/build/first-model.bin'), 'x' * (2 * 1024**2))
                write(str(work / 'out' / first / 'artifact.3dsx'), 'y' * (2 * 1024**2))
                credit = pipeline.existing_work_bytes(work)
                self.assertGreaterEqual(credit, 4 * 1024**2)
                self.assertLess(credit, allowance - 30 * 10**6)
                b, seen_b = self.make([])
                b.work, b.target = str(work), second
                with mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=allowance-credit)):
                    b.prepare()
                self.assertNotIn('argv', seen_a)
                self.assertNotIn('argv', seen_b)

    def test_rerun_credits_reusable_work_but_still_refuses_a_remaining_shortfall(self):
        allowance = products.UNDERTALE_WORK_MB * 10**6
        write(os.path.join(self.work, 'src/build/retained.bin'), 'x' * 1024**2)
        write(os.path.join(self.work, 'ccache/retained.bin'), 'y' * 1024**2)
        credit = pipeline.existing_work_bytes(self.work)
        self.assertEqual(credit, 2 * 1024**2)
        for model in ('o3ds', 'n3ds'):
            for short in (True, False):
                with self.subTest(model=model, short=short):
                    b, seen = self.make([])
                    b.target = model
                    free = allowance - credit - int(short)
                    with mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=free)), \
                            mock.patch.object(pipeline, 'mirror_source', return_value=(0, 0)) as mirror:
                        if short:
                            with self.assertRaises(pipeline.BuildFailed) as caught:
                                b.prepare()
                            self.assertIn('%d MB needed' % (-(-(allowance-credit) // 10**6)), str(caught.exception))
                            mirror.assert_not_called()
                        else:
                            b.prepare()
                            mirror.assert_called_once()
                    self.assertNotIn('argv', seen)

    def test_work_credit_excludes_delivery_unrelated_files_and_links_and_keeps_headroom(self):
        write(os.path.join(self.work, 'src/build/kept.bin'), 'a' * 4096)
        write(os.path.join(self.work, 'out/files/not-work.bin'), 'b' * 8192)
        write(os.path.join(self.work, 'unrelated.bin'), 'c' * 8192)
        self.assertEqual(pipeline.existing_work_bytes(self.work), 4096)
        try:
            os.symlink(self.src, os.path.join(self.work, 'ccache'), target_is_directory=True)
        except OSError:
            pass  # Windows may disallow fixture symlinks; Debian exercises this branch.
        else:
            self.assertEqual(pipeline.existing_work_bytes(self.work), 4096)
        b, _ = self.make([])
        with mock.patch.object(pipeline, 'existing_work_bytes', return_value=2 * 10**9), \
                mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=30 * 10**6 - 1)), \
                mock.patch.object(pipeline, 'mirror_source') as mirror:
            with self.assertRaisesRegex(pipeline.BuildFailed, '30 MB needed'):
                b.prepare()
            mirror.assert_not_called()

    def test_oversized_stale_cache_credit_is_capped_and_cannot_bypass_the_space_gate(self):
        # Scale 1000 MB total / 100 MB cap to 100 MB total / 10 MB cap.
        # Real cache bytes, with no source/build credit: 90 MB stale cache and 31 MB free
        # used to pass at the 30 MB minimum. No compiler or network is involved.
        self.assertEqual(pipeline.WORK_CACHE_CREDIT_MB, 100)
        cache = Path(self.work) / 'ccache/stale-entry'
        cache.parent.mkdir(parents=True)
        with cache.open('wb') as stream:
            for _ in range(90):
                stream.write(b'x' * 10**6)
        for model in ('o3ds', 'n3ds'):
            for free in (31 * 10**6, 90 * 10**6 - 1, 90 * 10**6):
                with self.subTest(model=model, free=free), \
                        mock.patch.object(products, 'UNDERTALE_WORK_MB', 100), \
                        mock.patch.object(pipeline, 'WORK_CACHE_CREDIT_MB', 10), \
                        mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=free)), \
                        mock.patch.object(pipeline, 'mirror_source', return_value=(0, 0)) as mirror, \
                        mock.patch.object(pipeline, 'write_python_shims') as shims:
                    self.assertEqual(pipeline.existing_work_bytes(self.work), 10 * 10**6)
                    b, seen = self.make([])
                    b.target = model
                    if free < 90 * 10**6:
                        with self.assertRaises(pipeline.BuildFailed) as caught:
                            b.prepare()
                        self.assertEqual(str(caught.exception),
                            '%s has %d MB free; 90 MB needed.\nFree up disk space and try again.' %
                            (self.work, free // 10**6))
                        mirror.assert_not_called()
                        shims.assert_not_called()
                    else:
                        b.prepare()
                        mirror.assert_called_once()
                    self.assertNotIn('argv', seen)

    def test_work_credit_deduplicates_real_hardlinks(self):
        src = Path(self.work) / 'src/kept.bin'
        write(str(src), 'x' * 4096)
        os.link(src, src.with_name('same.bin'))
        os.link(src, src.with_name('same-again.bin'))
        self.assertEqual(pipeline.existing_work_bytes(self.work), 4096)

    def test_work_credit_rejects_junction_sparse_compressed_and_excess_apparent_bytes(self):
        from types import SimpleNamespace
        work = Path(self.work)
        folder = work / 'src/junction'
        file = folder / 'kept.bin'
        write(str(file), 'x' * 4096)
        original_lstat = Path.lstat
        cases = (
            ('junction', folder, pipeline.stat.FILE_ATTRIBUTE_REPARSE_POINT, None, 0),
            ('sparse', file, pipeline.stat.FILE_ATTRIBUTE_SPARSE_FILE, None, 0),
            ('compressed', file, pipeline.stat.FILE_ATTRIBUTE_COMPRESSED, None, 0),
            ('allocation', file, 0, 2, 1024),
        )
        for name, changed, attributes, blocks, expected in cases:
            with self.subTest(case=name):
                def lstat(path):
                    info = original_lstat(path)
                    if path != changed:
                        return info
                    fields = {key: getattr(info, key) for key in ('st_mode', 'st_dev', 'st_ino', 'st_size')}
                    fields['st_file_attributes'] = attributes
                    if blocks is not None:
                        fields['st_blocks'] = blocks
                    return SimpleNamespace(**fields)

                # Synthetic Windows attributes are also exercised on Debian; real paths
                # and a real directory walk ensure a removed guard changes the credit.
                with mock.patch.object(Path, 'lstat', lstat):
                    self.assertEqual(pipeline.existing_work_bytes(work), expected)

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
        self.assertNotIn("argv", list(seen))
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
            self.assertEqual(pipeline.source_wanted("tools/aot/dskip.py", game), game != "deltarune")
            self.assertFalse(pipeline.source_wanted("tools/aot/bottomframe.py", game))
            self.assertTrue(pipeline.source_wanted("tools/aot/stglyphs.py", game))
            self.assertTrue(pipeline.source_wanted("tools/aot/stpack.py", game))
            self.assertEqual(pipeline.source_wanted("tools/aot/stmenu.py", game), game != "deltarune")
            # fontverdict.py imports both for DELTARUNE too (symverdict.py on its New 3DS build).
            self.assertTrue(pipeline.source_wanted("tools/aot/symverdict.py", game))
            self.assertTrue(pipeline.source_wanted("tools/aot/itemverdict.py", game))
            self.assertTrue(pipeline.source_wanted("tools/aot/inkart.py", game))
        manifest = Path(pipeline.SOURCE_ROOT, "tools/release/manifest-ut.txt")
        if not manifest.is_file():
            self.skipTest("Curated release manifests are not included in this builder source kit")
        manifest = manifest.read_text(encoding="utf-8")
        for name in ("tools/aot/pixgrid.py", "tools/aot/loadscreen_art.py", "tools/aot/btnprompt_build.py",
                     "tools/aot/dskip.py", "tools/aot/stglyphs.py", "tools/aot/stpack.py", "tools/aot/stmenu.py"):
            self.assertIn(name, manifest.splitlines())

    def test_work_dir(self):
        self.assertEqual(pipeline.default_work_dir({"LOCALAPPDATA": ('C:\\' + 'Users\\Jo Smith\\AppData\\Local')}, "win32"),
                         "C:/u3ds")
        self.assertEqual(pipeline.default_work_dir({"LOCALAPPDATA": ('C:\\' + 'Users\\jo\\AppData\\Local')}, "win32"),
                         os.path.join(('C:\\' + 'Users\\jo\\AppData\\Local'), "u3ds", "w"))

    def test_build_argv_shape(self):
        argv = pipeline.build_argv(self.tc, "C:/w", "undertale", "o3ds", "D:\\G", platform="win32")
        self.assertEqual(argv[:3], [self.tc.bash, "--login", "-c"])
        self.assertEqual(argv[-5:], ["undertale", "o3ds", "aot", "data=D:/G", "openh264=off"])
        argv = pipeline.build_argv(self.tc, "C:/w", "deltarune", "n3ds", "D:\\G", openh264=True, platform="win32")
        self.assertNotIn("openh264=off", argv)

    def test_build_argv_halfart(self):
        # The release setting is off: no build gets half-resolution art.
        self.assertFalse(products.HALFART)
        for game in ("undertale", "deltarune"):
            for target in ("o3ds", "n3ds"):
                self.assertNotIn("halfart", pipeline.build_argv(self.tc, "C:/w", game, target, "D:\\G", platform="win32"))
        # On, only UNDERTALE's Old 3DS build gets it.
        argv = pipeline.build_argv(self.tc, "C:/w", "undertale", "o3ds", "D:\\G", platform="win32", halfart=True)
        self.assertEqual(argv[-6:], ["undertale", "o3ds", "aot", "data=D:/G", "openh264=off", "halfart"])
        for game, target in (("undertale", "n3ds"), ("deltarune", "o3ds"), ("deltarune", "n3ds")):
            self.assertNotIn("halfart", pipeline.build_argv(self.tc, "C:/w", game, target, "D:\\G", platform="win32",
                                                            halfart=True))
        with mock.patch.object(products, "HALFART", True):
            self.assertEqual(pipeline.build_argv(self.tc, "C:/w", "undertale", "o3ds", "D:\\G", platform="win32")[-1],
                             "halfart")
        # Its packer and hash list go to UNDERTALE's builds only.
        for rel in ("tools/analysis/halfart_pack.py", "tools/analysis/halfart/undertale.txt", "platform/3ds/source/ctr_halfart.c"):
            self.assertTrue(pipeline.source_wanted(rel, "undertale"))
            self.assertFalse(pipeline.source_wanted(rel, "deltarune"))
        self.assertFalse(pipeline.source_wanted("tools/analysis/halfart/undertale-balanced.txt", "undertale"))
        self.assertFalse(pipeline.source_wanted("tools/analysis/halfart_balance.py", "undertale"))


# ---------------------------------------------------------------------------------------------------------------------

class Art:
    def __init__(self, path, kind):
        self.path, self.kind, self.size, self.sha256 = path, kind, os.path.getsize(path), "0" * 64


class DeliverTest(unittest.TestCase):
    def test_cia_instruction_paths_use_one_separator_on_every_host(self):
        for root, expected in [('E:\\', 'E:/cia'), ('E:\\cards\\SD', 'E:/cards/SD/cia'),
                               ('/media/card/', '/media/card/cia'), (None, 'sd:/cia')]:
            with self.subTest(root=root):
                text = deliver.instructions(products.PRODUCTS['undertale'], ['n3ds'], ['3dsx', 'cia'], root)
                self.assertIn('from ' + expected + ':', text)
                self.assertIn('open FBI, the 3DS installer', text)
                self.assertNotIn('\\', text)

    def setUp(self):
        manifest = mock.patch.object(products, 'read_music_manifest', return_value=fixture_music_manifest())
        manifest.start()
        self.addCleanup(manifest.stop)
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

    def test_netloader_send_is_disabled_even_for_legacy_callers(self):
        ran = []
        with self.assertRaises(deliver.DeliverError) as caught:
            deliver.send_3dslink('3dslink', 'a.3dsx', run=lambda argv: ran.append(argv) or 0)
        self.assertEqual(str(caught.exception), "Sending with the Homebrew Launcher's netloader is no longer supported. Use Send files over Wi-Fi with ftpd.")
        self.assertEqual(ran, [])

    def test_legacy_ftp_entry_uses_verified_transport(self):
        import ftp_delivery
        items = [deliver.CopyItem('invented', '3ds/game/program.3dsx', 8)]
        with mock.patch.object(ftp_delivery, 'send') as send:
            deliver.send_ftp('127.0.0.1', items, port=5000, log=lambda s: None)
        send.assert_called_once_with('127.0.0.1:5000', items)


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
            ("file2", "Frisk\nnot_a_number\n0\n0\n", "Save format not recognized"),
            ("file3", "Frisk\n0\n0\n0\n", "Save format not recognized"),
            ("file4", "Frisk\n999\n0\n0\n", "Save format not recognized"),
            ("file5", "Frisk\n1\nnot_a_number\n", "invalid save data"),
            ("file6", b"Frisk\x00\n1\n0\n", "Save format not recognized"),
            ("file7", b"\xff\xfe\xfd\x00", "Save format not recognized"),
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
            self.assertIn("could not write", str(e_posix.exception))
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
            self.assertIn("could not write", str(e_file.exception))
            self.assertIn("Lock switch", str(e_file.exception))
        finally:
            os.chmod(sd_file0, stat.S_IWRITE | stat.S_IREAD)
            os.remove(sd_file0)

        # 3. Write failure (PermissionError / EROFS) during probe or copy_items produces the same clear message.
        items = deliver.plan_sd_copy(self.sd, self.ut, arts, game)
        with mock.patch.object(deliver.shutil, "copy2", side_effect=PermissionError("Write protected")):
            with self.assertRaises(deliver.DeliverError) as e3:
                deliver.copy_items(self.sd, items)
            self.assertIn("could not write", str(e3.exception))
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
        real_write = saves._write_checked  # Stages the checked bytes (DEEP-06).

        def flaky(dst, data, mtime):
            if os.path.basename(dst) == "undertale.ini.part":
                raise OSError(5, "I/O error")
            real_write(dst, data, mtime)

        with mock.patch.object(saves, "_write_checked", side_effect=flaky):
            with self.assertRaises(deliver.DeliverError):
                deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=True)
        top_files = sorted(n for n in os.listdir(sd_saves) if os.path.isfile(os.path.join(sd_saves, n)))
        self.assertEqual(top_files, ["file0", "file8"])
        self.assertEqual(folder_snapshot(sd_saves), before)

        # A failure after files were replaced names the backup folder.
        with mock.patch.object(saves.os, "remove", side_effect=PermissionError(13, "denied")):
            with self.assertRaises(deliver.DeliverError) as e:
                deliver.copy_pc_saves(self.sd, self.ut, scan, overwrite=True)
        self.assertIn("previous 3DS saves are backed up in", str(e.exception))
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
    def setUp(self):
        super().setUp()
        # The CLI checks its own source first (B1). These tests are about later steps, so they never depend on
        # whether the checkout running them has Git history or a release descriptor.
        source = mock.patch.object(app.pipeline, 'release_source', return_value={
            'kind': 'release', 'public_commit': 'a' * 40,
            'source_url': 'https://github.com/ethancawse/undertale-3ds/tree/' + 'a' * 40})
        source.start()
        self.addCleanup(source.stop)

    def test_cli_missing_cia_tools_delivers_3dsx_and_only_its_instructions(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = SteamTree(tmp)
            sd = os.path.join(tmp, 'sd')
            os.makedirs(os.path.join(sd, '3ds'))
            tc = toolchain.Toolchain('/fixture', '/fixture/bash', [], [], [], None, None, None)
            artifact = os.path.join(tmp, 'undertale-3ds-n3ds.3dsx')
            write(artifact, b'3dsx')
            original_verify = discover.verify_copy
            class FakeBuild:
                def __init__(self, *args, **kwargs):
                    self.log_path = os.path.join(tmp, 'build.log')
                def run(self):
                    return [Art(artifact, '3dsx')], dict(target='n3ds', seconds=1,
                        compiler_warnings=0, notices=[pipeline.CIA_NOT_MADE])
            out = []
            with mock.patch.object(app, 'probes_for', return_value=tree.probes()), \
                    mock.patch.object(app.discover, 'verify_copy', side_effect=lambda g, f, p:
                                      original_verify(g, f, p, allow_list=ALLOW)), \
                    mock.patch.object(app.toolchain, 'detect', return_value=tc), \
                    mock.patch.object(app.pipeline, 'Build', FakeBuild):
                rc = app.cli(app.parse(['--cli', '--game', 'undertale', '--target', 'n3ds', '--cia', '--no-texprep',
                    '--work', os.path.join(tmp, 'work'), '--sd', sd]), out=out.append)
            self.assertEqual(rc, 0)
            self.assertTrue(Path(sd, '3ds/undertale-3ds/undertale-3ds-n3ds.3dsx').is_file())
            # The command line says it in its own words (B2): no window buttons, and only once.
            self.assertEqual(out.count(app.cli_text(pipeline.CIA_NOT_MADE)), 1)
            self.assertNotIn(pipeline.CIA_NOT_MADE, out)
            self.assertIn('--dry-run', app.cli_text(pipeline.CIA_NOT_MADE))
            self.assertNotIn('builder window', '\n'.join(out))
            self.assertNotIn('Click', app.cli_text(pipeline.CIA_NOT_MADE))
            self.assertIn('Homebrew Launcher', '\n'.join(out))
            self.assertNotIn('with FBI', '\n'.join(out))

    def test_cli_texture_fallback_keeps_diagnostics_in_log_and_delivers_3dsx(self):
        import ftp_delivery
        diagnostic = 'texprep: shipped texprep missing (texprep-windows-x64.exe)'
        for delivery in ('sd', 'files', 'wifi'):
            with self.subTest(delivery=delivery), tempfile.TemporaryDirectory() as tmp:
                tree = SteamTree(tmp)
                sd = os.path.join(tmp, 'sd')
                os.makedirs(os.path.join(sd, '3ds'))
                work = os.path.join(tmp, 'work')
                artifact = os.path.join(tmp, 'undertale-3ds-n3ds.3dsx')
                write(artifact, b'3dsx')
                log_path = os.path.join(tmp, 'build.log')
                tc = toolchain.Toolchain('/fixture', '/fixture/bash', [], [], [], None, None, None)
                original_verify = discover.verify_copy

                class FakeBuild:
                    def __init__(self, *args, **kwargs):
                        self.log_path = log_path
                    def run(self):
                        write(log_path, 'Build finished.\n')
                        return [Art(artifact, '3dsx')], dict(target='n3ds', seconds=1, compiler_warnings=0)

                # Exercise the real local layout preparation without opening an FTP connection.
                def send(_address, product, arts, folder, work, parts, *, log):
                    saved = deliver.save_files(product, arts, folder, work, parts, log=log)
                    log('Files checked.')
                    return saved

                args = ['--cli', '--game', 'undertale', '--target', 'n3ds', '--work', work]
                if delivery == 'sd':
                    args += ['--sd', sd]
                elif delivery == 'wifi':
                    args += ['--ftp', '127.0.0.1:5000']
                out = []
                with mock.patch.object(app, 'probes_for', return_value=tree.probes()), \
                        mock.patch.object(app.discover, 'verify_copy', side_effect=lambda g, f, p:
                                          original_verify(g, f, p, allow_list=ALLOW)), \
                        mock.patch.object(app.toolchain, 'detect', return_value=tc), \
                        mock.patch.object(app.pipeline, 'Build', FakeBuild), \
                        mock.patch.object(texcache, 'find_tool', return_value=texcache.Tool(
                            None, '', diagnostic.removeprefix('texprep: '))), \
                        mock.patch.object(ftp_delivery, 'send_product', side_effect=send), \
                        mock.patch.object(ftp_delivery, 'connect', side_effect=AssertionError('No network')):
                    self.assertEqual(app.cli(app.parse(args), out=out.append), 0)
                text = '\n'.join(out)
                self.assertNotIn('texprep', text)
                self.assertIn('Build finished.\n', Path(log_path).read_text())
                self.assertIn(diagnostic, Path(log_path).read_text())
                self.assertIn(texcache.FIRST_START, text)
                if delivery == 'wifi':
                    self.assertIn('Files checked.', out)
                destination = Path(sd if delivery == 'sd' else work)
                self.assertTrue(list(destination.rglob('undertale-3ds-n3ds.3dsx')))

    def test_cli_cia_setup_hint_is_a_real_flag_and_prints_headless_setup_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = SteamTree(tmp)
            work = os.path.join(tmp, 'work')
            tc = toolchain.Toolchain('/opt/devkitpro', '/bin/bash', [], [], [], None, None, None, 'linux')
            out = []
            with mock.patch.object(app, 'probes_for', return_value=tree.probes()), \
                    mock.patch.object(app.toolchain, 'detect', return_value=tc):
                self.assertEqual(app.dry_run(app.parse(['--dry-run', '--work', work]), out=out.append), 0)
            text = '\n'.join(out)
            self.assertIn('makerom', text)
            self.assertIn('build-bannertool', text)
            self.assertIn('SHA-256', text)
            self.assertIn('Dry run: nothing was downloaded, installed or changed.', text)
            self.assertFalse(os.path.exists(work))

    def test_cli_cia_advice_also_describes_unsupported_cpu_dry_run_truthfully(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = SteamTree(tmp)
            work = os.path.join(tmp, 'work')
            tc = toolchain.Toolchain('/opt/devkitpro', '/bin/bash', [], [], [], None, None, None, 'linux')
            out = []
            with mock.patch.object(app, 'probes_for', return_value=tree.probes()), \
                    mock.patch.object(app.toolchain, 'detect', return_value=tc), \
                    mock.patch.object(toolchain.assets, 'machine', return_value='aarch64'):
                self.assertEqual(app.dry_run(app.parse(['--dry-run', '--work', work]), out=out.append), 0)
            advice = app.cli_text(pipeline.CIA_NOT_MADE)
            self.assertIn("builder's setup steps and .cia availability", advice)
            self.assertNotIn('commands', advice)
            self.assertIn('.cia download unavailable on this CPU', '\n'.join(out))
            self.assertFalse(os.path.exists(work))

    def test_build_initialization_failure_is_a_plain_cli_refusal(self):
        verified = mock.Mock(accepted=True, message='fixture verified', result=mock.Mock(store='gog'))
        tc = mock.Mock(can_build=True, checks=[])
        out = []
        with mock.patch.object(app.discover, 'verify_copy', return_value=verified), \
                mock.patch.object(app.toolchain, 'detect', return_value=tc), \
                mock.patch.object(app.pipeline, 'claim_work') as claim, \
                mock.patch.object(app.pipeline, 'Build', side_effect=pipeline.BuildFailed('fixture work folder')):
            rc = app.cli(app.parse(['--cli', '--folder', '.', '--work', './work folder']), out=out.append)
        self.assertEqual(rc, 1)
        self.assertIn('fixture work folder', out)
        self.assertNotIn('Log: None', out)
        claim.return_value.release.assert_called_once()

    def test_work_folder_spaces_reach_each_platform_as_one_argument(self):
        with tempfile.TemporaryDirectory() as tmp:
            for platform in ('win32', 'linux', 'darwin'):
                work = os.path.join(tmp, platform+' work folder')
                build = pipeline.Build(products.PRODUCTS['undertale'], None, work, 'n3ds', './game folder')
                self.assertEqual(build.work, work)
                self.assertTrue(os.path.isdir(os.path.join(work, 'logs')))
                tc = mock.Mock(bash='/tool path/bash', root='/devkitPro path')
                argv = pipeline.build_argv(tc, work, 'undertale', 'n3ds', './game folder', platform=platform)
                self.assertIn(os.path.join(work, 'src'), argv)
                self.assertIn(os.path.join(work, 'bin'), argv)
                self.assertIn('data=./game folder', argv)

    def test_parse(self):
        # Default target is "both" (New 3DS and Old 3DS).
        d = app.parse([])
        self.assertEqual(d["game"], app.FIXED_GAME)
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
                app.dry_run(dict(app.parse(["--dry-run", "--game", "undertale", "--work", work])), out=out.append)
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
                rc1 = app.cli(dict(app.parse(["--cli", "--game", "undertale", "--target", "n3ds", "--work", os.path.join(tmp, "w"),
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
                rc2 = app.cli(dict(app.parse(["--cli", "--game", "undertale", "--target", "n3ds", "--work", os.path.join(tmp, "w"),
                                              "--sd", sd, "--import-saves", "--overwrite-saves"])), out=out2.append)
            self.assertEqual(rc2, 0)
            joined = "\n".join(out2)
            self.assertIn("Imported 1 PC save file(s)", joined)
            self.assertIn("Backed up existing 3DS saves", joined)
            self.assertIn("What to do now:", joined)


class GuiTest(unittest.TestCase):
    def test_unexpected_build_failure_preserves_traceback_without_stderr(self):
        tree = SteamTree(self.tmp)
        wiz = self.gui.App(self.root, self.gui.Options(game='undertale', work=os.path.join(self.tmp, 'work'),
                                                      probes=tree.probes(), boring=True))
        wiz.call = lambda fn, *args: fn(*args)
        wiz.state.targets = ['n3ds']
        wiz.state.verdict = mock.Mock(result=mock.Mock(store='steam'))
        page = wiz.pages[5]
        for stage in ('run', 'constructor'):
            with self.subTest(stage=stage):
                page.log_path = None
                page.log.delete('1.0', 'end')
                log = Path(self.tmp) / 'work/logs/build-fixture.log'
                write(str(log), 'Earlier build output\n')
                build = mock.Mock(log_path=str(log))
                build.run.side_effect = RuntimeError('synthetic build failure')
                with mock.patch.object(sys, 'stderr', None), \
                        mock.patch.object(pipeline, 'Build', return_value=build,
                            side_effect=RuntimeError('synthetic build failure') if stage == 'constructor' else None):
                    page.worker()
                self.assertIn('The build stopped unexpectedly. Choose Try again.', page.status.cget('text'))
                self.assertNotIn('RuntimeError', page.status.cget('text'))
                self.assertNotIn('synthetic build failure', page.status.cget('text'))
                self.assertTrue(page.copy_btn.instate(['!disabled']))
                diagnostic = Path(page.log_path).read_text(encoding='utf-8')
                self.assertIn('Traceback (most recent call last):', diagnostic)
                self.assertRegex(diagnostic, r'File "gui\.py", line \d+, in worker')
                self.assertNotIn(str(Path(self.gui.__file__).parent), diagnostic)
                self.assertIn('RuntimeError: synthetic build failure', diagnostic)
                self.assertIn('RuntimeError: synthetic build failure', page.log.get('1.0', 'end'))
                if stage == 'run':
                    self.assertEqual(page.log_path, str(log))
                    self.assertIn('Earlier build output', diagnostic)
                else:
                    self.assertEqual(Path(page.log_path).name, 'builder-error.log')

    def test_successful_3dsx_fallback_shows_notice_without_expanding_log(self):
        tree = SteamTree(self.tmp)
        app = self.gui.App(self.root, self.gui.Options(game='undertale',
            work=os.path.join(self.tmp, 'work'), probes=tree.probes(), boring=True))
        artifact = os.path.join(self.tmp, 'fixture.3dsx')
        write(artifact, b'3dsx')
        app.state.artifacts = [Art(artifact, '3dsx')]
        app.state.outputs = ['3dsx', 'cia']
        app.state.records = [dict(target='n3ds', seconds=1, notices=[pipeline.CIA_NOT_MADE])]
        page = app.pages[5]
        page.finished(None)
        self.assertTrue(app.state.build_ok)
        self.assertEqual(app.state.outputs, ['3dsx'])
        self.assertIn(pipeline.CIA_NOT_MADE, page.status.cget('text'))
        self.assertFalse(page.details.open)

    def setUp(self):
        import gui
        self.gui = gui
        self.tmp = tempfile.mkdtemp()
        try:
            self.root = gui.tk.Tk()
            self.root.withdraw()
        except gui.tk.TclError as e:
            from gui_test_support import tk_unavailable
            tk_unavailable(e)

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
        def wait_for_saves():
            deadline = time.monotonic() + 10
            while wiz.pages[7].busy() and time.monotonic() < deadline:
                self.root.update()
                time.sleep(0.01)
            self.assertFalse(wiz.pages[7].busy())
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
        wait_for_saves()
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
            wait_for_saves()
        self.assertEqual(wiz.index, 7)
        self.assertIn("could not write", wiz.page.status.cget("text"))
        self.assertIn("Lock switch", wiz.page.status.cget("text"))

        # User confirms the overwrite prompt on writable SD card -> backs up 3DS save, copies PC save, and advances to
        # a visible result, followed by DonePage (Step 9) on the next click.
        wiz.next()
        wait_for_saves()
        self.assertEqual(wiz.index, 7)
        self.assertIn('Copied 1 save to the SD card', wiz.page.status.cget('text'))
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
        unicode_path = ('PC folder: C:\\' + 'Users\\Zoë\\Юлия\\AppData\\Local\\UNDERTALE')
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
        self.assertFalse(wiz.theme.native)  # public-domain font and original heart before game assets
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
        page.build = mock.Mock(progress=mock.Mock(activity='Building'))
        page.build.progress.completion.return_value = 43 * len(wiz.state.targets)
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
        self.assertFalse(wiz.theme.native)
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

    def test_check_headline_finishes_with_the_verdict_in_both_looks(self):
        from types import SimpleNamespace
        wiz = self.wizard()
        folder = os.path.join(self.tmp, 'empty-game')
        os.mkdir(folder)
        wiz.state.folder = folder
        page = wiz.pages[2]
        for native in (False, True):
            with self.subTest(native=native):
                wiz.theme.runtime_loaded = True
                wiz.theme.boring = native
                wiz.theme.apply()
                wiz.state.verdict = None
                callbacks = []
                with mock.patch.object(wiz, 'work', side_effect=lambda job, done: callbacks.append(done)):
                    wiz.show(2)
                    self.assertTrue(page.running)
                    self.assertEqual(page.intro.cget('text'), 'Checking game files...')
                    page.on_show()
                    self.assertEqual(page.intro.cget('text'), 'Checking game files...')
                    verdict = SimpleNamespace(accepted=True, result=SimpleNamespace(folder=folder),
                                              parts=[], message='Ready to build.', store_links=[])
                    callbacks.pop()(verdict)
                    self.assertFalse(page.running)
                    self.assertEqual(page.intro.cget('text'), 'Game files checked.')
                    page.on_show()
                    self.assertEqual(page.intro.cget('text'), 'Game files checked.')
                    self.assertFalse(callbacks, 'A completed accepted check must not restart.')
                # An ordinary missing-file refusal goes through the real verifier and completion callback.
                wiz.state.verdict = None
                wiz.show(2)
                self.assertFalse(wiz.state.verdict.accepted)
                self.assertFalse(page.running)
                self.assertEqual(page.intro.cget('text'), 'Game files not accepted.')

    def test_completed_build_revisit_keeps_its_finished_headline_in_both_looks(self):
        wiz = self.wizard()
        page = wiz.pages[5]
        wiz.state.records = [dict(target='o3ds', seconds=1)]
        for native in (False, True):
            with self.subTest(native=native):
                wiz.theme.runtime_loaded = True
                wiz.theme.boring = native
                wiz.theme.apply()
                page.finished(None)
                self.assertEqual(page.intro.cget('text'), 'Build complete.')
                with mock.patch.object(page, 'start') as start:
                    wiz.show(5)
                    self.assertEqual(page.intro.cget('text'), 'Build complete.')
                    self.assertEqual(float(page.bar.cget('value')), 100)
                    self.assertTrue(page.cancel_btn.instate(['disabled']))
                    self.assertTrue(page.can_next())
                    start.assert_not_called()
                wiz.state.build_ok = False
                page.running = True
                page.on_show()
                self.assertEqual(page.intro.cget('text'), 'Building...')
                for message in ('Fixture build failure.', self.gui.CANCELLED):
                    page.finished(pipeline.BuildFailed(message, None))
                    self.assertEqual(page.intro.cget('text'), 'Build stopped.')

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
        self.assertEqual([b.step.key for b in page.boxes], ["devkitpro", "packages", "makerom", "bannertool"])
        body = fake_zip("makerom-v0.18.4/makerom.exe")
        pin = toolchain.CIA_TOOLS["makerom"]
        fetched = self.fake_network({pin["url"]: body})
        with mock.patch.dict(pin, sha256=hashlib.sha256(body).hexdigest()):
            wiz.page.detect()                          # rebuild the steps with the fixture's pin
            box = self.box(page, "makerom")
            self.assertEqual(str(box.go.cget("text")), "Install")
            self.assertEqual(box.show_btn.winfo_manager(), "")   # "Technical details" only after a failure
            self.assertTrue(page.run_step(box))
        self.assertEqual(fetched, [pin["url"]])
        self.assertEqual(len(asked), 1)
        self.assertIn("MIT License", asked[0][1])
        self.assertIn("https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.18.4/makerom/LICENSE", asked[0][1])
        self.assertTrue(os.path.isfile(os.path.join(wiz.opts.tools_dir, "makerom.exe")))
        self.assertEqual(page.result.cget("text"), "Done: %s." % box.step.title)
        self.assertTrue(box.box.winfo_exists())          # redetection keeps each setup row in place
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
            self.assertIn("did not match the expected file", page.failures["makerom"])
            self.assertIn("Technical details", page.result.cget("text"))
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
        self.assertEqual(box.status.cget("text"), "Not installed")
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
        wiz.state.game = 'deltarune'
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
                title = "Update devkitPro"
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
                self.assertIsNone(page.update_box)
                self.assertEqual(page.extra.winfo_children(), [])
                self.assertIn('Done:', page.result.cget('text'))

    def test_licence_line_and_links_on_the_page(self):
        wiz = self.wizard()
        self.open_toolchain_page(wiz, self.toolchain_fixture())
        page = wiz.page
        text, links = toolchain.licence_notice(sys.platform)
        self.assertEqual(page.notice.cget("text"),
                         "Install or Update means you agree to the listed licences.")
        found = []
        def walk(w):
            for child in w.winfo_children():
                if isinstance(child, self.gui.ttk.Button) and child.cget('text') == 'Licences':
                    found.append(child)
                walk(child)
        walk(page.frame)
        self.assertEqual(len(found), 1)
        found[0].invoke()
        shown = wiz.overlay        # in this window, not a separate pop-up
        self.assertEqual(shown.title, 'Licences')
        self.assertEqual(shown.message, text)
        detail = shown.list.get('1.0', 'end')
        for name, url in links:
            self.assertIn(name, detail)
            self.assertIn(url, detail)
        shown.buttons['OK'].invoke()
        self.assertIsNone(wiz.overlay)
        # Consent remains attached to every individual installation action.
        self.assertIn('By clicking OK you agree', self.box(page, 'makerom').step.confirm_text())

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
        self.assertEqual(str(box.go.cget("text")), "Technical details")
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
        # Notices and logs now start collapsed. Force overflow with fixture
        # content instead of depending on the prerequisite page's prose height.
        self.gui.label(wiz.page.frame, "\n".join(["Fixture overflow"] * 45))
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
        missing = toolchain.Toolchain(None, None, [], list(toolchain.REQUIRED_PACKAGES),
                                      list(toolchain.RECOMMENDED_PACKAGES), None, None, None, 'win32')
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
                            if index == 3:
                                # A new Windows player has neither devkitPro nor its packages. The real
                                # dependency sentences used to consume most of this table's width.
                                wiz.page.detected(missing)
                            elif isinstance(wiz.page, self.gui.FindPage):
                                wiz.page.tree.insert('', 'end', values=('folder', 'Fixture folder ' * 20,
                                                                       'Not checked yet ' * 20))
                            elif isinstance(wiz.page, self.gui.SavesPage):
                                # Synthetic display cells only; never open a player's saves.
                                wiz.page.tree.insert('', 'end', values=('file0', 'Fixture date',
                                                                       'Fixture name ' * 20, 'Unreadable'))
                            for _ in range(3):
                                self.root.update()
                                wiz.page.fit()
                            page = wiz.page
                            self.assertEqual(wiz.theme.native, boring)
                            self.assertLessEqual(page.frame.winfo_width(), page.canvas.winfo_width())
                            for widget in page.wrappers:
                                self.assertLessEqual(widget._wrap, max(120, page.wrap_width(widget)))
                            if hasattr(page, 'tree'):
                                columns = page.tree.cget('columns')
                                self.assertLessEqual(sum(page.tree.column(c, 'width') for c in columns),
                                                     page.tree.winfo_width())
                            if index == 3:
                                font = self.gui.tkfont.Font(page.tree,
                                    font=self.gui.ttk.Style(page.tree).lookup('Treeview', 'font') or
                                    'TkDefaultFont')
                                self.assertTrue(any('Waiting for ' in box.status.cget('text')
                                                    for box in page.boxes))
                                for row in page.tree.get_children():
                                    for column, value in zip(columns, page.tree.item(row, 'values')):
                                        self.assertLessEqual(font.measure(value) + 12,
                                                             page.tree.column(column, 'width'),
                                                             (column, value))
                            self.assertLessEqual(wiz.back_btn.winfo_rootx() + wiz.back_btn.winfo_width(),
                                                 wiz.next_btn.winfo_rootx())
                            self.assertLessEqual(wiz.next_btn.winfo_rootx() + wiz.next_btn.winfo_width(),
                                                 self.root.winfo_rootx() + self.root.winfo_width())


class GameFileGateTests(unittest.TestCase):
    """Real build-input emitter through the builder parser; synthetic bytes only."""

    def setUp(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('builder_gate_versions',
            Path(__file__).resolve().parents[1] / 'aot/data_versions.py')
        self.versions = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.versions)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name) / 'game'
        self.folder.mkdir()
        self.file = self.folder / 'data.win'
        self.data = b'A' * 4096
        self.file.write_bytes(self.data)
        self.cache = Path(self.temp.name) / 'cache'
        known = {('undertale', 'root', len(self.data), hashlib.sha256(self.data).hexdigest())}
        patch = mock.patch.object(self.versions, 'load_table', return_value=(1, known))
        patch.start()
        self.addCleanup(patch.stop)

    def emit(self, folder=True):
        out, err = io.StringIO(), io.StringIO()
        # Keep the real checker, but isolate its circular prepare import from other command modules.
        with mock.patch.dict(sys.modules, {'prepare': mock.Mock(find_data=mock.Mock(
                return_value=str(self.folder) if folder else None))}):
            result = self.versions.build_input('undertale', str(self.folder), str(self.cache), out, err)
        log = Path(self.temp.name) / 'build.log'
        log.write_text(err.getvalue(), encoding='utf-8')
        return result, err.getvalue(), pipeline.game_files_refusal(log, 'UNDERTALE')

    def test_every_build_rehashes_even_with_identical_size_and_mtime(self):
        result, diagnostic, notice = self.emit()
        self.assertEqual(result, (0, len(self.data)))
        self.assertIsNone(notice)
        self.assertIn('hashed', diagnostic)
        before = self.file.stat()
        with mock.patch.object(self.versions.hashlib, 'sha256', wraps=hashlib.sha256) as hashed:
            self.assertEqual(self.emit()[0], result)
            hashed.assert_called_once()
        self.file.write_bytes(b'B' * len(self.data))
        os.utime(self.file, ns=(before.st_atime_ns, before.st_mtime_ns))
        result, diagnostic, notice = self.emit()
        self.assertEqual(result, (2, None))
        self.assertIn("don't match an official release this builder knows", notice)
        self.assertIn('SHA-256 ' + hashlib.sha256(self.file.read_bytes()).hexdigest(), diagnostic)
        saved = json.loads((self.cache / 'data_versions.json').read_text(encoding='utf-8'))
        self.assertEqual(saved[str(self.file.resolve())]['sha256'], hashlib.sha256(self.file.read_bytes()).hexdigest())

    def test_read_and_changed_errors_reach_parser_without_raw_errno_or_wrong_advice(self):
        for error, expected in ((PermissionError(13, 'Permission denied', str(self.file)),
                                 'Check that the game folder is readable'),
                                (OSError('file changed while checking'),
                                 'Wait for any update or file copy to finish')):
            with self.subTest(error=str(error)), \
                    mock.patch.object(self.versions, 'check_file', side_effect=error):
                result, diagnostic, notice = self.emit()
                self.assertEqual(result, (2, None))
                self.assertIn(str(error), diagnostic)
                self.assertIn(expected, notice)
                self.assertIn('Go back to choose the game files.', notice)
                for raw in ('[Errno', str(self.file), 'Try again'):
                    self.assertNotIn(raw, notice)
                if 'changed' in str(error):
                    self.assertNotIn('readable', notice)

    def test_missing_input_has_a_real_diagnostic_and_cli_rerun_advice(self):
        result, diagnostic, notice = self.emit(folder=False)
        self.assertEqual(result, (3, None))
        self.assertEqual(diagnostic, 'Could not find the game files.\n')
        self.assertIn('Could not find the UNDERTALE game files.', notice)
        text = app.cli_text(notice)
        self.assertIn('Then run the same command again.', text)
        self.assertNotIn('Go back', text)

    def test_no_data_cli_exit_three_does_not_claim_the_build_was_refused(self):
        import subprocess
        empty = Path(self.temp.name) / 'empty'
        empty.mkdir()
        checker = Path(__file__).resolve().parents[1] / 'aot/data_versions.py'
        result = subprocess.run([sys.executable, '-X', 'utf8', '-I', '-B', str(checker),
            '--build-input', 'undertale', '--data', str(empty), '--cache', str(self.cache)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding='utf-8', timeout=15)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr, 'Could not find the game files.\n')
        self.assertFalse(self.cache.exists())

    def test_check_again_clears_failures_only_for_steps_now_installed(self):
        import gui
        for needed in (False, True):
            with self.subTest(needed=needed):
                page = gui.ToolchainPage.__new__(gui.ToolchainPage)
                page.app = mock.Mock(state=gui.State('undertale'))
                page.app.opts.toolchain_dry_run = False
                step = mock.Mock(key='packages')
                box = mock.Mock(step=step)
                page.boxes = [box]
                why = "Couldn't check the compiler. Click Try again."
                page.failures = {'packages': why}
                for name in ('tree', 'result', 'compiler_status', 'fit_status', 'update_buttons', 'write'):
                    setattr(page, name, mock.Mock())
                page.tree.get_children.return_value = []
                tc = mock.Mock(root=None, makerom=None, bannertool=None, platform='win32', compiler_info={})
                with mock.patch.object(gui.cleanup, 'record_before'), \
                        mock.patch.object(toolchain, 'compiler_status', return_value='fixture'), \
                        mock.patch.object(toolchain, 'full_plan', return_value=[step]), \
                        mock.patch.object(toolchain, 'setup_inventory', return_value=[(step, needed, None)]):
                    page.detected(tc)
                if needed:
                    box.failed.assert_called_once_with(why)
                    self.assertIn('packages', page.failures)
                    box.show_btn.pack_forget.assert_not_called()
                    box.page_btn.pack_forget.assert_not_called()
                else:
                    box.failed.assert_not_called()
                    box._say.assert_called_with('Installed')
                    box.go.config.assert_called_with(text='Install')
                    self.assertFalse(page.failures)
                    box.show_btn.pack_forget.assert_called_once()
                    box.page_btn.pack_forget.assert_called_once()
                    page.result.config.assert_called_with(text='Tools checked.')

    def test_check_again_keeps_the_failure_of_another_step_visible(self):
        import gui
        page = gui.ToolchainPage.__new__(gui.ToolchainPage)
        page.app = mock.Mock(state=gui.State('undertale'))
        page.app.opts.toolchain_dry_run = False
        first, second = mock.Mock(key='packages'), mock.Mock(key='makerom')
        box = mock.Mock(step=first, page_btn=None)
        other = mock.Mock(step=second)
        page.boxes = [box, other]
        page.failures = {'packages': 'old compiler failure', 'makerom': 'Download failed. Try again.'}
        for name in ('tree', 'result', 'compiler_status', 'fit_status', 'update_buttons', 'write'):
            setattr(page, name, mock.Mock())
        page.tree.get_children.return_value = []
        tc = mock.Mock(root=None, makerom=None, bannertool=None, platform='win32', compiler_info={})
        with mock.patch.object(gui.cleanup, 'record_before'), \
                mock.patch.object(toolchain, 'compiler_status', return_value='fixture'), \
                mock.patch.object(toolchain, 'full_plan', return_value=[first, second]), \
                mock.patch.object(toolchain, 'setup_inventory', return_value=[(first, False, None), (second, True, None)]):
            page.detected(tc)
        self.assertEqual(page.failures, {'makerom': 'Download failed. Try again.'})
        page.result.config.assert_called_with(text='Download failed. Try again.')
        box.show_btn.pack_forget.assert_called_once()
        other.failed.assert_called_once_with('Download failed. Try again.')
        other.show_btn.pack_forget.assert_not_called()


if __name__ == "__main__":
    from gui_test_support import isolated_builder_host
    with tempfile.TemporaryDirectory() as isolated, isolated_builder_host(isolated):
        unittest.main()
