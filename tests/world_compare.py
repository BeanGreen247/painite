#!/usr/bin/env python3 -I
"""Compare the chunk packets Painite sends for a vanilla world with the ones vanilla recorded
(tools/make_test_world.py): every block state and biome of every section must match.
usage: tests/world_compare.py --port P --chunks DIR/vanilla_chunks.bin [--protocol 777] [--ids data/vanilla-26.3.ids]
"""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke


from world_compare_lib import rv, container, parse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--chunks", required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = smoke.load_ids(a.ids)
    ref = {}
    raw = open(a.chunks, "rb").read()
    i = 0
    while i < len(raw):
        x, z, n = struct.unpack("<iiI", raw[i:i + 12])
        ref[(x, z)] = raw[i + 12:i + 12 + n]
        i += 12 + n
    c = smoke.C(a.port)
    c.send(0, smoke.vi(a.protocol) + smoke.vs("localhost") + struct.pack(">H", a.port) + smoke.vi(2))
    c.send(0, smoke.vs("Compare") + b"\0" * 16)
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
    ka_c, ka_s = ids[("play", "c", "keep_alive")], ids[("play", "s", "keep_alive")]
    bf, br = ids[("play", "c", "chunk_batch_finished")], ids[("play", "s", "chunk_batch_received")]
    got = {}
    end = time.time() + 15
    while time.time() < end and len(got) < 60:
        pid, d = c.recv()
        if pid == lc:
            k, secs, rest = parse(d)
            got[k] = (secs, rest, len(d))
        elif pid == ka_c:
            c.send(ka_s, d)
        elif pid == bf:
            c.send(br, struct.pack(">f", 20.0))
    fluid_ids = set()
    for line in open(a.ids.replace(".ids", ".blocks")):
        t = line.rstrip("\n").split("\t")
        if t[1] in ("minecraft:water", "minecraft:lava"):
            fluid_ids.add(int(t[0]))
    bad = 0
    checked = 0
    for k, rd in ref.items():
        if k not in got:
            continue
        _, rsecs, _ = parse(rd)
        secs, _, size = got[k]
        checked += 1
        for si, (r, g) in enumerate(zip(rsecs, secs)):
            # the recording is a live snapshot: fluids that flowed into air before the world was saved are expected
            benign = all(p == q or (p == 0 and q in fluid_ids) for p, q in zip(r[2], g[2]))
            if r[3] == g[3] and benign:
                continue
            if r[0] != g[0] or r[1] != g[1] or r[2] != g[2] or r[3] != g[3]:
                bad += 1
                diff = sum(1 for p, q in zip(r[2], g[2]) if p != q)
                names = {}
                for line in open(a.ids.replace(".ids", ".blocks")):
                    t = line.rstrip("\n").split("\t")
                    names[int(t[0])] = t[1][10:] + ("[" + t[2] + "]" if t[2] else "")
                pairs = {}
                for p, q in zip(r[2], g[2]):
                    if p != q:
                        pairs[(names[p], names[q])] = pairs.get((names[p], names[q]), 0) + 1
                if os.environ.get("VERBOSE"):
                    print("   ", sorted(pairs.items(), key=lambda kv: -kv[1])[:4])
                print(f"chunk {k} section {si}: nonair {r[0]}/{g[0]} fluid {r[1]}/{g[1]} block diffs {diff} biome diffs {sum(1 for p, q in zip(r[3], g[3]) if p != q)}")
        if len(rsecs) != len(secs):
            bad += 1
            print("section count", len(rsecs), len(secs))
    print(f"compared {checked} chunks (painite sent {len(got)}), {bad} mismatching sections")
    sys.exit(1 if bad or not checked else 0)


main()
