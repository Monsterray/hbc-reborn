#!/usr/bin/env python3
"""wiibench.py -- one queue for the Wii on the bench, shared by every project and agent.

The Wii (default 192.168.8.213) sits in the Homebrew Channel. Only one thing can use it at
a time, so work goes through this queue instead of straight to the Wii:

  python tools/wii-bench/wiibench.py add [--name N] [--cwd DIR] [--timeout S] -- CMD ARGS...
      queue a command (run in DIR, default the current directory) and start the dispatcher
      if none is running. Prints the job id. The command talks to the Wii itself (wiiload,
      e.g. WiiStation's scripts/wii_lab.py); it gets WII_BENCH_IP in its environment.
  python tools/wii-bench/wiibench.py wait ID      block until the job is done; print the
                                                  end of its log; exit with its exit code
  python tools/wii-bench/wiibench.py status       the queue, the job running, the Wii now
  python tools/wii-bench/wiibench.py cancel ID    remove a job that has not started
  python tools/wii-bench/wiibench.py run          the dispatcher (add starts it for you)

The dispatcher runs one job at a time, oldest first, and only when HBC has answered on TCP
4299 for --idle seconds in a row (default 20): HBC answers only in its menu, so a Wii that is
running anything -- a job, or another agent's test that did not use this queue -- is left
alone, and a quiet period keeps it from cutting in between another agent's back-to-back
runs. A job that runs past its timeout (default 3600 s) is stopped (its own process tree).
The dispatcher exits when the queue has been empty for 10 minutes; the next add starts it.

State lives in one directory shared by every copy of this script, so every project uses
the same queue: $WII_BENCH_HOME, else C:/tools/wii-bench on Windows and ~/.wii-bench
elsewhere. It holds queue/pending, queue/running, queue/done (JSON + .log each),
dispatcher.lock (the dispatcher's PID) and dispatcher.log. $WII_BENCH_NO_DISPATCH=1 makes
add queue a job without starting a dispatcher (for tests).
"""
import argparse
import json
import os
import pathlib
import signal
import socket
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


def hbc_idle():
    """True if HBC's loader takes a connection. The probe sends a 16-byte header HBC rejects
    at once ("invalid upload request"). On the stock HBC a bare connect and close holds its
    single loader thread for 10 s (its tcp_read retried on EOF until
    TCP_BLOCK_RECV_TIMEOUT; hbc-reborn 1.4.1 fixed this), and its listen backlog is 3:
    probing every 5 s that way filled the backlog and HBC stopped answering (2026-09-28)."""
    try:
        with socket.create_connection((WII, 4299), timeout=2) as s:
            s.sendall(b"PING" + bytes(12))
        return True
    except OSError:
        return False


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


def cmd_add(a):
    for d in (PENDING, RUNNING, DONE):
        d.mkdir(parents=True, exist_ok=True)
    if not a.cmd:
        sys.exit("add: give the command after --")
    job_id = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
    job = {"id": job_id, "name": a.name or pathlib.Path(a.cmd[0]).name, "cwd": str(pathlib.Path(a.cwd).resolve()),
           "cmd": a.cmd, "timeout": a.timeout, "added": time.strftime("%Y-%m-%d %H:%M:%S")}
    tmp = PENDING / f"{job_id}.tmp"
    tmp.write_text(json.dumps(job, indent=1))
    tmp.rename(PENDING / f"{job_id}.json")
    print(job_id)
    start_dispatcher()


def run_job(p):
    job = load(p)
    r = RUNNING / p.name
    p.rename(r)                                    # claims it: a rename is atomic
    job["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    r.write_text(json.dumps(job, indent=1))
    log = RUNNING / f"{job['id']}.log"
    t0 = time.monotonic()
    env = dict(os.environ, WII_BENCH_IP=WII, WII_BENCH_JOB=job["id"])
    with open(log, "w") as out:
        try:
            proc = subprocess.Popen(job["cmd"], cwd=job["cwd"], stdout=out, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, env=env,
                                    **({} if WINDOWS else {"start_new_session": True}))
            try:
                job["exit"] = proc.wait(timeout=job.get("timeout") or 3600)
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
        while True:
            q = jobs(PENDING)
            if not q:
                if time.monotonic() - empty_since > 600:
                    return
                time.sleep(5)
                continue
            quiet = 0.0
            while quiet < a.idle:                  # HBC in its menu, and staying there
                t = time.monotonic()
                quiet = quiet + 5 if hbc_idle() else 0.0
                time.sleep(max(0.0, 5 - (time.monotonic() - t)))
            q = jobs(PENDING)                      # a cancel may have emptied it meanwhile
            if q:
                run_job(q[0])
            empty_since = time.monotonic()
    finally:
        if dispatcher_pid() == os.getpid():
            LOCK.unlink(missing_ok=True)


def cmd_status(a):
    pid = dispatcher_pid()
    print(f"Wii {WII}: {'in HBC (free)' if hbc_idle() else 'busy or off'}; dispatcher: {f'pid {pid}' if pid else 'not running'}")
    for d, label in ((RUNNING, "running"), (PENDING, "pending")):
        for p in jobs(d):
            j = load(p)
            print(f"  {label:8} {j['id']}  {j['name']}  (added {j['added']})")
    for p in jobs(DONE)[-a.last:]:
        j = load(p)
        print(f"  done     {j['id']}  {j['name']}  exit {j['exit']}, {j['secs']} s, {j['finished']}")


def cmd_wait(a):
    while True:
        d, p = find(a.id)
        if d is None:
            sys.exit(f"no job {a.id}")
        if d == DONE:
            j = load(p)
            lines = (DONE / f"{a.id}.log").read_text(errors="replace").splitlines()
            print("\n".join(lines[-a.tail:]))
            print(f"job {a.id} {j['name']}: exit {j['exit']} in {j['secs']} s")
            sys.exit(j["exit"] if isinstance(j["exit"], int) else 1)
        if d == PENDING and not dispatcher_pid():
            start_dispatcher()
        time.sleep(5)


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
    s.add_argument("--timeout", type=int, default=3600); s.add_argument("cmd", nargs=argparse.REMAINDER)
    s = sub.add_parser("run"); s.add_argument("--idle", type=float, default=20)
    s = sub.add_parser("status"); s.add_argument("--last", type=int, default=5)
    s = sub.add_parser("wait"); s.add_argument("id"); s.add_argument("--tail", type=int, default=25)
    s = sub.add_parser("cancel"); s.add_argument("id")
    a = ap.parse_args()
    if a.op == "add" and a.cmd[:1] == ["--"]:
        a.cmd = a.cmd[1:]
    {"add": cmd_add, "run": cmd_run, "status": cmd_status, "wait": cmd_wait, "cancel": cmd_cancel}[a.op](a)


if __name__ == "__main__":
    main()
