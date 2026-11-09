"""Stops Wii keys, signing keys, console dumps and tokens from reaching GitHub.

    python tools/check_secrets.py [--range A..B | --staged | --all]

It scans what a push would send: every file added or changed in any commit of
the range, not only the tip, so a key that was added and then deleted is still
caught. Binary files are scanned too; files over 50 MB are skipped with a
warning. It fails on:

  a. a path under keys/ other than keys/.gitkeep, or a file name that matches
     the .gitignore key patterns (*keys.bin, otp.bin, common-key, sd-key,
     sd-iv, md5-blanker, root-key, korean-key, *.pem);
  b. names that look like console dumps: nand.bin, nand*.bin, boot0*.bin,
     boot1*.bin, seeprom.bin, otp*.bin, keys.txt, *.key;
  c. a PEM or OpenSSH private key header;
  d. GitHub tokens (ghp_, gho_, ghu_, ghs_, ghr_, github_pat_) and other
     well-known token shapes (AWS access key ids, Slack, Google API, Stripe
     live, GitLab and npm tokens);
  e. the bytes of any file in the local keys/ folder (gitignored, so it stays
     on this PC; a linked worktree also reads the main checkout's keys/).
     A key file of 8 to 64 bytes is searched for as raw bytes and as hex
     (lower and upper case; plain, space, colon and comma separated, and as a
     C array of 0x.. bytes). A larger key file, up to 16 MB, is searched by
     up to 8 raw 16-byte windows spread over the file: a copy of the whole file
     contains every window, and any one found is reported. Larger files are
     skipped with a note, as are files with fewer than 4 different byte values
     (they would match anything). The tool never prints a key's value, only
     the key file's name.

A reviewed exception is a path in tools/check_secrets_allow.txt, one
"path  # reason" per line (a reason is required; the path may use * and ?).
It exempts that path from every rule, by path and not by content: use it for
public keys and test vectors that only look like secrets.

What is scanned by default is the commits in @{upstream}..HEAD, or in
origin/develop..HEAD for a branch with no upstream."""
import argparse
import fnmatch
import pathlib
import re
import subprocess
import sys

MAX_SCAN = 50 * 1024 * 1024
MAX_KEYFILE = 16 * 1024 * 1024
ALLOW_FILE = pathlib.Path(__file__).resolve().with_name("check_secrets_allow.txt")

# (a) the .gitignore key patterns, (b) console dump names. Matched on the base
# name, lower-cased.
KEY_NAMES = ["*keys.bin", "otp.bin", "common-key", "sd-key", "sd-iv", "md5-blanker",
             "root-key", "korean-key", "*.pem"]
DUMP_NAMES = ["nand.bin", "nand*.bin", "boot0*.bin", "boot1*.bin", "seeprom.bin",
              "otp*.bin", "keys.txt", "*.key"]

CONTENT_RULES = [
    (re.compile(rb"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY(?: BLOCK)?-----"), "private key header (PEM, OpenSSH or PGP)"),
    (re.compile(rb"gh[pousr]_[A-Za-z0-9]{36,}"), "GitHub token (gh?_)"),
    (re.compile(rb"github_pat_[A-Za-z0-9_]{22,}"), "GitHub token (github_pat_)"),
    (re.compile(rb"(?<![A-Z0-9])(?:AKIA|ASIA)[0-9A-Z]{16}(?![A-Z0-9])"), "AWS access key id"),
    (re.compile(rb"xox[baprs]-[0-9A-Za-z-]{10,}"), "Slack token"),
    (re.compile(rb"AIza[0-9A-Za-z_-]{35}"), "Google API key"),
    (re.compile(rb"sk_live_[0-9a-zA-Z]{24,}"), "Stripe live key"),
    (re.compile(rb"glpat-[0-9A-Za-z_-]{20,}"), "GitLab token"),
    (re.compile(rb"npm_[A-Za-z0-9]{36}"), "npm token"),
]


class GitError(Exception):
    pass


def git(*args, input=None):
    r = subprocess.run(["git", *args], input=input, capture_output=True)
    if r.returncode != 0:
        raise GitError(f"git {' '.join(args)}: {r.stderr.decode('utf-8', 'replace').strip()}")
    return r.stdout


def name_reason(path):
    low = path.lower()
    base = low.rsplit("/", 1)[-1]
    if low.startswith("keys/") and low != "keys/.gitkeep":
        return "path under keys/"
    for pat in KEY_NAMES:
        if fnmatch.fnmatchcase(base, pat):
            return f"file name matches a .gitignore key pattern ({pat})"
    for pat in DUMP_NAMES:
        if fnmatch.fnmatchcase(base, pat):
            return f"file name looks like a console dump or key file ({pat})"
    return None


def parse_raw(out):
    """Entries (path, blob sha) from `git diff-tree/diff --raw -z` output."""
    parts = out.split(b"\0")
    res = []
    i = 0
    while i + 1 < len(parts):
        meta = parts[i].split()
        if not meta or not meta[0].startswith(b":"):
            i += 1
            continue
        mode, sha = meta[1], meta[3]
        path = parts[i + 1].decode("utf-8", "surrogateescape")
        if mode != b"160000":
            res.append((path, sha.decode()))
        i += 2
    return res


def entries(mode, rng):
    """[(label, path, sha)] for what is to be scanned."""
    out = []
    if mode == "staged":
        for p, s in parse_raw(git("diff", "--cached", "--raw", "-z", "--no-renames", "--diff-filter=d")):
            out.append(("index", p, s))
    elif mode == "all":
        for rec in git("ls-tree", "-r", "-z", "HEAD").split(b"\0"):
            if not rec:
                continue
            meta, path = rec.split(b"\t", 1)
            m, typ, sha = meta.split()
            if typ == b"blob" and m != b"160000":
                out.append(("HEAD", path.decode("utf-8", "surrogateescape"), sha.decode()))
    else:
        commits = git("rev-list", "--reverse", rng).split()
        for c in commits:
            c = c.decode()
            seen = set()
            raw = git("diff-tree", "-r", "-z", "--root", "-m", "--no-commit-id", "--no-renames",
                      "--diff-filter=d", c)
            for p, s in parse_raw(raw):
                if (p, s) not in seen:
                    seen.add((p, s))
                    out.append((c[:12], p, s))
    return out


def main_worktree_keys():
    try:
        common = pathlib.Path(git("rev-parse", "--path-format=absolute", "--git-common-dir").decode().strip())
    except GitError:
        return None
    return common.parent / "keys" if common.name == ".git" else None


def key_files(extra_dirs, notes):
    dirs = []
    try:
        dirs.append(pathlib.Path(git("rev-parse", "--show-toplevel").decode().strip()) / "keys")
    except GitError:
        pass
    m = main_worktree_keys()
    if m:
        dirs.append(m)
    dirs += [pathlib.Path(d) for d in extra_dirs]
    seen, res = set(), []
    for d in dirs:
        if not d.is_dir():
            continue
        for f in sorted(d.iterdir()):
            if not f.is_file() or f.name == ".gitkeep":
                continue
            try:
                key = f.resolve()
                if key in seen:
                    continue
                seen.add(key)
                size = f.stat().st_size
                if size < 8:
                    notes.append(f"key file {f.name}: under 8 bytes, not searched")
                    continue
                if size > MAX_KEYFILE:
                    notes.append(f"key file {f.name}: over 16 MB, not searched")
                    continue
                data = f.read_bytes()
            except OSError as e:
                notes.append(f"key file {f.name}: unreadable ({e.strerror}), not searched")
                continue
            pats = patterns_for(data)
            if not pats:
                notes.append(f"key file {f.name}: fewer than 4 different byte values, not searched")
                continue
            res.append((f.name, pats))
    return res


def patterns_for(data):
    if len(data) <= 64:
        if len(set(data)) < 4:
            return []
        h = data.hex()
        pats = {data}
        for case in (str.lower, str.upper):
            hx = case(h)
            pairs = [hx[i:i + 2] for i in range(0, len(hx), 2)]
            pats.add(hx.encode())
            pats.add(" ".join(pairs).encode())
            pats.add(":".join(pairs).encode())
            pats.add(",".join("0x" + p for p in pairs).encode())
            pats.add(", ".join("0x" + p for p in pairs).encode())
        return sorted(pats)
    wins = []
    for i in range(8):
        off = (len(data) - 16) * i // 7
        w = data[off:off + 16]
        if len(set(w)) >= 4 and w not in wins:
            wins.append(w)
    return wins


def load_allow(path):
    allow, problems = [], []
    if not path.exists():
        return allow, problems
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        p, sep, reason = line.partition("#")
        if not sep or not reason.strip() or not p.strip():
            problems.append(f"{path.name}:{n}: needs 'path  # reason'")
            continue
        allow.append(p.strip())
    return allow, problems


def blob_reasons(shas, keys, warn):
    """{sha: [reason]} for the content rules, reading blobs from one cat-file."""
    res = {}
    proc = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for sha in shas:
            proc.stdin.write(sha.encode() + b"\n")
            proc.stdin.flush()
            head = proc.stdout.readline().split()
            if len(head) != 3 or head[1] != b"blob":
                raise GitError(f"cat-file {sha}: {b' '.join(head).decode()}")
            size = int(head[2])
            if size > MAX_SCAN:
                left = size
                while left:
                    left -= len(proc.stdout.read(min(left, 1 << 20)))
                proc.stdout.read(1)
                res[sha] = None
                continue
            data = proc.stdout.read(size)
            proc.stdout.read(1)
            found = []
            for rx, why in CONTENT_RULES:
                if rx.search(data):
                    found.append(why)
            for name, pats in keys:
                if any(p in data for p in pats):
                    found.append(f"contains the bytes of local key file '{name}'")
            res[sha] = found
    finally:
        proc.stdin.close()
        proc.stdout.close()
        proc.wait()
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="check_secrets.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  python tools/check_secrets.py                       # what `git push` would send
  python tools/check_secrets.py --range origin/develop..HEAD
  python tools/check_secrets.py --staged              # before a commit
  python tools/check_secrets.py --all                 # every tracked file at HEAD

Enable it for every push with `git config core.hooksPath tools/hooks`
(tools/hooks/pre-push runs this once per pushed ref).

exit codes: 0 nothing found (a one-line summary is printed); 1 findings, each
as "commit path: reason" (commit is "index" for --staged and "HEAD" for
--all); 2 bad usage, no base to compare with, or a git error.""")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--range", metavar="A..B", help="scan the commits of this revision range "
                   "(default @{upstream}..HEAD, else origin/develop..HEAD)")
    g.add_argument("--staged", action="store_true", help="scan the index instead of commits")
    g.add_argument("--all", action="store_true", help="scan every tracked file at HEAD")
    ap.add_argument("--allow", metavar="FILE", default=str(ALLOW_FILE),
                    help="allow list (default tools/check_secrets_allow.txt)")
    ap.add_argument("--keys-dir", metavar="DIR", action="append", default=[],
                    help="another folder of key files to search for (repeatable)")
    args = ap.parse_args(argv)

    mode = "staged" if args.staged else "all" if args.all else "range"
    rng = args.range
    try:
        if mode == "range" and not rng:
            try:
                up = git("rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}").decode().strip()
            except GitError:
                up = "origin/develop"
                try:
                    git("rev-parse", "--verify", "--quiet", up + "^{commit}")
                except GitError:
                    print("check_secrets: no upstream and no origin/develop to compare with; "
                          "pass --range A..B", file=sys.stderr)
                    return 2
            rng = up + "..HEAD"
        notes = []
        allow, problems = load_allow(pathlib.Path(args.allow))
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 2
        keys = key_files(args.keys_dir, notes)
        ents = entries(mode, rng)
        results = blob_reasons(sorted({s for _, _, s in ents}), keys, None)
    except GitError as e:
        print(f"check_secrets: {e}", file=sys.stderr)
        return 2

    findings, skipped = [], set()
    for label, path, sha in ents:
        if any(fnmatch.fnmatchcase(path, pat) for pat in allow):
            continue
        why = []
        nr = name_reason(path)
        if nr:
            why.append(nr)
        content = results.get(sha)
        if content is None:
            if (label, path) not in skipped:
                skipped.add((label, path))
                print(f"warning: {label} {path}: over 50 MB, content not scanned", file=sys.stderr)
        else:
            why += content
        for w in why:
            findings.append(f"{label} {path}: {w}")
    for n in notes:
        print(f"note: {n}", file=sys.stderr)
    if findings:
        print("\n".join(dict.fromkeys(findings)))
        print(f"check_secrets: {len(set(findings))} finding(s); nothing here is a secret to keep: "
              "remove it from the history, or list a reviewed path in tools/check_secrets_allow.txt",
              file=sys.stderr)
        return 1
    what = {"staged": "the index", "all": "HEAD", "range": rng}[mode]
    print(f"check_secrets: ok, {len(ents)} file version(s) in {what} scanned, "
          f"{len(keys)} local key file(s) searched for")
    return 0


if __name__ == "__main__":
    sys.exit(main())
