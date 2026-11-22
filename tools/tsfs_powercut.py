#!/usr/bin/env python3
"""The power-cut test of the native store (design 11.14.6).

  tools/tsfs_powercut.py [--runs N] [--seed S] --disk IMAGE -- <qemu arguments...>

The machine is started with a kernel built for it (make KTONLY=tsfscut),
whose work changes the native volume in partition 1 of IMAGE without
end. At a moment chosen at random after the work has begun the machine
is killed, as the power going would stop it. Then fsck.tsfs reads the
image: the journal laid over what is in place, it must find no errors.
fsck.tsfs --fix --relink puts right what a cut may leave -- blocks given
up by a transaction that never became real, objects still to be relinked
or reaped -- and a last fsck.tsfs must find the volume clean.

The volume may be of version 4 or 5 (the kernel's ts_format_blk says
which); the work says it ("PCUT READY v5"), and each run checks that
the image is of that version. On version 5 the work makes the budget of
a transaction small, so that a cut may fall between the transactions of
one operation and leave a spare place for fsck.tsfs to find.

The image is put back as it was before each run, and the copy kept for
that is removed at the end.

Exit status: 0 when every run passed, 1 when one did not.
"""

import argparse
import os
import random
import shutil
import signal
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)


def fsck(image, *opts):
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "fsck.tsfs"),
                        "--partition", "1"] + list(opts) + [image],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    return r.returncode, r.stdout


def version_of(image):
    """The version of the format of the volume in partition 1, or None."""
    from tsfs_py import Volume
    from tsfs_py.image import BlockImage, read_partitions
    for part in read_partitions(image):
        if part.index == 1:
            try:
                with Volume.open(BlockImage(image, offset=part.offset,
                                            length=part.length)) as vol:
                    return vol.sb.version
            except Exception:
                return None
    return None


def one_run(qemu, qargs, image, rng, n):
    """Start, wait for the work, kill at a random moment; the rounds done"""
    p = subprocess.Popen([qemu, "-serial", "stdio"] + qargs + ["-monitor", "none"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    rounds = 0
    ready = None
    version = None
    deadline = time.time() + 600
    cut_at = None
    os.set_blocking(p.stdout.fileno(), False)
    buf = b""
    while time.time() < deadline:
        chunk = p.stdout.read(4096)
        if chunk:
            buf += chunk
            lines = buf.split(b"\n")
            buf = lines[-1]
            for ln in lines[:-1]:
                ln = ln.strip()
                if ln == b"PCUT READY" or ln.startswith(b"PCUT READY "):
                    ready = time.time()
                    cut_at = ready + rng.uniform(1.0, 8.0)
                    tail = ln[len(b"PCUT READY"):].strip()
                    if tail.startswith(b"v"):
                        try:
                            version = int(tail[1:])
                        except ValueError:
                            pass
                elif ln.startswith(b"PCUT "):
                    try:
                        rounds = int(ln.split()[1])
                    except ValueError:
                        pass
        elif p.poll() is not None:
            break
        else:
            time.sleep(0.02)
        if cut_at is not None and time.time() >= cut_at:
            break
    p.send_signal(signal.SIGKILL)
    p.wait()
    if ready is None:
        raise RuntimeError("run %d: the work never started" % n)
    return rounds, version


def main():
    ap = argparse.ArgumentParser(prog="tsfs_powercut")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--disk", required=True, help="the image QEMU is given")
    ap.add_argument("qemu_args", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    qargs = args.qemu_args[1:] if args.qemu_args[:1] == ["--"] else args.qemu_args
    qemu = os.environ.get("QEMU", "qemu-system-aarch64")
    rng = random.Random(args.seed)

    keep = args.disk + ".orig"
    shutil.copyfile(args.disk, keep)
    failed = 0
    try:
        for n in range(1, args.runs + 1):
            shutil.copyfile(keep, args.disk)
            rounds, want = one_run(qemu, qargs, args.disk, rng, n)
            have = version_of(args.disk)
            code, out = fsck(args.disk)
            ok = code == 0
            if want is not None and have != want:
                ok = False
                out += "the work said version %s, the image is of %s\n" % (want, have)
            code2, out2 = fsck(args.disk, "--fix", "--relink")
            code3, out3 = fsck(args.disk)
            ok = ok and code3 == 0
            last = [ln for ln in out.splitlines() if ln.strip()]
            print("PCUT run %d: version %s, cut after %d rounds: %s -- %s"
                  % (n, have, rounds, "ok" if ok else "FAILED",
                     last[-1] if last else ""))
            if not ok:
                failed += 1
                sys.stdout.write(out)
                sys.stdout.write(out3)
    finally:
        shutil.copyfile(keep, args.disk)
        os.remove(keep)
    print("PCUT %d of %d runs passed" % (args.runs - failed, args.runs))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
