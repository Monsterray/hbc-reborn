"""Build sdk/wiiupdate's release.c on the host and check what it picks from a
GitHub /releases reply: the highest-numbered release that is neither a draft
nor a prerelease and has a WAD for the channel's title and a detached
signature beside it, with its notes unescaped, and its version comparison.
Also builds verify.c with the vendored mbedTLS and checks that only a good
signature by the right key is taken (needs `cryptography`, like
tools/wadsign.py)."""

import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import sys
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


def asset(name):
    return {"name": name, "uploader": {"name": "not-an-asset-OHBC.wad"},
            "browser_download_url": "https://github.com/o/r/releases/download/x/" + name}


def with_sigs(*names):
    # A WAD's detached signature is the asset of the same name plus ".sig".
    return [n for name in names for n in (name, name + ".sig") if name.endswith(".wad")] + \
           [n for n in names if not n.endswith(".wad")]


def release(tag, assets, body="", draft=False, prerelease=False):
    # "name" and "body" ahead of "assets", as GitHub sends them, so the walk
    # must tell the release's own keys from its assets'.
    return {"tag_name": tag, "name": "HBC " + tag, "draft": draft, "prerelease": prerelease,
            "body": body, "assets": [asset(a) for a in assets]}


REPLY = [
    release("latest", with_sigs("hbc-latest-OHBC.wad")),
    release("v1.14.0", with_sigs("hbc-1.14.0-OHBC.wad"), draft=True),
    release("v1.13.0-rc1", with_sigs("hbc-1.13.0-rc1-OHBC.wad"), prerelease=True),
    # Newest, but its WAD has no signature beside it: not one to install.
    release("v1.13.5", ["hbc-1.13.5-OHBC.wad"]),
    release("v1.12.20", with_sigs("hbc-1.12.20-LULZ.wad", "hbc-1.12.20.zip")),
    release("v1.12.11", with_sigs("hbc-1.12.11.zip", "hbc-1.12.11-OHBC.wad"),
            body="Fixes:\r\n- \"quoted\" \\ and café \U0001F600"),
    release("v1.12.9", with_sigs("hbc-1.12.9-OHBC.wad")),
]


@unittest.skipUnless(CC, "no C compiler")
class WiiUpdateReleaseTest(unittest.TestCase):
    def run_parser(self, reply, suffix, *versions):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "wiiupdate_test.exe" if os.name == "nt" else "wiiupdate_test")
            path = pathlib.Path(tmp, "reply.json")
            path.write_text(json.dumps(reply, indent=1))
            subprocess.run([CC, "-O2", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-I", str(root / "tests/wiiupdate_host/include"),
                            "-I", str(root / "sdk/wiiupdate"),
                            str(root / "tests/wiiupdate_host/test.c"),
                            str(root / "sdk/wiiupdate/release.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            out = subprocess.run([str(exe), str(path), suffix, *versions], check=True,
                                 capture_output=True).stdout.decode("utf-8")
        result, tag, url, sig_url, rest = out.split("\n", 4)
        notes, _, cmps = rest.partition("\ncmp ") if "\ncmp " in rest else (rest.rstrip("\n"), "", "")
        return (int(result.split("=")[1]), tag[4:], url[4:], sig_url[8:], notes[6:],
                [int(line.split()[-1]) for line in ("cmp " + cmps).splitlines() if cmps])

    def test_newest_release_with_a_wad(self):
        result, tag, url, sig_url, notes, _ = self.run_parser(REPLY, "-OHBC.wad")
        self.assertEqual(result, 0)
        self.assertEqual(tag, "v1.12.11")
        self.assertEqual(url, "https://github.com/o/r/releases/download/x/hbc-1.12.11-OHBC.wad")
        self.assertEqual(sig_url, url + ".sig")
        # CRs dropped, escapes undone, characters outside the BMP become "?".
        self.assertEqual(notes, "Fixes:\n- \"quoted\" \\ and café ?")

    def test_other_title(self):
        result, tag, url, _, _, _ = self.run_parser(REPLY, "-LULZ.wad")
        self.assertEqual((result, tag), (0, "v1.12.20"))
        self.assertTrue(url.endswith("hbc-1.12.20-LULZ.wad"))

    def test_no_wad(self):
        result, tag, _, _, _, _ = self.run_parser(REPLY, "-XXXX.wad")
        self.assertEqual((result, tag), (-1, ""))
        result, _, _, _, _, _ = self.run_parser({"message": "API rate limit exceeded"}, "-OHBC.wad")
        self.assertEqual(result, -1)

    def test_compare_versions(self):
        *_, cmps = self.run_parser([], "-OHBC.wad",
                                   "v1.12.11", "1.12.10", "v1.12.1", "1.12.10", "v1.10", "1.9.2",
                                   "1.12", "1.12.0", "v1.13.0-beta", "1.13.0")
        self.assertEqual(cmps, [1, -1, 1, 0, 0])


MBEDTLS = root / "sdk/wiiupdate/mbedtls"
MBEDTLS_FLAGS = ["-DMBEDTLS_CONFIG_FILE=\"hbc_mbedtls_config.h\"", "-I", str(root / "sdk/wiiupdate"),
                 "-I", str(MBEDTLS / "include")]


@unittest.skipUnless(CC, "no C compiler")
class WiiUpdateVerifyTest(unittest.TestCase):
    """verify.c against signatures made by tools/wadsign.py."""

    @classmethod
    def setUpClass(cls):
        try:
            sys.path.insert(0, str(root / "tools"))
            import wadsign
        except ImportError:
            raise unittest.SkipTest("no cryptography module")
        cls.wadsign = wadsign
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = pathlib.Path(cls.tmp.name)
        objs = []
        for src in sorted((MBEDTLS / "library").glob("*.c")):
            obj = tmp / (src.stem + ".o")
            subprocess.run([CC, "-c", "-O1", *MBEDTLS_FLAGS, "-I", str(MBEDTLS / "library"),
                            str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        cls.exe = tmp / "verify_test"
        subprocess.run([CC, "-O1", "-Wall", "-Werror", *MBEDTLS_FLAGS,
                        "-I", str(root / "tests/wiiupdate_host/include"),
                        str(root / "sdk/wiiupdate/verify.c"),
                        str(root / "tests/wiiupdate_host/verify_test.c"), *objs,
                        "-o", str(cls.exe)], check=True)
        cls.key = wadsign.ec.generate_private_key(wadsign.ec.SECP256R1())
        cls.wad = os.urandom(70000)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def verify(self, key, sig, wad):
        tmp = pathlib.Path(self.tmp.name)
        for name, data in (("key", key), ("sig", sig), ("wad", wad)):
            (tmp / name).write_bytes(data)
        out = subprocess.run([str(self.exe), *(str(tmp / n) for n in ("key", "sig", "wad"))],
                             check=True, capture_output=True).stdout.decode()
        return int(out.split("=")[1])

    def test_good(self):
        point = self.wadsign.public_point(self.key)
        sig = self.wadsign.sign(self.key, self.wad)
        self.assertEqual(self.verify(point, sig, self.wad), 0)

    def test_refused(self):
        point = self.wadsign.public_point(self.key)
        sig = self.wadsign.sign(self.key, self.wad)
        tampered = self.wad[:-1] + bytes([self.wad[-1] ^ 1])
        flipped = bytes([sig[0] ^ 0x80]) + sig[1:]
        other = self.wadsign.sign(self.wadsign.ec.generate_private_key(self.wadsign.ec.SECP256R1()),
                                  self.wad)
        for what, key, s, wad in (
                ("a changed WAD", point, sig, tampered),
                ("a changed signature", point, flipped, self.wad),
                ("another signer's signature", point, other, self.wad),
                ("a short signature", point, sig[:63], self.wad),
                ("a long signature", point, sig + b"\0", self.wad),
                ("a zero signature", point, bytes(64), self.wad),
                ("the unset key", b"\x04" + bytes(64), sig, self.wad),
                ("a point off the curve", b"\x04" + bytes(63) + b"\x01", sig, self.wad)):
            with self.subTest(what):
                self.assertEqual(self.verify(key, s, wad), -1)

    def test_signature_is_for_this_use_only(self):
        # The same key signing the bare WAD (no domain prefix) is not a signature of a release.
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
        r, s = decode_dss_signature(self.key.sign(self.wad, ec.ECDSA(hashes.SHA256())))
        sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
        self.assertEqual(self.verify(self.wadsign.public_point(self.key), sig, self.wad), -1)


if __name__ == "__main__":
    unittest.main()
