#!/usr/bin/env python3
"""Make NVMe disks laid out as Windows leaves them, for the tests of
システム登録 (tests/ktest/x86_64/ktest_sysinst.c), and check them after.

  mkwindisk.py make <image> {C|E|R} [--free-mb N]
      a GPT disk: an EFI system partition (FAT32, 200 MB) with Windows'
      boot files (EFI/Microsoft/Boot/bootmgfw.efi, EFI/Microsoft/Boot/BCD,
      EFI/Boot/bootx64.efi), the Microsoft reserved partition (16 MB), a
      basic data partition (96 MB) named KTWIN-C (to be put beside),
      KTWIN-E (to be erased) or KTWIN-R (to be made TessronOS's), a stretch of N MB nothing takes (default
      1200), and the recovery partition (64 MB) at the end, as the fourth
      entry. Every partition is filled with bytes of its own; what each
      holds is written to <image>.json (SHA-256 of each partition and of
      each file of the ESP).
  mkwindisk.py check <image>
      after the system was put beside Windows: the four partitions there
      were have the same entries, and but for the ESP the same bytes; the
      ESP's Windows files are the same; a fifth entry, TessronOS, of the
      TSFS type lies in the free stretch and holds a TSFS volume; the ESP
      has EFI/TESSRON/BOOTX64.EFI, tessronos.elf and INSTALL.TXT.
  mkwindisk.py check-replace <image>
      after the basic data partition was made TessronOS's: four entries;
      the ESP, MSR and the recovery partition have the same entries, and
      but for the ESP the same bytes; the third entry is where it was,
      of the TSFS type, named TessronOS, grown to the recovery partition
      and holds a TSFS volume; the ESP's Windows files are the same and
      it has EFI/TESSRON/.
  mkwindisk.py check-erase <image>
      after the whole disk was taken: two entries, an ESP with
      EFI/BOOT/BOOTX64.EFI and EFI/TESSRON/ (the loader, tessronos.elf,
      INSTALL.TXT) on a FAT32 volume, and a TSFS partition after it.

Exits with 1 when anything is wrong. Written with the stdlib alone.
"""

import hashlib
import json
import os
import random
import struct
import sys
import uuid
import zlib

SECT = 512
MB = 2048

ESP_T = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
MSR_T = uuid.UUID("E3C9E316-0B5C-4DB8-817D-F92DF00215AE")
BASIC_T = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
WINRE_T = uuid.UUID("DE94BBA4-06D1-4D40-A16A-BFD50179D6AC")
TSFS_T = uuid.UUID("54465331-0000-4000-8000-5446532D5631")

WIN_FILES = ["EFI/Microsoft/Boot/bootmgfw.efi", "EFI/Microsoft/Boot/BCD", "EFI/Boot/bootx64.efi"]


# ---------------------------------------------------------------- GPT

def gpt_write(f, nsect, parts):
    """parts: (first, last, type uuid, name); 128 entries at 2, the backup at the end."""
    ent = bytearray(128 * 128)
    for i, (first, last, t, name) in enumerate(parts):
        e = t.bytes_le + uuid.uuid4().bytes_le + struct.pack("<QQQ", first, last, 0)
        e += name.encode("utf-16-le").ljust(72, b"\0")
        ent[i * 128:(i + 1) * 128] = e
    ecrc = zlib.crc32(ent) & 0xFFFFFFFF
    lu = nsect - 34
    dguid = uuid.uuid4().bytes_le

    def header(my, alt, elba):
        h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0,
                                  my, alt, 34, lu, dguid, elba, 128, 128, ecrc))
        struct.pack_into("<I", h, 16, zlib.crc32(h) & 0xFFFFFFFF)
        return bytes(h).ljust(SECT, b"\0")

    mbr = bytearray(SECT)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1,
                               min(nsect - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(mbr + header(1, nsect - 1, 2) + ent)
    f.seek((nsect - 33) * SECT)
    f.write(ent + header(nsect - 1, 1, nsect - 33))


def gpt_read(f, nsect):
    f.seek(SECT)
    h = f.read(92)
    assert h[:8] == b"EFI PART", "no GPT at sector 1"
    hh = bytearray(h)
    struct.pack_into("<I", hh, 16, 0)
    assert zlib.crc32(hh) & 0xFFFFFFFF == struct.unpack_from("<I", h, 16)[0], "the header's CRC"
    elba, nent, esz, ecrc = struct.unpack_from("<QIII", h, 72)
    f.seek(elba * SECT)
    ent = f.read(nent * esz)
    assert zlib.crc32(ent) & 0xFFFFFFFF == ecrc, "the entries' CRC"
    f.seek((nsect - 1) * SECT)
    b = f.read(92)
    assert b[:8] == b"EFI PART", "no backup GPT at the last sector"
    bb = bytearray(b)
    struct.pack_into("<I", bb, 16, 0)
    assert zlib.crc32(bb) & 0xFFFFFFFF == struct.unpack_from("<I", b, 16)[0], "the backup header's CRC"
    belba = struct.unpack_from("<Q", b, 72)[0]
    f.seek(belba * SECT)
    assert f.read(nent * esz) == ent, "the backup entries differ"
    out = []
    for i in range(nent):
        e = ent[i * esz:(i + 1) * esz]
        if e[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le").split("\0")[0]
        out.append(dict(entry=i, first=first, last=last, type=str(uuid.UUID(bytes_le=e[:16])).upper(),
                        uniq=str(uuid.UUID(bytes_le=e[16:32])), name=name))
    return out


# ---------------------------------------------------------------- FAT32

def short_alias(name, n):
    stem, _, ext = name.rpartition(".") if "." in name else (name, "", "")
    s = "".join(c for c in stem.upper() if c.isalnum())[:6] + "~%d" % n
    return (s.ljust(8) + ext.upper()[:3].ljust(3)).encode()


def needs_lfn(name):
    stem, dot, ext = name.partition(".")
    if len(stem) > 8 or len(ext) > 3 or "." in ext:
        return True
    return (stem != stem.upper() and stem != stem.lower()) or (ext != ext.upper() and ext != ext.lower())


def dir_entries(name, attr, clus, size, n):
    if not needs_lfn(name):
        stem, _, ext = name.partition(".")
        n83 = (stem.upper().ljust(8) + ext.upper().ljust(3)).encode()
        case = (0x08 if stem and stem == stem.lower() and stem != stem.upper() else 0) | \
               (0x10 if ext and ext == ext.lower() and ext != ext.upper() else 0)
        lfn = b""
    else:
        n83 = short_alias(name, n)
        case = 0
        units = list(struct.unpack("<%dH" % len(name), name.encode("utf-16-le")))
        if len(units) % 13:
            units.append(0)
        while len(units) % 13:
            units.append(0xFFFF)
        s = 0
        for c in n83:
            s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
        k = len(units) // 13
        lfn = b""
        for i in range(k, 0, -1):
            p = units[(i - 1) * 13:i * 13]
            e = bytearray(32)
            e[0] = i | (0x40 if i == k else 0)
            e[11] = 0x0F
            e[13] = s
            struct.pack_into("<5H", e, 1, *p[0:5])
            struct.pack_into("<6H", e, 14, *p[5:11])
            struct.pack_into("<2H", e, 28, *p[11:13])
            lfn += bytes(e)
    e = bytearray(32)
    e[0:11] = n83
    e[11] = attr
    e[12] = case
    struct.pack_into("<HHHHHHHI", e, 14, 0x6000, 0x5B21, 0x5B21, clus >> 16, 0x6000, 0x5B21, clus & 0xFFFF, size)
    return lfn + bytes(e)


def fat32_make(f, start, nsect, files):
    """files: {path: bytes}. One sector to a cluster."""
    rsvd, spc = 32, 1
    fatsz = 1
    while True:
        cl = (nsect - rsvd - 2 * fatsz) // spc
        need = -(-(cl + 2) * 4 // SECT)
        if need <= fatsz:
            break
        fatsz = need
    data = start + rsvd + 2 * fatsz
    fat = [0x0FFFFFF8, 0x0FFFFFFF]
    dirs = {"": []}
    blobs = {}

    def take(n):
        c = len(fat)
        for i in range(n):
            fat.append(c + i + 1 if i < n - 1 else 0x0FFFFFFF)
        return c

    root = take(1)
    dclus = {"": root}
    for path in sorted(files):
        parts = path.split("/")
        for d in range(1, len(parts)):
            dp = "/".join(parts[:d])
            if dp not in dclus:
                dclus[dp] = take(1)
                dirs[dp] = []
                dirs["/".join(parts[:d - 1])].append((parts[d - 1], 0x10, dclus[dp], 0))
        b = files[path]
        c = take(max(1, -(-len(b) // SECT)))
        blobs[c] = b
        dirs["/".join(parts[:-1])].append((parts[-1], 0x20, c, len(b)))
    for dp, ents in dirs.items():
        raw = bytearray()
        if dp == "":
            e = bytearray(32)
            e[0:11] = b"SYSTEM     "
            e[11] = 0x08
            raw += e
        else:
            par = "/".join(dp.split("/")[:-1])
            raw += dir_entries(".", 0x10, dclus[dp], 0, 0)[:0] + \
                struct.pack("<11sB", b".          ", 0x10) + bytes(8) + struct.pack("<H", dclus[dp] >> 16) + \
                bytes(4) + struct.pack("<HI", dclus[dp] & 0xFFFF, 0)
            pc = 0 if par == "" else dclus[par]
            raw += struct.pack("<11sB", b"..         ", 0x10) + bytes(8) + struct.pack("<H", pc >> 16) + \
                bytes(4) + struct.pack("<HI", pc & 0xFFFF, 0)
        for i, (name, attr, c, size) in enumerate(ents):
            raw += dir_entries(name, attr, c, size, i + 1)
        assert len(raw) <= SECT, "a directory larger than its cluster"
        blobs[dclus[dp]] = bytes(raw)
    for c, b in blobs.items():
        f.seek((data + (c - 2) * spc) * SECT)
        f.write(b)
    bpb = bytearray(SECT)
    bpb[0:3] = b"\xeb\x58\x90"
    bpb[3:11] = b"MSDOS5.0"
    struct.pack_into("<HBHBHHBHHHII", bpb, 11, SECT, spc, rsvd, 2, 0, 0, 0xF8, 0, 63, 255, start, nsect)
    struct.pack_into("<IHHIHH", bpb, 36, fatsz, 0, 0, 2, 1, 6)
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into("<I", bpb, 67, 0x1234ABCD)
    bpb[71:82] = b"NO NAME    "
    bpb[82:90] = b"FAT32   "
    bpb[510:512] = b"\x55\xaa"
    fsi = bytearray(SECT)
    struct.pack_into("<I", fsi, 0, 0x41615252)
    struct.pack_into("<III", fsi, 484, 0x61417272, 0xFFFFFFFF, len(fat))
    struct.pack_into("<I", fsi, 508, 0xAA550000)
    f.seek(start * SECT)
    f.write(bpb + fsi)
    f.seek((start + 6) * SECT)
    f.write(bpb + fsi)
    fb = struct.pack("<%dI" % len(fat), *fat)
    for k in range(2):
        f.seek((start + rsvd + k * fatsz) * SECT)
        f.write(fb)


def fat32_files(f, start):
    """{path: bytes} of a FAT32 volume, the long names taken."""
    f.seek(start * SECT)
    b = f.read(SECT)
    assert b[82:90] == b"FAT32   " and b[510:512] == b"\x55\xaa", "not a FAT32 volume"
    bps, spc, rsvd, nfat = struct.unpack_from("<HBHB", b, 11)
    fatsz, = struct.unpack_from("<I", b, 36)
    rootc, = struct.unpack_from("<I", b, 44)
    f.seek((start + rsvd) * SECT)
    fat = struct.unpack("<%dI" % (fatsz * bps // 4), f.read(fatsz * bps))
    data = start + rsvd + nfat * fatsz
    csz = spc * bps

    def chain(c, size=None):
        out = bytearray()
        while 2 <= c < 0x0FFFFFF8:
            f.seek((data + (c - 2) * spc) * bps)
            out += f.read(csz)
            c = fat[c] & 0x0FFFFFFF
        return bytes(out[:size]) if size is not None else bytes(out)

    files = {}

    def walk(c, prefix):
        raw = chain(c)
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                u = struct.unpack_from("<5H", e, 1) + struct.unpack_from("<6H", e, 14) + struct.unpack_from("<2H", e, 28)
                lfn.insert(0, u)
                continue
            if e[11] & 0x08:
                lfn = []
                continue
            if lfn:
                units = [x for p in lfn for x in p]
                if 0 in units:
                    units = units[:units.index(0)]
                name = struct.pack("<%dH" % len(units), *units).decode("utf-16-le")
            else:
                stem, ext = e[0:8].decode().rstrip(), e[8:11].decode().rstrip()
                if e[12] & 0x08:
                    stem = stem.lower()
                if e[12] & 0x10:
                    ext = ext.lower()
                name = stem + ("." + ext if ext else "")
            lfn = []
            if name in (".", ".."):
                continue
            c2 = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            size, = struct.unpack_from("<I", e, 28)
            path = prefix + name
            if e[11] & 0x10:
                walk(c2, path + "/")
            else:
                files[path] = chain(c2, size)

    walk(rootc, "")
    return files


def ifind(files, path):
    for k, v in files.items():
        if k.lower() == path.lower():
            return v
    return None


# ---------------------------------------------------------------- the commands

def fill(f, first, last, seed):
    """A partition filled with bytes of its own, a megabyte at a time."""
    r = random.Random(seed)
    block = bytes(r.getrandbits(8) for _ in range(MB * SECT // 64)) * 64
    f.seek(first * SECT)
    n = last - first + 1
    i = 0
    while n > 0:
        k = min(n, MB)
        f.write(struct.pack("<QQ", seed, i) + block[16:k * SECT])
        n -= k
        i += 1


def sha(f, first, last):
    h = hashlib.sha256()
    f.seek(first * SECT)
    n = last - first + 1
    while n > 0:
        k = min(n, MB)
        h.update(f.read(k * SECT))
        n -= k
    return h.hexdigest()


def make(img, kind, free_mb):
    esp, msr, dat, rec = 200, 16, 96, 64
    total = 1 + esp + msr + dat + free_mb + rec + 1
    nsect = total * MB
    p_esp = (MB, MB + esp * MB - 1)
    p_msr = (p_esp[1] + 1, p_esp[1] + msr * MB)
    p_dat = (p_msr[1] + 1, p_msr[1] + dat * MB)
    p_rec = (nsect - MB - rec * MB, nsect - MB - 1)
    parts = [(p_esp[0], p_esp[1], ESP_T, "EFI system partition"),
             (p_msr[0], p_msr[1], MSR_T, "Microsoft reserved partition"),
             (p_dat[0], p_dat[1], BASIC_T, "KTWIN-" + kind),
             (p_rec[0], p_rec[1], WINRE_T, "")]
    r = random.Random(7)
    boot = bytes(r.getrandbits(8) for _ in range(300 * 1024))
    files = {"EFI/Microsoft/Boot/bootmgfw.efi": boot,
             "EFI/Microsoft/Boot/BCD": bytes(r.getrandbits(8) for _ in range(64 * 1024)),
             "EFI/Boot/bootx64.efi": boot}
    with open(img, "wb") as f:
        f.truncate(nsect * SECT)
        fill(f, p_msr[0], p_msr[1], 2)
        fill(f, p_dat[0], p_dat[1], 3)
        fill(f, p_rec[0], p_rec[1], 4)
        fat32_make(f, p_esp[0], p_esp[1] - p_esp[0] + 1, files)
        gpt_write(f, nsect, parts)
    with open(img, "rb") as f:
        tbl = gpt_read(f, nsect)
        info = dict(nsect=nsect, parts=tbl,
                    sha=[None if p["type"] == str(ESP_T).upper() else sha(f, p["first"], p["last"]) for p in tbl],
                    files={k: hashlib.sha256(v).hexdigest() for k, v in files.items()},
                    free=[p_dat[1] + 1, p_rec[0] - 1])
    with open(img + ".json", "w") as f:
        json.dump(info, f, indent=1)
    print("mkwindisk: %s: %d MB, free %d MB between sectors %d and %d" % (img, total, free_mb, p_dat[1] + 1, p_rec[0] - 1))


def check(img):
    with open(img + ".json") as f:
        info = json.load(f)
    bad = []
    with open(img, "rb") as f:
        tbl = gpt_read(f, info["nsect"])
        if len(tbl) != len(info["parts"]) + 1:
            bad.append("%d entries, %d were there" % (len(tbl), len(info["parts"])))
        for i, p in enumerate(info["parts"]):
            q = tbl[i] if i < len(tbl) else {}
            for k in ("entry", "first", "last", "type", "uniq", "name"):
                if q.get(k) != p[k]:
                    bad.append("entry %d: %s %s, was %s" % (i, k, q.get(k), p[k]))
            if info["sha"][i] is not None:
                h = sha(f, p["first"], p["last"])
                print("  partition %d %s: %s" % (i + 1, "same" if h == info["sha"][i] else "CHANGED", h[:16]))
                if h != info["sha"][i]:
                    bad.append("partition %d changed" % (i + 1))
        if len(tbl) > len(info["parts"]):
            n = tbl[-1]
            print("  partition %d: %s \"%s\" sectors %d-%d" % (len(tbl), n["type"], n["name"], n["first"], n["last"]))
            if n["type"] != str(TSFS_T).upper() or n["name"] != "TessronOS":
                bad.append("the new entry is not TessronOS's")
            if n["first"] < info["free"][0] or n["last"] > info["free"][1]:
                bad.append("the new partition is not in the free stretch")
            f.seek(n["first"] * SECT)
            if f.read(8) != b"TFSVOL02":
                bad.append("no TSFS volume on the new partition")
        esp = [p for p in tbl if p["type"] == str(ESP_T).upper()][0]
        files = fat32_files(f, esp["first"])
        for k, h in info["files"].items():
            v = ifind(files, k)
            if v is None or hashlib.sha256(v).hexdigest() != h:
                bad.append("Windows' file %s changed" % k)
        for k in ("EFI/TESSRON/BOOTX64.EFI", "EFI/TESSRON/tessronos.elf", "EFI/TESSRON/INSTALL.TXT"):
            v = ifind(files, k)
            print("  %s: %s" % (k, "%d bytes" % len(v) if v is not None else "MISSING"))
            if v is None:
                bad.append("no " + k)
    for b in bad:
        print("mkwindisk: " + b)
    print("mkwindisk: %s: %s" % (img, "NG" if bad else "OK"))
    return 1 if bad else 0


def check_replace(img):
    with open(img + ".json") as f:
        info = json.load(f)
    bad = []
    with open(img, "rb") as f:
        tbl = gpt_read(f, info["nsect"])
        if len(tbl) != len(info["parts"]):
            bad.append("%d entries, %d were there" % (len(tbl), len(info["parts"])))
        for i, p in enumerate(info["parts"]):
            q = tbl[i] if i < len(tbl) else {}
            if i == 2:
                print("  partition 3: %s \"%s\" sectors %d-%d" % (q.get("type"), q.get("name"), q.get("first", 0),
                                                                q.get("last", 0)))
                if q.get("type") != str(TSFS_T).upper() or q.get("name") != "TessronOS":
                    bad.append("partition 3 is not TessronOS's")
                if q.get("first") != p["first"] or q.get("entry") != p["entry"]:
                    bad.append("partition 3 moved")
                if q.get("last") != info["free"][1] // MB * MB - 1 and q.get("last") != info["free"][1]:
                    bad.append("partition 3 not grown to the free stretch's end (%s)" % q.get("last"))
                f.seek(p["first"] * SECT)
                if f.read(8) != b"TFSVOL02":
                    bad.append("no TSFS volume on partition 3")
                continue
            for k in ("entry", "first", "last", "type", "uniq", "name"):
                if q.get(k) != p[k]:
                    bad.append("entry %d: %s %s, was %s" % (i, k, q.get(k), p[k]))
            if info["sha"][i] is not None:
                h = sha(f, p["first"], p["last"])
                print("  partition %d %s: %s" % (i + 1, "same" if h == info["sha"][i] else "CHANGED", h[:16]))
                if h != info["sha"][i]:
                    bad.append("partition %d changed" % (i + 1))
        esp = [p for p in tbl if p["type"] == str(ESP_T).upper()][0]
        files = fat32_files(f, esp["first"])
        for k, h in info["files"].items():
            v = ifind(files, k)
            if v is None or hashlib.sha256(v).hexdigest() != h:
                bad.append("Windows' file %s changed" % k)
        for k in ("EFI/TESSRON/BOOTX64.EFI", "EFI/TESSRON/tessronos.elf", "EFI/TESSRON/INSTALL.TXT"):
            v = ifind(files, k)
            print("  %s: %s" % (k, "%d bytes" % len(v) if v is not None else "MISSING"))
            if v is None:
                bad.append("no " + k)
    for b in bad:
        print("mkwindisk: " + b)
    print("mkwindisk: %s: %s" % (img, "NG" if bad else "OK"))
    return 1 if bad else 0


def check_erase(img):
    bad = []
    nsect = os.path.getsize(img) // SECT
    with open(img, "rb") as f:
        tbl = gpt_read(f, nsect)
        for p in tbl:
            print("  entry %d: %s \"%s\" sectors %d-%d" % (p["entry"], p["type"], p["name"], p["first"], p["last"]))
        if len(tbl) != 2 or tbl[0]["type"] != str(ESP_T).upper() or tbl[1]["type"] != str(TSFS_T).upper():
            bad.append("not an ESP and a TessronOS partition")
        else:
            files = fat32_files(f, tbl[0]["first"])
            for k in ("EFI/BOOT/BOOTX64.EFI", "EFI/TESSRON/BOOTX64.EFI", "EFI/TESSRON/tessronos.elf",
                      "EFI/TESSRON/INSTALL.TXT"):
                v = ifind(files, k)
                print("  %s: %s" % (k, "%d bytes" % len(v) if v is not None else "MISSING"))
                if v is None:
                    bad.append("no " + k)
            f.seek(tbl[1]["first"] * SECT)
            if f.read(8) != b"TFSVOL02":
                bad.append("no TSFS volume")
    for b in bad:
        print("mkwindisk: " + b)
    print("mkwindisk: %s: %s" % (img, "NG" if bad else "OK"))
    return 1 if bad else 0


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "make" and a[2] in ("C", "E", "R"):
        free = int(a[a.index("--free-mb") + 1]) if "--free-mb" in a else 1200
        make(a[1], a[2], free)
        return 0
    if len(a) == 2 and a[0] == "check":
        return check(a[1])
    if len(a) == 2 and a[0] == "check-replace":
        return check_replace(a[1])
    if len(a) == 2 and a[0] == "check-erase":
        return check_erase(a[1])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
