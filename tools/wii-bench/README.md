# The bench queue moved

The bench queue, its monitor and the lease server moved to their own repository, wii-bench,
on 2026-11-07 (`bench/` there; its README has everything that was here). The home server's
lease server runs from wii-bench since 2026-11-09.

`wiibench.py` and `monitor.sh` here are forwarders, kept until no project names this path.
Call the workstation's shim instead: `python "$(python tools/wii_paths.py wii_bench)" ARGS`.
