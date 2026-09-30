#!/usr/bin/env python3
"""A package of an application (.tpk): the program object and what it
brings, in one file, to be installed by dropping it on the list of the
バージョン sheet of システム環境設定.

  tools/mktpk.py --from DIR [--from DIR ...] --program ID|UUID -o OUT.tpk
  tools/mktpk.py --list PKG.tpk

The objects are taken in the TADjs form the system's definitions are
kept in ({uuid}.json, {uuid}_N.xtad, {uuid}_N.bin, {uuid}.ico), spread
over the --from directories: the program object, found by its id or its
UUID, and the templates its metadata names ("tessronos.program.base").
The version is the one the program's metadata says
("tessronos.program.version", R1.000).

The file, all of it little endian:

  0   "TSPK"
  4   UH  format (1)
  6   UH  n, the entries
  8   UW  0
  12  UW  0
  16  n entries of 64 bytes:
        name[48]  the file's name as above, or "package.json"; NUL filled
        UW offset from the start of the file
        UW size
        UW record type (OB_RT_*: 1 xmlTAD, 9 an executable, 15 bytes)
        UW 0
  ... the bytes of each entry

"package.json" says what it is:
  {"format":1,"program":"UUID","id":"...","name":"...","version":"R1.000",
   "objects":["UUID",...]}
"""

import argparse
import json
import os
import re
import struct
import sys

MAGIC = b"TSPK"
FORMAT = 1
HEAD = 16
ENTRY = 64
NAME_MAX = 48
RT_TAD, RT_PROG, RT_SYSDATA = 1, 9, 15
VERSION_RE = re.compile(r"^R[0-9]+\.[0-9]{3}$")
FILE_RE = re.compile(r"^([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"
                     r"(\.json|\.ico|_([0-9]+)\.(xtad|bin))$")


def files_of(dirs, uuid):
    """Every file of one object, by its name, from wherever it is"""
    out = {}
    for d in dirs:
        for name in sorted(os.listdir(d)):
            m = FILE_RE.match(name)
            if m and m.group(1) == uuid and name not in out:
                out[name] = os.path.join(d, name)
    return out


def find_program(dirs, want):
    for d in dirs:
        for name in sorted(os.listdir(d)):
            m = FILE_RE.match(name)
            if not m or m.group(2) != ".json":
                continue
            try:
                meta = json.load(open(os.path.join(d, name), encoding="utf-8"))
            except ValueError:
                continue
            prog = meta.get("tessronos", {}).get("program") if isinstance(meta, dict) else None
            if isinstance(prog, dict) and want in (m.group(1), prog.get("id")):
                return m.group(1), meta, prog
    return None, None, None


def record_type(name, data):
    if name.endswith(".xtad"):
        return RT_TAD
    if name.endswith(".bin") and data[:4] == b"\x7fELF":
        return RT_PROG
    return RT_SYSDATA


def make(args):
    uuid, meta, prog = find_program(args.dirs, args.program)
    if uuid is None:
        sys.exit("mktpk: no program %s in %s" % (args.program, ", ".join(args.dirs)))
    ver = prog.get("version", "")
    if not VERSION_RE.match(ver):
        sys.exit("mktpk: the program's version %r is not R1.000's form" % ver)
    objects = [uuid] + [b for b in prog.get("base", []) if b != uuid]
    entries = []
    for u in objects:
        fs = files_of(args.dirs, u)
        if u + ".json" not in fs:
            sys.exit("mktpk: the object %s has no metadata" % u)
        for name, path in fs.items():
            data = open(path, "rb").read()
            entries.append((name, data, record_type(name, data)))
    manifest = {"format": FORMAT, "program": uuid, "id": prog.get("id", ""),
                "name": prog.get("name") or meta.get("name", ""), "version": ver,
                "objects": objects}
    entries.insert(0, ("package.json",
                       json.dumps(manifest, ensure_ascii=False).encode("utf-8"), RT_SYSDATA))

    out = bytearray(struct.pack("<4sHHII", MAGIC, FORMAT, len(entries), 0, 0))
    at = HEAD + ENTRY * len(entries)
    body = bytearray()
    for name, data, rt in entries:
        nb = name.encode("utf-8")
        if len(nb) >= NAME_MAX:
            sys.exit("mktpk: the name %s is too long" % name)
        out += struct.pack("<48sIIII", nb, at + len(body), len(data), rt, 0)
        body += data
    out += body
    with open(args.output, "wb") as f:
        f.write(out)
    print("%s: %s %s (%s), %d objects, %d bytes"
          % (args.output, manifest["name"], ver, manifest["id"], len(objects), len(out)))


def show(path):
    raw = open(path, "rb").read()
    magic, fmt, n, _, _ = struct.unpack_from("<4sHHII", raw, 0)
    if magic != MAGIC:
        sys.exit("mktpk: %s is not a package" % path)
    for i in range(n):
        name, off, size, rt, _ = struct.unpack_from("<48sIIII", raw, HEAD + ENTRY * i)
        name = name.rstrip(b"\0").decode("utf-8")
        print("%-44s %8d  type %2d" % (name, size, rt))
        if name == "package.json":
            print("  " + raw[off:off + size].decode("utf-8"))


def main():
    p = argparse.ArgumentParser(prog="mktpk")
    p.add_argument("--from", dest="dirs", action="append", default=[])
    p.add_argument("--program")
    p.add_argument("-o", "--output")
    p.add_argument("--list")
    a = p.parse_args()
    if a.list:
        show(a.list)
        return 0
    if not a.dirs or not a.program or not a.output:
        p.error("--from, --program and -o are needed")
    make(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
