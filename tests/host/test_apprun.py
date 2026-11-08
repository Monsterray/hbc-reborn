"""New/updated app badges: the per-device apps_seen.txt (apprun.c) on the host."""
import ctypes
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
NEW, UNCHANGED, UPDATED = range(3)


def px(path):
    """A path as the Wii writes it: forward slashes."""
    return str(path).replace("\\", "/").encode()

LINE = re.compile(r"^[^\t]+\tboot\.dol=(-|\d+@\d+) boot\.elf=(-|\d+@\d+) "
                  r"meta\.xml=(-|\d+@\d+) icon\.png=(-|\d+@\d+)$")

# The host has no libfat and no MSVC-style <limits.h>: stand in for both.
COMPAT = """#include <limits.h>
#include <sys/stat.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifdef _MSC_VER
#include <direct.h>
#define mkdir(p) _mkdir(p)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
"""

# libfat's rename does not promise to replace an existing target, and the
# tests count commits and can make one fail.
WRAPPER = """#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "apprun.h"
#undef rename
#undef fopen
int rename(const char *from, const char *to);
FILE *fopen(const char *path, const char *mode);
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
static int fail_commit, commits, hide_seen;
/* libfat: a sector read error during the path lookup reads as ENOENT. */
FILE *apprun_test_fopen(const char *path, const char *mode) {
    if (hide_seen && mode[0] == 'r' && strstr(path, "apps_seen.txt")) {
        errno = ENOENT;
        return NULL;
    }
    return fopen(path, mode);
}
EXPORT void t_hide_seen(int hide) { hide_seen = hide; }
int apprun_test_rename(const char *from, const char *to) {
    FILE *f = fopen(to, "rb");
    if (f) { fclose(f); errno = EEXIST; return -1; }
    if (strstr(from, ".part")) {
        if (fail_commit) { errno = EIO; return -1; }
        commits++;
    }
    return rename(from, to);
}
EXPORT void t_fail_commit(int fail) { fail_commit = fail; }
EXPORT int t_commits(void) { return commits; }
EXPORT void t_begin(const char *root) { app_run_begin(root); }
EXPORT int t_status(const char *name) { return app_run_status(name); }
EXPORT void t_end(int complete) { app_run_end(complete); }
EXPORT int t_status_one(const char *root, const char *name) { return app_run_status_one(root, name); }
EXPORT int t_record(const char *path) { return app_run_record(path); }
"""


@unittest.skipUnless(CC, "no host C compiler")
class AppRunTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)  # Windows keeps the DLL open
        b = pathlib.Path(cls.build.name)
        (b / "compat.h").write_bytes(COMPAT.encode())
        (b / "wrapper.c").write_bytes(WRAPPER.encode())
        lib = b / ("apprun.dll" if os.name == "nt" else "apprun.so")
        src = ROOT / "channel/channelapp/source"
        cmd = [CC, "-shared", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
               "-D_CRT_NONSTDC_NO_WARNINGS", "-include", str(b / "compat.h"),
               "-Drename=apprun_test_rename", "-Dfopen=apprun_test_fopen", "-I", str(src),
               str(src / "apprun.c"), str(b / "wrapper.c"), "-o", str(lib)]
        if os.name != "nt":
            cmd.insert(2, "-fPIC")
        subprocess.run(cmd, check=True)
        api = cls.api = ctypes.CDLL(str(lib))
        api.t_fail_commit.argtypes = [ctypes.c_int]
        api.t_hide_seen.argtypes = [ctypes.c_int]
        api.t_commits.restype = ctypes.c_int
        api.t_begin.argtypes = [ctypes.c_char_p]
        api.t_status.argtypes = [ctypes.c_char_p]
        api.t_end.argtypes = [ctypes.c_int]
        api.t_status_one.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        api.t_record.argtypes = [ctypes.c_char_p]

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dev = pathlib.Path(self.tmp.name) / "sd"
        self.make_app(self.dev, "Foo")
        self.make_app(self.dev, "Bar")
        self.seen = self.dev / "hbc" / "apps_seen.txt"

    def tearDown(self):
        self.api.t_fail_commit(0)
        self.api.t_hide_seen(0)

    @staticmethod
    def make_app(dev, name, boot="boot.dol"):
        app = dev / "apps" / name
        app.mkdir(parents=True, exist_ok=True)
        (app / boot).write_bytes(b"original " + name.encode())
        return app

    def boot(self, name, dev=None, boot="boot.dol"):
        return (dev or self.dev) / "apps" / name / boot

    def record(self, name, dev=None, boot="boot.dol"):
        return bool(self.api.t_record(px(self.boot(name, dev, boot))))

    def scan(self, names, dev=None, complete=True):
        """One device scan; returns {name: state}."""
        self.api.t_begin(px(dev or self.dev))
        states = {n: self.api.t_status(n.encode()) for n in names}
        self.api.t_end(1 if complete else 0)
        return states

    def state(self, name):
        return self.scan([name])[name]

    def lines(self):
        return [l for l in self.seen.read_bytes().decode().splitlines() if not l.startswith("#")]

    def test_new_launch_clears_and_format(self):
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": NEW, "Bar": NEW})
        self.assertFalse(self.seen.exists())  # looking writes nothing
        self.assertTrue(self.record("Foo"))
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": UNCHANGED, "Bar": NEW})
        lines = self.lines()
        self.assertEqual(len(lines), 1)
        self.assertRegex(lines[0], LINE)
        self.assertTrue(lines[0].startswith("Foo\tboot.dol="))
        size = (self.boot("Foo")).stat().st_size
        self.assertIn(f"boot.dol={size}@", lines[0])
        self.assertIn("boot.elf=- meta.xml=- icon.png=-", lines[0])
        self.assertFalse(list((self.dev / "hbc").glob("*.part")))
        self.assertFalse(list((self.dev / "hbc").glob("*.bak")))

    def test_record_without_a_change_writes_nothing(self):
        self.assertTrue(self.record("Foo"))
        commits, before = self.api.t_commits(), self.seen.read_bytes()
        self.assertTrue(self.record("Foo"))
        self.assertEqual(self.api.t_commits(), commits)
        self.assertEqual(self.seen.read_bytes(), before)
        self.scan(["Foo", "Bar"])
        self.assertEqual(self.api.t_commits(), commits)

    def test_each_tracked_file_makes_an_update_and_launch_clears_it(self):
        for name in ("boot.dol", "boot.elf", "icon.png", "meta.xml"):
            with self.subTest(name=name):
                path = self.boot("Foo").parent / name
                path.write_bytes(b"before")
                self.assertTrue(self.record("Foo"))
                stamp = path.stat()
                path.write_bytes(b"after!")  # same size: the date tells
                os.utime(path, (stamp.st_atime, stamp.st_mtime + 4))
                self.assertEqual(self.state("Foo"), UPDATED)
                self.assertEqual(self.state("Foo"), UPDATED)  # scanning does not clear it
                self.assertTrue(self.record("Foo"))
                self.assertEqual(self.state("Foo"), UNCHANGED)
                path.write_bytes(b"longer updated contents")  # a different size
                self.assertEqual(self.state("Foo"), UPDATED)
                self.assertTrue(self.record("Foo"))

    def test_added_removed_and_renamed_files(self):
        self.assertTrue(self.record("Foo"))
        app = self.boot("Foo").parent
        for name in ("icon.png", "meta.xml", "boot.elf"):
            (app / name).write_bytes(b"new file")
            self.assertEqual(self.state("Foo"), UPDATED)
            self.assertTrue(self.record("Foo"))
            (app / name).unlink()
            self.assertEqual(self.state("Foo"), UPDATED)
            self.assertTrue(self.record("Foo"))
        self.boot("Foo").rename(app / "boot.elf")
        self.assertEqual(self.state("Foo"), UPDATED)
        self.assertTrue(self.record("Foo", boot="boot.elf"))
        self.assertEqual(self.state("Foo"), UNCHANGED)

    def test_pruning_takes_two_whole_scans_in_a_row(self):
        self.assertTrue(self.record("Foo"))
        self.assertTrue(self.record("Bar"))
        self.assertEqual(len(self.lines()), 2)
        commits = self.api.t_commits()
        shutil.rmtree(self.dev / "apps" / "Bar")
        # A scan that did not finish (card pulled, list full) must not prune.
        self.assertEqual(self.scan(["Foo"], complete=False), {"Foo": UNCHANGED})
        self.assertEqual(len(self.lines()), 2)
        self.assertEqual(self.api.t_commits(), commits)
        # One whole scan only marks the miss (a readdir error ends a scan early
        # and looks whole): the record stays, with the mark, in one write.
        self.assertEqual(self.scan(["Foo"]), {"Foo": UNCHANGED})
        self.assertEqual(self.api.t_commits(), commits + 1)
        lines = self.lines()
        self.assertEqual([l.split("	")[0] for l in lines], ["Foo", "Bar"])
        self.assertTrue(lines[1].endswith(" missed"))
        self.assertFalse(lines[0].endswith(" missed"))
        # Seen again: the mark goes, the history stays.
        self.api.t_begin(px(self.dev))
        self.api.t_status(b"Foo")
        self.api.t_status(b"Bar")      # the scan saw Bar this time
        self.api.t_end(1)
        self.assertEqual(self.api.t_commits(), commits + 2)
        self.assertFalse(any(l.endswith(" missed") for l in self.lines()))
        # Missed twice in a row: gone, in one write.
        self.scan(["Foo"])
        self.assertEqual(self.api.t_commits(), commits + 3)
        self.assertEqual(self.scan(["Foo"]), {"Foo": UNCHANGED})
        self.assertEqual(self.api.t_commits(), commits + 4)
        self.assertEqual([l.split("	")[0] for l in self.lines()], ["Foo"])
        # Nothing left to prune: the next scan writes nothing.
        self.scan(["Foo"])
        self.assertEqual(self.api.t_commits(), commits + 4)
        # A reinstalled Bar is new again.
        self.make_app(self.dev, "Bar")
        self.assertEqual(self.state("Bar"), NEW)

    def test_a_launch_clears_a_miss_mark(self):
        self.assertTrue(self.record("Foo"))
        self.assertTrue(self.record("Bar"))
        shutil.rmtree(self.dev / "apps" / "Bar")
        self.scan(["Foo"])
        self.assertTrue(self.lines()[1].endswith(" missed"))
        self.make_app(self.dev, "Bar")
        self.assertTrue(self.record("Bar"))
        self.assertFalse(any(l.endswith(" missed") for l in self.lines()))

    def test_missing_corrupt_and_partial_files(self):
        self.assertEqual(self.state("Foo"), NEW)  # no hbc folder at all
        self.assertTrue(self.record("Foo"))
        good = self.lines()[0]
        bad_files = [b"", b"\0\0\0\0", b"garbage", b"Foo\n", b"Foo\tboot.dol=1@1",
                     b"Foo\tboot.dol=x@1 boot.elf=- meta.xml=- icon.png=-\n",
                     b"Foo\tboot.dol=1@1 boot.elf=- meta.xml=- icon.png=- extra\n",
                     b"\tboot.dol=1@1 boot.elf=- meta.xml=- icon.png=-\n",
                     b"x" * 5000 + b"\n" + b"y" * 5000]
        for bad in bad_files:
            with self.subTest(bad=bad[:30]):
                self.seen.write_bytes(bad)
                self.assertEqual(self.state("Foo"), NEW)
                self.assertTrue(self.record("Foo"))
                self.assertEqual(self.state("Foo"), UNCHANGED)
        # A damaged line costs only that app; the rest of the file survives.
        self.seen.write_bytes(b"# comment\r\n" + good.encode() + b"\r\nBar\tbroken\n")
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": UNCHANGED, "Bar": NEW})
        # An overlong line does not swallow the line after it.
        self.seen.write_bytes(b"z" * 3000 + b"\n" + good.encode() + b"\n")
        self.assertEqual(self.state("Foo"), UNCHANGED)

    def test_a_file_that_cannot_be_read_is_never_rewritten(self):
        # 1.14 audit: an error other than "no file" read as an empty table,
        # and the next launch wrote the file back with one app in it.
        self.assertTrue(self.record("Foo"))
        self.assertTrue(self.record("Bar"))
        before = self.seen.read_bytes()
        self.seen.rename(self.seen.with_name("keep"))
        self.seen.with_name("apps_seen.txt.bak").mkdir()   # opening it fails, not ENOENT
        self.assertFalse(self.record("Foo"))
        self.scan(["Foo"])                                  # a whole scan prunes nothing
        self.assertFalse(self.seen.exists())
        self.seen.with_name("apps_seen.txt.bak").rmdir()
        self.seen.with_name("keep").rename(self.seen)
        self.assertEqual(self.seen.read_bytes(), before)

    def test_a_file_that_reads_as_missing_but_exists_is_never_rewritten(self):
        # 1.15.4 (audit 7): libfat reports a read error in the path lookup as
        # ENOENT. The empty table must not replace the file that is there.
        self.assertTrue(self.record("Foo"))
        self.assertTrue(self.record("Bar"))
        before = self.seen.read_bytes()
        commits = self.api.t_commits()
        self.api.t_hide_seen(1)
        self.assertFalse(self.record("Foo"))
        self.scan(["Foo"])                                  # a whole scan prunes nothing
        self.assertEqual(self.api.t_commits(), commits)
        self.assertEqual(self.seen.read_bytes(), before)
        self.api.t_hide_seen(0)
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": UNCHANGED, "Bar": UNCHANGED})
        # A file that is really not there still starts a new table.
        self.seen.unlink()
        self.assertTrue(self.record("Foo"))
        self.assertEqual(len(self.lines()), 1)

    def test_names_ignore_case(self):
        # FAT: MyApp and myapp are one folder.
        self.assertTrue(self.record("Foo"))
        self.assertEqual(self.state("foo"), UNCHANGED)
        self.assertEqual(self.scan(["FOO", "Bar"]), {"FOO": UNCHANGED, "Bar": NEW})
        self.assertEqual(len(self.lines()), 1)              # FOO kept Foo's line

    def test_an_unreadable_folder_is_not_updated(self):
        # No boot file to stamp: app_run_record could never clear the badge.
        self.assertTrue(self.record("Foo"))
        self.boot("Foo").unlink()
        (self.boot("Foo").parent / "meta.xml").write_bytes(b"<app/>")
        self.assertEqual(self.state("Foo"), UNCHANGED)

    def test_interrupted_commit_keeps_history(self):
        self.assertTrue(self.record("Foo"))
        backup = self.seen.with_name("apps_seen.txt.bak")
        self.seen.rename(backup)  # power cut between the two renames
        self.assertEqual(self.state("Foo"), UNCHANGED)
        self.assertTrue(self.record("Bar"))
        self.assertTrue(self.seen.exists())
        self.assertFalse(backup.exists())
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": UNCHANGED, "Bar": UNCHANGED})

    def test_failed_writes_keep_the_old_file(self):
        self.assertTrue(self.record("Foo"))
        saved = self.seen.read_bytes()
        self.boot("Foo").write_bytes(b"new update")
        part = self.seen.with_name("apps_seen.txt.part")
        part.mkdir()  # the temporary file cannot be opened
        self.assertFalse(self.record("Foo"))
        self.assertEqual(self.seen.read_bytes(), saved)
        self.assertEqual(self.state("Foo"), UPDATED)
        part.rmdir()
        self.api.t_fail_commit(1)
        self.assertFalse(self.record("Foo"))
        self.api.t_fail_commit(0)
        self.assertEqual(self.seen.read_bytes(), saved)
        self.assertFalse(part.exists())
        self.assertFalse(self.seen.with_name("apps_seen.txt.bak").exists())
        self.assertEqual(self.state("Foo"), UPDATED)
        self.assertTrue(self.record("Foo"))  # and then it works again
        self.assertEqual(self.state("Foo"), UNCHANGED)

    def test_a_device_that_cannot_be_written_forgets(self):
        (self.dev / "hbc").write_bytes(b"in the way")  # no folder can be made
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": NEW, "Bar": NEW})
        self.assertFalse(self.record("Foo"))
        self.assertEqual(self.state("Foo"), NEW)
        self.assertEqual((self.dev / "hbc").read_bytes(), b"in the way")
        bad = pathlib.Path(self.tmp.name) / "gone"  # not even a root
        self.assertFalse(self.record("Foo", dev=bad))

    def test_the_hbc_folder_is_made_when_needed(self):
        self.assertFalse((self.dev / "hbc").exists())
        self.scan(["Foo"])
        self.assertFalse((self.dev / "hbc").exists())
        self.assertTrue(self.record("Foo"))
        self.assertTrue(self.seen.is_file())

    def test_each_device_has_its_own_file(self):
        usb = pathlib.Path(self.tmp.name) / "usb"
        self.make_app(usb, "Foo")
        self.make_app(usb, "Baz")
        self.assertTrue(self.record("Foo"))
        self.assertEqual(self.scan(["Foo", "Baz"], dev=usb), {"Foo": NEW, "Baz": NEW})
        self.assertTrue(self.record("Baz", dev=usb))
        self.assertEqual(self.scan(["Foo", "Baz"], dev=usb), {"Foo": NEW, "Baz": UNCHANGED})
        self.assertEqual(self.scan(["Foo", "Bar"]), {"Foo": UNCHANGED, "Bar": NEW})
        self.assertEqual([l.split("\t")[0] for l in self.lines()], ["Foo"])
        useen = usb / "hbc" / "apps_seen.txt"
        self.assertEqual([l.split("\t")[0] for l in useen.read_text().splitlines()
                          if not l.startswith("#")], ["Baz"])
        # Pruning one device's scan leaves the other's file alone.
        saved = self.seen.read_bytes()
        shutil.rmtree(usb / "apps" / "Baz")
        self.scan(["Foo"], dev=usb)
        self.assertEqual(self.seen.read_bytes(), saved)
        self.assertEqual(self.api.t_status_one(px(self.dev), b"Foo"), UNCHANGED)
        self.assertEqual(self.api.t_status_one(px(usb), b"Foo"), NEW)

    def test_record_refuses_what_is_not_an_app_launch(self):
        for bad in (b"boot.dol", b"/boot.dol", b"x" * 5000 + b"/apps/Foo/boot.dol",
                    px(self.dev / "apps" / "Foo" / "other.dol"),
                    px(self.dev / "elsewhere" / "Foo" / "boot.dol"),
                    px(self.dev / "apps" / "missing" / "boot.dol"),
                    px(self.dev / "apps" / "boot.dol")):
            with self.subTest(path=bad[:40]):
                self.assertFalse(self.api.t_record(bad))
        self.assertFalse(self.seen.exists())

    def test_a_status_with_no_scan_open_is_new(self):
        self.assertEqual(self.api.t_status(b"Foo"), NEW)


if __name__ == "__main__":
    unittest.main()
