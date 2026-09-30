#!/usr/bin/env python3
"""Tests of what the desktop shows, quickly.

  tools/ui_run.py [--elf E] [--disk D] [--out DIR] [--tag T] [--fresh] STEPS...

Run from build_make after `make TARGET=_QEMU_VIRT_ DESKTOP=1 UITEST=1
BUILD_ID=<id>` and a KTEST build that made the test disk.

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


def machine_args(elf, disk, con_log, ut_port, mon_port, net="socket"):
    return [QEMU, "-M", "virt,gic-version=2", "-cpu", "cortex-a76", "-smp", "4", "-m", "2G",
            "-display", "none", "-no-reboot",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", "file=%s,if=none,format=raw,id=hd0" % disk,
            "-device", "virtio-blk-device,drive=hd0",
            "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd", "-device", "usb-mouse",
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


def wait_for(log, text, tmo):
    end = time.time() + tmo
    while time.time() < end:
        try:
            if text in open(log, encoding="utf-8", errors="replace").read():
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


def main():
    p = argparse.ArgumentParser(prog="ui_run")
    p.add_argument("--elf", default="tessronosdesk.elf")
    p.add_argument("--disk", default="obj/_QEMU_VIRT__ktest/test_disk.img")
    p.add_argument("--out", default=".")
    p.add_argument("--tag", default="ui")
    p.add_argument("--fresh", action="store_true")
    p.add_argument("--net", choices=("socket", "user"), default="socket",
                   help="user: the machine reaches the host and the internet through QEMU")
    p.add_argument("steps", nargs="*")
    a = p.parse_args()

    tmp = os.path.join(tempfile.gettempdir(), "ui_%s" % a.tag)
    os.makedirs(tmp, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    base, state, keyf = [os.path.join(tmp, n) for n in ("base.img", "vm.state", "key")]
    run_img, con = [os.path.join(tmp, n) for n in ("run.img", "console.log")]
    uts, mons = free_port(), free_port()
    key = key_of(a.elf, a.disk) + a.net
    warm = (not a.fresh and os.path.exists(state) and os.path.exists(base)
            and os.path.exists(keyf) and open(keyf).read() == key)
    for f in (con,):
        if os.path.exists(f):
            os.remove(f)
    shutil.copyfile(base if warm else a.disk, run_img)
    args = machine_args(a.elf, run_img, con, uts, mons, a.net)
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
        for path in a.steps:
            for raw in open(path, encoding="utf-8"):
                l = raw.strip()
                if not l or l.startswith("#"):
                    continue
                if l.startswith("shot "):
                    print("shot", shot(mon, a.out, l[5:].strip(), tmp))
                elif l.startswith("expect "):
                    met = wait_for(con, l[7:].strip(), 20)
                    print("expect %s: %s" % (l[7:].strip(), "met" if met else "NOT MET"))
                    ok = ok and met
                else:
                    ans = drv.ask(l)
                    print("%s -> %s" % (l, ans))
                    ok = ok and ans.startswith("ok")
                    if ans == "err no answer" and os.environ.get("UI_DIAG"):
                        for _ in range(2):
                            print(mon.cmd("info cpus"))
                            time.sleep(1)
                        print("shot", shot(mon, a.out, "no_answer", tmp))
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
        if os.path.exists(run_img):
            os.remove(run_img)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
