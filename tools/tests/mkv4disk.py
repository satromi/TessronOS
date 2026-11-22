#!/usr/bin/env python3
"""A copy of a build's test disk whose system volume is of version 4
(design 7.3, 更新と登録の流れ), for the tests of what a start does with
a disk of R1.410 and before: the conversion at start
(tools/tests/ui_tsfs_convert_boot.steps) and the system put over a kept
volume of version 4 (tests/ktest/x86_64/ktest_sysinst.c).

    cd build_make
    python3 ../tools/tests/mkv4disk.py OUT [make arguments ...]
    python3 ../tools/tests/mkv4disk.py --volume [--size BYTES] OUT [make arguments ...]

The make arguments are those the disk was built with (TARGET=, KTEST=,
DESKTOP=, UITEST=, BUILD_ID= ...). The disk is the build's test disk
(test_disk.img on QEMU virt, sd.img on x64), which must have been made.
The system volume is made again by the build's own mktsfs command, as
make would run it (make -n), with --format 4 and the size of the disk's
system partition, and written over that partition of the copy; nothing
of the build is changed. OUT is best on a disk of the host's own (WSL's
~/), as the copy is as large as the disk.

With --volume, OUT is the system volume of version 4 alone, of the size
of the disk's system partition or of --size BYTES (a multiple of 4096),
for a disk an earlier system was put on (tools/tests/mkwindisk.py
make-ts4).
"""

import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)

from tsfs_py.layout import MAGIC, SB_LABEL, SB_VERSION    # noqa: E402

SECT = 512
SYS_LABEL = b"TessronOS system"


def make_var(margs, name):
    """A variable of the makefile, as the build with these arguments has it."""
    out = subprocess.run(
        ["make", "--no-print-directory"] + margs
        + ["--eval=mkv4_show: ; @echo $(%s)" % name, "mkv4_show"],
        check=True, stdout=subprocess.PIPE, universal_newlines=True).stdout
    return out.strip().splitlines()[-1].strip()


def mktsfs_command(margs, sysvol):
    """The command make runs to make the system volume, as words."""
    out = subprocess.run(
        ["make", "--no-print-directory", "-n", "-W", "../tools/mktsfs"] + margs + [sysvol],
        check=True, stdout=subprocess.PIPE, universal_newlines=True).stdout
    text = out.replace("\\\n", " ")
    for line in text.splitlines():
        words = shlex.split(line)
        if any(w.endswith("tools/mktsfs") for w in words) and words[-1] == sysvol:
            return words
    raise SystemExit("mkv4disk: make does not say how it makes %s" % sysvol)


def system_partition(disk):
    """(first byte, bytes) of the partition holding the system volume."""
    with open(disk, "rb") as f:
        f.seek(SECT)
        hdr = f.read(SECT)
        if hdr[0:8] != b"EFI PART":
            raise SystemExit("mkv4disk: %s has no GPT" % disk)
        ent_lba, nent, esz = struct.unpack_from("<QII", hdr, 72)
        f.seek(ent_lba * SECT)
        ents = f.read(nent * esz)
        for i in range(nent):
            e = ents[i * esz:(i + 1) * esz]
            if e[0:16] == b"\0" * 16:
                continue
            first, last = struct.unpack_from("<QQ", e, 32)
            f.seek(first * SECT)
            sb = f.read(4096)
            if sb[0:len(MAGIC)] == MAGIC and sb[SB_LABEL:SB_LABEL + len(SYS_LABEL)] == SYS_LABEL:
                return first * SECT, (last - first + 1) * SECT
    raise SystemExit("mkv4disk: no system volume on %s" % disk)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    args = sys.argv[1:]
    only_vol = False
    want = None
    while args and args[0].startswith("--"):
        if args[0] == "--volume":
            only_vol = True
            args = args[1:]
        elif args[0] == "--size" and len(args) > 1:
            want = int(args[1])
            args = args[2:]
        else:
            sys.stderr.write(__doc__)
            return 2
    if not args or (want is not None and (not only_vol or want <= 0 or want % 4096)):
        sys.stderr.write(__doc__)
        return 2
    out, margs = args[0], args[1:]
    target = next((a.split("=", 1)[1] for a in margs if a.startswith("TARGET=")), "_QEMU_VIRT_")
    disk = make_var(margs, "SD_IMG" if target == "_X64_" else "TEST_DISK")
    sysvol = make_var(margs, "SYS_VOL")
    if not os.path.exists(disk):
        raise SystemExit("mkv4disk: %s is not made" % disk)
    cmd = mktsfs_command(margs, sysvol)
    off, size = system_partition(disk)
    if want is not None:
        size = want

    tmp = tempfile.mkdtemp(prefix="mkv4disk.")
    try:
        vol = os.path.join(tmp, "sysvol4.img")
        words = list(cmd[:-1])
        if "--size" in words:
            k = words.index("--size")
            del words[k:k + 2]
        words += ["--format", "4", "--size", str(size), vol]
        env = dict(os.environ)
        env.pop("TSFS_FORMAT", None)
        r = subprocess.run(words, env=env)
        if r.returncode not in (0, 1) or not os.path.exists(vol):
            raise SystemExit("mkv4disk: mktsfs failed (%d)" % r.returncode)
        with open(vol, "rb") as f:
            head = f.read(4096)
        if struct.unpack_from("<I", head, SB_VERSION)[0] != 4:
            raise SystemExit("mkv4disk: mktsfs did not make version 4")
        if os.path.getsize(vol) > size:
            raise SystemExit("mkv4disk: the volume is larger than the partition")
        if only_vol:
            shutil.move(vol, out)
            print("mkv4disk: %s: the system volume of %s of version 4 (%d MB)" % (out, disk, size >> 20))
            return 0
        shutil.copyfile(disk, out)
        with open(vol, "rb") as src, open(out, "r+b") as dst:
            dst.seek(off)
            shutil.copyfileobj(src, dst, 1 << 20)
        print("mkv4disk: %s: %s with a system volume of version 4 (%d MB at %d)"
              % (out, disk, size >> 20, off))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
