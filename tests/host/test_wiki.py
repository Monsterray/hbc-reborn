"""tools/wiki.py: every published doc exists, every link on every page works
on the wiki (a page of its own, or a GitHub URL), and the sidebar lists every
page. Builds in memory; nothing is written or pushed."""

import pathlib
import re
import sys
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import wiki  # noqa: E402

REPO = "https://github.com/example/hbc-reborn"


class Wiki(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files, cls.problems = wiki.build_pages(REPO, "master")

    def test_sources_exist(self):
        for page, src, _ in wiki.PAGES:
            self.assertTrue((root / src).is_file(), f"{page}: {src} is gone; update tools/wiki.py")

    def test_no_bad_links(self):
        self.assertEqual(self.problems, [])

    def test_links_work_on_the_wiki(self):
        pages = {p for p, _, _ in wiki.PAGES}
        for name, text in self.files.items():
            text = re.sub(r"```.*?```", "", text, flags=re.S)
            for target in re.findall(r"\]\(([^)\s]+)\)", text):
                if target.startswith(("http://", "https://", "mailto:", "#")):
                    continue
                self.assertIn(target.split("#")[0], pages, f"{name}: {target}")

    def test_sidebar_lists_every_page(self):
        side = self.files["_Sidebar.md"]
        for page, _, _ in wiki.PAGES:
            self.assertIn(f"]({page})", side)

    def test_cli(self):
        self.assertEqual(wiki.main(["check", "--repo", REPO]), 0)


if __name__ == "__main__":
    unittest.main()
