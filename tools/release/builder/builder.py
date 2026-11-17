#!/usr/bin/env python3
"""The builder app: turns your own UNDERTALE (or DELTARUNE) into a personal 3DS build on this PC.

    builder.py                      the wizard (window)
    builder.py --dry-run            print every detection result, URL, pinned hash and command it would run; changes
                                    nothing (the support tool)
    builder.py --cli --sd {sd_example}  the same steps without a window

Options:
    --game undertale|deltarune      fix the game (each public repo's launcher passes its own)
    --work DIR                      work folder (default {work_default})
    --devkitpro DIR                 devkitPro folder (default {devkitpro_default})
    --folder DIR                    game folder instead of searching (supported game-file checks still apply)
    --target n3ds|o3ds|both         (--cli) console model, default both (New and Old 3DS)
    --cia                           (--cli) also make a .cia
    --chapters 1,3                  (--cli) DELTARUNE chapters to build; default all checked chapters
    --openh264                      (--cli) build the software video decoder
    --sd DIR                        (--cli) SD card to copy to; without it the build stays in <work>/out
    --save-dir DIR                  PC save folder (default for {game}: {save_default}; read-only)
    --import-saves                  (--cli) copy PC saves to the 3DS (with --sd or --ftp)
    --overwrite-saves               (--cli) replace existing 3DS saves (backs them up first)
    --ftp IP:PORT                  (--cli) install over Wi-Fi using ftpd (default port 5000)
    --enable-wifi-send              old option, no longer needed (Wi-Fi is always offered)
    --no-texprep                    (--cli) leave texture preparation to the 3DS (its first start takes longer)
    --toolchain-dry-run             the wizard's setup buttons only print what they would run
    --boring                       start the wizard with standard platform widgets (also for autopilot)
    --autopilot PLAN.json           drive the wizard's pages from a plan and print each page (testing)
    --registry-export FILE.reg      read Steam/GOG registry keys from an export instead of the registry (support)

Wi-Fi sends only to the 3DS you choose. Toolchain downloads ask first.
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


def help_text(platform=None, env=None, game=None):
    """Use the same defaults as the build and save discovery, including environment overrides."""
    import saves
    platform = sys.platform if platform is None else platform
    env = os.environ if env is None else env
    game = FIXED_GAME or game or 'undertale'
    if game not in products.PRODUCTS:
        game = 'undertale'
    values = dict(sd_example='E:\\' if platform == 'win32' else
                  '/Volumes/SD' if platform == 'darwin' else '/media/SD',
                  work_default=pipeline.default_work_dir(env=env, platform=platform),
                  devkitpro_default=toolchain.candidate_roots(env=env, platform=platform)[0],
                  game=products.PRODUCTS[game].title,
                  save_default=saves.default_pc_save_dir(game, env=env, platform=platform))
    text = __doc__
    for name, value in values.items():
        text = text.replace('{' + name + '}', value)
    return text


def parse(argv):
    o = {"game": FIXED_GAME, "work": None, "devkitpro": None, "folder": None, "target": "both", "cia": False,
         "sd": None, "ftp": None, "save_dir": None, "import_saves": False, "overwrite_saves": False, "wifi": False,
         "tc_dry": False, "autopilot": None, "reg": None, "mode": "gui", "texprep": True, "boring": None,
         "chapters": None, "openh264": False}
    flags = {"--dry-run": ("mode", "dry"), "--cli": ("mode", "cli"), "--cia": ("cia", True),
             "--import-saves": ("import_saves", True), "--overwrite-saves": ("overwrite_saves", True),
             "--enable-wifi-send": ("wifi", True), "--toolchain-dry-run": ("tc_dry", True),
             "--boring": ("boring", True), "--no-texprep": ("texprep", False), "--openh264": ("openh264", True)}
    values = {"--game": "game", "--work": "work", "--devkitpro": "devkitpro", "--folder": "folder",
              "--target": "target", "--sd": "sd", "--save-dir": "save_dir", "--autopilot": "autopilot",
              "--registry-export": "reg", "--chapters": "chapters", "--ftp": "ftp"}
    i = 0
    show_help = False
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            show_help = True
            i += 1
            continue
        if a in flags:
            k, v = flags[a]
            o[k] = v
            i += 1
        elif a in values and i + 1 < len(argv):
            o[values[a]] = argv[i + 1]
            i += 2
        else:
            print("unknown or incomplete option: %s\n%s" % (a, help_text(game=o['game'])), file=sys.stderr)
            sys.exit(2)
    if show_help:
        print(help_text(game=o['game']))
        sys.exit(0)
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
    if o['ftp']:
        import ftp_delivery
        try:
            host, port = ftp_delivery.endpoint(o['ftp'])
            o['ftp'] = '%s:%d' % (host, port)
            if o['sd']:
                raise ValueError('Use --sd or --ftp, not both.')
        except (OSError, ValueError) as exc:
            print(str(exc), file=sys.stderr)
            sys.exit(2)
    return o


def probes_for(o):
    if o["reg"]:
        return discover.Probes(registry=discover.registry_from_export(discover.read_reg_file(o["reg"])),
                               save_dir=o["save_dir"])
    return discover.Probes(save_dir=o["save_dir"])


def source_line(game):
    """What the build would say about its source: the release kit's verified link, a source build, or why a build
    can't start (B1: the dry run used to call a source checkout fine, then every build stopped)."""
    try:
        source = pipeline.release_source(pipeline.SOURCE_ROOT, game)
    except pipeline.BuildFailed as exc:
        return "Source: CANNOT BUILD. %s" % exc
    if source.get('kind') == 'source':
        return pipeline.source_build_text(source)
    return "Source: release builder for %s; its Legal page shows the release tag." % source['source_url']


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
    out(source_line(game))
    out("")
    out("Steam clients: %s" % (", ".join(probes.roots()) or "none found"))
    copies = discover.find_copies(game, probes)
    out("Copies found: %d" % len(copies))
    for c in copies:
        out("  %-12s %s  (%s)%s" % (c.store, c.folder, c.version, "  " + c.note if c.note else ""))
    out("")
    scan = deliver.scan_saves(game, probes.pc_save_dir(game))
    out(scan.error or "PC saves (%s): %d" % (scan.folder, len(scan.files)))
    for sf in scan.files:
        out("  %-24s %s  %s%s" % (sf.name, sf.mtime_text, sf.summary, "" if sf.valid else " [skipped: unreadable]"))
    out("")
    tc = toolchain.detect(tools_dir, roots=[o["devkitpro"]] if o["devkitpro"] else None)
    out(toolchain.dry_run_report(tc, tools_dir, downloads, want_cia=True))
    out("")
    folder = o["folder"] or (copies[0].folder if copies else "<game folder>")
    if game == 'undertale':
        folder = os.path.join(work, 'data', products.UNDERTALE_AOT_FOLDER)
        out('AOT input: %s (a private copy; your game folder is unchanged)' % folder)
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
    if game == "undertale" and "o3ds" in targets:
        problem = pipeline.sfx_pack_problem()
        out(pipeline.SFX_PACK_MISSING if problem else "Sound pack for the Old 3DS: made on this PC")
        if problem:
            out("Sound preparation: " + problem)
    return 0


# --target both, and the second model failed (B7): the first one is fine, and this is how to copy only it.
BUILT_BEFORE_FAILURE = ("The %s build (%s) finished, but nothing was copied. To copy only it, run the same command "
                        "with --target %s; most of its work is reused.")
MODEL = {"n3ds": "New 3DS", "o3ds": "Old 3DS"}


# The window's messages name its buttons. The command line has none, so it says what to do there instead (B2).
CLI_WORDING = (
    ("Choose Try again, or Copy log for support.",
     "Run the same command again, or report the error with the log named below."),
    ("choose Send again", "run the same command again"),
    ("choose Copy to card again", "run the same command again"),
    ("choose Next again", "run the same command again"),
    ("choose Try again", "run the same command again"),
    ("Choose Try again", "Run the same command again"),
    ("choose Check again", "run the same command again"),
    ("or Skip.", "or skip save import."),
    ("Click Check again on the compiler page", "Run the same command again"),
    ("Click Update devkitPro. It brings all of devkitPro's packages up to date and then checks again.\nIf it can't "
     "finish, ", "To fix it, "),
    ("Click Update devkitPro to try again. If it still stops, ", "To fix it, "),
    ("untick everything except 3DS Development.", "leave only 3DS Development selected."),
    ("Click Check again to test it.", "Run the same command again to test it."),
    ("click Check again on the compiler page", "run the same command again"),
    ("Use Install beside the 3DS tools on the compiler page. "
     "Or click Try again and tick 3DS Development in the installer.",
     "Run the devkitPro installer again and select 3DS Development, then run the same command again."),
    ("Click Check again. If devkitPro is still missing, click Try again "
     "and select Minimal System and 3DS Development in the installer.",
     "Run the same command again. If devkitPro is still missing, run its installer again "
     "and select Minimal System and 3DS Development."),
    ("Click Copy log to report the error.", "Report the error with the log named below."),
    ("Go back to choose the game files.", "Then run the same command again."),
    ("Click Try again and tick 3DS Development in the installer.",
     "Run the devkitPro installer again and select 3DS Development."),
    ("Or use Install beside the 3DS tools on the compiler page.",
     "Then install the 3DS compiler using the builder's setup steps."),
    ("then click Try again.", "then run the same command again."),
    ("then click Check again.", "then run the same command again."),
    ("Then click Check again.", "Then run the same command again."),
    ("Click Update devkitPro to refresh it", "Update devkitPro to refresh it"),
    ("Click Try again, or Check again on the compiler page.", "Run the same command again."),
    ("Click Try again, or click Copy log to report the error.",
     "Run the same command to try again, or report the error with the log named below."),
    ("Click Try again when ready.", "Run the same command again when ready."),
    ("Click Copy log to report the error, then try again with a repaired builder.",
     "Report the error with the log named below, then try again with a repaired builder."),
    ("Untick .cia on Options or set up the .cia tools.",
     "Leave out --cia, or run this builder with --dry-run to see the builder's setup steps and .cia availability."),
    ("Click Update devkitPro on the compiler page, then try the build again.",
     "Update devkitPro, then run the same build command again. Use --dry-run to see the builder's setup steps."),
    ("Click Copy to card to try again.", "Run the same command to copy again."),
    ("Click Save files to try again.", "Run the same command to save them again."),
    ("then click Send to pick up where it left off.", "then run the same command to pick up where it left off."),
    ("then click Send.", "then run the same command again."),
    ("Click Next to finish copying.", "Run the same command again to finish copying."),
    ("Click Back, then Next, to check them again.", "Run the same command again to check them again."),
)


def cli_text(text):
    for window, command_line in CLI_WORDING:
        text = text.replace(window, command_line)
    return text


def cli(o, out=print):
    """The wizard's steps without a window. Refusals print the same messages, in command-line wording, and stop."""
    out = (lambda write: lambda text: write(cli_text(text)))(out)
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
    folder = v.result.folder
    parts = v.parts
    if game == 'deltarune' and o['chapters'] is not None:
        if not set(o['chapters']) <= set(v.parts):
            out('Choose only checked, installed chapters.')
            return 1
        try:
            import chapters
            parts = chapters.selection(folder, o['chapters'])
        except ValueError as exc:
            out(str(exc))
            return 1
    # The first step that touches the work folder: another builder using it must finish first (AUD-15).
    try:
        lease = pipeline.claim_work(work, devkitpro=o['devkitpro'])
    except pipeline.WorkLockError as exc:
        out(str(exc))
        return 1
    try:
        return _build_and_deliver(o, out, game, product, work, probes, folder, v, parts)
    finally:
        lease.release()


def _build_and_deliver(o, out, game, product, work, probes, folder, v, parts):
    # A source that can't build stops before the toolchain checks and the build (B1). A source checkout says so.
    try:
        source = pipeline.release_source(pipeline.SOURCE_ROOT, game)
    except pipeline.BuildFailed as exc:
        out(str(exc))
        return 1
    if source.get('kind') == 'source':
        out(pipeline.source_build_text(source))
    tc = toolchain.detect(os.path.join(work, "tools", "bin"), roots=[o["devkitpro"]] if o["devkitpro"] else None)
    toolchain.check_build_tools(tc)
    for c in tc.checks:
        out("  [%s] %s: %s" % ("ok" if c.ok else "--", c.name, c.detail))
        if c.technical:
            out(c.technical)
    if not tc.can_build:
        broken = toolchain.broken_tools_text(tc.checks, tc.root, window=False)
        out(broken or "The build needs the missing items above. Run the wizard (or --dry-run) to see the setup steps.")
        return 1
    outputs = ["3dsx"] + (["cia"] if o["cia"] else [])
    targets = ["n3ds", "o3ds"] if o["target"] == "both" else [o["target"]]
    shown = set()
    if game == "undertale" and "o3ds" in targets:
        problem = pipeline.sfx_pack_problem()
        if problem:       # Said now, before the long build, and again from the record once the Old build ends.
            out(pipeline.SFX_PACK_MISSING)
            out("Sound preparation: " + problem)
    if source.get('kind') == 'source':
        shown.add(pipeline.source_build_text(source))
    arts = []
    for t in targets:
        last = [-1, -1, None]

        def prog(p, last=last):
            if p.stage != last[0] or p.percent // 10 != last[1] or p.activity != last[2]:
                last[:] = p.stage, p.percent // 10, p.activity
                out("  %s - %d%%" % (p.activity, int(round(p.completion()))))
        try:
            b = pipeline.Build(product, tc, work, t, folder, outputs=outputs, on_progress=prog, store=v.result.store, parts=parts, openh264=o['openh264'])
            out("Building %s; log: %s" % (t, b.log_path))
            a, rec = b.run()
        except pipeline.BuildFailed as e:
            out(str(e))
            if e.log_path:
                cause = pipeline.first_error(e.log_path)
                if cause:
                    out("What failed: %s" % cause)
                out("Log: %s" % e.log_path)
            if t != targets[0]:
                out(BUILT_BEFORE_FAILURE % (MODEL[targets[0]], targets[0], targets[0]))
            return 1
        arts += a
        out("Built %s in %.1f s (%d compiler warnings)." % (t, rec["seconds"], rec["compiler_warnings"]))
        for x in a:
            out("  %s  %d bytes  sha256 %s" % (x.path, x.size, x.sha256))
        # The record's notices (no sound pack, and so on) reach the command line too. The source-build line was
        # already printed before the toolchain checks.
        for notice in rec.get("notices", ()):
            if notice not in shown:
                shown.add(notice)
                out(notice)

    def delivery_log(text):
        # Keep technical texture diagnostics in the last target's build log. Delivery also returns a plain
        # first-start notice when PC texture preparation isn't available.
        with open(b.log_path, "a", encoding="utf-8", newline="\n") as log:
            log.write(text + "\n")
        if not text.startswith(("texprep:", "building texprep from source")):
            out(text)

    saves_res = None
    if o.get('ftp'):
        import ftp_delivery
        import ftp_saves
        import preferences
        try:
            saved = ftp_delivery.send_product(o['ftp'], product, arts, folder, work, parts or None, log=delivery_log)
            out(saved.textures.message)
            preferences.save(work, preferences.load(work)['boring'], console=o['ftp'])
            if o['import_saves']:
                existing = ftp_saves.inspect(o['ftp'], product)
                scan = deliver.scan_saves(product, probes.pc_save_dir(game))
                saves_res = ftp_saves.copy(o['ftp'], product, scan, existing, overwrite=o['overwrite_saves'])
        except (OSError, __import__('ftplib').Error) as exc:
            out(ftp_delivery.error_text(exc))
            return 1
    elif o["sd"]:
        try:
            items = deliver.plan_sd_copy(o["sd"], product, arts, folder, parts or None)
            copied, skipped, nbytes, tex = deliver.copy_with_textures(o["sd"], items, product, folder, work,
                                                                      parts or None, log=delivery_log,
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
            saved = deliver.save_files(product, arts, folder, work, parts or None, log=delivery_log, textures=o["texprep"])
        except (deliver.DeliverError, OSError) as e:
            out(str(e))
            return 1
        out("Saved %d files (%.1f MB) to %s." % (saved.files, saved.bytes / 1e6, saved.folder or saved.root))
        out(saved.textures.message)
    out(deliver.instructions(product, targets, pipeline.available_outputs(arts), o["sd"], save_result=saves_res,
                             delivery="wifi" if o.get('ftp') else "sd" if o["sd"] else "files"))
    return 0


def bundled_mac_runtime():
    """The app bootstrap runs this entry with the embedded Resources/runtime Python."""
    from pathlib import Path
    executable = Path(sys.executable).resolve()
    for parent in Path(HERE).resolve().parents:
        if (parent.name == 'Resources' and parent.parent.name == 'Contents'
                and parent.parent.parent.suffix == '.app'):
            return executable.is_relative_to(parent / 'runtime')
    return False


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
        if sys.platform == "darwin" and bundled_mac_runtime():
            help_text = ("The builder's window files are missing. Download and unpack the complete Mac builder again, "
                         "then open %s 3DS Builder.app." % products.PRODUCTS[o["game"] or "undertale"].title)
        else:
            help_text = "The builder window needs Python's Tk library. " + help_text
        print(help_text, file=sys.stderr)
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
