#!/usr/bin/env python3 -I
"""Smoke test: status ping, offline login, configuration, play join and chunk receipt.
usage: tests/smoke.py [--port 25570] [--protocol 777]
"""
import argparse, json, socket, struct, sys, time, zlib


def vi(n):
    o = b""
    n &= 0xFFFFFFFF
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            o += bytes([b | 0x80])
        else:
            return o + bytes([b])


def rvi(d, i=0):
    n = s = 0
    while True:
        b = d[i]
        i += 1
        n |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return n, i


def vs(x):
    b = x.encode()
    return vi(len(b)) + b


class C:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.buf = b""
        self.thr = -1

    def need(self, k):
        while len(self.buf) < k:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("closed")
            self.buf += d
        r, self.buf = self.buf[:k], self.buf[k:]
        return r

    def send(self, pid, data=b""):
        p = vi(pid) + data
        if self.thr >= 0:
            p = vi(0) + p
        self.s.sendall(vi(len(p)) + p)

    def recv(self):
        n = s = 0
        while True:
            b = self.need(1)[0]
            n |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                break
        p = self.need(n)
        if self.thr >= 0:
            dl, i = rvi(p)
            p = p[i:] if dl == 0 else zlib.decompress(p[i:])
        pid, i = rvi(p)
        return pid, p[i:]


def load_ids(path):
    ids = {}
    for line in open(path):
        t = line.split()
        if t and t[0] == "id":
            ids[(t[1], t[2], t[3])] = int(t[4])
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=25570)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    ap.add_argument("--name", default="SmokeTest")
    a = ap.parse_args()
    ids = load_ids(a.ids)
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    # status
    c = C(a.port)
    c.send(0, vi(a.protocol) + vs("localhost") + struct.pack(">H", a.port) + vi(1))
    c.send(0)
    pid, d = c.recv()
    n, i = rvi(d)
    st = json.loads(d[i:i + n])
    check(pid == 0 and st["version"]["protocol"] == a.protocol, f"status response {st['version']} players {st['players']}")
    c.send(1, struct.pack(">q", 1234))
    pid, d = c.recv()
    check(pid == 1 and struct.unpack(">q", d)[0] == 1234, "ping/pong")

    # login
    c = C(a.port)
    c.send(0, vi(a.protocol) + vs("localhost") + struct.pack(">H", a.port) + vi(2))
    c.send(0, vs(a.name) + b"\0" * 16)
    comp = False
    while True:
        pid, d = c.recv()
        if pid == 3:
            c.thr = rvi(d)[0]
            comp = True
        elif pid == 2:
            break
        elif pid == 0:
            check(False, "login refused: " + d.decode(errors="replace"))
            sys.exit(1)
    check(len(d) == 16 + 1 + len(a.name) + 1 + 16, f"login success length {len(d)} (compression {'on' if comp else 'off'})")
    c.send(3)
    # configuration
    n_cfg = 0
    while True:
        pid, d = c.recv()
        n_cfg += 1
        if pid == ids[("configuration", "c", "select_known_packs")]:
            c.send(ids[("configuration", "s", "select_known_packs")], d)
        elif pid == ids[("configuration", "c", "finish_configuration")]:
            c.send(ids[("configuration", "s", "finish_configuration")])
            break
    check(n_cfg > 5, f"configuration packets received: {n_cfg}")
    # play
    seen = {}
    t_end = time.time() + 6
    chunk_pkts = 0
    c.s.settimeout(3)
    login_ok = False
    while time.time() < t_end:
        try:
            pid, d = c.recv()
        except socket.timeout:
            break
        seen[pid] = seen.get(pid, 0) + 1
        if pid == ids[("play", "c", "login")]:
            login_ok = True
        if pid == ids[("play", "c", "level_chunk_with_light")]:
            chunk_pkts += 1
        if pid == ids[("play", "c", "chunk_batch_finished")]:
            c.send(ids[("play", "s", "chunk_batch_received")], struct.pack(">f", 20.0))
        if pid == ids[("play", "c", "keep_alive")]:
            c.send(ids[("play", "s", "keep_alive")], d)
        if pid == ids[("play", "c", "player_position")]:
            tid, _ = rvi(d)
            c.send(ids[("play", "s", "accept_teleportation")], vi(tid) + b"\0" * 40)
    check(login_ok, "play login packet")
    check(chunk_pkts >= 100, f"chunks received: {chunk_pkts}")
    # chat
    c.s.settimeout(3)
    c.send(ids[("play", "s", "chat")], vs("hello from smoke test") + struct.pack(">qq", 0, 0) + b"\0" + vi(0) + b"\0" * 4)
    got = False
    t_end = time.time() + 3
    while time.time() < t_end and not got:
        try:
            pid, d = c.recv()
        except socket.timeout:
            break
        if pid == ids[("play", "c", "system_chat")] and b"hello from smoke test" in d:
            got = True
    check(got, "chat echoed back")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
