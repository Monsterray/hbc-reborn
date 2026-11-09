#!/usr/bin/env python3
"""Build the GitHub wiki from the repository's reader-facing docs.

The repository is the source; the wiki is a copy made by this script, so it
never drifts. Each page is one Markdown file from the repository with its
links rewritten: a link to another published file becomes a wiki link, any
other repository path becomes a GitHub link at --ref, and images point at the
raw file. Every page ends with a note naming its source file.

  python tools/wiki.py build                 # pages into build/wiki/
  python tools/wiki.py build --out DIR       # somewhere else
  python tools/wiki.py list                  # which file makes which page
  python tools/wiki.py check                 # build in memory, report bad links

Publishing is a separate, deliberate step: clone the wiki repository
(<repo>.wiki.git), run `build --out <that clone>`, review `git diff` there,
then commit and push it yourself. This script never pushes. Publish after a
release: the links point at master (--ref), which gets develop's files then.
"""
import argparse
import pathlib
import posixpath
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# (wiki page, source file, sidebar group). Order is the sidebar's order.
PAGES = [
    ("Home", "README.md", "Start"),
    ("Using-the-channel", "docs/using-hbc.md", "Start"),
    ("hbc-py", "docs/hbc-py.md", "Develop"),
    ("Developer-network", "docs/devnet.md", "Develop"),
    ("SDKs", "sdk/README.md", "SDKs"),
    ("SDK-hbc_netlog", "sdk/hbc_netlog/README.md", "SDKs"),
    ("SDK-hbc_agent", "sdk/hbc_agent/README.md", "SDKs"),
    ("SDK-wiispk", "sdk/wiispk/README.md", "SDKs"),
    ("SDK-slotled", "sdk/slotled/README.md", "SDKs"),
    ("SDK-mboard", "sdk/mboard/README.md", "SDKs"),
    ("SDK-mii", "sdk/mii/README.md", "SDKs"),
    ("Message-Board", "docs/mboard/README.md", "Guides"),
    ("Miis", "docs/mii/README.md", "Guides"),
]
PAGE_OF = {src: page for page, src, _ in PAGES}

# [text](target) and ![alt](target); a target has no spaces (none of ours do).
LINK = re.compile(r"(!?)\[([^\]]*)\]\(([^)\s]+)\)")
FENCE = re.compile(r"^(```|~~~)")


def repo_url():
    """https://github.com/<owner>/<repo> from the origin remote."""
    url = subprocess.run(["git", "remote", "get-url", "origin"], cwd=ROOT,
                         capture_output=True, text=True).stdout.strip()
    url = re.sub(r"\.git$", "", url)
    url = re.sub(r"^git@github\.com:", "https://github.com/", url)
    return url


def head_commit():
    return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                          capture_output=True, text=True).stdout.strip()


def page_title(page):
    return page.replace("-", " ") if page != "hbc-py" else "hbc.py"


def rewrite(src, text, repo, ref, problems):
    """Return text with every relative link made to work on the wiki."""
    srcdir = posixpath.dirname(src)

    def fix(m):
        bang, label, target = m.groups()
        if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("#"):
            return m.group(0)
        path, _, anchor = target.partition("#")
        anchor = "#" + anchor if anchor else ""
        dest = posixpath.normpath(posixpath.join(srcdir, path))
        if dest.startswith(".."):
            problems.append(f"{src}: {target} leaves the repository")
            return m.group(0)
        if not (ROOT / dest).exists():
            problems.append(f"{src}: {target} does not exist")
            return m.group(0)
        if bang:
            return f"![{label}]({repo}/raw/{ref}/{dest})"
        if dest in PAGE_OF:
            return f"[{label}]({PAGE_OF[dest]}{anchor})"
        kind = "tree" if (ROOT / dest).is_dir() else "blob"
        return f"[{label}]({repo}/{kind}/{ref}/{dest}{anchor})"

    out, fenced = [], False
    for line in text.split("\n"):
        if FENCE.match(line.lstrip()):
            fenced = not fenced
        out.append(line if fenced else LINK.sub(fix, line))
    return "\n".join(out)


def build_pages(repo, ref):
    """{file name: content} for every page, the sidebar and the footer."""
    problems, files = [], {}
    commit = head_commit()
    for page, src, _ in PAGES:
        text = (ROOT / src).read_bytes().decode("utf-8")
        body = rewrite(src, text, repo, ref, problems).rstrip("\n")
        files[page + ".md"] = (
            f"{body}\n\n---\n\n*This page is made from [`{src}`]({repo}/blob/{ref}/{src})"
            f" (commit {commit}) by `tools/wiki.py`. Edit that file, not the wiki.*\n")
    side, group = [], None
    for page, _, g in PAGES:
        if g != group:
            side += ([""] if group else []) + [f"**{g}**", ""]
            group = g
        side.append(f"- [{page_title(page)}]({page})")
    side += ["", f"[Source on GitHub]({repo})"]
    files["_Sidebar.md"] = "\n".join(side) + "\n"
    return files, problems


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write the wiki pages into a folder")
    b.add_argument("--out", type=pathlib.Path, default=ROOT / "build" / "wiki",
                   help="folder to write into, e.g. a clone of the wiki repository "
                        "(default: build/wiki)")
    for p in (b, sub.add_parser("check", help="build in memory and report bad links")):
        p.add_argument("--ref", default="master",
                       help="branch or tag the GitHub links point at (default: master)")
        p.add_argument("--repo", default=None,
                       help="https://github.com/<owner>/<repo> (default: the origin remote)")
    sub.add_parser("list", help="print each page and the file it is made from")
    args = ap.parse_args(argv)

    if args.cmd == "list":
        for page, src, group in PAGES:
            print(f"{group:8} {page:20} {src}")
        return 0

    files, problems = build_pages(args.repo or repo_url(), args.ref)
    for p in problems:
        print("bad link:", p, file=sys.stderr)
    if args.cmd == "check":
        print(f"{len(files)} pages, {len(problems)} bad links")
        return 1 if problems else 0

    args.out.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        (args.out / name).write_bytes(content.encode("utf-8"))
    print(f"wrote {len(files)} pages to {args.out}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
