"""An in-process ftpd on 127.0.0.1 for the builder's save-import tests (loopback only, ephemeral ports, stdlib only).

The same fake as the development tree's FTP staging tests use; never a console, and port 17491 (3dslink) is never
used.
"""
import os
from pathlib import Path
import socket
import socketserver
import threading
import time

NETLOADER_PORT = 17491  # 3dslink; a bare connect crashes the Homebrew Launcher's netloader
DISK_VERBS = {'CWD', 'SIZE', 'MKD', 'RNFR', 'RNTO', 'RMD', 'DELE'}  # each touches the card before its reply
# The steps each verb runs before 'reply', in order (a reply that refuses early skips the rest).
STEPS = dict(STOR=('accept', 'receive', 'write'), RETR=('accept', 'read+send'), MLSD=('accept', 'list'),
             **{verb: ('disk',) for verb in DISK_VERBS})
# A command's last step when it ended: answered ('reply'), or ended without an answer: 'closed' (the handler returned,
# e.g. drop_stor's dropped connections) or 'aborted' (it raised; the record's 'error' names the exception).
ENDS = ('reply', 'closed', 'aborted')


class FakeFtpd:
    """Enough of a 3DS ftpd for ftplib: USER/PASS, TYPE, PASV, MLSD, SIZE, MKD, STOR, RETR, RNFR/RNTO, DELE, QUIT.

    Fault knobs (each a {path substring: remaining count} dict, consumed per hit):
      drop_stor     read about half of the upload, then drop data and control connections (lid closed, Wi-Fi blip)
      short_stor    keep only half of the upload but answer 226 (a short write)
      corrupt_stor  flip the first byte of the upload
      perm_stor     answer 552 (card full); count -1 means always
    refuse: the next N connections are closed straight after accept.
    before_reply: called as before_reply(cmd, text) on the handler thread just before each reply is sent.
    Renaming onto an existing name fails with 550, as on the card.

    Timing (a load flake, 11-11): `trace` holds one record per command: its connection, verb, argument and the
    monotonic time each step finished ('start' when the line was read; '150' when a transfer is opened; STOR
    'accept', 'receive', 'write'; RETR 'accept', 'read+send'; MLSD 'accept', 'list'; the other card verbs 'disk';
    then 'reply' once it is sent). A command the handler left without an answer ends 'closed' (it returned) or
    'aborted' (it raised, record['error'] names the exception), never 'reply'.
    describe_wait() names the step a client waited on.
    """

    def __init__(self, root, banner='220 fake ftpd'):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.cmds, self.refuse, self.connections = [], 0, 0
        self.drop_stor, self.short_stor, self.corrupt_stor, self.perm_stor = {}, {}, {}, {}
        self.getmtime = True
        self.banner = banner
        self.missing_reply = '550 none'
        self.mkd_exists_reply = '550 exists'  # ftpd v2.3.1 sends '250 OK' when the folder already exists.
        self.after_retr = self.after_rename = self.before_reply = None
        self.trace = []
        self.lock = threading.Lock()
        outer = self

        class Handler(socketserver.StreamRequestHandler):
            record = None

            def step(self, name):
                if self.record is not None:
                    self.record['steps'].append((name, time.monotonic()))

            def reply(self, s):
                record = self.record
                if record is not None and record['cmd'] in DISK_VERBS and len(record['steps']) == 1:
                    self.step('disk')
                if outer.before_reply:
                    outer.before_reply(record and record['cmd'], s)
                self.wfile.write((s + "\r\n").encode())
                self.wfile.flush()
                self.step('150' if s.startswith('150') else 'reply')  # a 150 opens the transfer; it is no answer

            def handle(self):
                try:
                    self.serve()
                except BaseException as exc:
                    self.end('aborted', type(exc).__name__)
                    raise
                self.end('closed')

            def end(self, name, error=None):
                # the connection's last command, if it was not answered, ends here (under the lock: describe_wait
                # copies the steps under it)
                with outer.lock:
                    if self.record is not None and self.record['steps'][-1][0] != 'reply':
                        self.record['steps'].append((name, time.monotonic()))
                        if error:
                            self.record['error'] = error

            def serve(self):
                with outer.lock:
                    outer.connections += 1
                    connection = outer.connections
                    if outer.refuse > 0:
                        outer.refuse -= 1
                        return
                self.reply(outer.banner)
                pasv, rnfr = None, None
                cwd, restart = '/', 0
                while True:
                    try:
                        line = self.rfile.readline().decode()
                    except OSError:
                        return
                    if not line:
                        return
                    cmd, _, arg = line.strip().partition(" ")
                    cmd = cmd.upper()
                    self.record = dict(connection=connection, cmd=cmd, arg=arg, steps=[('start', time.monotonic())])
                    with outer.lock:
                        outer.cmds.append((cmd, arg))
                        outer.trace.append(self.record)
                    local = outer.local(arg)
                    if cmd == "USER":
                        self.reply("331 ok")
                    elif cmd == "PASS":
                        self.reply("230 ok")
                    elif cmd in ("TYPE", "NOOP"):
                        self.reply("200 ok")
                    elif cmd == 'PWD':
                        self.reply('257 "%s"' % cwd)
                    elif cmd == 'CWD':
                        if local.is_dir():
                            cwd = arg
                            self.reply('250 changed')
                        else:
                            self.reply('550 no such folder')
                    elif cmd == 'REST':
                        restart = int(arg)
                        self.reply('350 restart position accepted')
                    elif cmd == "OPTS":
                        self.reply("200 MLST facts selected")
                    elif cmd == "PASV":
                        pasv = socket.socket()
                        pasv.bind(("127.0.0.1", 0))
                        pasv.listen(1)
                        port = pasv.getsockname()[1]
                        self.reply("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (port >> 8, port & 255))
                    elif cmd == "MLSD":
                        if not local.is_dir():
                            pasv.close()
                            self.reply("550 No such file or directory")  # ftpd: 550 and strerror(ENOENT).
                            continue
                        self.reply("150 listing")
                        conn, _ = pasv.accept()
                        self.step('accept')
                        for p in sorted(local.iterdir()):
                            kind = "dir" if p.is_dir() else "file"
                            size = ";size=%d" % p.stat().st_size if kind == "file" else ""
                            ns = p.stat().st_mtime_ns
                            stamp = time.strftime('%Y%m%d%H%M%S', time.gmtime(ns // 10**9)) + '.%09d' % (ns % 10**9)
                            if not outer.getmtime:
                                stamp = '19700101000000'
                            conn.sendall(("type=%s%s;modify=%s; %s\r\n" % (kind, size, stamp, p.name)).encode())
                        conn.close()
                        pasv.close()
                        self.step('list')
                        self.reply("226 done")
                    elif cmd == "SIZE":
                        self.reply("213 %d" % local.stat().st_size if local.is_file() else outer.missing_reply)
                    elif cmd == "MKD":
                        if local.exists():
                            self.reply(outer.mkd_exists_reply)
                        elif not local.parent.is_dir():
                            self.reply("550 no parent")
                        else:
                            local.mkdir()
                            self.reply('257 "%s" created' % arg)
                    elif cmd == "STOR":
                        if outer.take(outer.perm_stor, arg):
                            pasv.close()
                            self.reply("552 card full")
                            continue
                        if local.is_dir() or not local.parent.is_dir():
                            pasv.close()
                            self.reply("550 bad path")
                            continue
                        self.reply("150 send")
                        conn, _ = pasv.accept()
                        self.step('accept')
                        drop = outer.take(outer.drop_stor, arg)
                        data = bytearray(local.read_bytes()[:restart]) if restart and local.is_file() else bytearray()
                        restart = 0
                        while True:
                            b = conn.recv(65536)
                            if not b:
                                break
                            data += b
                            if drop and len(data) >= 4096:
                                break
                        self.step('receive')
                        if drop:
                            local.write_bytes(bytes(data[:len(data) // 2]))
                            conn.close()
                            pasv.close()
                            return                   # control connection dropped, no reply
                        if outer.take(outer.short_stor, arg):
                            data = data[:len(data) // 2]
                        if data and outer.take(outer.corrupt_stor, arg):
                            data[0] ^= 0xFF
                        local.write_bytes(bytes(data))
                        self.step('write')
                        conn.close()
                        pasv.close()
                        self.reply("226 stored")
                    elif cmd == "RETR":
                        if not local.is_file():
                            pasv.close()
                            self.reply("550 none")
                            continue
                        self.reply("150 sending")
                        conn, _ = pasv.accept()
                        self.step('accept')
                        conn.sendall(local.read_bytes())
                        self.step('read+send')
                        if outer.after_retr:
                            outer.after_retr(arg)
                        conn.close()
                        pasv.close()
                        self.reply("226 sent")
                    elif cmd == "RNFR":
                        rnfr = local if local.exists() else None
                        self.reply("350 ready" if rnfr else "550 none")
                    elif cmd == "RNTO":
                        if rnfr is None or local.exists():
                            self.reply("550 cannot rename")
                        else:
                            os.rename(rnfr, local)
                            if outer.after_rename:
                                outer.after_rename(arg)
                            self.reply("250 renamed")
                        rnfr = None
                    elif cmd == "RMD":
                        try:
                            local.rmdir()
                        except OSError:
                            self.reply("550 cannot remove directory")
                        else:
                            self.reply("250 removed directory")
                    elif cmd == "DELE":
                        if local.is_file():
                            local.unlink()
                            self.reply("250 deleted")
                        else:
                            self.reply(outer.missing_reply)
                    elif cmd == "QUIT":
                        self.reply("221 bye")
                        return
                    else:
                        self.reply("502 not here")

        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        assert self.port != NETLOADER_PORT
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def local(self, arg):
        return self.root.joinpath(*[p for p in arg.split("/") if p])

    def take(self, knob, path):
        with self.lock:
            for key, left in knob.items():
                if key in path and left != 0:
                    knob[key] = left - 1 if left > 0 else left
                    return True
        return False

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def describe_wait(self, began, now, timeout):
        """Why a client that began waiting at `began` (monotonic) gave up at `now` after `timeout` s: each command
        still running on the server at `now` and the step it was in, the recent commands' step times, and how late the
        waiting thread itself was (now - began - timeout: near 0 when only the server was slow).
        One snapshot of the trace, classified as it stood at `now`: a step the server took after `now` (between the
        timeout and this call) never counts as done by then; it is shown as after the timeout."""
        with self.lock:
            records = [dict(r, steps=list(r['steps'])) for r in self.trace]
        lines = ['fake ftpd: the client waited %.3f s (timeout %s s); its own thread was %.3f s late'
                 % (now - began, timeout, now - began - (timeout or 0))]
        for r in records:
            r['by'] = [step for step in r['steps'] if step[1] <= now]      # the steps done at the timeout
            r['later'] = [step for step in r['steps'] if step[1] > now]    # and the ones after it
        running = [r for r in records if r['by'] and r['by'][-1][0] not in ENDS and r['cmd'] != 'QUIT']
        unanswered = [r for r in records if r['by'] and r['by'][-1][0] in ('closed', 'aborted')]
        for r in running:
            done = [name for name, _ in r['by']]
            waiting = next((name for name in STEPS.get(r['cmd'], ()) if name not in done), 'reply')
            later = ''
            if r['later']:
                name, at = r['later'][-1]
                later = ('; it ended after the timeout (%s %.3f s later)%s' % (
                    name, at - now, ' (%s)' % r['error'] if r.get('error') else '') if name in ENDS
                    else '; then %s %.3f s after the timeout' % (name, at - now))
            lines.append('  STILL RUNNING conn %d %s %s: in step %s for %.3f s (after %s)%s'
                         % (r['connection'], r['cmd'], r['arg'], waiting, now - r['by'][-1][1], done[-1], later))
        for r in unanswered[-4:]:
            ended = r['by'][-1][1]
            lines.append('  ENDED WITHOUT AN ANSWER conn %d %s %s: %s after %s, %.3f s %s the wait began%s' % (
                r['connection'], r['cmd'], r['arg'], r['by'][-1][0], r['by'][-2][0], abs(ended - began),
                'before' if ended < began else 'after', ' (%s)' % r['error'] if r.get('error') else ''))
        for r in [r for r in records if r['steps'][-1][1] >= began - 1.0][-4:] or records[-1:]:
            spans = [(name, at - r['steps'][i][1]) for i, (name, at) in enumerate(r['steps'][1:])]
            slowest = max(spans, key=lambda x: x[1]) if spans else None
            lines.append('  conn %d %s %s: %s%s' % (
                r['connection'], r['cmd'], r['arg'], ', '.join('%s %.3f s' % x for x in spans) or 'no step yet',
                '; slowest %s %.3f s' % slowest if slowest else ''))
        if not running:
            if any(r['by'][-1][1] >= began for r in unanswered):
                lines.append('  nothing still running: a command ended without an answer while the client waited')
            elif unanswered:
                lines.append('  nothing still running: every command since the wait began was answered (the ones'
                             ' that ended without an answer ended before it), so the waiting thread was late')
            else:
                lines.append('  nothing still running: the server had answered, so the waiting thread was late')
        return '\n'.join(lines)

    def written(self):
        """Paths given to STOR, RNTO and DELE."""
        return [(c, a) for c, a in self.cmds if c in ("STOR", "RNTO", "DELE", "RNFR")]
