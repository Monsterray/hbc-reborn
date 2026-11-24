#!/usr/bin/env python3
"""The bench queue moved to its own repository, wii-bench (bench/wiibench.py), on 2026-11-07.

This forwarder runs the workstation's queue so old commands keep working until every project
calls the shim instead:

    python "$(python tools/wii_paths.py wii_bench)" status

It runs the shim `wiibench.py setup` wrote (wii_paths.wii_bench()), else a wii-bench checkout
beside this one. It goes once no project names this path.
"""
import pathlib
import runpy
import sys

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent))
import wii_paths  # noqa: E402  (tools/wii_paths.py)

try:
    target = wii_paths.wii_bench()
except wii_paths.NotFound:
    target = here.parent.parent.parent / "wii-bench" / "bench" / "wiibench.py"
if not target.is_file():
    sys.exit("the bench queue moved to wii-bench: clone it beside hbc-reborn and run "
             "python wii-bench/bench/wiibench.py setup (wii-bench docs/paths.md)")
print(f"note: tools/wii-bench moved to wii-bench; running {target}", file=sys.stderr)
sys.argv[0] = str(target)
runpy.run_path(str(target), run_name="__main__")
