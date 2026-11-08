#!/usr/bin/env python3
"""Tests of what the desktop shows, quickly.

  tools/ui_run.py [--elf E] [--disk D] [--out DIR] [--tag T] [--fresh] [--wav F]
                  [--keep-disk F] [--cdrom ISO] STEPS...

Run from build_make after `make TARGET=_QEMU_VIRT_ DESKTOP=1 UITEST=1
BUILD_ID=<id>` and a KTEST build that made the test disk. --wav F adds
a USB sound device whose output QEMU writes to the WAVE file F, so
that what a test sounded can be looked at afterwards. --keep-disk F
copies the disk as the run left it to F, so that what the run wrote
(a file printed, an object saved) can be read afterwards
(tools/tsfs-export --partition 3 F); with --vols, the images of DIR
are replaced by the ones the run wrote as well. --cdrom ISO puts the image in a
USB CD-ROM drive (blocks of 2048 bytes), as tools/tests/mkbtcd.py makes
one for ファイル変換. --vols DIR gives the machine a USB hub with
nothing on it, and copies the disk images of DIR (tools/tests/
mkvoldisk.py) beside the run, where a step plugs them in.

The first run starts the machine, waits until the desktop says it is
up, and keeps the machine as it is then (its memory, and a copy of the
disk made at the same moment) under ui_<tag> in the temporary directory. Every later run with
the same kernel and disk starts from that, in a few seconds rather than
the minutes a start takes. --fresh starts it again.

The desktop is driven over the second serial port (dtuitest.c): each
line of a steps file is sent as it is and the answer waited for, so no
step waits longer than it must. The pointer goes straight to the place
given; windows can be found by their names ("win 管理情報" answers
where it is). A few lines are the runner's own:

  shot NAME        the screen, as NAME.png (or .ppm) in --out
  expect TEXT      the console must say TEXT (waits up to 20 seconds)
  expectin S TEXT  the same, waiting up to S seconds
  logmark          where the console's log is now: what expectsince looks after
  expectsince TEXT the console must say TEXT after logmark (waits up to 20
                   seconds): a line it said before as well (a state passed
                   through on the way) does not count
  expectsincein S TEXT  the same, waiting up to S seconds
  hostmark         the time from which hosttime counts, on this machine's clock
  hosttime NAME MS how long since hostmark, printed with NAME; at most MS
  want ANSWER      the answer to the step before must be ANSWER ("want ok c 1,4 a 1,4")
  keep NAME        the answer to the step before kept under NAME
  same NAME        the answer to the step before must be the one kept under NAME
  under NAME MS    the answer to the step before ("ok MS") must be at most MS;
                   it is printed with NAME either way, for the record of times
  pixel X Y RRGGBB the screen must be that colour at X, Y now (what is drawn
                   checked, not only what the console says)
  pixelnot X Y RRGGBB  it must not be
  absent TEXT      the console must not have said TEXT so far in this run
                   ("absent !OACV!": no call of a program refused for want
                   of a right, which a UITEST kernel tells)
  hmp COMMAND      a command to the QEMU monitor, $VOLS standing for where
                   the copies of the --vols images are (drive_add, device_add
                   usb-storage on the hub and device_del plug disks in and out)
                   and $DISK for the copy of the disk the machine runs on
  # ...            a comment

Exit status: 0 when every step was answered ok and every expect was
met, 1 otherwise, 2 when the machine did not come up.
"""

import argparse
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import time

import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_steps  # noqa: E402

# On Windows (with --net user: the WSL build of QEMU has no user network) the
# Windows QEMU is run by the Windows Python; either way the monitor and the
# second serial port are TCP ports on this machine.
QEMU = os.environ.get("QEMU", r"C:\Program Files\qemu\qemu-system-aarch64.exe" if os.name == "nt"
                      else os.path.expanduser("~/.local/xPacks/qemu-arm/bin/qemu-system-aarch64"))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def machine_args(elf, disk, con_log, ut_port, mon_port, net="socket", wav=None, cdrom=None, vols=False):
    snd = []
    if vols:
        # a hub the disks of a test are plugged into
        snd += ["-device", "usb-hub,bus=xhci.0,id=hubv"]
    if wav:
        # a USB sound device whose output is written to a WAVE file
        snd = ["-audiodev", "wav,id=snd0,path=%s" % wav, "-device", "usb-audio,bus=xhci.0,audiodev=snd0"]
    if cdrom:
        # a USB CD-ROM drive: a drive of media=cdrom makes the usb-storage answer as one
        snd += ["-drive", "file=%s,if=none,id=btcd,format=raw,media=cdrom,readonly=on" % cdrom,
                "-device", "usb-storage,bus=xhci.0,drive=btcd,removable=on"]
    return [QEMU, "-M", "virt,gic-version=2", "-cpu", "cortex-a76", "-smp", "4", "-m", "2G",
            "-display", "none", "-no-reboot",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", "file=%s,if=none,format=raw,id=hd0" % disk,
            "-device", "virtio-blk-device,drive=hd0",
            "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd", "-device", "usb-mouse"] + snd + [
            "-device", "bochs-display",
            "-netdev", "user,id=n0" if net == "user" else "socket,id=n0,mcast=230.0.0.1:12347",
            "-device", "virtio-net-device,netdev=n0",
            "-serial", "file:%s" % con_log,
            "-chardev", "socket,id=ut,host=127.0.0.1,port=%d,server=on,wait=off" % ut_port,
            "-serial", "chardev:ut",
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % mon_port,
            "-kernel", elf]


class Monitor:
    def __init__(self, path):
        self.s = connect(path)
        self.read_prompt()

    def read_prompt(self, tmo=60):
        buf = b""
        end = time.time() + tmo
        self.s.settimeout(1)
        while b"(qemu)" not in buf and time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                buf += d
            except socket.timeout:
                pass
        return buf.decode("utf-8", "replace")

    def cmd(self, line, tmo=60):
        self.s.sendall((line + "\n").encode())
        return self.read_prompt(tmo)


def connect(port, tmo=30):
    end = time.time() + tmo
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.2)


class Driver:
    """Lines sent on the second serial port; the answers read from the
    console's log, where each is marked "@ut " """

    def __init__(self, path, log):
        self.s = connect(path)
        self.log = log
        self.at = os.path.getsize(log) if os.path.exists(log) else 0
        self.lines = []

    def line(self, tmo):
        end = time.time() + tmo
        while not self.lines:
            if time.time() > end:
                return None
            try:
                with open(self.log, "rb") as f:
                    f.seek(self.at)
                    d = f.read()
            except OSError:
                d = b""
            k = d.rfind(b"\n")
            if k < 0:
                time.sleep(0.02)
                continue
            self.at += k + 1
            for l in d[:k].split(b"\n"):
                i = l.find(b"@ut ")
                if i >= 0 and not l[i + 4:].startswith(b">"):
                    self.lines.append(l[i + 4:].decode("utf-8", "replace").strip())
        return self.lines.pop(0)

    def ask(self, text, tmo=30):
        self.s.sendall((text + "\n").encode("utf-8"))
        while True:
            l = self.line(tmo)
            if l is None:
                return "err no answer"
            if l.startswith("ok") or l.startswith("err"):
                break
        out = [l]
        if text.strip() == "wins" and l.startswith("ok "):
            for _ in range(int(l.split()[1])):
                out.append(self.line(tmo) or "")
        return "\n".join(out)


def wait_for(log, text, tmo, since=0):
    end = time.time() + tmo
    while time.time() < end:
        try:
            with open(log, "rb") as f:
                f.seek(since)
                if text in f.read().decode("utf-8", "replace"):
                    return True
        except OSError:
            pass
        time.sleep(0.2)
    return False


def shot(mon, out, name, tmp):
    ppm = os.path.join(tmp, name + ".ppm")
    mon.cmd("screendump " + ppm)
    for _ in range(50):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.1)
    try:
        from PIL import Image
        dst = os.path.join(out, name + ".png")
        Image.open(ppm).save(dst)
        os.remove(ppm)
    except Exception:
        dst = os.path.join(out, name + ".ppm")
        shutil.copyfile(ppm, dst)
        os.remove(ppm)
    return dst


def save_state(mon, state):
    """The machine's state sent over TCP into a file (the Windows QEMU cannot
    migrate to a file and read one back the same way)"""
    import threading
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    done = []

    def take():
        c, _ = srv.accept()
        with open(state, "wb") as f:
            while True:
                d = c.recv(1 << 20)
                if not d:
                    break
                f.write(d)
        c.close()
        done.append(True)
    t = threading.Thread(target=take, daemon=True)
    t.start()
    mon.cmd("migrate tcp:127.0.0.1:%d" % port, 120)
    for _ in range(1200):
        if done and "completed" in mon.cmd("info migrate"):
            break
        time.sleep(0.1)
    srv.close()


def feed_state(port, state):
    """The kept state given to a machine waiting for it on a port"""
    import threading

    def give():
        c = connect(port, 60)
        with open(state, "rb") as f:
            while True:
                d = f.read(1 << 20)
                if not d:
                    break
                c.sendall(d)
        c.close()
    threading.Thread(target=give, daemon=True).start()


def key_of(elf, disk):
    h = hashlib.sha1()
    for p in (elf, disk):
        st = os.stat(p)
        h.update(("%s %d %d;" % (os.path.abspath(p), st.st_size, int(st.st_mtime))).encode())
    return h.hexdigest()


class QemuMachine:
    """What ui_steps.run_steps needs, on QEMU"""

    def __init__(self, mon, drv, con, out, tmp, vdir, run_img):
        self.mon, self.drv, self.con = mon, drv, con
        self.out, self.tmp, self.vdir, self.run_img = out, tmp, vdir, run_img

    def ask(self, text, tmo):
        return self.drv.ask(text, tmo)

    def shot(self, name):
        return shot(self.mon, self.out, name, self.tmp)

    def pixel(self, x, y):
        ppm = os.path.join(self.tmp, "pixel.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.mon.cmd("screendump " + ppm)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.1)
        try:
            from PIL import Image
            with Image.open(ppm) as im:
                r, g, b = im.convert("RGB").getpixel((x, y))[:3]
            os.remove(ppm)
            return "%02X%02X%02X" % (r, g, b)
        except Exception:
            return None

    def hmp(self, cmd):
        cmd = cmd.replace("$VOLS", self.vdir.replace("\\", "/")).replace("$DISK", self.run_img.replace("\\", "/"))
        out = self.mon.cmd(cmd)
        # the monitor echoes what is typed with its editing codes: the answer is the last line
        lines = [x.strip() for x in out.replace("(qemu)", "").split("\n")]
        lines = [x for x in lines if x and "\x1b" not in x]
        return "hmp %s -> %s" % (cmd, lines[-1] if lines else "ok")

    def log_at(self):
        return os.path.getsize(self.con) if os.path.exists(self.con) else 0

    def wait_for(self, text, tmo, since):
        return wait_for(self.con, text, tmo, since)

    def log_lines(self):
        return open(self.con, encoding="utf-8", errors="replace").read().splitlines()

    def no_answer(self):
        if not os.environ.get("UI_DIAG"):
            return False
        for _ in range(2):
            print(self.mon.cmd("info cpus"))
            time.sleep(1)
        print("shot", self.shot("no_answer"))
        return True


def main():
    p = argparse.ArgumentParser(prog="ui_run")
    p.add_argument("--elf", default="tessronosdesk.elf")
    p.add_argument("--disk", default="obj/_QEMU_VIRT__ktest/test_disk.img")
    p.add_argument("--out", default=".")
    p.add_argument("--tag", default="ui")
    p.add_argument("--fresh", action="store_true")
    p.add_argument("--net", choices=("socket", "user"), default="socket",
                   help="user: the machine reaches the host and the internet through QEMU")
    p.add_argument("--wav", help="a USB sound device; what it plays is written to this WAVE file")
    p.add_argument("--keep-disk", help="the disk as the run left it, copied to this file")
    p.add_argument("--cdrom", help="an image put in a USB CD-ROM drive")
    p.add_argument("--vols", help="a directory of disk images a step plugs in (hmp)")
    p.add_argument("steps", nargs="*")
    a = p.parse_args()

    tmp = os.path.join(tempfile.gettempdir(), "ui_%s" % a.tag)
    os.makedirs(tmp, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    base, state, keyf = [os.path.join(tmp, n) for n in ("base.img", "vm.state", "key")]
    run_img, con = [os.path.join(tmp, n) for n in ("run.img", "console.log")]
    uts, mons = free_port(), free_port()
    key = key_of(a.elf, a.disk) + a.net + ("wav" if a.wav else "") + (key_of(a.cdrom, a.cdrom) if a.cdrom else "")         + ("vols" if a.vols else "")
    vdir = os.path.join(tmp, "vols")
    if a.vols:
        # the images as they were made, every run: a run writes to them
        os.makedirs(vdir, exist_ok=True)
        for n in os.listdir(a.vols):
            if n.endswith(".img"):
                shutil.copyfile(os.path.join(a.vols, n), os.path.join(vdir, n))
    warm = (not a.fresh and os.path.exists(state) and os.path.exists(base)
            and os.path.exists(keyf) and open(keyf).read() == key)
    for f in (con,):
        if os.path.exists(f):
            os.remove(f)
    shutil.copyfile(base if warm else a.disk, run_img)
    args = machine_args(a.elf, run_img, con, uts, mons, a.net, a.wav, a.cdrom, bool(a.vols))
    if warm:
        inport = free_port()
        args += ["-incoming", "tcp:127.0.0.1:%d" % inport]
    t0 = time.time()
    q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if warm:
        feed_state(inport, state)
    ok = True
    mon = None
    try:
        for _ in range(10):
            if q.poll() is not None:
                sys.stderr.write("ui_run: QEMU ended: %s\n" % q.stderr.read().decode("utf-8", "replace"))
                return 2
            time.sleep(0.1)
        mon = Monitor(mons)
        drv = Driver(uts, con)
        if not warm:
            if not wait_for(con, "uitest: ready", 300):
                sys.stderr.write("ui_run: the desktop did not come up\n")
                return 2
            drv.ask("wait 1500")
            # kept as it is now: the memory, and the disk at the same moment
            mon.cmd("stop")
            save_state(mon, state)
            shutil.copyfile(run_img, base)
            open(keyf, "w").write(key)
            mon.cmd("cont")
        if warm:
            # the state was kept stopped: go on once it is loaded
            for _ in range(300):
                st = mon.cmd("info status")
                if "running" in st:
                    break
                if "paused" in st or "postmigrate" in st:
                    mon.cmd("cont")
                time.sleep(0.1)
        if drv.ask("ready", 60) != "ok":
            sys.stderr.write("ui_run: no answer from the desktop\n")
            return 2
        print("ui_run: up in %.1f s (%s)" % (time.time() - t0, "kept state" if warm else "started"))
        mach = QemuMachine(mon, drv, con, a.out, tmp, vdir, run_img)
        ok, stopped = ui_steps.run_steps(a.steps, mach)
        if stopped:
            return 1
        print("ui_run: done in %.1f s" % (time.time() - t0))
    finally:
        try:
            if mon is not None:
                mon.s.sendall(b"quit\n")
        except Exception:
            pass
        try:
            q.wait(10)
        except subprocess.TimeoutExpired:
            q.kill()
        if a.keep_disk and os.path.exists(run_img):
            shutil.copyfile(run_img, a.keep_disk)
        if os.path.exists(run_img):
            os.remove(run_img)
        if a.vols and os.path.isdir(vdir):
            if a.keep_disk:
                # the disks as the run left them too, for a run that starts from the disk kept
                for n in os.listdir(vdir):
                    if n.endswith(".img"):
                        shutil.copyfile(os.path.join(vdir, n), os.path.join(a.vols, n))
            shutil.rmtree(vdir, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
