"""Run Dolphin on the hbc-dolphin-sysmenu profile: `install WAD SECONDS` boots
(and so installs) a WAD; `menu SECONDS` boots the installed Wii Menu. Only the
Dolphin started here is stopped. Lists the Wii Menu's NAND data folder after."""
import pathlib, subprocess, sys, time

BASE = pathlib.Path("C:/Users/Monty Perrotti/Documents/hbc-dolphin-sysmenu")
USER = BASE / "user"
DOLPHIN = r"C:\tools\Dolphin-x64\Dolphin.exe"
SETTINGS = ["Dolphin.Interface.ConfirmStop=False", "Dolphin.Interface.UsePanicHandlers=False",
            "Dolphin.Analytics.PermissionAsked=True", "Dolphin.Analytics.Enabled=False",
            "Dolphin.Core.CPUThread=True", "Logger.Options.WriteToFile=True",
            "Logger.Options.Verbosity=" + __import__("os").environ.get("VERB", "3"), "Logger.Logs.BOOT=True", "Logger.Logs.IOS=True",
            "Logger.Logs.IOS_ES=True", "Logger.Logs.IOS_FS=True", "Logger.Logs.OSREPORT=True",
            "Dolphin.Input.BackgroundInput=True", "Dolphin.Core.WiiSDCard=True",
            "Dolphin.Core.WiiSDCardAllowWrites=True",
            "DualShockUDPClient.Server.Enabled=True",
            "DualShockUDPClient.Server.Entries=hbcpad:127.0.0.1:26761;"]
# RTC=YYYY-MM-DDTHH:MM runs the Wii's clock from that local time.
_rtc = __import__("os").environ.get("RTC")
if _rtc:
    import calendar
    SETTINGS += ["Dolphin.Core.EnableCustomRTC=True",
                 f"Dolphin.Core.CustomRTCValue={calendar.timegm(time.strptime(_rtc, '%Y-%m-%dT%H:%M'))}"]
# EXTRA="System.Section.Key=Value;..." adds settings for one run.
SETTINGS += [x for x in __import__("os").environ.get("EXTRA", "").split(";") if x]
# A scripted controller (dsu_pad.py) runs beside Dolphin when PAD is set: its steps.
PAD = __import__("os").environ.get("PAD")

mode, seconds = sys.argv[1], int(sys.argv[-1])
args = [DOLPHIN, "-b", "-u", str(USER)]
for s in SETTINGS:
    args += ["-C", s]
args += ["-e", sys.argv[2]] if mode == "install" else ["-n", "0000000100000002"]
USER.mkdir(parents=True, exist_ok=True)
# Dolphin asks before installing over a different version of a title, and in
# a batch run nobody can answer: drop the installed HBC's contents (its data
# folder stays) before a WAD install.
if mode == "install" and sys.argv[2].lower().endswith(".wad"):
    import shutil
    for tid in ("4f484243", "4c554c5a"):   # OHBC, LULZ
        shutil.rmtree(USER / f"Wii/title/00010001/{tid}/content", ignore_errors=True)
proc = subprocess.Popen(args)
pad = subprocess.Popen([sys.executable, str(pathlib.Path(__file__).with_name("dsu_pad.py")), "26761", PAD]) if PAD else None
time.sleep(seconds)
if pad:
    pad.wait(timeout=60)
alive = proc.poll() is None
# A normal close (as the window's X), so Dolphin saves its NAND state; kill only if it hangs.
subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True)
try:
    proc.wait(timeout=20)
    print("closed normally")
except subprocess.TimeoutExpired:
    proc.kill()
    proc.wait()
    print("had to kill it")
print("dolphin", "ran the whole time" if alive else f"exited early ({proc.returncode})")
nand = USER / "Wii"
for d in ("title/00000001/00000002/content", "title/00000001/00000050/content",
          "title/00000001/00000002/data"):
    p = nand / d
    files = sorted(p.iterdir()) if p.exists() else []
    print(f"{d}: " + (", ".join(f"{f.name} ({f.stat().st_size})" for f in files) or "missing"))
log = USER / "Logs/dolphin.log"
if log.exists():
    lines = log.read_text(errors="replace").splitlines()
    print("log tail:")
    for line in lines[-12:]:
        print("  " + line[:200])
