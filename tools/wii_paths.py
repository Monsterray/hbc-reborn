#!/usr/bin/env python3
"""Where the shared Wii tools are on this workstation, without hard-coded paths.

Every project finds hbc-reborn, the bench queue, Dolphin and the capture card's
tool the same way: an environment variable first, then a default that works
on any workstation (a checkout next to this one, the program on PATH, a
standard install). Never write a path of your own PC into a project.

A tool that is not on PATH (Dolphin unpacked somewhere, the capture card's
script) is named once per workstation: in an environment variable, or in
~/.wii-paths.json, e.g. {"DOLPHIN": "D:/emu/Dolphin/Dolphin.exe"}. The
variable wins over the file.

| Variable    | What it names                          | Default                                                      |
| ----------- | -------------------------------------- | ------------------------------------------------------------ |
| HBC_REBORN  | an hbc-reborn checkout                 | ../hbc-reborn beside this project                            |
| HBC_PY      | hbc-reborn's tools/hbc.py              | $HBC_REBORN/tools/hbc.py                                     |
| WII_BENCH   | the bench queue's wiibench.py          | the workstation's shim: $WII_BENCH_HOME/wiibench.py, else    |
|             |                                        | C:/tools/wii-bench (Windows) or ~/.wii-bench, + /wiibench.py |
| DOLPHIN     | the Dolphin executable                 | PATH (Dolphin, dolphin-emu), then the standard install       |
| WIICAP      | the capture card's wiicap.py           | none: capture is skipped when unset                          |
| PYTHON      | the Python for a project's scripts     | the Python running this file                                 |

PYTHON is for shell scripts that need a Python with packages (Pillow, say)
where `python` is not on PATH, as in devkitPro's MSYS2 shell. Any Python can
ask: `PYTHON=$(py tools/wii_paths.py python)` on Windows (`py` is the Python
launcher, on PATH even there), `python3 tools/wii_paths.py python` elsewhere.

Usage from Python:
    import wii_paths
    subprocess.run([sys.executable, str(wii_paths.hbc_py()), "status"])

From a shell or a Makefile:
    python tools/wii_paths.py hbc_py      # prints the path, or exits 1 with the reason
    python tools/wii_paths.py --all       # every name, found or not

This file comes from the wii-bench kit (template/tools/wii_paths.py). Fix it
there and copy it back, so every project stays the same.
"""

import argparse
import os
import pathlib
import platform
import shutil
import sys

# This file sits in <project>/tools/; the project root is one level up.
PROJECT = pathlib.Path(__file__).resolve().parents[1]


class NotFound(Exception):
    """A tool is not on this workstation; the message says how to point at it."""


def _home():
    """The user's home folder, or None.

    devkitPro's MSYS2 login shell clears USERPROFILE, and then Path.home() fails
    on Windows; the shell API still knows the profile folder."""
    try:
        return pathlib.Path.home()
    except RuntimeError:
        pass
    if os.name == "nt":
        import ctypes
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x28, None, 0, buf) == 0:  # CSIDL_PROFILE
            return pathlib.Path(buf.value)
    return None


def user_settings():
    """~/.wii-paths.json: this workstation's answers, for tools not on PATH.

    {"DOLPHIN": "D:/emu/Dolphin/Dolphin.exe", "WIICAP": "D:/capture/wiicap.py"}
    It lives in the user's home, outside every project; an environment variable
    of the same name wins over it."""
    import json
    p = os.environ.get("WII_PATHS_FILE")
    if not p:
        home = _home()
        if home is None:
            return {}
        p = home / ".wii-paths.json"
    p = pathlib.Path(p)
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _env_path(name):
    v = os.environ.get(name, "").strip() or str(user_settings().get(name, "")).strip()
    return pathlib.Path(v).expanduser() if v else None


def hbc_reborn():
    """The hbc-reborn checkout: $HBC_REBORN, else ../hbc-reborn beside this project."""
    p = _env_path("HBC_REBORN") or PROJECT.parent / "hbc-reborn"
    if not (p / "tools" / "hbc.py").is_file():
        raise NotFound(f"no hbc-reborn checkout at {p}: clone it next to this project "
                       "(git clone https://github.com/Monsterray/hbc-reborn.git) or set HBC_REBORN")
    return p


def hbc_py():
    """hbc-reborn's PC client: $HBC_PY, else $HBC_REBORN/tools/hbc.py."""
    p = _env_path("HBC_PY")
    if p:
        if not p.is_file():
            raise NotFound(f"HBC_PY={p} does not exist")
        return p
    return hbc_reborn() / "tools" / "hbc.py"


def bench_home():
    """The bench queue's state folder on this workstation (wiibench.py's own rule)."""
    p = _env_path("WII_BENCH_HOME")
    if p:
        return p
    if platform.system() == "Windows":
        return pathlib.Path("C:/tools/wii-bench")
    home = _home()
    if home is None:
        raise NotFound("no home folder for ~/.wii-bench: set WII_BENCH_HOME")
    return home / ".wii-bench"


def wii_bench():
    """The bench queue: $WII_BENCH, else the shim `wiibench.py setup` wrote in bench_home().

    The shim runs whichever wii-bench checkout the workstation was set up with,
    so projects never depend on where that checkout is."""
    p = _env_path("WII_BENCH") or bench_home() / "wiibench.py"
    if not p.is_file():
        raise NotFound(f"no bench queue at {p}: run `python <wii-bench>/bench/wiibench.py setup` "
                       "on this workstation, or set WII_BENCH to wiibench.py")
    return p


def dolphin():
    """The Dolphin executable: $DOLPHIN, else PATH, else the standard install."""
    p = _env_path("DOLPHIN")
    if p:
        if not p.exists():
            raise NotFound(f"DOLPHIN={p} does not exist")
        return p
    for name in ("Dolphin", "dolphin-emu"):
        found = shutil.which(name)
        if found:
            return pathlib.Path(found)
    standard = {
        "Windows": [pathlib.Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Dolphin" / "Dolphin.exe"],
        "Darwin": [pathlib.Path("/Applications/Dolphin.app/Contents/MacOS/Dolphin")],
        "Linux": [pathlib.Path("/usr/bin/dolphin-emu"), pathlib.Path("/usr/local/bin/dolphin-emu")],
    }.get(platform.system(), [])
    for c in standard:
        if c.exists():
            return c
    raise NotFound("Dolphin not found: put it on PATH or set DOLPHIN to the executable")


def wiicap():
    """The capture card's wiicap.py: $WIICAP. There is no default; callers skip capture."""
    p = _env_path("WIICAP")
    if not p:
        raise NotFound("WIICAP is not set: no capture card on this workstation")
    if not p.is_file():
        raise NotFound(f"WIICAP={p} does not exist")
    return p


def python():
    """The Python for a project's scripts: $PYTHON, else the Python running this file."""
    p = _env_path("PYTHON")
    if p:
        if not p.is_file():
            raise NotFound(f"PYTHON={p} does not exist")
        return p
    return pathlib.Path(sys.executable)


NAMES = {"hbc_reborn": hbc_reborn, "hbc_py": hbc_py, "bench_home": bench_home,
         "wii_bench": wii_bench, "dolphin": dolphin, "wiicap": wiicap, "python": python}


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Print where a shared Wii tool is on this workstation (see the table in this file).",
        epilog="Exit 0 with the path on stdout; exit 1 with the reason on stderr when it is missing.")
    ap.add_argument("name", nargs="?", choices=sorted(NAMES), help="the tool to find")
    ap.add_argument("--all", action="store_true", help="print every name, found or not")
    a = ap.parse_args(argv)
    if a.all:
        for n in sorted(NAMES):
            try:
                print(f"{n:11} {NAMES[n]()}")
            except NotFound as e:
                print(f"{n:11} missing: {e}")
        return 0
    if not a.name:
        ap.error("give a name or --all")
    try:
        print(NAMES[a.name]())
    except NotFound as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
