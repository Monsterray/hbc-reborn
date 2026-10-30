"""Build sdk/wiiupdate's release.c on the host and check what it picks from a
GitHub /releases reply: the highest-numbered release that is neither a draft
nor a prerelease and has a WAD for the channel's title, with its notes
unescaped, and its version comparison."""

import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


def asset(name):
    return {"name": name, "uploader": {"name": "not-an-asset-OHBC.wad"},
            "browser_download_url": "https://github.com/o/r/releases/download/x/" + name}


def release(tag, assets, body="", draft=False, prerelease=False):
    # "name" and "body" ahead of "assets", as GitHub sends them, so the walk
    # must tell the release's own keys from its assets'.
    return {"tag_name": tag, "name": "HBC " + tag, "draft": draft, "prerelease": prerelease,
            "body": body, "assets": [asset(a) for a in assets]}


REPLY = [
    release("latest", ["hbc-latest-OHBC.wad"]),
    release("v1.14.0", ["hbc-1.14.0-OHBC.wad"], draft=True),
    release("v1.13.0-rc1", ["hbc-1.13.0-rc1-OHBC.wad"], prerelease=True),
    release("v1.12.20", ["hbc-1.12.20-LULZ.wad", "hbc-1.12.20.zip"]),
    release("v1.12.11", ["hbc-1.12.11.zip", "hbc-1.12.11-OHBC.wad"],
            body="Fixes:\r\n- \"quoted\" \\ and café \U0001F600"),
    release("v1.12.9", ["hbc-1.12.9-OHBC.wad"]),
]


@unittest.skipUnless(CC, "no C compiler")
class WiiUpdateReleaseTest(unittest.TestCase):
    def run_parser(self, reply, suffix, *versions):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "wiiupdate_test.exe" if os.name == "nt" else "wiiupdate_test")
            path = pathlib.Path(tmp, "reply.json")
            path.write_text(json.dumps(reply, indent=1))
            subprocess.run([CC, "-O2", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-I", str(root / "tests/wiiupdate_host/include"),
                            "-I", str(root / "sdk/wiiupdate"),
                            str(root / "tests/wiiupdate_host/test.c"),
                            str(root / "sdk/wiiupdate/release.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            out = subprocess.run([str(exe), str(path), suffix, *versions], check=True,
                                 capture_output=True).stdout.decode("utf-8")
        result, tag, url, rest = out.split("\n", 3)
        notes, _, cmps = rest.partition("\ncmp ") if "\ncmp " in rest else (rest.rstrip("\n"), "", "")
        return (int(result.split("=")[1]), tag[4:], url[4:], notes[6:],
                [int(line.split()[-1]) for line in ("cmp " + cmps).splitlines() if cmps])

    def test_newest_release_with_a_wad(self):
        result, tag, url, notes, _ = self.run_parser(REPLY, "-OHBC.wad")
        self.assertEqual(result, 0)
        self.assertEqual(tag, "v1.12.11")
        self.assertEqual(url, "https://github.com/o/r/releases/download/x/hbc-1.12.11-OHBC.wad")
        # CRs dropped, escapes undone, characters outside the BMP become "?".
        self.assertEqual(notes, "Fixes:\n- \"quoted\" \\ and café ?")

    def test_other_title(self):
        result, tag, url, _, _ = self.run_parser(REPLY, "-LULZ.wad")
        self.assertEqual((result, tag), (0, "v1.12.20"))
        self.assertTrue(url.endswith("hbc-1.12.20-LULZ.wad"))

    def test_no_wad(self):
        result, tag, _, _, _ = self.run_parser(REPLY, "-XXXX.wad")
        self.assertEqual((result, tag), (-1, ""))
        result, _, _, _, _ = self.run_parser({"message": "API rate limit exceeded"}, "-OHBC.wad")
        self.assertEqual(result, -1)

    def test_compare_versions(self):
        *_, cmps = self.run_parser([], "-OHBC.wad",
                                   "v1.12.11", "1.12.10", "v1.12.1", "1.12.10", "v1.10", "1.9.2",
                                   "1.12", "1.12.0", "v1.13.0-beta", "1.13.0")
        self.assertEqual(cmps, [1, -1, 1, 0, 0])


if __name__ == "__main__":
    unittest.main()
