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
wait. Jobs run in the order received, across every workstation: the lease server numbers each
job as add reports it, the Wii goes to the earliest-received, and a turn goes on while its
workstation's next job is also the next received (jobs received one after the other stack up;
the same agent's after one probe). With nothing queued anywhere the lease is held CHAIN_GRACE
seconds for this workstation's next add. After each job the dispatcher, still holding the Wii,
checks that HBC came back (RETURN_WAIT s), collects the job's --collect paths into its run
folder (runs/ID, WII_BENCH_OUT) before the next job can overwrite them, and says so in the
job's record, the release and the history; old runs roll over into runs/archive (`runs`,
`results ID --to DIR` find them, live or archived). The queue never
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
RUNS = HOME / "runs"                               # runs/<id>/: each job's data (WII_BENCH_OUT)
ARCHIVE = RUNS / "archive"                         # archive/YYYY-MM/<id>.tar.gz, index.jsonl
HBC_PY = pathlib.Path(__file__).resolve().parent.parent / "hbc.py"


def _env_num(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return float(default)


KEEP_DAYS = _env_num("WII_BENCH_KEEP_DAYS", 14)      # days a job's run stays live before it is archived
RUNS_MAX_GB = _env_num("WII_BENCH_RUNS_MAX_GB", 20)  # live runs past this are archived, oldest first
ARCHIVE_MAX_GB = _env_num("WII_BENCH_ARCHIVE_MAX_GB", 100)   # archived months past this go, oldest first
COLLECT_TIMEOUT = 600                              # s for one --collect from the Wii
WII = os.environ.get("WII_BENCH_IP", "192.168.8.213")
PORT = int(os.environ.get("WII_BENCH_PORT", "4299"))  # another port only for tests
LOG_PORT_BASE = 4300  # the first Wii's jobs get TCP 4300 for the network log, the next 4301, ...


def parse_pool(spec):
    """'name=ip[:port] [pinned],...' (or one per line) -> the Wiis, in order: the first is the
    one dispatchers before 1.10.8 use, and gets log port LOG_PORT_BASE. A Wii marked `pinned`
    takes only jobs added with --wii NAME, never one for any Wii: a console with real data on
    it (the production Wiis) is on the queue so that nothing collides there, not to be picked."""
    pool = []
    for item in spec.replace("\n", ",").split(","):
        item = item.split("#", 1)[0].strip()
        if not item:
            continue
        name, _, addr = item.partition("=")
        if not addr:
            name, addr = f"wii{len(pool) + 1}", name
        addr, *marks = addr.split()
        ip, _, port = addr.partition(":")
        pool.append({"name": name.strip(), "ip": ip, "port": int(port or 4299), "pinned": "pinned" in marks})
    for i, w in enumerate(pool):
        w["log_port"] = LOG_PORT_BASE + i
    return pool


def default_pool(ip=WII):
    """The Wiis: $WII_BENCH_WIIS, else the file HOME/wiis, else the one Wii at `ip`, named
    "bench". A lease server with no pool set passes ip None: each workstation then uses its own
    $WII_BENCH_IP, as before 1.10.8."""
    spec = os.environ.get("WII_BENCH_WIIS")
    if spec is None:
        try:
            spec = read_setting(HOME / "wiis")
        except (OSError, UnicodeDecodeError):
            spec = ""
    return parse_pool(spec) or [{"name": "bench", "ip": ip, "port": PORT if ip else None, "log_port": LOG_PORT_BASE}]


def bench_wii(wii=None):
    """The Wii to talk to: one from the pool (a lease grant's), else the one at $WII_BENCH_IP."""
    if not wii:
        return {"name": "bench", "ip": WII, "port": PORT, "log_port": LOG_PORT_BASE}
    return dict(wii, ip=wii.get("ip") or WII, port=wii.get("port") or PORT)


PINNED = set()        # the Wiis that take only jobs pinned to them (the pool's `pinned` mark)


def may_use(job, slot):
    """True if the job may run on the Wii named `slot` (add --wii pins it to one; a pinned-only
    Wii takes nothing else)."""
    want = job.get("want_wii") or "any"
    return want == slot or (want == "any" and slot not in PINNED)
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


def wii_state(wii=None):
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
    w = bench_wii(wii)
    try:
        with socket.create_connection((w["ip"], w["port"]), timeout=2) as s:
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


def hbc_idle(wii=None):
    """True only for HBC in its menu: an agent app answering on 4299 is busy."""
    return wii_state(wii)[0] == "hbc"


def agent_app(wii=None):
    """The running agent app's name, from its status reply (HBCS), or "an app"."""
    w = bench_wii(wii)
    try:
        with socket.create_connection((w["ip"], w["port"]), timeout=2) as s:
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


def wii_line(wii=None):
    state, text = wii_state(wii)
    if state == "hbc":
        return f"in HBC {text} (free)" if text else "in HBC (free)"
    if state == "agent":
        return f"busy: {agent_app(wii)} is running (agent {text.removesuffix(' agent')})"
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
# A holder's phase while it waits for HBC to be idle before a job. It keeps the lease meanwhile
# (whoever is next would only wait on the same Wii), so the time is accounted for instead: the
# MacBook's turns of 10.4 h and 5.9 h on 2026-10-03/04 looked like jobs, and were most likely
# a Wii left out of HBC (WiiXplorer) with the next job waiting for it.
WAITING_FOR_HBC = "waiting for HBC"
QUEUE_MAX = 50        # jobs per workstation the server keeps (and the total, for the rest)
REQUEST_TTL = 3600    # s a cancel or stop waits for the workstation that has the job
RESERVE = 10          # s the Wii waits for the workstation with the earliest job to ask for it


class Leases:
    """The server's state: a pool of Wiis, each with its own lease. Every client call refreshes
    its entry; one not heard from for ttl seconds is dropped, so a dead workstation frees its Wii
    by itself. After a restart nothing is granted for one ttl: a holder still running its job
    renews within that time and gets its lease back. `log` gets each event (the history).

    Each waiter waits with one job: the Wii it wants ("any", or a Wii's name) and when that job
    was received. A free Wii goes to the earliest-received job that may use it. A dispatcher
    before 1.10.8 says nothing of Wiis: it only ever talks to the first Wii in the pool, so that
    is the only one it is given.

    A release says what its Wii was doing when the holder let go ("hbc", or what answered
    instead). Only a release with the Wii back in HBC counts as clean for the next holder's short
    idle wait; any other is kept as that Wii's `left` and shown until a clean release."""

    PRIVATE = ("seen", "ticket", "joined", "granted_at", "phase_at", "hbc_wait")   # seq_join is shown: the order

    def __init__(self, ttl=TTL, clock=time.monotonic, log=None, pool=None):
        self.ttl, self.clock, self.log = ttl, clock, log or (lambda ev: None)
        self.pool = pool or default_pool()
        self.names = [w["name"] for w in self.pool]
        self.default = self.names[0]
        self.pinned = {w["name"] for w in self.pool if w.get("pinned")}
        self.slots = {n: {"holder": None, "released_at": None, "left": None, "released_by": None} for n in self.names}
        self.waiters = {}
        self.grace_until = clock() + ttl
        self.queues = {}                           # qid -> a workstation's queued jobs (QUEUE_MAX)
        # Receive order: every job gets a number when the server first hears of it (add tells it
        # at once), and a Wii goes to the earliest-received job, whichever workstation has it.
        self.seq, self.received = 0, {}
        # Cancels and stops for jobs on other workstations (from the monitor's manage page or
        # `cancel`/`stop` there), handed to the workstation's dispatcher with its next acquire or
        # renew: job id -> ("cancel"|"stop", when, owner qid). Kept REQUEST_TTL s.
        self.requests = {}
        self.jobs_logged = {}                      # job id -> when its record was logged (no twice)

    # The first Wii's lease, as one-Wii code and tests know it.
    @property
    def holder(self):
        return self.slots[self.default]["holder"]

    @holder.setter
    def holder(self, value):
        self.slots[self.default]["holder"] = value

    def wii_info(self, name):
        w = next(x for x in self.pool if x["name"] == name)
        return {"slot": name, "ip": w["ip"], "port": w["port"], "log_port": w.get("log_port", LOG_PORT_BASE),
                "pinned": bool(w.get("pinned"))}

    def _want(self, d):
        """The Wii a waiter or a queued job may use: "any", a Wii's name, or (said nothing:
        a client before 1.10.8) the first Wii, the one it knows."""
        return d.get("want") or self.default

    def _eligible(self, want, slot):
        return want == slot or (want == "any" and slot not in self.pinned)

    def _held_by(self, ticket):
        return next((n for n, s in self.slots.items() if s["holder"] and s["holder"]["ticket"] == ticket), None)

    def queue(self, d):
        """A workstation's queued jobs, from /queue or with an acquire, renew or release. `qid` is
        the workstation and its state folder (two queues on one host stay apart); `queue_at` is
        the sender's clock, compared only with the same sender's, so a list sent before a newer
        one (a long poll started 20 s ago) cannot replace it."""
        now = self.clock()
        q, jobs_ = d.get("qid"), d.get("queue")
        if not q or not isinstance(jobs_, list):
            return {"ok": False}
        old = self.queues.get(q)
        if old and old["queue_at"] > d.get("queue_at", 0):
            return {"ok": True}
        listed = []
        for j in jobs_[:QUEUE_MAX]:                # a workstation's own list is in its receive order
            if not isinstance(j, dict) or not j.get("id"):
                continue
            if self.requests.get(j["id"], ("",))[0] == "cancel":
                continue                           # cancelled: its workstation has not heard yet
            if j["id"] not in self.received:
                self.seq += 1
                self.received[j["id"]] = self.seq
            listed.append(dict(j, seq=self.received[j["id"]], want=self._want(j)))
        self.queues[q] = {"qid": q, "host": d.get("host"), "jobs": listed, "total": d.get("total", len(jobs_)),
                          "queue_at": d.get("queue_at", 0), "seen": now}
        return {"ok": True}

    def request(self, d):
        """Cancel or stop jobs: by id, or every queued job of an agent, a host, or all."""
        now = self.clock()
        action = d.get("action")
        if action not in ("cancel", "stop"):
            return {"ok": False, "error": "action: cancel or stop"}
        ids = set(d.get("ids") or [])
        if action == "cancel" and (d.get("agent") or d.get("host") or d.get("all")):
            for q in self.queues.values():
                for j in q["jobs"]:
                    if (d.get("all") or (d.get("host") and q["host"] == d["host"])
                            or (d.get("agent") and j.get("agent") and str(d["agent"]).startswith(j["agent"]))):
                        ids.add(j["id"])
        for i in ids:                              # whose job it is: the list that has it, or a holder
            owner = next((q["qid"] for q in self.queues.values() if any(j["id"] == i for j in q["jobs"])), None)
            if owner is None:
                owner = next((s["holder"].get("qid") for s in self.slots.values()
                              if s["holder"] and s["holder"].get("job") == i), None)
            self.requests[i] = (action, now, owner)
        if action == "cancel":                     # off every list now, so monitors stop showing them
            for q in self.queues.values():
                q["jobs"] = [j for j in q["jobs"] if j["id"] not in ids]
        self.log({"type": action, "ids": sorted(ids), "by": d.get("by"), "host": d.get("by_host")})
        return {"ok": True, "ids": sorted(ids)}

    def requests_for(self, qid, now):
        """The cancels and stops waiting for this workstation: its own jobs', and those of jobs
        no list knew (a dispatcher before 1.10.4), which go to every workstation until they expire."""
        out = {"cancel": [], "stop": []}
        for i, (action, t, owner) in list(self.requests.items()):
            if now - t > REQUEST_TTL:
                del self.requests[i]
            elif owner in (qid, None):
                out[action].append(i)
        if not out["cancel"] and not out["stop"]:
            return None
        return out

    def done_with(self, ids, qid=None):
        """The workstation handled these: its own are done (one nobody owned waits for its TTL)."""
        for i in ids or []:
            if i in self.requests and self.requests[i][2] is not None and self.requests[i][2] == qid:
                del self.requests[i]

    def _head_seq(self, w):
        """When the job a waiter waits with was received: that job (from 1.10.8), else its
        queue's first job, else (a dispatcher before 1.10.2 sends no queue) when it joined."""
        if w.get("head") in self.received:
            return self.received[w["head"]]
        q = self.queues.get(w.get("qid"))
        return q["jobs"][0]["seq"] if q and q["jobs"] else w["seq_join"]

    def _line(self, slot):
        """The waiters that may use this Wii, in the order it serves them."""
        ws = [w for w in self.waiters.values() if self._eligible(w["want"], slot)]
        return sorted(ws, key=lambda w: (self._head_seq(w), w["seq_join"]))

    def _reserved(self, slot, w, now):
        """True while a job received before w's, that may use this Wii, belongs to a workstation
        not in line yet whose queue came in less than RESERVE s ago: its dispatcher is starting,
        or about to ask, and the Wii waits for it rather than go to a job received later."""
        mine = self._head_seq(w)
        for q in self.queues.values():
            if q["qid"] == w.get("qid") or now - q["seen"] >= RESERVE:
                continue
            if any(x.get("qid") == q["qid"] for x in self.waiters.values()):
                continue
            if any(j["seq"] < mine and self._eligible(j["want"], slot) for j in q["jobs"]):
                return True
        return False

    def next_up(self, slot=None):
        """For a Wii: the owner (qid, or an old waiter's ticket) and id of the earliest-received
        job that may use it, or None."""
        slot = slot or self.default
        best = None
        for q in self.queues.values():
            for j in q["jobs"]:
                if self._eligible(j["want"], slot) and (best is None or j["seq"] < best[0]):
                    best = (j["seq"], q["qid"], j["id"])
        for t, w in self.waiters.items():
            if not (w.get("qid") in self.queues and self.queues[w["qid"]]["jobs"]) and self._eligible(w["want"], slot):
                if best is None or w["seq_join"] < best[0]:
                    best = (w["seq_join"], t, None)
        return best and (best[1], best[2])

    def _event(self, kind, e, **more):
        self.log(dict({"type": kind, "host": e.get("host"), "name": e.get("name")}, **more))

    def _expire(self, now):
        for name, s in self.slots.items():
            h = s["holder"]
            if h and now - h["seen"] > self.ttl:
                self._event("expired", h, role="holder", slot=name, held_s=round(now - h.get("granted_at", now), 1))
                s["holder"] = None
        for t in [t for t, w in self.waiters.items() if now - w["seen"] > self.ttl]:
            self._event("expired", self.waiters[t], role="waiter", waited_s=round(now - self.waiters[t]["joined"], 1))
            del self.waiters[t]
        for q in [q for q, v in self.queues.items() if now - v["seen"] > self.ttl or not v["jobs"]]:
            del self.queues[q]                     # a workstation gone quiet, or with nothing queued
        if len(self.received) > 4 * QUEUE_MAX:     # forget numbers of jobs no list has any more
            listed = {j["id"] for v in self.queues.values() for j in v["jobs"]}
            listed |= {w.get("head") for w in self.waiters.values()}
            self.received = {k: v for k, v in self.received.items() if k in listed}

    def _strip(self, d):
        if "queue" in d:
            self.queue(d)
        return {k: v for k, v in d.items() if k not in ("queue", "queue_at", "total", "handled", "wait")}

    def acquire(self, d):
        now = self.clock()
        self.done_with(d.get("handled"), d.get("qid"))
        d = self._strip(d)
        self._expire(now)
        t = d["ticket"]
        reqs = self.requests_for(d.get("qid"), now) if d.get("qid") else None
        held = self._held_by(t)
        if held:
            self.slots[held]["holder"]["seen"] = now
            return dict({"granted": True}, **self.wii_info(held))
        if t not in self.waiters:
            self.seq += 1
            self.waiters[t] = dict(d, want=self._want(d), since=time.strftime("%Y-%m-%d %H:%M:%S"), joined=now,
                                   seq_join=self.seq)
            self._event("join", d, place=len(self.waiters), want=self._want(d))
        w = self.waiters[t]
        w["seen"] = now
        if "head" in d:                            # its job was cancelled: it waits with the next
            w.update(head=d["head"], want=self._want(d), name=d.get("name", w.get("name")))
        if now >= self.grace_until:
            for slot in self.names:                # a free Wii it may use, whose next job is its
                s = self.slots[slot]
                if s["holder"] is not None or not self._eligible(w["want"], slot):
                    continue
                line = self._line(slot)
                if line and line[0] is w and not self._reserved(slot, w, now):
                    s["holder"] = self.waiters.pop(t)
                    s["holder"].update(granted=time.strftime("%Y-%m-%d %H:%M:%S"), granted_at=now, slot=slot)
                    # How long ago a queued job last let go of this Wii with it in HBC: it came back
                    # from a queue job, not from someone outside the queue, so the client can skip
                    # most of its idle wait.
                    ago = None if s["released_at"] is None else round(now - s["released_at"], 1)
                    self._event("grant", w, slot=slot, waited_s=round(now - w["joined"], 1), released_ago=ago)
                    r = dict({"granted": True, "released_ago": ago, "released_by": s["released_by"]}, **self.wii_info(slot))
                    if reqs:                       # cancels for the jobs it got the Wii with
                        r["requests"] = reqs
                    return r
        mine = [n for n in self.names if self._eligible(w["want"], n)]
        place = min((next((i for i, x in enumerate(self._line(n)) if x is w), 0) for n in mine), default=0)
        busy = next((self.slots[n]["holder"] for n in mine if self.slots[n]["holder"]), None)
        r = {"granted": False, "position": place + 1, "holder": self._show(busy, now)}
        if w["want"] != "any" and w["want"] not in self.names:
            r["error_want"] = f"no Wii named {w['want']} here: {', '.join(self.names)}"
        if reqs:
            r["requests"] = reqs
        return r

    def renew(self, d):
        now = self.clock()
        self.done_with(d.get("handled"), d.get("qid"))
        d = self._strip(d)
        self._expire(now)
        t = d["ticket"]
        slot = self._held_by(t)
        if slot is None and t not in self.waiters:   # after a restart: reclaim the Wii it had
            want = d.get("slot") if d.get("slot") in self.names else self.default
            if self.slots[want]["holder"] is None:
                slot = want
                h = dict(d, since=time.strftime("%Y-%m-%d %H:%M:%S"), granted="reclaimed", granted_at=now,
                         slot=slot, want=self._want(d))
                if d.get("phase") is not None:     # its phase began no later than now (it was 0 s for ever)
                    h["phase_at"] = now
                self.slots[slot]["holder"] = h
                self._event("reclaim", d, slot=slot)
        if slot is None:
            return {"ok": False, "waiting": len(self.waiters)}
        h = self.slots[slot]["holder"]
        h["seen"] = now
        if d.get("name"):                          # the job the turn is for now (the first was cancelled)
            h["name"] = d["name"]
        h["job"] = d.get("job")                    # the job running now, if one is
        if d.get("phase") is not None and d["phase"] != h.get("phase"):
            self._phase_end(h, now)
            h["phase"], h["phase_at"] = d["phase"], now
        nxt = self.next_up(slot)
        r = dict({"ok": True, "waiting": len(self._line(slot)), "next": nxt and nxt[0], "next_job": nxt and nxt[1],
                  "next_is_mine": nxt is not None and nxt[0] == h.get("qid")}, **self.wii_info(slot))
        reqs = self.requests_for(h.get("qid"), now) if h.get("qid") else None
        if reqs:
            r["requests"] = reqs
        return r

    def _phase_end(self, h, now):
        """A holder's phase ends: time spent waiting for HBC adds up for the release event."""
        if h.get("phase", "").startswith(WAITING_FOR_HBC) and "phase_at" in h:
            h["hbc_wait"] = h.get("hbc_wait", 0.0) + now - h["phase_at"]

    def release(self, d):
        now = self.clock()
        self.done_with(d.get("handled"), d.get("qid"))
        if "queue" in d:                           # what is left queued there, as it lets go
            self.queue(d)
        slot = self._held_by(d["ticket"])
        if slot:
            s, h = self.slots[slot], self.slots[slot]["holder"]
            wii = d.get("wii", "hbc")              # a client before 1.9.1 does not say
            self._phase_end(h, now)
            more = {"hbc_wait_s": round(h["hbc_wait"], 1)} if h.get("hbc_wait") else {}
            self._event("release", h, slot=slot, held_s=round(now - h.get("granted_at", now), 1), wii=wii, **more)
            if wii == "hbc":
                s["released_at"], s["left"] = now, None
                s["released_by"] = {"host": h.get("host"), "name": h.get("name")}
            else:
                s["released_at"] = None
                s["left"] = {"host": h.get("host"), "name": h.get("name"), "wii": wii,
                             "since": time.strftime("%Y-%m-%d %H:%M:%S"), "at": now}
            s["holder"] = None
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
        if "phase_at" in e:
            more["phase_s"] = int(now - e["phase_at"])
        elif "joined" in e:
            more["waited_s"] = int(now - e["joined"])
        return {k: v for k, v in dict(e, **more).items() if k not in self.PRIVATE}

    def _left(self, s, now):
        return s["left"] and {k: v for k, v in dict(s["left"], ago_s=int(now - s["left"]["at"])).items() if k != "at"}

    def status(self):
        now = self.clock()
        self._expire(now)
        queues = [{"qid": v["qid"], "host": v["host"], "jobs": v["jobs"], "total": v["total"],
                   "age_s": round(now - v["seen"], 1)} for v in self.queues.values()]
        line = sorted(self.waiters.values(), key=lambda w: (self._head_seq(w), w["seq_join"]))   # the order served
        wiis = [dict(self.wii_info(n), name=n, holder=self._show(s["holder"], now), left=self._left(s, now),
                     released_ago=None if s["released_at"] is None else round(now - s["released_at"], 1))
                for n, s in self.slots.items()]
        d = self.slots[self.default]
        return {"holder": self._show(d["holder"], now), "waiters": [self._show(w, now) for w in line],
                "left": self._left(d, now), "queues": queues, "wiis": wiis}


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


def make_server(port, ttl=TTL, host="0.0.0.0", history=None, pool=None):
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

    leases, cond = Leases(ttl, log=log, pool=pool or default_pool(None)), threading.Condition()

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
                with cond:
                    seen = d.get("type") == "job" and d.get("id") in leases.jobs_logged
                    if d.get("type") == "job" and d.get("id"):
                        leases.jobs_logged[d["id"]] = time.monotonic()
                        if len(leases.jobs_logged) > 5000:
                            for k in sorted(leases.jobs_logged, key=leases.jobs_logged.get)[:1000]:
                                del leases.jobs_logged[k]
                if not seen:                       # a retry of one already logged: once is enough
                    log(d)
                return self.reply({"ok": True})
            if self.path == "/queue":              # a workstation's queued jobs, from add or cancel
                with cond:
                    r = leases.queue(d)
                return self.reply(r, 200 if r["ok"] else 400)
            if self.path == "/request":            # cancel or stop jobs, wherever they are
                with cond:
                    r = leases.request(d)
                    cond.notify_all()              # a waiter whose job it is hears at once
                return self.reply(r, 200 if r["ok"] else 400)
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
                if op == leases.acquire:           # a long poll: answer once granted, a cancel, or after wait
                    end = time.monotonic() + wait
                    while not r["granted"] and not r.get("requests") and time.monotonic() < end:
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
    pool = parse_pool(",".join(a.wii)) if a.wii else default_pool(None)
    srv = make_server(a.port, a.ttl, a.host, a.history, pool)
    if srv.history:
        srv.history.keep = a.keep_months
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))   # PID 1 in Docker ignores it otherwise
    print("Wiis: " + ", ".join(f"{w['name']} {w['ip'] or '(each workstation its own WII_BENCH_IP)'}" for w in pool),
          flush=True)
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


class LocalLeases:
    """The lease server's logic inside the dispatcher, for a workstation without a server: the
    same pool of Wiis and receive order, for this workstation's queue alone. It answers like
    lease_call."""

    def __init__(self, pool):
        self.leases = Leases(TTL, pool=pool)
        self.leases.grace_until = 0                # no holder from before: this process is all there is
        self.cond = threading.Condition()

    def __call__(self, path, body=None, timeout=10):
        d = dict(body or {})
        with self.cond:
            if path == "/status":
                return self.leases.status()
            if path == "/queue":
                return self.leases.queue(d)
            if path in ("/event", "/request"):    # the local history has it; nobody else to ask
                return {"ok": True}
            op = {"/acquire": self.leases.acquire, "/renew": self.leases.renew, "/release": self.leases.release}[path]
            r = op(d)
            if op == self.leases.acquire:
                end = time.monotonic() + min(float(d.get("wait", 0)), 30.0)
                while not r["granted"] and time.monotonic() < end:
                    self.cond.wait(min(1.0, end - time.monotonic()))
                    r = op(d)
            else:
                self.cond.notify_all()
            return r


class Claims:
    """Which pending jobs this dispatcher's turns have: each turn waits for a Wii with a job of its
    own, so two turns never run the same one."""

    def __init__(self):
        self.lock, self.owner = threading.Lock(), {}

    def take(self, job_id, who):
        with self.lock:
            if self.owner.get(job_id, who) is not who:
                return False
            self.owner[job_id] = who
            return True

    def move(self, old, new):
        """Everything `old` has goes to `new` at once: no other turn can take it in between."""
        with self.lock:
            for k, v in self.owner.items():
                if v is old:
                    self.owner[k] = new

    def drop(self, who, job_id=None):
        with self.lock:
            for k in [k for k, v in self.owner.items() if v is who and job_id in (None, k)]:
                del self.owner[k]

    def free(self, slot=None):
        """The pending jobs no turn has, in receive order, that may use `slot` (None: any Wii)."""
        out = []
        for p in jobs(PENDING) if PENDING.exists() else []:
            if p.stem in self.owner:
                continue
            try:
                j = load(p)
            except (OSError, ValueError):
                continue                           # cancelled meanwhile
            if slot is None or may_use(j, slot):
                out.append((p, j))
        return out

    def next(self, who, slot=None, only=None):
        """Take the earliest free job that may use `slot` (or just `only`): its path, or None."""
        for p, _ in self.free(slot):
            if only and p.stem != only:
                continue
            if self.take(p.stem, who):
                return p
        return None


CLAIMS = Claims()


_SAY = threading.Lock()


def say(msg):
    """A line in dispatcher.log; with more than one Wii, each turn's lines say which Wii."""
    tag = getattr(_LOCAL, "tag", "")
    with _SAY:
        print(time.strftime("%Y-%m-%d %H:%M:%S ") + (f"[{tag}] " if tag else "") + msg, flush=True)


STATE = HOME / "dispatcher.state"
_STATE, _STATE_LOCK = {}, threading.Lock()


def report(job_id, detail, wii=None):
    """What the dispatcher is doing for each job it has a turn for, for `wait` and the monitor:
    {"jobs": {id: {detail, since, wii}}}, and the latest one as job/detail/since (before 1.10.8
    that was all there was, one turn at a time)."""
    if job_id is None:
        return
    with _STATE_LOCK:
        now = time.time()
        _STATE[job_id] = {"detail": detail, "since": now, "wii": wii}
        for k in list(_STATE):                     # jobs done or cancelled are history
            if k != job_id and not (PENDING / f"{k}.json").exists() and not (RUNNING / f"{k}.json").exists():
                del _STATE[k]
        try:
            tmp = HOME / f"dispatcher.state.{os.getpid()}"
            tmp.write_text(json.dumps({"job": job_id, "detail": detail, "since": now, "jobs": _STATE}))
            fs_retry(os.replace, tmp, STATE)
        except OSError:
            pass


def read_state():
    """dispatcher.state as {job id: {detail, since, wii}}, from any version's file."""
    try:
        st = json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}
    if "jobs" in st:
        return st["jobs"]
    return {st["job"]: {"detail": st.get("detail"), "since": st.get("since")}} if st.get("job") else {}


class Turn:
    """A turn at one Wii: waits in line for a lease, with the job it has (`job_id`, claimed), then
    keeps it renewed until released. `call` is the lease server (lease_call) or the dispatcher's
    LocalLeases; with neither (no server, outside a dispatcher) it is a no-op on the one Wii.
    `target` is the Wii it got: name, ip, port, log_port."""

    def __init__(self, name, job_id=None, want="any", call=None):
        self.info = {"ticket": uuid.uuid4().hex, "host": socket.gethostname(), "name": name, "qid": queue_id(),
                     "head": job_id, "want": want}
        self.call = call or (lease_call if lease_server() else None)
        self.server = call is None and self.call is not None   # every workstation's: send it the history
        self.target, self.slot, self.next_id, self.last_wait = bench_wii(), None, None, {}
        self.job_id, self.stop, self.released_ago, self.released_by = job_id, None, None, None
        self.wii = "hbc"                           # what the Wii was doing at the release
        self.empty = False                         # all its jobs were cancelled before it got the Wii
        self.gap_max, self.lost = 0.0, False       # the longest time without a renewal, during a job

    def rename(self, name, job_id=None, want=None):
        """The job this turn is for now (the one it was for was cancelled): told at once."""
        if job_id:
            self.job_id = self.info["head"] = job_id
        if want:
            self.info["want"] = want
        if self.info.get("name") == name:
            return
        self.info["name"] = name
        if self.stop:
            try:
                self.call("/renew", dict(self.info, **queue_payload()))
            except OSError:
                pass                               # the next renew carries it

    def set_phase(self, phase):
        """What this turn is doing now, for every workstation's status (and the time a turn
        spends waiting for HBC). Sent at once, then with every renew."""
        if self.info.get("phase") == phase:
            return
        self.info["phase"] = phase
        if self.stop:
            try:
                self.call("/renew", dict(self.info, **queue_payload()))   # the queue changed too
            except OSError:
                pass                               # the next renew carries it

    def next_is_mine(self):
        """After a job: True when the earliest-received job of all is this workstation's (keep
        the turn: stack it), False when another workstation's is, None when no job is queued
        anywhere, or when the server is older than 1.10.2 and cannot say (then: others_waiting)."""
        if not self.stop:                          # no server: this workstation's queue is all
            return True if jobs(PENDING) else None
        try:
            r = self.call("/renew", dict(self.info, **queue_payload()))
        except OSError:
            return None
        if "next_is_mine" not in r:
            return "old"
        self.next_id = r.get("next_job")
        if r["next_is_mine"]:
            return True
        return False if r.get("next") else None

    def others_waiting(self):
        """Workstations in line behind this one (0 without a server, or if it cannot say)."""
        if not self.stop:
            return 0
        try:
            return self.call("/renew", self.info).get("waiting", 0)
        except OSError:
            return 0

    def _refill(self):
        """The job it waited with is gone (cancelled): take the next free one, or False."""
        if self.job_id is None or (PENDING / f"{self.job_id}.json").exists():
            return True
        CLAIMS.drop(self, self.job_id)
        p = CLAIMS.next(self, self.slot)
        if p is None:
            return False
        try:
            j = load(p)
        except (OSError, ValueError):
            return self._refill()
        self.rename(j.get("name"), j["id"], j.get("want_wii") or "any")
        return True

    def __enter__(self):
        if self.call is None:
            return self
        said = None
        handled = []
        while True:
            t = time.monotonic()
            try:
                r = self.call("/acquire", dict(self.info, wait=LONG_POLL, handled=handled, **queue_payload()),
                              timeout=LONG_POLL + 10)
            except OSError as e:
                r = {"granted": False, "error": str(e)}
            handled = apply_requests(r.get("requests"))
            if not self._refill():                 # everything it could wait with was cancelled
                try:
                    self.call("/release", dict(self.info, handled=handled, **queue_payload()))
                except OSError:
                    pass
                self.empty = True
                return self
            if r["granted"]:
                break
            msg = (f"lease server {lease_server()} unreachable: {r['error']}" if "error" in r else
                   r["error_want"] if r.get("error_want") else
                   f"waiting for a Wii: place {r['position']} in line"
                   + (f", held by {r['holder']['host']} for {r['holder']['name']}" if r.get("holder") else ""))
            if msg != said:
                say(msg)
                report(self.job_id, msg)
                said = msg
            if time.monotonic() - t < 1:           # an error, or a server without the long poll
                time.sleep(5)
        self.released_ago = r.get("released_ago")
        self.released_by = r.get("released_by")
        # The Wii it got. A server before 1.10.8, or one with no pool set, names no address:
        # this workstation's own WII_BENCH_IP, as before.
        self.slot = r.get("slot") or "bench"
        self.target = bench_wii({"name": self.slot, "ip": r.get("ip"), "port": r.get("port"),
                                 "log_port": r.get("log_port") or LOG_PORT_BASE})
        self.info["slot"] = self.slot              # a restarted server gives the same Wii back
        say(f"lease taken for {self.info['name']}" + (f" on {self.slot} ({self.target['ip']})" if r.get("slot") else ""))
        self.stop = threading.Event()

        def renew():
            # Never let this thread die: a turn whose renewals stop loses its lease after TTL,
            # under a running job, and the Wii can be granted to another workstation mid-run.
            # On 2026-10-17 a lease expired 92 min into a turn (a 49-minute job); nothing said
            # why, because only network errors were caught, and silently.
            handled, failing, told = [], None, False
            while not self.stop.wait(min(TTL / 4, 5)):   # every 5 s: a stop is heard soon
                try:
                    r = self.call("/renew", dict(self.info, handled=handled, **queue_payload()))
                    handled = apply_requests(r.get("requests"))
                    if not r["ok"]:
                        self.lost = True
                        say("lease lost: another workstation may have the Wii")
                    if failing is not None:
                        gap = time.monotonic() - failing
                        self.gap_max = max(self.gap_max, gap)
                        if told:
                            say(f"renewing works again after {gap:.0f} s")
                    failing, told = None, False
                except Exception as e:             # anything: log it, keep trying
                    if failing is None:
                        failing = time.monotonic() - min(TTL / 4, 5)
                    gap = time.monotonic() - failing
                    self.gap_max = max(self.gap_max, gap)
                    if gap > 20 and not told:
                        say(f"renewing the lease has failed for {gap:.0f} s: {type(e).__name__}: {e}")
                        told = True
        threading.Thread(target=renew, daemon=True).start()
        return self

    def __exit__(self, *exc):
        CLAIMS.drop(self)
        if self.stop:
            self.stop.set()
            try:
                self.call("/release", dict(self.info, wii=self.wii, **queue_payload()))
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
    for w, h in (the_wiis(s) if not err else [(bench_wii(), None)]):
        print(f"Wii    {w['name']} {w['ip']}: {wii_now({'holder': h} if s else None, err, w)}")
    print("Jobs that use the network log need inbound TCP 4300-4315 open on this machine "
          "(4300 + the Wii's place in the pool).")


INTERPRETERS = ("python", "python3", "py", "pythonw", "bash", "sh", "zsh", "env", "cmd", "powershell", "pwsh", "node")


def default_name(cmd):
    """A job's name when add gets no --name: its script and first argument, not the interpreter.
    "python.exe" named every one of WiiStation's jobs on 2026-10-12, and its agent could not tell
    them for its own."""
    words, inline = [], False
    for w in cmd:
        base = pathlib.Path(w).name
        stem = base.lower().removesuffix(".exe")
        if not words:
            if w == "-c":                          # python -c CODE: the code is the name
                inline = True
                continue
            if inline:
                return ("python -c " + " ".join(w.split()))[:80]
            if stem in INTERPRETERS or stem.startswith("python") or ("=" in w and not w.startswith("-")) or w.startswith("-"):
                continue                           # the interpreter, env VAR=..., its options
            words.append(base)
        else:                                      # the script's first argument, unless options start
            if not w.startswith("-"):
                words.append(w)
            break
    name = " ".join(words) or pathlib.Path(cmd[0]).name
    return name[:80]


def command_missing(cmd, cwd):
    """Why the command cannot start, or None. Its program is looked up as the job would: a path
    relative to the job's folder, else on PATH (with PATHEXT on Windows). In 2026-10-12..19 three
    jobs failed in under 6 s on a program that was not there."""
    import shutil
    prog = cmd[0]
    if os.sep in prog or (os.altsep and os.altsep in prog):
        path = pathlib.Path(prog)
        if not path.is_absolute():
            path = pathlib.Path(cwd) / path
        if path.exists() or (WINDOWS and path.with_suffix(".exe").exists()):
            return None
        return f"{prog} does not exist (looked in {path.parent})"
    if shutil.which(prog):
        return None
    return f"{prog} is not on PATH here"


def job_agent(given=None):
    """Who queued the job, so the same agent's back-to-back jobs can run as one chain:
    --agent, else $WII_BENCH_AGENT, else the Claude Code session, else nobody (no chain)."""
    for v in (given, os.environ.get("WII_BENCH_AGENT"), os.environ.get("CLAUDE_CODE_SESSION_ID")):
        if v:
            return v
    return None


def queue_id():
    """This workstation's queue for the lease server: its host and its state folder."""
    import hashlib
    return f"{socket.gethostname()}:{hashlib.sha1(str(HOME.resolve()).encode()).hexdigest()[:8]}"


def queue_payload():
    """This workstation's queued jobs, oldest first, for the lease server: each job's age, not
    its time, so no two clocks are ever compared."""
    now, out = time.time(), []
    q = jobs(PENDING) if PENDING.exists() else []
    for p in q[:QUEUE_MAX]:
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        t0 = _parse_local(j.get("added")) or now
        out.append({"id": j["id"], "name": j.get("name"), "agent": (j.get("agent") or "")[:12],
                    "folder": pathlib.Path(j.get("cwd") or ".").name, "age_s": round(now - t0),
                    "want": j.get("want_wii") or "any"})
    return {"qid": queue_id(), "host": socket.gethostname(), "queue": out, "total": len(q),
            "queue_at": time.time()}


def cancel_here(ids, why="cancelled"):
    """Cancel pending jobs of this workstation: each leaves a record in done/ with exit
    "cancelled", so an agent waiting on it hears so instead of "no job". Returns those cancelled."""
    out = []
    for i in ids:
        p = PENDING / f"{i}.json"
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        j.update(exit="cancelled", secs=0, finished=time.strftime("%Y-%m-%d %H:%M:%S"), cancelled=why)
        DONE.mkdir(parents=True, exist_ok=True)
        try:
            fs_retry(p.unlink)
        except OSError:
            continue                               # started meanwhile, or another cancel had it
        (DONE / f"{i}.json").write_text(json.dumps(j, indent=1))
        (DONE / f"{i}.log").write_text(f"{why}\n")
        out.append(i)
    return out


def stop_here(ids):
    """Ask this workstation's dispatcher to stop running jobs: a stop file it looks for every
    second (the job then ends like a timeout, "stopped")."""
    out = []
    for i in ids:
        if (RUNNING / f"{i}.json").exists():
            (RUNNING / f"{i}.stop").write_text(time.strftime("%Y-%m-%d %H:%M:%S"))
            out.append(i)
        elif (RECOVERING / f"{i}.json").exists():  # waiting for a frozen Wii to come back: give up
            (RECOVERING / f"{i}.giveup").write_text(time.strftime("%Y-%m-%d %H:%M:%S"))
            out.append(i)
    return out


def apply_requests(reqs):
    """Cancels and stops relayed by the lease server for this workstation's jobs."""
    if not reqs:
        return []
    done = cancel_here(reqs.get("cancel") or [], "cancelled through the lease server")
    done += stop_here(reqs.get("stop") or [])
    gone = [i for i in reqs.get("cancel") or [] if not (PENDING / f"{i}.json").exists()]
    if done:
        say(f"by request: {', '.join(done)}")
    return sorted(set(done) | set(gone) | set(i for i in reqs.get("stop") or [] if not (RUNNING / f"{i}.json").exists()))


def send_queue():
    """Tell the lease server what is queued here now (add and cancel): best effort."""
    if lease_server():
        try:
            lease_call("/queue", queue_payload(), timeout=3)
        except OSError:
            pass                                   # the dispatcher sends it with its next call


def cmd_add(a):
    for d in (PENDING, RUNNING, DONE):
        d.mkdir(parents=True, exist_ok=True)
    if not a.cmd:
        sys.exit("add: give the command after --")
    job_id = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
    job = {"id": job_id, "name": a.name or default_name(a.cmd), "cwd": str(pathlib.Path(a.cwd).resolve()),
           "cmd": a.cmd, "timeout": a.timeout, "added": time.strftime("%Y-%m-%d %H:%M:%S"),
           "agent": job_agent(a.agent)}
    if a.collect:
        job["collect"] = a.collect
    if a.wii and a.wii != "any":
        names = pool_names(timeout=3)
        if names is not None and a.wii not in names:
            sys.exit(f"add: no Wii named {a.wii}; the Wiis are {', '.join(names)}")
        job["want_wii"] = a.wii
    if not a.no_check:
        missing = command_missing(a.cmd, job["cwd"])
        if missing:
            sys.exit(f"add: {missing}; the job would fail at once (exit 127, or 9009 on Windows). "
                     "Fix the command, or --no-check if it only exists by the time the job runs")
    tmp = PENDING / f"{job_id}.tmp"
    tmp.write_text(json.dumps(job, indent=1))
    fs_retry(tmp.rename, PENDING / f"{job_id}.json")
    print(job_id, flush=True)
    start_dispatcher()
    send_queue()


def pool_names(timeout=10):
    """The Wiis' names: the lease server's (one, "bench", for a server before 1.10.8), else this
    workstation's pool; PINNED learns which take pinned jobs only. None when the server does
    not answer."""
    if not lease_server():
        pool = default_pool()
    else:
        try:
            pool = lease_call("/status", timeout=timeout).get("wiis") or [{"name": "bench"}]
        except OSError:
            return None
    PINNED.clear()
    PINNED.update(w["name"] for w in pool if w.get("pinned"))
    return [w["name"] for w in pool]


def job_window():
    """Windows: the dispatcher has no console, so each console job (python.exe) gets a
    window of its own. Show it minimized and without taking the focus."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 7                             # SW_SHOWMINNOACTIVE
    return {"startupinfo": si}


def run_job(p, started=None, wii=None):
    w = bench_wii(wii)
    job = load(p)
    r = RUNNING / p.name
    fs_retry(p.rename, r)                          # claims it: a rename is atomic
    if started:
        started()                                  # out of the queue: tell the lease server
    report(job["id"], "running", w["name"])
    job["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    job["wii"], job["wii_ip"] = w["name"], w["ip"]
    r.write_text(json.dumps(job, indent=1))
    log = RUNNING / f"{job['id']}.log"
    out_dir = RUNS / job["id"]                     # this job's own: no other run writes here
    out_dir.mkdir(parents=True, exist_ok=True)
    job["out"] = str(out_dir)
    r.write_text(json.dumps(job, indent=1))
    t0 = time.monotonic()
    # Its Wii, for the job and the hbc.py it runs (HBC_WII, HBC_PORT): with more than one Wii,
    # whichever is free runs the job. WII_BENCH_LOG_PORT is a TCP port for the app's network log
    # that no job on another Wii of this workstation uses at the same time.
    env = dict(os.environ, WII_BENCH_IP=w["ip"], WII_BENCH_PORT=str(w["port"]), HBC_WII=w["ip"],
               HBC_PORT=str(w["port"]), WII_BENCH_WII=w["name"], WII_BENCH_LOG_PORT=str(w.get("log_port") or LOG_PORT_BASE),
               WII_BENCH_JOB=job["id"], WII_BENCH_OUT=str(out_dir), WII_BENCH_JOB_START=f"{time.time():.3f}")
    with open(log, "w") as out:
        try:
            proc = subprocess.Popen(job["cmd"], cwd=job["cwd"], stdout=out, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, env=env,
                                    **(job_window() if WINDOWS else {"start_new_session": True}))
            limit, stop = time.monotonic() + (job.get("timeout") or TIMEOUT), RUNNING / f"{job['id']}.stop"
            while True:                            # its timeout, or a stop (the manage page, `stop`)
                try:
                    job["exit"] = proc.wait(timeout=1)
                    break
                except subprocess.TimeoutExpired:
                    if stop.exists():
                        kill_tree(proc)
                        job["exit"] = "stopped"
                        out.write(f"\nstopped on request ({stop.read_text().strip()})\n")
                        break
                    if time.monotonic() > limit:
                        kill_tree(proc)
                        job["exit"] = "timeout"
                        break
        except OSError as e:
            out.write(f"could not start: {e}\n")
            job["exit"] = "not started"
    job["secs"] = int(time.monotonic() - t0)
    job["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    (DONE / f"{job['id']}.json").write_text(json.dumps(job, indent=1))
    fs_retry(log.rename, DONE / log.name)
    fs_retry(r.unlink)
    (RUNNING / f"{job['id']}.stop").unlink(missing_ok=True)
    say(f"done {job['id']} {job['name']}: exit {job['exit']} in {job['secs']} s")
    return job


def _tree_size(path):
    files = size = 0
    for root, _, names in os.walk(path):
        for n in names:
            try:
                size += os.path.getsize(os.path.join(root, n))
                files += 1
            except OSError:
                pass
    return files, size


def hbc_cmd(*args, timeout=60, wii=None):
    """hbc.py against a bench Wii, outside any job (the queue's own requests between jobs)."""
    w = bench_wii(wii)
    env = dict(os.environ, HBC_PORT=str(w["port"]))
    env.pop("WII_BENCH_JOB", None)
    return subprocess.run([sys.executable, str(HBC_PY), "--wii", w["ip"], *args], env=env, capture_output=True,
                          timeout=timeout)


def save_wii_state(job, folder, what, wii=None):
    """What the Wii can still tell about a job that did not end in HBC, into the job's run folder
    (runs/ID/freeze/FOLDER): status, a picture of the screen, the app's log so far or the one HBC
    kept, and HBC's crash report. Each that answers is a file; the rest are noted. Returns the
    files saved."""
    out = pathlib.Path(job["out"]) / "freeze" / folder
    out.mkdir(parents=True, exist_ok=True)
    saved = []
    for name, args in (("status.json", ("--json", "status")), ("lastlog.txt", ("lastlog",)),
                       ("crash.txt", ("crash",)), ("screen.png", ("screen", str(out / "screen.png")))):
        if name not in what:
            continue
        try:
            r = hbc_cmd(*args, wii=wii)
            if name != "screen.png":
                (out / name).write_bytes(r.stdout + (b"\n" + r.stderr if r.returncode and r.stderr else b""))
            if r.returncode == 0 or name != "screen.png":
                saved.append(f"freeze/{folder}/{name}")
        except (OSError, subprocess.SubprocessError) as e:
            (out / (name + ".error")).write_text(str(e))
    return saved


RECOVERING = HOME / "recovering"                   # ID.json: a job whose data waits for its Wii to come back


def recover_and_collect(job, turn):
    """A job left the Wii with nothing answering: frozen hard, or off. The data still on its SD
    card (whatever the app flushed) and what HBC kept (a reset keeps MEM2; a power cut does not)
    can only be fetched once HBC runs again, after someone resets or power-cycles the Wii, and
    before another job can overwrite them. So this turn keeps the Wii and waits, then saves
    HBC's kept log and crash report and collects the job's Wii paths, and only then lets the
    next job go. `stop ID` (or the monitor's manage page) gives up the wait."""
    wii = getattr(turn, "target", None)
    RECOVERING.mkdir(parents=True, exist_ok=True)
    rec, giveup = RECOVERING / f"{job['id']}.json", RECOVERING / f"{job['id']}.giveup"
    giveup.unlink(missing_ok=True)
    rec.write_text(json.dumps({"id": job["id"], "name": job.get("name"), "since": time.time(),
                               "wii": (wii or {}).get("name")}))
    turn.set_phase(f"{WAITING_FOR_HBC}: to save the data of {job.get('name')} after a freeze (reset or power-cycle the Wii)")
    report(job["id"], "waiting for the Wii to be reset, to save this job's data")
    say(f"{job['id']}: nothing answers; waiting for HBC to save its data (stop {job['id']} gives up)")
    t0 = time.monotonic()
    try:
        while True:
            if giveup.exists():
                job["freeze"]["recovered"] = "given up"
                say(f"{job['id']}: gave up waiting for the Wii")
                return False
            state, _ = wii_state(wii)
            if state == "hbc":
                break
            for _ in range(int(BUSY_STEP)):        # look for a give-up every second, probe every BUSY_STEP
                time.sleep(1)
                if giveup.exists():
                    break
        job["freeze"]["recovered_after_s"] = round(time.monotonic() - t0)
        job["freeze"]["after"] = save_wii_state(job, "after", ("status.json", "lastlog.txt", "crash.txt"), wii)
        late = [c for c in job.get("collected") or [] if c.get("error") and c["src"].split(":", 1)[0].lower()
                in ("sd", "usb", "usb1", "usb2")]
        if late:
            redo = collect_job(dict(job, collect=[c["src"] for c in late]), True, wii)
            for c in redo:
                c["after_recovery"] = True
            keep = [c for c in job.get("collected") or [] if c not in late]
            job["collected"] = keep + redo
        turn.wii = "hbc"
        say(f"{job['id']}: HBC is back after {job['freeze']['recovered_after_s']} s; its data is saved")
        return True
    finally:
        rec.unlink(missing_ok=True)
        giveup.unlink(missing_ok=True)


def collect_job(job, wii_ok, wii=None):
    """The job's --collect paths, into its run folder, after the job and before anything else
    gets the Wii, so the next run cannot overwrite them (a fixed output folder, a log at a fixed
    place on the SD card). A Wii path needs HBC back; a local one is copied either way.
    Returns one record per path: src, dest (in the run folder), files, bytes, error."""
    import shutil
    out_dir = pathlib.Path(job["out"])
    results = []
    for src in job.get("collect") or []:
        rec = {"src": src}
        try:
            if src.split(":", 1)[0].lower() in ("sd", "usb", "usb1", "usb2") and ":/" in src:
                name = src.rstrip("/").rsplit("/", 1)[-1] or "root"
                dest = out_dir / "wii" / name
                rec["dest"] = dest.relative_to(out_dir).as_posix()
                if not wii_ok:
                    raise OSError("HBC did not come back after the job, so nothing could be fetched")
                dest.parent.mkdir(parents=True, exist_ok=True)
                w = bench_wii(wii)
                env = dict(os.environ, HBC_PORT=str(w["port"]))
                env.pop("WII_BENCH_JOB", None)
                base = [sys.executable, str(HBC_PY), "--wii", w["ip"], "get"]
                r = subprocess.run(base + [src, str(dest)], env=env, capture_output=True, text=True,
                                   timeout=COLLECT_TIMEOUT)
                if r.returncode and "use get -r" in (r.stdout + r.stderr):   # a folder
                    r = subprocess.run(base + ["-r", src, str(dest)], env=env, capture_output=True, text=True,
                                       timeout=COLLECT_TIMEOUT)
                if r.returncode:
                    raise OSError((r.stderr or r.stdout).strip().splitlines()[-1][:200] if (r.stderr or r.stdout).strip()
                                  else f"hbc.py get: exit {r.returncode}")
            else:
                path = pathlib.Path(src)
                if not path.is_absolute():
                    path = pathlib.Path(job["cwd"]) / path
                dest = out_dir / "local" / (path.name or "root")
                rec["dest"] = dest.relative_to(out_dir).as_posix()
                dest.parent.mkdir(parents=True, exist_ok=True)
                if path.is_dir():
                    shutil.copytree(path, dest, dirs_exist_ok=True)
                elif path.exists():
                    shutil.copy2(path, dest)
                else:
                    raise OSError(f"{path} does not exist")
            rec["files"], rec["bytes"] = _tree_size(dest) if dest.is_dir() else (1, dest.stat().st_size)
        except (OSError, subprocess.SubprocessError, ValueError) as e:
            rec["error"] = str(e)[:200]
            say(f"collect {src} for {job['id']}: {rec['error']}")
        results.append(rec)
    return results


def _archive_month(job):
    t = _parse_local(job.get("finished") or job.get("added") or "")
    return time.strftime("%Y-%m", time.localtime(t)) if t else "unknown"


def archive_job(job_id):
    """A finished job's record, log and run folder, packed into ARCHIVE/YYYY-MM/<id>.tar.gz
    (listed in ARCHIVE/index.jsonl), then removed from done/ and runs/."""
    import shutil
    import tarfile
    rec_p, log_p, run_p = DONE / f"{job_id}.json", DONE / f"{job_id}.log", RUNS / job_id
    try:
        job = load(rec_p)
    except (OSError, ValueError):
        return False
    month = ARCHIVE / _archive_month(job)
    month.mkdir(parents=True, exist_ok=True)
    tmp, dest = month / f"{job_id}.tar.gz.tmp", month / f"{job_id}.tar.gz"
    with tarfile.open(tmp, "w:gz") as t:
        t.add(rec_p, arcname=f"{job_id}/job.json")
        if log_p.exists():
            t.add(log_p, arcname=f"{job_id}/job.log")
        if run_p.exists():
            t.add(run_p, arcname=f"{job_id}/run")
    os.replace(tmp, dest)
    entry = {k: job.get(k) for k in ("id", "name", "agent", "added", "finished", "exit", "secs")}
    entry["archive"] = dest.relative_to(ARCHIVE).as_posix()
    with open(ARCHIVE / "index.jsonl", "a", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(entry) + "\n")
    for path in (rec_p, log_p):
        try:
            fs_retry(path.unlink)
        except OSError:
            pass
    shutil.rmtree(run_p, ignore_errors=True)
    return True


def rollover(force=False):
    """Keep the live runs small and the archive bounded: a finished job older than KEEP_DAYS,
    or the oldest while the live runs pass RUNS_MAX_GB, goes into the archive; the oldest
    archived months go while the archive passes ARCHIVE_MAX_GB. Runs at most every 10 minutes
    unless forced; returns how many jobs were archived."""
    import shutil
    stamp = HOME / "rollover.stamp"
    try:
        if not force and time.time() - stamp.stat().st_mtime < 600:
            return 0
    except OSError:
        pass
    HOME.mkdir(parents=True, exist_ok=True)
    stamp.write_text(time.strftime("%Y-%m-%d %H:%M:%S"))
    now, moved = time.time(), 0
    finished = []
    for p in DONE.glob("*.json") if DONE.exists() else []:
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        t = _parse_local(j.get("finished") or "") or p.stat().st_mtime
        finished.append((t, j["id"]))
    finished.sort()
    sizes = {jid: _tree_size(RUNS / jid)[1] for _, jid in finished if (RUNS / jid).exists()}
    live = sum(sizes.values())
    for t, jid in finished:
        if now - t > KEEP_DAYS * 86400 or live > RUNS_MAX_GB * 1e9:
            try:
                if archive_job(jid):
                    moved += 1
                    live -= sizes.get(jid, 0)
            except (OSError, EOFError) as e:
                say(f"rollover: {jid}: {e}")
        else:
            break
    if ARCHIVE.exists():
        months = sorted(d for d in ARCHIVE.iterdir() if d.is_dir())
        total = sum(_tree_size(d)[1] for d in months)
        while months and total > ARCHIVE_MAX_GB * 1e9:
            d = months.pop(0)
            total -= _tree_size(d)[1]
            shutil.rmtree(d, ignore_errors=True)
            say(f"rollover: dropped the archive of {d.name} (over {ARCHIVE_MAX_GB:g} GB)")
    return moved


def find_job(job_id):
    """Where a job is: ("pending"|"running"|"done", record) or ("archive", index entry), or None."""
    for d, where in ((PENDING, "pending"), (RUNNING, "running"), (DONE, "done")):
        p = d / f"{job_id}.json"
        if p.exists():
            try:
                return where, load(p)
            except (OSError, ValueError):
                pass
    for e in archive_index():
        if e.get("id") == job_id:
            return "archive", e
    return None


def archive_index():
    try:
        lines = (ARCHIVE / "index.jsonl").read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    out = []
    for l in lines:
        try:
            out.append(json.loads(l))
        except ValueError:
            pass
    return out


def cmd_runs(a):
    """Jobs here, newest first: queued, running, done and archived, with their data."""
    agent = job_agent() if a.mine else a.agent
    rows = []
    for d, where in ((PENDING, "queued"), (RUNNING, "running"), (DONE, "done")):
        for p in jobs(d) if d.exists() else []:
            try:
                j = load(p)
            except (OSError, ValueError):
                continue
            try:
                order = p.stat().st_mtime_ns          # written in the order they ran or came in
            except OSError:
                order = 0
            rows.append((j.get("finished") or j.get("started") or j.get("added") or "", order, where, j))
    for i, e in enumerate(archive_index()):          # the index is in the order they were archived
        rows.append((e.get("finished") or "", i, "archived", e))
    seen, out = set(), []
    # newest first; within one second (ids end in a random tail) by when each record was written
    for when, _, where, j in sorted(rows, key=lambda r: (r[0], r[1]), reverse=True):
        if j.get("id") in seen:
            continue
        seen.add(j.get("id"))
        if agent and j.get("agent") != agent:
            continue
        if a.name and a.name.lower() not in (j.get("name") or "").lower():
            continue
        out.append((where, j))
        if len(out) >= a.last:
            break
    if a.json:
        print(json.dumps([dict(j, where=w) for w, j in out], indent=1))
        return
    for where, j in out:
        files, size = _tree_size(RUNS / j["id"]) if where in ("done", "running") and (RUNS / j["id"]).exists() else (0, 0)
        data = f"{files} file(s), {size / 1e6:.1f} MB" if files else ("archived" if where == "archived" else "-")
        print(f"{j['id']}  {where:8} exit {str(j.get('exit', '-')):>4}  {(j.get('finished') or j.get('added') or '')[5:16]}  "
              f"{data:24} {j.get('name')}")


def cmd_results(a):
    """A job's record, the end of its log, and what it collected; --to copies it all out,
    from the live run or from the archive."""
    import shutil
    import tarfile
    found = find_job(a.id)
    if not found:
        sys.exit(f"no job {a.id} here (runs lists them)")
    where, j = found
    if where in ("pending", "running"):
        print(f"job {a.id} {j.get('name')}: {where}; its results are not in yet (wait {a.id})")
        sys.exit(3)
    if where == "archive":
        tar = ARCHIVE / j["archive"]
        if not tar.exists():
            sys.exit(f"job {a.id} was archived in {tar}, and that month has since been dropped")
        with tarfile.open(tar) as t:
            rec = json.loads(t.extractfile(f"{a.id}/job.json").read())
            try:
                log = t.extractfile(f"{a.id}/job.log").read().decode("utf-8", "replace")
            except KeyError:
                log = ""
            names = [m.name for m in t.getmembers() if m.isfile() and m.name.startswith(f"{a.id}/run/")]
            if a.to:
                dest = pathlib.Path(a.to)
                dest.mkdir(parents=True, exist_ok=True)
                for m in t.getmembers():
                    if m.name.startswith(f"{a.id}/") and m.isfile():
                        rel = m.name[len(a.id) + 1:]
                        rel = rel[4:] if rel.startswith("run/") else rel
                        target = dest / rel
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with t.extractfile(m) as src, open(target, "wb") as f:
                            shutil.copyfileobj(src, f)
        files = [n[len(a.id) + 5:] for n in names]
    else:
        rec = j
        try:
            log = (DONE / f"{a.id}.log").read_text(encoding="utf-8", errors="replace")
        except OSError:
            log = ""
        run = RUNS / a.id
        files = sorted(str(f.relative_to(run)).replace("\\", "/") for f in run.rglob("*") if f.is_file()) if run.exists() else []
        if a.to:
            dest = pathlib.Path(a.to)
            dest.mkdir(parents=True, exist_ok=True)
            if run.exists():
                shutil.copytree(run, dest, dirs_exist_ok=True)
            shutil.copy2(DONE / f"{a.id}.json", dest / "job.json")
            if (DONE / f"{a.id}.log").exists():
                shutil.copy2(DONE / f"{a.id}.log", dest / "job.log")
    if a.json:
        print(json.dumps(dict(rec, where=where, files=files), indent=1))
        return
    print(f"job {a.id} {rec.get('name')}: exit {rec.get('exit')} in {rec.get('secs')} s, finished {rec.get('finished')} ({where})")
    for c in rec.get("collected") or []:
        print(f"  collected {c['src']} -> {c.get('dest')}: " + (f"ERROR {c['error']}" if c.get("error") else
                                                               f"{c.get('files')} file(s), {c.get('bytes')} bytes"))
    print(f"  files: {len(files)}" + (f" in {RUNS / a.id}" if where == "done" else ""))
    for f in files[:a.files]:
        print(f"    {f}")
    if len(files) > a.files:
        print(f"    ... and {len(files) - a.files} more")
    if a.to:
        print(f"  copied to {a.to}")
    tail = log.splitlines()[-a.tail:]
    if tail:
        print("  log:")
        for l in tail:
            print(f"    {l}")


def cmd_rollover(a):
    n = rollover(force=True)
    print(f"archived {n} job(s); live runs keep {KEEP_DAYS:g} days and at most {RUNS_MAX_GB:g} GB, "
          f"the archive at most {ARCHIVE_MAX_GB:g} GB ({ARCHIVE})")


def finish_job(job, turn, again=False):
    """After the HBC check: the job's record gets it, and the job goes into the history,
    the lease server's (every workstation's) and this workstation's own. `again`: the record
    after a frozen Wii came back (or was given up), as an event of its own."""
    p = DONE / f"{job['id']}.json"
    tmp = DONE / f"{job['id']}.json.tmp"
    tmp.write_text(json.dumps(job, indent=1))
    try:
        fs_retry(os.replace, tmp, p)
    except OSError as e:                           # the record without the check, not a crash
        say(f"could not update {p.name}: {e}")
        tmp.unlink(missing_ok=True)
    ev = {"type": "recovery" if again else "job", "host": socket.gethostname(), **{k: job.get(k) for k in (
        "id", "name", "agent", "added", "started", "finished", "secs", "exit", "timeout", "wii",
        "hbc_back_s", "hbc_version", "wii_left", "chained", "freeze", "lease_gap_s", "lease_lost")},
          "collected": [{k: c.get(k) for k in ("src", "files", "bytes", "error")} for c in job.get("collected") or []]}
    local_history(ev)
    if turn.server:                                # every workstation's history
        try:
            lease_call("/event", ev)
        except OSError as e:
            say(f"history: the lease server did not take the job record: {e}")


_HISTORY = threading.Lock()


def local_history(ev):
    """An event into this workstation's history: one turn at a time, as turns on other Wiis run."""
    try:
        with _HISTORY:
            History(HOME / "history").write(ev)
    except OSError as e:
        say(f"history: {e}")


def wait_idle(full, need, job_id=None, turn=None):
    """Return True once HBC's menu has answered for `need` seconds in a row (0: one answer),
    or False when nothing is queued any more: the turn has nothing to wait for. (On 2026-10-08
    a turn's job was cancelled while it waited 25 min for a Wii out of HBC: the turn went on
    waiting under the gone job's name, and with an empty queue it would have held the lease
    for ever.) While it waits, the turn and dispatcher.state follow the head of the queue.

    Only ever called while this dispatcher holds the Wii and no job of its own runs, so it
    never reaches into a queue run. `need` is short only right after a queue job, whose end
    we know about; the first probe that finds the Wii busy or off means someone outside the
    queue may be at it, and the wait goes back to the full `full` seconds. A busy Wii is
    probed only every BUSY_STEP s, and an agent app's name asked for once, so an app run
    outside the queue is disturbed as little as possible."""
    step = 1.0 if need < full else 5.0
    ok_since = busy_since = None
    said, busy_what = None, None
    head = [job_id]
    wii = turn.target if turn is not None else None
    busy_log = {}        # busy_s (how long the Wii was not in HBC), what answered: turn.last_wait
    if turn is not None:
        turn.last_wait = busy_log

    def follow():
        """False when the turn has no job any more; else it follows its job (the next free one
        for its Wii when that was cancelled)."""
        if turn is None:
            return True
        if not turn._refill():
            return False
        head[0] = turn.job_id
        return True

    while True:
        t = time.monotonic()
        if not follow():
            return False
        job_id = head[0]
        state, text = wii_state(wii)
        if state == "hbc":
            ok_since = t if ok_since is None else ok_since
            step = min(step, 5.0)                  # HBC is back: the quiet period at 5 s, not BUSY_STEP
            if busy_since is not None:             # back after a busy spell: how long, and what it was
                busy_log["busy_s"] = round(t - busy_log["first_busy"])
                busy_log["what"] = busy_what.split(": ", 1)[1] if busy_what else "busy"
            busy_since, busy_what = None, None
            if t - ok_since >= need:
                return True
            msg = "waiting for HBC to stay idle"
        else:
            ok_since, need, step = None, full, BUSY_STEP
            busy_since = t if busy_since is None else busy_since
            busy_log["first_busy"] = busy_log.get("first_busy", busy_since)
            mins = int((t - busy_since) // 60)
            if busy_what is None or not busy_what.startswith(state):
                busy_what = f"{state}: busy: {agent_app(wii)} is running" if state == "agent" else f"{state}: busy or off"
            msg = f"Wii {busy_what.split(': ', 1)[1]}" + (f" for {mins} min" if mins else "")
        if msg != said:
            report(job_id, msg, (wii or {}).get("name"))
            said = msg
        if turn is not None:                       # without the minutes: the server counts them
            turn.set_phase(WAITING_FOR_HBC + (f": Wii {busy_what.split(': ', 1)[1]}" if busy_what else ""))
        # Until the next probe, look at the queue (not the Wii) every second: a cancel ends the
        # wait at once instead of after up to BUSY_STEP s.
        while (left := t + step - time.monotonic()) > 0:
            time.sleep(min(1.0, left))
            if not follow():
                return False
            if head[0] != job_id:                  # a new head: say so under its id
                job_id, said = head[0], None
                report(job_id, msg, (wii or {}).get("name"))


def outside_use(turn, wait, ago):
    by = turn.released_by or {}
    ev = {"type": "outside", "host": socket.gethostname(), "busy_s": wait["busy_s"], "what": wait.get("what"),
          "after": by.get("name"), "after_host": by.get("host"), "since_release_s": ago, "wii": turn.slot}
    say(f"the Wii was used outside the queue: {wait.get('what')} for {wait['busy_s']} s"
        + (f", after {by.get('host')}'s {by.get('name')} left it in HBC" if by else ""))
    local_history(ev)
    if turn.server:
        try:
            lease_call("/event", ev)
        except OSError:
            pass


def hbc_back(limit=RETURN_WAIT, wii=None):
    """After a job, while this dispatcher still holds the Wii: (seconds until HBC's menu
    answered, "hbc", its version), or (None, what answers instead, None). Probes only once the
    job's process is gone, so never during a run.

    The seconds are measured to the answer, not to the probe that got it: until 1.9.4 a first
    answer always read 0.0, however long it took, and 51 of the first 52 checks said "0.0 s"
    (most jobs wait for HBC themselves before they end, so HBC is usually there at once)."""
    t0 = time.monotonic()
    while True:
        t = time.monotonic()
        state, text = wii_state(wii)
        if state == "hbc":
            return round(time.monotonic() - t0, 2), "hbc", text or "stock HBC"
        if time.monotonic() - t0 >= limit:
            return None, (f"busy: {agent_app(wii)} is running" if state == "agent" else "busy or off"), None
        time.sleep(max(0.0, 1 - (time.monotonic() - t)))


def next_in_turn(turn, agent):
    """The next job this turn runs on its Wii (claimed), or None: the turn ends. Receive order
    decides (see Turn.next_is_mine); a server before 1.10.2 cannot say, and the rule before it
    applies (the same agent's next job, while nobody else waits)."""
    mine = turn.next_is_mine()
    if mine == "old":
        if turn.others_waiting() or not agent:
            return None
        return next_job(turn, agent, CHAIN_GRACE, turn.others_waiting)
    if mine:
        if turn.next_id:                           # that very job, unless another turn here has it
            return CLAIMS.next(turn, turn.slot, only=turn.next_id)
        return CLAIMS.next(turn, turn.slot)
    if mine is False:                              # another workstation's job came in first
        return None
    # Nothing queued anywhere: hold the turn a moment for this workstation's next add, and let
    # go as soon as another workstation's job arrives first.
    end = time.monotonic() + CHAIN_GRACE
    while time.monotonic() < end:
        if CLAIMS.free(turn.slot):
            m = turn.next_is_mine()
            if m in (True, None):
                return CLAIMS.next(turn, turn.slot, only=turn.next_id if m else None)
            return None
        if turn.next_is_mine() is False:
            return None
        time.sleep(1)
    return None


def next_job(turn, agent, grace, waiting=lambda: 0):
    """The next free job for the turn's Wii if it is this agent's, waiting up to `grace` s for
    it to be added (an agent that waits for one job and then adds the next). Another agent's
    job first, or another workstation joining the line (`waiting`, asked about once a second),
    ends the chain at once."""
    end, asked = time.monotonic() + grace, time.monotonic()
    while True:
        q = CLAIMS.free(turn.slot)
        if q:
            p, j = q[0]
            return CLAIMS.next(turn, turn.slot, only=p.stem) if j.get("agent") == agent else None
        if time.monotonic() >= end:
            return None
        if time.monotonic() - asked >= 1:
            if waiting():
                return None
            asked = time.monotonic()
        time.sleep(0.25)


_ROLL = threading.Lock()


def rollover_quietly():
    """rollover() after a job, unless another turn is at it already."""
    if _ROLL.acquire(blocking=False):
        try:
            rollover()
        finally:
            _ROLL.release()


def run_turn(a, p, shared, worker):
    """One turn: wait for a Wii with the job at `p` (claimed for `worker`), run it and the jobs
    that follow it on that Wii in receive order, and let the Wii go."""
    try:
        head = load(p)
    except (OSError, ValueError):                  # cancelled meanwhile
        CLAIMS.drop(worker)
        return
    turn = Turn(head["name"], head["id"], head.get("want_wii") or "any", shared.call)
    CLAIMS.move(worker, turn)                      # the turn has it now, and drops it when it ends
    worker.turn = turn
    with turn:
        if turn.empty:                             # cancelled while it waited for a Wii
            return
        worker.slot = turn.slot
        _LOCAL.tag = turn.slot if shared.many else ""
        last_end = shared.last_end.get(turn.slot)  # monotonic time our last job on this Wii ended
        ago = turn.released_ago
        recent = ((last_end is not None and time.monotonic() - last_end < RECENT)
                  or (ago is not None and ago < RECENT))
        if not wait_idle(a.idle, min(QUICK, a.idle) if recent else a.idle, turn.job_id, turn):
            return                                 # all cancelled while it waited: give the turn up
        if turn.last_wait.get("busy_s") and (ago is not None or last_end is not None):
            # The last queue job left the Wii in HBC, yet it was not: someone used it
            # outside the queue (a person, or an agent's hbc.py run that skipped it).
            # 3 of 4 long waits for HBC in 2026-10-12..19 were this.
            outside_use(turn, turn.last_wait, ago)
        p = PENDING / f"{turn.job_id}.json"
        chained = False
        while p:
            try:
                pj = load(p)
                agent = pj.get("agent")
                def started(pj=pj):
                    turn.info["job"] = pj["id"]
                    turn.info["name"] = pj.get("name")      # a stacked turn: this job, not the first
                    turn.gap_max, turn.lost = 0.0, False
                    turn.set_phase(f"running: {pj.get('name')}")
                job = run_job(p, started, turn.target)
                turn.info.pop("job", None)
                if turn.gap_max > 30 or turn.lost:
                    job["lease_gap_s"] = round(turn.gap_max)
                    job["lease_lost"] = turn.lost
            except (FileNotFoundError, ValueError):   # cancelled just before it started
                break
            finally:
                CLAIMS.drop(turn, p.stem)
            last_end = shared.last_end[turn.slot] = time.monotonic()
            # The Wii is still ours: see that HBC came back, and say so if it did not.
            turn.set_phase("checking HBC after the job")
            back_s, turn.wii, hbc_version = hbc_back(wii=turn.target)
            job.update(hbc_back_s=back_s, hbc_version=hbc_version, chained=chained)
            answers = "hbc" if back_s is not None else ("agent" if turn.wii.startswith("busy: ") else "nothing")
            if back_s is None:
                job["wii_left"] = turn.wii
                say(f"the Wii did not come back to HBC within {RETURN_WAIT} s: {turn.wii}")
                job["freeze"] = {"answered_by": answers}
                if answers == "agent":             # the app is stuck but its agent answers: ask it now
                    turn.set_phase("saving what the stuck app can tell")
                    job["freeze"]["saved"] = save_wii_state(job, "agent", ("status.json", "lastlog.txt", "screen.png"),
                                                            turn.target)
            # Its data, before anything else gets the Wii: the next run cannot overwrite it.
            # A stuck app's agent serves files too; with nothing answering, see below.
            if job.get("collect"):
                turn.set_phase("collecting the job's data")
                job["collected"] = collect_job(job, answers != "nothing", turn.target)
            finish_job(job, turn)
            if answers == "nothing":               # a hard freeze: keep the Wii until it is back
                recover_and_collect(job, turn)
                finish_job(job, turn, again=True)
                rollover_quietly()
                break
            rollover_quietly()
            if back_s is None:
                break
            # Receive order: the turn goes on while the earliest-received job of all that may
            # use this Wii is this workstation's (jobs received one after the other stack up);
            # when another workstation's came in first, it goes. With nothing queued anywhere,
            # the lease is held CHAIN_GRACE s for this workstation's next add.
            p = next_in_turn(turn, agent)
            if p:
                try:
                    nj = load(p)
                    same = agent and nj.get("agent") == agent
                    turn.rename(nj.get("name"), nj["id"], nj.get("want_wii") or "any")
                except (OSError, ValueError):
                    same = False
                chained = True
                say(f"chained: {p.stem}" + (" from the same agent" if same else " (received next)"))
                if not wait_idle(a.idle, 0 if same else min(QUICK, a.idle), p.stem, turn):
                    break
                p = PENDING / f"{turn.job_id}.json"   # its job, or the next free one if that was cancelled
                if turn.last_wait.get("busy_s"):   # between two jobs of this turn: outside the queue
                    turn.released_by = {"host": socket.gethostname(), "name": job.get("name")}
                    outside_use(turn, turn.last_wait, 0)


def startable(j, workers, names):
    """True if a new turn for job j could get a Wii this dispatcher's turns are not already on or
    waiting for: one turn per Wii, so a job pinned to a free Wii starts while another Wii's jobs wait."""
    want = j.get("want_wii") or "any"
    mine = set(names) - PINNED if want == "any" else {want} & set(names)
    if not mine:
        return False
    busy = 0
    for w in workers:
        if w.slot:
            busy += w.slot in mine
        else:
            want_w = getattr(w.turn, "info", {}).get("want") if w.turn else w.want
            busy += bool((set(names) - PINNED if want_w in (None, "any") else {want_w}) & mine)
    return busy < len(mine)


def cmd_run(a):
    import types
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
        # One turn per Wii at a time: each waits for a Wii with a job of its own, and runs it and
        # the jobs that follow on that Wii. Without a lease server, the same lease logic runs here.
        shared = types.SimpleNamespace(call=None if lease_server() else LocalLeases(default_pool()),
                                       last_end={}, many=False)
        workers, names, names_at = [], ["bench"], None
        empty_since = time.monotonic()

        def work(p, worker):
            try:
                run_turn(a, p, shared, worker)
            except Exception:                      # one turn's bug must not stop the others
                import traceback
                say("a turn failed:\n" + traceback.format_exc())
            finally:
                CLAIMS.drop(worker)

        while True:
            workers = [w for w in workers if w.thread.is_alive()]
            if names_at is None or time.monotonic() - names_at > 60:
                names = pool_names() or names      # the Wiis: the server can add one while we run
                shared.many, names_at = len(names) > 1, time.monotonic()
            free = CLAIMS.free()
            if workers or free:
                empty_since = time.monotonic()
            elif time.monotonic() - empty_since > 600:
                return
            for p, j in free:
                if not startable(j, workers, names):
                    continue
                worker = types.SimpleNamespace(slot=None, turn=None, want=j.get("want_wii") or "any")
                if not CLAIMS.take(p.stem, worker):
                    continue
                worker.thread = threading.Thread(target=work, args=(p, worker), daemon=True)
                worker.thread.start()
                workers.append(worker)
            time.sleep(0.5)
    finally:
        if dispatcher_pid() == os.getpid():
            LOCK.unlink(missing_ok=True)


def wii_now(lease=None, lease_error=None, wii=None):
    """A Wii's line for status and setup. It is probed only when no run can be going on: no job
    running here on it, and its lease free (or no server). Otherwise it says who has the Wii,
    and leaves it alone: the queue never reaches into a run."""
    w = bench_wii(wii)
    for p in jobs(RUNNING) if RUNNING.exists() else []:
        try:
            j = load(p)
        except (OSError, ValueError):
            return "in use by a job here (not probed)"
        if j.get("wii", w["name"]) == w["name"] or not j.get("wii"):
            return f"in use by job {j['name']} here (not probed)"
    if lease_error:
        return "not probed: the lease server cannot say whether a run is going on"
    h = (lease or {}).get("holder")
    if h:
        return f"in use: {h['host']} has it for {h['name']} (not probed)"
    return wii_line(w)


def the_wiis(s=None):
    """[(wii, its holder or None)] from a server's status (one Wii, its holder, from a server
    before 1.10.8), else this workstation's pool."""
    if s and s.get("wiis"):
        return [(bench_wii(w), w.get("holder")) for w in s["wiis"]]
    if s is not None:
        return [(bench_wii(), s.get("holder"))]
    return [(bench_wii(w), None) for w in default_pool()]


def cmd_status(a):
    pid = dispatcher_pid()
    s = err = None
    if lease_server():
        try:
            s = lease_call("/status")
        except OSError as e:
            err = e
    wiis = the_wiis(s) if not err else [(w, None) for w in [bench_wii()]]
    for w, h in wiis:
        print(f"Wii {w['name']} {w['ip']}: {wii_now({'holder': h} if s else None, err, w)}"
              + (f" [pinned jobs only: add --wii {w['name']}]" if w.get("pinned") else ""))
    print(f"dispatcher: {f'pid {pid}' if pid else 'not running'}")
    if lease_server():
        if err:
            print(f"lease {lease_server()}: unreachable ({err})")
        else:
            for w in s.get("wiis") or [dict(name="bench", holder=s["holder"], left=s.get("left"))]:
                h = w.get("holder")
                print(f"lease {lease_server()} {w['name']}: "
                      + (f"held by {h['host']} for {h['name']} (since {h['granted']})" if h else "free"))
                if w.get("left"):
                    l = w["left"]
                    print(f"  left     {l['host']}'s {l['name']} left the Wii {l['wii']} (since {l['since']})")
            for x in s["waiters"]:
                want = x.get("want")
                print(f"  waiting  {x['host']}  {x['name']}  (since {x['since']})" + (f"  for {want}" if want and want != "any" else ""))
            for q in s.get("queues") or []:
                if q.get("qid") != queue_id():     # this workstation's own are listed below
                    print(f"  queued on {q['host']}: {q.get('total', len(q['jobs']))} job(s)")
                    for j in q["jobs"]:
                        print(f"    {j.get('id')}  {j.get('name')}")
    for d, label in ((RUNNING, "running"), (PENDING, "pending")):
        for p in jobs(d):
            j = load(p)
            on = f"  on {j['wii']}" if j.get("wii") else f"  for {j['want_wii']}" if j.get("want_wii") else ""
            print(f"  {label:8} {j['id']}  {j['name']}  (added {j['added']}){on}")
    for p in jobs(DONE)[-a.last:]:
        j = load(p)
        print(f"  done     {j['id']}  {j['name']}  exit {j['exit']}, {j['secs']} s, {j['finished']}"
              + (f", on {j['wii']}" if j.get("wii") else ""))


def progress(job_id, d, started):
    """One line on why the job is not done yet, for `wait`."""
    mins = int((time.monotonic() - started) // 60)
    if d == RUNNING:
        return f"job {job_id}: running ({mins} min in wait)"
    ahead = [p.stem for p in jobs(PENDING)]
    place = ahead.index(job_id) if job_id in ahead else 0
    why = "queued"
    st = read_state()
    if job_id in st:
        why = st[job_id].get("detail") or why
    elif place == 0 and any(v.get("detail") == "running" for v in st.values()):
        why = "the job before it is still running"
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
            print(f"job {a.id} {j['name']}: " + (f"{j.get('cancelled', 'cancelled')}" if j["exit"] == "cancelled" else
                                                  f"exit {j['exit']} in {j.get('secs', 0)} s"))
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


def handovers(evs):
    """Seconds from each release to a grant of the same Wii to someone already waiting then."""
    rel, gaps = {}, []
    for e in evs:
        w = e.get("slot")                          # none before 1.10.8: the one Wii
        if e.get("type") == "release":
            rel[w] = e
        elif e.get("type") == "grant" and rel.get(w) and e.get("waited_s", 0) > e["t"] - rel[w]["t"]:
            gaps.append(e["t"] - rel.pop(w)["t"])
    return gaps


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
    wiis = sorted({e.get("wii") for e in jobs_ if e.get("wii")})
    if len(wiis) > 1:                              # several Wiis: how much each one did
        print(f"\n{'Wii':24} {'jobs':>5} {'failed':>6} {'Wii min':>8} {'busy':>5}")
        for w in wiis:
            wj = [e for e in jobs_ if e.get("wii") == w]
            secs = sum(e.get("secs") or 0 for e in wj)
            print(f"{w[:24]:24} {len(wj):5} {sum(1 for e in wj if e.get('exit') != 0):6} {secs / 60:8.0f} "
                  f"{100 * secs / span:4.0f}%")
    gaps = handovers(evs)
    if gaps:
        print(f"\nhand-overs to a waiting workstation: {len(gaps)}, median {1000 * st.median(gaps):.0f} ms, "
              f"longest {1000 * max(gaps):.0f} ms")
    long = sorted(grants, key=lambda e: e.get("waited_s", 0), reverse=True)[:5]
    if long and long[0].get("waited_s", 0) >= 60:
        print("longest waits for the Wii:")
        for e in long:
            if e.get("waited_s", 0) >= 60:
                print(f"  {e['waited_s'] / 60:5.1f} min  {loc(e['t'])}  {e.get('host')}: {e.get('name')}")
    turns = {h: sum(1 for e in grants if e.get("host") == h) for h in hosts}
    trouble = ([f"{loc(e['t'])} {e.get('host')}'s {e.get('name')}: {job_problem(e)[1]}"
                for e in jobs_ if job_problem(e) and e.get("exit") != "timeout"]
               + [f"{loc(e['t'])} {e.get('host')} found the Wii used outside the queue for {e.get('busy_s')} s "
                  f"({e.get('what')}), after {e.get('after_host')}'s {e.get('after')}" for e in evs if e["type"] == "outside"]
               + [f"{loc(e['t'])} {e.get('host')}'s {e.get('name')}: no lease renewal for {e['lease_gap_s']} s"
                  + (" and the lease was lost" if e.get("lease_lost") else "") for e in jobs_ if e.get("lease_gap_s")]
               + [f"{loc(e['t'])} {e.get('host')} held the lease {e['hbc_wait_s'] / 60:.0f} min waiting for HBC "
                  f"before {e.get('name')}" for e in evs if e["type"] == "release" and e.get("hbc_wait_s", 0) > 600]
               + [f"{h}: {turns[h]} turn(s) and no job records: its wiibench.py predates 1.9.1 (git pull)"
                  for h in hosts if turns.get(h) and not any(e.get("host") == h for e in jobs_) and not src.startswith("this")]
               + [f"{loc(e['t'])} {e.get('host')}'s {e.get('name')} left the Wii {e['wii']}"
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


def queue_order(status, queues, my_qid=None, my_jobs=None):
    """Every workstation's queued jobs in the order their turns will come. A server from
    1.10.2 on numbers each job as it is received: then that is the order. An older one does
    not, and the order is its turns' (see queue_order_turns)."""
    if any(j.get("seq") is not None for q in queues for j in q.get("jobs", [])):
        seqs = {j["id"]: j["seq"] for q in queues for j in q.get("jobs", []) if j.get("id") and j.get("seq") is not None}
        out = []
        for q in queues:
            if my_qid is not None and q.get("qid") == my_qid:
                continue                           # this workstation's own: from disk, below
            age = q.get("age_s", 0)
            out += [dict(j, host=q.get("host"), age_s=(j.get("age_s") or 0) + age) for j in q.get("jobs", [])]
        for j in my_jobs or []:                    # not yet on the server: after everything it knows
            out.append(dict(j, host=socket.gethostname(), seq=seqs.get(j["id"], 10 ** 9)))
        listed = {q.get("qid") for q in queues if q.get("jobs")}
        for w in status.get("waiters") or []:      # a waiter before 1.10.2: its one job
            if w.get("qid") not in listed and w.get("seq_join") is not None:
                out.append({"host": w.get("host"), "name": w.get("name"), "age_s": w.get("waited_s"), "seq": w["seq_join"]})
        return sorted(out, key=lambda j: (j.get("seq", 10 ** 9), -(j.get("age_s") or 0)))
    return queue_order_turns(status, queues, my_qid, my_jobs)


def queue_order_turns(status, queues, my_qid=None, my_jobs=None):
    """Every workstation's queued jobs in the order their turns will come: the workstations in
    line take turns one job each, in line order; a workstation with more queued goes to the back
    after each job; the holder rejoins at the back after its job; a workstation with jobs and no
    place in line yet (between two turns) comes after those in line. A waiter from a dispatcher
    before 1.10.0 sends no queue: it counts as the one job it is waiting with. This
    workstation's own list is read from disk, not the server's copy, so it is never stale."""
    lists = {}
    for q in queues:
        age = q.get("age_s", 0)
        lists[q["qid"]] = [dict(j, host=q.get("host"), age_s=(j.get("age_s") or 0) + age) for j in q.get("jobs", [])]
    if my_qid is not None and my_jobs is not None:
        lists[my_qid] = [dict(j, host=socket.gethostname()) for j in my_jobs]
    line, seen = [], set()
    for w in status.get("waiters") or []:
        q = w.get("qid")
        if q in lists and q not in seen:
            line.append(q)
            seen.add(q)
        elif q not in lists:
            line.append({"host": w.get("host"), "name": w.get("name"), "age_s": w.get("waited_s")})
    h = (status.get("holder") or {}).get("qid")
    for q in [h] + sorted(lists):
        if q in lists and q not in seen and lists[q]:
            line.append(q)
            seen.add(q)
    out = []
    while line:
        q = line.pop(0)
        if isinstance(q, dict):
            out.append(q)
            continue
        if lists[q]:
            out.append(lists[q].pop(0))
            if lists[q]:
                line.append(q)
    return out


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
    """(severity, kind, detail) for a finished job that went wrong, else None. A cancelled job
    did not go wrong: someone did not want it."""
    ex = j.get("exit")
    if ex == "cancelled":
        return None
    if j.get("hbc_back_s") is None and j.get("wii_left"):
        f, more = j.get("freeze") or {}, ""
        if f.get("answered_by") == "agent":
            more = f"; its agent answered, {len(f.get('saved') or [])} file(s) saved in freeze/agent"
        elif f.get("recovered_after_s") is not None:
            more = f"; nothing answered, HBC was back {_age(f['recovered_after_s'])} later and its data was saved then"
        elif f.get("recovered") == "given up":
            more = "; nothing answered, and the wait for a reset was given up"
        elif f.get("answered_by") == "nothing":
            more = "; nothing answers: reset or power-cycle the Wii, its data is saved when HBC is back"
        return "err", "Wii left", f"HBC not back after the job: {j['wii_left']}{more}"
    if ex == "timeout":
        return "err", "timeout", f"stopped after {j.get('secs')} s (--timeout {j.get('timeout')})"
    if ex == "not started":
        return "err", "not started", "the command could not start"
    if ex not in (0, None):
        if isinstance(ex, int) and ex >= 2 ** 31:  # Windows' unsigned exit code: -1 is 4294967295
            ex -= 2 ** 32
        return "err", f"exit {ex}", None
    return None


LANE_PREFIX = 13      # columns before a lane's cells: "  dev   .213 "
LANE_STATS = 26       # columns after them: jobs, failed, busy, longest wait


def hbc_name(j):
    """'HBC 1.10.9', 'stock HBC', or 'HBC' from a job record."""
    v = j.get("hbc_version")
    return v if v and v.startswith("stock") else f"HBC {v}" if v else "HBC"


def _short_ip(ip):
    return "." + ip.rsplit(".", 1)[1] if ip and ip.count(".") == 3 else (ip or "-")


def wii_board(board, order, evs, rows, last_on, now, hours, width, server_down, row, kv):
    """Each Wii, described for the monitor: a "wiis" row (its card on the queue page, and its
    part of the one-line status at the top of every page, in three lengths) and a "lanes" row
    (the history page: the window in cells, what each Wii did). From what the snapshot already
    has; nothing here asks the Wii."""
    first = board[0]["name"] if board else "bench"
    here = {}
    for r in rows.get("running", []):
        here[r.get("wii") if r.get("wii") not in (None, "-") else first] = r
    span = hours * 3600
    jobs_ = [e for e in evs if e.get("type") == "job"]
    wh = f"{hours:g}h"
    for b in board:
        name, h, left, pinned = b["name"], b["holder"] or {}, b["left"], b["pinned"]
        local = here.get(name)
        phase = str(h.get("phase") or "")
        if left:
            state = "left"
        elif server_down:
            state = "unknown"
        elif h and phase.startswith(WAITING_FOR_HBC):
            state = "waiting"
        elif h or local:
            state = "running"
        else:
            state = "free"
        job = (local or {}).get("name") or h.get("name") or (left or {}).get("name") or ""
        job_id = (local or {}).get("id") or h.get("job") or ""
        who = h.get("host") or (left or {}).get("host") or (socket.gethostname() if local else "")
        if local and local.get("agent"):
            who += f" · {local['agent']}"
        elapsed = local["elapsed_s"] if local else (h.get("phase_s") if phase.startswith("running") else None)
        timeout = (local or {}).get("timeout_s")
        for_s = (left.get("ago_s") if left else h.get("phase_s") if state == "waiting" else
                 elapsed if state == "running" and elapsed is not None else h.get("held_s") if h else
                 b.get("released_ago"))
        what = phase.split(": ", 1)[1] if state == "waiting" and ": " in phase else ""
        detail = (left.get("wii") if left else what if state == "waiting" else
                  phase if state == "running" and phase and not phase.startswith("running") else "")
        lease = (f"lease held {_age(h['held_s'])}" + (f" · renewed {h['age']}s ago" if "age" in h else "")
                 if h and "held_s" in h else "")
        nxt = [j for j in order if (j.get("want") or "any") == name or ((j.get("want") or "any") == "any" and not pinned)]
        # The last job on this Wii: every workstation's (the history), else this one's own.
        mine = [e for e in jobs_ if (e.get("wii") or first) == name]
        last = max(mine, key=lambda e: e.get("t", 0)) if mine else None
        if last is None and (last_on.get(name) or (len(board) == 1 and last_on.get(""))):
            tf, j = last_on.get(name) or last_on.get("")
            last = dict(j, t=tf)
        if last:
            back = last.get("hbc_back_s")
            last_hbc = (f"left it {last.get('wii_left')}" if back is None and last.get("wii_left") else
                        f"{hbc_name(last)} at once" if back is not None and back <= HBC_AT_ONCE else
                        f"{hbc_name(last)} after {back:.0f} s" if back is not None else "")
            last_bad = bool(job_problem(last))
        busy = sum(e.get("secs") or 0 for e in mine)
        n = len(nxt)
        # The one-line status, in three lengths: the monitor takes the longest that fits.
        el = _age(elapsed) if elapsed is not None else ""
        fr = _age(for_s) if for_s is not None else ""
        if state == "left":
            wide = f"LEFT OUT OF HBC {fr}: {left.get('wii')} (after {left.get('host')}'s {left.get('name')})"
            med, narrow = f"LEFT {fr}", fr
        elif state == "waiting":
            wide, med, narrow = f"waiting {fr} for HBC" + (f": {what}" if what else ""), f"HBC? {fr}", fr
        elif state == "running" and el:
            tm = f"/{_age(timeout)}" if timeout else ""
            wide = f"{job} {el}{tm}" + (f" +{n} queued" if n else "")
            med = f"{job[:18]} {el}" + (f" +{n}" if n else "")
            narrow = el + (f" +{n}" if n else "")
        elif state == "running":
            wide, med, narrow = f"{job}: {phase or 'starting'}", f"{job[:18]}", ""
        elif state == "unknown":
            wide, med, narrow = "server unreachable", "?", ""
        else:
            wide = "free" + (" (pinned)" if pinned else "") + (f" +{n} queued" if n else "")
            med, narrow = "free" + (f" +{n}" if n else ""), ""
        row("wiis", name=name, ip=b["ip"], state=state, line=wide,
            rank={"left": 0, "waiting": 1, "unknown": 2, "running": 3}.get(state, 4),
            takes="pinned jobs only" if pinned else "takes any job",
            log_port=b.get("log_port") or "", for_=fr, job=job, job_id=job_id, who=who,
            elapsed_s=elapsed if elapsed is not None else "", timeout_s=timeout or "", detail=detail, lease=lease,
            next1=nxt[0].get("name") if n > 0 else "", next2=nxt[1].get("name") if n > 1 else "",
            more=max(0, n - 2) or "",
            last_mark=("bad" if last_bad else "ok") if last else "",
            last_name=last.get("name") if last else "", last_exit=last.get("exit") if last else "",
            last_hbc=last_hbc if last else "", last_ago=_age(now - last["t"]) if last else "",
            win=wh, win_jobs=len(mine), win_failed=sum(1 for e in mine if job_problem(e)),
            win_busy=f"{100 * busy / span:.0f}%" if span else "0%",
            med=med, narrow=narrow, pinned="yes" if pinned else "no")
    # The lanes: the window in cells, oldest on the left.
    cells = max(20, width - LANE_PREFIX - LANE_STATS - 2)
    start, step = now - span, span / cells
    kv["lane_cells"], kv["lane_step"] = cells, _age(step)
    ticks = [" "] * cells
    every = max(8, cells // 6)
    for i in range(0, cells - 6, every):
        lab = time.strftime("%H:%M", time.localtime(start + i * step))
        ticks[i:i + len(lab)] = lab
    ticks[cells - 3:] = "now"
    kv["lane_ticks"] = "".join(ticks)
    for b in board:
        name = b["name"]
        lane, labels = ["."] * cells, [" "] * cells

        def paint(t0, t1, code, label=None):
            i0 = max(0, int((t0 - start) / step))
            i1 = min(cells - 1, int((t1 - start) / step))
            if i1 < 0 or i0 >= cells:
                return
            for i in range(i0, max(i0, i1) + 1):
                lane[i] = code
            room = i1 - i0 + 1                     # a name only where the block has room for one
            if label and room >= 3 and all(c == " " for c in labels[i0:i1 + 1]):
                text = label[:room]
                for k, ch in enumerate(text):
                    if i0 + k < cells:
                        labels[i0 + k] = ch
        mine = lambda e: (e.get("wii") or e.get("slot") or first) == name
        for e in evs:                              # the waits for HBC at the start of a turn
            if e.get("type") == "release" and mine(e) and e.get("hbc_wait_s"):
                g = e["t"] - e.get("held_s", 0)
                paint(g, g + e["hbc_wait_s"], "y")
        for e in evs:                              # each job, and the hold after a hard freeze
            if e.get("type") == "job" and mine(e):
                t1 = e["t"] - (e.get("hbc_back_s") or 0)
                paint(t1 - (e.get("secs") or 0), t1, "r" if job_problem(e) else "g",
                      (e.get("name") or "").split(" ")[0] if (e.get("secs") or 0) < 600 else e.get("name"))
            elif e.get("type") == "recovery" and mine(e):
                held = ((e.get("freeze") or {}).get("recovered_after_s")) or 0
                paint(e["t"] - held, e["t"], "r", f"{e.get('name')} froze: held {_age(held)} for a reset")
        h = b["holder"] or {}
        local = here.get(name)
        if str(h.get("phase", "")).startswith(WAITING_FOR_HBC):
            paint(now - h.get("phase_s", 0), now, "y")
        elif local or h:
            paint(now - (local["elapsed_s"] if local else h.get("phase_s", h.get("held_s", 0))), now, "b")
        for e in evs:                              # someone used the Wii outside the queue
            if e.get("type") == "outside" and mine(e):
                i = int((e["t"] - start) / step)
                if 0 <= i < cells:
                    lane[i] = "!"
        js = [e for e in evs if e.get("type") == "job" and mine(e)]
        waits = [e.get("waited_s", 0) for e in evs if e.get("type") == "grant" and mine(e) and e.get("waited_s", 0) >= 1]
        busy = sum(e.get("secs") or 0 for e in js)
        row("lanes", name=name, ip=_short_ip(b["ip"]), cells="".join(lane), labels="".join(labels),
            jobs=len(js), failed=sum(1 for e in js if job_problem(e)),
            busy=f"{100 * busy / span:.0f}%" if span else "0%", wait=_age(max(waits)) if waits else "-")


def snapshot(hours=24, sorts=(), filters=(), done_rows=12, log_lines=200, cache=None, width=None):
    now = time.time()
    since = now - hours * 3600
    kv, rows = {}, {}

    def row(sec, **f):
        rows.setdefault(sec, []).append(f)

    kv.update(now=int(now), clock=time.strftime("%H:%M:%S"), host=socket.gethostname(), home=str(HOME),
              wii_ip=WII, window_h=f"{hours:g}")
    pid = dispatcher_pid()
    kv["dispatcher_pid"] = pid or "-"
    states = read_state()
    # The line shows one turn: one waiting for its Wii first, as that is the one to explain.
    pick = sorted(states.items(), key=lambda kvp: (kvp[1].get("detail") == "running", -(kvp[1].get("since") or 0)))
    st = dict(pick[0][1], job=pick[0][0]) if pick else {}
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
            left_s=int(max(0, to - (now - t0))), wii=j.get("wii") or "-", timeout_s=to)
    local_q = []
    for p in pending:
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        t0 = _parse_local(j.get("added")) or now
        local_q.append({"id": j["id"], "name": j.get("name"), "agent": (j.get("agent") or "")[:12],
                        "folder": pathlib.Path(j.get("cwd") or ".").name, "age_s": int(now - t0),
                        "want": j.get("want_wii") or "any"})
    kv["running_n"], kv["pending_n"] = len(rows.get("running", [])), len(local_q)
    # dispatcher.state names the last job it worked for; once that job is done it is history.
    live = {r["id"] for r in rows.get("running", [])} | {j["id"] for j in local_q}
    kv["dispatcher_turns"] = sum(1 for k in states if k in live)
    if kv["dispatcher_job"] not in live and (not pid or kv["dispatcher_detail"] in (None, "running")):
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
        for c in (j.get("collected") or []) if tf >= since else []:
            if c.get("error"):                     # the job's data, or some of it, was not kept
                problems.append(dict(t=tf, severity="warn", kind="collect", host=socket.gethostname(),
                                     job=j.get("name"), id=j["id"], detail=f"{c['src']}: {c['error']}"))
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
            chained="yes" if j.get("chained") else "no", wii=j.get("wii") or "-")
    ran = [r for r in recent if r[1].get("exit") != "cancelled"]     # a cancelled job never had the Wii
    if ran and ran[0][1].get("hbc_version") == "stock HBC":
        # HBC answered, but not hbc-reborn: an app exited to the original HBC (2026-10-08). Jobs
        # that use hbc-reborn's requests (hbc.py) fail there until it is back.
        tf, j = ran[0]
        problems.append(dict(t=tf, severity="warn", kind="stock HBC", host=socket.gethostname(), job=j.get("name"),
                             id=j["id"], detail="the Wii came back to a stock HBC, not hbc-reborn: "
                                                "jobs that use hbc.py will fail until hbc-reborn runs again"))
    last_on = {}                                   # each Wii's last job here: the "last:" note
    for tf, j in ran:
        last_on.setdefault(j.get("wii") or "", (tf, j))

    def last_note(tf, j):
        back = j.get("hbc_back_s")
        ago = _age(now - tf)
        return (f"{j.get('name')} left it {j.get('wii_left')} ({ago} ago)" if back is None and j.get("wii_left") else
                f"HBC answered as {j.get('name')} ended ({ago} ago)" if back is not None and back <= HBC_AT_ONCE else
                f"HBC came back {back:.0f} s after {j.get('name')} ({ago} ago)" if back is not None else
                f"{j.get('name')} ended {ago} ago, before the queue checked HBC")
    if ran:
        kv["wii_last"] = last_note(*ran[0])

    # The lease server: who has the Wii, who waits, and every workstation's history.
    url = lease_server()
    kv["server"] = url or "none"
    evs, s, queues = [], {}, None
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
            many = len(s.get("wiis") or []) > 1
            for wi in s.get("wiis") or [{"name": "bench", "holder": s.get("holder")}]:
                h = wi.get("holder")
                # Durations from the server (1.9.1); an older one gives only its own clock's times.
                if h and h.get("phase") and h["phase"].startswith(WAITING_FOR_HBC) and h.get("phase_s", 0) > 300:
                    problems.append(dict(t=now - h["phase_s"], severity="warn", kind="Wii not in HBC",
                                         host=h.get("host"), job=h.get("name"), id="-",
                                         detail=(f"{wi['name']}: " if many else "") +
                                                f"has the lease and has been {h['phase']} for {_age(h['phase_s'])}: "
                                                "every job for this Wii waits until someone brings it back to HBC "
                                                "(an app, a crash screen, the Wii Menu after a power cycle, or another HBC: an app "
                                                "that exits to the original HBC leaves hbc-reborn)"))
            h = s.get("holder")
            if h and h.get("phase"):
                kv.update(holder_phase=h["phase"], holder_phase_for=_age(h["phase_s"]) if "phase_s" in h else None)
            if h:
                kv.update(holder_host=h.get("host"), holder_name=h.get("name"),
                          holder_for=_age(h["held_s"]) if "held_s" in h else f"since {str(h.get('granted'))[-8:]} server time")
            for i, w in enumerate(s.get("waiters") or [], 1):
                ws = w.get("waited_s")
                row("waiting", place=i, place_s=i, host=w.get("host"), name=w.get("name"),
                    since=_when(now - ws, now) if ws is not None else "-",
                    waited=_age(ws) if ws is not None else "?", waited_s=ws or 0)
            queues = s.get("queues")
            for wi in s.get("wiis") or [{"name": "bench", "left": s.get("left")}]:
                left = wi.get("left")
                if not left:
                    continue
                ago = f"{_age(left['ago_s'])} ago" if "ago_s" in left else f"since {str(left.get('since'))[-8:]} server time"
                if left is s.get("left") or left == s.get("left"):
                    kv.update(left_host=left.get("host"), left_name=left.get("name"), left_wii=left.get("wii"), left_ago=ago)
                problems.append(dict(t=now - left.get("ago_s", 0), severity="err", kind="Wii left", host=left.get("host"),
                                     job=left.get("name"), id="-",
                                     detail=(f"{wi['name']}: " if many else "") + f"still: {left.get('wii')} ({ago})"))
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

    # Each Wii: the server's pool, else this workstation's. Described at the end (wii_board),
    # which needs the queue's order and the history.
    if s.get("wiis"):
        board = [dict(name=w["name"], ip=w.get("ip") or WII, holder=w.get("holder"), left=w.get("left"),
                      pinned=bool(w.get("pinned")), log_port=w.get("log_port"), released_ago=w.get("released_ago"))
                 for w in s["wiis"]]
    elif s:
        board = [dict(name="bench", ip=WII, holder=s.get("holder"), left=s.get("left"), pinned=False,
                      log_port=LOG_PORT_BASE, released_ago=None)]
    else:
        board = [dict(name=w["name"], ip=w["ip"], holder=None, left=None, pinned=bool(w.get("pinned")),
                      log_port=w.get("log_port"), released_ago=None) for w in default_pool()]
    pool = [(w["name"], w["ip"], w["holder"], w["left"]) for w in board]
    kv["wiis_n"] = len(board)

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
        elif typ == "outside":
            problems.append(dict(t=t, severity="warn", kind="outside the queue", host=host, job=e.get("after") or "-", id="-",
                                 detail=f"the Wii was not in HBC for {_age(e.get('busy_s', 0))} ({e.get('what')}) "
                                        f"although the last queue job" + (f", {e.get('after_host')}'s," if e.get("after_host") else "")
                                        + " left it there: queue everything that uses the Wii"))
        if typ == "job" and e.get("lease_gap_s"):
            problems.append(dict(t=t, severity="err" if e.get("lease_lost") else "warn", kind="lease gap", host=host,
                                 job=e.get("name"), id=e.get("id") or "-",
                                 detail=f"no renewal for {e['lease_gap_s']} s during the job" +
                                        (", and the lease was lost: another workstation may have had the Wii" if e.get("lease_lost") else "")))
        if typ == "release" and e.get("hbc_wait_s", 0) > 600:
            problems.append(dict(t=t, severity="warn", kind="Wii not in HBC", host=host, job=e.get("name"), id="-",
                                 detail=f"held the lease {_age(e['hbc_wait_s'])} waiting for HBC before its job"))
        if typ == "release" and e.get("wii", "hbc") != "hbc":
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
    # Every workstation's queue, in the order the turns will come (a server before 1.10.0 sends
    # none: then only this workstation's).
    if queues is None:
        order = [dict(j, host=socket.gethostname()) for j in local_q]
    else:
        order = queue_order(s, queues, queue_id(), local_q)
    for i, j in enumerate(order, 1):
        row("pending", turn=i, turn_s=i, host=j.get("host"), id=j.get("id") or "-", name=j.get("name"),
            agent=j.get("agent") or "-", age=_age(j["age_s"]) if j.get("age_s") is not None else "?",
            age_s=j.get("age_s") or 0, wii=j.get("want") or "any")
    kv["queued_n"] = len(order)
    # The manage page: what is running (here, or the lease holder's) and everything queued.
    for r in rows.get("running", []):
        row("manage", state="running", turn="-", host=socket.gethostname(), id=r["id"], name=r["name"],
            agent=r.get("agent") or "-", folder=r.get("folder") or "-", age=r["elapsed"], age_s=r["elapsed_s"])
    for _, _, h, _ in pool:                    # every Wii's holder elsewhere
        if h and h.get("host") != socket.gethostname():
            row("manage", state="running" if str(h.get("phase", "")).startswith("running") else "waiting",
                turn="-", host=h.get("host"), id=h.get("job") or "-", name=h.get("name"), agent="-", folder="-",
                age=_age(h.get("held_s", 0)), age_s=h.get("held_s", 0))
    for i, j in enumerate(order, 1):
        row("manage", state="queued", turn=i, host=j.get("host"), id=j.get("id") or "-", name=j.get("name"),
            agent=j.get("agent") or "-", folder=j.get("folder") or "-",
            age=_age(j["age_s"]) if j.get("age_s") is not None else "?", age_s=j.get("age_s") or 0)
    for f in sorted(RECOVERING.glob("*.json")) if RECOVERING.is_dir() else []:
        try:
            rec = json.loads(f.read_text())
            kv.update(recovering_id=rec["id"], recovering_name=rec.get("name"), recovering_for=_age(time.time() - rec["since"]),
                      recovering_wii=rec.get("wii") or "")
            break                                  # the manage page gives up one at a time
        except (OSError, ValueError, KeyError):
            pass
    kv["queued_more"] = sum(max(0, q.get("total", 0) - len(q.get("jobs", []))) for q in (queues or []))

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
    gaps = handovers(evs)
    if gaps:
        kv["handover"] = (f"{len(gaps)} hand-over(s) to a waiting workstation, median "
                          f"{1000 * st_.median(gaps):.0f} ms, longest {1000 * max(gaps):.0f} ms")
    for e in sorted(grants, key=lambda e: e.get("waited_s", 0), reverse=True)[:8]:
        if e.get("waited_s", 0) >= 60:
            row("waits", when=_when(e["t"], now), when_s=int(e["t"]), waited=_age(e["waited_s"]),
                waited_s=e["waited_s"], host=e.get("host"), name=e.get("name"))

    wii_board(board, order, evs, rows, last_on, now, hours, width or 120,
              bool(url) and kv.get("server_ok") != "yes", row, kv)

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
    "wiis": ("name", "ip", "state", "line", "rank", "takes", "log_port", "for_", "job", "job_id", "who",
             "elapsed_s", "timeout_s", "detail", "lease", "next1", "next2", "more", "last_mark", "last_name",
             "last_exit", "last_hbc", "last_ago", "win", "win_jobs", "win_failed", "win_busy", "med", "narrow",
             "pinned"),
    "lanes": ("name", "ip", "cells", "labels", "jobs", "failed", "busy", "wait"),
    "running": ("id", "name", "agent", "started", "elapsed", "timeout", "left", "wii"),
    "pending": ("turn", "host", "id", "name", "agent", "age", "wii"),
    "manage": ("state", "turn", "host", "id", "name", "agent", "folder", "age"),
    "waiting": ("place", "host", "name", "since", "waited"),
    "done": ("finished", "id", "name", "exit", "secs", "hbc", "chained", "wii"),
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
            parts += [""]
            kv, rows = snapshot(hours, [x for x in parts[1].split("\x1f") if x],
                                [x for x in parts[2].split("\x1f") if x], cache=cache,
                                width=int(parts[3]) if parts[3].isdigit() else None)
            out = snapshot_lines(kv, rows)
        except Exception as e:                     # say so on screen; the next request tries again
            out = [f"@kv\terror\t{_cell(f'{type(e).__name__}: {e}')}"]
        sys.stdout.buffer.write(("\n".join(out + ["@end"]) + "\n").encode("utf-8"))
        sys.stdout.flush()


def cmd_snapshot(a):
    if a.serve:
        return serve_snapshots(a.hours)
    kv, rows = snapshot(a.hours, a.sort or (), a.filter or (), width=a.width)
    if a.json:
        print(json.dumps({"kv": kv, "rows": rows}, indent=1))
        return
    out = snapshot_lines(kv, rows)
    sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))   # a pipe on Windows is cp1252 otherwise
    sys.stdout.flush()


def cmd_cancel(a):
    """Cancel queued jobs: by id (here, or on another workstation through the lease server), or
    every queued job of an agent (--agent), a workstation (--host) or everyone (--all)."""
    if not (a.ids or a.agent or a.host or a.all):
        sys.exit("cancel: give job ids, or --agent A, --host H or --all")
    here = []
    for p in jobs(PENDING) if PENDING.exists() else []:
        try:
            j = load(p)
        except (OSError, ValueError):
            continue
        if (j["id"] in a.ids or a.all or (a.host and a.host == socket.gethostname())
                or (a.agent and j.get("agent") and j["agent"].startswith(a.agent))):
            here.append(j["id"])
    done = cancel_here(here, "cancelled with `wiibench.py cancel`")
    for i in done:
        print(f"cancelled {i}")
    rest = [i for i in a.ids if i not in done]
    for i in rest:
        d, _ = find(i)
        if d is not None:                          # here, but running or done already
            print(f"{i}: already {d.name}" + (" (stop stops a running job)" if d == RUNNING else ""))
    rest = [i for i in rest if find(i)[0] is None]
    if lease_server() and (rest or a.agent or a.host or a.all):
        try:
            r = lease_call("/request", {"action": "cancel", "ids": rest, "agent": a.agent, "host": a.host,
                                        "all": a.all, "by": job_agent(), "by_host": socket.gethostname()})
            for i in r.get("ids", []):
                if i not in done:
                    print(f"cancel sent for {i} (its workstation's dispatcher cancels it)")
        except OSError as e:
            print(f"the lease server did not take the cancel: {e}")
    elif rest:
        for i in rest:
            print(f"{i}: not found")
    send_queue()
    if not done and not lease_server() and not a.agent and not a.all:
        sys.exit(1)


def cmd_stop(a):
    """Stop running jobs (here, or on another workstation through the lease server): the job is
    ended like a timeout ("stopped"), then the Wii's HBC is checked and its data collected."""
    for i in a.ids:
        if stop_here([i]):
            print(f"stopping {i}")
        elif lease_server():
            try:
                lease_call("/request", {"action": "stop", "ids": [i], "by": job_agent(), "by_host": socket.gethostname()})
                print(f"stop sent for {i} (its workstation's dispatcher stops it within a few seconds)")
            except OSError as e:
                print(f"{i}: the lease server did not take the stop: {e}")
        else:
            print(f"{i}: not running here")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="op", required=True)
    s = sub.add_parser("add"); s.add_argument("--name"); s.add_argument("--cwd", default=".")
    s.add_argument("--timeout", type=int, default=TIMEOUT); s.add_argument("--agent")
    s.add_argument("--no-check", action="store_true", help="queue it even if its program is not found now")
    s.add_argument("--wii", metavar="NAME", help="run it on this Wii only (default: whichever is free)")
    s.add_argument("--collect", action="append", metavar="PATH",
                   help="after the job, before the next one: copy PATH into the job's run folder "
                        "(sd:/... or usb:/... from the Wii, else a path here, relative to --cwd)")
    s.add_argument("cmd", nargs=argparse.REMAINDER)
    s = sub.add_parser("run"); s.add_argument("--idle", type=float, default=20)
    s = sub.add_parser("status"); s.add_argument("--last", type=int, default=5)
    s = sub.add_parser("wait"); s.add_argument("id"); s.add_argument("--tail", type=int, default=25)
    s.add_argument("--every", type=float, default=60, help="seconds between progress lines (stderr)")
    s = sub.add_parser("cancel", help="cancel queued jobs, here or anywhere (through the lease server)")
    s.add_argument("ids", nargs="*"); s.add_argument("--agent"); s.add_argument("--host")
    s.add_argument("--all", action="store_true", help="every queued job, on every workstation")
    s = sub.add_parser("stop", help="stop running jobs, here or anywhere")
    s.add_argument("ids", nargs="+")
    s = sub.add_parser("serve"); s.add_argument("--port", type=int, default=4310); s.add_argument("--ttl", type=float, default=TTL)
    s.add_argument("--host", default="0.0.0.0")
    s.add_argument("--history", help="keep the history (events, rotated and archived) in this directory")
    s.add_argument("--keep-months", type=int, default=0, help="months of archives to keep (0: forever)")
    s.add_argument("--wii", action="append", metavar="NAME=IP[:PORT]",
                   help="a Wii in the pool (again for each; default: $WII_BENCH_WIIS)")
    s = sub.add_parser("runs", help="jobs here, newest first, with their data (live and archived)")
    s.add_argument("--agent"); s.add_argument("--mine", action="store_true", help="this agent's (as add would tag it)")
    s.add_argument("--name", help="only names containing this"); s.add_argument("--last", type=int, default=30)
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("results", help="a job's record, log and collected data; --to DIR copies them out")
    s.add_argument("id"); s.add_argument("--to"); s.add_argument("--tail", type=int, default=15)
    s.add_argument("--files", type=int, default=40); s.add_argument("--json", action="store_true")
    s = sub.add_parser("rollover", help="archive old runs now (the dispatcher does it every 10 minutes)")
    s = sub.add_parser("snapshot", help="what monitor.sh shows, as tab-separated lines (never touches the Wii)")
    s.add_argument("--hours", type=float, default=24); s.add_argument("--json", action="store_true")
    s.add_argument("--width", type=int, help="the terminal's columns, for the history lanes (default 120)")
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
    {"add": cmd_add, "run": cmd_run, "status": cmd_status, "wait": cmd_wait, "cancel": cmd_cancel, "stop": cmd_stop,
     "serve": cmd_serve, "setup": cmd_setup, "report": cmd_report,
     "snapshot": cmd_snapshot, "runs": cmd_runs, "results": cmd_results, "rollover": cmd_rollover}[a.op](a)


if __name__ == "__main__":
    main()
