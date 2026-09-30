#!/usr/bin/env python3
"""What the kernel lock cost, from the output of a run built with
make KLOCKSTAT=1 (design 8.6.2).

  tools/klock_report.py LOG ELF

LOG is what the machine printed; ELF the kernel it ran. The suites are
listed with what the lock cost while each ran, and the places the lock
was taken from with the function and line each address is in.
"""

import re
import subprocess
import sys

ADDR2LINE = "aarch64-none-elf-addr2line"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    log, elf = sys.argv[1], sys.argv[2]
    suites, sites, total = [], [], None
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.match(r"KLOCK suite (\S+) acq (\d+) cont (\d+) wait_us (\d+) hold_us (\d+)", line)
        if m:
            suites.append((m.group(1),) + tuple(int(x) for x in m.groups()[1:]))
            continue
        m = re.match(r"KLOCK site ([0-9a-f]+) acq (\d+) cont (\d+) wait_us (\d+) hold_us (\d+)", line)
        if m:
            sites.append((int(m.group(1), 16),) + tuple(int(x) for x in m.groups()[1:]))
            continue
        m = re.match(r"KLOCK all acq (\d+) cont (\d+) wait_us (\d+) hold_us (\d+)", line)
        if m:
            total = tuple(int(x) for x in m.groups())

    if total:
        acq, cont, wait, hold = total
        print("all: taken %d, waited %d times (%.2f%%), %.1f ms waiting, %.1f ms held"
              % (acq, cont, 100.0 * cont / max(acq, 1), wait / 1000.0, hold / 1000.0))
    print()
    print("%-10s %10s %8s %7s %10s %10s" % ("suite", "taken", "waited", "%", "wait ms", "held ms"))
    for name, acq, cont, wait, hold in sorted(suites, key=lambda s: -s[3]):
        print("%-10s %10d %8d %6.2f%% %10.1f %10.1f"
              % (name, acq, cont, 100.0 * cont / max(acq, 1), wait / 1000.0, hold / 1000.0))

    names = {}
    if sites:
        out = subprocess.run([ADDR2LINE, "-f", "-s", "-e", elf] + ["%x" % s[0] for s in sites],
                             stdout=subprocess.PIPE, universal_newlines=True).stdout.split("\n")
        for i, s in enumerate(sites):
            names[s[0]] = "%s %s" % (out[2 * i], out[2 * i + 1]) if 2 * i + 1 < len(out) else "?"
    print()
    print("%-48s %10s %8s %10s %10s" % ("place", "taken", "waited", "wait ms", "held ms"))
    for pc, acq, cont, wait, hold in sites:
        print("%-48s %10d %8d %10.1f %10.1f" % (names.get(pc, hex(pc))[:48], acq, cont,
                                               wait / 1000.0, hold / 1000.0))


if __name__ == "__main__":
    main()
