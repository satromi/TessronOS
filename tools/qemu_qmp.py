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

A kernel built under WSL names its files by WSL's paths (/mnt/c/...).
Run with the Windows Python next to a Windows QEMU, the script gives
them to QEMU as Windows paths (C:/...). A Windows QEMU has no character
device that reads a file either: one that reads /dev/zero becomes a TCP
connection to a server of this script that sends zeros and drops what
it is sent. A Windows QEMU that takes a usb-serial out while its socket
line still brings bytes may crash, so before a device_del the script
ends those lines (QEMU takes the device out when its line ends) and lets
the device_del follow.
"""

import json
import os
import re
import socket
import sys
import threading
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


def zero_server():
    """A TCP port on the loopback that gives zeros to whoever connects, until zero_lines_end."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)

    def serve(c):
        c.setblocking(True)
        try:
            while True:
                c.sendall(bytes(4096))
        except OSError:
            pass

    def accept():
        while True:
            c, _ = srv.accept()
            ZERO_CONNS.append(c)
            threading.Thread(target=serve, args=(c,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return srv.getsockname()[1]


ZERO_PORT = None
ZERO_CONNS = []


def zero_lines_end():
    """The lines of zero_server ended, before a device is taken out."""
    if not ZERO_CONNS:
        return
    while ZERO_CONNS:
        c = ZERO_CONNS.pop()
        try:
            c.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        c.close()
    time.sleep(0.5)


def win_chardev(cmd):
    """chardev-add of a file read from /dev/zero, as a connection to zero_server."""
    global ZERO_PORT
    if os.name != "nt" or '"/dev/zero"' not in cmd:
        return cmd
    c = json.loads(cmd)
    b = c.get("arguments", {}).get("backend", {})
    if c.get("execute") != "chardev-add" or b.get("type") != "file":
        return cmd
    if ZERO_PORT is None:
        ZERO_PORT = zero_server()
    c["arguments"]["backend"] = {"type": "socket", "data": {
        "addr": {"type": "inet", "data": {"host": "127.0.0.1", "port": str(ZERO_PORT)}},
        "server": False}}
    return json.dumps(c)


def win_paths(cmd):
    """/mnt/<drive>/... in the command's strings as <DRIVE>:/... on Windows."""
    if os.name != "nt":
        return cmd
    return re.sub(r'(["= ])/mnt/([a-zA-Z])/', lambda m: '%s%s:/' % (m.group(1), m.group(2).upper()), cmd)


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
                cmd = win_paths(win_chardev(cmd))
                if os.name == "nt" and '"device_del"' in cmd:
                    zero_lines_end()
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
