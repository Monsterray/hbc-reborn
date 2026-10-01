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
                             "since": time.strftime("%Y-%m-%d %H:%M:%S")}
            self.holder = None
        if d["ticket"] in self.waiters:
            w = self.waiters.pop(d["ticket"])
            self._event("leave", w, waited_s=round(now - w["joined"], 1))
        return {"ok": True}

    def _show(self, e, now):
        return e and {k: v for k, v in dict(e, age=int(now - e["seen"])).items() if k not in self.PRIVATE}

    def status(self):
        now = self.clock()
        self._expire(now)
        return {"holder": self._show(self.holder, now), "waiters": [self._show(w, now) for w in self.waiters.values()],
                "left": self.left}


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
            if not self.path.startswith(("/status", "/acquire", "/renew", "/event", "/history")):   # the polls are noise
                super().log_message(fmt, *args)

    srv = http.server.ThreadingHTTPServer((host, port), Handler)
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


def lease_call(path, body=None, timeout=10):
    import urllib.request
    req = urllib.request.Request(lease_server() + path, method="POST" if body is not None else "GET",
                                 data=body is not None and json.dumps(body).encode() or None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


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
    tmp.rename(PENDING / f"{job_id}.json")
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
    p.rename(r)                                    # claims it: a rename is atomic
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
    log.rename(DONE / log.name)
    r.unlink()
    print(f"{job['finished']} done {job['id']} {job['name']}: exit {job['exit']} in {job['secs']} s", flush=True)
    return job


def finish_job(job, turn):
    """After the HBC check: the job's record gets it, and the job goes into the history,
    the lease server's (every workstation's) and this workstation's own."""
    p = DONE / f"{job['id']}.json"
    tmp = DONE / f"{job['id']}.json.tmp"
    tmp.write_text(json.dumps(job, indent=1))
    os.replace(tmp, p)
    ev = {"type": "job", "host": socket.gethostname(), **{k: job.get(k) for k in (
        "id", "name", "agent", "added", "started", "finished", "secs", "exit", "timeout",
        "hbc_back_s", "wii_left", "chained")}}
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
    """After a job, while this dispatcher still holds the Wii: seconds until HBC's menu
    answers, or None and what answers instead. Probes only once the job's process is gone,
    so never during a run."""
    t0 = time.monotonic()
    while True:
        t = time.monotonic()
        state, text = wii_state()
        if state == "hbc":
            return round(t - t0, 1), "hbc"
        if t - t0 >= limit:
            return None, f"busy: {agent_app()} is running" if state == "agent" else "busy or off"
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
            p.rename(PENDING / p.name)
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
                    back_s, turn.wii = hbc_back()
                    job.update(hbc_back_s=back_s, chained=chained)
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


def cmd_cancel(a):
    d, p = find(a.id)
    if d != PENDING:
        sys.exit(f"{a.id}: {'not found' if d is None else 'already ' + d.name}")
    p.unlink()
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
     "serve": cmd_serve, "setup": cmd_setup, "report": cmd_report}[a.op](a)


if __name__ == "__main__":
    main()
