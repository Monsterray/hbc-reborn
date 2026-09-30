"""Line endings: text is LF in the repository and in the checkout, on every
platform (.gitattributes, whatever core.autocrlf says). Only the upstream
files .gitattributes marks -text may hold CRLF, and Git leaves those as they
are."""

import pathlib
import shutil
import subprocess
import unittest

root = pathlib.Path(__file__).resolve().parents[1]


def eol_listing():
    # safe.directory: in CI's devkitPPC container the checkout belongs to
    # another user, and git refuses to read it (exit 128) without this.
    out = subprocess.run(["git", "-c", f"safe.directory={root.as_posix()}", "ls-files", "--eol"],
                         cwd=root, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        meta, path = line.split("\t", 1)
        index, work, attr = meta.split(None, 2)
        yield index, work, attr.strip(), path


@unittest.skipUnless(shutil.which("git") and (root / ".git").exists(), "needs a git checkout")
class LineEndings(unittest.TestCase):
    def test_index_is_lf(self):
        bad = [p for i, _, a, p in eol_listing()
               if i in ("i/crlf", "i/mixed") and a != "attr/-text"]
        self.assertEqual(bad, [], "CRLF committed; mark it -text in .gitattributes "
                                  "only if it is an upstream file kept as it came")

    def test_checkout_is_lf(self):
        bad = [p for _, w, a, p in eol_listing()
               if w in ("w/crlf", "w/mixed") and a.startswith("attr/text")]
        self.assertEqual(bad, [], "text checked out with CRLF; run tools/fix_line_endings.py")

    def test_every_file_has_a_rule(self):
        bad = [p for _, _, a, p in eol_listing() if a == "attr/"]
        self.assertEqual(bad, [], "no .gitattributes rule applies")


if __name__ == "__main__":
    unittest.main()
