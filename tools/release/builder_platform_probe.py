#!/usr/bin/env python3
"""Platform test kit. No installs, Steam writes, SD-card access or console connections.

    python3 -I tools/release/builder_platform_probe.py
    python3 -I tools/release/builder_platform_probe.py --build [--folder STEAM_INSTALL]

--build runs the real verifier, AOT compiler, .3dsx/.cia packaging and delivery to a new scratch SD folder.
Return only the printed support log; other scratch files include your personal build and game data.
"""
import argparse
import datetime
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import traceback

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE / "builder"))
import builder
import pipeline
import toolchain


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", action="store_true")
    p.add_argument("--folder")
    p.add_argument("--no-window", action="store_true", help="headless Linux: report Tk import only")
    o = p.parse_args()
    scratch = ROOT / "scratch"
    scratch.mkdir(exist_ok=True)
    log = scratch / ("builder-platform-%s.log" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    lines = []
    failed = False
    support_filter = pipeline.SupportLogFilter()

    def emit(text):
        text = support_filter(str(text)).replace("%USERPROFILE%", "<home>")
        text = text.replace(str(ROOT), "<source>").replace(ROOT.as_posix(), "<source>")
        lines.append(text)
        log.write_text("".join(lines), encoding="utf-8")
        print(text, end="", flush=True)

    def command(argv, timeout=60):
        nonlocal failed
        emit("RUN " + " ".join(argv))
        try:
            r = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, errors="replace", timeout=timeout)
            emit(r.stdout)
            emit("exit %d" % r.returncode)
            failed |= r.returncode != 0
        except (OSError, subprocess.SubprocessError) as exc:
            failed = True
            emit(str(exc))

    emit("Builder platform kit v1: %s %s; Python %s" % (sys.platform, platform.machine(), platform.python_version()))
    try:
        import tkinter
        emit("Tk import: %s" % tkinter.TkVersion)
        if not o.no_window:
            window = tkinter.Tk()
            window.title("Builder platform check")
            window.update()
            emit("Tk window created; Tcl/Tk %s" % window.tk.call("info", "patchlevel"))
            window.destroy()
    except Exception as exc:
        failed = True
        emit("Tk check failed: %s" % exc)
    command([sys.executable, "-I", str(HERE / "test_builder_platforms.py")])
    command([sys.executable, "-I", str(HERE / "builder" / "builder.py"), "--game", "undertale", "--dry-run"])
    if sys.platform == "darwin":
        command(["/usr/bin/xcode-select", "-p"])
        for name in ("devkitARM/bin/arm-none-eabi-gcc", "tools/bin/picasso", "pacman/bin/pacman"):
            path = "/opt/devkitpro/" + name
            command(["/usr/bin/file", path])
        # A working compiler invocation plus file(1) records native vs Rosetta execution.
        command(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc", "--version"])
    if o.build:
        work = Path(tempfile.mkdtemp(prefix="builder-platform-", dir=scratch))
        sd = work / "sd"
        (sd / "3ds").mkdir(parents=True)
        emit("Real AOT + CIA build, simulated SD target; personal output retained in %s" % work)
        opts = builder.parse(["--cli", "--game", "undertale", "--cia", "--work", str(work), "--sd", str(sd)] +
                             (["--folder", o.folder] if o.folder else []))
        try:
            # Reuse an existing builder's locally downloaded tools without copying anything into devkitPro.
            existing = Path(pipeline.default_work_dir()) / "tools" / "bin"
            local = work / "tools" / "bin"
            if existing.is_dir():
                import shutil
                shutil.copytree(existing, local)
            rc = builder.cli(opts, out=emit)
            failed |= rc != 0
            emit("REAL BUILD exit %d" % rc)
        except Exception:
            failed = True
            emit(traceback.format_exc())
    emit("RESULT: %s" % ("FAIL / prerequisites missing; read the log" if failed else "PASS"))
    print("Return this log only: %s" % log)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
