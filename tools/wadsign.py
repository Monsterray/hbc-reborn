#!/usr/bin/env python3
"""Signs the channel's release WAD, so that an installed HBC will take it as
an update (sdk/wiiupdate/verify.c checks the signature this makes).

  wadsign.py keygen KEY.pem        make a P-256 private key (once; keep it safe)
  wadsign.py pubkey KEY.pem        the key as the C initializer for
                                   UPDATE_PUBLIC_KEY in channel/channelapp/config.h
  wadsign.py sign KEY.pem WAD      write WAD.sig, 64 bytes, to attach to the
                                   release beside the WAD
  wadsign.py verify KEY.pem WAD    check WAD.sig against the key

The signature is ECDSA on P-256 over the SHA-256 of "HBC-WAD-SIGNATURE-1", a
0 byte, then the WAD's bytes, r and s as 32 big-endian bytes each.
Needs `pip install cryptography`.
"""

import pathlib
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

DOMAIN = b"HBC-WAD-SIGNATURE-1\0"


def load(path):
    key = serialization.load_pem_private_key(pathlib.Path(path).read_bytes(), None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit("%s is not a P-256 private key" % path)
    return key


def public_point(key):
    return key.public_key().public_bytes(serialization.Encoding.X962,
                                         serialization.PublicFormat.UncompressedPoint)


def sign(key, wad):
    r, s = decode_dss_signature(key.sign(DOMAIN + wad, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify(public, sig, wad):
    if len(sig) != 64:
        return False
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        public.verify(der, DOMAIN + wad, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        return False
    return True


def c_initializer(point):
    rows = ["\t0x%02x, \\" % point[0]]
    for i in range(1, 65, 16):
        rows.append("\t" + ", ".join("0x%02x" % b for b in point[i:i + 16]) + (", \\" if i < 49 else " }"))
    return "#define UPDATE_PUBLIC_KEY { \\\n" + "\n".join(rows)


def main(argv):
    if len(argv) == 3 and argv[1] == "keygen":
        key = ec.generate_private_key(ec.SECP256R1())
        path = pathlib.Path(argv[2])
        if path.exists():
            sys.exit("%s exists" % path)
        path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                           serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
        path.chmod(0o600)
        print(c_initializer(public_point(key)))
    elif len(argv) == 3 and argv[1] == "pubkey":
        print(c_initializer(public_point(load(argv[2]))))
    elif len(argv) == 4 and argv[1] == "sign":
        wad = pathlib.Path(argv[3])
        sig = pathlib.Path(str(wad) + ".sig")
        sig.write_bytes(sign(load(argv[2]), wad.read_bytes()))
        print("wrote", sig)
    elif len(argv) == 4 and argv[1] == "verify":
        wad = pathlib.Path(argv[3])
        ok = verify(load(argv[2]).public_key(), pathlib.Path(str(wad) + ".sig").read_bytes(),
                    wad.read_bytes())
        print("good" if ok else "BAD")
        return 0 if ok else 1
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
