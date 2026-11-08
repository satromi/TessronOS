#!/usr/bin/env python3
#
# wg_peer.py -- a WireGuard peer written from the protocol's paper, for the
# test of the VPN (make WG=1, tests/ktest/ktest_wg.c): the responder's side of
# the handshake, and an echo of UDP through the tunnel. It shares nothing with
# the driver under test, so a test that passes shows the two speak the same
# protocol. Run where QEMU's user network reaches it (10.0.2.2).
#
#   wg_peer.py PORT [--exit-after-seconds N]
#
# Needs the Python package cryptography.
#
import hashlib
import hmac
import os
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, PrivateFormat, NoEncryption

CONSTRUCTION = b"Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s"
IDENTIFIER = b"WireGuard v1 zx2c4 Jason@zx2c4.com"
LABEL_MAC1 = b"mac1----"

# The test's keys, the same in tests/ktest/ktest_wg.c
PEER_PRIV = bytes(range(0x40, 0x60))
GUEST_PUB_CHECK = None
PSK = bytes(32)


def h(*parts):
    d = hashlib.blake2s()
    for p in parts:
        d.update(p)
    return d.digest()


def hm(key, data):
    return hmac.new(key, data, hashlib.blake2s).digest()


def kdf(c, data, n):
    t0 = hm(c, data)
    out = []
    t = b""
    for i in range(1, n + 1):
        t = hm(t0, t + bytes([i]))
        out.append(t)
    return out


def mac(key, data):
    return hashlib.blake2s(data, digest_size=16, key=key).digest()


def nonce(n):
    return b"\0\0\0\0" + struct.pack("<Q", n)


def pub_of(priv):
    return priv.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def dh(priv, pub):
    return priv.exchange(X25519PublicKey.from_public_bytes(pub))


def cksum(b):
    s = 0
    for i in range(0, len(b) - 1, 2):
        s += (b[i] << 8) | b[i + 1]
    if len(b) & 1:
        s += b[-1] << 8
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def main():
    port = int(sys.argv[1])
    limit = 600
    if "--exit-after-seconds" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--exit-after-seconds") + 1])
    sr = X25519PrivateKey.from_private_bytes(PEER_PRIV)
    sr_pub = pub_of(sr)
    print("wg_peer: public key", sr_pub.hex(), flush=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    sock.settimeout(1)
    print("wg_peer: on 127.0.0.1:%d" % port, flush=True)
    end = time.time() + limit
    send_key = recv_key = None
    their_index = 0
    my_index = 0x1234
    tx = 0
    while time.time() < end:
        try:
            m, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        t = m[0]
        if t == 1 and len(m) == 148:
            if mac(h(LABEL_MAC1, sr_pub), m[:116]) != m[116:132]:
                print("wg_peer: initiation with a wrong mac1", flush=True)
                continue
            their_index = struct.unpack("<I", m[4:8])[0]
            ei, enc_static, enc_ts = m[8:40], m[40:88], m[88:116]
            c = h(CONSTRUCTION)
            hh = h(c, IDENTIFIER)
            hh = h(hh, sr_pub)
            c = kdf(c, ei, 1)[0]
            hh = h(hh, ei)
            c, k = kdf(c, dh(sr, ei), 2)
            si = ChaCha20Poly1305(k).decrypt(nonce(0), enc_static, hh)
            hh = h(hh, enc_static)
            c, k = kdf(c, dh(sr, si), 2)
            ts = ChaCha20Poly1305(k).decrypt(nonce(0), enc_ts, hh)
            hh = h(hh, enc_ts)
            print("wg_peer: handshake from", si.hex(), "timestamp", ts.hex(), flush=True)

            er = X25519PrivateKey.generate()
            er_pub = pub_of(er)
            c = kdf(c, er_pub, 1)[0]
            hh = h(hh, er_pub)
            c = kdf(c, dh(er, ei), 1)[0]
            c = kdf(c, dh(er, si), 1)[0]
            c, tau, k = kdf(c, PSK, 3)
            hh = h(hh, tau)
            empty = ChaCha20Poly1305(k).encrypt(nonce(0), b"", hh)
            hh = h(hh, empty)
            resp = struct.pack("<I", 2) + struct.pack("<II", my_index, their_index) + er_pub + empty
            resp += mac(h(LABEL_MAC1, si), resp) + bytes(16)
            recv_key, send_key = kdf(c, b"", 2)
            tx = 0
            sock.sendto(resp, addr)
        elif t == 4 and recv_key is not None and len(m) >= 32:
            ctr = struct.unpack("<Q", m[8:16])[0]
            try:
                ip = ChaCha20Poly1305(recv_key).decrypt(nonce(ctr), m[16:], b"")
            except Exception:
                print("wg_peer: a data packet that does not open", flush=True)
                continue
            if len(ip) == 0:
                print("wg_peer: keepalive", flush=True)
                continue
            hl = (ip[0] & 15) * 4
            total = struct.unpack(">H", ip[2:4])[0]
            if ip[9] != 17:
                print("wg_peer: IP protocol", ip[9], flush=True)
                continue
            sport, dport = struct.unpack(">HH", ip[hl:hl + 4])
            data = ip[hl + 8:total]
            print("wg_peer: UDP %d->%d: %r" % (sport, dport, data), flush=True)
            reply = b"echo:" + data
            hdr = bytearray(ip[:hl])
            struct.pack_into(">H", hdr, 2, hl + 8 + len(reply))
            hdr[12:16], hdr[16:20] = ip[16:20], ip[12:16]
            hdr[8] = 64
            struct.pack_into(">H", hdr, 10, 0)
            struct.pack_into(">H", hdr, 10, cksum(bytes(hdr)))
            pkt = bytes(hdr) + struct.pack(">HHHH", dport, sport, 8 + len(reply), 0) + reply
            pkt += bytes((16 - len(pkt) % 16) % 16)
            enc = ChaCha20Poly1305(send_key).encrypt(nonce(tx), pkt, b"")
            sock.sendto(struct.pack("<IIQ", 4, their_index, tx) + enc, addr)
            tx += 1
    print("wg_peer: done", flush=True)


if __name__ == "__main__":
    main()
