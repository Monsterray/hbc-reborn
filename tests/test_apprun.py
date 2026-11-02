"""New/updated app badges, last-run file stamps and legacy marker migration."""
import ctypes
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
NEW, UNCHANGED, UPDATED = range(3)


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class AppRunTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        lib = pathlib.Path(cls.build.name) / 'apprun.so'
        # libfat rename does not promise replacement of an existing target.
        wrapper = pathlib.Path(cls.build.name) / 'fat_rename.c'
        wrapper.write_text('''#undef rename
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int fail_commit;
void apprun_test_fail_commit(int fail) { fail_commit = fail; }
int apprun_test_rename(const char *from, const char *to) {
    if (fail_commit && strstr(from, ".part")) { errno = EIO; return -1; }
    if (!access(to, F_OK)) { errno = EEXIST; return -1; }
    return rename(from, to);
}
''')
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-Drename=apprun_test_rename',
                        str(ROOT / 'channel/channelapp/source/apprun.c'),
                        str(wrapper),
                        '-o', str(lib)], check=True)
        cls.api = ctypes.CDLL(str(lib))
        cls.api.app_run_status.argtypes = [ctypes.c_char_p]
        cls.api.app_run_status.restype = ctypes.c_int
        cls.api.app_run_record.argtypes = [ctypes.c_char_p]
        cls.api.app_run_record.restype = ctypes.c_bool
        cls.api.apprun_test_fail_commit.argtypes = [ctypes.c_int]
        cls.api.apprun_test_fail_commit.restype = None

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.app = pathlib.Path(self.tmp.name) / 'app'
        self.app.mkdir()
        self.boot = self.app / 'boot.dol'
        self.boot.write_bytes(b'original app')
        self.marker = self.app / '.hbc-ran'

    def state(self, path=None):
        return self.api.app_run_status(os.fsencode(path or self.boot))

    def record(self, path=None):
        return self.api.app_run_record(os.fsencode(path or self.boot))

    def test_launch_and_folder_identity(self):
        self.assertEqual(self.state(), NEW)
        self.assertTrue(self.record())
        self.assertEqual(self.state(), UNCHANGED)
        self.assertEqual(self.state(self.app / 'boot.elf'), UNCHANGED)
        self.assertEqual(self.state(self.app.parent / 'other/boot.dol'), NEW)
        stamp = self.marker.stat().st_mtime_ns
        self.assertTrue(self.record())
        self.assertEqual(self.marker.stat().st_mtime_ns, stamp)
        self.assertFalse((self.app / '.hbc-ran.part').exists())
        self.marker.unlink()
        self.assertEqual(self.state(), NEW)

    def test_individual_file_updates_and_launch_acknowledgement(self):
        for name in ('boot.dol', 'boot.elf', 'icon.png', 'meta.xml'):
            with self.subTest(name=name):
                path = self.app / name
                path.write_bytes(b'before')
                self.assertTrue(self.record())
                stamp = path.stat()
                # Same-size updates are detected through timestamps too.
                path.write_bytes(b'after!')
                os.utime(path, (stamp.st_atime, stamp.st_mtime + 4))
                self.assertEqual(self.state(), UPDATED)
                self.assertEqual(self.state(), UPDATED)  # scanning does not clear it
                self.assertTrue(self.record())
                self.assertEqual(self.state(), UNCHANGED)
                path.write_bytes(b'longer updated contents')
                self.assertEqual(self.state(), UPDATED)
                self.assertTrue(self.record())

    def test_added_removed_and_renamed_files(self):
        self.assertTrue(self.record())
        for name in ('icon.png', 'meta.xml', 'boot.elf'):
            path = self.app / name
            path.write_bytes(b'new file')
            self.assertEqual(self.state(), UPDATED)
            self.assertTrue(self.record())
            path.unlink()
            self.assertEqual(self.state(), UPDATED)
            self.assertTrue(self.record())
        self.boot.rename(self.app / 'boot.elf')
        self.assertEqual(self.state(self.app / 'boot.elf'), UPDATED)
        self.assertTrue(self.record(self.app / 'boot.elf'))
        self.assertEqual(self.state(self.app / 'boot.elf'), UNCHANGED)

    def test_legacy_baseline_migrates_once(self):
        self.marker.write_bytes(b'HBC ran\n')
        self.assertEqual(self.state(), UNCHANGED)
        self.assertTrue(self.marker.read_bytes().startswith(b'HBC ran 2\n'))
        self.boot.write_bytes(b'updated after migration')
        self.assertEqual(self.state(), UPDATED)
        self.assertEqual(self.state(), UPDATED)
        self.assertTrue(self.record())
        self.assertEqual(self.state(), UNCHANGED)

    def test_interrupted_marker_commit_keeps_history(self):
        self.assertTrue(self.record())
        backup = self.app / '.hbc-ran.bak'
        self.marker.rename(backup)
        self.assertEqual(self.state(), UNCHANGED)
        self.assertTrue(self.record())
        self.assertTrue(self.marker.exists())
        self.assertFalse(backup.exists())

    def test_missing_invalid_and_failed_writes(self):
        for bad in (b'', b'HBC', b'HBC ran\ntrailing junk', b'HBC ran 2\n',
                    b'HBC ran 2\n2 0 0 0\n' * 4):
            self.marker.write_bytes(bad)
            self.assertEqual(self.state(), NEW)
            self.assertTrue(self.record())
            self.assertEqual(self.state(), UNCHANGED)
        saved = self.marker.read_bytes()
        self.boot.write_bytes(b'new update')
        part = self.app / '.hbc-ran.part'
        part.mkdir()  # cannot open the temporary marker for writing
        self.assertFalse(self.record())
        self.assertEqual(self.marker.read_bytes(), saved)
        self.assertEqual(self.state(), UPDATED)
        part.rmdir()
        self.api.apprun_test_fail_commit(1)
        try:
            self.assertFalse(self.record())
        finally:
            self.api.apprun_test_fail_commit(0)
        self.assertEqual(self.marker.read_bytes(), saved)
        self.assertFalse((self.app / '.hbc-ran.bak').exists())
        self.assertEqual(self.state(), UPDATED)
        for invalid in (b'boot.dol', b'x' * 5000 + b'/boot.dol',
                        os.fsencode(self.app / 'missing/boot.dol')):
            self.assertEqual(self.api.app_run_status(invalid), NEW)
            self.assertFalse(self.api.app_run_record(invalid))


if __name__ == '__main__':
    unittest.main()
