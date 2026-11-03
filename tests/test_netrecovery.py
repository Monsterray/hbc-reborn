"""Exercise cleanup supervision independently of the wedged worker mutex."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class NetrecoveryTest(unittest.TestCase):
    def test_restart_escalation(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc/machine").mkdir(parents=True)
            (tmp / "ogc/machine/processor.h").write_text(
                "#pragma once\n#include <stdint.h>\n"
                "typedef uint32_t u32; typedef uint64_t u64;\n"
                "#define _CPU_ISR_Disable(x) ((x)=0)\n"
                "#define _CPU_ISR_Restore(x) ((void)(x))\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <ogc/machine/processor.h>\nextern u64 now;\n"
                "static inline u64 gettime(void) { return now; }\n"
                "#define ticks_to_millisecs(t) (t)\n")
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "netrecovery.h"
uint64_t now;
int main(void) {
    netrecovery_reset();
    netrecovery_cleanup_begin(); // zero is a valid start time
    now = 100000;
    assert(!netrecovery_take_reload()); // unrelated cleanup does not request recovery
    netrecovery_request();
    assert(netrecovery_take_reload()); // Restart during existing stuck cleanup
    assert(!netrecovery_take_reload()); // consume only once
    netrecovery_reset();
    now = 200000;
    netrecovery_request();
    netrecovery_cleanup_begin();
    now += HBC_NET_CLEANUP_TIMEOUT_MS - 1;
    assert(!netrecovery_take_reload());
    now++;
    assert(netrecovery_take_reload()); // Restart led to stuck cleanup
    assert(!netrecovery_take_reload());
    netrecovery_reset();
    netrecovery_request();
    netrecovery_cleanup_begin();
    netrecovery_cleanup_complete();
    now += 100000;
    assert(!netrecovery_take_reload()); // cleanup finished normally
    netrecovery_cleanup_begin();
    now += HBC_NET_CLEANUP_TIMEOUT_MS;
    netrecovery_ready();
    assert(!netrecovery_take_reload()); // old requests cannot affect later work
    netrecovery_request();
    netrecovery_reset();
    assert(!netrecovery_take_reload()); // new boot has no pending request
    // Automatic fault recovery uses the same supervisor without any user input.
    netrecovery_reset();
    netrecovery_request(); // failure arms supervision before entering cleanup
    netrecovery_cleanup_begin();
    now += HBC_NET_CLEANUP_TIMEOUT_MS;
    assert(netrecovery_take_reload());
    netrecovery_reset();
    // A failed start whose cleanup returns is left to the menu's own retry
    // backoff: no channel reload, however long the network stays down.
    netrecovery_request();
    netrecovery_cleanup_begin();
    netrecovery_cleanup_complete();
    now += 10 * HBC_NET_CLEANUP_TIMEOUT_MS;
    assert(!netrecovery_take_reload());
    // After a cancelled start the cleanup may wait out a slow IOS step: the
    // 5 s timeout must not fire, the start's 30 s one does.
    netrecovery_reset();
    netrecovery_request();
    netrecovery_cleanup_begin_after_start();
    now += HBC_NET_CLEANUP_TIMEOUT_MS;
    assert(!netrecovery_take_reload());
    now += HBC_NET_CLEANUP_AFTER_START_TIMEOUT_MS - HBC_NET_CLEANUP_TIMEOUT_MS - 1;
    assert(!netrecovery_take_reload());
    now++;
    assert(netrecovery_take_reload());
    // and the next ordinary cleanup is back to 5 s
    netrecovery_reset();
    netrecovery_request();
    netrecovery_cleanup_begin();
    now += HBC_NET_CLEANUP_TIMEOUT_MS;
    assert(netrecovery_take_reload());
    return 0;
}
''')
            exe = tmp / "check"
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(source), str(ROOT / "channel/channelapp/source/netrecovery.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
