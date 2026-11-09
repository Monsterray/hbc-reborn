"""Put tools/ and every test layer on sys.path, so tests and tools import each
other by name (`import hbc`, `import dolphin_smoke`, `import agent_checks`).

Each test starts with

    sys.path.insert(0, str(root / "tests" / "lib"))
    import testenv

where root is the repository. The layers are tests/lib (shared helpers),
tests/host, tests/dolphin and tests/wii; tests/apps holds the DOLs they send.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
TOOLS = ROOT / "tools"
APPS = TESTS / "apps"
LAYERS = ("lib", "host", "dolphin", "wii")

# Ahead of site-packages, as the per-test inserts were; tools/ first.
for _p in reversed([TOOLS] + [TESTS / d for d in LAYERS]):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))
