"""Polling instead of fixed sleeps: wait for a state HBCS shows, with a deadline."""
import time

import testenv  # noqa: F401
import hbc


def wait_until(check, seconds, interval=0.2):
    """Call check() until it returns something true or `seconds` pass. Returns that
    value, or None at the deadline: the caller's next assert then reports the real
    state, so a slow run fails with a clear message, not a lucky sleep. Errors
    from check() (a Wii that is busy) count as "not yet"."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            got = check()
            if got:
                return got
        except (OSError, hbc.HBCError, KeyError, ValueError, AssertionError):
            pass
        if time.monotonic() >= deadline:
            return None
        time.sleep(interval)
