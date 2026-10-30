#!/usr/bin/env python3
"""Platform fixtures only: no installs, real game data, SD cards or internet. Run with Python -I."""
import io
import ast
import errno
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import urllib.error
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "builder"))
sys.path.insert(0, str(HERE))
import discover
import deliver
import pipeline
import platform_assets
import toolchain
import package_builder
import builder_platform_probe
import test_builder as fixtures
from test_builder import SteamTree, ALLOW, write, fake_devkitpro, pacman, UT


def unix_root(tmp):
    root = Path(tmp) / "devkitpro"
    for name in toolchain.REQUIRED_FILES:
        p = root / name.replace(".exe", "")
        write(str(p), b"fixture")
        p.chmod(0o755)
    return str(root)


def tc(platform, root=None):
    return toolchain.Toolchain(root, "/bin/bash", [], [], [], None, None, None, platform)


class PlatformToolchainTests(unittest.TestCase):
    def test_download_uses_honest_builder_user_agent(self):
        body = b"pinned script"
        seen = []
        class Response(io.BytesIO):
            headers = {}
        def opener(request, timeout):
            seen.append(request)
            return Response(body)
        with tempfile.TemporaryDirectory() as tmp:
            for host in ("devkitpro.org", "apt.devkitpro.org", "pkg.devkitpro.org"):
                toolchain.download("https://%s/file" % host, str(Path(tmp) / "file"),
                                   hashlib.sha256(body).hexdigest(), opener=opener, size=len(body))
            self.assertTrue(all(r.get_header("User-agent") == "ctrport-builder/" + toolchain.builder_version()
                                for r in seen))

    def test_block_pages_and_http_errors_are_plain_refusals(self):
        body = b"<!DOCTYPE html><html>Cloudflare: access denied</html>"
        class Response(io.BytesIO):
            headers = {}
        def blocked(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {}, None)
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "installer"
            pin = platform_assets.DEBIAN_INSTALLER
            for opener, message in ((lambda request, timeout: Response(body), "devkitPro blocked"),
                                    (blocked, "devkitPro blocked")):
                with self.subTest(message=message), self.assertRaisesRegex(toolchain.Refused, message):
                    toolchain.download(pin["url"], str(dest), pin["sha256"], opener=opener, size=pin["size"])
                self.assertFalse(dest.exists())
                self.assertFalse(Path(str(dest) + ".part").exists())
            dest.write_bytes(b"previous verified file")
            Response.headers = {"Content-Type": "text/html"}
            with self.assertRaisesRegex(toolchain.Refused, "devkitPro blocked"):
                toolchain.download(pin["url"], str(dest), pin["sha256"],
                                   opener=lambda request, timeout: Response(body), size=pin["size"])
            self.assertEqual(dest.read_bytes(), b"previous verified file")
            self.assertFalse(Path(str(dest) + ".part").exists())

    def test_blocked_setup_gives_pinned_official_steps_and_allows_redetection(self):
        def blocked(request, timeout):
            self.assertTrue(request.get_header("User-agent").startswith("ctrport-builder/"))
            raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {}, None)
        with tempfile.TemporaryDirectory() as tmp:
            steps = toolchain.full_plan(tmp, tmp, platform="linux", arch="x86_64", want_cia=False)
            step = next(s for s in steps if s.key == "devkitpro")
            runner = mock.Mock(side_effect=AssertionError("blocked installer must not run"))
            actions = toolchain.Actions(download=lambda *a, **kw: toolchain.download(*a, opener=blocked, **kw),
                                        run=runner)
            with self.assertRaises(toolchain.Refused) as error:
                toolchain.run_step(step, lambda _: None, False, actions)
            message = str(error.exception)
            for required in ("wget " + platform_assets.DEBIAN_INSTALLER["url"],
                             platform_assets.DEBIAN_INSTALLER["sha256"], "sha256sum -c - &&",
                             "chmod +x ./install-devkitpro-pacman", "sudo ./install-devkitpro-pacman",
                             "sudo dkp-pacman -Syu --needed 3ds-dev 3ds-libvorbisidec 3ds-libogg", "Check again"):
                self.assertIn(required, message)
            self.assertNotIn("User-Agent: dkp apt", message)
            runner.assert_not_called()
            # Represent the user's completed manual setup with fixture files/packages, then use normal detection.
            root = unix_root(tmp)
            with mock.patch.object(toolchain, "host_tool", return_value="/bin/fixture"):
                detected = toolchain.detect(tmp, platform="linux", roots=[root],
                                             run=pacman(toolchain.UNIX_PACKAGES + toolchain.RECOMMENDED_PACKAGES))
            self.assertTrue(detected.can_build)
            self.assertNotIn("devkitpro", [s.key for s in toolchain.plan(detected, tmp, tmp, want_cia=False)])

    def test_local_download_failures_do_not_blame_connection(self):
        body = b"pinned fixture"
        real_open = open
        class Response(io.BytesIO):
            headers = {}
        class BrokenFile:
            def __init__(self, path, phase, code):
                self.file = real_open(path, "wb")
                self.phase, self.code = phase, code
            def write(self, data):
                if self.phase == "write":
                    raise OSError(self.code, "fixture", self.file.name)
                return self.file.write(data)
            def close(self):
                self.file.close()
                if self.phase == "close":
                    raise OSError(self.code, "fixture", self.file.name)
        cases = (("mkdir", errno.ENOSPC, "Not enough space"), ("open", errno.EROFS, "Cannot write"),
                 ("write", errno.ENOSPC, "Not enough space"), ("close", errno.EDQUOT, "Not enough space"),
                 ("replace", errno.EACCES, "Cannot write"), ("write", errno.EIO, "Could not save"),
                 ("mkdir", errno.ENOTDIR, "Cannot save the download at this path"),
                 ("open", errno.ENOENT, "Cannot save the download at this path"),
                 ("replace", errno.ENAMETOOLONG, "Cannot save the download at this path"))
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "file"
            for platform, phase, code, message in ((p, *c) for p in ("win32", "linux", "darwin") for c in cases):
                dest.write_bytes(b"previous verified file")
                error = OSError(code, "fixture", str(dest))
                if phase in ("open", "write", "close"):
                    def file_open(path, mode="r", **kwargs):
                        if mode != "wb":
                            return real_open(path, mode, **kwargs)
                        if phase == "open":
                            raise error
                        return BrokenFile(path, phase, code)
                    effect = file_open
                    patch = mock.patch.object(toolchain, "open", effect, create=True)
                else:
                    patch = mock.patch.object(toolchain.os, "makedirs" if phase == "mkdir" else "replace",
                                              side_effect=error)
                with self.subTest(platform=platform, phase=phase, code=code), patch, \
                     mock.patch.object(toolchain.sys, "platform", platform), \
                     self.assertRaisesRegex(toolchain.Refused, message) as caught:
                    toolchain.download("https://example.invalid/file", str(dest), hashlib.sha256(body).hexdigest(),
                                       opener=lambda request, timeout: Response(body), size=len(body))
                self.assertIn("[Errno %d] fixture" % code, str(caught.exception))
                self.assertIn(repr(str(dest))[:-1], str(caught.exception))
                self.assertNotIn("connection", str(caught.exception).lower())
                self.assertEqual(dest.read_bytes(), b"previous verified file")
                self.assertFalse(Path(str(dest) + ".part").exists())

    def test_windows_download_errors_without_errno_keep_the_os_cause(self):
        for code, message in ((39, "Not enough space"), (112, "Not enough space"),
                              (5, "Cannot write"), (19, "Cannot write"),
                              (3, "Cannot save the download at this path"),
                              (123, "Cannot save the download at this path"),
                              (206, "Cannot save the download at this path")):
            error = OSError(None, "fixture Windows error %d" % code, "C:/fixture/work/file.part")
            error.winerror = code
            with self.subTest(winerror=code), tempfile.TemporaryDirectory() as tmp:
                dest = Path(tmp) / "file"
                dest.write_bytes(b"previous verified file")
                opener = mock.Mock()
                with mock.patch.object(toolchain, "open", side_effect=error, create=True), \
                     self.assertRaisesRegex(toolchain.Refused, message) as caught:
                    toolchain.download("https://example.invalid/file", str(dest), "0"*64, opener=opener)
                self.assertIn(str(error), str(caught.exception))
                opener.assert_not_called()
                self.assertEqual(dest.read_bytes(), b"previous verified file")
                self.assertFalse(Path(str(dest) + ".part").exists())

    def test_socket_failure_still_reports_connection(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(toolchain.Refused, "Check your connection"):
                toolchain.download("https://example.invalid/file", str(Path(tmp) / "file"), "0" * 64,
                                   opener=mock.Mock(side_effect=urllib.error.URLError("fixture timeout")))

    def test_native_packages_without_windows_split_packages(self):
        for platform in ("linux", "darwin"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as tmp:
                root = unix_root(tmp)
                calls = []
                def query(argv):
                    calls.append(argv)
                    return pacman(toolchain.UNIX_PACKAGES + toolchain.RECOMMENDED_PACKAGES)(argv)
                with mock.patch.object(toolchain, "host_tool", return_value="/bin/fixture"):
                    found = toolchain.detect(tmp, platform=platform, roots=[root], run=query)
                self.assertTrue(found.can_build, found.checks)
                self.assertEqual(found.platform, platform)
                self.assertTrue(any(argv[-1] == "-Q" for argv in calls))
                self.assertIn("devkitARM", found.package_versions)
                self.assertNotIn("devkitarm-gcc", found.missing_packages)

    def test_missing_host_tool_blocks_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = unix_root(tmp)
            with mock.patch.object(toolchain, "host_tool", return_value=None):
                found = toolchain.detect(tmp, platform="linux", roots=[root],
                                         run=pacman(toolchain.UNIX_PACKAGES))
            self.assertFalse(found.can_build)
            self.assertTrue(any(c.name == "cmake" and not c.ok for c in found.checks))

    def test_per_os_installer_and_native_makerom(self):
        cases = [("win32", "x86_64", ".exe", "win_x86_64"),
                 ("linux", "x86_64", "install-devkitpro-pacman", "ubuntu_x86_64"),
                 ("darwin", "arm64", ".pkg", "macos_arm64"),
                 ("darwin", "x86_64", ".pkg", "macos_x86_64")]
        for platform, arch, installer, binary in cases:
            with self.subTest(platform=platform, arch=arch):
                steps = {s.key: s for s in toolchain.full_plan("t", "d", platform=platform, arch=arch)}
                self.assertTrue(steps["devkitpro"].downloads[0]["url"].endswith(installer))
                self.assertIn(binary, steps["makerom"].downloads[0]["url"])
                self.assertTrue(all(len(d["sha256"]) == 64 for s in steps.values() for d in s.downloads))
                if platform != "win32":
                    self.assertIn("source", steps["bannertool"].consent)
                    self.assertNotIn("UAC", steps["devkitpro"].describe())
                    self.assertNotIn("cmake make", steps["packages"].describe())

    def test_unknown_architecture_has_no_silent_x86_fallback(self):
        with self.assertRaises(toolchain.Refused):
            toolchain.full_plan("t", "d", platform="linux", arch="arm64")

    def test_unix_elevation_is_not_windows_uac(self):
        got = []
        for platform in ("linux", "darwin"):
            s = toolchain.update_step("/opt/devkitpro", platform=platform)
            actions = toolchain.Actions(elevated=mock.Mock(side_effect=AssertionError),
                                        run=lambda argv, log: got.append(argv) or 0)
            toolchain.run_step(s, lambda _: None, False, actions)
        self.assertEqual(got[0][0], "pkexec")
        self.assertEqual(got[1][0], "/usr/bin/osascript")
        argv = toolchain.privileged(["/path with space/pacman", "a'$(touch bad)"], "darwin")
        import shlex
        self.assertEqual(shlex.split(argv[-1]), ["/path with space/pacman", "a'$(touch bad)"])

    def test_dry_run_makes_no_mutations(self):
        actions = toolchain.Actions(**{k: mock.Mock(side_effect=AssertionError) for k in
                                       ("download", "extract", "elevated", "run")})
        for platform in ("linux", "darwin"):
            for step in toolchain.full_plan("t", "d", platform=platform, arch="x86_64"):
                toolchain.run_step(step, lambda _: None, True, actions)

    def test_archive_traversal_and_links_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "bad.tar.gz"
            for name, link in (("../outside", False), ("/absolute", False), ("link", True)):
                with tarfile.open(archive, "w:gz") as tar:
                    m = tarfile.TarInfo(name)
                    if link:
                        m.type = tarfile.SYMTYPE
                        m.linkname = "../outside"
                    else:
                        m.size = 1
                    tar.addfile(m, None if link else io.BytesIO(b"x"))
                with self.assertRaises(toolchain.Refused):
                    toolchain.extract_tar(str(archive), str(Path(tmp) / "dest"))
            self.assertFalse((Path(tmp) / "outside").exists())

    def test_zip_tool_gets_executable_permission(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "tool.zip"
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("dir/makerom", b"tool")
            dest = toolchain.extract_exe(str(p), "makerom", str(Path(tmp) / "bin"))
            self.assertEqual(Path(dest).read_bytes(), b"tool")
            self.assertTrue(os.access(dest, os.X_OK))


# relprep verifies every music file: use test_builder's three-file music manifest, which game_folder() writes.
class SteamPlatformTests(fixtures.MusicFixtures):
    def test_default_roots_and_library_records_each_os(self):
        for platform, rel in [("linux", ".steam/steam"), ("linux", ".local/share/Steam"),
                              ("linux", ".var/app/com.valvesoftware.Steam/.local/share/Steam"),
                              ("darwin", "Library/Application Support/Steam")]:
            with self.subTest(root=rel), tempfile.TemporaryDirectory() as tmp:
                t = SteamTree(tmp)
                dest = Path(tmp) / "home" / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                import shutil
                shutil.move(t.root, dest)
                roots = discover.vg.default_steam_roots(home=str(Path(tmp) / "home"), platform=platform)
                self.assertIn(str(dest), roots)
                probes = discover.Probes(steam_roots=roots, gog_dirs=[], platform=platform)
                self.assertTrue(discover.verify_copy("undertale", t.folder, probes, ALLOW).accepted)
                # Unknown bytes and missing ownership still fail on every platform.
                stamp = os.stat(os.path.join(t.folder, "data.win"))
                write(os.path.join(t.folder, "data.win"), UT[:-1] + b"!")
                os.utime(os.path.join(t.folder, "data.win"), ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                self.assertFalse(discover.verify_copy("undertale", t.folder, probes, ALLOW).accepted)

    def test_native_layouts_refuse_with_player_steps(self):
        # A Mac bundle is a known layout (verify_game.MACOS_UNDERTALE); its unknown file is an unknown version.
        for platform, name, expected, status in [
                ("linux", "assets/game.unx", "Compatibility", discover.vg.MISSING_FILES),
                ("darwin", "UNDERTALE.app/Contents/Resources/game.ios", "download_depot 391540 391541",
                 discover.vg.UNKNOWN_VERSION)]:
            with tempfile.TemporaryDirectory() as tmp:
                t = SteamTree(tmp)
                os.remove(os.path.join(t.folder, "data.win"))
                write(os.path.join(t.folder, name), b"native fixture")
                probes = t.probes()
                probes.platform = platform
                result = discover.verify_copy("undertale", t.folder, probes, ALLOW)
                self.assertFalse(result.accepted)
                self.assertIn(expected, result.message)
                self.assertEqual(result.result.status, status)

    def test_loose_download_depot_is_not_an_install(self):
        from test_builder import game_folder
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp)
            depot = game_folder(str(Path(tmp) / "Steam/steamapps/content/app_391540/depot_391541"))
            probes = t.probes()
            probes.platform = "darwin"
            self.assertFalse(discover.verify_copy("undertale", depot, probes, ALLOW).accepted)

    def test_signed_out_account_refused_on_unix(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = SteamTree(tmp, owner="76561190000000099")
            probes = t.probes()
            probes.platform = "linux"
            self.assertFalse(discover.verify_copy("undertale", t.folder, probes, ALLOW).accepted)


class PipelinePlatformTests(unittest.TestCase):
    def home_mirror(self, tmp):
        source = Path(fixtures.fake_source(tmp))
        work = Path(tmp) / 'work'
        art = ['platform/3ds/assets/' + kind + '-undertale-' + target + '.png'
               for kind in ('banner', 'icon') for target in ('o3ds', 'n3ds')]
        art.append('platform/3ds/assets/banner.wav')
        for rel in art:
            write(str(source / rel), b'kit art0')
            os.utime(source / rel, (0, 0))
        def seal():
            descriptor = pipeline.products.release_contract.source_descriptor('undertale', '0.1.0',
                pipeline.products.release_contract.payload_digest(pipeline.source_payload(source, 'undertale')))
            descriptor['public_commit'] = 'a' * 40
            import json
            (source / pipeline.products.release_contract.DESCRIPTOR).write_text(json.dumps(descriptor))
        def prepare(target):
            return pipeline.Build(pipeline.products.PRODUCTS['undertale'], tc('win32'),
                str(work), target, str(Path(tmp) / 'invented-game'), source_root=str(source)).prepare()
        seal()
        return source, work / 'src', art, seal, prepare

    def test_verified_unchanged_kit_keeps_both_models_after_generated_home_art(self):
        for order in (('o3ds', 'n3ds'), ('n3ds', 'o3ds')):
            with self.subTest(order=order), tempfile.TemporaryDirectory() as tmp:
                source, dest, art, seal, prepare = self.home_mirror(tmp)
                prepare(order[0])
                for rel in art:
                    write(str(dest / rel), b'generated private HOME art')
                write(str(dest / 'platform/3ds/assets/home-art.json'), '{}')
                objects = [dest / 'build' / ('3ds-undertale-' + target) / 'retained.o'
                           for target in order]
                objects.append(dest / 'build/aot-undertale-o3ds/retained.c')
                for obj in objects:
                    write(str(obj), b'compiled fixture')
                _, copied, removed = prepare(order[1])
                self.assertEqual((copied, removed), (5, 1))
                for obj in objects:
                    self.assertEqual(obj.read_bytes(), b'compiled fixture')
                for rel in art:
                    self.assertEqual((dest / rel).read_bytes(), (source / rel).read_bytes())
                self.assertFalse((dest / 'platform/3ds/assets/home-art.json').exists())

    def test_verified_source_change_or_non_art_damage_still_invalidates_both_models(self):
        for kind in ('source-art', 'source-code', 'mirror-code', 'missing-stamp', 'bad-stamp'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                source, dest, art, seal, prepare = self.home_mirror(tmp)
                prepare('o3ds')
                for rel in art:
                    write(str(dest / rel), b'generated private HOME art')
                for target in ('o3ds', 'n3ds'):
                    write(str(dest / 'build' / ('3ds-undertale-' + target) / 'retained.o'), b'old object')
                if kind == 'source-art':
                    write(str(source / art[0]), b'kit art1')
                    os.utime(source / art[0], (0, 0))
                    seal()
                elif kind == 'source-code':
                    write(str(source / 'engine/src/vm.c'), b'int y;\n')
                    seal()
                elif kind == 'mirror-code':
                    write(str(dest / 'engine/src/vm.c'), b'int y;\n')
                else:
                    stamp = dest / '.builder-source.json'
                    self.assertTrue(stamp.is_file())
                    if kind == 'missing-stamp':
                        stamp.unlink()
                    else:
                        stamp.write_bytes(b'bad metadata')
                # Invalidate before any copy; failure cannot leave old objects beside changed inputs.
                with mock.patch.object(pipeline.shutil, 'copy2', side_effect=OSError('fixture interrupted')):
                    with self.assertRaisesRegex(OSError, 'fixture interrupted'):
                        prepare('n3ds')
                self.assertFalse((dest / 'build').exists())
                prepare('n3ds')
                self.assertFalse((dest / 'build').exists())

    def test_same_size_epoch_update_invalidates_objects_before_copy_even_if_interrupted(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, dest = Path(tmp) / 'kit', Path(tmp) / 'work/src'
            rel = 'engine/src/update.c'
            write(str(source / rel), b'int result = 1;\n')
            os.utime(source / rel, (0, 0))
            pipeline.mirror_source(source, dest, 'undertale')
            build = dest / 'build'
            artifact = build / '3ds-undertale-o3ds/result.o'
            write(str(artifact), b'old compiled code')
            # Same kit is incremental; a timestamp-only change is also immaterial.
            self.assertEqual(pipeline.mirror_source(source, dest, 'undertale'), (0, 0))
            os.utime(source / rel, (1234, 1234))
            self.assertEqual(pipeline.mirror_source(source, dest, 'undertale'), (0, 0))
            self.assertTrue(artifact.exists())
            write(str(source / rel), b'int result = 2;\n')
            os.utime(source / rel, (0, 0))
            self.assertEqual((source / rel).stat().st_size, (dest / rel).stat().st_size)
            self.assertEqual((source / rel).stat().st_mtime, (dest / rel).stat().st_mtime)
            # Even an interrupted copy has already invalidated every target's generated state.
            with mock.patch.object(pipeline.shutil, 'copy2', side_effect=OSError('interrupted')):
                with self.assertRaisesRegex(OSError, 'interrupted'):
                    pipeline.mirror_source(source, dest, 'undertale')
            self.assertFalse(build.exists())
            self.assertEqual((dest / rel).read_bytes(), b'int result = 1;\n')
            self.assertEqual(pipeline.mirror_source(source, dest, 'undertale'), (1, 0))
            self.assertEqual((dest / rel).read_bytes(), b'int result = 2;\n')

    def test_removed_inputs_and_same_size_top_level_files_clean_only_generated_state(self):
        for remove, rel in ((True, 'engine/src/old.c'), (False, 'VERSION'),
                            (False, 'docs/release/build-contract.json')):
            with self.subTest(remove=remove, rel=rel), tempfile.TemporaryDirectory() as tmp:
                source, work = Path(tmp) / 'kit', Path(tmp) / 'work'
                dest = work / 'src'
                write(str(source / rel), b'old input')
                os.utime(source / rel, (0, 0))
                pipeline.mirror_source(source, dest)
                write(str(dest / 'build/aot/old.c'), b'old generated code')
                write(str(work / 'out/game.3dsx'), b'kept output')
                write(str(work / 'ccache/object'), b'kept compiler cache')
                if remove:
                    (source / rel).unlink()
                    (source / rel).parent.rmdir()
                else:
                    write(str(source / rel), b'new input')
                    os.utime(source / rel, (0, 0))
                self.assertEqual(pipeline.mirror_source(source, dest), (0, 1) if remove else (1, 0))
                self.assertFalse((dest / 'build').exists())
                self.assertEqual((work / 'out/game.3dsx').read_bytes(), b'kept output')
                self.assertEqual((work / 'ccache/object').read_bytes(), b'kept compiler cache')

    def test_shipping_host_binaries_preserves_source_descriptor_but_builder_edits_do_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in pipeline.SOURCE_FILES + ('tools/release/release_contract.py',
                        'tools/release/verify_game.py', 'tools/release/music-versions.json',
                        'tools/release/builder/builder.py', 'tools/release/builder/texcache.py'):
                write(str(root / rel), 'source fixture')
            before = pipeline.source_payload(root, 'undertale')
            write(str(root / 'tools/release/builder/bin/texprep-windows-x64.exe'), b'host binary fixture')
            write(str(root / 'tools/release/builder/runtime/python.exe'), b'runtime fixture')
            self.assertEqual(pipeline.source_payload(root, 'undertale'), before)
            write(str(root / 'tools/release/builder/texcache.py'), 'changed builder source')
            self.assertNotEqual(pipeline.source_payload(root, 'undertale'), before)

    def test_package_and_mirror_share_the_3ds_source_filter(self):
        keep = ("engine/src/aot/vm_aot_hooks.h", "engine/src/cli/state_hash_reach.inc",
                "engine/src/physics/disabled/physics_engine.c", "platform/3ds/source/ctr_argui.c",
                "platform/3ds/bench_placeholder.S.in", "platform/3ds/bench/drawskip_undertale.txt",
                "tools/aot/typed.py", "tools/aot/selection/typed-undertale.txt",
                "tools/analysis/vine_old_observer_compile.py", "tools/analysis/vine_old_parent.py",
                "tools/analysis/vine_extract.py")
        omit = ("engine/cmake/toolchains/PS3.cmake", "engine/src/loop.c", "engine/src/audio/al.c",
                "engine/src/embedded/main.c", "engine/src/ps3/main.c", "engine/src/gl/renderer.c",
                "engine/src/physics/box2d/physics.c", "engine/vendor/ps3gl/LICENSE",
                "engine/vendor/box2d/box2d.h", "engine/compat/ps3/compat.h",
                "platform/3ds/labhost/bench.py", "platform/3ds/bench/undertale.txt",
                "platform/3ds/bench/coverage_undertale.txt", "platform/3ds/profiles/private.txt",
                "tools/aot/census.py", "tools/aot/selection/deltarune.txt",
                "tools/analysis/crispart_pack.py", "tools/analysis/unrelated.py")
        with tempfile.TemporaryDirectory() as tmp:
            source, dest = Path(tmp) / "source", Path(tmp) / "dest"
            for rel in keep + omit + pipeline.SOURCE_FILES + package_builder.EXTRA_FILES:
                write(str(source / rel), "fixture")
            write(str(source / "tools/release/builder/builder.py"), 'FIXED_GAME = "undertale"\n')
            for rel in package_builder.DIRECTORIES:
                (source / rel).mkdir(parents=True, exist_ok=True)
            pipeline.mirror_source(str(source), str(dest), "undertale")
            packaged = package_builder.inputs(source, "undertale")
            payload = pipeline.source_payload(source, "undertale")
            for rel in keep:
                self.assertIn(rel, packaged)
                self.assertIn(rel, payload)
                self.assertTrue((dest / rel).is_file(), rel)
            for rel in omit:
                self.assertNotIn(rel, packaged)
                self.assertNotIn(rel, payload)
                self.assertFalse((dest / rel).exists(), rel)

    def test_curated_manifest_covers_the_builder_source_inputs(self):
        root = HERE.parents[1]
        manifest = root / "tools/release/manifest-ut.txt"
        if not manifest.exists():
            self.skipTest("manifest audit runs in the source checkout")
        paths = {line.strip() for line in manifest.read_text(encoding="utf-8").splitlines()
                 if line.strip() and not line.lstrip().startswith("#")}
        required = set(pipeline.SOURCE_FILES + package_builder.EXTRA_FILES)
        for rel in package_builder.DIRECTORIES:
            for path in (root / rel).rglob("*"):
                name = path.relative_to(root).as_posix()
                if path.is_file() and pipeline.source_wanted(name, "undertale"):
                    required.add(name)
        self.assertEqual(sorted(required - paths), [])
        self.assertNotIn("engine/cmake", pipeline.SOURCE_DIRS)
        for path in paths:
            self.assertFalse(path.startswith(("engine/src/ps3/", "engine/vendor/ps3gl/",
                                              "platform/3ds/labhost/", "docs/3ds-hardware.md")), path)

    def test_unix_argv_preserves_paths_and_devkitpro_override(self):
        argv = pipeline.build_argv(tc("darwin", "/custom/dkp"), "/tmp/space ' folder", "undertale", "n3ds",
                                  "/game/space ' $()", platform="darwin")
        self.assertEqual(argv[0], "/bin/bash")
        self.assertNotIn("cygpath", argv[2])
        self.assertIn("/custom/dkp", argv)
        self.assertIn("data=/game/space ' $()", argv)
        self.assertIn("exec bash platform/3ds/build.sh", argv[2])

    @unittest.skipIf(sys.platform == "win32", "runs actual Unix executable shims")
    def test_shims_run_with_quoted_python_and_no_system_nproc(self):
        with tempfile.TemporaryDirectory(prefix="builder ' ") as tmp:
            pipeline.write_python_shims(tmp, sys.executable)
            r = subprocess.run([str(Path(tmp) / "nproc")], stdin=subprocess.DEVNULL, capture_output=True,
                               text=True, timeout=10)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertGreater(int(r.stdout), 0)
            r = subprocess.run([str(Path(tmp) / "python3"), "-I", "-c", "print(42)"], stdin=subprocess.DEVNULL,
                               capture_output=True, text=True, timeout=10)
            self.assertEqual(r.stdout.strip(), "42")

    def test_mirror_has_current_release_contract_and_legal_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            for rel in pipeline.SOURCE_FILES:
                write(os.path.join(tmp, "source", rel), "fixture")
            pipeline.mirror_source(os.path.join(tmp, "source"), os.path.join(tmp, "dest"))
            for rel in ("LICENSE", "THIRD_PARTY_NOTICES", "docs/release/build-contract.json",
                        "tools/release/prepare_legal.py"):
                self.assertTrue(Path(tmp, "dest", rel).is_file())

    def test_sd_mounts_deduplicate_and_copy_to_temp_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "volumes"
            (base / "card/3ds").mkdir(parents=True)
            for platform in ("linux", "darwin"):
                cards = deliver.list_sd_cards(platform=platform, roots=[str(base), str(base)])
                self.assertEqual(len(cards), 1)
                src = Path(tmp) / "build.3dsx"
                src.write_bytes(b"test package")
                item = deliver.CopyItem(str(src), "3ds/test.3dsx", src.stat().st_size)
                self.assertEqual(deliver.copy_items(cards[0].root, [item])[:2], (1, 0))
                self.assertEqual((base / "card/3ds/test.3dsx").read_bytes(), src.read_bytes())


class SdFilesystemTests(unittest.TestCase):
    """argreview-bx: Linux and macOS list only FAT mount points, never a fixed disk that has a 3ds folder."""

    def test_only_fat_mounts_are_listed(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            for name in ("card", "disk", "share"):
                (base / name / "3ds").mkdir(parents=True)
            card, disk, share = (os.path.realpath(str(base / n)) for n in ("card", "disk", "share"))
            mounts = {card: "vfat", disk: "ext4", share: "9p"}
            cards = deliver.list_sd_cards(platform="linux", roots=[str(base)], mounts=mounts)
            self.assertEqual([os.path.realpath(c.root) for c in cards], [card])
            self.assertEqual(deliver.list_sd_cards(platform="darwin", roots=[str(base)],
                                                   mounts={card: "apfs"}), [])

    def test_mount_tables_parse(self):
        linux = ("/dev/sdb1 /media/me/SD\\040CARD vfat rw,nosuid 0 0\n"
                 "C:\\134 /mnt/c 9p rw 0 0\n/dev/nvme0n1p2 / ext4 rw 0 0\n")
        self.assertEqual(deliver.mount_types("linux", read=lambda: linux),
                         {"/media/me/SD CARD": "vfat", "/mnt/c": "9p", "/": "ext4"})
        mac = ("/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)\n"
               "/dev/disk4s1 on /Volumes/SD CARD (msdos, local, nodev, nosuid, noowners)\n")
        self.assertEqual(deliver.mount_types("darwin", run=lambda: mac),
                         {"/": "apfs", "/Volumes/SD CARD": "msdos"})
        self.assertEqual(deliver.mount_types("linux", read=lambda: (_ for _ in ()).throw(OSError())), {})


class SupportPrivacyTests(unittest.TestCase):
    def test_streaming_save_section_and_private_paths_are_removed(self):
        scrub = pipeline.SupportLogFilter(home="/home/fixture-user")
        chunks = ["PC saves (/some/private/folder): 2", "  file0 2026-10-20  Name Fixture, LV 8",
                  "  undertale.ini 2026-10-20 Other Fixture, LV 9", "", "Compiler: devkitARM 66-1",
                  "Steam account 76561190000000001", "Found: D:\\Family Folder\\Steam\\game",
                  "Found: /Volumes/Private Volume/Steam/game", "Found: /home/another-user/Steam/game",
                  "Source: /Volumes/Private Volume/source (build ID archive-0.1.0+aot12)",
                  "  /home/fixture-user/My Output/game.3dsx  12 bytes sha256 " + "a" * 64]
        text = "".join(scrub(c) for c in chunks)
        for private in ("Name Fixture", "Other Fixture", "LV 8", "2026-10-20", "file0", "private/folder",
                        "Family Folder", "Private Volume", "another-user", "fixture-user", "My Output",
                        "76561190000000001"):
            self.assertNotIn(private, text)
        self.assertIn("Compiler: devkitARM 66-1", text)
        self.assertIn("build ID archive-0.1.0+aot12", text)
        self.assertIn("sha256 " + "a" * 64, text)

    def test_owner_probe_writes_sanitized_argsaves_dry_run(self):
        import types
        fixture = ("PC saves (C:\\Users\\Fixture Person\\UNDERTALE): 1\n"
                   "  file0 2026-10-20  Player Fixture, LV 7\n\n"
                   "Compiler: devkitARM 66-1\nFound: /Volumes/Fixture Person/Steam/game\n"
                   "Steam account 76561190000000001\n")
        with tempfile.TemporaryDirectory() as tmp:
            if hasattr(fixtures, "valid_ut_slot"):
                # On the eventual argsaves merge, exercise its real scanner and dry-run formatter too.
                savedir = Path(tmp) / "Fixture Person"
                write(str(savedir / "file0"), fixtures.valid_ut_slot("Player Fixture", 7))
                opts = builder_platform_probe.builder.parse(["--dry-run", "--game", "undertale",
                                                            "--save-dir", str(savedir)])
                probes = discover.Probes(steam_roots=[], gog_dirs=[], save_dir=str(savedir))
                detected = tc("linux")
                detected.package_versions = {"devkitARM": "66-1"}
                raw = []
                with mock.patch.object(builder_platform_probe.builder, "probes_for", return_value=probes), \
                        mock.patch.object(toolchain, "detect", return_value=detected), \
                        mock.patch.object(platform_assets.platform, "machine", return_value="x86_64"):
                    self.assertEqual(builder_platform_probe.builder.dry_run(opts, out=raw.append), 0)
                fixture = "\n".join(raw) + "\nSteam account 76561190000000001\n"
                self.assertIn("Player Fixture", fixture)
            with mock.patch.object(builder_platform_probe, "ROOT", Path(tmp)), \
                    mock.patch.object(sys, "argv", ["probe", "--no-window"]), \
                    mock.patch.dict(sys.modules, {"tkinter": types.SimpleNamespace(TkVersion=8.6)}), \
                    mock.patch.object(builder_platform_probe.subprocess, "run",
                                      return_value=subprocess.CompletedProcess([], 0, fixture)), \
                    mock.patch("builtins.print"):
                self.assertEqual(builder_platform_probe.main(), 0)
            support = next((Path(tmp) / "scratch").glob("builder-platform-*.log")).read_text()
            for private in ("Fixture Person", "Player Fixture", "file0", "LV 7", "76561190000000001"):
                self.assertNotIn(private, support)
            self.assertIn("devkitARM 66-1", support)
            self.assertIn("RESULT: PASS", support)


class PackageVersionLogTests(unittest.TestCase):
    def test_compiler_query_is_host_independent_and_unreadable_only_warns(self):
        for host in ('win32', 'linux', 'darwin'):
            detected = tc(host)
            detected.root = '/fixture/devkitpro'
            calls = []
            def run(argv):
                calls.append(argv)
                return 0, 'arm-none-eabi\n' if argv[-1] == '-dumpmachine' else '16.1.0\n'
            actual = toolchain.compiler_details(detected, run=run)
            self.assertEqual((actual['version'], actual['target']), ('16.1.0', 'arm-none-eabi'))
            self.assertEqual(calls[0][0], os.path.join(detected.root, 'devkitARM', 'bin', 'arm-none-eabi-gcc') +
                             ('.exe' if host == 'win32' else ''))
            def missing(argv):
                raise OSError('fixture missing compiler')
            self.assertEqual(toolchain.compiler_details(detected, run=missing)['version'], 'unknown')

    def test_detected_package_versions_reach_build_log_and_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = fixtures.fake_source(tmp)
            work = Path(tmp) / "work"
            detected = tc(sys.platform)
            detected.package_versions = {"devkitARM": "66-1", "libctru": "2.7.0-1"}
            def output():
                bdir = work / "src/build/3ds-undertale-n3ds"
                write(str(bdir / "CMakeCache.txt"), "CTRPORT_AOT_DIR:PATH=/fixture/aot\n")
                write(str(bdir / "generated/ctrport_build_id.h"),
                      '#define CTRPORT_COMMIT_TEXT "archive-0.1.0+aot12"\n')
                write(str(bdir / "undertale-3ds-n3ds.3dsx"), b"fixture artifact")
            build = pipeline.Build(fixtures.products.PRODUCTS["undertale"], detected, str(work), "n3ds", "/fixture",
                                   source_root=src, popen=lambda *a, **kw: fixtures.FakeProc([], 0, on_start=output),
                                    art_generator=lambda *a, **k: {})
            with mock.patch.object(toolchain, 'compiler_details', return_value={
                    'path': '/fixture/gcc', 'version': '16.1.0', 'target': 'arm-none-eabi'}):
                _arts, record = build.run()
            self.assertEqual(record["toolchain_packages"], detected.package_versions)
            log = Path(build.log_path).read_text()
            self.assertIn("devkitARM 66-1", log)
            self.assertIn("libctru 2.7.0-1", log)
            report = toolchain.dry_run_report(detected, tmp, tmp, want_cia=False)
            self.assertIn("devkitARM 66-1", report)


# release_version() reads the shared release contract (relprep), so fixture roots carry the real one.
CONTRACT = (Path(__file__).resolve().parents[2] / "docs/release/build-contract.json").read_text(encoding="utf-8")


class PackagingTests(unittest.TestCase):
    def test_same_version_tar_update_with_identical_size_and_mtime_replaces_build_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, unpacked, dest = (Path(tmp) / name for name in ('export', 'kit', 'work/src'))
            for rel in pipeline.SOURCE_FILES + package_builder.EXTRA_FILES:
                write(str(root / rel), '0.1.0\n' if rel == 'VERSION' else
                      CONTRACT if rel == 'docs/release/build-contract.json' else 'fixture')
            for rel in package_builder.DIRECTORIES:
                (root / rel).mkdir(parents=True, exist_ok=True)
            write(str(root / 'tools/release/builder/builder.py'), 'FIXED_GAME = None\n')
            rel = 'engine/src/update.c'
            for value in (1, 2):
                write(str(root / rel), 'int result = %d;\n' % value)
                archive = package_builder.package(root, Path(tmp) / 'out', 'undertale', 'linux')
                with tarfile.open(archive) as tar:
                    member = next(m for m in tar.getmembers() if m.name.endswith('/' + rel))
                    self.assertEqual(member.mtime, 0)
                    tar.extractall(unpacked, filter='data')
                    kit = unpacked / member.name.split('/')[0]
                pipeline.mirror_source(kit, dest, 'undertale')
                self.assertEqual((dest / rel).read_text(), 'int result = %d;\n' % value)
                self.assertFalse((dest / 'build').exists())
                # Both targets, generated AOT and future-dated objects must be invalidated.
                for target in ('o3ds', 'n3ds', 'aot'):
                    obj = dest / 'build' / target / 'update.o'
                    write(str(obj), b'previous kit object')
                    os.utime(obj, (2000000000, 2000000000))

    def test_owner_descriptor_packages_only_the_same_game_version_and_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'export'
            for rel in pipeline.SOURCE_FILES + package_builder.EXTRA_FILES:
                write(str(root / rel), '0.1.0\n' if rel == 'VERSION' else
                      CONTRACT if rel == 'docs/release/build-contract.json' else 'fixture')
            for rel in package_builder.DIRECTORIES:
                (root / rel).mkdir(parents=True, exist_ok=True)
            write(str(root / 'tools/release/builder/builder.py'), 'FIXED_GAME = None\n')
            for game in ('undertale', 'deltarune'):
                files = package_builder.inputs(root, game)
                contract = pipeline.products.release_contract
                descriptor = contract.source_descriptor(game, '0.1.0',
                    contract.payload_digest({rel: files[rel] for rel in pipeline.source_payload(root, game)}))
                descriptor['public_commit'] = 'a' * 40
                path = Path(tmp) / 'approved.json'
                import json
                path.write_text(json.dumps(descriptor))
                archive = package_builder.package(root, Path(tmp) / 'out', game, 'linux', release_descriptor=path)
                with tarfile.open(archive) as source:
                    record = next(m for m in source.getmembers() if m.name.endswith('/release-descriptor.json'))
                    self.assertEqual(json.load(source.extractfile(record)), descriptor)
                # Export-time metadata is carried without a manual packaging argument.
                (root / contract.DESCRIPTOR).write_text(json.dumps(descriptor))
                archive = package_builder.package(root, Path(tmp) / 'auto', game, 'linux')
                with tarfile.open(archive) as source:
                    record = next(m for m in source.getmembers() if m.name.endswith('/release-descriptor.json'))
                    self.assertEqual(json.load(source.extractfile(record)), descriptor)
                descriptor['source_sha256'] = '0' * 64
                path.write_text(json.dumps(descriptor))
                with self.assertRaisesRegex(ValueError, 'does not match'):
                    package_builder.package(root, Path(tmp) / 'out', game, 'linux', release_descriptor=path)

    def test_unpacked_kits_import_the_builder_and_probe_dependency_closure(self):
        # Discover dependencies from real sources, independently of the package allow-list.
        # AST traversal includes imports inside GUI/platform branches that this host cannot run.
        repo = HERE.parents[1]
        search = (HERE / "builder", HERE, HERE.parent / "aot")
        available = {p.stem: p for directory in reversed(search) for p in directory.glob("*.py")}
        pending = [p.stem for p in (HERE / "builder").glob("*.py")]
        pending += ["builder_platform_probe", "test_builder_platforms", "test_builder_native_data"]
        dependencies = {}
        while pending:
            name = pending.pop()
            if name in dependencies:
                continue
            source = available[name]
            dependencies[name] = source
            for node in ast.walk(ast.parse(source.read_text(encoding="utf-8"))):
                imports = ([alias.name for alias in node.names] if isinstance(node, ast.Import) else
                           [node.module] if isinstance(node, ast.ImportFrom) and not node.level and node.module else [])
                pending.extend(module.split(".")[0] for module in imports
                               if module.split(".")[0] in available)
        scratch = repo / "scratch"
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as tmp:
            root = Path(tmp) / "source"
            for rel in pipeline.SOURCE_FILES + package_builder.EXTRA_FILES:
                target = root / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(repo / rel, target)
            for rel in package_builder.DIRECTORIES:
                (root / rel).mkdir(parents=True, exist_ok=True)
            for source in dependencies.values():
                target = root / source.relative_to(repo)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            # Runtime bytes are synthetic: this checks all archive layouts using the host's Python/Tk.
            runtime = Path(tmp) / "runtime"
            for rel in ("python.exe", "pythonw.exe", "_tkinter.pyd", "_multiprocessing.pyd", "tcl86t.dll", "tk86t.dll",
                        "tkinter/__init__.py", "tcl/fixture.txt", "LICENSE.txt",
                        "tcl/tcl8.6/license.terms", "tcl/tk8.6/license.terms"):
                write(str(runtime / rel), b"runtime fixture")
            code = (
                "import importlib,json,sys; from pathlib import Path; "
                "root=Path(sys.argv[1]).resolve(); "
                "sys.path[:0]=[str(root/'tools/release/builder'),str(root/'tools/release'),"
                "str(root/'tools/aot')]; "
                "names=json.loads(sys.argv[2]); "
                "modules=[importlib.import_module(n) for n in names]; "
                "assert all(root in Path(m.__file__).resolve().parents for m in modules); "
                "import pipeline,products; assert pipeline.release_version(str(root)); "
                "\ntry:\n    pipeline.release_source(root,'undertale')\n"
                "except pipeline.BuildFailed as exc:\n    assert 'no approved public source' in str(exc)\n"
                "else:\n    raise AssertionError('An unset descriptor allowed a build')\n"
                "descriptor=json.loads((root/'release-descriptor.json').read_text()); "
                "assert descriptor['public_commit'] is None; "
                "descriptor['public_commit']='a'*40; "
                "(root/'release-descriptor.json').write_text(json.dumps(descriptor)); "
                "assert pipeline.release_source(root,'undertale')['source_url'].endswith('/tree/'+'a'*40); "
                "manifest=json.loads((root/'tools/release/music-versions.json').read_text()); "
                "assert all(len(products.load_music_manifest(v,store=s,platform=p))==r['count'] "
                "for s,ps in manifest['releases'].items() for p,vs in ps.items() for v,r in vs.items()); "
                "print('Imported from unpacked kit:',','.join(m.__name__ for m in modules)); "
                "from unittest import mock; import platform_assets,test_builder_platforms\n"
                "for arch in ('x86_64','arm64'):\n"
                "    with mock.patch.object(platform_assets.platform,'machine',return_value=arch):\n"
                "        test_builder_platforms.SupportPrivacyTests("
                "'test_owner_probe_writes_sanitized_argsaves_dry_run').debug()\n"
                "print('Sanitized probe passed with simulated x86_64 and arm64 hosts')"
            )
            import json
            for platform in ("windows", "linux", "macos"):
                with self.subTest(platform=platform):
                    archive = package_builder.package(root, Path(tmp) / "out", "undertale", platform,
                                                      runtime if platform == "windows" else None)
                    unpacked = Path(tmp) / platform
                    unpacked.mkdir()
                    if platform == "windows":
                        with zipfile.ZipFile(archive) as source:
                            source.extractall(unpacked)
                    else:
                        with tarfile.open(archive) as source:
                            source.extractall(unpacked, filter="data")
                    kit = next(unpacked.iterdir())
                    if platform in ("windows", "macos"):
                        opening = (kit / "README.md").read_text(encoding="utf-8")
                        suffix = ".cmd" if platform == "windows" else ".command"
                        self.assertIn("undertale-3ds-builder" + suffix, opening)
                        self.assertTrue((kit / ("undertale-3ds-builder" + suffix)).is_file())
                        self.assertIn("This beta builder is unsigned.", opening)
                        self.assertNotIn(".sha256", opening)
                        self.assertNotIn("Check the download", opening)
                    expected = sorted(name for name, source in dependencies.items()
                                      if pipeline.source_wanted(source.relative_to(repo).as_posix(), 'undertale'))
                    self.assertFalse((kit / 'tools/release/builder/chapters.py').exists())
                    result = subprocess.run([sys.executable, "-I", "-c", code, str(kit),
                                             json.dumps(expected)], cwd=unpacked,
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    self.assertIn("release_contract", result.stdout)
                    self.assertIn("probe passed with simulated x86_64 and arm64 hosts", result.stdout)

    def test_unix_archives_have_launchers_sources_and_no_runtime_or_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "export"
            for rel in pipeline.SOURCE_FILES + package_builder.EXTRA_FILES:
                write(str(root / rel), "0.1.0\n" if rel == "VERSION" else
                      CONTRACT if rel == "docs/release/build-contract.json" else "fixture")
            for rel in package_builder.DIRECTORIES:
                (root / rel).mkdir(parents=True, exist_ok=True)
            write(str(root / "tools/release/builder/builder.py"), "FIXED_GAME = None\n")
            write(str(root / "tools/release/builder/fixture.ogg"), b"never package")
            write(str(root / "platform/3ds/build.sh"), "#!/bin/bash\n")
            for platform in ("linux", "macos"):
                archive = package_builder.package(root, Path(tmp) / "out", "undertale", platform)
                with tarfile.open(archive) as tar:
                    names = tar.getnames()
                    start = next(n for n in names if n.endswith(("builder.sh", "builder.command")))
                    self.assertEqual(tar.getmember(start).mode, 0o755)
                    self.assertIn(b"python3", tar.extractfile(start).read())
                    main = next(n for n in names if n.endswith("/builder/builder.py"))
                    self.assertIn(b'FIXED_GAME = "undertale"', tar.extractfile(main).read())
                    self.assertFalse(any("runtime/" in n or n.endswith(".ogg") for n in names))
                    opening = tar.extractfile(next(n for n in names if n.endswith("/README.md"))).read()
                    self.assertNotIn(b"delta" + b"tale", opening.lower())
                    self.assertNotIn(b".sha256", opening)
                    if platform == "macos":
                        self.assertIn(Path(start).name.encode("utf-8"), opening)
                        self.assertIn(b'choose "Open Anyway", then "Open".', opening)
                        self.assertNotIn(b".sha256", opening)
                    else:
                        self.assertIn(b'`sh ./undertale-3ds-builder.sh`', opening)
                        self.assertIn(b'python3 python3-tk', opening)
                        self.assertIn(b'3DS compiler page', opening)
                self.assertTrue(archive.with_name(archive.name + ".sha256").is_file())

    def test_windows_runtime_is_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(str(root / "VERSION"), "0.1.0\n")
            write(str(root / "docs/release/build-contract.json"), CONTRACT)
            with self.assertRaisesRegex(ValueError, "runtime"):
                package_builder.package(root, root / "out", "undertale", "windows")


if __name__ == "__main__":
    unittest.main()
