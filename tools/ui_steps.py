"""The lines of a steps file (tools/tests/*.steps), for whatever carries them.

tools/ui_run.py runs them on QEMU, over the second serial port and the
monitor; tools/mcp over the network, on a machine that runs the test agent.
Which of the runner's own lines there are is told in ui_run.py. A machine
gives run_steps the following:

  ask(text, tmo)      a line sent; its answer ("ok ...", "err ...")
  shot(name)          the screen kept under the name; the file's path
  hmp(cmd)            a command to the QEMU monitor; what it answered
  log_at()            how many bytes of the console's log there are now
  wait_for(text, tmo, since)  the log says text after since, within tmo seconds
  log_lines()         the lines of the log so far
  no_answer()         a step got no answer: True to stop there
  pixel(x, y)         the colour of the screen there now, "RRGGBB" (may be
                      missing: the pixel lines are then not met)
"""

import time

# What may take long: a large object made or read, a wait for the desktop
LONG_STEPS = ("mkbig", "sum", "sync", "waitwin", "sysup")


def step_timeout(line):
    return 1800 if line.split()[0] == "sysup" else 900 if line.split()[0] in LONG_STEPS else 30


def shot_same(a, b, box):
    """Whether two shots kept are the same, pixel for pixel, in x0 y0 x1 y1; and if not, where"""
    if a is None or b is None:
        return False, "no such shot"
    try:
        from PIL import Image
        with Image.open(a) as ia, Image.open(b) as ib:
            ca = ia.convert("RGB").crop(tuple(box))
            cb = ib.convert("RGB").crop(tuple(box))
            if ca.size != cb.size:
                return False, "sizes differ"
            pa, pb = ca.load(), cb.load()
            bad = [(x + box[0], y + box[1]) for y in range(ca.size[1]) for x in range(ca.size[0])
                   if pa[x, y] != pb[x, y]]
    except Exception as e:
        return False, str(e)
    if not bad:
        return True, ""
    ys = sorted(set(p[1] for p in bad))
    return False, "%d pixels in %d rows, first %s, rows %d..%d" % (len(bad), len(ys), bad[0], ys[0], ys[-1])


def run_steps(paths, m, emit=print):
    """Every line of the files, in order. Returns (ok, stopped)"""
    ok = True
    last = ""
    kept = {}
    shots = {}
    host_mark = time.time()
    log_mark = 0
    for path in paths:
        for raw in open(path, encoding="utf-8"):
            l = raw.strip()
            if not l or l.startswith("#"):
                continue
            if l.startswith("shot "):
                shots[l[5:].strip()] = m.shot(l[5:].strip())
                emit("shot %s" % shots[l[5:].strip()])
            elif l.startswith("shotsame "):
                w = l.split()
                met, why = shot_same(shots.get(w[1]), shots.get(w[2]), [int(v) for v in w[3:7]])
                emit("%s: %s" % (l, "met" if met else "NOT MET (%s)" % why))
                ok = ok and met
            elif l.startswith("hmp "):
                emit(m.hmp(l[4:].strip()))
            elif l.startswith("expectin "):
                secs, text = l[9:].strip().split(" ", 1)
                met = m.wait_for(text.strip(), int(secs), 0)
                emit("expect %s: %s" % (text.strip(), "met" if met else "NOT MET"))
                ok = ok and met
            elif l == "logmark":
                log_mark = m.log_at()
            elif l.startswith("expectsincein "):
                secs, text = l[14:].strip().split(" ", 1)
                met = m.wait_for(text.strip(), int(secs), log_mark)
                emit("expect %s (since the mark): %s" % (text.strip(), "met" if met else "NOT MET"))
                ok = ok and met
            elif l.startswith("expectsince "):
                met = m.wait_for(l[12:].strip(), 20, log_mark)
                emit("expect %s (since the mark): %s" % (l[12:].strip(), "met" if met else "NOT MET"))
                ok = ok and met
            elif l == "hostmark":
                host_mark = time.time()
            elif l.startswith("hosttime "):
                name, lim = l[9:].split()
                ms = int((time.time() - host_mark) * 1000)
                met = ms <= int(lim)
                emit("time %s: %d ms (%s %s)" % (name, ms, "within" if met else "OVER", lim))
                ok = ok and met
            elif l.startswith("expect "):
                met = m.wait_for(l[7:].strip(), 20, 0)
                emit("expect %s: %s" % (l[7:].strip(), "met" if met else "NOT MET"))
                ok = ok and met
            elif l.startswith("keep "):
                kept[l[5:].strip()] = last
                emit("keep %s: %s" % (l[5:].strip(), last))
            elif l.startswith("same "):
                met = kept.get(l[5:].strip()) == last
                emit("same %s: %s" % (l[5:].strip(), "met" if met else "NOT MET (%s, kept %s)" % (last, kept.get(l[5:].strip()))))
                ok = ok and met
            elif l.startswith("under "):
                name, lim = l[6:].split()
                try:
                    ms = int(last.split()[1])
                except (IndexError, ValueError):
                    ms = -1
                met = 0 <= ms <= int(lim)
                emit("time %s: %d ms (%s %s)" % (name, ms, "within" if met else "OVER", lim))
                ok = ok and met
            elif l.startswith("want "):
                met = last == l[5:].strip()
                emit("want %s: %s" % (l[5:].strip(), "met" if met else "NOT MET (%s)" % last))
                ok = ok and met
            elif l.startswith("pixel ") or l.startswith("pixelnot "):
                w = l.split()
                at = getattr(m, "pixel", None)
                got = at(int(w[1]), int(w[2])) if at is not None else None
                want = w[3].upper()
                met = got is not None and ((got == want) == (w[0] == "pixel"))
                emit("%s: %s (%s)" % (l, "met" if met else "NOT MET", got))
                ok = ok and met
            elif l.startswith("absent "):
                said = [x for x in m.log_lines() if l[7:].strip() in x]
                emit("absent %s: %s" % (l[7:].strip(), "met" if not said else "NOT MET"))
                for x in said[:20]:
                    emit("    " + x.strip())
                ok = ok and not said
            else:
                ans = m.ask(l, step_timeout(l))
                last = ans.strip()
                emit("%s -> %s" % (l, ans))
                ok = ok and ans.startswith("ok")
                if ans == "err no answer" and m.no_answer():
                    return False, True
    return ok, False
