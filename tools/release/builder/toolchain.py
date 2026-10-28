"""Wizard page 4: find devkitPro and the other build tools, and set up what is missing, each step behind consent.

devkitPro is never bundled. Missing pieces come from devkitPro's official installer and pacman repositories, and the
.cia tools from their upstream GitHub releases, each download checked against a SHA-256 pinned here. A dry run
(`builder.py --dry-run`) prints every detection result, URL, pin and command without downloading or running anything;
it is the support tool and the only mode exercised on the development PC.
"""
from __future__ import annotations

import dataclasses
import errno
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

import platform_assets as assets
from processes import hidden_options

# devkitPro's Windows installer. It needs administrator rights (UAC),
# is 64-bit only, refuses paths with spaces, and has no silent mode: the user ticks "3DS Development" on its Components
# page. Pinned on 2026-10-19 from GitHub's releases/latest (v3.0.3, published 2018-06-03): the asset's
# size matches the API's, and the file was hashed, never run. A download of another size or digest is refused.
DEVKITPRO_INSTALLER = {
    "version": "3.0.3",
    "url": "https://github.com/devkitPro/installer/releases/download/v3.0.3/devkitProUpdater-3.0.3.exe",
    "sha256": "038a99dc84f1ca0b52e9e0e074a94a3b0672e6d7bf0988563f0ab0812dcbb38d",
    "size": 196304,
}
# Upstream releases with the digests pinned in docs/building-cia.md. MIT-licensed; downloaded into the builder's own
# tools folder, never into devkitPro.
CIA_TOOLS = {
    "makerom": {"version": "v0.18.4",
                "url": "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/"
                       "makerom-v0.18.4-win_x86_64.zip",
                "sha256": "d743507dc66d4c6f8efcce05ff8f5f47163a60e54e0e0c57fa12ec2f54acdf2f"},
    "bannertool": {"version": "1.2.3",
                   "url": "https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/"
                          "bannertool-1.2.3-windows.zip",
                   "sha256": "d0f277bd875e2ad411844378155d27afe124780fcccbd81d9a594e886e2dd3c4"},
}
# pacman packages the build needs: the 3ds-dev group's build pieces, the Tremor music decoder, and CMake/make from
# devkitPro's MSYS2. 3dslink (in 3ds-dev) is only for "Send over Wi-Fi".
REQUIRED_PACKAGES = ("devkitARM", "devkitarm-gcc", "devkitarm-binutils", "devkitarm-newlib", "devkitarm-rules",
                     "devkitarm-cmake", "3ds-cmake", "libctru", "citro3d", "3dstools", "picasso", "tex3ds",
                     "cmake", "make")
RECOMMENDED_PACKAGES = ("3ds-libvorbisidec", "3ds-libogg")
INSTALL_PACKAGES = ("3ds-dev",) + RECOMMENDED_PACKAGES + ("cmake", "make")
UNIX_PACKAGES = ("devkitARM", "devkitarm-rules", "3ds-cmake", "libctru", "citro3d", "3dstools", "picasso", "tex3ds")
UNIX_INSTALL_PACKAGES = ("3ds-dev",) + RECOMMENDED_PACKAGES
# Licence notice beside the Install buttons (docs/release/builder.md, "One-click setup"). Each name was read from the
# project's own repository on 2026-10-22. devkitPro has no single licence: its installer repository has no licence
# file, and its packages carry their own (pacman GPL-2.0, libctru and citro3d zlib, tex3ds GPL-3.0, picasso MIT), so
# the link goes to its GitHub organisation, which lists each repository's licence.
LICENCES = {
    "devkitpro": ("devkitPro", "each package's own licence", "https://github.com/devkitPro"),
    "makerom": ("makerom", "MIT License", "https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.18.4/makerom/LICENSE"),
    "bannertool": ("bannertool", "MIT License", "https://github.com/carstene1ns/3ds-bannertool/blob/1.2.3/LICENSE.txt"),
    "cmake": ("CMake", "BSD 3-Clause License", "https://github.com/Kitware/CMake/blob/v3.31.8/Copyright.txt"),
    "apple": ("Apple Command Line Tools", "Apple's licence, shown by its installer", "https://www.apple.com/legal/sla/"),
    "debian": ("Debian packages", "each package's own licence", "https://www.debian.org/legal/licenses/"),
}
STEP_LICENCES = {"devkitpro": "devkitpro", "packages": "devkitpro", "update": "devkitpro", "makerom": "makerom",
                 "bannertool": "bannertool", "cmake": "cmake", "clt": "apple", "host": "debian"}
# Official release pages, offered as "Open download page" when a one-click step fails.
RELEASE_PAGES = {
    ("win32", "devkitpro"): "https://github.com/devkitPro/installer/releases/tag/v3.0.3",
    ("linux", "devkitpro"): "https://github.com/devkitPro/pacman/releases/tag/v6.0.2",
    ("darwin", "devkitpro"): "https://github.com/devkitPro/pacman/releases/tag/v6.0.2",
    ("*", "makerom"): "https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.18.4",
    ("*", "bannertool"): "https://github.com/carstene1ns/3ds-bannertool/releases/tag/1.2.3",
    ("darwin", "cmake"): "https://github.com/Kitware/CMake/releases/tag/v3.31.8",
}
REQUIRED_FILES = ("cmake/3DS.cmake", "libctru/include/3ds.h", "libctru/lib/libctru.a", "libctru/lib/libcitro3d.a",
                  "devkitARM/bin/arm-none-eabi-gcc.exe", "tools/bin/picasso.exe", "tools/bin/3dsxtool.exe",
                  "tools/bin/smdhtool.exe")


@dataclasses.dataclass
class Check:
    name: str
    ok: bool
    detail: str
    need: str            # "build", "cia", "speed" (optional but recommended), "wifi"


@dataclasses.dataclass
class Toolchain:
    root: str | None
    bash: str | None
    checks: list
    missing_packages: list
    missing_recommended: list
    makerom: str | None
    bannertool: str | None
    threedslink: str | None
    platform: str = sys.platform
    package_versions: dict = dataclasses.field(default_factory=dict)

    @property
    def can_build(self):
        return all(c.ok for c in self.checks if c.need == "build")

    @property
    def can_cia(self):
        return bool(self.makerom and self.bannertool)


def run_capture(argv, timeout=60):
    p = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors="replace", timeout=timeout, **hidden_options())
    return p.returncode, p.stdout


def candidate_roots(env=None, platform=sys.platform):
    env = os.environ if env is None else env
    out = []
    v = env.get("DEVKITPRO", "")
    if platform == "win32":
        # The installer sets DEVKITPRO=/opt/devkitpro (an MSYS2 path), so a Windows path there is a user override.
        if v and not v.startswith("/"):
            out.append(v)
        out.append("C:/devkitPro")
    else:
        if v:
            out.append(v)
        out.append("/opt/devkitpro")
    return list(dict.fromkeys(os.path.normpath(p) for p in out))


def compiler_details(tc, run=run_capture):
    """Query the selected compiler just before building; an unknown version only warns."""
    exe = 'arm-none-eabi-gcc' + ('.exe' if tc.platform == 'win32' else '')
    path = os.path.join(tc.root or '', 'devkitARM', 'bin', exe)
    result = dict(path=path, version='unknown', target='unknown')
    if not tc.root:
        return result
    for key, args, pattern in (('version', ['-dumpfullversion', '-dumpversion'], r'[0-9]+\.[0-9]+\.[0-9]+'),
                               ('target', ['-dumpmachine'], r'[a-z0-9_-]+')):
        try:
            rc, out = run([path] + args)
            if rc == 0 and re.fullmatch(pattern, out.strip()):
                result[key] = out.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return result


def _find_tool(name, dirs, exe):
    for d in dirs:
        if d:
            p = os.path.join(d, name + exe)
            if os.path.isfile(p) and (exe or os.access(p, os.X_OK)):
                return p
    return None


def detect(tools_dir, env=None, platform=sys.platform, run=run_capture, isfile=os.path.isfile, roots=None):
    """Read-only. `run` executes the package query; tests replace it."""
    exe = ".exe" if platform == "win32" else ""
    checks = []
    required = REQUIRED_PACKAGES if platform == "win32" else UNIX_PACKAGES
    if platform != "win32":
        bash = shutil.which("bash")
        checks.append(Check("bash", bool(bash), bash or "Install bash with your system package manager.", "build"))
        clt_ok = True
        if platform == "darwin":
            try:
                clt_ok = run(["/usr/bin/xcode-select", "-p"])[0] == 0
            except (OSError, subprocess.SubprocessError):
                clt_ok = False
        for name in ("cmake", "make", "c++"):
            path = host_tool(name, tools_dir)
            ok = bool(path) and (clt_ok or name == "cmake")
            checks.append(Check(name, ok, path if ok else "not found (install host build tools)", "build"))
        if platform == "linux":
            checks.append(Check("wget", bool(shutil.which("wget")), "Debian download prerequisite", "setup"))
            checks.append(Check("ca-certificates", isfile("/etc/ssl/certs/ca-certificates.crt"),
                                "Debian HTTPS certificates", "setup"))
    root = None
    for r in roots if roots is not None else candidate_roots(env, platform):
        if isfile(os.path.join(r, "msys2", "usr", "bin", "bash.exe")) if platform == "win32" else os.path.isdir(r):
            root = r
            break
    if root is None:
        checks.append(Check("devkitPro", False, "not found (looked in %s)" % ", ".join(
            roots if roots is not None else candidate_roots(env, platform)), "build"))
        return Toolchain(None, None, checks, list(required), list(RECOMMENDED_PACKAGES),
                         _find_tool("makerom", [tools_dir], exe), _find_tool("bannertool", [tools_dir], exe), None,
                         platform)
    bash = os.path.join(root, "msys2", "usr", "bin", "bash.exe") if platform == "win32" else shutil.which("bash")
    checks.append(Check("devkitPro", True, root, "build"))

    installed = set()
    versions = {}
    try:
        argv = [bash, "--login", "-c", "pacman -Q"] if platform == "win32" else [pacman_exe(root), "-Q"]
        rc, out = run(argv)
        if rc == 0:
            versions = dict(line.split(None, 1) for line in out.splitlines() if len(line.split()) >= 2)
            installed = set(versions)
        pkg_detail = "pacman -Q listed %d packages" % len(installed) if rc == 0 else "pacman -Q failed (exit %d)" % rc
    except (OSError, subprocess.SubprocessError) as e:
        pkg_detail = "pacman -Q could not run (%s)" % e
    missing = [p for p in required if p not in installed]
    missing_rec = [p for p in RECOMMENDED_PACKAGES if p not in installed]
    checks.append(Check("devkitPro packages", not missing,
                        "all %d present" % len(required) if not missing else "missing: " + ", ".join(missing)
                        + " (" + pkg_detail + ")", "build"))
    checks.append(Check("Tremor music decoder", not missing_rec,
                        "present" if not missing_rec else "missing: %s (music then uses the slower decoder)"
                        % ", ".join(missing_rec), "speed"))
    files = REQUIRED_FILES if platform == "win32" else tuple(f.replace(".exe", "") for f in REQUIRED_FILES)
    missing_files = [f for f in files if not isfile(os.path.join(root, f))]
    checks.append(Check("Compiler and libraries", not missing_files,
                        "present" if not missing_files else "missing: " + ", ".join(missing_files), "build"))

    py_ok = sys.version_info >= (3, 9)
    checks.append(Check("Python", py_ok, "%d.%d.%d (%s)" % (sys.version_info[:3] + (python_exe(),)), "build"))

    tool_dirs = [tools_dir, os.path.join(root, "tools", "bin")]
    makerom = _find_tool("makerom", tool_dirs, exe)
    bannertool = _find_tool("bannertool", tool_dirs, exe)
    checks.append(Check("makerom (for .cia)", bool(makerom), makerom or "not found", "cia"))
    checks.append(Check("bannertool (for .cia)", bool(bannertool), bannertool or "not found", "cia"))
    threedslink = _find_tool("3dslink", [os.path.join(root, "tools", "bin")], exe)
    return Toolchain(root, bash, checks, missing, missing_rec, makerom, bannertool, threedslink, platform, versions)


def pacman_exe(root="/opt/devkitpro"):
    return shutil.which("dkp-pacman") or os.path.join(root, "pacman", "bin", "pacman")


def host_tool(name, tools_dir):
    dirs = [os.path.join(tools_dir, "cmake", "cmake-3.31.8-macos-universal", "CMake.app", "Contents", "bin"),
            "/Applications/CMake.app/Contents/bin", "/opt/homebrew/bin", "/usr/local/bin"]
    return _find_tool(name, dirs, "") or shutil.which(name)


def python_exe():
    """A console python.exe for build.sh (the GUI may run under pythonw.exe)."""
    exe = sys.executable
    d, base = os.path.split(exe)
    if base.lower() == "pythonw.exe" and os.path.isfile(os.path.join(d, "python.exe")):
        return os.path.join(d, "python.exe")
    return exe


# ---------------------------------------------------------------------------------------------------------------------
# Setup steps

@dataclasses.dataclass
class Step:
    key: str
    title: str
    consent: str                 # shown before the button: what, from where, how big
    downloads: list              # dicts: url, sha256, size, dest
    commands: list               # argv lists, in order
    admin: bool = False
    note: str = ""
    platform: str = sys.platform

    @property
    def licence(self):
        return LICENCES.get(STEP_LICENCES.get(self.key, ""))

    @property
    def page(self):
        return RELEASE_PAGES.get((self.platform, self.key)) or RELEASE_PAGES.get(("*", self.key))

    def confirm_text(self):
        """The one question before a step runs: what it does, then whose licence the click accepts."""
        text = self.consent
        if self.licence:
            name, licence, url = self.licence
            text += "\n\nBy clicking OK you agree to the licence of %s (%s):\n%s" % (name, licence, url)
        return text

    def describe(self):
        prompt = "Windows shows a UAC prompt" if self.platform == "win32" else "your system asks for permission"
        lines = ["%s%s" % (self.title, " (needs administrator permission: %s)" % prompt if self.admin else "")]
        lines.append("  asks first: " + self.consent)
        for d in self.downloads:
            lines.append("  download %s" % d["url"])
            lines.append("    SHA-256 %s" % (d["sha256"] or "NOT PINNED: the builder refuses this download until a "
                                              "digest is pinned in toolchain.py"))
            lines.append("    size %s; saved to %s" % (("%d bytes" % d["size"]) if d.get("size") else "not measured",
                                                       d["dest"]))
        for c in self.commands:
            lines.append("  run " + " ".join(_quote(a) for a in c))
        if self.note:
            lines.append("  note: " + self.note)
        return "\n".join(lines)


def _quote(a):
    return '"%s"' % a if (" " in a or not a) else a


def full_plan(tools_dir, downloads_dir, root=None, want_cia=True, packages=None, platform=sys.platform, arch=None):
    """Every step for a machine with nothing installed (what the dry run shows as the clean-machine bootstrap)."""
    if platform != "win32":
        return unix_plan(tools_dir, downloads_dir, root or "/opt/devkitpro", want_cia, platform, arch)
    root = root or "C:/devkitPro"
    packages = INSTALL_PACKAGES if packages is None else packages
    bash = os.path.normpath(os.path.join(root, "msys2", "usr", "bin", "bash.exe"))
    inst = os.path.join(downloads_dir, os.path.basename(DEVKITPRO_INSTALLER["url"]))
    steps = [
        Step("devkitpro", "Install devkitPro %s with its official installer" % DEVKITPRO_INSTALLER["version"],
             "The 3DS compiler (devkitPro) isn't installed. The builder can download devkitPro's official installer "
             "(%s) from github.com/devkitPro. Windows will ask for administrator permission; on its Components page, "
             "tick 3DS Development." % ("%d KB" % round(DEVKITPRO_INSTALLER["size"] / 1000)
                                         if DEVKITPRO_INSTALLER["size"] else "size not measured"),
             [dict(DEVKITPRO_INSTALLER, dest=inst)], [[inst]], admin=True,
             note="the installer itself downloads MSYS2 and packages from downloads.devkitpro.org; it has no silent "
                  "mode, so the user clicks through it"),
        Step("packages", "Install the 3DS packages with devkitPro's pacman",
             "devkitPro's package manager will download and install: %s (about 460 MB installed) from "
             "downloads.devkitpro.org. No administrator permission is needed." % ", ".join(packages),
             [], [[bash, "--login", "-c", "pacman -S --needed --noconfirm " + " ".join(packages)]]),
    ]
    if want_cia:
        for name, pin in CIA_TOOLS.items():
            z = os.path.join(downloads_dir, os.path.basename(pin["url"]))
            steps.append(Step(name, "Download %s %s (for .cia files)" % (name, pin["version"]),
                              "Downloads %s %s from github.com (MIT licence) into the builder's tools folder %s."
                              % (name, pin["version"], tools_dir),
                              [dict(pin, size=None, dest=z)], [["extract", z, name + ".exe", tools_dir]]))
    for s in steps:
        s.platform = platform
    return steps


def cia_download_supported(platform):
    if platform == 'win32':
        return True
    try:
        assets.makerom_pin(platform)
        return True
    except ValueError:
        return False


def plan(tc: Toolchain, tools_dir, downloads_dir, want_cia):
    """Only the steps this machine needs."""
    steps = full_plan(tools_dir, downloads_dir, root=tc.root, want_cia=want_cia and cia_download_supported(tc.platform),
                      packages=INSTALL_PACKAGES, platform=tc.platform)
    keep = []
    for s in steps:
        if s.key in ("host", "cmake", "clt"):
            needed = {c.name for c in tc.checks if not c.ok}
            if (s.key == "host" and needed.intersection(("cmake", "make", "c++", "bash", "wget", "ca-certificates"))) or (
                    s.key == "cmake" and "cmake" in needed) or (s.key == "clt" and needed.intersection(("make", "c++"))):
                keep.append(s)
            continue
        if s.key == "devkitpro" and tc.root is None:
            keep.append(s)
        elif s.key == "packages" and (tc.root is None or tc.missing_packages or tc.missing_recommended or
                                      not all(c.ok for c in tc.checks if c.name == "Compiler and libraries")):
            keep.append(s)
        elif s.key == "makerom" and want_cia and not tc.makerom:
            keep.append(s)
        elif s.key == "bannertool" and want_cia and not tc.bannertool:
            keep.append(s)
    return keep


def setup_inventory(tc, steps):
    """Stable full bootstrap list. Only dependencies gate an individual action, never unrelated installers."""
    needed = {s.key for s in plan(tc, '', '', want_cia=True)}
    dependencies = {'packages': ('devkitpro',), 'bannertool': ('host', 'clt')}
    return [(s, s.key in needed, next((p.title for p in steps
             if p.key in dependencies.get(s.key, ()) and p.key in needed), None)) for s in steps]


def licence_notice(platform=sys.platform):
    """The line beside the Install buttons and the links under it: every tool this platform's setup can install."""
    keys = {"win32": ("devkitpro", "makerom", "bannertool"),
            "linux": ("debian", "devkitpro", "makerom", "bannertool"),
            "darwin": ("apple", "cmake", "devkitpro", "makerom", "bannertool")}.get(platform,
                                                                                  ("devkitpro", "makerom", "bannertool"))
    entries = [LICENCES[k] for k in keys]
    text = "By clicking Install or Update you agree to the licences of: %s." % ", ".join(
        "%s (%s)" % (name, licence) for name, licence, _ in entries)
    return text, [(name, url) for name, _, url in entries]


def update_step(root, platform=sys.platform):
    """The separate "Update devkitPro" button: never run silently, since an MSYS2 core update can need a shell
    restart."""
    if platform != "win32":
        return Step("update", "Update devkitPro", "Updates devkitPro packages from its official repositories.", [],
                    [privileged([pacman_exe(root or "/opt/devkitpro"), "-Syu", "--noconfirm"], platform)],
                    admin=True, platform=platform)
    bash = os.path.normpath(os.path.join(root or "C:/devkitPro", "msys2", "usr", "bin", "bash.exe"))
    return Step("update", "Update devkitPro (pacman -Syu)",
                "Updates every devkitPro package from downloads.devkitpro.org. If MSYS2 itself updates, close the "
                "builder, then run the update again.", [], [[bash, "--login", "-c", "pacman -Syu --noconfirm"]],
                platform=platform)


class Refused(Exception):
    pass


def privileged(argv, platform):
    """Use the desktop password prompt; stdin belongs to the build log, never to sudo."""
    if platform == "darwin":
        script = 'on run argv\n do shell script (item 1 of argv) with administrator privileges\nend run'
        return ["/usr/bin/osascript", "-e", script, shlex.join(argv)]
    return ["pkexec"] + argv


def unix_plan(tools_dir, downloads_dir, root, want_cia, platform, arch):
    if platform not in ("linux", "darwin"):
        raise Refused("Supported builder systems: Windows, Debian-family Linux and macOS.")
    pin = assets.MAC_INSTALLER if platform == "darwin" else assets.DEBIAN_INSTALLER
    inst = os.path.abspath(os.path.join(downloads_dir, os.path.basename(pin["url"])))
    if platform == "linux":
        conf = os.path.abspath(os.path.join(downloads_dir, "apt-builder.conf"))
        steps = [Step("host", "Install Debian build prerequisites",
                      "Installs python3, python3-tk, bash, cmake, make, g++, wget and CA certificates from your "
                      "system's apt repositories. The desktop asks for administrator permission (PolicyKit).", [],
                      [privileged(["/usr/bin/apt-get", "update"], platform),
                       privileged(["/usr/bin/apt-get", "install", "-y", "python3", "python3-tk", "bash", "cmake",
                                   "make", "g++", "wget", "ca-certificates"], platform)], admin=True),
                 Step("devkitpro", "Install devkitPro's Debian package manager",
                      "Downloads devkitPro's official 800-byte apt setup script, checks its pinned SHA-256, then "
                      "adds the official apt repository and installs devkitpro-pacman with administrator permission.",
                      [dict(pin, dest=inst)], [["apt-config", conf],
                      privileged(["/usr/bin/env", "APT_CONFIG=" + conf, "DEBIAN_FRONTEND=noninteractive",
                                  "/bin/bash", inst], platform)], admin=True,
                      note="Needs pkexec and a running desktop authentication agent; on a headless system, install "
                           "devkitpro-pacman from a terminal first.")]
    else:
        cmake_archive = os.path.abspath(os.path.join(downloads_dir, os.path.basename(assets.MAC_CMAKE["url"])))
        steps = [Step("clt", "Install Apple's Command Line Tools",
                      "Opens Apple's installer for make and the C++ compiler. Finish its window, then click Check again.",
                      [], [["/usr/bin/xcode-select", "--install"]]),
                 Step("cmake", "Download CMake 3.31.8 (Apple Silicon and Intel)",
                      "Downloads 78.4 MB from Kitware's official GitHub release, checks SHA-256, and unpacks "
                      "CMake into the builder's tools folder. No administrator permission is needed.",
                      [dict(assets.MAC_CMAKE, dest=cmake_archive)],
                      [["untar-cmake", cmake_archive, os.path.join(tools_dir, "cmake")]]),
                 Step("devkitpro", "Install devkitPro pacman 6.0.2",
                      "Downloads devkitPro's official 65.4 MB .pkg, checks SHA-256, then asks macOS for "
                      "administrator permission. The .pkg selects Apple Silicon or Intel automatically.",
                      [dict(pin, dest=inst)],
                      [privileged(["/usr/sbin/installer", "-pkg", inst, "-target", "/"], platform)], admin=True)]
    steps.append(Step("packages", "Install devkitPro's 3DS packages",
                      "Downloads and installs 3ds-dev, 3ds-libvorbisidec and 3ds-libogg from devkitPro's official "
                      "repositories (several hundred MB). Your system asks for administrator permission.", [],
                      [privileged([pacman_exe(root), "-Syu", "--needed", "--noconfirm"] +
                                  list(UNIX_INSTALL_PACKAGES), platform)], admin=True))
    if want_cia:
        try:
            makerom = assets.makerom_pin(platform, arch)
        except ValueError as e:
            raise Refused(str(e)) from e
        archive = os.path.abspath(os.path.join(downloads_dir, os.path.basename(makerom["url"])))
        steps.append(Step("makerom", "Download makerom v0.18.4",
                          "Downloads the native makerom binary from Project_CTR's GitHub release with a pinned "
                          "SHA-256 into the builder's tools folder (under 0.3 MB).", [dict(makerom, dest=archive)],
                          [["extract", archive, "makerom", tools_dir]]))
        archive = os.path.abspath(os.path.join(downloads_dir, os.path.basename(assets.BANNERTOOL_SOURCE["url"])))
        source = os.path.abspath(os.path.join(downloads_dir, "bannertool-source"))
        steps.append(Step("bannertool", "Build bannertool 1.2.3 for this computer",
                          "Downloads 211 KB of MIT-licensed source from bannertool's GitHub release, checks "
                          "SHA-256, and compiles it with the system C++ compiler into the builder's tools folder. "
                          "No administrator permission is needed.", [dict(assets.BANNERTOOL_SOURCE, dest=archive)],
                          [["untar", archive, source], ["build-bannertool", source, tools_dir]],
                          note="No macOS binary is published for 1.2.3. Its Linux binary needs glibc 2.39; "
                               "building the pinned source also supports Debian's older glibc."))
    for s in steps:
        s.platform = platform
    return steps


def extract_tar(archive, dest, cmake_cli=False):
    """Only regular files and directories; never follow archive links or escape the destination."""
    os.makedirs(dest, exist_ok=True)
    base = os.path.realpath(dest)
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        if cmake_cli:
            # Kitware also packages Qt GUI framework symlinks. The builder needs only the CLI and its resources.
            prefix = "cmake-3.31.8-macos-universal/CMake.app/Contents/"
            members = [m for m in members if m.name.startswith(prefix + "share/") or
                       m.name in [prefix + "bin/" + n for n in ("cmake", "ctest", "cpack", "ccmake")]]
        for m in members:
            target = os.path.realpath(os.path.join(base, m.name))
            if (m.name.startswith(("/", "\\")) or "\\" in m.name or
                    os.path.commonpath([base, target]) != base or not (m.isdir() or m.isfile())):
                raise Refused("Unsafe archive member: %s" % m.name)
        for m in members:
            target = os.path.join(base, m.name)
            if m.isdir():
                os.makedirs(target, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with tar.extractfile(m) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                os.chmod(target, 0o755 if m.mode & 0o111 else 0o644)


BANNER_PATCH_SHA256 = 'd23e4f94d0a986f45a12854dff49e2c1d453ad62a00b51bb5e7d5d090516a80c'
BANNER_LZ11_SHA256 = 'edb1e8800ab594b6433d678c71419eeb0e2815dc9449e107c6890a2e238010c5'
BANNER_LZ11_PATCHED_SHA256 = '7a87ae0932215fdb92323fd8c73c6c600c0b08b15cd61a5b14bd97ec56f3dcac'


def patch_bannertool(source):
    patch = os.path.join(os.path.dirname(__file__), 'bannertool-1.2.3-clang.patch')
    with open(patch, 'rb') as f:
        if hashlib.sha256(f.read()).hexdigest() != BANNER_PATCH_SHA256:
            raise Refused("The bannertool source patch is damaged. Unpack the builder again.")
    path = os.path.join(source, '3ds', 'lz11.cpp')
    with open(path, 'rb') as f:
        data = f.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest == BANNER_LZ11_PATCHED_SHA256:
        return
    if digest != BANNER_LZ11_SHA256:
        raise Refused("The bannertool source does not match the pinned version. Unpack the builder again.")
    data = data.replace(b'u8 pad[padLength] = {0};', b'u8 pad[4] = {0};')
    if hashlib.sha256(data).hexdigest() != BANNER_LZ11_PATCHED_SHA256:
        raise Refused("The bannertool source patch is damaged. Unpack the builder again.")
    with open(path + '.part', 'wb') as f:
        f.write(data)
    os.replace(path + '.part', path)


def build_bannertool(source, tools_dir, run, log):
    """Equivalent to upstream's CMake target, using only the native C++17 compiler.

    Avoids requiring CMake >=3.28 just for this host tool on Debian 12. All third-party sources are in the pin.
    """
    source = os.path.join(source, "bannertool-1.2.3", "source")
    cpp = sorted(os.path.join(d, f) for d, _, files in os.walk(source) for f in files if f.endswith(".cpp"))
    if len(cpp) != 10:
        raise Refused("The pinned bannertool source is incomplete (expected 10 .cpp files).")
    patch_bannertool(source)
    os.makedirs(tools_dir, exist_ok=True)
    dest = os.path.abspath(os.path.join(tools_dir, "bannertool"))
    command = [host_tool("c++", tools_dir) or "c++", "-std=c++17", "-O2", '-DVERSION="1.2.3"',
               "-DSTBI_ONLY_PNG", "-DSTBI_NO_LINEAR", "-DSTBI_NO_STDIO", "-DSTB_VORBIS_NO_PUSHDATA_API",
               "-DSTB_VORBIS_NO_STDIO", "-DDR_WAV_NO_STDIO", "-I" + source, "-I" + os.path.join(source, "pc")]
    code = run(command + cpp + ["-o", dest + ".part"], log)
    if code == 0:
        os.chmod(dest + ".part", 0o755)
        os.replace(dest + ".part", dest)
    return code


def builder_version():
    try:
        with open(os.path.join(os.path.dirname(__file__), "..", "..", "..", "VERSION"), encoding="utf-8") as f:
            version = f.read(80).strip()
        return version if re.fullmatch(r"[0-9A-Za-z.+-]{1,64}", version) else "unknown"
    except OSError:
        return "unknown"


def manual_install_message(platform="linux"):
    """Official terminal route, with our pin check added before the installer runs. Never executes it."""
    packages = "sudo dkp-pacman -Syu --needed 3ds-dev 3ds-libvorbisidec 3ds-libogg"
    if platform == "darwin":
        pin = assets.MAC_INSTALLER
        name = "devkitpro-pacman-installer.pkg"
        steps = ("Download %s from https://github.com/devkitPro/pacman/releases/tag/v6.0.2\n"
                 "cd ~/Downloads\n"
                 "echo '%s  %s' | shasum -a 256 -c - &&\n"
                 "sudo installer -pkg ./%s -target / &&\n%s" % (name, pin["sha256"], name, name, packages))
    elif platform == "win32":
        pin = DEVKITPRO_INSTALLER
        steps = ("Download %s\nCheck Get-FileHash .\\devkitProUpdater-3.0.3.exe -Algorithm SHA256\n"
                 "Expected: %s\nRun the installer and select 3DS Development." % (pin["url"], pin["sha256"]))
    else:
        pin = assets.DEBIAN_INSTALLER
        name = "install-devkitpro-pacman"
        steps = ("wget %s\n"
                 "echo '%s  %s' | sha256sum -c - &&\n"
                 "chmod +x ./%s && sudo ./%s &&\n%s" % (pin["url"], pin["sha256"], name, name, name, packages))
    return "devkitPro blocked the download. Install manually:\n%s\nThen click Check again." % steps


def _save_download(action, *args, **kwargs):
    """Keep local file errors separate from HTTP/socket errors."""
    try:
        return action(*args, **kwargs)
    except OSError as exc:
        if exc.errno in (errno.ENOSPC, errno.EDQUOT):
            message = "Not enough space in the work folder."
        elif exc.errno in (errno.EACCES, errno.EPERM, errno.EROFS):
            message = "Cannot write to the work folder. Check its permissions."
        else:
            message = "Could not save the download. Check the work folder."
        raise Refused(message) from None


def download(url, dest, sha256, opener=urllib.request.urlopen, progress=None, size=None):
    """Downloads to dest only if the bytes match the pinned SHA-256 (and the pinned size, when there is one)."""
    if not sha256:
        raise Refused("No pinned SHA-256 for %s; the builder will not download it." % url)
    _save_download(os.makedirs, os.path.dirname(dest) or ".", exist_ok=True)
    tmp = dest + ".part"
    h = hashlib.sha256()
    host = (urllib.parse.urlsplit(url).hostname or "").lower()
    devkitpro = host == "devkitpro.org" or host.endswith(".devkitpro.org")
    request = (urllib.request.Request(url, headers={"User-Agent": "ctrport-builder/" + builder_version()})
               if host else url)
    blocked = (manual_install_message("linux" if host == "apt.devkitpro.org" else sys.platform)
               if devkitpro else "Download blocked by the server. Try again later.")
    try:
        f = _save_download(open, tmp, "wb")
        try:
            with opener(request, timeout=60) as r:
                headers = getattr(r, "headers", None) or {}
                if "text/html" in headers.get("Content-Type", "").lower():
                    raise Refused(blocked)
                total = int(headers.get("Content-Length") or 0)
                done = 0
                while True:
                    chunk = r.read(1 << 16)
                    if not chunk:
                        break
                    if done == 0 and chunk.lstrip().lower().startswith((b"<!doctype html", b"<html")):
                        raise Refused(blocked)
                    _save_download(f.write, chunk)
                    h.update(chunk)
                    done += len(chunk)
                    if size and done > size:
                        break
                    if progress:
                        progress(done, total)
        finally:
            _save_download(f.close)
        if size and done != size:
            raise Refused("%s is %s bytes, not the pinned %d; deleted it." % (
                url, "more than %d" % size if done > size else done, size))
        if h.hexdigest() != sha256.lower():
            raise Refused("%s has SHA-256 %s, not the pinned %s; deleted it." % (url, h.hexdigest(), sha256))
        _save_download(os.replace, tmp, dest)
    except (Refused, OSError, ValueError) as exc:
        if os.path.exists(tmp):
            _save_download(os.remove, tmp)
        if isinstance(exc, Refused):
            raise
        if isinstance(exc, urllib.error.HTTPError):
            code = exc.code
            exc.close()
            if code == 403 and devkitpro:
                raise Refused(blocked) from None
            raise Refused("Download failed (HTTP %d). Try again later." % code) from None
        raise Refused("Could not download the file. Check your connection and try again.") from None
    return dest


def extract_exe(zip_path, exe_name, dest_dir):
    with zipfile.ZipFile(zip_path) as z:
        members = [m for m in z.namelist() if os.path.basename(m).lower() == exe_name.lower()]
        if not members:
            raise Refused("%s has no %s" % (zip_path, exe_name))
        os.makedirs(dest_dir, exist_ok=True)
        out = os.path.join(dest_dir, exe_name)
        with z.open(members[0]) as src, open(out, "wb") as dst:
            shutil.copyfileobj(src, dst)
    if not exe_name.lower().endswith(".exe"):
        os.chmod(out, 0o755)
    return out


def run_elevated(path):
    """Starts the devkitPro installer with a UAC prompt and waits for it."""
    import ctypes
    from ctypes import wintypes

    class SHELLEXECUTEINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("fMask", ctypes.c_ulong), ("hwnd", wintypes.HWND),
                    ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                    ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                    ("dwHotKey", wintypes.DWORD), ("hIcon", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]
    info = SHELLEXECUTEINFO(cbSize=ctypes.sizeof(SHELLEXECUTEINFO), fMask=0x40, lpVerb="runas", lpFile=path, nShow=1)
    if not ctypes.windll.shell32.ShellExecuteExW(ctypes.byref(info)):
        raise Refused("The devkitPro installer did not start (the administrator prompt was declined?).")
    ctypes.windll.kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
    code = wintypes.DWORD()
    ctypes.windll.kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(info.hProcess)
    return code.value


@dataclasses.dataclass
class Actions:
    """What running a step does. The dry run and the tests swap these out; nothing here runs on import."""
    download: object = download
    extract: object = extract_exe
    elevated: object = run_elevated
    run: object = None           # callable(argv, log) -> exit code


def run_step(step: Step, log, dry_run, actions: Actions | None = None, stage=None, work=None, root=None):
    """Runs one consented step. In a dry run it only logs what it would do. Returns True on success.

    stage(kind, text), when given, hears each phase ("download" or "run") so a window can show progress."""
    actions = actions or Actions()
    stage = stage or (lambda kind, text: None)
    if dry_run:
        log("[dry run] would do: " + step.describe())
        return True
    if work is not None:
        import cleanup
        paths = installed_paths(step, work, root)
        cleanup.record_before(work, paths)
        cleanup.record_before(work, [d['dest'] for d in step.downloads], 'downloads')
    for d in step.downloads:
        stage("download", "Downloading %s..." % os.path.basename(d["dest"]))
        actions.download(d["url"], d["dest"], d["sha256"], size=d.get("size"))
        if work is not None:
            cleanup.record_after(work, [d['dest']])
        log("downloaded %s (SHA-256 checked)" % d["url"])
    for argv in step.commands:
        stage("run", "Waiting for the installer..." if step.admin else "Installing...")
        if argv[0] == "apt-config":
            with open(argv[1], "w", encoding="utf-8", newline="\n") as f:
                f.write('APT::Get::Assume-Yes "true";\n')
            continue
        if argv[0] in ("untar", "untar-cmake"):
            extract_tar(argv[1], argv[2], cmake_cli=argv[0] == "untar-cmake")
            log("unpacked %s" % argv[1])
            continue
        if argv[0] == "build-bannertool":
            code = build_bannertool(argv[1], argv[2], actions.run, log)
            if code:
                raise Refused("The native bannertool build failed (exit %d). Check the host C++ compiler." % code)
            continue
        if argv[0] == "extract":
            out = actions.extract(argv[1], argv[2], argv[3])
            log("extracted %s" % out)
            continue
        if step.admin and step.platform == "win32":
            code = actions.elevated(argv[0])
        else:
            code = actions.run(argv, log)
        if code != 0:
            if step.key in ("packages", "update"):
                raise Refused("devkitPro's package manager couldn't install %s. Check your internet connection and "
                              "try again." % " ".join(INSTALL_PACKAGES if step.platform == "win32" else UNIX_INSTALL_PACKAGES))
            raise Refused("%s exited with code %s." % (os.path.basename(argv[0]), code))
    if work is not None:
        cleanup.record_after(work, paths)
    return True


def installed_paths(step, work, root=None):
    """Only explicit installer destinations. OS package managers stay outside cleanup."""
    if step.key in ('devkitpro', 'packages'):
        return [root or ('C:/devkitPro' if step.platform == 'win32' else '/opt/devkitpro')]
    paths = []
    for argv in step.commands:
        if argv[0] == 'extract':
            paths.append(os.path.join(argv[3], argv[2]))
        elif argv[0] == 'untar-cmake':
            paths.append(argv[2])
        elif argv[0] == 'build-bannertool':
            paths.append(os.path.join(argv[2], 'bannertool'))
    return paths


def dry_run_report(tc: Toolchain, tools_dir, downloads_dir, want_cia=True):
    lines = ["Detection:"]
    if want_cia and not cia_download_supported(tc.platform):
        lines.append('.cia download unavailable on this CPU. You can still build .3dsx.')
        want_cia = False
    for c in tc.checks:
        lines.append("  [%s] %-30s %s (needed for: %s)" % ("ok" if c.ok else "--", c.name, c.detail, c.need))
    if tc.package_versions:
        lines.append("Installed devkitPro package versions (pacman -Q):")
        lines.extend("  %s %s" % item for item in sorted(tc.package_versions.items()))
    steps = plan(tc, tools_dir, downloads_dir, want_cia)
    lines.append("")
    lines.append("Steps this machine needs: %s" % ("none" if not steps else len(steps)))
    for s in steps:
        lines.append(s.describe())
    lines.append("")
    lines.append("Full bootstrap on a machine with nothing installed (each step asks for consent first):")
    for i, s in enumerate(full_plan(tools_dir, downloads_dir, want_cia=want_cia, platform=tc.platform), 1):
        lines.append("%d. %s" % (i, s.describe()))
    lines.append("")
    lines.append("Separate button, never automatic:")
    lines.append(update_step(tc.root, platform=tc.platform).describe())
    lines.append("")
    lines.append("Dry run: nothing was downloaded, installed or changed.")
    return "\n".join(lines)
