"""Shared steps of the real-Wii tests (tests/wii): is this a bench Wii, is this
build's HBC already running, and send an app even when the send times out.

    sys.path.insert(0, str(root / "tests" / "lib"))
    import testenv
    import wii_checks
"""
import os
import pathlib
import shutil
import socket
import sys
import tempfile
import time

import testenv  # noqa: F401  tools/ and every test layer on sys.path
import hbc

ROOT = testenv.ROOT
CHANNEL_DOL = ROOT / "channel/channelapp/channelapp-channel.dol"

# Real data: the queue names the consoles (WII_BENCH_WII); the addresses cover a
# run outside the queue. Only prod is protected: lrwii is a dev-group bench Wii
# (2026-11-10). $WII_PROTECTED adds names or addresses (comma-separated). The
# NAND-writing tests (wii_mboard, wii_slotled, wii_way_out, wii_replug) keep
# their own, stricter lists.
PRODUCTION_NAMES = ("prod",)
PRODUCTION_IPS = ("192.168.8.200",)


def is_production(wii):
    name = os.environ.get("WII_BENCH_WII", "").strip().lower()
    extra = {s.strip().lower() for s in os.environ.get("WII_PROTECTED", "").split(",") if s.strip()}
    addr = str(wii).strip()
    return (name in PRODUCTION_NAMES or addr in PRODUCTION_IPS
            or (name and name in extra) or addr.lower() in extra)


def refuse_production(wii, why="it writes the Wii's NAND or SD card"):
    """Exit before any send when `wii` is a production console."""
    if is_production(wii):
        sys.exit(f"refusing {wii}: a production Wii ({why})")


def expected_build():
    import dolphin_smoke
    return dolphin_smoke.expected


def ensure_hbc(wii, dol=None, expected=None, fresh=False, say=print):
    """Leave this build's HBC running on `wii`. Skips the send (about 36 s on a
    Wii) when HBC already answers with `expected`, the build tag of the last
    build (x.y.z+N.gHASH: one per commit). A `.dirty` tag is never trusted: it
    names no one build. `fresh=True` always sends, for a test that needs HBC
    just started (counters from 0, the first overlay opening, start-up timing).
    Returns "reused" or "sent"."""
    expected = expected or expected_build()
    if not fresh and not expected.endswith(".dirty"):
        try:
            if hbc.version(wii, hbc.POLL_TIMEOUT) == expected:
                say(f"HBC {expected} already running: no send")
                return "reused"
        except (OSError, hbc.HBCError):
            pass
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-ensure-")) / "channelapp-channel.dol"
    try:
        shutil.copy(dol or CHANNEL_DOL, snap)
        send_app(wii, str(snap), [])
        hbc.relaunch_wait(wii, expected)
    finally:
        shutil.rmtree(snap.parent, ignore_errors=True)
    return "sent"


def send_app(wii, path, args, retries=2, say=print):
    """hbc.send, tolerant of the known "send times out but loads": after a
    timeout or a reset (not a missing file) look at who answers. Nothing, or an agent, means the app
    loaded; HBC still answering means the upload never started, so send again
    (up to `retries` more times). The caller still waits for the app."""
    for attempt in range(retries + 1):
        try:
            hbc.send(wii, path, args)
            return
        except (TimeoutError, ConnectionError, socket.timeout) as exc:
            say(f"send {os.path.basename(path)}: {exc!r}; looking at who answers")
            time.sleep(3)
            try:
                text = hbc.version(wii, hbc.POLL_TIMEOUT)
            except (OSError, hbc.HBCError):
                return  # not answering: the app is loading
            if hbc.is_agent(text) or attempt == retries:
                if hbc.is_agent(text):
                    return
                raise
