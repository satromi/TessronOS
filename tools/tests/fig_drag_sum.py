#!/usr/bin/env python3
"""The times of tools/tests/ui_fig_drag_perf.steps, as a table.

  python3 tools/tests/fig_drag_sum.py CONSOLE_LOG

CONSOLE_LOG is the console of the run (ui_run.py keeps it as console.log
under ui_<tag> in the temporary directory). The steps drag in the same
order in each block -- the rectangle, the ellipse and the line drawn, then
a rectangle sized by its handle, each with 60 moves 8 ms apart -- with the desktop's figure editor (lines "desktop: drag moves")
and the process (lines "figure: drag moves") by turns, with few shapes and
with many. Each case is shown with the median of its rounds: the
time one move took to follow and show (follow), from a move to its being
shown (lag avg and max), and from the last move to its being shown (last),
in ms, with the moves told and followed.
"""

import re
import statistics
import sys

CASES = [(what, "60x8ms") for what in ("rect", "ellipse", "line", "size")]
KERNEL = re.compile(r"desktop: drag moves (\d+) of (\d+) follow avg (\d+) max (\d+) us "
                    r"lag avg (\d+) max (\d+) last (\d+) us")
PROC = re.compile(r"figure: drag moves (\d+) waited (\d+) followed (\d+) follow avg (\d+) max (\d+) us "
                  r"lag avg (\d+) max (\d+) last (\d+) us shown (\d+) kpx")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    blocks = []                 # (who, shapes, [rows]) in the order they ran
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        for line in f:
            if "testagent=" in line:
                continue
            m = KERNEL.search(line)
            if m:
                n, put, fa, fm, la, lm, ll = map(int, m.groups())
                row = dict(told=put, got=n, followed=n, follow=fa, lag=la, lagmax=lm, last=ll)
                who = "kernel"
            else:
                m = PROC.search(line)
                if not m:
                    continue
                n, w, fo, fa, fm, la, lm, ll, px = map(int, m.groups())
                row = dict(told=n, got=n, waited=w, followed=fo, follow=fa, lag=la, lagmax=lm, last=ll,
                           kpx=px)
                who = "process"
            if not blocks or blocks[-1][0] != who or len(blocks[-1][2]) == len(CASES):
                blocks.append([who, None, []])
            blocks[-1][2].append(row)
    # the blocks go: the desktop and the process with few shapes, then both with many
    for i, b in enumerate(blocks):
        b[1] = "many" if i % 4 >= 2 else "few"
    table = {}
    for who, size, rows in blocks:
        for k, row in enumerate(rows):
            table.setdefault((size, CASES[k], who), []).append(row)

    def med(rows, key):
        v = [r[key] for r in rows if key in r]
        return statistics.median(v) / 1000.0 if v else float("nan")

    print("%-5s %-8s %-8s | %-35s | %-35s" % ("", "", "", "kernel: follow lag avg/max last (ms)",
                                              "process: follow lag avg/max last (ms)"))
    for size in ("few", "many"):
        for case in CASES:
            out = []
            for who in ("kernel", "process"):
                rows = table.get((size, case, who), [])
                if not rows:
                    out.append("%-35s" % "-")
                    continue
                out.append("%6.0f %6.0f/%6.0f %6.0f  (%dx)" % (med(rows, "follow"),
                                                             med(rows, "lag"), med(rows, "lagmax"),
                                                             med(rows, "last"), len(rows)))
            print("%-5s %-8s %-8s | %-35s | %-35s" % (size, case[0], case[1], out[0], out[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
