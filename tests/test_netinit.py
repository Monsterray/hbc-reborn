"""Run the actual network wait against a busy/failed IOS and timed requests."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class NetinitTest(unittest.TestCase):
    def test_deadline_and_cancellation(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc").mkdir()
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\ntypedef uint64_t u64; typedef int32_t s32;\n")
            (tmp / "network.h").write_text("#include <gctypes.h>\ns32 net_get_status(void);\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <gctypes.h>\nextern u64 now;\n"
                "static inline u64 gettime(void) { return now; }\n"
                "#define ticks_to_millisecs(t) (t)\n")
            harness = tmp / "check.c"
            harness.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include "netinit.h"
u64 now;
static volatile bool stop, restart;
static int polls, sleeps, mode;
s32 net_get_status(void) {
    polls++;
    if (mode == 1 && now >= 12100) return 0;
    if (mode == 2) return -EIO;
    return -EBUSY;
}
int usleep(useconds_t us) {
    now += us / 1000;
    sleeps++;
    if (mode == 3 && now >= 150) restart = true;
    if (mode == 4 && now >= 150) stop = true;
    return 0;
}
static void reset(int m) {
    now = 0; stop = restart = false;
    polls = sleeps = 0; mode = m;
}
int main(void) {
    // A perpetually busy IOS cannot keep HBC polling forever.
    reset(0);
    assert(netinit_wait(0, &stop, &restart) == -ETIMEDOUT);
    assert(now == HBC_NET_INIT_TIMEOUT_MS);
    assert(sleeps == 600 && polls == 600);
    // The observed 12.1-second successful connection remains allowed.
    reset(1);
    assert(netinit_wait(0, &stop, &restart) == 0);
    assert(now == 12100);
    reset(2);
    assert(netinit_wait(0, &stop, &restart) == -EIO);
    assert(!sleeps);
    // Restart/exit interrupt an already-busy wait at the next poll.
    reset(3);
    assert(netinit_wait(0, &stop, &restart) == -ECANCELED);
    assert(now == 150 && restart && !stop);
    reset(4);
    assert(netinit_wait(0, &stop, &restart) == -ECANCELED);
    assert(now == 150 && stop && !restart);
    // Pending cancellation wins even before querying IOS.
    reset(0); restart = true;
    assert(netinit_wait(0, &stop, &restart) == -ECANCELED);
    assert(!polls && !sleeps);
    // Retry attempts share the original deadline, rather than extending it.
    reset(0); now = 29950;
    assert(netinit_wait(0, &stop, &restart) == -ETIMEDOUT);
    assert(now == 30000 && sleeps == 1);
    return 0;
}
''')
            exe = tmp / "check"
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netinit.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
