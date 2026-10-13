#!/usr/bin/env python3 -I
"""Bans, pardons, whitelist and the server icon, driven by an operator bot and by editing the files next to the server.
usage: tests/lists_test.py --port P --dir SERVER_DIR   (offline mode; ops.json must list "Bobby")
"""
import argparse, json, os, struct, sys, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_test as gt

vi, rvi, vs, smoke = gt.vi, gt.rvi, gt.vs, gt.smoke


def png(w, h):
    raw = b"".join(b"\x00" + bytes([200, 30, 30]) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def status(port, proto):
    c = smoke.C(port)
    c.send(0, vi(proto) + vs("localhost") + struct.pack(">H", port) + vi(1))
    c.send(0)
    pid, d = c.recv()
    n, i = rvi(d)
    c.s.close()
    return json.loads(d[i:i + n])


def try_join(port, proto, ids, name):
    c = smoke.C(port)
    c.send(0, vi(proto) + vs("localhost") + struct.pack(">H", port) + vi(2))
    c.send(0, vs(name) + gt.offline_uuid(name))
    while True:
        pid, d = c.recv()
        if pid == 3:
            c.thr = rvi(d)[0]
        elif pid == 2:
            c.send(3)
            c.s.close()
            return None
        elif pid == 0:
            c.s.close()
            return d.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = smoke.load_ids(a.ids)
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    B = gt.Bot(a.port, a.protocol, ids, "Bobby")
    B.pump(1.5)
    def cmd(text, wait=0.6):
        B.c.send(B.S("chat_command"), vs(text))
        B.pump(wait)
        return b" ".join(B.take("system_chat"))
    check(try_join(a.port, a.protocol, ids, "Victim") is None, "Victim can join")
    out = cmd("ban Victim griefing")
    check(b"Banned Victim" in out, "/ban answers: " + out[-40:].decode(errors="replace"))
    msg = try_join(a.port, a.protocol, ids, "Victim")
    check(msg is not None and "banned" in msg and "griefing" in msg, f"banned Victim is refused with the reason ({msg and msg[-60:]!r})")
    bans = json.load(open(os.path.join(a.dir, "banned-players.json")))
    check(bans and bans[0]["name"] == "Victim" and bans[0]["reason"] == "griefing" and len(bans[0]["uuid"]) == 36, "banned-players.json has the vanilla fields")
    out = cmd("pardon Victim")
    check(b"Unbanned" in out and try_join(a.port, a.protocol, ids, "Victim") is None, "/pardon lets Victim back in")
    out = cmd("whitelist add Victim")
    cmd("whitelist on")
    check(try_join(a.port, a.protocol, ids, "Victim") is None, "whitelisted Victim joins")
    msg = try_join(a.port, a.protocol, ids, "Stranger")
    check(msg is not None and "white-listed" in msg, "a player that is not whitelisted is refused")
    cmd("whitelist off")
    check(try_join(a.port, a.protocol, ids, "Stranger") is None, "whitelist off lets everybody in")
    # files edited by hand are picked up
    open(os.path.join(a.dir, "banned-players.json"), "w").write(json.dumps([{"uuid": "", "name": "Stranger", "created": "x", "source": "test", "expires": "forever", "reason": "by hand"}]))
    time.sleep(4)
    msg = try_join(a.port, a.protocol, ids, "Stranger")
    check(msg is not None and "by hand" in msg, "hand edited banned-players.json is reloaded")
    open(os.path.join(a.dir, "banned-players.json"), "w").write("[]")
    # server icon
    open(os.path.join(a.dir, "server-icon.png"), "wb").write(png(64, 64))
    time.sleep(5)
    st = status(a.port, a.protocol)
    check(st.get("favicon", "").startswith("data:image/png;base64,"), "64x64 server-icon.png appears in the server list without a restart")
    open(os.path.join(a.dir, "server-icon.png"), "wb").write(png(32, 32))
    time.sleep(5)
    check("favicon" not in status(a.port, a.protocol), "a 32x32 icon is ignored")
    os.remove(os.path.join(a.dir, "server-icon.png"))
    time.sleep(5)
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
