"""Bring a checkout made before .gitattributes pinned line endings to LF.

    python tools/fix_line_endings.py

Git for Windows' core.autocrlf=true checked text out with CRLF, and a few
binary files got CRLF too. This rewrites CRLF to LF in every file Git treats
as text, and restores a binary or -text file only where CRLF-to-LF alone turns
it back into the committed bytes. Other changes, committed or not, are kept.
"""

import argparse
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]


def git(*args, data=None):
    return subprocess.run(["git", *args], cwd=root, input=data, capture_output=True,
                          check=True).stdout


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""example:
  python tools/fix_line_endings.py

exit codes: 0 done (prints the counts; it rewrites files in this checkout);
2 bad usage (argparse's own). A failing git command ends with a Python
traceback (exit 1).""")
    ap.parse_args(argv)
    fixed = restored = 0
    listing = git("ls-files", "--eol", "-z", "--cached", "--others", "--exclude-standard")
    for entry in listing.split(b"\0"):
        if not entry:
            continue
        meta, path = entry.decode().split("\t", 1)
        attr = meta.split(None, 2)[2].strip()
        file = root / path
        if not file.is_file():
            continue
        raw = file.read_bytes()
        if b"\r\n" not in raw:
            continue
        if attr.startswith("attr/text"):
            file.write_bytes(raw.replace(b"\r\n", b"\n"))
            fixed += 1
            continue
        # A binary or -text file: only undo an old autocrlf conversion.
        staged = git("ls-files", "-s", "--", path).split()
        if len(staged) < 2:
            continue
        lf = raw.replace(b"\r\n", b"\n")
        if git("hash-object", "--no-filters", "--stdin", data=lf).strip() == staged[1]:
            file.write_bytes(lf)
            restored += 1
    git("update-index", "-q", "--refresh")
    print(f"{fixed} text files now LF, {restored} binary files restored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
