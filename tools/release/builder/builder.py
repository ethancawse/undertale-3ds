#!/usr/bin/env python3
"""The builder app: turns your own purchased UNDERTALE (or DELTARUNE) into a personal 3DS build on this PC.

    builder.py                      the wizard (window)
    builder.py --dry-run            print every detection result, URL, pinned hash and command it would run; changes
                                    nothing (the support tool)
    builder.py --cli --sd E:\\      the same steps without a window

Options:
    --game undertale|deltarune      fix the game (each public repo's launcher passes its own)
    --work DIR                      work folder (default %LOCALAPPDATA%\\u3ds\\w, or C:\\u3ds)
    --devkitpro DIR                 devkitPro folder (default C:\\devkitPro)
    --folder DIR                    game folder instead of searching (it still has to pass the install record check)
    --target n3ds|o3ds|both         (--cli) console model, default both (New and Old 3DS)
    --cia                           (--cli) also make a .cia
    --chapters 1,3                  (--cli) DELTARUNE chapters to build; default all checked chapters
    --openh264                      (--cli) build the software video decoder
    --sd DIR                        (--cli) SD card to copy to; without it the build stays in <work>/out
    --save-dir DIR                  PC save folder instead of %LOCALAPPDATA%\\<GAME> (read-only)
    --import-saves                  (--cli) copy PC saves into the SD card's saves folder
    --overwrite-saves               (--cli) replace existing 3DS saves on the SD card (backs them up first)
    --enable-wifi-send              offer "Send over Wi-Fi" (files uploaded to ftpd)
    --no-texprep                    (--cli) leave texture preparation to the 3DS (its first start takes longer)
    --toolchain-dry-run             the wizard's setup buttons only print what they would run
    --boring                       start the wizard with standard platform widgets (also for autopilot)
    --autopilot PLAN.json           drive the wizard's pages from a plan and print each page (testing)
    --registry-export FILE.reg      read Steam/GOG registry keys from an export instead of the registry (support)

Nothing is uploaded; the only downloads are the toolchain steps you agree to.
"""
from __future__ import annotations

import json
import os
import sys

# Redirected Windows terminals may use a legacy code page. Paths remain unchanged internally.
for _stream in (sys.stdout, sys.stderr):
    if _stream is not None and hasattr(_stream, 'reconfigure'):
        _stream.reconfigure(errors='backslashreplace')

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
sys.dont_write_bytecode = True

import deliver  # noqa: E402
import discover  # noqa: E402
import pipeline  # noqa: E402
import products  # noqa: E402
import texcache  # noqa: E402
import toolchain  # noqa: E402

# The public repos' launchers fix this (repo-split overlay); None offers the choice on the Welcome page.
FIXED_GAME = None


def parse(argv):
    o = {"game": FIXED_GAME, "work": None, "devkitpro": None, "folder": None, "target": "both", "cia": False,
         "sd": None, "save_dir": None, "import_saves": False, "overwrite_saves": False, "wifi": False,
         "tc_dry": False, "autopilot": None, "reg": None, "mode": "gui", "texprep": True, "boring": None,
         "chapters": None, "openh264": False}
    flags = {"--dry-run": ("mode", "dry"), "--cli": ("mode", "cli"), "--cia": ("cia", True),
             "--import-saves": ("import_saves", True), "--overwrite-saves": ("overwrite_saves", True),
             "--enable-wifi-send": ("wifi", True), "--toolchain-dry-run": ("tc_dry", True),
             "--boring": ("boring", True), "--no-texprep": ("texprep", False), "--openh264": ("openh264", True)}
    values = {"--game": "game", "--work": "work", "--devkitpro": "devkitpro", "--folder": "folder",
              "--target": "target", "--sd": "sd", "--save-dir": "save_dir", "--autopilot": "autopilot",
              "--registry-export": "reg", "--chapters": "chapters"}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        if a in flags:
            k, v = flags[a]
            o[k] = v
            i += 1
        elif a in values and i + 1 < len(argv):
            o[values[a]] = argv[i + 1]
            i += 2
        else:
            print("unknown or incomplete option: %s\n%s" % (a, __doc__), file=sys.stderr)
            sys.exit(2)
    if o["game"] and o["game"] not in products.PRODUCTS:
        print("--game must be one of: %s" % ", ".join(products.PRODUCTS), file=sys.stderr)
        sys.exit(2)
    if o["target"] not in ("n3ds", "o3ds", "both"):
        print("--target must be n3ds, o3ds or both", file=sys.stderr)
        sys.exit(2)
    if o['chapters'] is not None:
        numbers = o['chapters'].split(',')
        if not numbers or len(set(numbers)) != len(numbers) or any(n not in '12345' or len(n) != 1 for n in numbers):
            print('--chapters needs distinct chapter numbers from 1 to 5.', file=sys.stderr)
            sys.exit(2)
        if o['game'] != 'deltarune':
            print('--chapters needs --game deltarune.', file=sys.stderr)
            sys.exit(2)
        o['chapters'] = ['chapter%s_windows' % n for n in numbers]
    # devkitPro's login shell starts in its home folder, so every path handed to it must be absolute.
    for k in ("work", "devkitpro", "folder", "sd", "save_dir"):
        if o[k]:
            o[k] = os.path.abspath(o[k])
    return o


def probes_for(o):
    if o["reg"]:
        return discover.Probes(registry=discover.registry_from_export(discover.read_reg_file(o["reg"])),
                               save_dir=o["save_dir"])
    return discover.Probes(save_dir=o["save_dir"])


def dry_run(o, out=print):
    game = o["game"] or "undertale"
    work = o["work"] or pipeline.default_work_dir()
    tools_dir, downloads = os.path.join(work, "tools", "bin"), os.path.join(work, "downloads")
    probes = probes_for(o)
    out("Builder dry run for %s. Work folder: %s" % (products.PRODUCTS[game].title, work))
    try:
        v = pipeline.release_version()
        ver = "version %s, build ID %s+aot..." % (v, pipeline.expected_build_id(v))
    except pipeline.BuildFailed as e:
        ver = "cannot build: %s" % e
    out("Source tree: %s (%s)" % (pipeline.SOURCE_ROOT, ver))
    out("")
    out("Steam clients: %s" % (", ".join(probes.roots()) or "none found"))
    copies = discover.find_copies(game, probes)
    out("Copies found: %d" % len(copies))
    for c in copies:
        out("  %-12s %s  (%s)%s" % (c.store, c.folder, c.version, "  " + c.note if c.note else ""))
    out("")
    scan = deliver.scan_saves(game, probes.pc_save_dir(game))
    out("PC saves (%s): %d" % (scan.folder, len(scan.files)))
    for sf in scan.files:
        out("  %-24s %s  %s%s" % (sf.name, sf.mtime_text, sf.summary, "" if sf.valid else " [skipped: corrupt]"))
    out("")
    tc = toolchain.detect(tools_dir, roots=[o["devkitpro"]] if o["devkitpro"] else None)
    out(toolchain.dry_run_report(tc, tools_dir, downloads, want_cia=True))
    out("")
    folder = o["folder"] or (copies[0].folder if copies else "<game folder>")
    targets = ["n3ds", "o3ds"] if o["target"] == "both" else [o["target"]]
    if tc.bash or sys.platform != "win32":
        for t in targets:
            argv = pipeline.build_argv(tc, work, game, t, folder)
            out("Build command for %s (after copying the source tree to %s):" % (t, os.path.join(work, "src")))
            out("  " + " ".join(toolchain._quote(a) if "\n" not in a else "<script>" for a in argv))
        script = pipeline.BUILD_SCRIPT if sys.platform == "win32" else pipeline.UNIX_BUILD_SCRIPT
        out("  <script> = " + script.strip().replace("\n", "; "))
        out("  environment: CTRPORT_CCACHE_DIR=%s, PYTHONDONTWRITEBYTECODE=1" % os.path.join(work, "ccache"))
    out("")
    if o["texprep"]:
        tex_folder = o["folder"] or (copies[0].folder if copies else None)
        for line in texcache.dry_run_lines(products.PRODUCTS[game], tex_folder, work):
            out(line)
    else:
        out("Texture preparation on the PC: off (--no-texprep); the 3DS prepares them")
    for line in texcache.data_version_dry_run_lines(products.PRODUCTS[game],
                                                    o["folder"] or (copies[0].folder if copies else None)):
        out(line)
    return 0


def cli(o, out=print):
    """The wizard's steps without a window. Refusals print the same messages and stop."""
    game = o["game"] or "undertale"
    product = products.PRODUCTS[game]
    work = o["work"] or pipeline.default_work_dir()
    probes = probes_for(o)
    folder = o["folder"]
    if not folder:
        copies = discover.find_copies(game, probes)
        if not copies:
            out("%s was not found in your Steam libraries or GOG installs. Install it, then try again. Get it here: %s"
                % (product.title, " or ".join(product.game.store_links)))
            return 1
        folder = copies[0].folder
        out("Found: %s %s (%s)" % (copies[0].store, folder, copies[0].version))
    v = discover.verify_copy(game, folder, probes)
    out(("OK: " if v.accepted else "REFUSED: ") + v.message)
    if not v.accepted:
        return 1
    parts = v.parts
    if game == 'deltarune' and o['chapters'] is not None:
        if not set(o['chapters']) <= set(v.parts):
            out('Choose only checked, installed chapters.')
            return 1
        try:
            parts = pipeline.chapters.selection(folder, o['chapters'])
        except ValueError as exc:
            out(str(exc))
            return 1
    tc = toolchain.detect(os.path.join(work, "tools", "bin"), roots=[o["devkitpro"]] if o["devkitpro"] else None)
    for c in tc.checks:
        out("  [%s] %s: %s" % ("ok" if c.ok else "--", c.name, c.detail))
    if not tc.can_build:
        out("The build needs the missing items above. Run the wizard (or --dry-run) to see the setup steps.")
        return 1
    outputs = ["3dsx"] + (["cia"] if o["cia"] else [])
    targets = ["n3ds", "o3ds"] if o["target"] == "both" else [o["target"]]
    arts = []
    for t in targets:
        last = [-1, -1]

        def prog(p, last=last):
            if p.stage != last[0] or p.percent // 10 != last[1]:
                last[0], last[1] = p.stage, p.percent // 10
                out("  %s%s %s" % (pipeline.STAGES[p.stage], " %d%%" % p.percent if p.stage == 2 else "",
                                   pipeline.format_eta(p.estimate())))
        b = pipeline.Build(product, tc, work, t, folder, outputs=outputs, on_progress=prog,
                           parts=parts, openh264=o['openh264'])
        out("Building %s; log: %s" % (t, b.log_path))
        try:
            a, rec = b.run()
        except pipeline.BuildFailed as e:
            out(str(e))
            out("Log: %s" % e.log_path)
            return 1
        arts += a
        out("Built %s in %.1f s (%d compiler warnings)." % (t, rec["seconds"], rec["compiler_warnings"]))
        for x in a:
            out("  %s  %d bytes  sha256 %s" % (x.path, x.size, x.sha256))
    saves_res = None
    if o["sd"]:
        try:
            items = deliver.plan_sd_copy(o["sd"], product, arts, folder, parts or None)
            copied, skipped, nbytes, tex = deliver.copy_with_textures(o["sd"], items, product, folder, work,
                                                                      parts or None, log=out,
                                                                      textures=o["texprep"])
        except (deliver.DeliverError, OSError) as e:
            out(str(e))
            return 1
        out("Copied %d files (%.1f MB) to %s; %d already there." % (copied, nbytes / 1e6, o["sd"], skipped))
        out(tex.message)
        if o["import_saves"]:
            scan = deliver.scan_saves(product, probes.pc_save_dir(game))
            try:
                saves_res = deliver.copy_pc_saves(o["sd"], product, scan, overwrite=o["overwrite_saves"])
            except deliver.ExistingSavesError as e:
                out(str(e))
                out("Add --overwrite-saves to back them up and replace them.")
                return 1
            except (deliver.DeliverError, OSError) as e:
                out(str(e))
                return 1
            out("Imported %d PC save file(s) to %s." % (saves_res.copied, deliver.sd_save_dir(o["sd"], product)))
            if saves_res.backup_dir:
                out("Backed up existing 3DS saves to %s." % saves_res.backup_dir)
        if deliver.dsp_missing(o["sd"]):
            out(deliver.DSP_MESSAGE)
    else:
        try:
            saved = deliver.save_files(product, arts, folder, work, parts or None, log=out, textures=o["texprep"])
        except (deliver.DeliverError, OSError) as e:
            out(str(e))
            return 1
        out("Saved %d files (%.1f MB) to %s." % (saved.files, saved.bytes / 1e6, saved.root))
        out(saved.textures.message)
    out(deliver.instructions(product, targets, outputs, o["sd"], save_result=saves_res,
                             delivery="sd" if o["sd"] else "files"))
    return 0


def main(argv):
    if sys.version_info < (3, 9):
        print('The builder needs Python 3.9 or newer.', file=sys.stderr)
        return 2
    o = parse(argv[1:])
    if o["mode"] == "dry":
        return dry_run(o)
    if o["mode"] == "cli":
        return cli(o)
    try:
        import gui
    except ModuleNotFoundError as e:
        if e.name not in ("tkinter", "_tkinter"):
            raise
        if sys.platform == "darwin":
            help_text = "Install Python's universal2 package from python.org (it includes Tk), then use that Python."
        elif sys.platform == "win32":
            help_text = "Unpack the whole builder archive again; its Python runtime must include matching Tcl/Tk."
        else:
            help_text = "Install the system Tk package: sudo apt-get install python3-tk. Then run the builder again."
        print("The builder window needs Python's Tk library. " + help_text, file=sys.stderr)
        return 2
    plan = None
    confirm_overwrite = None
    if o["autopilot"]:
        with open(o["autopilot"], encoding="utf-8") as f:
            plan = json.load(f)
        if "overwrite_saves" in plan:
            confirm_overwrite = lambda _t, _m: bool(plan.get("overwrite_saves", False))  # noqa: E731
    return gui.run(gui.Options(game=o["game"], work=o["work"], wifi=o["wifi"], toolchain_dry_run=o["tc_dry"],
                               autopilot=plan, devkitpro=o["devkitpro"], probes=probes_for(o),
                               confirm_overwrite=confirm_overwrite, boring=o["boring"], folder=o['folder']))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
