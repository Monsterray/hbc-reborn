#!/usr/bin/env python3
"""tests/lib/wii_checks.py: ensure_hbc skips the send for the running build,
send_app survives a send that times out, production Wiis are refused."""

import os
import pathlib
import sys
import unittest
from unittest import mock

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401
import hbc  # noqa: E402
import wii_checks  # noqa: E402

TAG = "1.2.3+4.gabcdef0"


class EnsureHbc(unittest.TestCase):
    def test_running_build_is_not_sent(self):
        with mock.patch.object(hbc, "version", return_value=TAG), \
             mock.patch.object(hbc, "send") as send:
            self.assertEqual(wii_checks.ensure_hbc("w", expected=TAG, say=lambda *_: None), "reused")
            send.assert_not_called()

    def test_fresh_dirty_and_other_builds_are_sent(self):
        for expected, running, fresh in ((TAG, TAG, True), (TAG + ".dirty", TAG + ".dirty", False),
                                         (TAG, "1.2.3+3.g1111111", False), (TAG, "1.0 agent", False)):
            with mock.patch.object(hbc, "version", return_value=running), \
                 mock.patch.object(hbc, "send") as send, \
                 mock.patch.object(hbc, "relaunch_wait") as wait, \
                 mock.patch.object(wii_checks.shutil, "copy"):
                self.assertEqual(wii_checks.ensure_hbc("w", expected=expected, fresh=fresh,
                                                       say=lambda *_: None), "sent")
                send.assert_called_once()
                wait.assert_called_once_with("w", expected)

    def test_no_answer_is_sent(self):
        with mock.patch.object(hbc, "version", side_effect=OSError("down")), \
             mock.patch.object(hbc, "send") as send, \
             mock.patch.object(hbc, "relaunch_wait"), \
             mock.patch.object(wii_checks.shutil, "copy"):
            self.assertEqual(wii_checks.ensure_hbc("w", expected=TAG, say=lambda *_: None), "sent")
            send.assert_called_once()


class SendApp(unittest.TestCase):
    def run_send(self, send_effects, version):
        with mock.patch.object(hbc, "send", side_effect=send_effects) as send, \
             mock.patch.object(hbc, "version", **version), \
             mock.patch.object(wii_checks.time, "sleep"):
            wii_checks.send_app("w", "x.dol", [], say=lambda *_: None)
            return send.call_count

    def test_timeout_then_silence_means_loaded(self):
        self.assertEqual(self.run_send([TimeoutError()], {"side_effect": OSError("down")}), 1)

    def test_timeout_then_agent_means_loaded(self):
        self.assertEqual(self.run_send([TimeoutError()], {"return_value": "1.2.3 agent"}), 1)

    def test_timeout_with_hbc_still_up_sends_again(self):
        self.assertEqual(self.run_send([TimeoutError(), None], {"return_value": TAG}), 2)

    def test_gives_up_after_the_retries(self):
        with self.assertRaises(TimeoutError):
            self.run_send([TimeoutError()] * 3, {"return_value": TAG})


class Production(unittest.TestCase):
    def test_refused_by_name_and_address(self):
        for env, wii in (({"WII_BENCH_WII": "lrwii"}, "10.0.0.1"), ({}, "192.168.8.243"),
                         ({"WII_BENCH_WII": "prod"}, "x")):
            with mock.patch.dict(os.environ, env, clear=False):
                if "WII_BENCH_WII" not in env:
                    os.environ.pop("WII_BENCH_WII", None)
                with self.assertRaises(SystemExit):
                    wii_checks.refuse_production(wii)

    def test_bench_wii_passes(self):
        with mock.patch.dict(os.environ, {"WII_BENCH_WII": "devwii"}):
            wii_checks.refuse_production("192.168.8.213")


if __name__ == "__main__":
    unittest.main()
