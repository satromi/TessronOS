#!/usr/bin/env python3
"""A package (.tpk): an application, or the system itself, in one file,
to be installed by dropping it on the list of the バージョン sheet of
システム環境設定.

  tools/mktpk.py --from DIR [--from DIR ...] --program ID|UUID -o OUT.tpk
  tools/mktpk.py --system --machine M --arch A --version R1.200
                 [--over DIR ...] --from DIR [--from DIR ...] [--user DIR ...]
                 [--exclude NAME ...] [--keep UUID ...]
                 [--boot PATH=FILE ...] [--boot-keep PATH=FILE ...] -o OUT.tpk
  tools/mktpk.py --system --machine M --arch A --version R1.200
                 [--over, --from, --user, --exclude, --keep as above] --manifest DIR
  tools/mktpk.py --list PKG.tpk

The objects are taken in the TADjs form the system's definitions are
kept in ({uuid}.json, {uuid}_N.xtad, {uuid}_N.bin, {uuid}.ico,
{uuid}_N_M.ext), spread over the --from directories.

An application's package holds the program object, found by its id or
its UUID, the templates its metadata names ("tessronos.program.base")
and the objects it uses that are not templates ("tessronos.program.res":
its menu, its licence), which its record 0 links to so that they go with it. The version is the one the program's
metadata says ("tessronos.program.version", R1.000).

The system's package (--system) holds what an image of one machine is
made of: every object of the --over and --from directories, read as
tools/mktsfs reads them for the system volume (a file of an --over
directory replaces the same file of the --from ones; --exclude leaves a
file out), and of the --user directories (the objects the person starts
with, etc/xtad) only the templates the programs name. Each object says
how an update treats it, in its metadata ("tessronos.update"):

  (nothing)  the system's own: its metadata, icon, records and resources
             are made the package's
  "keep"     the person's (the settings, the user dictionary, the
             browser's store, the home): made only when the system does
             not have it, and never written over
  "links"    a box the person adds to (the program box, the font box,
             the system box): made when missing; otherwise kept as it
             is, with a link added to each object the package's box
             links to and the system's does not

The system volume keeps the same list (package.json's "objects", "keep"
and "links", with the version, the machine and the processor, but not
the boot files) as record 1 of the system box (システム): --manifest DIR
writes it as {uuid}_1.bin for tools/mktsfs to take with --from, and an
update writes the package's there. システム登録 reads it to put the
system's objects on a disk the system was put on before, as an update
would, leaving the rest there as it is.

--keep names one more object to be kept (one a build step makes, whose
metadata the build does not write: the Wi-Fi settings). The files the
firmware and the loader read are entries "boot/PATH", PATH as a medium
of tools/mksd.py has the file; the update puts each where the running
system keeps it (a PC installed by システム登録 in \\EFI\\TESSRON\\).
--boot ones are put on the boot partition anew, the one there kept
beside as PATH.old;
--boot-keep ones only where there is none (the start-up settings the
person writes). --machine is the machine the package is for (rpi5,
rpi4, x64, qemu_virt), --arch the processor's architecture.

The file, all of it little endian:

  0   "TSPK"
  4   UH  format (1)
  6   UH  n, the entries
  8   UW  0
  12  UW  0
  16  n entries of 64 bytes:
        name[48]  the file's name as above, "boot/PATH", or "package.json";
                  NUL filled
        UW offset from the start of the file
        UW size
        UW record type (OB_RT_*: 1 xmlTAD, 9 an executable, 15 bytes)
        UW the record's subtype (0 in an application's package)
  ... the bytes of each entry

"package.json" says what it is:
  {"format":1,"program":"UUID","id":"...","name":"...","version":"R1.000",
   "objects":["UUID",...]}
  {"format":1,"kind":"system","id":"tessronos","name":"TessronOS",
   "version":"R1.200","machine":"rpi5","arch":"aarch64",
   "objects":["UUID",...],"keep":["UUID",...],"links":["UUID",...],
   "boot":["PATH",...],"bootkeep":["PATH",...]}
"""

import argparse
import json
import os
import re
import struct
import sys
import uuid as uuidmod

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"TSPK"
FORMAT = 1
HEAD = 16
ENTRY = 64
NAME_MAX = 48
RT_TAD, RT_PROG, RT_SYSDATA = 1, 9, 15
VERSION_RE = re.compile(r"^R[0-9]+\.[0-9]{3}$")
FILE_RE = re.compile(r"^([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"
                     r"(\.json|\.ico|_([0-9]+)\.(xtad|bin))$")
MACHINES = ("rpi5", "rpi4", "x64", "qemu_virt")
ARCHS = ("aarch64", "x86_64")
UPDATES = ("keep", "links")
SYSTEM_BOX = "01a0d8c4-5211-7c52-9e0a-3b61d27f4c85"     # システム (SYSDEF_SYSTEM_BOX)
MANIFEST_REC = 1        # its record that lists the system's objects (dtsysup.c SU_MAN_REC)
U32 = 0xFFFFFFFF


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


def write_package(path, entries):
    """entries: [(name, bytes or a file's path, rt, sub)], written in order"""
    sizes = []
    for name, src, _rt, _sub in entries:
        nb = name.encode("utf-8")
        if len(nb) >= NAME_MAX:
            sys.exit("mktpk: the name %s is too long" % name)
        sizes.append(len(src) if isinstance(src, (bytes, bytearray)) else os.path.getsize(src))
    at = HEAD + ENTRY * len(entries)
    if len(entries) > 0xFFFF or at + sum(sizes) > U32:
        sys.exit("mktpk: %d entries of %d bytes do not fit in a package"
                 % (len(entries), sum(sizes)))
    with open(path, "wb") as f:
        f.write(struct.pack("<4sHHII", MAGIC, FORMAT, len(entries), 0, 0))
        for (name, _src, rt, sub), size in zip(entries, sizes):
            f.write(struct.pack("<48sIIII", name.encode("utf-8"), at, size, rt, sub))
            at += size
        for name, src, _rt, _sub in entries:
            if isinstance(src, (bytes, bytearray)):
                f.write(src)
            else:
                with open(src, "rb") as g:
                    while True:
                        b = g.read(1 << 20)
                        if not b:
                            break
                        f.write(b)
    return at


def make(args):
    uuid, meta, prog = find_program(args.dirs, args.program)
    if uuid is None:
        sys.exit("mktpk: no program %s in %s" % (args.program, ", ".join(args.dirs)))
    ver = prog.get("version", "")
    if not VERSION_RE.match(ver):
        sys.exit("mktpk: the program's version %r is not R1.000's form" % ver)
    objects = [uuid] + [b for b in prog.get("base", []) + prog.get("res", []) if b != uuid]
    entries = []
    for u in objects:
        fs = files_of(args.dirs, u)
        if u + ".json" not in fs:
            sys.exit("mktpk: the object %s has no metadata" % u)
        for name, path in fs.items():
            data = open(path, "rb").read()
            entries.append((name, data, record_type(name, data), 0))
    manifest = {"format": FORMAT, "program": uuid, "id": prog.get("id", ""),
                "name": prog.get("name") or meta.get("name", ""), "version": ver,
                "objects": objects}
    entries.insert(0, ("package.json",
                       json.dumps(manifest, ensure_ascii=False).encode("utf-8"), RT_SYSDATA, 0))
    size = write_package(args.output, entries)
    print("%s: %s %s (%s), %d objects, %d bytes"
          % (args.output, manifest["name"], ver, manifest["id"], len(objects), size))


# ---------------------------------------------------------------- the system's package

def boot_pairs(specs, what):
    out = []
    for s in specs:
        path, eq, src = s.partition("=")
        path = path.strip("/")
        if not eq or not path or not os.path.isfile(src):
            sys.exit("mktpk: %s %r is not PATH=FILE of a file there is" % (what, s))
        if ".." in path.split("/"):
            sys.exit("mktpk: %s %r goes out of the boot partition" % (what, s))
        out.append((path, src))
    return out


def system_objects(args):
    """The objects of the system as the image has them, each with how an
    update treats it: [(UUID text, its set, None | "keep" | "links")] in
    UUID order, and those left out with why. The package (make_system)
    and the list the system volume keeps (make_manifest) both take
    theirs from here, so that the two say the same."""
    from tsfs_py import transfer

    if args.machine not in MACHINES:
        sys.exit("mktpk: --machine is one of %s" % ", ".join(MACHINES))
    if args.arch not in ARCHS:
        sys.exit("mktpk: --arch is one of %s" % ", ".join(ARCHS))
    if not VERSION_RE.match(args.version or ""):
        sys.exit("mktpk: the system's version %r is not R1.200's form" % args.version)

    problems = []
    sets = transfer.scan(args.over + args.dirs, problems, len(args.over), args.exclude)
    user = transfer.scan(args.user, problems, 0, args.exclude) if args.user else {}
    for p in problems:
        sys.stderr.write("mktpk: %s\n" % p)

    # the templates the programs name, of the objects the person starts with
    def program_of(s):
        try:
            d = json.load(open(s.json, encoding="utf-8"))
        except (ValueError, OSError):
            return None
        p = d.get("tessronos", {}).get("program") if isinstance(d, dict) else None
        return p if isinstance(p, dict) else None

    for s in list(sets.values()):
        p = program_of(s) if s.json else None
        for b in (p or {}).get("base", []):
            try:
                bu = uuidmod.UUID(b)
            except ValueError:
                continue
            if bu not in sets and bu in user:
                sets[bu] = user[bu]

    # the objects of the person's that システム links to by a role and that
    # an update adds links to (the system's 原紙箱, "update": "links")
    sysbox = uuidmod.UUID(SYSTEM_BOX)
    if sysbox in sets and sets[sysbox].recs:
        text = open(sets[sysbox].recs[0][1], encoding="utf-8", errors="replace").read()
        for m in re.finditer(r'<link\b[^>]*\bid="([0-9a-f-]{36})_0\.xtad"', text):
            lu = uuidmod.UUID(m.group(1))
            if lu in sets or lu not in user:
                continue
            try:
                d = json.load(open(user[lu].json, encoding="utf-8"))
            except (ValueError, OSError, TypeError):
                continue
            if isinstance(d, dict) and d.get("tessronos", {}).get("update") == "links":
                sets[lu] = user[lu]

    keep_more = set(k.lower() for k in args.keep)
    picked, left = [], []
    for u in sorted(sets, key=str):
        s = sets[u]
        transfer.prepare(s)
        if s.problem:
            left.append((str(u), s.problem))
            continue
        us = str(u)
        d = json.load(open(s.json, encoding="utf-8"))
        how = d.get("tessronos", {}).get("update") if isinstance(d, dict) else None
        if how is not None and how not in UPDATES:
            sys.exit("mktpk: %s says it is updated as %r, not one of %s"
                     % (s.json, how, ", ".join(UPDATES)))
        if us in keep_more:
            how = "keep"
        picked.append((us, s, how))
    return picked, left


def system_manifest(args, picked):
    """What the system is: package.json of its package without the boot
    files, as the record MANIFEST_REC of the system box holds it too"""
    return {"format": FORMAT, "kind": "system", "id": "tessronos", "name": "TessronOS",
            "version": args.version, "machine": args.machine, "arch": args.arch,
            "objects": [us for us, _s, _h in picked],
            "keep": [us for us, _s, h in picked if h == "keep"],
            "links": [us for us, _s, h in picked if h == "links"]}


def make_manifest(args):
    """The list of the system's objects, as the record the system volume
    keeps it in: {SYSTEM_BOX}_{MANIFEST_REC}.bin in the directory given,
    which tools/mktsfs takes with --from"""
    picked, left = system_objects(args)
    man = system_manifest(args, picked)
    os.makedirs(args.manifest, exist_ok=True)
    path = os.path.join(args.manifest, "%s_%d.bin" % (SYSTEM_BOX, MANIFEST_REC))
    data = json.dumps(man, ensure_ascii=False).encode("utf-8")
    with open(path + ".tmp", "wb") as f:
        f.write(data)
    os.replace(path + ".tmp", path)
    for u, why in left:
        sys.stderr.write("mktpk: %s left out: %s\n" % (u, why))
    print("%s: TessronOS %s (%s, %s), %d objects (%d kept, %d boxes added to)"
          % (path, args.version, args.machine, args.arch, len(man["objects"]),
             len(man["keep"]), len(man["links"])))
    return 0                    # what is left out is said; the image goes on without it, as mktsfs does


def make_system(args):
    from tsfs_py import meta as metamod

    picked, left = system_objects(args)
    entries = []
    for us, s, _how in picked:
        entries.append((us + ".json", s.json, RT_SYSDATA, 0))
        if s.icon is not None:
            entries.append((us + ".ico", bytes(s.icon), RT_SYSDATA, 0))
        for pos in range(len(s.recs)):
            kind, path = s.recs[pos]
            r = s.records[pos]
            ext = "xtad" if kind == metamod.REC_XTAD else "bin"
            entries.append(("%s_%d.%s" % (us, pos, ext), path, r["rt"], r.get("sub", 0)))
        for (pos, resno) in sorted(s.res):
            ext, path = s.res[(pos, resno)]
            entries.append((os.path.basename(path), path, RT_SYSDATA, 0))

    boot, bootkeep = [], []
    for path, src in boot_pairs(args.boot, "--boot"):
        boot.append(path)
        entries.append(("boot/" + path, src, RT_SYSDATA, 0))
    for path, src in boot_pairs(args.boot_keep, "--boot-keep"):
        bootkeep.append(path)
        entries.append(("boot/" + path, src, RT_SYSDATA, 0))
    if len(set(boot + bootkeep)) != len(boot + bootkeep):
        sys.exit("mktpk: a boot file is named twice")

    manifest = system_manifest(args, picked)
    manifest["boot"] = boot
    manifest["bootkeep"] = bootkeep
    entries.insert(0, ("package.json",
                       json.dumps(manifest, ensure_ascii=False).encode("utf-8"), RT_SYSDATA, 0))
    size = write_package(args.output, entries)
    for u, why in left:
        sys.stderr.write("mktpk: %s left out: %s\n" % (u, why))
    print("%s: TessronOS %s (%s, %s), %d objects (%d kept, %d boxes added to), "
          "%d boot files, %d bytes"
          % (args.output, args.version, args.machine, args.arch, len(manifest["objects"]),
             len(manifest["keep"]), len(manifest["links"]), len(boot) + len(bootkeep), size))
    return 1 if left else 0


def show(path):
    with open(path, "rb") as f:
        magic, fmt, n, _, _ = struct.unpack("<4sHHII", f.read(HEAD))
        if magic != MAGIC:
            sys.exit("mktpk: %s is not a package" % path)
        table = f.read(ENTRY * n)
        for i in range(n):
            name, off, size, rt, sub = struct.unpack_from("<48sIIII", table, ENTRY * i)
            name = name.rstrip(b"\0").decode("utf-8")
            print("%-44s %10d  type %2d %d" % (name, size, rt, sub))
            if name == "package.json":
                f.seek(off)
                print("  " + f.read(size).decode("utf-8"))


def main():
    p = argparse.ArgumentParser(prog="mktpk")
    p.add_argument("--from", dest="dirs", action="append", default=[])
    p.add_argument("--over", action="append", default=[])
    p.add_argument("--user", action="append", default=[])
    p.add_argument("--exclude", action="append", default=[])
    p.add_argument("--keep", action="append", default=[])
    p.add_argument("--boot", action="append", default=[])
    p.add_argument("--boot-keep", dest="boot_keep", action="append", default=[])
    p.add_argument("--program")
    p.add_argument("--system", action="store_true")
    p.add_argument("--machine")
    p.add_argument("--arch")
    p.add_argument("--version")
    p.add_argument("-o", "--output")
    p.add_argument("--manifest")
    p.add_argument("--list")
    a = p.parse_args()
    if a.list:
        show(a.list)
        return 0
    if a.system and a.manifest:
        if not a.dirs:
            p.error("--system --manifest needs --from")
        return make_manifest(a)
    if a.system:
        if not a.dirs or not a.output:
            p.error("--system needs --from and -o")
        return make_system(a)
    if not a.dirs or not a.program or not a.output:
        p.error("--from, --program and -o are needed")
    make(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
