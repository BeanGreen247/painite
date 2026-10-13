#!/usr/bin/env python3 -I
"""Chunks of a world that has block entities (made with tools/make_test_world.py --cmd "setblock ... chest" ...) must carry them.
usage: tests/block_entity_test.py --port P [--expect 4] [--ids data/vanilla-26.3.ids]"""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke
from world_compare_lib import rv, parse, skip_nbt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    ap.add_argument("--expect", type=int, default=4)
    a = ap.parse_args()
    ids = smoke.load_ids(a.ids)
    reg = {}
    for line in open(a.ids):
        t = line.split()
        if t and t[0] == "reg" and t[1] == "block_entity_type":
            reg[int(t[3])] = t[2]
    c = smoke.C(a.port)
    c.send(0, smoke.vi(a.protocol) + smoke.vs("localhost") + struct.pack(">H", a.port) + smoke.vi(2))
    c.send(0, smoke.vs("BEChecker") + b"\0" * 16)
    while True:
        pid, d = c.recv()
        if pid == 3:
            c.thr = smoke.rvi(d)[0]
        elif pid == 2:
            break
    c.send(3)
    while True:
        pid, d = c.recv()
        if pid == ids[("configuration", "c", "select_known_packs")]:
            c.send(ids[("configuration", "s", "select_known_packs")], d)
        elif pid == ids[("configuration", "c", "finish_configuration")]:
            c.send(ids[("configuration", "s", "finish_configuration")])
            break
    lc = ids[("play", "c", "level_chunk_with_light")]
    found = []
    end = time.time() + 12
    while time.time() < end:
        pid, d = c.recv()
        if pid == ids[("play", "c", "keep_alive")]:
            c.send(ids[("play", "s", "keep_alive")], d)
        elif pid == ids[("play", "c", "chunk_batch_finished")]:
            c.send(ids[("play", "s", "chunk_batch_received")], struct.pack(">f", 20.0))
        elif pid == lc:
            _, _, rest = parse(d)
            n, i = rv(rest, 0)
            for _k in range(n):
                xz, y = rest[i], struct.unpack(">h", rest[i + 1:i + 3])[0]
                i += 3
                t, i = rv(rest, i)
                j = skip_nbt(rest, i)
                found.append((reg.get(t, t), y, j - i))
                i = j
    print("block entities seen:", sorted(found))
    ok = len(found) >= a.expect and any(f[0] == "chest" for f in found) and any(f[0] == "sign" for f in found)
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
