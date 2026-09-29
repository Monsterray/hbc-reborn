#!/usr/bin/env python3
"""Python 3 checks for PyWii that need no Wii keys.

Run from the repository root with: python -m unittest tests.test_pywii
"""

import hashlib
import os
import pathlib
import py_compile
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PYWII = ROOT / "pywii"
TOOLS = PYWII / "pywii-tools"
TITLE = ROOT / "channel" / "title"

sys.path.insert(0, str(PYWII / "Common"))
sys.path.insert(0, str(PYWII / "Alameda"))

import pywii as wii  # noqa: E402
from pywii import ec  # noqa: E402

_home = None
_saved_env = {}


def setUpModule():
    # Point every home-directory lookup at an empty directory so nothing can
    # read a real ~/.wii key store.
    global _home
    _home = tempfile.TemporaryDirectory()
    for name in ("HOME", "USERPROFILE"):
        _saved_env[name] = os.environ.get(name)
        os.environ[name] = _home.name
    wii.keys.clear()


def tearDownModule():
    for name, value in _saved_env.items():
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value
    _home.cleanup()


def run_tool(name, *args, cwd=None):
    env = dict(os.environ)
    env["HOME"] = env["USERPROFILE"] = _home.name
    env.pop("PYTHONPATH", None)
    return subprocess.run(
        [sys.executable, str(TOOLS / name), *map(str, args)],
        cwd=cwd or _home.name, env=env, capture_output=True, text=True,
        timeout=20)


class ECDSATest(unittest.TestCase):
    def test_curve_arithmetic(self):
        self.assertTrue(ec.ec_G.on_curve())
        self.assertTrue((ec.ec_N * ec.ec_G) == 0)
        self.assertFalse(ec.ec_G == 0)

    def test_sign_and_verify(self):
        k = ec.gen_priv_key()
        self.assertEqual(len(k), 30)
        q = ec.priv_to_pub(k)
        self.assertEqual(len(q), 60)
        self.assertTrue(ec.Point(q).on_curve())

        sha = hashlib.sha1(b"PyWii ECDSA test").digest()
        r, s = ec.generate_ecdsa(k, sha)
        self.assertEqual((len(r), len(s)), (30, 30))
        self.assertTrue(ec.check_ecdsa(q, r, s, sha))

        tampered = bytes([sha[0] ^ 1]) + sha[1:]
        self.assertFalse(ec.check_ecdsa(q, r, s, tampered))
        bad_s = s[:-1] + bytes([s[-1] ^ 1])
        self.assertFalse(ec.check_ecdsa(q, r, bad_s, sha))

    def test_ec_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "in.bin").write_bytes(b"signed payload\n")
            res = run_tool("ecgenpriv.py", tmp / "key.priv")
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            res = run_tool("ecpriv2pub.py", tmp / "key.priv", tmp / "key.pub")
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertEqual(ec.priv_to_pub((tmp / "key.priv").read_bytes()),
                             (tmp / "key.pub").read_bytes())
            res = run_tool("ecsign.py", tmp / "key.priv", tmp / "in.bin", tmp / "out.bin")
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            signed = (tmp / "out.bin").read_bytes()
            self.assertEqual(signed[:4], b"SIG0")
            res = run_tool("ecchecksig.py", tmp / "key.pub", tmp / "out.bin")
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("Signature is VALID", res.stdout)
            (tmp / "bad.bin").write_bytes(signed[:-1] + b"?")
            res = run_tool("ecchecksig.py", tmp / "key.pub", tmp / "bad.bin")
            self.assertEqual(res.returncode, 4, res.stdout + res.stderr)
            self.assertIn("Signature is INVALID", res.stdout)


class ArchiveTest(unittest.TestCase):
    FILES = {
        "banner.bin": b"B" * 45,
        "icon.bin": bytes(range(256)) * 3,
        "sub/deeper/sound.bin": b"\x00\x01\x02",
        "sub/zzz.txt": b"last file\n",
    }

    def test_pack_list_unpack(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            src = tmp / "src"
            for name, data in self.FILES.items():
                path = src / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            arc = tmp / "test.arc"
            res = run_tool("arcpack.py", arc, src)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            data = arc.read_bytes()

            tag, fstoff, fstsize, dataoff = struct.unpack(">IIII16x", data[:0x20])
            self.assertEqual(tag, 0x55AA382D)
            self.assertEqual(dataoff % 0x20, 0)

            # Walk the FST with the Common parser.
            fst = wii.WiiFST(data[fstoff:fstoff + fstsize])
            found = {}

            def walk(d, prefix):
                for e in d.entries:
                    if isinstance(e, wii.WiiFSTDir):
                        walk(e, prefix + e.name + "/")
                    else:
                        found[prefix + e.name] = data[e.off:e.off + e.size]
                        self.assertEqual(e.off % 0x20, 0)
            walk(fst.root, "")
            self.assertEqual(found, self.FILES)

            # Regenerating the FST reproduces the packed bytes.
            self.assertEqual(fst.generate(), data[fstoff:fstoff + fstsize])

            # Alameda's independent U8 reader sees the same files.
            import Alameda
            u8 = Alameda.U8(data.decode("latin-1"))
            self.assertEqual(
                {k: v.encode("latin-1") for k, v in u8.Files.items()},
                {"./" + k: v for k, v in self.FILES.items()})

            res = run_tool("arclist.py", arc)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("sound.bin", res.stdout)


class TitleTemplateTest(unittest.TestCase):
    TITLE_ID = bytes.fromhex("000100014f484243")

    def test_tmd_template(self):
        raw = (TITLE / "tmd.template").read_bytes()
        tmd = wii.WiiTmd(raw)
        self.assertEqual(tmd.title_id, self.TITLE_ID)
        self.assertEqual(tmd.sys_version, 0x000000010000003A)
        self.assertEqual(tmd.issuer[-1], "CP00000004")
        crs = tmd.get_content_records()
        self.assertEqual([cr.cid for cr in crs], [0, 1])
        self.assertEqual([cr.index for cr in crs], [0, 1])
        self.assertEqual(tmd.boot_index, 1)
        # Re-serializing an unchanged TMD is byte-identical.
        tmd.update()
        self.assertEqual(tmd.data, raw[:len(tmd.data)])

    def test_tmd_fakesign(self):
        tmd = wii.WiiTmd((TITLE / "tmd.template").read_bytes())
        tmd.title_version = 0x1234
        tmd.update()
        tmd.null_signature()
        tmd.brute_sha()
        self.assertEqual(tmd.get_hash()[:1], b"\x00")
        self.assertEqual(wii.WiiTmd(tmd.data).title_version, 0x1234)

    def test_ticket_template(self):
        raw = (TITLE / "cetk.template").read_bytes()
        tik = wii.WiiTik(raw)
        self.assertEqual(tik.title_id, self.TITLE_ID)
        self.assertEqual(tik.common_key_index, 0)
        self.assertEqual(tik.title_key_iv, self.TITLE_ID + bytes(8))
        self.assertEqual(len(tik.title_key_enc), 16)
        self.assertIsNone(tik.title_key)  # no common key loaded
        self.assertEqual(tik.issuer[-1], "XS00000003")
        tik.update()
        self.assertEqual(tik.data, raw[:len(tik.data)])


class ToolUsageTest(unittest.TestCase):
    def test_tools_compile_and_show_usage(self):
        tools = sorted(TOOLS.glob("*.py"))
        self.assertTrue(tools)
        with tempfile.TemporaryDirectory() as tmp:
            for tool in tools:
                with self.subTest(tool=tool.name):
                    py_compile.compile(str(tool), cfile=os.path.join(tmp, tool.name + "c"),
                                       doraise=True)
                    res = run_tool(tool.name)
                    out = res.stdout + res.stderr
                    self.assertNotIn("Traceback", out)
                    self.assertIn("usage", out.lower())


if __name__ == "__main__":
    unittest.main()
