#!/usr/bin/env python3
"""Pass the kernel's requests to the QEMU monitor while the tests run.

A test that needs something done to the machine from outside -- a USB
device taken out and plugged back in, a key pressed on the emulated
keyboard -- prints a line

    KTEST QMP {"execute": ..., "arguments": {...}}

on the console. This script follows the console log and sends each such
command to QEMU's QMP socket. Nothing is answered back to the kernel: the
test watches for the effect instead (the device gone, the key read).

Usage: qemu_qmp.py <qmp socket, or tcp:HOST:PORT> <console log>
"""

import json
import os
import socket
import sys
import time

MARK = "KTEST QMP "


def connect(path, deadline):
    """A unix socket's path, or tcp:HOST:PORT (a QEMU run from Windows)."""
    while time.time() < deadline:
        try:
            if path.startswith("tcp:"):
                host, port = path[4:].rsplit(":", 1)
                s = socket.create_connection((host, int(port)), timeout=5)
                s.settimeout(None)
            else:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
            return s
        except OSError:
            time.sleep(0.2)
    return None


def main():
    sock_path, log_path = sys.argv[1], sys.argv[2]
    s = connect(sock_path, time.time() + 30)
    if s is None:
        return
    f = s.makefile("rw")
    f.readline()                                # the greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    f.readline()

    while not os.path.exists(log_path):
        time.sleep(0.2)
    pos = 0
    pending = ""
    while True:
        try:
            with open(log_path, "r", errors="replace") as lf:
                lf.seek(pos)
                data = lf.read()
                pos = lf.tell()
        except OSError:
            return
        if data:
            pending += data
            lines = pending.split("\n")
            pending = lines.pop()
            for line in lines:
                i = line.find(MARK)
                if i < 0:
                    continue
                cmd = line[i + len(MARK):].strip()
                try:
                    json.loads(cmd)
                except ValueError:
                    continue
                try:
                    f.write(cmd + "\n")
                    f.flush()
                    # answers and events come back; the answer is the
                    # line with "return" or "error"
                    while True:
                        r = f.readline()
                        if not r:
                            return
                        if '"return"' in r or '"error"' in r:
                            sys.stderr.write("qemu_qmp: %s -> %s" % (cmd, r))
                            break
                except OSError:
                    return
        else:
            time.sleep(0.1)


if __name__ == "__main__":
    main()
