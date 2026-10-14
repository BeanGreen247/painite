#!/usr/bin/env python3
"""Bedrock dimensions: join, run /execute in the_nether and the_end, expect ChangeDimension (61) and LevelChunk packets that name the new dimension.
usage: tests/bedrock_dim_test.py --port P [--name BedDim]   (the server needs the name in ops.json, offline-mode friendly)
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
    ap.add_argument("--name", default="BedDim")
    a = ap.parse_args()
    ok = True

    def check(c, m):
        nonlocal ok
        print(("PASS " if c else "FAIL ") + m)
        ok = ok and c

    c = b.BedrockClient(a.host, a.port)
    c.connect()
    c.request_settings()
    c.login(a.name, f"{a.host}:{a.port}")
    spawned = False
    runtime = None
    steps = [("the_nether", 1), ("the_end", 2), ("overworld", 0)]
    step = 0
    changed = {}
    dims_in_chunks = {}
    cur_dim = 0
    last_cmd = 0
    t_end = time.time() + 60
    while time.time() < t_end and step <= len(steps):
        pk = c.recv_packets(0.5)
        if pk is not None:
            for pid, pl in pk:
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
                elif pid == 2 and len(pl) == 4 and struct.unpack(">i", pl)[0] == 3:
                    spawned = True
                    c.send_packets([(113, b.varuint(runtime or 1))])
                    last_cmd = time.time() - 1
                elif pid == 61:
                    z, i = b.read_varuint(pl)
                    d = (z >> 1) ^ -(z & 1)
                    changed[d] = changed.get(d, 0) + 1
                    cur_dim = d
                elif pid == 58:
                    # x, z zigzag varints, then the dimension varint
                    _, i = b.read_varuint(pl)
                    _, i = b.read_varuint(pl, i)
                    dim, i = b.read_varuint(pl, i)
                    dims_in_chunks[dim] = dims_in_chunks.get(dim, 0) + 1
                elif pid == 5:
                    print("DISCONNECT", pl)
        if spawned and time.time() - last_cmd > 4 and step < len(steps):
            name, num = steps[step]
            cmd = f"/execute in minecraft:{name} run tp @s 0 80 0"
            c.send_packets([(77, pstr(cmd) + bytes([0]) + b"\x00" * 24)])
            step += 1
            last_cmd = time.time()
        elif spawned and step == len(steps) and time.time() - last_cmd > 4:
            step += 1
    check(spawned, "joined")
    for name, num in steps:
        check(changed.get(num, 0) >= 1, f"ChangeDimension to {name}")
        check(dims_in_chunks.get(num, 0) >= 10, f"LevelChunk packets for {name}: {dims_in_chunks.get(num, 0)}")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
