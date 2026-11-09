"""The stuck-USB guard (sdk/usbguard) on a PC: the phases and their timing,
a retry after a replug, the free path and the no-AHBPROT path.

tests/host/usbguard_host fakes the EHCI registers (port 0 a USB 2.0 drive, port
3 a USB 1.1 device at the companion) and runs fake calls that block until a
pull or a free, fail, return late or give up as d2x rc3 does (-101). The
on_wait it passes pulls, replugs, frees or gives up at set times. warn_ms is
200 ms here, the replug wait without AHBPROT 300 ms, the settle 100 ms and d2x's
give-up 250 ms (1500 and 6500 ms on a Wii; the build overrides them with -D).
Lower time bounds are exact; upper ones leave room for a loaded PC."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import test_cdblog  # noqa: E402

CC = test_cdblog.CC
WARN = 200


@unittest.skipUnless(CC, "no C compiler")
class UsbguardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = pathlib.Path(cls.tmp.name) / "usbguard_host"
        subprocess.run([CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-DUSBGUARD_NO_OGC",
                        "-DUSBGUARD_SETTLE_MS=100", "-DUSBGUARD_D2X_GIVEUP_MS=250",
                        "-I", str(root / "sdk/usbguard"),
                        "-I", str(root / "tests/host/usbguard_host/include"),
                        str(root / "tests/host/usbguard_host/main.c"),
                        str(root / "sdk/usbguard/usbguard.c"), "-o", str(cls.exe)]
                       + ([] if sys.platform == "win32" else ["-lpthread"]), check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_host(self, *args):
        out = subprocess.run([str(self.exe), *args], capture_output=True, text=True,
                             timeout=30)
        lines = out.stdout.split("\n")[:-1]
        self.assertEqual(out.returncode, 0, "\n".join(lines))
        return lines

    @staticmethod
    def get(lines, word):
        return [l.split()[1:] for l in lines if l.split()[0] == word]

    def phases(self, lines):
        return [(p[0], int(p[1])) for p in self.get(lines, "phase")]

    def results(self, lines):
        return [r[0] for r in self.get(lines, "result")]

    def calls(self, lines):
        return int(self.get(lines, "calls")[0][0])

    # ---- Healthy calls: as before -----------------------------------------

    def test_quick_call_returns_its_result(self):
        lines = self.run_host("fns=ok")
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.phases(lines), [])
        self.assertEqual(self.get(lines, "write"), [])

    def test_quick_failure_returns_its_result(self):
        lines = self.run_host("fns=fail")
        self.assertEqual(self.results(lines), ["-5"])
        self.assertEqual(self.calls(lines), 1)

    def test_late_answer_goes_waiting_then_stuck_then_returns(self):
        lines = self.run_host("fns=late")
        p = self.phases(lines)
        self.assertEqual([n for n, _ in p], ["WAITING", "STUCK"])
        self.assertLess(p[0][1], WARN)
        self.assertGreaterEqual(p[1][1], WARN)
        self.assertLess(p[1][1], WARN + 1500)
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.calls(lines), 1)  # a success is not retried

    # ---- A pull --------------------------------------------------------------

    def test_pull_then_replug_retries_once(self):
        lines = self.run_host("fns=block,ok", "act=pull@400,replug@600")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK", "REPLUG"])
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.calls(lines), 2)
        # The retry waits for the replug and the settle.
        self.assertGreaterEqual(int(self.get(lines, "result")[0][1]), 600 + 100)

    def test_late_success_after_a_pull_waits_for_the_replug(self):
        # A stuck IOS_Open can answer with a handle as the drive is pulled;
        # the next call needs the drive back.
        lines = self.run_host("fns=blockok", "act=pull@300,replug@600")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK", "REPLUG"])
        self.assertEqual(self.results(lines), ["3"])
        self.assertEqual(self.calls(lines), 1)  # a success is not run again
        self.assertGreaterEqual(int(self.get(lines, "result")[0][1]), 600 + 100)

    def test_late_success_with_no_drive_at_the_start_does_not_wait(self):
        lines = self.run_host("nodrive", "fns=late")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK"])
        self.assertEqual(self.results(lines), ["7"])

    def test_stuck_again_after_the_retry_is_freed_not_retried_again(self):
        lines = self.run_host("fns=block,block", "act=pull@300,replug@400,pull@800")
        self.assertEqual(self.results(lines), ["FREED"])
        self.assertEqual(self.calls(lines), 2)

    def test_pull_without_ahbprot_retries_after_replug_ms(self):
        lines = self.run_host("noaccess", "fns=block,ok", "act=pull@400")
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.calls(lines), 2)
        self.assertGreaterEqual(int(self.get(lines, "result")[0][1]), 400 + 300)
        self.assertEqual(self.get(lines, "check"), [["NO_ACCESS"]])

    # ---- usbguard_free -------------------------------------------------------

    def test_free_cuts_the_port_off_and_returns_freed(self):
        lines = self.run_host("fns=block", "act=free@300", "after=pull")
        self.assertEqual(self.results(lines), ["FREED"])
        self.assertEqual(self.calls(lines), 1)  # no retry: the drive is cut off
        # PORTSC 1005 (power, enabled, connected): PE cleared, PEC and OCC
        # written 0, CSC written 1 to clear it; CCS is read-only.
        self.assertEqual(self.get(lines, "write"), [["0", "1003"]])
        self.assertEqual(self.get(lines, "freed"), [["0", "1005", "1001"]])
        # Cut off, not pulled; then the pull is seen.
        self.assertEqual(self.get(lines, "check"), [["CUT_OFF"], ["OK"]])
        self.assertEqual(self.get(lines, "pulled"), [["0"], ["1"]])

    def test_free_is_freed_even_when_the_request_answers_ok(self):
        # After the free the stuck IOS_Open can still answer with a handle.
        lines = self.run_host("fns=blockok", "act=free@300")
        self.assertEqual(self.results(lines), ["FREED"])
        self.assertEqual(self.calls(lines), 1)

    def test_freed_ports_lists_the_latest_free_only(self):
        lines = self.run_host("other", "after=free2")
        self.assertEqual(self.get(lines, "free"), [["1"], ["1"]])
        self.assertEqual([f[0] for f in self.get(lines, "freed")], ["0", "1"])

    def test_free_leaves_the_companion_port_alone(self):
        lines = self.run_host("fns=block", "act=free@300")
        self.assertNotIn("3", [w[0] for w in self.get(lines, "write")])

    def test_no_ahbprot_no_free(self):
        lines = self.run_host("noaccess", "fns=block", "act=free@300,giveup@500")
        self.assertEqual(self.get(lines, "free"), [["0"]])
        self.assertEqual(self.get(lines, "write"), [])
        self.assertEqual(self.results(lines), ["STUCK"])

    # ---- Giving up and the workers --------------------------------------------

    def test_give_up_keeps_the_worker_until_it_comes_back(self):
        lines = self.run_host("fns=block,ok", "act=giveup@300", "after=release")
        self.assertEqual(self.results(lines), ["STUCK", "7"])
        self.assertEqual(self.get(lines, "blocked"), [["1"], ["0"]])
        self.assertEqual(self.calls(lines), 2)

    def test_call_while_blocked_starts_stuck_and_workers_run_out(self):
        lines = self.run_host("fns=block", "repeat=5", "act=giveup@100")
        self.assertEqual(self.results(lines), ["STUCK"] * 5)
        names = [n for n, _ in self.phases(lines)]
        self.assertEqual(names, ["WAITING", "STUCK", "STUCK", "STUCK"])
        self.assertEqual(self.calls(lines), 4)  # the fifth had no worker and did not run

    # ---- d2x rc3's -101 --------------------------------------------------------

    def test_d2x_gave_up_with_a_drive_in_waits_for_the_pull(self):
        lines = self.run_host("fns=d2x,ok", "act=pull@500,replug@700")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK", "REPLUG"])
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.calls(lines), 2)

    def test_d2x_gave_up_with_no_drive_is_returned(self):
        lines = self.run_host("nodrive", "fns=d2x")
        self.assertEqual(self.results(lines), ["-101"])

    def test_d2x_gave_up_then_free(self):
        lines = self.run_host("fns=d2x", "act=free@500")
        self.assertEqual(self.results(lines), ["FREED"])

    def test_d2x_without_ahbprot_slow_is_stuck_quick_is_not(self):
        self.assertEqual(self.results(self.run_host("noaccess", "fns=d2x")), ["STUCK"])
        self.assertEqual(self.results(self.run_host("noaccess", "fns=d2xfast")), ["-101"])

    def test_quick_d2x_gave_up_with_a_drive_in_is_returned(self):
        # A -101 sooner than d2x's give-up time is an answer, drive or not.
        lines = self.run_host("fns=d2xfast,ok")
        self.assertEqual(self.results(lines), ["-101"])
        self.assertEqual(self.calls(lines), 1)
        self.assertEqual(self.phases(lines), [])

    def test_d2x_gave_up_without_on_wait_gives_up_after_replug_ms(self):
        # AHBPROT, the drive never leaves, nothing to say stop: bounded.
        lines = self.run_host("nowait", "replug=300", "fns=d2x,ok")
        self.assertEqual(self.results(lines), ["STUCK"])
        t = int(self.get(lines, "result")[0][1])
        self.assertGreaterEqual(t, 300 + 300)  # the call, then replug_ms
        self.assertLess(t, 300 + 300 + 3000)
        self.assertEqual(self.calls(lines), 1)

    # ---- Giving up while waiting for the pull or the replug --------------------

    def test_give_up_during_the_replug_wait_is_freed_without_a_retry(self):
        lines = self.run_host("fns=block,ok", "act=pull@300,giveup@500")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK", "REPLUG"])
        self.assertEqual(self.results(lines), ["FREED"])
        self.assertEqual(self.calls(lines), 1)

    def test_give_up_during_the_wait_for_the_pull_is_stuck_with_no_worker_left(self):
        # d2x answers -101 at 300 ms, so the worker is gone when STUCK comes
        # back: STUCK does not always mean a request is still in IOS.
        lines = self.run_host("fns=d2x,ok", "act=giveup@600")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK"])
        self.assertEqual(self.results(lines), ["STUCK"])
        self.assertEqual(self.calls(lines), 1)
        self.assertEqual(self.get(lines, "blocked"), [["0"]])

    # ---- Another device on another port ---------------------------------------

    def test_the_drive_replugged_into_another_port_is_the_replug(self):
        lines = self.run_host("fns=block,ok", "act=pull@300,replugother@600")
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.calls(lines), 2)
        self.assertGreaterEqual(int(self.get(lines, "result")[0][1]), 600 + 100)

    def test_a_second_device_is_not_the_replug_when_the_drive_stays_out(self):
        # Port 1 keeps its device and port 2 stays empty: the drive is not back,
        # so with no replug the wait ends only at the give-up.
        lines = self.run_host("other", "fns=block,ok", "act=pull@300,giveup@1000")
        self.assertEqual(self.results(lines), ["FREED"])
        self.assertEqual(self.calls(lines), 1)

    def test_a_second_device_does_not_hide_the_pull(self):
        lines = self.run_host("other", "fns=d2x,ok", "act=pull@500,replug@700")
        self.assertEqual([n for n, _ in self.phases(lines)], ["WAITING", "STUCK", "REPLUG"])
        self.assertEqual(self.results(lines), ["7"])
        self.assertEqual(self.calls(lines), 2)

    def test_a_second_device_does_not_stand_in_for_the_replug(self):
        lines = self.run_host("other", "fns=block,ok", "act=pull@300,replug@700")
        self.assertEqual(self.results(lines), ["7"])
        self.assertGreaterEqual(int(self.get(lines, "result")[0][1]), 700 + 100)

    def test_free_cuts_a_second_device_off_too(self):
        lines = self.run_host("other", "fns=block", "act=free@300")
        self.assertEqual([w[0] for w in self.get(lines, "write")], ["0", "1"])

    # ---- A reload --------------------------------------------------------------

    def test_a_reload_that_finishes_after_a_free_passes(self):
        lines = self.run_host("reload", "fns=blockok", "act=free@300")
        self.assertEqual(self.results(lines), ["3"])
        self.assertEqual(self.calls(lines), 1)

    def test_a_reload_that_fails_after_a_free_is_freed_and_not_retried(self):
        lines = self.run_host("reload", "fns=block,ok", "act=free@300")
        self.assertEqual(self.results(lines), ["FREED"])
        self.assertEqual(self.calls(lines), 1)

    # ---- usbguard_mount ----------------------------------------------------------

    def test_mount(self):
        self.assertEqual(self.results(self.run_host("mount", "fns=ok")), ["0"])
        self.assertEqual(self.results(self.run_host("mount", "fns=fail")), ["NO_DRIVE"])
        self.assertEqual(self.results(self.run_host("mount", "nofat", "fns=ok")),
                         ["NOT_MOUNTED"])
        self.assertEqual(self.results(self.run_host("mount", "nodrive", "fns=d2x")),
                         ["NO_DRIVE"])

    def test_mount_slow_startup_failure_with_a_drive_is_stuck(self):
        lines = self.run_host("mount", "fns=d2x,ok", "act=pull@500,replug@600")
        self.assertEqual(self.results(lines), ["0"])
        self.assertEqual(len(self.get(lines, "fatMount")), 1)


if __name__ == "__main__":
    unittest.main()
