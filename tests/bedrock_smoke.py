#!/usr/bin/env python3
"""Bedrock smoke test: RakNet session, settings, login, encryption, resource pack handshake, StartGame replay,
chunk streaming, sub chunk request and chat. Works against Painite (and, with data capture, against BDS).
usage: tests/bedrock_smoke.py [--port 25565] [--no-chunks]
"""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import bedrockclient as b


def pstr(x):
    d = x.encode()
    return b.varuint(len(d)) + d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=25565)
    ap.add_argument("--name", default="BedrockSmoke")
    a = ap.parse_args()
    ok = True

    def check(c, m):
        nonlocal ok
        print(("PASS " if c else "FAIL ") + m)
        ok = ok and c

    c = b.BedrockClient(a.host, a.port)
    c.connect()
    check(True, f"RakNet session established (mtu {c.mtu})")
    c.request_settings()
    check(c.compress, f"network settings: zlib threshold {c.threshold}")
    c.login(a.name, f"{a.host}:{a.port}")
    seen = {}
    runtime = None
    chunks = 0
    spawned = False
    sub = None
    chat = False
    sent_chat = False
    t_end = time.time() + 40
    asked_sub = False
    while time.time() < t_end and not (chunks > 0 and spawned and sub and chat):
        pk = c.recv_packets(1.0)
        if pk is None:
            continue
        for pid, pl in pk:
            seen[pid] = seen.get(pid, 0) + 1
            if pid == 3:
                ln, i = b.read_varuint(pl)
                c.start_encryption(pl[i:i + ln].decode())
                c.send_packets([(4, b"")])
            elif pid == 6:
                c.send_packets([(8, b.varuint(2) + pstr("downloadingfinished"))])
            elif pid == 7:
                c.send_packets([(8, b.varuint(3) + pstr("resourcepackstackfinished"))])
            elif pid == 11:
                _, i = b.read_varuint(pl)
                runtime, i = b.read_varuint(pl, i)
                c.send_packets([(129, b"\x00"), (69, b.varuint(b.zigzag(4)) + bytes([4]))])
            elif pid == 58:
                chunks += 1
                if not asked_sub:
                    asked_sub = True
                    zx, i = b.read_varuint(pl)
                    zz, i = b.read_varuint(pl, i)
                    cx = (zx >> 1) ^ -(zx & 1)
                    cz = (zz >> 1) ^ -(zz & 1)
                    offs = b"".join(bytes([0, k, 0]) for k in range(24))
                    c.send_packets([(175, b.varuint(0) + b.varuint(24) + offs + struct.pack("<iii", cx, -4, cz))])
            elif pid == 174:
                sub = pl
            elif pid == 2 and len(pl) == 4 and struct.unpack(">i", pl)[0] == 3:
                spawned = True
                c.send_packets([(113, b.varuint(runtime or 1))])
                time.sleep(0.2)
                c.send_packets([(9, bytes([0, 1, 1]) + pstr(a.name) + pstr("hello from bedrock") + pstr("") + pstr("") + b"\x00")])
                sent_chat = True
            elif pid == 9 and b"hello from bedrock" in pl:
                chat = True
            elif pid == 5:
                print("DISCONNECT", pl)
    check(11 in seen, "StartGame replay received")
    check(seen.get(63, 0) >= 1, "player list received (patched with our name)")
    check(chunks >= 40, f"LevelChunk packets: {chunks}")
    check(sub is not None and len(sub) > 1000, f"SubChunk response: {len(sub) if sub else 0} bytes")
    if sub:
        i = 1
        _, i = b.read_varuint(sub, i)
        i += 12
        n, i = b.read_varuint(sub, i)
        good = air = bad = 0
        for _ in range(n):
            i += 3
            res = sub[i]
            i += 1
            pres = sub[i]
            i += 1
            if pres:
                ln, i = b.read_varuint(sub, i)
                data = sub[i:i + ln]
                i += ln
                if res == 1 and data[0] == 9 and data[1] in (1, 2):
                    good += 1
                else:
                    bad += 1
            else:
                air += 1
            ht = sub[i]
            i += 1
            if ht == 1:
                i += 1
                for _r in range(16):
                    ln, i = b.read_varuint(sub, i)
                    i += ln
            else:
                i += 1
            rt = sub[i]
            i += 1
            i += 1 if rt != 1 else 1
            i += 1
        check(i == len(sub) and bad == 0 and good > 0, f"SubChunk entries parse: {good} with blocks, {air} empty, {bad} bad, consumed {i}/{len(sub)}")
    check(spawned, "PlayStatus player_spawn")
    check(chat, "chat echoed back to the Bedrock client")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
