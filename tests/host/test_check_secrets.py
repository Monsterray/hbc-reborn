"""tools/check_secrets.py and the pre-push hook: keys, key-looking names, private key headers, tokens and local key bytes are caught in any pushed commit, the allow list and clean pushes pass, and the repository itself is clean."""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
TOOL = root / "tools" / "check_secrets.py"
HOOK = root / "tools" / "hooks" / "pre-push"
ZEROS = "0" * 40

# Written split up so this file does not trip the scan itself.
PEM = "-----" + "BEGIN RSA PRIV" + "ATE KEY-----"
TOKEN = "gh" + "p_" + "a1B2c3D4e5F6g7H8i9J0k1L2m3N4o5P6q7R8"


@unittest.skipUnless(shutil.which("git"), "needs git")
class CheckSecrets(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = pathlib.Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "develop")
        self.git("config", "user.email", "t@example.invalid")
        self.git("config", "user.name", "t")
        self.git("config", "commit.gpgsign", "false")
        self.write("README.md", b"hello\n")
        self.commit("base")
        self.base = self.git("rev-parse", "HEAD").strip()

    def git(self, *args, **kw):
        r = subprocess.run(["git", *args], cwd=self.repo, capture_output=True, text=True, **kw)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def write(self, rel, data):
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def commit(self, msg):
        self.git("add", "-A", "-f")
        self.git("commit", "-q", "-m", msg)

    def run_tool(self, *args, cwd=None):
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1")
        r = subprocess.run([sys.executable, str(TOOL), *args], cwd=cwd or self.repo,
                           capture_output=True, text=True, env=env)
        return r.returncode, r.stdout, r.stderr

    def check(self, *extra):
        return self.run_tool("--range", f"{self.base}..HEAD", *extra)

    def test_clean_push_passes(self):
        self.write("src/a.c", b"int main(void) { return 0; }\n")
        self.write("keys/.gitkeep", b"")
        self.commit("code")
        code, out, _ = self.check()
        self.assertEqual(code, 0, out)
        self.assertIn("ok", out)

    def test_key_under_keys_folder(self):
        key = os.urandom(16)
        self.write("keys/sd-key", key)
        self.commit("oops")
        code, out, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("keys/sd-key: path under keys/", out)
        self.assertNotIn(key.hex(), out + err)

    def test_key_name_patterns(self):
        for name in ("a/common-key", "x/foo-keys.bin", "x/server.pem"):
            self.write(name, b"harmless text\n")
        self.commit("names")
        code, out, _ = self.check()
        self.assertEqual(code, 1)
        for name in ("a/common-key", "x/foo-keys.bin", "x/server.pem"):
            self.assertIn(f"{name}: file name matches", out)

    def test_dump_names(self):
        names = ["nand.bin", "nand_dump.bin", "boot0.bin", "boot1_x.bin", "seeprom.bin",
                 "otp_dump.bin", "keys.txt", "dev.key"]
        for n in names:
            self.write(n, b"harmless text\n")
        self.commit("dumps")
        code, out, _ = self.check()
        self.assertEqual(code, 1)
        for n in names:
            self.assertIn(f"{n}: file name looks like", out)

    def test_key_added_then_deleted_is_still_caught(self):
        self.write("keys/common-key", os.urandom(16))
        self.commit("add")
        (self.repo / "keys" / "common-key").unlink()
        self.commit("delete")
        code, out, _ = self.check()
        self.assertEqual(code, 1)
        self.assertIn("keys/common-key", out)
        # the tip alone looks clean, which is why every commit is read
        self.assertEqual(self.run_tool("--range", "HEAD~1..HEAD")[0], 0)

    def test_private_key_header_and_tokens(self):
        self.write("doc.txt", (PEM + "\nabc\n").encode())
        self.write("env.sh", f"TOKEN={TOKEN}\n".encode())
        self.write("blob.dat", b"\0\1\2" + TOKEN.encode() + b"\0")
        self.commit("secrets")
        code, out, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("doc.txt: private key header", out)
        self.assertIn("env.sh: GitHub token", out)
        self.assertIn("blob.dat: GitHub token", out)
        self.assertNotIn(TOKEN, out + err)

    def test_local_key_bytes_in_source(self):
        key = os.urandom(16)
        # the key stays out of git, as keys/ does on a real checkout
        self.write(".gitignore", b"/keys/*\n")
        self.write("keys/my-secret", key)
        self.write("src/k.c", ("static const char k[] = \"%s\";\n" % key.hex()).encode())
        self.write("src/k2.c", ("unsigned char k[] = {%s};\n" % ", ".join("0x%02X" % b for b in key)).encode())
        self.write("src/raw.bin", b"xx" + key + b"yy")
        self.write("src/k3.c", ("const char *k = \"%s\";\n" % key.hex().upper()).encode())
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "leak")
        code, out, err = self.check()
        self.assertEqual(code, 1)
        for f in ("src/k.c", "src/k2.c", "src/raw.bin", "src/k3.c"):
            self.assertIn(f"{f}: contains the bytes of local key file 'my-secret'", out)
        self.assertNotIn("keys/my-secret:", out)  # untracked, so never pushed
        shown = out + err
        self.assertNotIn(key.hex(), shown.lower())

    def test_large_local_key_file_by_windows(self):
        key = os.urandom(256)
        self.write(".gitignore", b"/keys/*\n")
        self.write("keys/big", key)
        self.write("copy.dat", b"head" + key + b"tail")
        self.write("other.dat", os.urandom(300))
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "leak")
        code, out, _ = self.check()
        self.assertEqual(code, 1)
        self.assertIn("copy.dat: contains the bytes of local key file 'big'", out)
        self.assertNotIn("other.dat", out)

    def test_allow_list(self):
        self.write("test/vector.pem", b"public stuff\n")
        self.write("test/vec.txt", (PEM + "\n").encode())
        self.commit("vectors")
        self.assertEqual(self.check()[0], 1)
        allow = pathlib.Path(self.tmp.name) / "allow.txt"
        allow.write_text("# header\ntest/vector.pem  # public test vector\n", encoding="utf-8")
        code, out, _ = self.check("--allow", str(allow))
        self.assertEqual(code, 1)
        self.assertNotIn("vector.pem", out)
        self.assertIn("test/vec.txt", out)
        allow.write_text("test/vector.pem  # public\ntest/vec.txt  # fake\n", encoding="utf-8")
        self.assertEqual(self.check("--allow", str(allow))[0], 0)
        allow.write_text("test/vec.txt\n", encoding="utf-8")  # no reason
        self.assertEqual(self.check("--allow", str(allow))[0], 2)

    def test_staged_and_all(self):
        self.write("keys/root-key", os.urandom(16))
        self.git("add", "-f", "keys/root-key")
        code, out, _ = self.run_tool("--staged")
        self.assertEqual(code, 1)
        self.assertIn("index keys/root-key", out)
        self.git("commit", "-q", "-m", "k")
        self.assertEqual(self.run_tool("--all")[0], 1)
        self.assertEqual(self.run_tool("--range", f"{self.base}..HEAD")[0], 1)

    def test_default_range_without_upstream_needs_origin_develop(self):
        code, _, err = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("--range", err)

    def test_help_describes_the_rules(self):
        r = subprocess.run([sys.executable, str(TOOL), "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        for word in ("keys/", "--staged", "--all", "50 MB", "check_secrets_allow.txt", "exit codes"):
            self.assertIn(word, r.stdout)


@unittest.skipUnless(shutil.which("git") and shutil.which("sh"), "needs git and sh")
class PrePushHook(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = pathlib.Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "develop")
        self.git("config", "user.email", "t@example.invalid")
        self.git("config", "user.name", "t")
        self.git("config", "commit.gpgsign", "false")
        (self.repo / "a.txt").write_text("a\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.base = self.git("rev-parse", "HEAD").strip()
        # a remote-tracking ref, as after a clone
        self.git("update-ref", "refs/remotes/origin/develop", self.base)

    def git(self, *args):
        r = subprocess.run(["git", *args], cwd=self.repo, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def commit_file(self, name, data):
        (self.repo / name).parent.mkdir(parents=True, exist_ok=True)
        (self.repo / name).write_bytes(data)
        self.git("add", "-f", "-A")
        self.git("commit", "-q", "-m", name)
        return self.git("rev-parse", "HEAD").strip()

    def hook(self, lines):
        env = dict(os.environ, PYTHON=sys.executable)
        return subprocess.run(["sh", str(HOOK), "origin", "https://example.invalid/x.git"],
                              cwd=self.repo, input=lines, capture_output=True, text=True, env=env)

    def test_existing_branch_clean_and_dirty(self):
        clean = self.commit_file("src/a.c", b"int x;\n")
        r = self.hook(f"refs/heads/develop {clean} refs/heads/develop {self.base}\n")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        bad = self.commit_file("keys/sd-key", os.urandom(16))
        r = self.hook(f"refs/heads/develop {bad} refs/heads/develop {self.base}\n")
        self.assertEqual(r.returncode, 1)
        self.assertIn("keys/sd-key", r.stdout)

    def test_new_branch_checks_against_origin_develop(self):
        bad = self.commit_file("x.pem", b"harmless\n")
        r = self.hook(f"refs/heads/topic {bad} refs/heads/topic {ZEROS}\n")
        self.assertEqual(r.returncode, 1)
        self.assertIn("x.pem", r.stdout)
        self.assertIn("origin/develop", r.stderr)

    def test_new_branch_falls_back_to_origin_master(self):
        self.git("update-ref", "-d", "refs/remotes/origin/develop")
        self.git("update-ref", "refs/remotes/origin/master", self.base)
        bad = self.commit_file("nand.bin", b"harmless\n")
        r = self.hook(f"refs/heads/topic {bad} refs/heads/topic {ZEROS}\n")
        self.assertEqual(r.returncode, 1)
        self.assertIn("origin/master", r.stderr)

    def test_deletion_is_skipped(self):
        self.commit_file("keys/sd-key", os.urandom(16))
        r = self.hook(f"(delete) {ZEROS} refs/heads/old {self.base}\n")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("checking", r.stderr)

    def test_empty_stdin_and_second_ref_still_checked(self):
        self.assertEqual(self.hook("").returncode, 0)
        clean = self.commit_file("ok.c", b"int y;\n")
        bad = self.commit_file("common-key", os.urandom(16))
        r = self.hook(f"refs/heads/a {clean} refs/heads/a {self.base}\n"
                      f"refs/heads/b {bad} refs/heads/b {ZEROS}\n")
        self.assertEqual(r.returncode, 1)
        self.assertIn("common-key", r.stdout)


@unittest.skipUnless(shutil.which("git") and (root / ".git").exists(), "needs a git checkout")
class RepositoryIsClean(unittest.TestCase):
    def test_tree_is_clean(self):
        r = subprocess.run([sys.executable, str(TOOL), "--all"], cwd=root, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
