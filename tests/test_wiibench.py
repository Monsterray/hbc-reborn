"""Check tools/wii-bench/wiibench.py's queue handling without a Wii."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
BENCH = root / "tools/wii-bench/wiibench.py"


class WiiStateTest(unittest.TestCase):
    """The queue's idle check against a fake Wii: HBC's menu is free, an agent app is not."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(root / "tests"))
        from test_hbc_tool import FakeHBC
        cls.fake = FakeHBC()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.env = dict(os.environ, WII_BENCH_HOME=self.tmp.name, WII_BENCH_NO_DISPATCH="1",
                        WII_BENCH_IP="127.0.0.1", WII_BENCH_PORT=str(self.fake.port),
                        WII_BENCH_SERVER="")
        self.fake.agent = False

    def state(self):
        code = ("import sys, runpy; m = runpy.run_path(sys.argv[1]); "
                "print(m['wii_state']()[0], m['hbc_idle']())")
        out = subprocess.run([sys.executable, "-c", code, str(BENCH)], env=self.env,
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout.split()

    def status(self):
        return subprocess.run([sys.executable, str(BENCH), "status"], env=self.env,
                              capture_output=True, text=True, timeout=30).stdout

    def test_hbc_menu_is_free(self):
        self.assertEqual(self.state(), ["hbc", "True"])
        self.assertIn("in HBC 1.2.0 (free)", self.status())

    def test_an_agent_app_is_busy(self):
        # An app linking sdk/hbc_agent answers on 4299 too; it must not look like HBC.
        self.fake.agent = True
        self.assertEqual(self.state(), ["agent", "False"])
        self.assertIn("busy: otherapp is running", self.status())
        self.assertEqual(self.fake.exits, 0)


class WiiBenchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = pathlib.Path(self.tmp.name)
        # No dispatcher, and a Wii address nothing answers on.
        self.env = dict(os.environ, WII_BENCH_HOME=str(self.home),
                        WII_BENCH_NO_DISPATCH="1", WII_BENCH_IP="127.0.0.1", WII_BENCH_SERVER="")

    def tearDown(self):
        self.tmp.cleanup()

    def bench(self, *args):
        return subprocess.run([sys.executable, str(BENCH), *args], env=self.env,
                              capture_output=True, text=True, timeout=30)

    def test_add_status_cancel(self):
        added = self.bench("add", "--name", "unit job", "--cwd", str(root), "--",
                           sys.executable, "-c", "print('hi')")
        self.assertEqual(added.returncode, 0, added.stderr)
        job_id = added.stdout.strip()
        job = json.loads((self.home / "queue/pending" / f"{job_id}.json").read_text())
        self.assertEqual(job["name"], "unit job")
        self.assertEqual(job["cmd"][1:], ["-c", "print('hi')"])
        self.assertFalse((self.home / "dispatcher.lock").exists())

        status = self.bench("status")
        self.assertIn("busy or off", status.stdout)
        self.assertIn(job_id, status.stdout)

        self.assertEqual(self.bench("cancel", job_id).returncode, 0)
        self.assertFalse((self.home / "queue/pending" / f"{job_id}.json").exists())
        self.assertNotEqual(self.bench("cancel", job_id).returncode, 0)

    def test_jobs_run_oldest_first(self):
        ids = [self.bench("add", "--cwd", str(root), "--", sys.executable, "-c", "0").stdout.strip()
               for _ in range(3)]
        sys.path.insert(0, str(BENCH.parent))
        try:
            os.environ["WII_BENCH_HOME"] = str(self.home)
            import importlib
            import wiibench
            importlib.reload(wiibench)
            order = [p.stem for p in wiibench.jobs(wiibench.PENDING)]
        finally:
            sys.path.remove(str(BENCH.parent))
            os.environ.pop("WII_BENCH_HOME", None)
        self.assertEqual(order, ids)

    def test_setup_writes_a_shim_and_the_server(self):
        env = dict(self.env)
        del env["WII_BENCH_SERVER"]                # read from HOME/server instead
        run = lambda *args: subprocess.run([sys.executable, *args], env=env, capture_output=True,
                                           text=True, timeout=60)
        r = run(str(BENCH), "setup", "--server", "http://127.0.0.1:9/")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("unreachable", r.stdout)
        self.assertEqual((self.home / "server").read_bytes(), b"http://127.0.0.1:9/\n")
        r = run(str(self.home / "wiibench.py"), "status")   # the path other projects call
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("lease http://127.0.0.1:9: unreachable", r.stdout)
        self.assertEqual(run(str(BENCH), "setup").returncode, 0)   # again: leaves the shim
        (self.home / "wiibench.py").write_text("print('mine')\n")
        self.assertNotEqual(run(str(BENCH), "setup").returncode, 0)

    def test_server_file_in_any_encoding(self):
        wb = import_wiibench()
        p = self.home / "server"
        for data in ("http://h:4310\r\n".encode("utf-16"),        # Windows PowerShell 5.1 echo >
                     b"\xef\xbb\xbfhttp://h:4310\r\n", b"http://h:4310\n"):
            p.write_bytes(data)
            self.assertEqual(wb.read_setting(p), "http://h:4310")


def import_wiibench():
    sys.path.insert(0, str(BENCH.parent))
    try:
        import wiibench
        return wiibench
    finally:
        sys.path.remove(str(BENCH.parent))


class LeaseTest(unittest.TestCase):
    def setUp(self):
        self.wb = import_wiibench()
        self.now = 0.0
        self.leases = self.wb.Leases(ttl=60, clock=lambda: self.now)
        self.now = 60.0                            # past the start-up grace

    def acq(self, t):
        return self.leases.acquire({"ticket": t, "host": t, "name": "job"})

    def test_first_come_first_served(self):
        self.assertTrue(self.acq("a")["granted"])
        self.assertEqual(self.acq("b"), {"granted": False, "position": 1, "holder": self.leases.status()["holder"]})
        self.assertEqual(self.acq("c")["position"], 2)
        self.assertFalse(self.acq("c")["granted"])  # polling keeps its place
        self.leases.release({"ticket": "a"})
        self.assertFalse(self.acq("c")["granted"])  # b is first in line
        self.assertTrue(self.acq("b")["granted"])
        self.assertTrue(self.acq("b")["granted"])   # asking again while holding it

    def test_dead_holder_and_waiter_expire(self):
        self.acq("a")
        self.acq("b")
        self.now += 30
        self.acq("c")
        self.now += 31                             # a and b silent for 61 s, c for 31
        self.assertTrue(self.acq("c")["granted"])
        self.assertEqual(self.leases.status()["waiters"], [])

    def test_renew_keeps_it_and_reclaims_after_a_restart(self):
        self.acq("a")
        for _ in range(5):
            self.now += 50
            self.assertTrue(self.leases.renew({"ticket": "a"})["ok"])
        self.assertFalse(self.acq("b")["granted"])
        self.assertFalse(self.leases.renew({"ticket": "b"})["ok"])   # a waiter cannot renew its way in

        restarted = self.wb.Leases(ttl=60, clock=lambda: self.now)
        self.assertFalse(restarted.acquire({"ticket": "b", "host": "b", "name": "j"})["granted"])  # grace
        self.assertTrue(restarted.renew({"ticket": "a", "host": "a", "name": "j"})["ok"])
        self.now += 61
        restarted.renew({"ticket": "a"})
        self.assertFalse(restarted.acquire({"ticket": "b", "host": "b", "name": "j"})["granted"])

    def test_http_round_trip(self):
        import threading
        srv = self.wb.make_server(0, ttl=0.5, host="127.0.0.1")
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        os.environ["WII_BENCH_SERVER"] = f"http://127.0.0.1:{srv.server_address[1]}"
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")
        import time
        time.sleep(0.6)                            # the start-up grace
        with self.wb.Turn("first") as t:
            s = self.wb.lease_call("/status")
            self.assertEqual(s["holder"]["name"], "first")
            self.assertFalse(self.wb.lease_call("/acquire", {"ticket": "x", "host": "h", "name": "second"})["granted"])
        self.assertIsNone(self.wb.lease_call("/status")["holder"])
        self.assertTrue(self.wb.lease_call("/acquire", {"ticket": "x", "host": "h", "name": "second"})["granted"])

    def test_release_and_waiting_are_reported(self):
        self.acq("a")
        self.acq("b")
        self.assertEqual(self.leases.renew({"ticket": "a"}), {"ok": True, "waiting": 1})
        self.leases.release({"ticket": "a"})
        self.now += 3
        self.assertEqual(self.acq("b"), {"granted": True, "released_ago": 3.0})
        self.now += 100                            # b dies without a release: no clean hand-over
        self.assertEqual(self.acq("c"), {"granted": True, "released_ago": 103.0})

    def test_long_poll_answers_on_release(self):
        import threading
        import time
        srv = self.wb.make_server(0, ttl=5, host="127.0.0.1")
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        os.environ["WII_BENCH_SERVER"] = f"http://127.0.0.1:{srv.server_address[1]}"
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")
        a, b = ({"ticket": t, "host": t, "name": "j", "wait": 10} for t in "ab")
        t0 = time.monotonic()
        self.assertTrue(self.wb.lease_call("/acquire", a, timeout=20)["granted"])   # after the 5 s grace
        self.assertGreater(time.monotonic() - t0, 4)
        threading.Timer(0.5, self.wb.lease_call, ("/release", {"ticket": "a"})).start()
        t0 = time.monotonic()
        r = self.wb.lease_call("/acquire", b, timeout=20)
        took = time.monotonic() - t0
        self.assertTrue(r["granted"])
        self.assertLess(took, 2)                   # not the 10 s wait, nor a 5 s poll
        self.assertGreater(took, 0.4)
        # Still held by b: a's poll gives up after its own wait.
        self.assertEqual(self.wb.lease_call("/acquire", dict(a, wait=0.5))["position"], 1)


class IdleWaitTest(unittest.TestCase):
    """wait_idle's three lengths, on a fake clock: no real sleeping."""

    def setUp(self):
        from unittest import mock
        self.wb = import_wiibench()
        self.now, self.probes, self.answers = 0.0, 0, []
        def sleep(s):
            self.now += s
        def state():
            self.probes += 1
            return (self.answers.pop(0) if self.answers else "hbc"), "1.8.8"
        patches = [mock.patch.object(self.wb.time, "monotonic", lambda: self.now),
                   mock.patch.object(self.wb.time, "sleep", sleep),
                   mock.patch.object(self.wb, "wii_state", state),
                   mock.patch.object(self.wb, "agent_app", lambda: "otherapp")]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def test_full_wait_after_someone_outside_the_queue(self):
        self.wb.wait_idle(20, 20)
        self.assertEqual((self.now, self.probes), (20.0, 5))

    def test_quick_after_a_queue_job(self):
        self.wb.wait_idle(20, 2)
        self.assertEqual((self.now, self.probes), (2.0, 3))

    def test_one_answer_for_a_chained_job(self):
        self.wb.wait_idle(20, 0)
        self.assertEqual((self.now, self.probes), (0.0, 1))

    def test_a_busy_wii_brings_back_the_full_wait(self):
        self.answers = ["hbc", "agent", "off"]
        self.wb.wait_idle(20, 2)
        self.assertGreaterEqual(self.now, 2 + 20)
        self.assertGreater(self.probes, 5)


class DispatcherTest(unittest.TestCase):
    """A real dispatcher against the fake HBC: the idle wait it uses, and same-agent chains."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(root / "tests"))
        from test_hbc_tool import FakeHBC
        cls.fake = FakeHBC()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = pathlib.Path(self.tmp.name)
        self.env = dict(os.environ, WII_BENCH_HOME=self.tmp.name, WII_BENCH_NO_DISPATCH="1",
                        WII_BENCH_IP="127.0.0.1", WII_BENCH_PORT=str(self.fake.port),
                        WII_BENCH_SERVER="", WII_BENCH_AGENT="")
        self.env.pop("CLAUDE_CODE_SESSION_ID", None)
        self.fake.agent = False

    def add(self, name, agent="", secs=0.5):
        code = f"import time; print(time.time()); time.sleep({secs})"
        r = subprocess.run([sys.executable, str(BENCH), "add", "--name", name, "--agent", agent, "--",
                            sys.executable, "-c", code], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def dispatch(self, idle):
        d = subprocess.Popen([sys.executable, str(BENCH), "run", "--idle", str(idle)], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.addCleanup(d.wait)
        self.addCleanup(d.kill)
        return d

    def done(self, job_id, timeout=60):
        import time
        end = time.monotonic() + timeout
        p = self.home / "queue/done" / f"{job_id}.log"
        while not p.exists():
            self.assertLess(time.monotonic(), end, f"{job_id} did not finish")
            time.sleep(0.2)
        time.sleep(0.2)
        return float(p.read_text().split()[0]), json.loads((self.home / "queue/done" / f"{job_id}.json").read_text())

    def test_same_agent_chains_and_another_agent_waits_briefly(self):
        import time
        a1, a2 = self.add("a1", "A"), self.add("a2", "A")
        b1 = self.add("b1", "B")
        t0 = time.time()
        self.dispatch(idle=6)
        s1, j1 = self.done(a1)
        s2, j2 = self.done(a2)
        s3, j3 = self.done(b1)
        self.assertEqual(j1["agent"], "A")
        self.assertGreater(s1 - t0, 5)             # the first job: the full idle wait
        self.assertLess(s2 - s1, 0.5 + 1.5)        # chained: one probe, no wait
        self.assertGreater(s3 - s2, 0.5 + 1.5)     # another agent: the short wait after a queue job
        self.assertLess(s3 - s2, 0.5 + 5)

        time.sleep(3)                              # B's turn holds on for B; A's job ends that at once
        a3 = self.add("a3", "A")
        s4, _ = self.done(a3)
        self.assertLess(s4 - s3, 0.5 + 3 + 5)      # a new turn with the short wait, not 15 s

    def test_a_chain_holds_on_for_the_same_agents_next_add(self):
        import time
        a1 = self.add("a1", "A", secs=0.2)
        self.dispatch(idle=4)
        s1, _ = self.done(a1)
        time.sleep(1)                              # the agent's wait returns, then it adds again
        a2 = self.add("a2", "A", secs=0.2)
        s2, _ = self.done(a2)
        self.assertLess(s2 - s1, 0.2 + 1 + 2.5)    # no idle wait at all (an add takes ~0.5 s)

    def test_no_agent_no_chain(self):
        n1, n2 = self.add("n1"), self.add("n2")
        self.dispatch(idle=4)
        s1, j1 = self.done(n1)
        s2, _ = self.done(n2)
        self.assertIsNone(j1["agent"])
        self.assertGreater(s2 - s1, 0.5 + 1.5)     # the short wait, not a chain

    def test_wait_says_why(self):
        job = self.add("n1")
        p = subprocess.Popen([sys.executable, str(BENCH), "wait", job, "--every", "0.5"], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            _, err = p.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()
            _, err = p.communicate()
        self.assertIn(f"job {job}: next here; queued", err)


class LeaseEventsTest(unittest.TestCase):
    """What the server logs, and a release that leaves the Wii out of HBC."""

    def setUp(self):
        self.wb = import_wiibench()
        self.now, self.log = 0.0, []
        self.leases = self.wb.Leases(ttl=60, clock=lambda: self.now, log=self.log.append)
        self.now = 60.0

    def acq(self, t):
        return self.leases.acquire({"ticket": t, "host": t, "name": "job", "wait": 25})

    def test_events_and_a_wii_left_in_an_app(self):
        self.acq("a")
        self.acq("b")
        self.now += 10
        self.leases.release({"ticket": "a", "wii": "busy: otherapp is running"})
        self.assertEqual(self.leases.status()["left"]["wii"], "busy: otherapp is running")
        self.assertEqual(self.acq("b"), {"granted": True, "released_ago": None})   # no short wait
        self.leases.release({"ticket": "b"})       # a client before 1.9.1: counts as clean
        self.assertIsNone(self.leases.status()["left"])
        self.assertEqual(self.acq("c")["released_ago"], 0.0)
        self.acq("d")
        self.leases.release({"ticket": "d"})       # gave up waiting
        self.acq("e")
        self.now += 61
        self.leases.status()
        kinds = [(e["type"], e["host"]) for e in self.log]
        self.assertEqual(kinds, [("join", "a"), ("grant", "a"), ("join", "b"), ("release", "a"),
                                 ("grant", "b"), ("release", "b"), ("join", "c"), ("grant", "c"),
                                 ("join", "d"), ("leave", "d"), ("join", "e"),
                                 ("expired", "c"), ("expired", "e")])
        release_a = self.log[3]
        self.assertEqual((release_a["held_s"], release_a["wii"]), (10.0, "busy: otherapp is running"))
        self.assertEqual(self.log[4]["waited_s"], 10.0)
        self.assertNotIn("wait", self.leases.status()["waiters"] or [{}])


class HistoryTest(unittest.TestCase):
    """Daily files, gzipped after a week, packed into monthly archives, read back across all."""

    def setUp(self):
        self.wb = import_wiibench()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name) / "history"
        import calendar
        self.t0 = calendar.timegm((2026, 8, 20, 12, 0, 0))
        self.now = self.t0

    def fill(self, days, keep=0):
        h = self.wb.History(self.root, keep_months=keep, clock=lambda: self.now)
        for d in range(days):
            self.now = self.t0 + d * 86400
            h.write({"type": "job", "n": d})
        return h

    def test_rotation_and_the_deep_archive(self):
        h = self.fill(43)                          # 2026-08-20 .. 2026-10-01
        names = sorted(p.name for p in self.root.glob("events-*"))
        self.assertEqual(names[0], "events-2026-09-24.jsonl")       # this week's: live
        self.assertEqual(names[-1], "events-2026-10-01.jsonl")
        self.assertEqual(len(names), 8)
        self.assertEqual(sorted(p.name for p in (self.root / "archive").iterdir()),
                         ["2026-08.tar", "2026-09.tar"])
        import tarfile
        with tarfile.open(self.root / "archive/2026-09.tar") as t:
            self.assertEqual(len(t.getnames()), 23)                 # 09-01 .. 09-23, gzipped
            self.assertTrue(all(n.endswith(".jsonl.gz") for n in t.getnames()))
        self.assertEqual([e["n"] for e in h.read(0)], list(range(43)))
        mid = self.t0 + 10 * 86400                 # 08-30: from the August archive on
        self.assertEqual([e["n"] for e in h.read(mid, mid + 25 * 86400)], list(range(10, 35)))

    def test_keep_months(self):
        self.fill(43, keep=1)
        self.assertEqual(sorted(p.name for p in (self.root / "archive").iterdir()), ["2026-09.tar"])


class ServerHistoryTest(unittest.TestCase):
    def test_events_and_history_over_http(self):
        import threading
        wb = import_wiibench()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        srv = wb.make_server(0, ttl=30, host="127.0.0.1", history=tmp.name)
        srv.leases.grace_until = 0
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        os.environ["WII_BENCH_SERVER"] = f"http://127.0.0.1:{srv.server_address[1]}"
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")
        self.assertTrue(wb.lease_call("/acquire", {"ticket": "x", "host": "pc", "name": "n"})["granted"])
        self.assertTrue(wb.lease_call("/event", {"type": "job", "host": "pc", "name": "n", "secs": 5})["ok"])
        wb.lease_call("/release", {"ticket": "x", "wii": "hbc"})
        evs = wb.lease_call("/history?since=0")["events"]
        self.assertEqual([e["type"] for e in evs], ["join", "grant", "job", "release"])
        self.assertTrue(all("utc" in e and "t" in e for e in evs))
        import urllib.error
        with self.assertRaises(urllib.error.HTTPError):
            wb.lease_call("/event", {"no": "type"})


class LeaseDispatcherTest(unittest.TestCase):
    """A real dispatcher, a real lease server and a rival workstation, against the fake HBC."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(root / "tests"))
        from test_hbc_tool import FakeHBC
        cls.fake = FakeHBC()

    def setUp(self):
        import threading
        self.wb = import_wiibench()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = pathlib.Path(self.tmp.name)
        self.srv = self.wb.make_server(0, ttl=30, host="127.0.0.1", history=str(self.home / "server-history"))
        self.srv.leases.grace_until = 0
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.env = dict(os.environ, WII_BENCH_HOME=self.tmp.name, WII_BENCH_NO_DISPATCH="1",
                        WII_BENCH_IP="127.0.0.1", WII_BENCH_PORT=str(self.fake.port),
                        WII_BENCH_SERVER=self.url, WII_BENCH_AGENT="", WII_BENCH_RETURN_WAIT="3")
        self.env.pop("CLAUDE_CODE_SESSION_ID", None)
        self.fake.agent = False
        os.environ["WII_BENCH_SERVER"] = self.url  # for lease_call in this process
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")

    add = DispatcherTest.add
    done = DispatcherTest.done

    def dispatch(self):
        d = subprocess.Popen([sys.executable, str(BENCH), "run", "--idle", "4"], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.addCleanup(d.wait)
        self.addCleanup(d.kill)
        return d

    def until(self, cond, secs=30):
        import time
        end = time.monotonic() + secs
        while not cond():
            self.assertLess(time.monotonic(), end)
            time.sleep(0.1)

    def rival(self, hold=0.5):
        """Another workstation: waits in line (long polls), holds the Wii briefly, lets go."""
        import threading
        import time
        got = {}

        def run():
            info = {"ticket": "rival", "host": "rival-pc", "name": "rival job"}
            while not self.wb.lease_call("/acquire", dict(info, wait=10), timeout=20)["granted"]:
                pass
            got["t"] = time.time()
            time.sleep(hold)
            self.wb.lease_call("/release", dict(info, wii="hbc"))
        th = threading.Thread(target=run, daemon=True)
        th.start()
        return got, th

    def history(self):
        return self.wb.lease_call("/history?since=0")["events"]

    def test_no_chain_while_someone_waits(self):
        a1, a2 = self.add("a1", "A", secs=2), self.add("a2", "A", secs=0.3)
        self.dispatch()
        self.until(lambda: list((self.home / "queue/running").glob("*.json")))
        got, th = self.rival()
        s1, _ = self.done(a1)
        s2, j2 = self.done(a2)
        th.join(10)
        self.assertGreater(got["t"], s1 + 2)       # the rival went after a1 ...
        self.assertLess(got["t"], s2)              # ... and before a2
        self.assertFalse(j2["chained"])
        self.assertEqual(j2["hbc_back_s"], 0.0)
        hosts = [e["host"] for e in self.history() if e["type"] == "grant"]
        self.assertEqual(hosts[-2:], ["rival-pc", hosts[0]])

    def test_the_hold_lets_go_when_someone_joins(self):
        import time
        a1 = self.add("a1", "A", secs=0.3)
        self.dispatch()
        self.done(a1)
        time.sleep(1)                              # inside the 15 s hold for A's next job
        t = time.time()
        got, th = self.rival(hold=0)
        th.join(10)
        self.assertLess(got["t"] - t, 3)           # not the rest of the 15 s

    def test_a_wii_left_in_an_app_is_reported(self):
        n1 = self.add("n1", "A", secs=1.5)
        self.dispatch()
        self.until(lambda: list((self.home / "queue/running").glob("*.json")))
        self.fake.agent = True                     # the job's app never goes back to HBC
        try:
            _, j = self.done(n1)
            self.until(lambda: self.wb.lease_call("/status").get("left"), 15)
            left = self.wb.lease_call("/status")["left"]
            out = subprocess.run([sys.executable, str(BENCH), "status"], env=self.env,
                                 capture_output=True, text=True, timeout=30).stdout
        finally:
            self.fake.agent = False
        j = json.loads((self.home / "queue/done" / f"{n1}.json").read_text())
        self.assertIsNone(j["hbc_back_s"])
        self.assertEqual(j["wii_left"], "busy: otherapp is running")
        self.assertEqual(left["wii"], "busy: otherapp is running")
        self.assertIn("left the Wii busy: otherapp is running", out)
        rel = [e for e in self.history() if e["type"] == "release"]
        self.assertEqual(rel[-1]["wii"], "busy: otherapp is running")
        local = self.wb.History(self.home / "history").read(0)
        self.assertEqual([e["id"] for e in local], [n1])

    def test_status_leaves_a_run_alone(self):
        seen = []
        orig = self.fake.handle
        self.fake.handle = lambda c, a: (seen.append(1), orig(c, a))
        self.addCleanup(setattr, self.fake, "handle", orig)
        status = lambda: subprocess.run([sys.executable, str(BENCH), "status"], env=self.env,
                                        capture_output=True, text=True, timeout=30).stdout
        self.assertTrue(self.wb.lease_call("/acquire", {"ticket": "o", "host": "other-pc", "name": "its run"})["granted"])
        out = status()
        self.assertIn("in use: other-pc has it for its run (not probed)", out)
        self.wb.lease_call("/release", {"ticket": "o", "wii": "hbc"})
        running = self.home / "queue/running"
        running.mkdir(parents=True)
        (running / "x.json").write_text(json.dumps({"id": "x", "name": "a run here", "added": "now"}))
        out = status()
        self.assertIn("in use by job a run here here (not probed)", out)
        self.assertEqual(seen, [])                 # the Wii was never contacted
        (running / "x.json").unlink()
        self.assertIn("in HBC", status())
        self.assertEqual(len(seen), 1)


def bench_home_with_trouble(home):
    """A state folder with one of everything the monitor reports."""
    home = pathlib.Path(home)
    q = home / "queue"
    for d in ("pending", "running", "done"):
        (q / d).mkdir(parents=True, exist_ok=True)
    now = __import__("time").time()
    stamp = lambda ago: __import__("time").strftime("%Y-%m-%d %H:%M:%S", __import__("time").localtime(now - ago))
    def done(jid, name, ago, **more):
        j = dict(id=jid, name=name, cmd=["x"], cwd=".", timeout=600, added=stamp(ago + 60), started=stamp(ago + 30),
                 finished=stamp(ago), secs=30, exit=0, agent="A", hbc_back_s=0.0, chained=False)
        j.update(more)
        (q / "done" / f"{jid}.json").write_text(json.dumps(j))
        (q / "done" / f"{jid}.log").write_text(more.pop("log", "all fine\n"))
    done("j-ok", "fine job", 4000)
    done("j-fail", "failing job", 3000, exit=1, log="step 1\nboom: the build is broken\n")
    done("j-time", "slow job", 2000, exit="timeout", secs=600)
    done("j-left", "leaving job", 1000, hbc_back_s=None, wii_left="busy: otherapp is running")
    done("j-old", "ancient failure", 3 * 86400, exit=3)          # outside the window
    (q / "running" / "j-run.json").write_text(json.dumps(dict(id="j-run", name="running job", added=stamp(100),
                                                               started=stamp(50), timeout=600, agent="B")))
    for i, n in enumerate(("queued one", "queued two")):
        p = q / "pending" / f"j-q{i}.json"
        p.write_text(json.dumps(dict(id=f"j-q{i}", name=n, added=stamp(40 - i), agent="B")))
    (home / "dispatcher.log").write_text("\n".join([
        f"{stamp(900)} lease taken for something",
        f"{stamp(800)} lease lost: another workstation may have the Wii",
        f"{stamp(700)} done x y: exit 0 in 1 s",
        "Traceback (most recent call last):",
        '  File "wiibench.py", line 1, in <module>',
        "OSError: [Errno 28] No space left on device",
        f"{stamp(600)} lease server http://h:4310 unreachable: timed out",
    ]) + "\n")
    return home


class SnapshotTest(unittest.TestCase):
    """wiibench.py snapshot, what monitor.sh draws: problems found, sorting, and no Wii contact."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(root / "tests"))
        from test_hbc_tool import FakeHBC
        cls.fake = FakeHBC()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = bench_home_with_trouble(self.tmp.name)
        self.env = dict(os.environ, WII_BENCH_HOME=self.tmp.name, WII_BENCH_SERVER="",
                        WII_BENCH_IP="127.0.0.1", WII_BENCH_PORT=str(self.fake.port))

    def snap(self, *args, env=None):
        r = subprocess.run([sys.executable, str(BENCH), "snapshot", *args], env=env or self.env,
                           capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.decode("utf-8")

    def test_problems_queue_and_no_contact_with_the_wii(self):
        seen = []
        orig = self.fake.handle
        self.fake.handle = lambda c, a: (seen.append(1), orig(c, a))
        self.addCleanup(setattr, self.fake, "handle", orig)
        (self.home / "dispatcher.state").write_text(json.dumps({"job": "j-ok", "detail": "running", "since": 1}))
        s = json.loads(self.snap("--json"))
        kv, rows = s["kv"], s["rows"]
        self.assertIsNone(kv["dispatcher_detail"])   # its job is done: not "running" forever
        kinds = {(e["kind"], e["id"]) for e in rows["errors"]}
        self.assertIn(("exit 1", "j-fail"), kinds)
        self.assertIn(("timeout", "j-time"), kinds)
        self.assertIn(("Wii left", "j-left"), kinds)
        self.assertNotIn("j-old", {e["id"] for e in rows["errors"]})
        by_kind = {e["kind"]: e for e in rows["errors"]}
        self.assertEqual(by_kind["exit 1"]["detail"], "boom: the build is broken")   # the job log's last line
        self.assertEqual(by_kind["dispatcher crashed"]["detail"], "OSError: [Errno 28] No space left on device")
        self.assertIn("lease lost", by_kind)
        self.assertEqual(by_kind["lease server"]["severity"], "warn")
        self.assertIn("no dispatcher", by_kind)    # two jobs queued, nobody to run them
        self.assertEqual(kv["errors_err"], 5)
        self.assertEqual([r["id"] for r in rows["running"]], ["j-run"])
        self.assertEqual([r["name"] for r in rows["pending"]], ["queued one", "queued two"])
        self.assertEqual(rows["done"][0]["id"], "j-left")
        self.assertEqual(rows["done"][0]["hbc"], "left")
        self.assertEqual(kv["wii_last"], "left busy: otherapp is running by leaving job")
        self.assertEqual(seen, [])                 # the Wii was never contacted

    def test_tab_separated_sorted_and_filtered(self):
        out = self.snap("--sort", "done:secs:desc", "--filter", "errors:timeout")
        done = [l.split("\t") for l in out.splitlines() if l.startswith("@row\tdone\t")]
        self.assertEqual(done[0][4], "slow job")   # 600 s first (@row, done, finished, id, name, ...)
        errs = [l.split("\t") for l in out.splitlines() if l.startswith("@row\terrors\t")]
        self.assertEqual([e[4] for e in errs], ["timeout"])
        self.assertTrue(all("" not in l.split("\t") for l in out.splitlines()))     # never an empty field

    def test_lease_server_facts(self):
        import threading
        wb = import_wiibench()
        srv = wb.make_server(0, ttl=30, host="127.0.0.1", history=str(self.home / "srv"))
        srv.leases.grace_until = 0
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        url = f"http://127.0.0.1:{srv.server_address[1]}"
        os.environ["WII_BENCH_SERVER"] = url
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")
        wb.lease_call("/acquire", {"ticket": "m", "host": "old-mac", "name": "its job"})
        wb.lease_call("/release", {"ticket": "m", "wii": "busy or off"})          # no job record: old client
        wb.lease_call("/acquire", {"ticket": "h", "host": "holder-pc", "name": "running there"})
        wb.lease_call("/acquire", {"ticket": "w", "host": "waiter-pc", "name": "next one"})
        s = json.loads(self.snap("--json", env=dict(self.env, WII_BENCH_SERVER=url)))
        kv, rows = s["kv"], s["rows"]
        self.assertEqual((kv["holder_host"], kv["holder_name"]), ("holder-pc", "running there"))
        self.assertTrue(kv["holder_for"].endswith("s"))
        self.assertEqual([(r["host"], r["name"]) for r in rows["waiting"]], [("waiter-pc", "next one")])
        self.assertEqual(kv["left_wii"], "busy or off")
        hosts = {(e["kind"], e["host"]) for e in rows["errors"]}
        self.assertIn(("Wii left", "old-mac"), hosts)          # from the server's history and its status
        self.assertIn(("old dispatcher", "old-mac"), hosts)
        self.assertEqual(kv["history_src"], "lease server")


class SpeedTest(unittest.TestCase):
    """The monitor's fast paths: one lookup and one kept connection per process, and a
    served snapshot that fetches only new history."""

    def setUp(self):
        import threading
        self.wb = import_wiibench()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.srv = self.wb.make_server(0, ttl=30, host="127.0.0.1", history=self.tmp.name + "/srv")
        self.srv.leases.grace_until = 0
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        # "localhost", not the IP, so there is a name to look up
        self.url = f"http://localhost:{self.srv.server_address[1]}"
        os.environ["WII_BENCH_SERVER"] = self.url
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")

    def test_one_lookup_and_one_connection(self):
        from unittest import mock
        import socket as so
        import ipaddress
        real, calls = so.getaddrinfo, []
        def counting(*a, **k):
            try:
                ipaddress.ip_address(a[0])         # connecting to the IP itself: no lookup
            except ValueError:
                calls.append(a[0])
            return real(*a, **k)
        self.wb._ADDR.clear()
        self.wb._conns().clear()
        with mock.patch.object(self.wb.socket, "getaddrinfo", counting):
            for _ in range(5):
                self.wb.lease_call("/status")
            self.wb.lease_call("/acquire", {"ticket": "x", "host": "h", "name": "n"})
        self.assertEqual(calls, ["localhost"])
        conns = self.wb._conns()
        self.assertEqual(len(conns), 1)            # kept open: the server speaks HTTP/1.1
        self.assertIsNotNone(next(iter(conns.values())).sock)

    def test_a_dropped_kept_connection_is_retried(self):
        self.wb.lease_call("/status")
        next(iter(self.wb._conns().values())).sock.close()      # as an idle timeout would
        self.assertIn("holder", self.wb.lease_call("/status"))

    def test_served_snapshots_fetch_only_new_history(self):
        home = bench_home_with_trouble(self.tmp.name + "/home")
        env = dict(os.environ, WII_BENCH_HOME=str(home), WII_BENCH_SERVER=self.url,
                   WII_BENCH_IP="127.0.0.1", WII_BENCH_PORT="9")
        p = subprocess.Popen([sys.executable, str(BENCH), "snapshot", "--serve"], env=env,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.addCleanup(p.wait)
        self.addCleanup(p.stdin.close)

        def ask(req="24\t\t"):
            p.stdin.write((req + "\n").encode())
            p.stdin.flush()
            out = []
            while True:
                l = p.stdout.readline().decode("utf-8").rstrip("\r\n")
                self.assertTrue(l or out, "the snapshot process ended")
                if l == "@end":
                    return out
                out.append(l)

        hosts = lambda out: [l.split("\t")[2] for l in out if l.startswith("@row\thosts\t")]
        self.assertEqual(hosts(ask()), [])
        self.wb.lease_call("/event", {"type": "job", "host": "pc-a", "name": "a", "secs": 5, "exit": 0})
        self.assertEqual(hosts(ask()), ["pc-a"])
        self.wb.lease_call("/event", {"type": "job", "host": "pc-b", "name": "b", "secs": 5, "exit": 1})
        out = ask("24\tdone:secs:1\terrors:exit 1")
        self.assertEqual(hosts(out), ["pc-a", "pc-b"])          # each event once, not twice
        errs = [l.split("\t") for l in out if l.startswith("@row\terrors\t")]
        self.assertIn(("exit 1", "pc-b"), {(e[4], e[5]) for e in errs})   # @row errors when sev KIND HOST
        self.assertTrue(all("exit 1" in "\t".join(e) for e in errs))   # the filter applied


def bash_for_monitor():
    """A bash that runs monitor.sh here, or None: never Windows' WSL launcher in System32."""
    import shutil
    b = shutil.which("bash")
    if not b or "system32" in b.lower():
        return None
    return b


class MonitorTest(unittest.TestCase):
    """monitor.sh --once, every page, on this platform's bash."""

    def test_every_page_once(self):
        b = bash_for_monitor()
        if not b:
            self.skipTest("no bash on PATH")
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        bench_home_with_trouble(tmp.name)
        env = dict(os.environ, WII_BENCH_HOME=tmp.name, WII_BENCH_SERVER="", WII_BENCH_IP="127.0.0.1",
                   WII_BENCH_PORT="9", WII_BENCH_PYTHON=sys.executable.replace("\\", "/"),
                   COLUMNS="160", LINES="50", TERM="xterm")
        ver = subprocess.run([b, "-c", "echo ${BASH_VERSINFO[0]}.${BASH_VERSINFO[1]}"], capture_output=True, text=True).stdout.strip()
        major, minor = (int(x) for x in ver.split("."))
        mon = str(root / "tools/wii-bench/monitor.sh").replace("\\", "/")
        if (major, minor) < (4, 3):                # macOS's own bash: a clear message, not a crash
            r = subprocess.run([b, mon, "--once"], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 2)
            self.assertIn("needs bash 4.3", r.stderr)
            return
        want = {"queue": ["RUNNING HERE", "running job", "QUEUED HERE", "queued two", "RECENT JOBS HERE", "leaving job",
                          "no lease server", "5 error(s)"],
                "errors": ["PROBLEMS", "boom: the build is broken", "No space left on device", "Wii left"],
                "history": ["WORKSTATIONS", "LONGEST WAITS"],
                "log": ["DISPATCHER LOG", "lease lost"]}
        for page, texts in want.items():
            r = subprocess.run([b, mon, "--once", f"--page={page}"], env=env, capture_output=True, timeout=120)
            out = r.stdout.decode("utf-8", "replace")
            self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
            self.assertNotIn("\033[", out)         # plain text when not a terminal
            self.assertIn(f"[{list(want).index(page) + 1} {page}]", out)
            for t in texts:
                self.assertIn(t, out, f"{page}: {t}")
        r = subprocess.run([b, mon, "--help"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0)
        self.assertIn("It never contacts the Wii", r.stdout)


class ReportTest(unittest.TestCase):
    def test_report_from_this_workstations_history(self):
        import time
        wb = import_wiibench()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        h = wb.History(pathlib.Path(tmp.name) / "history")
        for host, secs, ex in (("pc-a", 120, 0), ("pc-a", 60, 1), ("pc-b", 30, "timeout")):
            h.write({"type": "job", "host": host, "name": f"{host} job", "secs": secs, "exit": ex,
                     "hbc_back_s": 0.0})
        env = dict(os.environ, WII_BENCH_HOME=tmp.name, WII_BENCH_SERVER="")
        out = subprocess.run([sys.executable, str(BENCH), "report", "--hours", "1"], env=env,
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("3 jobs, 0 chained", out.stdout)
        self.assertRegex(out.stdout, r"pc-a\s+2\s+1\s+3 ")
        self.assertRegex(out.stdout, r"pc-b\s+1\s+1\s+0 ")
        self.assertIn("pc-b's pc-b job timed out after 30 s", out.stdout)
        old = subprocess.run([sys.executable, str(BENCH), "report", "--since", "2020-01-01", "--json"],
                             env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(len(json.loads(old.stdout)), 3)


if __name__ == "__main__":
    unittest.main()
