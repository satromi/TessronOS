"""The PC's side of the test agent (application/desktop/dttagent.c, design 12.4.1).

TLS 1.2 with the shared key, Python 3.13 or later (ssl.SSLContext.
set_psk_client_callback). Where the targets and their keys are kept is told in
tessron_mcp.py; nothing here connects until asked to.
"""

import ipaddress
import json
import os
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ui_steps  # noqa: E402

DEFAULT_PORT = 7878
IDENTITY = b"tessron-test"
CIPHER = "PSK-AES128-GCM-SHA256"
KEY_BYTES = 32


class AgentError(Exception):
    pass


def home():
    return os.environ.get("TESSRON_AGENT_DIR") or os.path.join(os.path.expanduser("~"), ".tessronos", "testagent")


def targets_path():
    return os.path.join(home(), "targets.json")


def load_targets():
    try:
        with open(targets_path(), encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def token_path(name):
    return os.path.join(home(), name + ".token")


def check_lan(host):
    """The port is for one's own LAN: a public address is refused before anything is sent"""
    try:
        addr = ipaddress.ip_address(socket.gethostbyname(host))
    except (OSError, ValueError) as e:
        raise AgentError("the address of %s: %s" % (host, e))
    if not (addr.is_private or addr.is_loopback or addr.is_link_local):
        raise AgentError("%s is not on a private network: refused" % addr)


def read_token(path):
    with open(path, encoding="ascii") as f:
        text = f.read().strip()
    try:
        key = bytes.fromhex(text)
    except ValueError:
        raise AgentError("%s is not hex digits" % path)
    if not 32 <= len(key) <= 64:
        raise AgentError("%s: the key must be 32 to 64 bytes" % path)
    return key


def write_private(path, text):
    """A file only this user reads: the mode on POSIX, the access list on Windows"""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(text)
    if os.name == "nt":
        subprocess.run(["icacls", path, "/inheritance:r", "/grant:r", "%s:F" % os.environ.get("USERNAME", "")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def new_target(name, host, port=DEFAULT_PORT, serial=None):
    """A new key made and kept, and the target written down. Returns the word for cmdline.txt"""
    os.makedirs(home(), exist_ok=True)
    check_lan(host)
    token = secrets.token_hex(KEY_BYTES)
    write_private(token_path(name), token + "\n")
    t = load_targets()
    t[name] = {"host": host, "port": port}
    if serial:
        t[name]["serial"] = serial
    with open(targets_path(), "w", encoding="utf-8") as f:
        json.dump(t, f, indent=1)
    return "testagent=" + token


def png_of(w, h, rgb):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        rows += rgb[y * w * 3:(y + 1) * w * 3]
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


class Client:
    def __init__(self, host, port, key, tmo=15):
        check_lan(host)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        ctx.set_ciphers(CIPHER)
        ctx.set_psk_client_callback(lambda hint: (IDENTITY.decode(), key))
        raw = socket.create_connection((host, port), timeout=tmo)
        try:
            self.s = ctx.wrap_socket(raw)
        except Exception:
            raw.close()
            raise
        self.buf = b""

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _line(self, tmo):
        self.s.settimeout(tmo)
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise AgentError("the connection was closed")
            self.buf += d
        l, self.buf = self.buf.split(b"\n", 1)
        return l.decode("utf-8", "replace").strip()

    def _bytes(self, n, tmo):
        self.s.settimeout(tmo)
        parts = [self.buf[:n]]
        got = len(parts[0])
        self.buf = self.buf[n:]
        while got < n:
            d = self.s.recv(min(1 << 20, n - got))
            if not d:
                raise AgentError("the connection was closed")
            parts.append(d)
            got += len(d)
        return b"".join(parts)

    def _send(self, text):
        self.s.sendall((text + "\n").encode("utf-8"))

    def ask(self, text, tmo=30):
        """As ui_run.py's Driver: the first ok or err, and the lines of a wins"""
        self._send(text)
        try:
            l = self._line(tmo)
            while not (l.startswith("ok") or l.startswith("err")):
                l = self._line(tmo)
            out = [l]
            if text.strip() == "wins" and l.startswith("ok "):
                for _ in range(int(l.split()[1])):
                    out.append(self._line(tmo))
        except (socket.timeout, TimeoutError):
            return "err no answer"
        return "\n".join(out)

    def shot(self, tmo=120):
        self._send("shot")
        a = self._line(tmo)
        if not a.startswith("ok "):
            raise AgentError(a)
        _, w, h, n = a.split()
        return int(w), int(h), self._bytes(int(n), tmo)

    def log(self, pos=0, tmo=30):
        """Bytes, the place after them, and where the log ends"""
        self._send("log %d" % pos)
        a = self._line(tmo)
        if not a.startswith("ok "):
            raise AgentError(a)
        _, n, nxt, end = a.split()
        return self._bytes(int(n), tmo), int(nxt), int(end)

    def log_all(self, pos=0):
        out = b""
        while True:
            d, pos, end = self.log(pos)
            out += d
            if not d or pos >= end:
                return out, pos

    def put(self, path, tmo=120):
        size = os.path.getsize(path)
        self._send("put %d" % size)
        self.s.settimeout(tmo)
        with open(path, "rb") as f:
            while True:
                d = f.read(1 << 16)
                if not d:
                    break
                self.s.sendall(d)
        a = self._line(max(tmo, 600))
        if not a.startswith("ok "):
            raise AgentError(a)
        return a.split()[1]


class AgentMachine:
    """What ui_steps.run_steps needs, over the agent"""

    def __init__(self, cli, out):
        self.c = cli
        self.out = out
        self.text = b""
        self.pos = 0

    def ask(self, text, tmo):
        return self.c.ask(text, tmo)

    def shot(self, name):
        w, h, rgb = self.c.shot()
        os.makedirs(self.out, exist_ok=True)
        path = os.path.join(self.out, name + ".png")
        with open(path, "wb") as f:
            f.write(png_of(w, h, rgb))
        return path

    def hmp(self, cmd):
        raise AgentError("hmp is QEMU's monitor: a real machine has none (%s)" % cmd)

    def _read(self):
        d, self.pos = self.c.log_all(self.pos)
        self.text += d

    def log_at(self):
        self._read()
        return len(self.text)

    def wait_for(self, text, tmo, since):
        end = time.time() + tmo
        want = text.encode("utf-8")
        while True:
            self._read()
            if want in self.text[since:]:
                return True
            if time.time() >= end:
                return False
            time.sleep(0.2)

    def log_lines(self):
        self._read()
        return self.text.decode("utf-8", "replace").splitlines()

    def no_answer(self):
        return True


def run_steps(cli, paths, out, emit=print):
    return ui_steps.run_steps(paths, AgentMachine(cli, out), emit)


def tpk_manifest(path):
    """package.json of a .tpk, which tells a program from the system (tools/mktpk.py)"""
    with open(path, "rb") as f:
        head = f.read(16)
        if head[:4] != b"TSPK":
            raise AgentError("%s is not a package" % path)
        n = struct.unpack("<H", head[6:8])[0]
        for _ in range(n):
            e = f.read(64)
            if e[:48].split(b"\0")[0] == b"package.json":
                off, size = struct.unpack("<II", e[48:56])
                f.seek(off)
                return json.loads(f.read(size).decode("utf-8"))
    raise AgentError("%s has no package.json" % path)
