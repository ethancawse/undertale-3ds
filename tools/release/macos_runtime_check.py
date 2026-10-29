"""Opt-in native Mac retest diagnostic. No network, installation, game inputs or console access."""
from pathlib import Path
import hashlib
import json
import multiprocessing
import os
import platform
import ssl
import subprocess
import sys


def worker(queue):
    queue.put(dict(executable=sys.executable, isolated=sys.flags.isolated,
                   digest=hashlib.sha256(b'builder worker fixture').hexdigest()))


def main():
    resources = Path(__file__).resolve().parent
    arch = platform.machine()
    runtime = resources / 'runtime' / arch
    assert sys.platform == 'darwin'
    assert Path(sys.executable).resolve().is_relative_to(runtime.resolve())
    assert sys.flags.isolated and sys.flags.no_user_site and sys.dont_write_bytecode
    assert all(not p or Path(p).resolve().is_relative_to(runtime.resolve()) for p in sys.path)
    assert ssl.create_default_context().cert_store_stats()['x509_ca'] > 0
    child = subprocess.run([sys.executable, '-I', '-B', '-c',
                            'import json,ssl,sys; print(json.dumps([sys.executable,sys.flags.isolated,ssl.OPENSSL_VERSION]))'],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
    assert child.returncode == 0, child.stderr
    context = multiprocessing.get_context('spawn')
    queue = context.Queue()
    process = context.Process(target=worker, args=(queue,))
    process.start()
    try:
        result = queue.get(timeout=30)
        process.join(timeout=30)
        assert process.exitcode == 0 and result['executable'] == sys.executable
        assert result['digest'] == hashlib.sha256(b'builder worker fixture').hexdigest()
        assert result['isolated'] == 1
    finally:
        if process.is_alive():
            process.terminate()  # This exact multiprocessing child only.
            process.join(timeout=10)
        queue.close()
        queue.join_thread()
    import tkinter
    import _tkinter
    root = tkinter.Tk()
    root.withdraw()
    try:
        tk = dict(tcl=root.tk.call('info', 'patchlevel'), tk=root.tk.call('package', 'require', 'Tk'),
                  window_system=root.tk.call('tk', 'windowingsystem'),
                  module=_tkinter.__file__, tcl_library=root.tk.call('info', 'library'))
        assert tk['window_system'] == 'aqua'
        assert Path(tk['tcl_library']).resolve().is_relative_to(runtime.resolve())
    finally:
        root.destroy()
    report = dict(passed=True, executable=sys.executable, python=sys.version, arch=arch,
                  ssl=ssl.OPENSSL_VERSION, certificate_file=os.environ.get('SSL_CERT_FILE'),
                  child=json.loads(child.stdout), worker=result, tkinter=tk)
    logs = Path.home() / 'Library/Logs/3DS Builder'
    logs.mkdir(parents=True, exist_ok=True)
    (logs / 'runtime-check.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
