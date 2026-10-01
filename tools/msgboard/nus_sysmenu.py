"""Download the USA Wii Menu (and its IOS) the way Dolphin's online system update
does: the title list from Dolphin's NetUpdateSOAP stand-in, the files from the
CDN it names. Contents stay encrypted; each title is packed into a WAD that
Dolphin installs (and decrypts) itself."""
import pathlib, re, struct, sys, urllib.request

OUT = pathlib.Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)
SOAP = """<?xml version="1.0" encoding="UTF-8"?>
<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"
  xmlns:xsd="http://www.w3.org/2001/XMLSchema"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <soapenv:Body>
    <GetSystemUpdateRequest xmlns="urn:nus.wsapi.broadon.com">
      <Version>1.0</Version>
      <MessageId>0</MessageId>
      <DeviceId>4362227774</DeviceId>
      <RegionId>USA</RegionId>
    </GetSystemUpdateRequest>
  </soapenv:Body>
</soapenv:Envelope>
"""


def get(url, data=None, headers=None):
    cache = OUT / "raw" / url.rsplit("/download/", 1)[-1].replace("/", "_") if data is None else None
    if cache and cache.exists():
        return cache.read_bytes()
    req = urllib.request.Request(url, data=data, headers=headers or {"User-Agent": "wii libnup/1.0"})
    with urllib.request.urlopen(req, timeout=180) as r:
        body = r.read()
    if cache:
        cache.parent.mkdir(exist_ok=True)
        cache.write_bytes(body)
    return body


reply = get("https://fakenus.dolphin-emu.org/nus/services/NetUpdateSOAP", SOAP.encode(), {
    "SOAPAction": "urn:nus.wsapi.broadon.com/GetSystemUpdate",
    "User-Agent": "wii libnup/1.0", "Content-Type": "text/xml; charset=utf-8"}).decode()
code = re.search(r"<ErrorCode>(-?\d+)</ErrorCode>", reply)
assert code and code[1] == "0", reply[:500]
prefix = re.search(r"<ContentPrefixURL>([^<]+)</ContentPrefixURL>", reply)[1].replace("https://", "http://")
titles = {int(t, 16): int(v) for t, v in re.findall(
    r"<TitleId>([0-9A-Fa-f]+)</TitleId>\s*<Version>(\d+)</Version>", reply)}
print("content prefix", prefix)
print(len(titles), "titles; System Menu version", titles.get(0x0000000100000002))


def wad(tid, ver):
    tmd_raw = get(f"{prefix}/{tid:016x}/tmd.{ver}" if ver else f"{prefix}/{tid:016x}/tmd")
    n = struct.unpack(">H", tmd_raw[0x1de:0x1e0])[0]
    tmd_len = 0x1e4 + 36 * n
    tmd, tmd_certs = tmd_raw[:tmd_len], tmd_raw[tmd_len:]
    cetk = get(f"{prefix}/{tid:016x}/cetk")
    tik, tik_certs = cetk[:0x2a4], cetk[0x2a4:]
    contents = []
    for i in range(n):
        cid, idx, ctype, size = struct.unpack(">IHHQ", tmd[0x1e4 + 36 * i:0x1e4 + 36 * i + 16])
        data = get(f"{prefix}/{tid:016x}/{cid:08x}")
        contents.append(data[:(size + 15) & ~15])
        print(f"  {tid:016x} content {cid:08x} ({size} bytes)", flush=True)
    # The chain: tmd_certs holds CP + CA, tik_certs XS + CA; a WAD carries CA, CP, XS.
    def split(blob):
        out, off = [], 0
        while off < len(blob):
            sigtype = struct.unpack(">I", blob[off:off + 4])[0]
            sig_len = {0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3c}[sigtype]
            body = off + 4 + sig_len
            body = (body + 0x3f) & ~0x3f
            keytype = struct.unpack(">I", blob[body + 0x40:body + 0x44])[0]
            key_len = {0: 0x238, 1: 0x138, 2: 0x78}[keytype]
            end = body + 0x88 + key_len
            end = (end + 0x3f) & ~0x3f
            name = blob[body + 0x44:body + 0x84].split(b"\0")[0].decode()
            out.append((name, blob[off:end]))
            off = end
        return out
    certs = dict(split(tmd_certs) + split(tik_certs))
    chain = b"".join(certs[k] for k in ("CA00000001", "CP00000004", "XS00000003"))
    pad = lambda b: b + bytes(-len(b) % 0x40)
    data = b"".join(pad(c) for c in contents)
    hdr = struct.pack(">I2sHIIIIII", 0x20, b"Is", 0, len(chain), 0, len(tik), len(tmd), len(data), 0)
    blob = pad(hdr) + pad(chain) + pad(tik) + pad(tmd) + data
    path = OUT / f"{tid:016x}-v{ver}.wad"
    path.write_bytes(blob)
    ios = struct.unpack(">Q", tmd[0x184:0x18c])[0]
    print(f"wrote {path} ({len(blob)} bytes); needs IOS {ios & 0xffffffff}", flush=True)
    return ios


ios = wad(0x0000000100000002, titles[0x0000000100000002])
wad(ios, titles.get(ios, 0))
