#!/usr/bin/env bash
# The bench monitor moved to wii-bench (bench/monitor.sh) on 2026-11-07. This forwarder runs
# it from $WII_BENCH_REPO, else a wii-bench checkout beside hbc-reborn. It goes once no project
# names this path.
here=$(cd "$(dirname "$0")" && pwd)
repo=${WII_BENCH_REPO:-$here/../../../wii-bench}
if [ ! -f "$repo/bench/monitor.sh" ]; then
    echo "the bench monitor moved to wii-bench: clone it beside hbc-reborn, or set WII_BENCH_REPO" >&2
    exit 2
fi
echo "note: tools/wii-bench moved to wii-bench; running $repo/bench/monitor.sh" >&2
exec bash "$repo/bench/monitor.sh" "$@"
