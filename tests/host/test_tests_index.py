"""tests/README.md lists every test as its docstring says (tests/lib/make_index.py)."""

import pathlib
import sys
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import make_index  # noqa: E402


class TestsIndex(unittest.TestCase):
    def test_index_is_current(self):
        current = make_index.OUT.read_bytes().decode("utf-8") if make_index.OUT.exists() else ""
        self.assertEqual(current, make_index.render(),
                         "tests/README.md is out of date: run python tests/lib/make_index.py")

    def test_only_layer_folders(self):
        layers = {name for name, _, _ in make_index.LAYERS}
        stray = [p.name for p in (root / "tests").iterdir()
                 if p.name not in layers and p.name not in ("README.md", "__pycache__")]
        self.assertEqual(stray, [], "put each test in host/, dolphin/, wii/, apps/ or lib/")


if __name__ == "__main__":
    unittest.main()
