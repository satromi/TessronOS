#!/usr/bin/env python3
"""A copy of the test disk whose first partition is a native volume
damaged on purpose, for the test of ディスク修復 (ui_diskfix.steps).

  tools/tests/dfix_disk.py [--boot] TEST_DISK OUT

The volume holds a cabinet the superblock's root names, linking to two
documents, one of which has a record of three blocks and links to the
other, and two documents that link only to each other. Then, below the
store: a reference count is raised by three, three blocks are taken that
nothing uses, and a block of the long record is marked free. What
fsck.tsfs says of the result is printed, so that the damage is seen by
a reader of the format other than the one being tested.

--boot also raises the count of the program box on the system volume
(the third partition) by two, and puts the request DISKFIX.REQ for its
repair on the boot volume (the second partition, FAT32), as the
accessory leaves it before the machine starts again
(ui_diskfix_boot.steps).

  tools/tests/dfix_disk.py --pc SD_IMG OUT

does only what --boot does, on the disk image of the x64 PC (make
TARGET=_X64_ sdimg): the EFI system partition (the first, FAT32) is the
boot volume and the system volume is the second partition, nvmea1. No
damaged volume is made there: the first partition is the boot volume
(tools/x64/tests/ui_diskfix_boot.steps).
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

from tsfs_py import Volume                     # noqa: E402
from tsfs_py.image import BlockImage, read_partitions   # noqa: E402

BIG = 12000


def meta(name, nrec):
    return {"name": name, "relationship": [], "linktype": False,
            "makeDate": "2026-10-04T00:00:00Z", "updateDate": "2026-10-04T00:00:00Z",
            "accessDate": "2026-10-04T00:00:00Z", "periodDate": None,
            "refCount": 0, "recordCount": nrec, "editable": True, "deletable": True,
            "readable": True, "maker": "TessronOS", "applist": {}}


def link(target, n):
    return ('<link id="%s_0.xtad" vobjid="%s" vobjleft="8" vobjtop="%d" vobjright="248" '
            'vobjbottom="%d" height="25" chsz="14" frcol="#000000" chcol="#000000" '
            'tbcol="#ffffff" bgcol="#ffffff" pictdisp="true" namedisp="true" '
            'framedisp="true"/>\n' % (target, uuid.uuid4(), 8 + n * 32, 33 + n * 32))


def write_obj(d, u, name, body, links, big=False, figure=False):
    with open(os.path.join(d, "%s.json" % u), "w", encoding="utf-8") as f:
        json.dump(meta(name, 2 if big else 1), f, ensure_ascii=False, indent=2)
    x = '<tad version="1.0" encoding="UTF-8" filename="%s">\n' % name
    if figure:
        x += "<figure>\n" + "".join(link(t, i) for i, t in enumerate(links)) + "</figure>\n"
    else:
        x += "<document>\n<p>%s</p>\n" % body
        x += "".join(link(t, i) for i, t in enumerate(links))
        x += "</document>\n"
    x += "</tad>\n"
    with open(os.path.join(d, "%s_0.xtad" % u), "w", encoding="utf-8") as f:
        f.write(x)
    if big:
        with open(os.path.join(d, "%s_1.bin" % u), "wb") as f:
            f.write(bytes((i * 13 + (i >> 7)) & 0xFF for i in range(BIG)))


PROG_BOX = uuid.UUID("01a0d6d1-08cf-750a-8479-73bdcfc4823c")
SYSVOL_DEV = "vblka2"
MODE_REPAIR = 0x10


def fat32_put(path, part, name83, data):
    """A file put in the root directory of a FAT32 volume: one cluster,
    an 8.3 name with no long name."""
    with open(path, "r+b") as f:
        def rd(off, n):
            f.seek(part.offset + off)
            return f.read(n)

        def wr(off, b):
            f.seek(part.offset + off)
            f.write(b)

        bpb = rd(0, 512)
        bps = struct.unpack_from("<H", bpb, 11)[0]
        spc = bpb[13]
        rsvd = struct.unpack_from("<H", bpb, 14)[0]
        nfats = bpb[16]
        fatsz = struct.unpack_from("<I", bpb, 36)[0]
        root = struct.unpack_from("<I", bpb, 44)[0]
        csize = bps * spc
        data0 = (rsvd + nfats * fatsz) * bps
        if len(data) > csize:
            raise SystemExit("dfix_disk: the request is larger than a cluster")
        fat = bytearray(rd(rsvd * bps, fatsz * bps))

        def entry(n):
            return struct.unpack_from("<I", fat, n * 4)[0] & 0x0FFFFFFF

        free = next(n for n in range(2, len(fat) // 4) if entry(n) == 0)
        clus, slot = root, None
        while slot is None:
            d = rd(data0 + (clus - 2) * csize, csize)
            for i in range(0, csize, 32):
                if d[i] in (0x00, 0xE5):
                    slot = data0 + (clus - 2) * csize + i
                    break
            if slot is None:
                clus = entry(clus)
                if clus >= 0x0FFFFFF8:
                    raise SystemExit("dfix_disk: the root directory is full")
        struct.pack_into("<I", fat, free * 4, 0x0FFFFFFF)
        for k in range(nfats):
            wr((rsvd + k * fatsz) * bps + free * 4, bytes(fat[free * 4:free * 4 + 4]))
        wr(data0 + (free - 2) * csize, data + bytes(csize - len(data)))
        ent = bytearray(32)
        ent[0:11] = name83.encode("ascii")
        ent[11] = 0x20
        struct.pack_into("<HH", ent, 20, free >> 16, 0)
        struct.pack_into("<HHHI", ent, 22, 0x6000, 0x5A21, free & 0xFFFF, len(data))
        wr(slot, bytes(ent))


def boot_request(out, fat=2, sysvol=3, sysdev=SYSVOL_DEV):
    parts = {p.index: p for p in read_partitions(out)}
    dev = BlockImage(out, offset=parts[sysvol].offset, length=parts[sysvol].length, writable=True)
    with Volume.open(dev, replay=True) as vol:
        o = vol.object(PROG_BOX)
        o.refcnt = o.refcnt + 2
        o.store()
        vol.flush(both=True, spares=True)
    fat32_put(out, parts[fat], "DISKFIX REQ", ("%s %d\n" % (sysdev, MODE_REPAIR)).encode("ascii"))
    print("dfix_disk: count of the program box raised by 2, the repair of %s asked for" % sysdev)


def main():
    args = sys.argv[1:]
    boot = "--boot" in args
    pc = "--pc" in args
    args = [a for a in args if a not in ("--boot", "--pc")]
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    src, out = args
    shutil.copyfile(src, out)
    if pc:
        boot_request(out, fat=1, sysvol=2, sysdev="nvmea1")
        return 0
    root, a, b, c, d = (uuid.uuid4() for _ in range(5))
    tmp = tempfile.mkdtemp(prefix="dfix_")
    try:
        write_obj(tmp, b, "参考資料", "参考資料の本文", [])
        write_obj(tmp, a, "設計メモ", "設計メモの本文", [b], big=True)
        write_obj(tmp, c, "下書き1", "下書き1の本文", [d])
        write_obj(tmp, d, "下書き2", "下書き2の本文", [c])
        write_obj(tmp, root, "保管箱", "", [a, b], figure=True)
        subprocess.check_call([sys.executable, os.path.join(TOOLS, "mktsfs"), "-q", "-L", "DFIX",
                               "--partition", "1", "--from", tmp, "--root", str(root), out])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    part = [p for p in read_partitions(out) if p.index == 1][0]
    dev = BlockImage(out, offset=part.offset, length=part.length, writable=True)
    with Volume.open(dev, replay=True) as vol:
        o = vol.object(b)
        o.refcnt = o.refcnt + 3
        o.store()
        start, count = vol.alloc_ext(3)
        oa = vol.object(a)
        runs = [pl for pl in (e.place for e in oa.placements()) if pl.nbytes == BIG]
        lost = runs[0].a
        vol.free_ext(lost, 1)
        vol.flush(both=True, spares=True)
    print("dfix_disk: %s: count of %s raised by 3, blocks %d..%d taken, block %d made free"
          % (out, b, start, start + count - 1, lost))
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "fsck.tsfs"), "--partition", "1", out],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    sys.stdout.write(r.stdout)
    if boot:
        boot_request(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
