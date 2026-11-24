"""No paths of one workstation: no tracked text file names a project, tool or home
folder of one PC (C:/projects/..., /c/tools/..., /mnt/c/..., a C:/Python3xx), so
the project works on anyone's workstation. Shared tools are found through
wii_paths.py instead. Saved logs and history keep the paths they were recorded
with: list their folders in EXEMPT.

Allowed: the bench queue's standard folder on Windows (C:/tools/wii-bench, as
devkitPro's is C:/devkitPro), and placeholders such as C:/projects/... or
C:\\Users\\<user name>.

This file comes from the wii-bench kit (template/tests/test_workstation_paths.py).
Fix it there and copy it back; only EXEMPT is the project's own.

Run as a script, it lists every finding: python tests/test_workstation_paths.py
"""

import fnmatch
import pathlib
import re
import shutil
import subprocess
import unittest

# Files kept as a record, with the paths they were made with. fnmatch patterns
# against the path from the repository root; "*" crosses folders.
EXEMPT = (
    "docs/history/*",
    "docs/plans/2026-11-reorg.md",  # the record of the path work, naming what it fixed
)

_FOLDER = r"(?:projects|tools|users|ai)[\\/]+(?!wii-bench\b)[\w-]"
_PATTERN = re.compile(
    r"\b[a-z]:[\\/]+" + _FOLDER                          # C:/projects/x, C:\Users\name
    + r"|/mnt/[a-z]/" + _FOLDER                          # /mnt/c/projects/x (WSL)
    + r"|(?<![\w.~/-])/[a-z]/" + _FOLDER                 # /c/tools/x (MSYS2, Git Bash)
    + r"|(?:\b[a-z]:|(?<![\w.~/-])/[a-z])[\\/]+python3\d+\b",  # C:/Python312
    re.IGNORECASE)

here = pathlib.Path(__file__).resolve().parent
# This test, in any project: its own pattern and examples would match.
SELF = ("test_workstation_paths.py", "workstation_paths_test.py")


def repo_root():
    if not shutil.which("git"):
        return None
    r = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=here,
                       capture_output=True, text=True)
    return pathlib.Path(r.stdout.strip()) if r.returncode == 0 else None


root = repo_root()


def findings():
    """(path, line number, line) for every match outside EXEMPT."""
    out = subprocess.run(["git", "-c", f"safe.directory={root.as_posix()}", "ls-files", "-z"],
                         cwd=root, capture_output=True, check=True).stdout
    for name in filter(None, out.decode("utf-8", "replace").split("\0")):
        if any(fnmatch.fnmatch(name, p) for p in EXEMPT) or name.rsplit("/", 1)[-1] in SELF:
            continue
        try:
            data = (root / name).read_bytes()
        except OSError:  # a file deleted but not yet staged
            continue
        if b"\0" in data[:8192]:
            continue
        for n, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
            if _PATTERN.search(line):
                yield name, n, line.strip()


@unittest.skipUnless(root, "needs a git checkout")
class WorkstationPaths(unittest.TestCase):
    def test_no_workstation_paths(self):
        bad = [f"{p}:{n}: {line[:160]}" for p, n, line in findings()]
        self.assertEqual(bad, [], "paths of one workstation; find the tool through wii_paths.py, "
                                  "or add a folder of saved logs to EXEMPT")

    def test_pattern(self):
        for s in ("C:/projects/hbc-reborn", "c:\\tools\\Dolphin-x64", "/c/tools/Dolphin-x64",
                  "/mnt/c/projects/Wii64", "C:/Python312/python.exe", "/c/Python312/python",
                  "C:\\Users\\Someone\\AppData", "C:/ai/venvs/x"):
            self.assertRegex(s, _PATTERN)
        for s in ("C:/tools/wii-bench/wiibench.py", "/c/tools/wii-bench", "C:/projects/...",
                  "C:\\Users\\<user name>", "mounts `C:\\Users` on", "C:/devkitPro/msys2",
                  "/opt/devkitpro/tools/bin", "~/c/tools/x", "a/c/tools/x", "https://x.org/c/tools/y"):
            self.assertNotRegex(s, _PATTERN)


if __name__ == "__main__":
    if root is None:
        raise SystemExit("needs a git checkout")
    found = list(findings())
    for p, n, line in found:
        print(f"{p}:{n}: {line[:160]}")
    raise SystemExit(1 if found else 0)
