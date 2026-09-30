#!/usr/bin/env python3
"""qemu_shot.py — QEMU を起動し、UART に印が出た時点の画面を PNG に落とす

  tools/qemu_shot.py --shot '<印>=<出力.png>' [--shot ...] [-t 秒] -- <qemu の引数...>

印は UART 出力に現れる文字列。その行を読んだ直後に QEMU のモニタへ
screendump を送り、返ってきた PPM を PNG に直す。印を省いたときは
時間切れまで走らせて最後の画面を撮る。UART 出力はそのまま流す。
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def monitor_cmd(sock_path, line, wait=2.0):
    """QEMU のモニタは接続を一つしか受け付けず、切れたあと聞き直さない。
    だから撮る回数だけモニタを開けておき、一つにつき一度だけ使う。"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(sock_path)
    time.sleep(0.2)
    try:
        s.settimeout(0.3)
        s.recv(65536)
    except socket.timeout:
        pass
    s.settimeout(5.0)
    s.sendall((line + "\n").encode())
    end = time.time() + wait
    out = b""
    while time.time() < end:
        try:
            s.settimeout(0.3)
            b = s.recv(65536)
            if not b:
                break
            out += b
        except socket.timeout:
            pass
    s.close()
    return out.decode(errors="replace")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 ppm: %r" % data[:16])
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    rgb = data[i:i + w * h * 3]
    if len(rgb) < w * h * 3:		# 書き終わる前に読んだぶんは黒で埋める
        rgb += b"\x00" * (w * h * 3 - len(rgb))
    return w, h, rgb


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shot", action="append", default=[],
                    help="<印>=<出力.png>、印が空なら終了時に撮る")
    ap.add_argument("-t", "--timeout", type=float, default=480.0)
    ap.add_argument("qemu_args", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    args = a.qemu_args
    if args and args[0] == "--":
        args = args[1:]
    if not args:
        ap.error("qemu の引数がない")

    shots = []
    for s in a.shot:
        mark, _, out = s.partition("=")
        shots.append([mark, os.path.abspath(out), False])
    if not shots:
        ap.error("--shot がない")

    tmp = tempfile.mkdtemp(prefix="qemushot")
    ppm = os.path.join(tmp, "shot.ppm")
    socks = [os.path.join(tmp, "mon")]

    qemu = os.environ.get("QEMU", "qemu-system-aarch64")
    cmd = [qemu] + args + ["-serial", "stdio",
                           "-monitor", "unix:%s,server,nowait" % socks[0]]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, bufsize=1,
                         universal_newlines=True)

    taken = [0]

    def take(shot):
        if os.path.exists(ppm):
            os.remove(ppm)
        sock = socks[0]
        taken[0] += 1
        try:
            monitor_cmd(sock, "screendump %s" % ppm)
        except Exception as e:
            print("qemu_shot: モニタが答えない: %r" % (e,))
            return
        last = -1				# 書き終わるまで大きさが変わる
        for _ in range(40):
            size = os.path.getsize(ppm) if os.path.exists(ppm) else 0
            if size > 0 and size == last:
                break
            last = size
            time.sleep(0.2)
        try:
            w, h, rgb = read_ppm(ppm)
            write_png(shot[1], w, h, rgb)
        except Exception as e:
            print("qemu_shot: 画面が取れない: %r" % (e,))
            return
        print("qemu_shot: %s (%dx%d)" % (shot[1], w, h))
        shot[2] = True

    end = time.time() + a.timeout
    try:
        for line in p.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            for shot in shots:
                if not shot[2] and shot[0] and shot[0] in line:
                    take(shot)
            if time.time() > end:
                break
    except KeyboardInterrupt:
        pass

    for shot in shots:
        if not shot[2] and not shot[0]:
            take(shot)
    for shot in shots:
        if not shot[2]:
            take(shot)

    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


if __name__ == "__main__":
    main()
