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


if __name__ == "__main__":
    unittest.main()
