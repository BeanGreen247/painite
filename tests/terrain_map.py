#!/usr/bin/env python3 -I
"""Joins a server, collects the chunks it sends and prints a top-down map (one character per chunk) plus timing.
usage: tests/terrain_map.py --port P [--secs 20]"""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke
from world_compare_lib import parse

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--secs", type=int, default=20)
ap.add_argument("--ids", default="data/vanilla-26.3.ids")
a = ap.parse_args()
ids = smoke.load_ids(a.ids)
names = {}
for line in open(a.ids.replace(".ids", ".blocks")):
    t = line.rstrip("\n").split("\t")
    names[int(t[0])] = t[1][10:]
c = smoke.C(a.port)
c.send(0, smoke.vi(777) + smoke.vs("localhost") + struct.pack(">H", a.port) + smoke.vi(2))
c.send(0, smoke.vs("Mapper") + b"\0" * 16)
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
got = {}
t0 = time.time()
c.s.settimeout(3)
while time.time() - t0 < a.secs:
    try:
        pid, d = c.recv()
    except OSError:
        break
    if pid == ids[("play", "c", "keep_alive")]:
        c.send(ids[("play", "s", "keep_alive")], d)
    elif pid == ids[("play", "c", "chunk_batch_finished")]:
        c.send(ids[("play", "s", "chunk_batch_received")], struct.pack(">f", 60.0))
    elif pid == lc:
        k, secs, _ = parse(d)
        got[k] = secs
print("chunks", len(got), "in %.1fs" % (time.time() - t0))
sym = {"grass_block": ".", "sand": ":", "water": "~", "sandstone": ":", "stone": "#", "snow": "*", "gravel": "g", "dirt": "d", "snow_block": "*", "red_sand": ":"}
skip = ("air", "short_grass", "dandelion", "poppy", "oak_leaves", "oak_log", "snow", "cactus")
xs = sorted({k[0] for k in got})
zs = sorted({k[1] for k in got})
for z in zs:
    row = ""
    for x in xs:
        secs = got.get((x, z))
        ch = " "
        if secs:
            for si in range(len(secs) - 1, -1, -1):
                blk = secs[si][2]
                col = [blk[y * 256 + 8 * 16 + 8] for y in range(15, -1, -1)]
                nm = [names[v] for v in col if v and names[v] not in skip]
                if nm:
                    ch = sym.get(nm[0].split("[")[0], "?")
                    break
        row += ch
    print(row)
