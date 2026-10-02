#!/usr/bin/env python3
"""wiibench.py -- one queue for the Wii on the bench, shared by every project and agent.

The Wii (default 192.168.8.213) sits in the Homebrew Channel. Only one thing can use it at
a time, so work goes through this queue instead of straight to the Wii:

  python tools/wii-bench/wiibench.py add [--name N] [--cwd DIR] [--timeout S] [--agent A] -- CMD ARGS...
      queue a command (run in DIR, default the current directory) and start the dispatcher
      if none is running. Prints the job id. The command talks to the Wii itself (wiiload,
      e.g. WiiStation's scripts/wii_lab.py); it gets WII_BENCH_IP in its environment.
      --agent (else $WII_BENCH_AGENT, else the Claude Code session) names who queued it.
  python tools/wii-bench/wiibench.py wait ID      block until the job is done; print the
                                                  end of its log; exit with its exit code;
                                                  once a minute, say why not yet (stderr)
  python tools/wii-bench/wiibench.py status       the queue, the job running, the Wii now
  python tools/wii-bench/wiibench.py cancel ID    remove a job that has not started
  python tools/wii-bench/wiibench.py run          the dispatcher (add starts it for you)
  python tools/wii-bench/wiibench.py serve        the lease server shared by every workstation
  python tools/wii-bench/wiibench.py setup [--server URL]   make this workstation a client
  python tools/wii-bench/wiibench.py report [--hours N | --days N | --since DATE] [--local]
                                                  the queue's history: every workstation's
                                                  from the server, or this one's

Several workstations share the Wii through a lease server (`serve`, run in Docker on the
homeserver): each keeps its own queue and dispatcher, and a dispatcher takes the lease before
each job. The server's URL is $WII_BENCH_SERVER, else the file HOME/server; with neither the
dispatcher runs alone, as before.

The dispatcher runs one job at a time, oldest first, and only when HBC's own menu has
answered on TCP 4299 for --idle seconds in a row (default 20). Apps that link hbc-reborn's
in-app agent answer on 4299 too, so the probe asks for the version: an agent app's reply ends
in " agent", and counts as busy. A Wii that is running anything -- a job, or another
workstation's test that did not use this queue -- is left alone, and a quiet period keeps
the dispatcher from cutting in between another agent's back-to-back runs. Right after a
queue job (ours, or the lease's last holder's, which released it cleanly) we know why the
Wii was busy, so QUICK seconds of idle do; any probe that finds it busy brings back the full
wait. The same agent's next job runs in the same turn after one probe, but only while no other
workstation waits: with someone in line every job ends the turn. With nobody waiting the
lease is held CHAIN_GRACE seconds for the agent's next add, let go within a second of someone
joining. After each job the dispatcher, still holding the Wii, checks that HBC came back
(RETURN_WAIT s) and says so in the job's record, the release and the history. The queue never
contacts the Wii while a job runs: it probes only between jobs, and `status` reports who has
the Wii instead of probing it during a run. `report` reads the history. Each job gets
WII_BENCH_JOB_START (Unix time), and tools/hbc.py will not exit an agent app started before
it. A job that runs past its timeout (default 1800 s) is stopped (its own process tree).
The dispatcher exits when the queue has been empty for 10 minutes; the next add starts it.

State lives in one directory shared by every copy of this script, so every project uses
the same queue: $WII_BENCH_HOME, else C:/tools/wii-bench on Windows and ~/.wii-bench
elsewhere. It holds queue/pending, queue/running, queue/done (JSON + .log each),
dispatcher.lock (the dispatcher's PID), dispatcher.log, and dispatcher.state (what it is
waiting for, which `wait` reports). $WII_BENCH_NO_DISPATCH=1 makes
add queue a job without starting a dispatcher (for tests).
"""
import argparse
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid

WINDOWS = os.name == "nt"


def state_home():
    if os.environ.get("WII_BENCH_HOME"):
        return pathlib.Path(os.environ["WII_BENCH_HOME"])
    if WINDOWS:
        return pathlib.Path(r"C:\tools\wii-bench")
    return pathlib.Path.home() / ".wii-bench"


HOME = state_home()
Q = HOME / "queue"
PENDING, RUNNING, DONE = Q / "pending", Q / "running", Q / "done"
LOCK = HOME / "dispatcher.lock"
WII = os.environ.get("WII_BENCH_IP", "192.168.8.213")
PORT = int(os.environ.get("WII_BENCH_PORT", "4299"))  # another port only for tests
TIMEOUT = 1800        # s a job may run by default: the slowest 1% took 2,111 s and set --timeout
QUICK = 2             # s of HBC idle after a queue job (ours, or the last lease holder's)
RECENT = 60           # s after a queue job ended that QUICK applies
CHAIN_GRACE = 15      # s the lease is kept for the same agent's next job, nobody else waiting
RETURN_WAIT = float(os.environ.get("WII_BENCH_RETURN_WAIT", "120"))   # s for HBC to come back after a job (shorter only for tests)
BUSY_STEP = 15        # s between probes of a Wii that is busy: an app outside the queue runs


def pid_alive(pid):
    if not WINDOWS:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return bool(ok) and code.value == 259                        # STILL_ACTIVE


def dispatcher_pid():
    try:
        pid = int(LOCK.read_text().strip())
    except (OSError, ValueError):
        return 0
    return pid if pid_alive(pid) else 0


def wii_state():
    """What answers on TCP 4299: ("hbc", version) for HBC in its menu, ("agent", version) for
    an app running hbc-reborn's in-app agent, or ("off", None) for nothing.

    The probe is HBCV, a version request. hbc-reborn's HBC answers with its version, and
    an app with sdk/hbc_agent linked answers on the same port with "<version> agent"
    (tools/hbc.py's is_agent), so a bare "something answers" cannot tell an app from HBC's
    menu (it did until 1.8.7, and a job exited another workstation's running agent app).
    The stock HBC answers nothing to HBCV and rejects it at once ("invalid upload
    request"), which still means its menu. A 16-byte request, never a bare connect and
    close: on the stock HBC that holds its single loader thread for 10 s (its tcp_read
    retried on EOF until TCP_BLOCK_RECV_TIMEOUT; hbc-reborn 1.4.1 fixed this), and its
    listen backlog is 3; probing every 5 s that way filled the backlog and HBC stopped
    answering (2026-09-28)."""
    try:
        with socket.create_connection((WII, PORT), timeout=2) as s:
            s.settimeout(2)
            s.sendall(b"HBCV" + bytes(12))
            reply = b""
            while b"\0" not in reply and len(reply) < 256:
                chunk = s.recv(64)
                if not chunk:
                    break
                reply += chunk
    except OSError:
        return "off", None
    text = reply.split(b"\0", 1)[0].decode("ascii", "replace")
    return ("agent" if text.endswith(" agent") else "hbc"), text


def hbc_idle():
    """True only for HBC in its menu: an agent app answering on 4299 is busy."""
    return wii_state()[0] == "hbc"


def agent_app():
    """The running agent app's name, from its status reply (HBCS), or "an app"."""
    try:
        with socket.create_connection((WII, PORT), timeout=2) as s:
            s.settimeout(2)
            s.sendall(b"HBCS" + bytes(12))
            head = b""
            while len(head) < 8:
                chunk = s.recv(8 - len(head))
                if not chunk:
                    raise OSError("short reply")
                head += chunk
            code, length = struct.unpack(">iI", head)
            body = b""
            while code == 0 and len(body) < min(length, 65536):
                chunk = s.recv(4096)
                if not chunk:
                    break
                body += chunk
        return json.loads(body).get("app") or "an app"
    except (OSError, ValueError):
        return "an app"


def wii_line():
    state, text = wii_state()
    if state == "hbc":
        return f"in HBC {text} (free)" if text else "in HBC (free)"
    if state == "agent":
        return f"busy: {agent_app()} is running (agent {text.removesuffix(' agent')})"
    return "busy or off"


def fs_retry(fn, *args):
    """A rename, replace or unlink of a queue file, retried for up to 2 s on Windows' sharing
    error: there a file another process has open (a `wait` polling for its job, the monitor's
    snapshot) cannot be replaced or removed until it is closed again, a few ms later. On
    2026-10-02 that killed the dispatcher right after a job (PermissionError [WinError 5] on
    the done record), losing the job's HBC check and its history. A missing file is still an
    error at once: that is a cancel."""
    for i in range(40):
        try:
            return fn(*args)
        except PermissionError:
            if i == 39:
                raise
            time.sleep(0.05)


def jobs(d):
    """Oldest first. By the time the job file was written, then the name: the IDs sort by the
    second only, and two jobs added in one second ran in the order of their random tails."""
    def key(p):
        try:
            return (p.stat().st_mtime_ns, p.name)
        except OSError:                           # moved meanwhile (taken, cancelled)
            return (0, p.name)
    return sorted(d.glob("*.json"), key=key)


def load(p):
    return json.loads(p.read_text())


def find(job_id):
    for d in (PENDING, RUNNING, DONE):
        p = d / f"{job_id}.json"
        if p.exists():
            return d, p
    return None, None


def start_dispatcher():
    if dispatcher_pid() or os.environ.get("WII_BENCH_NO_DISPATCH"):
        return
    HOME.mkdir(parents=True, exist_ok=True)
    log = open(HOME / "dispatcher.log", "a")
    detach = ({"creationflags": 0x00000008 | 0x00000200}     # DETACHED_PROCESS | NEW_PROCESS_GROUP
              if WINDOWS else {"start_new_session": True})
    subprocess.Popen([sys.executable, str(pathlib.Path(__file__).resolve()), "run"],
                     stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                     env=dict(os.environ, WII_BENCH_HOME=str(HOME)), **detach)


def kill_tree(proc):
    """Stop a job's own process tree, nothing else."""
    if WINDOWS:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


# --- the bench lease: one turn at the Wii across workstations --------------------------------
#
# Each workstation keeps its own queue and dispatcher (a job runs where it was added: its build,
# its cwd, its firewall rule for the network log). With a lease server configured, a dispatcher
# takes the lease before each job and gives it back after, so workstations take turns, first
# come first served. The server is `wiibench.py serve` (Dockerfile alongside); a client finds it
# in $WII_BENCH_SERVER, else in the file HOME/server (e.g. http://homeserver:4310).

TTL = 60                                           # a lease or a place in line not renewed dies


class Leases:
    """The server's state. One holder; waiters in arrival order. Every client call refreshes
    its entry; one not heard from for ttl seconds is dropped, so a dead workstation frees the
    Wii by itself. After a restart nothing is granted for one ttl: a holder still running its
    job renews within that time and gets its lease back. `log` gets each event (the history).

    A release says what the Wii was doing when the holder let go ("hbc", or what answered
    instead). Only a release with the Wii back in HBC counts as clean for the next holder's
    short idle wait; any other is kept as `left` and shown until a clean release."""

    PRIVATE = ("seen", "ticket", "joined", "granted_at")

    def __init__(self, ttl=TTL, clock=time.monotonic, log=None):
        self.ttl, self.clock, self.log = ttl, clock, log or (lambda ev: None)
        self.holder, self.waiters = None, {}
        self.grace_until = clock() + ttl
        self.released_at = None                    # the last holder's clean release
        self.left = None                           # the last holder left the Wii out of HBC

    def _event(self, kind, e, **more):
        self.log(dict({"type": kind, "host": e.get("host"), "name": e.get("name")}, **more))

    def _expire(self, now):
        if self.holder and now - self.holder["seen"] > self.ttl:
            self._event("expired", self.holder, role="holder",
                        held_s=round(now - self.holder.get("granted_at", now), 1))
            self.holder = None
        for t in [t for t, w in self.waiters.items() if now - w["seen"] > self.ttl]:
            self._event("expired", self.waiters[t], role="waiter", waited_s=round(now - self.waiters[t]["joined"], 1))
            del self.waiters[t]

    def acquire(self, d):
        now = self.clock()
        self._expire(now)
        t = d["ticket"]
        if self.holder and self.holder["ticket"] == t:
            self.holder["seen"] = now
            return {"granted": True}
        if t not in self.waiters:
            d = {k: v for k, v in d.items() if k != "wait"}
            self.waiters[t] = dict(d, since=time.strftime("%Y-%m-%d %H:%M:%S"), joined=now)
            self._event("join", d, place=len(self.waiters),
                        holder=self.holder and self.holder.get("host"))
        w = self.waiters[t]
        w["seen"] = now
        if self.holder is None and now >= self.grace_until and next(iter(self.waiters)) == t:
            self.holder = self.waiters.pop(t)
            self.holder.update(granted=time.strftime("%Y-%m-%d %H:%M:%S"), granted_at=now)
            # How long ago a queued job last let go with the Wii back in HBC: the Wii came
            # back from a queue job, not from someone outside the queue, so the client can
            # skip most of its idle wait.
            ago = None if self.released_at is None else round(now - self.released_at, 1)
            self._event("grant", w, waited_s=round(now - w["joined"], 1), released_ago=ago)
            return {"granted": True, "released_ago": ago}
        return {"granted": False, "position": list(self.waiters).index(t) + 1, "holder": self._show(self.holder, now)}

    def renew(self, d):
        now = self.clock()
        self._expire(now)
        if self.holder is None and d["ticket"] not in self.waiters:   # after a restart: reclaim
            self.holder = dict(d, since=time.strftime("%Y-%m-%d %H:%M:%S"), granted="reclaimed", granted_at=now)
            self._event("reclaim", d)
        if self.holder and self.holder["ticket"] == d["ticket"]:
            self.holder["seen"] = now
            return {"ok": True, "waiting": len(self.waiters)}
        return {"ok": False, "waiting": len(self.waiters)}

    def release(self, d):
        now = self.clock()
        if self.holder and self.holder["ticket"] == d["ticket"]:
            wii = d.get("wii", "hbc")              # a client before 1.9.1 does not say
            self._event("release", self.holder, held_s=round(now - self.holder.get("granted_at", now), 1), wii=wii)
            if wii == "hbc":
                self.released_at, self.left = now, None
            else:
                self.released_at = None
                self.left = {"host": self.holder.get("host"), "name": self.holder.get("name"), "wii": wii,
                             "since": time.strftime("%Y-%m-%d %H:%M:%S"), "at": now}
            self.holder = None
        if d["ticket"] in self.waiters:
            w = self.waiters.pop(d["ticket"])
            self._event("leave", w, waited_s=round(now - w["joined"], 1))
        return {"ok": True}

    def _show(self, e, now):
        """A holder or waiter as status shows it. Durations, not times: the server's clock
        (UTC in its container) is not the reader's, so `held_s`/`waited_s` say how long."""
        if not e:
            return e
        more = {"age": int(now - e["seen"])}
        if "granted_at" in e:
            more["held_s"] = int(now - e["granted_at"])
        elif "joined" in e:
            more["waited_s"] = int(now - e["joined"])
        return {k: v for k, v in dict(e, **more).items() if k not in self.PRIVATE}

    def status(self):
        now = self.clock()
        self._expire(now)
        left = self.left and {k: v for k, v in dict(self.left, ago_s=int(now - self.left["at"])).items() if k != "at"}
        return {"holder": self._show(self.holder, now), "waiters": [self._show(w, now) for w in self.waiters.values()],
                "left": left}


# --- the history: every grant, release and job, kept for good -----------------------------
#
# One JSON object per line, in history/events-YYYY-MM-DD.jsonl (UTC days). A day's file is
# gzipped once it is GZIP_DAYS old, and each month's .gz files are packed into
# history/archive/YYYY-MM.tar once the month is over (the deep archive). Archives are kept
# for keep_months (0: forever). The lease server keeps every workstation's; each dispatcher
# also keeps its own jobs in HOME/history.

GZIP_DAYS = 7


class History:
    def __init__(self, root, keep_months=0, clock=time.time):
        import threading
        self.root, self.keep, self.clock = pathlib.Path(root), keep_months, clock
        self.lock, self.maintained = threading.Lock(), None

    @staticmethod
    def day(t):
        return time.strftime("%Y-%m-%d", time.gmtime(t))

    def write(self, ev):
        now = self.clock()
        line = json.dumps(dict({"t": round(now, 3), "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))}, **ev))
        with self.lock:
            self.root.mkdir(parents=True, exist_ok=True)
            with open(self.root / f"events-{self.day(now)}.jsonl", "a", encoding="utf-8", newline="\n") as f:
                f.write(line + "\n")
            if self.maintained != self.day(now):   # once a day: rotate, compress, archive
                self.maintain()

    def maintain(self):
        import gzip
        import shutil
        import tarfile
        now = self.clock()
        self.maintained = self.day(now)
        old = self.day(now - GZIP_DAYS * 86400)
        for f in sorted(self.root.glob("events-*.jsonl")):
            if f.stem[7:] < old:
                with open(f, "rb") as src, gzip.open(f.with_name(f.name + ".gz"), "wb") as dst:
                    shutil.copyfileobj(src, dst)
                f.unlink()
        month = self.day(now)[:7]
        arch = self.root / "archive"
        for f in sorted(self.root.glob("events-*.jsonl.gz")):
            m = f.name[7:14]
            if m < month:
                arch.mkdir(exist_ok=True)
                with tarfile.open(arch / f"{m}.tar", "a") as t:
                    if f.name not in t.getnames():
                        t.add(f, arcname=f.name)
                f.unlink()
        if self.keep:
            y, mo = map(int, month.split("-"))
            mo -= self.keep
            while mo < 1:
                y, mo = y - 1, mo + 12
            for t in arch.glob("*.tar") if arch.exists() else ():
                if t.stem < f"{y:04d}-{mo:02d}":
                    t.unlink()

    def read(self, since, until=None):
        """Events with since <= t < until, oldest first, from the live, gzipped and archived files."""
        import gzip
        import io
        import tarfile
        until = until or self.clock() + 1
        first, last = self.day(since), self.day(until)

        def lines(name, data):
            if name.endswith(".gz"):
                data = gzip.decompress(data)
            for l in data.decode("utf-8", "replace").splitlines():
                try:
                    ev = json.loads(l)
                except ValueError:
                    continue
                if since <= ev.get("t", 0) < until:
                    yield ev

        out = []
        with self.lock:
            files = {}
            for t in sorted((self.root / "archive").glob("*.tar")) if (self.root / "archive").exists() else ():
                if first[:7] <= t.stem <= last[:7]:
                    with tarfile.open(t) as tar:
                        for m in tar.getmembers():
                            if first <= m.name[7:17] <= last:
                                files[m.name] = tar.extractfile(m).read()
            for f in self.root.glob("events-*.jsonl*"):
                if first <= f.name[7:17] <= last:
                    files[f.name] = f.read_bytes()
        for name in sorted(files, key=lambda n: n[7:17]):
            out.extend(lines(name, files[name]))
        out.sort(key=lambda ev: ev.get("t", 0))
        return out


def make_server(port, ttl=TTL, host="0.0.0.0", history=None):
    import http.server
    import threading
    import urllib.parse
    hist = History(history) if history else None

    def log(ev):
        if hist:
            try:
                hist.write(ev)
            except OSError as e:
                print(f"history: {e}", flush=True)

    leases, cond = Leases(ttl, log=log), threading.Condition()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"            # keep-alive: a client reuses its connection
        timeout = 60                               # s an idle kept connection may stay open

        def reply(self, body, code=200):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path == "/status":
                with cond:
                    r = leases.status()
                return self.reply(r)
            if url.path == "/history":
                if not hist:
                    return self.reply({"error": "this server keeps no history (serve --history DIR)"}, 404)
                q = urllib.parse.parse_qs(url.query)
                try:
                    since = float(q.get("since", [time.time() - 86400])[0])
                    until = float(q["until"][0]) if "until" in q else None
                except ValueError:
                    return self.reply({"error": "bad request"}, 400)
                return self.reply({"events": hist.read(since, until)})
            self.reply({"error": "not found"}, 404)

        def do_POST(self):
            try:
                d = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
                if not isinstance(d, dict):
                    raise TypeError
            except (ValueError, TypeError):
                return self.reply({"error": "bad request"}, 400)
            if self.path == "/event":              # a job's record from a dispatcher
                if not isinstance(d.get("type"), str):
                    return self.reply({"error": "bad request"}, 400)
                log(d)
                return self.reply({"ok": True})
            op = {"/acquire": leases.acquire, "/renew": leases.renew, "/release": leases.release}.get(self.path)
            try:
                d["ticket"]
                wait = min(max(float(d.get("wait", 0)), 0.0), 30.0)
            except (ValueError, KeyError, TypeError):
                return self.reply({"error": "bad request"}, 400)
            if op is None:
                return self.reply({"error": "not found"}, 404)
            with cond:
                r = op(d)
                if op == leases.acquire:           # a long poll: answer once granted, or after wait
                    end = time.monotonic() + wait
                    while not r["granted"] and time.monotonic() < end:
                        cond.wait(min(1.0, end - time.monotonic()))   # 1 s: expiry is by time
                        r = op(d)
                else:
                    cond.notify_all()              # a release may let a waiter in
            self.reply(r)                          # never write to a socket holding the lock

        def log_message(self, fmt, *args):
            path = getattr(self, "path", "")       # none yet when a kept connection idles out
            if path and not path.startswith(("/status", "/acquire", "/renew", "/event", "/history")):   # polls: noise
                super().log_message(fmt, *args)

    class Server(http.server.ThreadingHTTPServer):
        def handle_error(self, request, client_address):
            # A client that goes away from a kept connection (a monitor quit, a dispatcher
            # killed) resets it: routine with keep-alive, not worth a traceback each time.
            if isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
                return
            super().handle_error(request, client_address)

    srv = Server((host, port), Handler)
    srv.history, srv.leases = hist, leases
    return srv


def cmd_serve(a):
    if a.history:
        History(a.history, a.keep_months).maintain()
    srv = make_server(a.port, a.ttl, a.host, a.history)
    if srv.history:
        srv.history.keep = a.keep_months
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))   # PID 1 in Docker ignores it otherwise
    print(f"wii-bench lease server on {a.host or '*'}:{a.port}, ttl {a.ttl} s, history "
          + (f"in {a.history} (archives kept {f'{a.keep_months} months' if a.keep_months else 'forever'})"
             if a.history else "off"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


def read_setting(p):
    """A one-line setting file, however it was written: Windows PowerShell 5.1's `echo x > f`
    makes UTF-16 with a BOM, Notepad may add a UTF-8 BOM."""
    b = p.read_bytes()
    return (b.decode("utf-16") if b[:2] in (b"\xff\xfe", b"\xfe\xff") else b.decode("utf-8-sig")).strip()


def lease_server():
    url = os.environ.get("WII_BENCH_SERVER")
    if url is None:
        try:
            url = read_setting(HOME / "server")
        except (OSError, UnicodeDecodeError):
            url = ""
    return url.rstrip("/")


# Each lookup of a .local name took 220-250 ms on Windows, which does not cache mDNS answers
# (measured 2026-10-02: homeserver.local, every call), and every call made one. So a process
# resolves the server once (for ADDR_TTL s, again after a failure) and keeps its connection
# open: the server speaks HTTP/1.1. One connection per thread: the renew thread calls too.
ADDR_TTL = 300
_ADDR = {}


def _resolve(host, port):
    hit = _ADDR.get((host, port))
    if hit and hit[1] > time.monotonic():
        return hit[0]
    infos = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    infos.sort(key=lambda i: i[0] != socket.AF_INET)   # IPv4 first: a link-local IPv6 needs a scope
    ip = infos[0][4][0]
    _ADDR[(host, port)] = (ip, time.monotonic() + ADDR_TTL)
    return ip


_LOCAL = threading.local()


def _conns():
    if not hasattr(_LOCAL, "conns"):
        _LOCAL.conns = {}
    return _LOCAL.conns


def lease_call(path, body=None, timeout=10):
    import http.client
    import urllib.error
    import urllib.parse
    url = lease_server() + path
    u = urllib.parse.urlsplit(url)
    host, port = u.hostname, u.port or 80
    data = json.dumps(body).encode() if body is not None else None
    target = url[len(f"{u.scheme}://{u.netloc}"):] or "/"
    for attempt in (0, 1):
        conns, key, c = _conns(), None, None
        try:
            key = (_resolve(host, port), port)
            c = conns.get(key)
            if c is None:
                c = conns[key] = http.client.HTTPConnection(key[0], port, timeout=timeout)
            c.timeout = timeout
            if c.sock:
                c.sock.settimeout(timeout)
            c.request("POST" if data is not None else "GET", target, body=data,
                      headers={"Host": u.netloc, "Content-Type": "application/json"})
            r = c.getresponse()
            raw = r.read()
            if r.will_close:                       # an HTTP/1.0 server (before 1.9.4) closes each time
                c.close()
                conns.pop(key, None)
        except (OSError, http.client.HTTPException) as e:
            # A kept connection the server has since closed fails at once: try a fresh one, once.
            if c is not None:
                c.close()
            conns.pop(key, None)
            if attempt:
                _ADDR.pop((host, port), None)     # and look the name up again next time
                raise e if isinstance(e, OSError) else OSError(f"{type(e).__name__}: {e}")
            continue
        if r.status >= 400:
            raise urllib.error.HTTPError(url, r.status, f"{r.reason}: {raw[:200]!r}", r.headers, None)
        return json.loads(raw)


LONG_POLL = 25                                     # s the server holds an /acquire open


def say(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S ") + msg, flush=True)


STATE = HOME / "dispatcher.state"


def report(job_id, detail):
    """What the dispatcher is doing for the job at the head of the queue, for `wait`."""
    if job_id is None:
        return
    try:
        tmp = HOME / f"dispatcher.state.{os.getpid()}"
        tmp.write_text(json.dumps({"job": job_id, "detail": detail, "since": time.time()}))
        os.replace(tmp, STATE)
    except OSError:
        pass


class Turn:
    """This dispatcher's turn at the Wii: waits in line for the lease, then keeps it renewed
    until released. Without a lease server it is a no-op (a single workstation)."""

    def __init__(self, name, job_id=None):
        self.info = {"ticket": uuid.uuid4().hex, "host": socket.gethostname(), "name": name}
        self.job_id, self.stop, self.released_ago = job_id, None, None
        self.wii = "hbc"                           # what the Wii was doing at the release

    def others_waiting(self):
        """Workstations in line behind this one (0 without a server, or if it cannot say)."""
        if not self.stop:
            return 0
        try:
            return lease_call("/renew", self.info).get("waiting", 0)
        except OSError:
            return 0

    def __enter__(self):
        if not lease_server():
            return self
        import threading
        said = None
        while True:
            t = time.monotonic()
            try:
                r = lease_call("/acquire", dict(self.info, wait=LONG_POLL), timeout=LONG_POLL + 10)
            except OSError as e:
                r = {"granted": False, "error": str(e)}
            if r["granted"]:
                break
            msg = (f"lease server {lease_server()} unreachable: {r['error']}" if "error" in r else
                   f"waiting for the lease: place {r['position']} in line"
                   + (f", held by {r['holder']['host']} for {r['holder']['name']}" if r.get("holder") else ""))
            if msg != said:
                say(msg)
                report(self.job_id, msg)
                said = msg
            if time.monotonic() - t < 1:           # an error, or a server without the long poll
                time.sleep(5)
        self.released_ago = r.get("released_ago")
        say(f"lease taken for {self.info['name']}")
        self.stop = threading.Event()

        def renew():
            while not self.stop.wait(TTL / 4):
                try:
                    if not lease_call("/renew", self.info)["ok"]:
                        say("lease lost: another workstation may have the Wii")
                except OSError:
                    pass                           # retried; the server keeps it for TTL
        threading.Thread(target=renew, daemon=True).start()
        return self

    def __exit__(self, *exc):
        if self.stop:
            self.stop.set()
            try:
                lease_call("/release", dict(self.info, wii=self.wii))
            except OSError:
                pass                               # it expires after TTL


SHIM = """#!/usr/bin/env python3
# Written by `wiibench.py setup`: the bench queue's source lives in git ({src}).
# The queue's state stays in this directory, so every project that calls this path shares
# one queue and one dispatcher on this workstation.
import os, runpy, sys
src = os.environ.get("WII_BENCH_SRC", {src!r})
os.environ.setdefault("WII_BENCH_HOME", os.path.dirname(os.path.abspath(__file__)))
sys.argv[0] = src
runpy.run_path(src, run_name="__main__")
"""


def cmd_setup(a):
    """Make this workstation a bench client: HOME/wiibench.py (a shim to this file, the path
    other projects call) and, with --server, HOME/server. Safe to run again."""
    HOME.mkdir(parents=True, exist_ok=True)
    shim, src = HOME / "wiibench.py", str(pathlib.Path(__file__).resolve())
    if shim.exists() and "runpy.run_path" not in shim.read_text(encoding="utf-8", errors="replace"):
        sys.exit(f"{shim} exists and is not a shim; move it away first")
    if shim.resolve() != pathlib.Path(src):
        shim.write_text(SHIM.format(src=src), encoding="utf-8", newline="\n")
        print(f"shim   {shim} -> {src}")
    if a.server is not None:
        (HOME / "server").write_text(a.server.strip() + "\n", encoding="utf-8", newline="\n")
    print(f"state  {HOME}")
    print(f"server {lease_server() or 'none: this workstation has the Wii to itself'}")
    s = err = None
    if lease_server():
        try:
            s = lease_call("/status")
            print("        reachable")
        except OSError as e:
            err = e
            print(f"        unreachable: {e}")
    print(f"Wii    {WII}: {wii_now(s, err)}")
    print("Jobs that use the network log need inbound TCP 4300 open on this machine.")


def job_agent(given=None):
    """Who queued the job, so the same agent's back-to-back jobs can run as one chain:
    --agent, else $WII_BENCH_AGENT, else the Claude Code session, else nobody (no chain)."""
    for v in (given, os.environ.get("WII_BENCH_AGENT"), os.environ.get("CLAUDE_CODE_SESSION_ID")):
        if v:
            return v
    return None


def cmd_add(a):
    for d in (PENDING, RUNNING, DONE):
        d.mkdir(parents=True, exist_ok=True)
    if not a.cmd:
        sys.exit("add: give the command after --")
    job_id = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
    job = {"id": job_id, "name": a.name or pathlib.Path(a.cmd[0]).name, "cwd": str(pathlib.Path(a.cwd).resolve()),
           "cmd": a.cmd, "timeout": a.timeout, "added": time.strftime("%Y-%m-%d %H:%M:%S"),
           "agent": job_agent(a.agent)}
    tmp = PENDING / f"{job_id}.tmp"
    tmp.write_text(json.dumps(job, indent=1))
    fs_retry(tmp.rename, PENDING / f"{job_id}.json")
    print(job_id)
    start_dispatcher()


def job_window():
    """Windows: the dispatcher has no console, so each console job (python.exe) gets a
    window of its own. Show it minimized and without taking the focus."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 7                             # SW_SHOWMINNOACTIVE
    return {"startupinfo": si}


def run_job(p):
    job = load(p)
    r = RUNNING / p.name
    fs_retry(p.rename, r)                          # claims it: a rename is atomic
    report(job["id"], "running")
    job["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    r.write_text(json.dumps(job, indent=1))
    log = RUNNING / f"{job['id']}.log"
    t0 = time.monotonic()
    env = dict(os.environ, WII_BENCH_IP=WII, WII_BENCH_JOB=job["id"],
               WII_BENCH_JOB_START=f"{time.time():.3f}")
    with open(log, "w") as out:
        try:
            proc = subprocess.Popen(job["cmd"], cwd=job["cwd"], stdout=out, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, env=env,
                                    **(job_window() if WINDOWS else {"start_new_session": True}))
            try:
                job["exit"] = proc.wait(timeout=job.get("timeout") or TIMEOUT)
            except subprocess.TimeoutExpired:
                kill_tree(proc)
                job["exit"] = "timeout"
        except OSError as e:
            out.write(f"could not start: {e}\n")
            job["exit"] = "not started"
    job["secs"] = int(time.monotonic() - t0)
    job["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    (DONE / f"{job['id']}.json").write_text(json.dumps(job, indent=1))
    fs_retry(log.rename, DONE / log.name)
    fs_retry(r.unlink)
    print(f"{job['finished']} done {job['id']} {job['name']}: exit {job['exit']} in {job['secs']} s", flush=True)
    return job


def finish_job(job, turn):
    """After the HBC check: the job's record gets it, and the job goes into the history,
    the lease server's (every workstation's) and this workstation's own."""
    p = DONE / f"{job['id']}.json"
    tmp = DONE / f"{job['id']}.json.tmp"
    tmp.write_text(json.dumps(job, indent=1))
    try:
        fs_retry(os.replace, tmp, p)
    except OSError as e:                           # the record without the check, not a crash
        say(f"could not update {p.name}: {e}")
        tmp.unlink(missing_ok=True)
    ev = {"type": "job", "host": socket.gethostname(), **{k: job.get(k) for k in (
        "id", "name", "agent", "added", "started", "finished", "secs", "exit", "timeout",
        "hbc_back_s", "hbc_version", "wii_left", "chained")}}
    try:
        History(HOME / "history").write(ev)
    except OSError as e:
        say(f"history: {e}")
    if turn.stop:                                  # holding a lease from a server
        try:
            lease_call("/event", ev)
        except OSError as e:
            say(f"history: the lease server did not take the job record: {e}")


def wait_idle(full, need, job_id=None):
    """Return once HBC's menu has answered for `need` seconds in a row (0: one answer).

    Only ever called while this dispatcher holds the Wii and no job of its own runs, so it
    never reaches into a queue run. `need` is short only right after a queue job, whose end
    we know about; the first probe that finds the Wii busy or off means someone outside the
    queue may be at it, and the wait goes back to the full `full` seconds. A busy Wii is
    probed only every BUSY_STEP s, and an agent app's name asked for once, so an app run
    outside the queue is disturbed as little as possible."""
    step = 1.0 if need < full else 5.0
    ok_since = busy_since = None
    said, busy_what = None, None
    while True:
        t = time.monotonic()
        state, text = wii_state()
        if state == "hbc":
            ok_since = t if ok_since is None else ok_since
            busy_since, busy_what = None, None
            if t - ok_since >= need:
                return
            msg = "waiting for HBC to stay idle"
        else:
            ok_since, need, step = None, full, BUSY_STEP
            busy_since = t if busy_since is None else busy_since
            mins = int((t - busy_since) // 60)
            if busy_what is None or not busy_what.startswith(state):
                busy_what = f"{state}: busy: {agent_app()} is running" if state == "agent" else f"{state}: busy or off"
            msg = f"Wii {busy_what.split(': ', 1)[1]}" + (f" for {mins} min" if mins else "")
        if msg != said:
            report(job_id, msg)
            said = msg
        time.sleep(max(0.0, step - (time.monotonic() - t)))


def hbc_back(limit=RETURN_WAIT):
    """After a job, while this dispatcher still holds the Wii: (seconds until HBC's menu
    answered, "hbc", its version), or (None, what answers instead, None). Probes only once the
    job's process is gone, so never during a run.

    The seconds are measured to the answer, not to the probe that got it: until 1.9.4 a first
    answer always read 0.0, however long it took, and 51 of the first 52 checks said "0.0 s"
    (most jobs wait for HBC themselves before they end, so HBC is usually there at once)."""
    t0 = time.monotonic()
    while True:
        t = time.monotonic()
        state, text = wii_state()
        if state == "hbc":
            return round(time.monotonic() - t0, 2), "hbc", text or "stock HBC"
        if time.monotonic() - t0 >= limit:
            return None, (f"busy: {agent_app()} is running" if state == "agent" else "busy or off"), None
        time.sleep(max(0.0, 1 - (time.monotonic() - t)))


def next_job(agent, grace, waiting=lambda: 0):
    """The next pending job if it is this agent's, waiting up to `grace` s for it to be
    added (an agent that waits for one job and then adds the next). Another agent's job at
    the head of the queue, or another workstation joining the line (`waiting`, asked about
    once a second), ends the chain at once."""
    end, asked = time.monotonic() + grace, time.monotonic()
    while True:
        q = jobs(PENDING)
        if q:
            try:
                j = load(q[0])
            except (OSError, ValueError):          # cancelled meanwhile
                continue
            return q[0] if j.get("agent") == agent else None
        if time.monotonic() >= end:
            return None
        if time.monotonic() - asked >= 1:
            if waiting():
                return None
            asked = time.monotonic()
        time.sleep(0.25)


def cmd_run(a):
    for d in (PENDING, RUNNING, DONE):
        d.mkdir(parents=True, exist_ok=True)
    while True:                                    # the lock: created atomically, or stale
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            break
        except FileExistsError:
            if dispatcher_pid():
                sys.exit("a dispatcher is already running")
            time.sleep(0.5)                        # a lock being written, or one left by a dead one
            if not dispatcher_pid():
                LOCK.unlink(missing_ok=True)
    try:
        for p in jobs(RUNNING):                    # a dispatcher that died mid-job: requeue it
            fs_retry(p.rename, PENDING / p.name)
        empty_since = time.monotonic()
        last_end = None                            # monotonic time our last job ended
        while True:
            q = jobs(PENDING)
            if not q:
                if time.monotonic() - empty_since > 600:
                    return
                time.sleep(1)
                continue
            try:
                head = load(q[0])
            except (OSError, ValueError):          # cancelled meanwhile
                continue
            with Turn(head["name"], head["id"]) as turn:   # other workstations' jobs go first if first in line
                ago = turn.released_ago
                recent = ((last_end is not None and time.monotonic() - last_end < RECENT)
                          or (ago is not None and ago < RECENT))
                wait_idle(a.idle, min(QUICK, a.idle) if recent else a.idle, head["id"])
                p = jobs(PENDING)[:1]
                p = p[0] if p else None            # a cancel may have emptied it meanwhile
                chained = False
                while p:
                    try:
                        agent = load(p).get("agent")
                        job = run_job(p)
                    except (FileNotFoundError, ValueError):   # cancelled just before it started
                        break
                    last_end = time.monotonic()
                    # The Wii is still ours: see that HBC came back, and say so if it did not.
                    back_s, turn.wii, hbc_version = hbc_back()
                    job.update(hbc_back_s=back_s, hbc_version=hbc_version, chained=chained)
                    if back_s is None:
                        job["wii_left"] = turn.wii
                        say(f"the Wii did not come back to HBC within {RETURN_WAIT} s: {turn.wii}")
                    finish_job(job, turn)
                    if back_s is None or not agent:
                        break
                    # The same agent's next job runs in this turn without the idle wait, but
                    # only while no other workstation waits: with someone in line, this turn
                    # ends after every job (a hand-over costs about 2 s since the long poll).
                    # With nobody waiting, the lease is held CHAIN_GRACE s for the agent's
                    # next add, and let go within a second of someone joining the line.
                    if turn.others_waiting():
                        break
                    p = next_job(agent, CHAIN_GRACE, turn.others_waiting)
                    if p:
                        chained = True
                        say(f"chained: {p.stem} from the same agent")
                        wait_idle(a.idle, 0, p.stem)
            empty_since = time.monotonic()
    finally:
        if dispatcher_pid() == os.getpid():
            LOCK.unlink(missing_ok=True)


def wii_now(lease=None, lease_error=None):
    """The Wii's line for status and setup. It is probed only when no run can be going on:
    nothing running here, and the lease free (or no server). Otherwise it says who has the
    Wii, and leaves it alone: the queue never reaches into a run."""
    running = jobs(RUNNING) if RUNNING.exists() else []
    if running:
        try:
            return f"in use by job {load(running[0])['name']} here (not probed)"
        except (OSError, ValueError):
            return "in use by a job here (not probed)"
    if lease_error:
        return "not probed: the lease server cannot say whether a run is going on"
    h = (lease or {}).get("holder")
    if h:
        return f"in use: {h['host']} has it for {h['name']} (not probed)"
    return wii_line()


def cmd_status(a):
    pid = dispatcher_pid()
    s = err = None
    if lease_server():
        try:
            s = lease_call("/status")
        except OSError as e:
            err = e
    print(f"Wii {WII}: {wii_now(s, err)}; dispatcher: {f'pid {pid}' if pid else 'not running'}")
    if lease_server():
        if err:
            print(f"lease {lease_server()}: unreachable ({err})")
        else:
            h = s["holder"]
            print(f"lease {lease_server()}: " + (f"held by {h['host']} for {h['name']} (since {h['granted']})" if h else "free"))
            for w in s["waiters"]:
                print(f"  waiting  {w['host']}  {w['name']}  (since {w['since']})")
            if s.get("left"):
                l = s["left"]
                print(f"  left     {l['host']}'s {l['name']} left the Wii {l['wii']} (since {l['since']})")
    for d, label in ((RUNNING, "running"), (PENDING, "pending")):
        for p in jobs(d):
            j = load(p)
            print(f"  {label:8} {j['id']}  {j['name']}  (added {j['added']})")
    for p in jobs(DONE)[-a.last:]:
        j = load(p)
        print(f"  done     {j['id']}  {j['name']}  exit {j['exit']}, {j['secs']} s, {j['finished']}")


def progress(job_id, d, started):
    """One line on why the job is not done yet, for `wait`."""
    mins = int((time.monotonic() - started) // 60)
    if d == RUNNING:
        return f"job {job_id}: running ({mins} min in wait)"
    ahead = [p.stem for p in jobs(PENDING)]
    place = ahead.index(job_id) if job_id in ahead else 0
    why = "queued"
    try:
        s = json.loads(STATE.read_text())
        if s.get("job") == job_id:
            why = s["detail"]
        elif place == 0 and s.get("detail") == "running":
            why = "the job before it is still running"
    except (OSError, ValueError, KeyError):
        pass
    return (f"job {job_id}: pending, {place} ahead on this workstation" if place else
            f"job {job_id}: next here; {why}") + f" ({mins} min)"


def cmd_wait(a):
    started, told = time.monotonic(), time.monotonic()
    while True:
        d, p = find(a.id)
        if d is None:
            sys.exit(f"no job {a.id}")
        if d == DONE:
            j = load(p)
            lines = (DONE / f"{a.id}.log").read_text(encoding="utf-8", errors="replace").splitlines()
            print("\n".join(lines[-a.tail:]))
            print(f"job {a.id} {j['name']}: exit {j['exit']} in {j['secs']} s")
            sys.exit(j["exit"] if isinstance(j["exit"], int) else 1)
        if d == PENDING and not dispatcher_pid():
            start_dispatcher()
        if time.monotonic() - told >= a.every:     # once a minute: why it is not done yet
            print(progress(a.id, d, started), file=sys.stderr, flush=True)
            told = time.monotonic()
        time.sleep(0.5)


def report_since(a):
    if a.since:
        for fmt in ("%Y-%m-%d %H:%M", "%Y-%m-%d"):
            try:
                return time.mktime(time.strptime(a.since, fmt))
            except ValueError:
                pass
        sys.exit("--since: YYYY-MM-DD or 'YYYY-MM-DD HH:MM' (local time)")
    return time.time() - (a.days * 86400 if a.days else a.hours * 3600)


def cmd_report(a):
    """The queue over a window, from the lease server's history (every workstation), or this
    workstation's own with --local or without a server."""
    import statistics as st
    since, now = report_since(a), time.time()
    if lease_server() and not a.local:
        try:
            evs, src = lease_call(f"/history?since={since}", timeout=60)["events"], f"lease server {lease_server()}"
        except OSError as e:
            sys.exit(f"the lease server's history: {e} (--local for this workstation's)")
    else:
        evs, src = History(HOME / "history").read(since), f"this workstation ({HOME / 'history'})"
    if a.json:
        print(json.dumps(evs, indent=1))
        return
    loc = lambda t: time.strftime("%m-%d %H:%M", time.localtime(t))
    span = now - since
    print(f"bench queue {loc(since)} .. {loc(now)} ({span / 3600:.1f} h), from {src}: {len(evs)} events")
    jobs_ = [e for e in evs if e["type"] == "job"]
    grants = [e for e in evs if e["type"] == "grant"]
    hosts = sorted({e.get("host") or "?" for e in jobs_ + grants})
    if not evs:
        return
    busy = sum(e.get("secs") or 0 for e in jobs_)
    print(f"Wii in use by queue jobs {busy / 60:.0f} min ({100 * busy / span:.0f}% of the window), "
          f"{len(jobs_)} jobs, {sum(1 for e in jobs_ if e.get('chained'))} chained, {len(grants)} turns")
    print(f"\n{'workstation':24} {'jobs':>5} {'failed':>6} {'Wii min':>8} {'turns':>5} {'wait med':>8} {'wait max':>8}")
    for h in hosts:
        hj = [e for e in jobs_ if (e.get("host") or "?") == h]
        hw = [e["waited_s"] for e in grants if (e.get("host") or "?") == h]
        failed = sum(1 for e in hj if e.get("exit") != 0)
        med = f"{st.median(hw) / 60:.1f} m" if hw else "-"
        mx = f"{max(hw) / 60:.1f} m" if hw else "-"
        print(f"{h[:24]:24} {len(hj):5} {failed:6} {sum(e.get('secs') or 0 for e in hj) / 60:8.0f} {len(hw):5} {med:>8} {mx:>8}")
    # Hand-overs: a grant to someone already waiting when the previous holder let go.
    rel, gaps = None, []
    for e in evs:
        if e["type"] == "release":
            rel = e
        elif e["type"] == "grant" and rel and e.get("waited_s", 0) > e["t"] - rel["t"]:
            gaps.append(e["t"] - rel["t"])
            rel = None
    if gaps:
        print(f"\nhand-overs to a waiting workstation: {len(gaps)}, median {st.median(gaps):.1f} s, max {max(gaps):.1f} s")
    long = sorted(grants, key=lambda e: e.get("waited_s", 0), reverse=True)[:5]
    if long and long[0].get("waited_s", 0) >= 60:
        print("longest waits for the Wii:")
        for e in long:
            if e.get("waited_s", 0) >= 60:
                print(f"  {e['waited_s'] / 60:5.1f} min  {loc(e['t'])}  {e.get('host')}: {e.get('name')}")
    trouble = ([f"{loc(e['t'])} {e.get('host')}'s {e.get('name')} left the Wii {e['wii']}"
                for e in evs if e["type"] == "release" and e.get("wii", "hbc") != "hbc"]
               + [f"{loc(e['t'])} {e.get('host')}'s {e.get('name')}: {e.get('wii_left')}"
                  for e in jobs_ if e.get("hbc_back_s") is None and e.get("wii_left") and src.startswith("this")]
               + [f"{loc(e['t'])} {e.get('host')} {e['role']} expired ({e.get('name')})"
                  for e in evs if e["type"] == "expired"]
               + [f"{loc(e['t'])} {e.get('host')}'s {e.get('name')} timed out after {e.get('secs')} s"
                  for e in jobs_ if e.get("exit") == "timeout"])
    print("\n" + ("\n".join(["needs a look:"] + [f"  {t}" for t in trouble]) if trouble else "nothing went wrong"))


# --- snapshot: everything the monitor shows, without touching the Wii ----------------------
#
# monitor.sh draws; this collects. One call per frame prints tab-separated lines:
#   @kv   KEY VALUE              a single fact (the Wii's owner, the lease server, counts)
#   @row  SECTION F1 F2 ...      one table row, already sorted and filtered
# No field is ever empty ("-" instead): bash's read collapses runs of tabs. Nothing here
# probes the Wii: it reads this workstation's queue files and dispatcher.log, and the lease
# server's status and history.

def _cell(v):
    if v is None or v == "":
        return "-"
    return str(v).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def _age(s):
    s = max(0, int(s))
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    if s < 86400:
        return f"{s // 3600}h{(s % 3600) // 60:02d}m"
    return f"{s // 86400}d{(s % 86400) // 3600}h"


def _when(t, now):
    lt, ln = time.localtime(t), time.localtime(now)
    return time.strftime("%H:%M:%S" if lt[:3] == ln[:3] else "%m-%d %H:%M", lt)


def _parse_local(s):
    try:
        return time.mktime(time.strptime(s, "%Y-%m-%d %H:%M:%S"))
    except (TypeError, ValueError):
        return None


HBC_AT_ONCE = 1.5     # s: HBC answering this soon after a job was there as it ended


def hbc_word(j):
    """A finished job's HBC column: ok (there as it ended), Ns (came back that much later),
    LEFT (never, within RETURN_WAIT), or - (a record from before the check, 1.9.1)."""
    back = j.get("hbc_back_s")
    if back is None:
        return "LEFT" if j.get("wii_left") else "-"
    return "ok" if back <= HBC_AT_ONCE else f"{back:.0f}s"


ERROR_WORDS = ("error", "fail", "exception", "traceback", "timed out", "timeout", "not found",
               "refused", "denied", "no such", "cannot", "can't", "could not", "unable", "fatal")


def _last_line(p):
    """What a failed job's log says went wrong: its last line that reads like an error, else
    its last line. (WiiStation's log ends with the result files it got, which says nothing.)"""
    try:
        lines = [l.strip() for l in p.read_text(encoding="utf-8", errors="replace").splitlines() if l.strip()]
    except OSError:
        return None
    for l in reversed(lines[-40:]):
        if any(w in l.lower() for w in ERROR_WORDS):
            return l[:160]
    return lines[-1][:160] if lines else None


LOG_PROBLEMS = (("lease lost", "err", "lease lost"), ("unreachable", "warn", "lease server"),
                ("history:", "warn", "history"), ("did not come back", None, None))


SERVER_TIMEOUT = 2    # s the monitor waits for the lease server (on the LAN it answers in ms)
SERVER_RETRY = 15     # s before a server that did not answer is asked again


class SnapCache:
    """What `snapshot --serve` keeps from one refresh to the next, so a refresh costs only
    what changed: each finished job's record (by file mtime), the history fetched so far (only
    newer events are asked for), and dispatcher.log (by size and mtime)."""

    def __init__(self):
        self.jobs, self.hist, self.local, self.log = {}, None, None, None
        self.down_until, self.down_error = 0.0, None   # the lease server failed: ask again later


LOG_TAIL = 512 * 1024


def _log_lines(cache=None):
    p = HOME / "dispatcher.log"
    try:
        st = p.stat()
    except OSError:
        return []
    key = (st.st_size, st.st_mtime_ns)
    if cache is not None and cache.log and cache.log[0] == key:
        return cache.log[1]
    try:
        with open(p, "rb") as f:                   # the log only grows: its last LOG_TAIL bytes
            if st.st_size > LOG_TAIL:
                f.seek(-LOG_TAIL, 2)
            data = f.read()
        lines = data.decode("utf-8", "replace").splitlines()
        if st.st_size > LOG_TAIL:
            lines = lines[1:]                      # the first is cut somewhere in the middle
    except OSError:
        return []
    if cache is not None:
        cache.log = (key, lines)
    return lines


def _job_record(p, m, cache=None):
    """A finished job's record, and its log's last line when it failed: from the cache while
    the file is unchanged."""
    hit = cache.jobs.get(p) if cache is not None else None
    if hit and hit[0] == m:
        return hit[1], hit[2]
    j = load(p)
    last = _last_line(DONE / f"{j['id']}.log") if job_problem(j) else None
    if cache is not None:
        cache.jobs[p] = (m, j, last)
    return j, last


def _server_history(since, cache=None):
    """The lease server's events since `since`; with a cache, only those newer than the last
    one already held are fetched."""
    url = lease_server()
    h = cache.hist if cache is not None else None
    try:
        if h is None or h["url"] != url or h["since"] > since:
            evs = lease_call(f"/history?since={since}", timeout=SERVER_TIMEOUT * 3)["events"]
            h = {"url": url, "since": since, "evs": evs}
        else:
            last = h["evs"][-1]["t"] if h["evs"] else since
            new = lease_call(f"/history?since={last}", timeout=SERVER_TIMEOUT)["events"]
            known = {json.dumps(e, sort_keys=True) for e in h["evs"] if e.get("t") == last}
            h["evs"] += [e for e in new if not (e.get("t") == last and json.dumps(e, sort_keys=True) in known)]
            h["evs"] = [e for e in h["evs"] if e.get("t", 0) >= since]
            h["since"] = since
    except OSError:
        if cache is not None:
            cache.hist = None
        raise
    if cache is not None:
        cache.hist = h
    return list(h["evs"])


def _local_history(since, cache=None):
    root = HOME / "history"
    try:
        key = tuple(sorted((str(f), f.stat().st_mtime_ns) for f in root.rglob("*") if f.is_file()))
    except OSError:
        key = ()
    if cache is not None and cache.local and cache.local[0] == key and cache.local[1] <= since:
        return [e for e in cache.local[2] if e.get("t", 0) >= since]
    evs = History(root).read(since)
    if cache is not None:
        cache.local = (key, since, evs)
    return evs


def dispatcher_log_problems(since, lines=None):
    """Problems this workstation's dispatcher logged since `since`: (t, severity, kind, detail).
    A traceback is one problem, its last line the detail, at the time of the line before it."""
    out, ts, tb = [], None, None
    for line in (_log_lines() if lines is None else lines):
        t = _parse_local(line[:19]) if len(line) >= 19 else None
        if t is not None:
            if tb:
                out.append((tb[0], "err", "dispatcher crashed", tb[1]))
                tb = None
            ts, msg = t, line[20:]
            for pat, sev, kind in LOG_PROBLEMS:
                if pat in msg:
                    if sev and t >= since:         # "did not come back" is in the job's record
                        out.append((t, sev, kind, msg[:160]))
                    break
        elif line.startswith("Traceback"):
            tb = [ts or time.time(), "Traceback"]
        elif tb and line.strip():
            tb[1] = line.strip()[:160]
    if tb:
        out.append((tb[0], "err", "dispatcher crashed", tb[1]))
    return [p for p in out if p[0] >= since]


def job_problem(j):
    """(severity, kind, detail) for a finished job that went wrong, else None."""
    ex = j.get("exit")
    if j.get("hbc_back_s") is None and j.get("wii_left"):
        return "err", "Wii left", f"HBC not back after the job: {j['wii_left']}"
    if ex == "timeout":
        return "err", "timeout", f"stopped after {j.get('secs')} s (--timeout {j.get('timeout')})"
    if ex == "not started":
        return "err", "not started", "the command could not start"
    if ex not in (0, None):
        return "err", f"exit {ex}", None
    return None


def snapshot(hours=24, sorts=(), filters=(), done_rows=12, log_lines=200, cache=None):
    now = time.time()
    since = now - hours * 3600
    kv, rows = {}, {}

    def row(sec, **f):
        rows.setdefault(sec, []).append(f)

    kv.update(now=int(now), clock=time.strftime("%H:%M:%S"), host=socket.gethostname(), home=str(HOME),
              wii_ip=WII, window_h=f"{hours:g}")
    pid = dispatcher_pid()
    kv["dispatcher_pid"] = pid or "-"
    try:
        st = json.loads(STATE.read_text())
    except (OSError, ValueError):
        st = {}
    kv["dispatcher_job"], kv["dispatcher_detail"] = st.get("job"), st.get("detail")
    kv["dispatcher_since"] = _age(now - st["since"]) if st.get("since") else None

    # This workstation's queue.
    running, pending = (jobs(RUNNING) if RUNNING.exists() else []), (jobs(PENDING) if PENDING.exists() else [])
    for p in running:
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        t0 = _parse_local(j.get("started")) or now
        to = j.get("timeout") or TIMEOUT
        row("running", id=j["id"], name=j.get("name"), agent=(j.get("agent") or "")[:12], started=_when(t0, now),
            elapsed=_age(now - t0), elapsed_s=int(now - t0), timeout=_age(to), left=_age(max(0, to - (now - t0))),
            left_s=int(max(0, to - (now - t0))))
    for i, p in enumerate(pending, 1):
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        t0 = _parse_local(j.get("added")) or now
        row("pending", place=i, place_s=i, id=j["id"], name=j.get("name"), agent=(j.get("agent") or "")[:12],
            added=_when(t0, now), age=_age(now - t0), age_s=int(now - t0))
    kv["running_n"], kv["pending_n"] = len(rows.get("running", [])), len(rows.get("pending", []))
    # dispatcher.state names the last job it worked for; once that job is done it is history.
    live = {r["id"] for r in rows.get("running", []) + rows.get("pending", [])}
    if kv["dispatcher_job"] not in live:
        kv["dispatcher_job"] = None
        kv["dispatcher_detail"] = "idle" if pid else None
        kv["dispatcher_since"] = None

    # Finished jobs: the last few, and every one in the window for problems.
    problems = []
    done = []
    if DONE.exists():
        for p in DONE.glob("*.json"):
            try:
                m = p.stat().st_mtime
            except OSError:
                continue
            done.append((m, p))
    done.sort()
    seen_ids, recent = set(), []
    tail = set(p for _, p in done[-2 * done_rows:])   # candidates: a file's mtime only roughly orders them
    for m, p in done:
        if m < since and p not in tail:
            continue
        try:
            j, last = _job_record(p, m, cache)
        except (OSError, ValueError):
            continue
        tf = _parse_local(j.get("finished")) or m
        recent.append((tf, j))
        pr = job_problem(j) if tf >= since else None
        if pr:
            sev, kind, detail = pr
            detail = detail or last or "no output"
            problems.append(dict(t=tf, severity=sev, kind=kind, host=socket.gethostname(), job=j.get("name"),
                                 id=j["id"], detail=detail))
            seen_ids.add(j["id"])
    recent.sort(key=lambda r: r[0], reverse=True)     # newest first, by when each job finished
    for tf, j in recent[:done_rows]:
        back = j.get("hbc_back_s")
        row("done", finished=_when(tf, now), finished_s=int(tf), id=j["id"], name=j.get("name"),
            exit=j.get("exit"), secs=j.get("secs"), secs_s=j.get("secs") or 0,
            hbc=hbc_word(j),
            chained="yes" if j.get("chained") else "no")
    if recent:
        try:
            tf, j = recent[0]
            back = j.get("hbc_back_s")
            ago = _age(now - tf)
            kv["wii_last"] = (f"{j.get('name')} left it {j.get('wii_left')} ({ago} ago)" if back is None and j.get("wii_left") else
                              f"HBC answered as {j.get('name')} ended ({ago} ago)" if back is not None and back <= HBC_AT_ONCE else
                              f"HBC came back {back:.0f} s after {j.get('name')} ({ago} ago)" if back is not None else
                              f"{j.get('name')} ended {ago} ago, before the queue checked HBC")
        except (OSError, ValueError):
            pass

    # The lease server: who has the Wii, who waits, and every workstation's history.
    url = lease_server()
    kv["server"] = url or "none"
    evs = []
    down = cache is not None and cache.down_until > time.monotonic()
    if url and down:
        kv["server_ok"], kv["server_err"] = "no", cache.down_error
        kv["server_retry"] = f"{cache.down_until - time.monotonic():.0f}"
        problems.append(dict(t=now, severity="err", kind="lease server", host=socket.gethostname(), job="-",
                             id="-", detail=f"unreachable now: {cache.down_error}"))
    if url and not down:
        try:
            s = lease_call("/status", timeout=SERVER_TIMEOUT)
            kv["server_ok"] = "yes"
            h = s.get("holder")
            # Durations from the server (1.9.1); an older one gives only its own clock's times.
            if h:
                kv.update(holder_host=h.get("host"), holder_name=h.get("name"),
                          holder_for=_age(h["held_s"]) if "held_s" in h else f"since {str(h.get('granted'))[-8:]} server time")
            for i, w in enumerate(s.get("waiters") or [], 1):
                ws = w.get("waited_s")
                row("waiting", place=i, place_s=i, host=w.get("host"), name=w.get("name"),
                    since=_when(now - ws, now) if ws is not None else "-",
                    waited=_age(ws) if ws is not None else "?", waited_s=ws or 0)
            left = s.get("left")
            if left:
                ago = f"{_age(left['ago_s'])} ago" if "ago_s" in left else f"since {str(left.get('since'))[-8:]} server time"
                kv.update(left_host=left.get("host"), left_name=left.get("name"), left_wii=left.get("wii"), left_ago=ago)
                problems.append(dict(t=now - left.get("ago_s", 0), severity="err", kind="Wii left", host=left.get("host"),
                                     job=left.get("name"), id="-", detail=f"still: {left.get('wii')} ({ago})"))
        except OSError as e:
            kv["server_ok"], kv["server_err"] = "no", str(e)[:120]
            if cache is not None:
                cache.down_until, cache.down_error = time.monotonic() + SERVER_RETRY, str(e)[:120]
            problems.append(dict(t=now, severity="err", kind="lease server", host=socket.gethostname(), job="-",
                                 id="-", detail=f"unreachable now: {str(e)[:120]}"))
        if kv.get("server_ok") == "yes":           # no point asking a server that did not answer
            try:
                evs = _server_history(since, cache)
                kv["server_history"] = "yes"
            except OSError as e:
                kv["server_history"] = "no"
                problems.append(dict(t=now, severity="note", kind="no history", host=url, job="-", id="-",
                                     detail="the lease server keeps no history: rebuild its container (README step 7)"))
    if not evs:
        try:
            evs = _local_history(since, cache)
            kv["history_src"] = "this workstation"
        except OSError:
            evs = []
    else:
        kv["history_src"] = "lease server"

    # Problems in the history (every workstation's when the server keeps it).
    for e in evs:
        t, typ, host = e.get("t", now), e.get("type"), e.get("host")
        if typ == "job" and e.get("id") not in seen_ids:
            pr = job_problem(e)
            if pr:
                sev, kind, detail = pr
                problems.append(dict(t=t, severity=sev, kind=kind, host=host, job=e.get("name"), id=e.get("id"),
                                     detail=detail or "see the job's log on its workstation"))
                seen_ids.add(e.get("id"))
        elif typ == "release" and e.get("wii", "hbc") != "hbc":
            if not any(p["kind"] == "Wii left" and p["host"] == host and abs(p["t"] - t) < 30 for p in problems):
                problems.append(dict(t=t, severity="err", kind="Wii left", host=host, job=e.get("name"), id="-",
                                     detail=f"released with the Wii {e.get('wii')}"))
        elif typ == "expired":
            if e.get("role") == "holder":
                problems.append(dict(t=t, severity="err", kind="lease expired", host=host, job=e.get("name"), id="-",
                                     detail=f"stopped renewing while holding the Wii ({e.get('held_s')} s in): crashed or offline"))
            else:
                problems.append(dict(t=t, severity="warn", kind="left the line", host=host, job=e.get("name"), id="-",
                                     detail=f"stopped asking after {e.get('waited_s')} s: its dispatcher went away"))
    log = _log_lines(cache)
    for t, sev, kind, detail in dispatcher_log_problems(since, log):
        problems.append(dict(t=t, severity=sev, kind=kind, host=socket.gethostname(), job="-", id="-", detail=detail))
    if pending and not pid:
        problems.append(dict(t=now, severity="warn", kind="no dispatcher", host=socket.gethostname(), job="-", id="-",
                             detail=f"{len(pending)} job(s) queued and no dispatcher: the next add or wait starts one"))
    d = kv.get("dispatcher_detail") or ""
    if d.startswith("Wii busy") and st.get("since") and now - st["since"] > 300:
        problems.append(dict(t=now, severity="warn", kind="Wii busy", host=socket.gethostname(), job="-", id="-",
                             detail=f"{d} (outside the queue?)"))
    rank = {"err": 0, "warn": 1, "note": 2}
    problems.sort(key=lambda p: (-p["t"], rank.get(p["severity"], 3)))
    for p in problems:
        row("errors", when=_when(p["t"], now), when_s=int(p["t"]), severity=p["severity"], kind=p["kind"],
            host=p["host"], job=p["job"], id=p["id"], detail=p["detail"])
    kv["errors_err"] = sum(1 for p in problems if p["severity"] == "err")
    kv["errors_warn"] = sum(1 for p in problems if p["severity"] == "warn")

    # Per workstation, over the window.
    import statistics as st_
    jobs_ = [e for e in evs if e.get("type") == "job"]
    grants = [e for e in evs if e.get("type") == "grant"]
    problems_late = []
    for h in sorted({e.get("host") or "?" for e in jobs_ + grants}):
        hj = [e for e in jobs_ if (e.get("host") or "?") == h]
        hw = [e.get("waited_s", 0) for e in grants if (e.get("host") or "?") == h]
        mins = sum(e.get("secs") or 0 for e in hj) / 60
        row("hosts", host=h, jobs=len(hj), jobs_s=len(hj), failed=sum(1 for e in hj if job_problem(e)),
            wii_min=f"{mins:.0f}", wii_min_s=mins, turns=len(hw), turns_s=len(hw),
            wait_med=_age(st_.median(hw)) if hw else "-", wait_med_s=st_.median(hw) if hw else 0,
            wait_max=_age(max(hw)) if hw else "-", wait_max_s=max(hw) if hw else 0)
        if hw and not hj and kv.get("history_src") == "lease server":
            # Turns but no job records: that workstation's dispatcher predates 1.9.1's /event.
            problems_late.append(dict(t=max(e["t"] for e in grants if (e.get("host") or "?") == h), severity="note",
                                      kind="old dispatcher", host=h, job="-", id="-",
                                      detail=f"{len(hw)} turn(s) and no job records: update its hbc-reborn (git pull)"))
    busy = sum(e.get("secs") or 0 for e in jobs_)
    kv["window_jobs"], kv["window_busy"] = len(jobs_), f"{100 * busy / (hours * 3600):.0f}%"
    for p in problems_late:
        row("errors", when=_when(p["t"], now), when_s=int(p["t"]), severity=p["severity"], kind=p["kind"],
            host=p["host"], job=p["job"], id=p["id"], detail=p["detail"])
    rel, gaps = None, []
    for e in evs:
        if e.get("type") == "release":
            rel = e
        elif e.get("type") == "grant" and rel and e.get("waited_s", 0) > e["t"] - rel["t"]:
            gaps.append(e["t"] - rel["t"])
            rel = None
    if gaps:
        kv["handover"] = (f"{len(gaps)} hand-over(s) to a waiting workstation, median "
                          f"{1000 * st_.median(gaps):.0f} ms, longest {1000 * max(gaps):.0f} ms")
    for e in sorted(grants, key=lambda e: e.get("waited_s", 0), reverse=True)[:8]:
        if e.get("waited_s", 0) >= 60:
            row("waits", when=_when(e["t"], now), when_s=int(e["t"]), waited=_age(e["waited_s"]),
                waited_s=e["waited_s"], host=e.get("host"), name=e.get("name"))

    for l in log[-log_lines:]:
        row("log", line=l)

    # Sort and filter, the monitor's --sort/--filter and header clicks: a field with a
    # numeric twin (elapsed and elapsed_s) sorts by the number.
    for spec in filters:
        sec, _, text = spec.partition(":")
        if sec in rows and text:
            t = text.lower()
            rows[sec] = [r for r in rows[sec] if any(t in str(v).lower() for k, v in r.items() if not k.endswith("_s"))]
    for spec in sorts:
        sec, _, rest = spec.partition(":")
        field, _, desc = rest.partition(":")
        if sec in rows and field:
            key = field + "_s" if field + "_s" in (rows[sec][0] if rows[sec] else {}) else field
            def k(r, key=key):
                v = r.get(key)
                return (0, v) if isinstance(v, (int, float)) else (1, str(v))
            rows[sec].sort(key=k, reverse=desc in ("1", "d", "desc"))
    return kv, rows


SNAPSHOT_FIELDS = {
    "running": ("id", "name", "agent", "started", "elapsed", "timeout", "left"),
    "pending": ("place", "id", "name", "agent", "added", "age"),
    "waiting": ("place", "host", "name", "since", "waited"),
    "done": ("finished", "id", "name", "exit", "secs", "hbc", "chained"),
    "errors": ("when", "severity", "kind", "host", "job", "id", "detail"),
    "hosts": ("host", "jobs", "failed", "wii_min", "turns", "wait_med", "wait_max"),
    "waits": ("when", "waited", "host", "name"),
    "log": ("line",),
}


def snapshot_lines(kv, rows):
    out = [f"@kv\t{k}\t{_cell(v)}" for k, v in kv.items()]
    for sec, fields in SNAPSHOT_FIELDS.items():
        for r in rows.get(sec, []):
            out.append("\t".join(["@row", sec] + [_cell(r.get(f)) for f in fields]))
    return out


def serve_snapshots(default_hours):
    """monitor.sh's coprocess: one request per line on stdin, HOURS<TAB>SORTS<TAB>FILTERS
    (sorts and filters joined by \\x1f), answered with a snapshot and a line "@end". Staying up
    saves a Python start per refresh and keeps the SnapCache; it ends when stdin closes."""
    cache = SnapCache()
    while True:
        line = sys.stdin.readline()
        if not line:
            return
        parts = line.rstrip("\r\n").split("\t") + ["", ""]
        try:
            hours = float(parts[0]) if parts[0] else default_hours
            kv, rows = snapshot(hours, [x for x in parts[1].split("\x1f") if x],
                                [x for x in parts[2].split("\x1f") if x], cache=cache)
            out = snapshot_lines(kv, rows)
        except Exception as e:                     # say so on screen; the next request tries again
            out = [f"@kv\terror\t{_cell(f'{type(e).__name__}: {e}')}"]
        sys.stdout.buffer.write(("\n".join(out + ["@end"]) + "\n").encode("utf-8"))
        sys.stdout.flush()


def cmd_snapshot(a):
    if a.serve:
        return serve_snapshots(a.hours)
    kv, rows = snapshot(a.hours, a.sort or (), a.filter or ())
    if a.json:
        print(json.dumps({"kv": kv, "rows": rows}, indent=1))
        return
    out = snapshot_lines(kv, rows)
    sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))   # a pipe on Windows is cp1252 otherwise
    sys.stdout.flush()


def cmd_cancel(a):
    d, p = find(a.id)
    if d != PENDING:
        sys.exit(f"{a.id}: {'not found' if d is None else 'already ' + d.name}")
    fs_retry(p.unlink)
    print(f"cancelled {a.id}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="op", required=True)
    s = sub.add_parser("add"); s.add_argument("--name"); s.add_argument("--cwd", default=".")
    s.add_argument("--timeout", type=int, default=TIMEOUT); s.add_argument("--agent")
    s.add_argument("cmd", nargs=argparse.REMAINDER)
    s = sub.add_parser("run"); s.add_argument("--idle", type=float, default=20)
    s = sub.add_parser("status"); s.add_argument("--last", type=int, default=5)
    s = sub.add_parser("wait"); s.add_argument("id"); s.add_argument("--tail", type=int, default=25)
    s.add_argument("--every", type=float, default=60, help="seconds between progress lines (stderr)")
    s = sub.add_parser("cancel"); s.add_argument("id")
    s = sub.add_parser("serve"); s.add_argument("--port", type=int, default=4310); s.add_argument("--ttl", type=float, default=TTL)
    s.add_argument("--host", default="0.0.0.0")
    s.add_argument("--history", help="keep the history (events, rotated and archived) in this directory")
    s.add_argument("--keep-months", type=int, default=0, help="months of archives to keep (0: forever)")
    s = sub.add_parser("snapshot", help="what monitor.sh shows, as tab-separated lines (never touches the Wii)")
    s.add_argument("--hours", type=float, default=24); s.add_argument("--json", action="store_true")
    s.add_argument("--serve", action="store_true", help="answer requests on stdin (monitor.sh's coprocess)")
    s.add_argument("--sort", action="append", help="SECTION:FIELD[:desc]")
    s.add_argument("--filter", action="append", help="SECTION:TEXT")
    s = sub.add_parser("report")
    g = s.add_mutually_exclusive_group()
    g.add_argument("--hours", type=float, default=12); g.add_argument("--days", type=float)
    g.add_argument("--since", help="YYYY-MM-DD or 'YYYY-MM-DD HH:MM', local time")
    s.add_argument("--local", action="store_true", help="this workstation's history, not the server's")
    s.add_argument("--json", action="store_true", help="the events themselves")
    s = sub.add_parser("setup"); s.add_argument("--server", help="the lease server's URL, e.g. http://homeserver:4310; '' for none")
    a = ap.parse_args()
    if a.op == "add" and a.cmd[:1] == ["--"]:
        a.cmd = a.cmd[1:]
    {"add": cmd_add, "run": cmd_run, "status": cmd_status, "wait": cmd_wait, "cancel": cmd_cancel,
     "serve": cmd_serve, "setup": cmd_setup, "report": cmd_report,
     "snapshot": cmd_snapshot}[a.op](a)


if __name__ == "__main__":
    main()
