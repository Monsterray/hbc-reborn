"""Compile the transfer label selector used by HBC and the in-app agent."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class TransferNameTest(unittest.TestCase):
    def test_app_paths_and_other_transfers(self):
        source = r'''
#include <assert.h>
#include "devfile_name.h"

int main(void) {
    const char *cases[][2] = {
        {"sd:/apps/ridgerac/boot.dol", "ridgerac/boot.dol"},
        {"usb:/apps/another/boot.dol", "another/boot.dol"},
        {"usb2:/apps/ridgerac/data/course.bin", "ridgerac/data/course.bin"},
        {"carda:/apps/test/boot.elf", "test/boot.elf"},
        {"cardb:/apps/test/save.dat", "test/save.dat"},
        {"sd:/apps/caf\xc3\xa9/boot.dol", "caf\xc3\xa9/boot.dol"},
        {"sd:/save.dat", "save.dat"},
        {"sd:/saves/game/save.dat", "save.dat"},
        {"sd:/other/apps/test/boot.dol", "boot.dol"},
        {"sd:/apps-old/test/boot.dol", "boot.dol"},
        {"boot.dol", "boot.dol"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert(!strcmp(devfile_display_name(cases[i][0]), cases[i][1]));

    // A maximum-length valid path keeps its app prefix and file suffix;
    // the UI can scroll the entire label instead of cutting it to 47 bytes.
    char path[DEVFILE_PATH_MAX];
    memcpy(path, "sd:/apps/", 9);
    memset(path + 9, 'a', sizeof(path) - 9);
    memcpy(path + sizeof(path) - 10, "/boot.dol", 10);
    const char *name = devfile_display_name(path);
    assert(name == path + 9);
    assert(strlen(name) == sizeof(path) - 10);
    assert(!strcmp(name + strlen(name) - 9, "/boot.dol"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp, "transfer_name.c")
            exe = pathlib.Path(tmp, "transfer_name.exe" if os.name == "nt" else "transfer_name")
            src.write_text(source)
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "channel/channelapp/source"),
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
