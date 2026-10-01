"""Set IPL.CD and IPL.CD2 (initial setup done) in the Dolphin profile's SYSCONF.
An entry is a header byte ((type << 5) | (name length - 1)), the name, then
the value; ByteBool (type 7) has a 1-byte value."""
import pathlib

p = pathlib.Path("C:/Users/Monty Perrotti/Documents/hbc-dolphin-sysmenu/user/Wii/shared2/sys/SYSCONF")
d = bytearray(p.read_bytes())
assert d[:4] == b"SCv0", d[:4]
for name in (b"IPL.CD", b"IPL.CD2"):
    head = bytes([(7 << 5) | (len(name) - 1)]) + name
    i = d.find(head)
    while i >= 0 and d[i + len(head):i + len(head) + 1] not in (b"\0", b"\1"):
        i = d.find(head, i + 1)
    assert i >= 0, name
    v = i + len(head)
    print(name.decode(), "was", d[v])
    d[v] = 1
p.write_bytes(bytes(d))
print("set")
