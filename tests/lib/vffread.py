"""Read a Wii VFF (cdb.vff, wc24recv.mbx): a FAT16 volume in a NAND file,
geometry from its header. Independent of sdk/mboard's writer, for checking
it. Also reads the WiiConnect24 receive box's control file."""

import struct


class Vff:
    def __init__(self, img):
        self.img = img
        if img[0:4] != b"VFF " or img[4:6] != b"\xfe\xff":
            raise ValueError("not a VFF")
        size = struct.unpack_from(">I", img, 8)[0]
        self.cl = struct.unpack_from(">H", img, 12)[0] * 16
        clusters = size // self.cl
        if clusters < 4085:
            raise ValueError("FAT12 VFF")
        self.fat_len = (clusters * 2 + 511) & ~511
        self.fat1 = 0x20
        self.fat2 = self.fat1 + self.fat_len
        self.root = self.fat2 + self.fat_len
        self.data = self.root + 0x1000
        self.nclusters = (size - self.data) // self.cl + 2

    def next(self, c):
        return struct.unpack_from("<H", self.img, self.fat1 + 2 * c)[0]

    def chain(self, c):
        out = []
        while 2 <= c < 0xfff8 and len(out) < self.nclusters:
            out.append(c)
            c = self.next(c)
        return out

    def fats_match(self):
        return self.img[self.fat1:self.fat2] == self.img[self.fat2:self.root]

    def entries(self, cluster=0):
        """A folder's entries: short name, long name, attribute, cluster, size."""
        if cluster == 0:
            areas = [(self.root, 0x1000)]
        else:
            areas = [(self.data + (c - 2) * self.cl, self.cl) for c in self.chain(cluster)]
        lfn = None
        for start, size in areas:
            for off in range(start, start + size, 32):
                e = self.img[off:off + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xe5:
                    lfn = None
                elif e[11] == 0x0f:
                    lfn = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le").split("\0")[0]
                else:
                    yield {"short": bytes(e[0:11]), "long": lfn, "attr": e[11],
                           "cluster": struct.unpack_from("<H", e, 26)[0],
                           "size": struct.unpack_from("<I", e, 28)[0]}
                    lfn = None

    def read(self, ent):
        data = b"".join(self.img[self.data + (c - 2) * self.cl:self.data + (c - 1) * self.cl]
                        for c in self.chain(ent["cluster"]))
        return data[:ent["size"]]

    def files(self, cluster=0, path=""):
        """{path: bytes} of every file, depth first."""
        out = {}
        for e in self.entries(cluster):
            if e["short"][0:1] == b".":
                continue
            name = e["long"] or e["short"][:8].decode("latin-1").strip() + (
                "." + e["short"][8:].decode("latin-1").strip() if e["short"][8:].strip() else "")
            if e["attr"] & 0x10:
                out.update(self.files(e["cluster"], f"{path}/{name}"))
            else:
                out[f"{path}/{name}"] = self.read(e)
        return out

    def free(self):
        return sum(1 for c in range(2, self.nclusters) if self.next(c) == 0)


def box_header(ctl):
    names = ("magic", "version", "num", "capacity", "total", "size", "next_id", "free_at",
             "oldest", "free_space")
    h = dict(zip(names, struct.unpack_from(">4s9I", ctl, 0)))
    h["ids"] = [struct.unpack_from(">I", ctl, off)[0] for off in range(0x80, len(ctl), 0x80)
                if struct.unpack_from(">I", ctl, off)[0]]
    return h
