#!/usr/bin/env python3
"""An MCP server (stdio) for testing TessronOS on real machines (design 12.4.1).

  python tools/mcp/tessron_mcp.py                 serve MCP on stdin and stdout
  python tools/mcp/tessron_mcp.py --new-target NAME --host ADDR [--port P] [--serial COM3]
                                                  make a key, write the target down, and
                                                  print the word to put in cmdline.txt

The targets are in targets.json and each key in NAME.token, under
~/.tessronos/testagent (TESSRON_AGENT_DIR), outside the repository. The server
connects to nothing until the connect tool names a target. Windows' Python 3.13
or later: the TLS with a shared key needs it. Without the standard library's
anything else: the protocol of MCP is written out here.
"""

import argparse
import base64
import json
import os
import socket
import ssl
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agent_client as ac  # noqa: E402

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PROTOCOL = "2025-06-18"


class State:
    name = None
    target = None
    key = None
    cli = None


S = State()


def drop():
    if S.cli is not None:
        S.cli.close()
    S.cli = None


def text(s, error=False):
    r = {"content": [{"type": "text", "text": s}]}
    if error:
        r["isError"] = True
    return r


def client():
    """The connection, made again if it was lost; only to a target connect has named"""
    if S.target is None:
        raise ac.AgentError("no target: call connect first")
    if S.cli is None:
        S.cli = ac.Client(S.target["host"], S.target.get("port", ac.DEFAULT_PORT), S.key)
    return S.cli


def ask(line, tmo=None):
    a = client().ask(line, tmo if tmo else ac.ui_steps.step_timeout(line))
    return text(a, not a.startswith("ok"))


def t_targets(a):
    t = ac.load_targets()
    return text(json.dumps({k: dict(v, key=os.path.exists(ac.token_path(k))) for k, v in t.items()},
                           ensure_ascii=False, indent=1) + "\ndirectory: " + ac.home())


def t_connect(a):
    t = ac.load_targets().get(a["target"])
    if t is None:
        return text("no target %r in %s" % (a["target"], ac.targets_path()), True)
    drop()
    S.name, S.target, S.key = a["target"], t, ac.read_token(ac.token_path(a["target"]))
    return text("%s: %s" % (S.name, client().ask("hello", 15)))


def t_disconnect(a):
    drop()
    S.name = S.target = S.key = None
    return text("disconnected")


def t_screenshot(a):
    w, h, rgb = client().shot()
    png = ac.png_of(w, h, rgb)
    note = "%d x %d" % (w, h)
    if a.get("path"):
        with open(a["path"], "wb") as f:
            f.write(png)
        note += ", " + a["path"]
    return {"content": [{"type": "image", "data": base64.b64encode(png).decode(), "mimeType": "image/png"},
                        {"type": "text", "text": note}]}


def t_log(a):
    pos = int(a.get("from", 0))
    d, nxt = client().log_all(pos)
    s = d.decode("utf-8", "replace")
    if a.get("grep"):
        s = "\n".join(l for l in s.splitlines() if a["grep"] in l)
    if a.get("tail"):
        s = "\n".join(s.splitlines()[-int(a["tail"]):])
    return text(s + "\n[next from: %d]" % nxt)


def t_key(a):
    if a.get("text") is not None:
        return ask("text " + a["text"])
    return ask("key %s %s" % (a["code"], a.get("mods", "0")))


def t_pointer(a):
    act = a["action"]
    if act in ("down", "up"):
        return ask("%s %d" % (act, a.get("button", 0)))
    if act == "wheel":
        return ask("wheel %d" % a["notches"])
    if act == "drag":
        return ask("drag %d %d %d %d %d" % (a["x"], a["y"], a["x2"], a["y2"], a.get("button", 0)))
    if act == "move" or act == "dclick":
        return ask("%s %d %d" % (act, a["x"], a["y"]))
    return ask("%s %d %d %d" % (act, a["x"], a["y"], a.get("button", 0)))


def t_win_list(a):
    return ask("wins")


def t_win(a):
    return ask("win " + a["title"])


def t_focus(a):
    return ask("focus %s %s" % (a.get("tag", "t"), a["title"]))


def t_step(a):
    return ask(a["line"])


def t_send_steps(a):
    path = a["path"]
    if not os.path.isabs(path):
        path = os.path.join(REPO, "tools", "tests", path)
    out = a.get("out") or os.path.join(ac.home(), "shots")
    lines = []
    ok, stopped = ac.run_steps(client(), [path], out, lines.append)
    lines.append("result: %s" % ("stopped: no answer" if stopped else "ok" if ok else "FAILED"))
    return text("\n".join(lines), not ok)


def power(line):
    a = client().ask(line, 15)
    drop()
    return a


def t_reboot(a):
    r = power("reboot")
    return text(r + " (the session ends and the machine starts again: wait_up)", not r.startswith("ok"))


def t_power_off(a):
    r = power("poweroff")
    return text(r + " (nothing here can turn it on again: that is for a person)", not r.startswith("ok"))


def t_wait_up(a):
    tmo = float(a.get("timeout", 300))
    t0 = time.time()
    if S.target is None:
        raise ac.AgentError("no target: call connect first")
    last = ""
    while time.time() - t0 < tmo:
        drop()
        try:
            r = client().ask("ready", 60)
            if r == "ok":
                return text("up after %.1f s" % (time.time() - t0))
            last = r
        except (OSError, ssl.SSLError, ac.AgentError) as e:
            last = str(e)
        drop()
        time.sleep(3)
    return text("not up after %d s (%s). A machine that stopped in its start needs a person; a Raspberry Pi: serial_log" % (tmo, last), True)


def t_install_tpk(a):
    path = a["path"]
    m = ac.tpk_manifest(path)
    system = m.get("kind") == "system"
    force = bool(a.get("force"))
    uuid = client().put(path)
    if system:
        r = client().ask("sysup %s%s" % (uuid, " force" if force else ""), 1800)
        done = r == "ok"
    else:
        r = client().ask("%s %s" % ("proginstf" if force else "proginst", uuid), 700)
        done = r.startswith("ok ") and not r.split()[1].startswith("-")
    if done:
        client().ask("sync 60", 70)
        rm = client().ask("rmobj " + uuid, 60)
    else:
        rm = "kept"
    note = " The system takes it at the next start: reboot, wait_up." if system and done else ""
    return text("%s %s: %s; the object %s %s.%s" % (m.get("name", "?"), m.get("version", "?"), r, uuid, rm, note), not done)


def t_serial_log(a):
    port = (S.target or {}).get("serial") or a.get("port")
    if not port:
        return text("no serial port: give port or name one in the target", True)
    try:
        import serial
    except ImportError:
        return text("pyserial is not installed", True)
    secs = float(a.get("seconds", 5))
    out = b""
    try:
        with serial.Serial(port, 115200, timeout=0.2) as p:
            t0 = time.time()
            while time.time() - t0 < secs:
                out += p.read(4096)
    except serial.SerialException as e:
        return text("%s: %s (TeraTerm and the like must not hold it)" % (port, e), True)
    return text(out.decode("utf-8", "replace"))


def obj(props=None, req=None):
    return {"type": "object", "properties": props or {}, "required": req or []}


S_STR = {"type": "string"}
S_INT = {"type": "integer"}
S_NUM = {"type": "number"}
S_BOOL = {"type": "boolean"}

TOOLS = {
    "targets": (t_targets, "List the machines written down, and where their keys are kept.", obj()),
    "connect": (t_connect, "Connect to a machine named in targets.json over TLS with its key. Nothing connects before this.",
                obj({"target": S_STR}, ["target"])),
    "disconnect": (t_disconnect, "Close the connection and forget the target.", obj()),
    "send_steps": (t_send_steps, "Run a steps file (a name in tools/tests, or a full path); hmp lines do not work on a real machine. The answers come back; shots go to out.",
                   obj({"path": S_STR, "out": S_STR}, ["path"])),
    "step": (t_step, "One line of dtuitest.c as it is.", obj({"line": S_STR}, ["line"])),
    "screenshot": (t_screenshot, "The screen as a PNG image, pointer included; path also saves it.", obj({"path": S_STR})),
    "log": (t_log, "What the console said (as KERNEL.TXT): from a place, the last tail lines, lines with grep. Answers the next place.",
            obj({"from": S_INT, "tail": S_INT, "grep": S_STR})),
    "key": (t_key, "A key by usage code in hex with modifiers in hex, or text: letters, digits and . - / : typed.",
            obj({"code": S_STR, "mods": S_STR, "text": S_STR})),
    "pointer": (t_pointer, "The pointer: action move, click, dclick, hold, drag (x2, y2), down, up, wheel (notches).",
                obj({"action": {"type": "string", "enum": ["move", "click", "dclick", "hold", "drag", "down", "up", "wheel"]},
                     "x": S_INT, "y": S_INT, "x2": S_INT, "y2": S_INT, "button": S_INT, "notches": S_INT}, ["action"])),
    "win_list": (t_win_list, "Every window shown, nearest first.", obj()),
    "win": (t_win, "Where the window of a title is: L T R B.", obj({"title": S_STR}, ["title"])),
    "focus": (t_focus, "Check that the window of a title has the keys.", obj({"title": S_STR, "tag": S_STR}, ["title"])),
    "reboot": (t_reboot, "End the session, windows saved, and start the machine again.", obj()),
    "power_off": (t_power_off, "End the session and turn the machine off. A person must turn it on again.", obj()),
    "wait_up": (t_wait_up, "Keep trying to connect until the desktop answers, up to timeout seconds (300).", obj({"timeout": S_NUM})),
    "install_tpk": (t_install_tpk, "Send a .tpk from this PC and install it: a program, or the system (then reboot). force replaces whatever version is there.",
                    obj({"path": S_STR, "force": S_BOOL}, ["path"])),
    "serial_log": (t_serial_log, "What a serial console says for some seconds (a Raspberry Pi's on COM3): works when the network does not. No token, no commands: it only reads.",
                   obj({"port": S_STR, "seconds": S_NUM})),
}


def call(name, args):
    if name not in TOOLS:
        return text("no tool " + name, True)
    try:
        return TOOLS[name][0](args)
    except (OSError, ssl.SSLError, socket.timeout) as e:
        drop()
        return text("%s: %s" % (type(e).__name__, e), True)
    except (ac.AgentError, KeyError, ValueError) as e:
        return text("%s: %s" % (type(e).__name__, e), True)


def handle(m):
    meth = m.get("method")
    if meth == "initialize":
        return {"protocolVersion": m.get("params", {}).get("protocolVersion", PROTOCOL),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "tessronos-test", "version": "1.0"}}
    if meth == "ping":
        return {}
    if meth == "tools/list":
        return {"tools": [{"name": k, "description": v[1], "inputSchema": v[2]} for k, v in TOOLS.items()]}
    if meth == "tools/call":
        p = m.get("params", {})
        return call(p.get("name"), p.get("arguments") or {})
    raise LookupError(meth)


def serve():
    out = sys.stdout.buffer
    for raw in sys.stdin.buffer:
        if not raw.strip():
            continue
        try:
            m = json.loads(raw)
        except ValueError:
            continue
        if "id" not in m:
            continue	# a notification: none needs an answer here
        r = {"jsonrpc": "2.0", "id": m["id"]}
        try:
            r["result"] = handle(m)
        except LookupError as e:
            r["error"] = {"code": -32601, "message": "no method %s" % e}
        out.write(json.dumps(r, ensure_ascii=False).encode("utf-8") + b"\n")
        out.flush()
    drop()


def main():
    p = argparse.ArgumentParser(prog="tessron_mcp")
    p.add_argument("--new-target")
    p.add_argument("--host")
    p.add_argument("--port", type=int, default=ac.DEFAULT_PORT)
    p.add_argument("--serial")
    a = p.parse_args()
    if a.new_target:
        if not a.host:
            p.error("--host is needed")
        word = ac.new_target(a.new_target, a.host, a.port, a.serial)
        print("kept: %s\nput this word in cmdline.txt of the test image:\n%s" % (ac.token_path(a.new_target), word))
        return 0
    serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
