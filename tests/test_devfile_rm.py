"""Compile the pattern matcher and protected-path check of HBCF op E."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class DevfileRmTest(unittest.TestCase):
    def test_glob_and_protected_paths(self):
        source = r'''
#include <assert.h>
#include <stdio.h>
#include "devfile_rm.h"

int main(void) {
    const char *match[][2] = {
        {"*.log", "a.log"}, {"*.log", ".log"}, {"*.LOG", "b.log"}, {"*.log", "B.Log"},
        {"a?c", "abc"}, {"*", "anything"}, {"*", ""}, {"a*b*c", "aXXbYYc"},
        {"a*b*c", "abbc"}, {"**x", "x"}, {"log*", "log"}, {"*a*", "banana"},
        {"?", "\xc3"}, {"a*a*a*a*b", "aaaaaaaaaaaab"},
    };
    const char *miss[][2] = {
        {"*.log", "a.logs"}, {"*.log", "a.txt"}, {"a?c", "ac"}, {"a?c", "abbc"},
        {"?", ""}, {"a*b*c", "aXXbYY"}, {"", "x"}, {"x", ""},
        {"a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
    };
    for (unsigned i = 0; i < sizeof(match) / sizeof(match[0]); ++i)
        if (!devfile_glob_match(match[i][0], match[i][1]))
            printf("should match: %s %s\n", match[i][0], match[i][1]), assert(0);
    for (unsigned i = 0; i < sizeof(miss) / sizeof(miss[0]); ++i)
        if (devfile_glob_match(miss[i][0], miss[i][1]))
            printf("should not match: %s %s\n", miss[i][0], miss[i][1]), assert(0);
    assert(devfile_glob_match("", ""));

    assert(devfile_has_wildcard("a*") && devfile_has_wildcard("?x"));
    assert(!devfile_has_wildcard("plain.log"));

    const char *prot[] = {
        "sd:/", "sd:", "usb:/", "carda:/", "sd:/apps", "sd:/apps/", "SD:/Apps//",
        "sd:/./apps", "usb2:/apps/.", "sd:/./", "cardb:/APPS", "sd://apps",
    };
    const char *free_[] = {
        "sd:/apps/x", "sd:/apps/x/boot.dol", "sd:/application", "sd:/app",
        "sd:/apps2", "sd:/logs", "sd:/a/apps", "usb:/x/./apps", "sd:/appsx",
        "sd:/apps.bak", "sd:/.apps",
    };
    for (unsigned i = 0; i < sizeof(prot) / sizeof(prot[0]); ++i)
        if (!devfile_protected(prot[i]))
            printf("should be protected: %s\n", prot[i]), assert(0);
    for (unsigned i = 0; i < sizeof(free_) / sizeof(free_[0]); ++i)
        if (devfile_protected(free_[i]))
            printf("should not be protected: %s\n", free_[i]), assert(0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp, "check.c")
            exe = pathlib.Path(tmp, "check")
            src.write_text(source)
            subprocess.run([CC, "-std=c99", "-Wall", "-Werror", "-I",
                            str(ROOT / "channel/channelapp/source"), "-o", str(exe), str(src)],
                           check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
