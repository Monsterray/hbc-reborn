#!/usr/bin/env python3
"""Every meta.xml option, through HBC's menu in Dolphin.

Boots this tree's retail WAD in Dolphin (tests/dolphin_smoke.py's throwaway
profile, with an SD card and Wii Remote 1 mapped to the pad), puts three
copies of tests/agent_app (mode "meta": prints its argv, IOS and AHBPROT,
and exits) under sd:/apps
with meta.xml files that between them use every option HBC reads, then
drives the menu with a scripted pad (tools/msgboard/dsu_pad.py) as a person
would and checks what each app got:

  A  name, author, version, release_date (date and time), short and long
     description, arguments, ahb_access, sort_id, icon.png
     opened by pointing at it; launched by pointing at Load
  B  coder (the old name for author), release_date (date only), arguments,
     no_ios_reload, sort_id
     with the pointer off the screen, as the D-pad alone: down to it, A to
     open it, A again on the default button, which must be Load
  C  name and arguments only, the sort_id the same as B's (ties go by name)
     launched by pointing, as A

Checks: the Name sort lists A B C first; each app printed its path and its
<arg>s (HBC passes the path, then the args); AHBPROT open for A; B kept
HBC's IOS, A and C got the one HBC reloads to; then, switched in Options,
the Date sort lists A (the later date) then B, and the Custom sort lists
B C A (sort_ids -30 -30 -10). Dolphin reports AHBPROT open and IOS58
whatever the options, so ahb_access and no_ios_reload are proven only as
far as HBC taking the path for them; on a Wii, C would show AHBPROT closed.
Pictures of the list and the dialogs (the text the menu shows: name,
author, version, description, icon) go to OUTDIR (default: a temporary
folder). The test apps are removed at the end.

usage: tests/dolphin_meta.py [OUTDIR]
"""

import pathlib
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

PAD_PORT = "26761"
PAD_SETTINGS = ["Dolphin.Input.BackgroundInput=True",
                "Graphics.Hacks.XFBToTextureEnable=False",
                "DualShockUDPClient.Server.Enabled=True",
                f"DualShockUDPClient.Server.Entries=hbcpad:127.0.0.1:{PAD_PORT};"]
# Wii Remote 1 from the DSU pad, as tools/msgboard's profile maps it.
PAD_MAP = """[Wiimote1]
Device = DSUClient/0/hbcpad
Buttons/A = Cross
Buttons/B = Circle
Buttons/1 = Square
Buttons/2 = Triangle
Buttons/- = L1
Buttons/+ = R1
Buttons/Home = PS
D-Pad/Up = `Pad N`
D-Pad/Down = `Pad S`
D-Pad/Left = `Pad W`
D-Pad/Right = `Pad E`
IR/Up = `Right Y+`
IR/Down = `Right Y-`
IR/Left = `Right X-`
IR/Right = `Right X+`
"""
# The pointer's height from the right stick's Y (Dolphin's profile maps it to
# the IR; X does not move it, and every target here is at the screen's
# middle): list rows 1-3, the app dialog's middle button (Load), off screen.
ROW = {1: -0.25, 2: -0.05, 3: 0.15}
LOAD = 0.61
OFF = -1

APPS = {
    "000-hbctest-meta-a": b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>000 Meta test A</name>
  <author>HBC test author</author>
  <version>1.2.3</version>
  <release_date>20261004123456</release_date>
  <short_description>Short description A</short_description>
  <long_description>Long description A, first line.
Second line, &amp; an escaped ampersand.</long_description>
  <arguments>
    <arg>meta</arg>
    <arg>with space</arg>
    <arg>key=value</arg>
  </arguments>
  <ahb_access/>
  <sort_id>-10</sort_id>
</app>
""",
    "000-hbctest-meta-b": b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>000 Meta test B</name>
  <coder>HBC test coder</coder>
  <version>0.1</version>
  <release_date>20261003</release_date>
  <short_description>Short description B</short_description>
  <long_description>Long description B.</long_description>
  <arguments>
    <arg>meta</arg>
    <arg>b</arg>
  </arguments>
  <no_ios_reload/>
  <sort_id>-30</sort_id>
</app>
""",
    "000-hbctest-meta-c": b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>000 Meta test C</name>
  <arguments>
    <arg>meta</arg>
  </arguments>
  <sort_id>-30</sort_id>
</app>
""",
}
EXPECT_ARGS = {
    "000-hbctest-meta-a": ["meta", "with space", "key=value"],
    "000-hbctest-meta-b": ["meta", "b"],
    "000-hbctest-meta-c": ["meta"],
}


def icon_png():
    """A 128x48 icon: a red to blue gradient."""
    rows = bytearray()
    for _ in range(48):
        for x in range(0, 128, 2):
            rows += bytes([81, 90 + x, 81, 240 - x])  # Y U Y V
    return hbc.yuyv_png(128, 48, bytes(rows))


class Pad:
    """One dsu_pad.py server for the whole run (steps on its stdin), so
    Dolphin stays connected to it. send() starts steps; done() waits."""

    def __init__(self):
        self.proc = subprocess.Popen(
            [sys.executable, "-u", str(root / "tools/msgboard/dsu_pad.py"), PAD_PORT, "-"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def send(self, steps):
        self.proc.stdin.write(steps + "\n")
        self.proc.stdin.flush()

    def done(self):
        for line in self.proc.stdout:
            if line.strip() == "ok":
                return

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def shot(wii, out, name):
    try:
        width, height, data = hbc.screen(wii)
        (out / f"{name}.png").write_bytes(hbc.yuyv_png(width, height, data))
        print(f"  picture: {out / (name + '.png')}", flush=True)
    except (OSError, hbc.HBCError) as exc:
        print(f"  picture {name}: {exc}", flush=True)


def kept_log(wii):
    try:
        kept = hbc.lastlog(wii)
    except (OSError, hbc.HBCError):
        return None
    return kept["text"] if kept else ""


def back_in_hbc(wii, before, seconds=60):
    """Wait for the app to run and HBC to answer again with a new kept log."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if not hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT)):
                text = kept_log(wii)
                if text and text != before and "agent_app: IOS" in text:
                    return True
        except (OSError, hbc.HBCError):
            pass
        time.sleep(0.5)
    return False


def app_output(wii):
    text = kept_log(wii) or ""
    argv = []
    ios = ahbprot = None
    for line in text.splitlines():
        if line.startswith("agent_app: argv["):
            argv.append(line.split("'", 1)[1].rsplit("'", 1)[0])
        elif line.startswith("agent_app: IOS "):
            parts = line.split()
            ios, ahbprot = int(parts[2]), parts[5].rstrip(",")
    return argv, ios, ahbprot, text


def launch(wii, pad, out, folder, row, by_pointer):
    """Open the app's dialog from the list and press Load."""
    before = kept_log(wii)
    if by_pointer:
        pad.send(f"stick 0 {ROW[row]}; wait 1.5; press Cross; wait 2.5; "
                  f"stick 0 {LOAD}; wait 2.5; press Cross; wait 1")
        time.sleep(3.5)
        shot(wii, out, f"{folder}-dialog")
        time.sleep(2.2)
        shot(wii, out, f"{folder}-on-load")
    else:
        # As with the D-pad alone: the pointer off the screen (over nothing on
        # the way, which leaves nothing selected), down to the app's row, then
        # A, A: the dialog must open on Load.
        pad.send(f"stick 0 {LOAD}; wait 1; stick 0 {OFF}; wait 1.5" +
                 "; press PadS; wait 0.5" * row + "; wait 1")
        pad.done()
        shot(wii, out, f"{folder}-selected")
        pad.send("press Cross; wait 3; press Cross; wait 1")
        time.sleep(2.5)
        shot(wii, out, f"{folder}-dialog-default")
    pad.done()
    return back_in_hbc(wii, before)


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-meta-"))
    out.mkdir(parents=True, exist_ok=True)
    dolphin = dolphin_smoke.Dolphin(root / "channel/title/channel_retail.wad", sd=True,
                                    extra=PAD_SETTINGS, files={"Config/WiimoteNew.ini": PAD_MAP})
    wii = dolphin.address
    failed = []
    pad = Pad()

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    try:
        print(f"HBC {hbc.hbc_wait(wii, 60)} on {wii}", flush=True)
        hbc_status = hbc.status(wii)
        print(f"HBC's IOS {hbc_status['ios']}, AHBPROT {hbc_status['ahbprot']}", flush=True)
        dol = (root / "tests/agent_app/agent_app.dol").read_bytes()
        icon = icon_png()
        for folder, meta in APPS.items():
            hbc.put_file(wii, f"sd:/apps/{folder}/boot.dol", dol)
            hbc.put_file(wii, f"sd:/apps/{folder}/meta.xml", meta)
        hbc.put_file(wii, "sd:/apps/000-hbctest-meta-a/icon.png", icon)
        time.sleep(3)
        listed = hbc.status(wii)["app_list"]
        print(f"list ({listed['sort']}): {listed['order'][:5]}", flush=True)
        check(listed["order"][:3] == list(APPS), "the three test apps first, A B C")
        # Leave the pointer below the rows while the list is pictured.
        pad.send(f"stick 0 {LOAD}; wait 3")
        time.sleep(2)
        shot(wii, out, "list")
        pad.done()

        for row, (folder, by_pointer) in enumerate(
                (("000-hbctest-meta-a", True), ("000-hbctest-meta-b", False),
                 ("000-hbctest-meta-c", True)), start=1):
            print(f"{folder}: {'pointer on Load' if by_pointer else 'A on the default button'}",
                  flush=True)
            ran = launch(wii, pad, out, folder, row, by_pointer)
            check(ran, f"{folder} ran and HBC came back")
            argv, ios, ahbprot, text = app_output(wii)
            print("  " + "\n  ".join(text.splitlines()[-8:]), flush=True)
            check(argv[1:] == EXPECT_ARGS[folder] and argv[:1] and
                  argv[0].lower().endswith(f"/apps/{folder}/boot.dol"),
                  f"{folder} got its path and arguments ({argv})")
            if folder.endswith("-a"):
                check(ahbprot == "open", f"ahb_access: AHBPROT open ({ahbprot})")
                ios_reloaded = ios
            elif folder.endswith("-b"):
                check(ios == hbc_status["ios"], f"no_ios_reload: HBC's IOS {hbc_status['ios']} ({ios})")
            else:
                check(ios == ios_reloaded, f"no options: the IOS HBC reloads to ({ios})")
                print(f"  AHBPROT without ahb_access: {ahbprot}", flush=True)

        # The sorts, through Options (1) with the D-pad, the pointer away:
        # up from Back to the sort row (Date), right for Custom; A; down to
        # Back, left to Ok, A.
        for sort, keys, expect in (
                ("date", "", ["000-hbctest-meta-a", "000-hbctest-meta-b"]),
                ("custom", "; press PadE; wait 0.5",
                 ["000-hbctest-meta-b", "000-hbctest-meta-c", "000-hbctest-meta-a"])):
            pad.send(f"stick 0 {OFF}; wait 1; press Square; wait 2; press PadN; wait 0.5" + keys +
                     "; press Cross; wait 0.5; press PadS; wait 0.5; press PadW; wait 0.5"
                     "; press Cross; wait 3")
            pad.done()
            listed = hbc.status(wii)["app_list"]
            print(f"list ({listed['sort']}): {listed['order'][:5]}", flush=True)
            check(listed["sort"] == sort and listed["order"][:len(expect)] == expect,
                  f"the {sort} sort: {expect}")
        shot(wii, out, "list-custom")
    except (OSError, hbc.HBCError, KeyError) as exc:
        check(False, f"error: {exc}")
    finally:
        for folder in APPS:
            try:
                hbc.remove_tree(wii, f"sd:/apps/{folder}", out=lambda _: None)
            except (OSError, hbc.HBCError) as exc:
                print(f"cleanup {folder}: {exc}", flush=True)
        pad.close()
        dolphin.stop()
    print(f"pictures in {out}")
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
