#!/usr/bin/env python3 -I
"""Scheduled block updates: sand falls, plants lose their ground, tall plants lose their other half; the second bot sees every change.
usage: tests/region_test.py --port P   (offline, "Bobby" is an operator)"""
import argparse, math, os, random, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_test as gt

vi, rvi, vs = gt.vi, gt.rvi, gt.vs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = gt.smoke.load_ids(a.ids)
    names = {}
    for line in open(a.ids.replace(".ids", ".blocks")):
        t = line.rstrip("\n").split("\t")
        names[int(t[0])] = t[1][10:] + ("[" + t[2] + "]" if t[2] else "")
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    B = gt.Bot(a.port, a.protocol, ids, "Bobby")
    W = gt.Bot(a.port, a.protocol, ids, "Watcher")
    B.pump(2)
    W.pump(1)
    bx, by, bz = B.pos
    x = int(math.floor(bx)) + random.randint(3, 6)
    z = int(math.floor(bz)) + random.randint(-3, 3)
    y0 = int(math.floor(by)) - 1          # the ground under the player

    def cmd(text, secs=0.4):
        B.c.send(B.S("chat_command"), vs(text))
        B.pump(secs)
        W.pump(secs)

    def updates():
        got = {}
        for d in W.take("block_update"):
            p = struct.unpack(">Q", d[:8])[0]
            px = p >> 38
            px = px - (1 << 26) if px >= 1 << 25 else px
            pz = (p >> 12) & 0x3FFFFFF
            pz = pz - (1 << 26) if pz >= 1 << 25 else pz
            py = p & 0xFFF
            py = py - 4096 if py >= 2048 else py
            got[(px, py, pz)] = names.get(rvi(d, 8)[0], "?")
        return got

    cmd("fill %d %d %d %d %d %d stone" % (x - 1, y0, z - 1, x + 1, y0, z + 1))   # own floor (the world may have changed since the last run)
    cmd("fill %d %d %d %d %d %d air" % (x, y0 + 1, z, x, y0 + 12, z))   # clear the column
    updates()
    # 1. sand dropped in the air lands on the ground
    cmd("setblock %d %d %d sand" % (x, y0 + 8, z), 1.5)
    u = updates()
    check(u.get((x, y0 + 8, z)) == "air", "sand leaves its starting block: " + str(u.get((x, y0 + 8, z))))
    check(u.get((x, y0 + 1, z)) == "sand", "sand lands on the ground at y=%d: %s" % (y0 + 1, u.get((x, y0 + 1, z))))
    # 2. a flower on the sand pops when the sand goes
    cmd("setblock %d %d %d dandelion" % (x, y0 + 2, z))
    updates()
    cmd("setblock %d %d %d air" % (x, y0 + 1, z), 1.0)
    u = updates()
    check(u.get((x, y0 + 2, z)) == "air", "the flower is removed with its ground: " + str(u.get((x, y0 + 2, z))))
    # 3. tall grass: the top goes when the bottom is removed
    cmd("setblock %d %d %d tall_grass[half=lower]" % (x, y0 + 1, z))
    cmd("setblock %d %d %d tall_grass[half=upper]" % (x, y0 + 2, z), 1.0)
    updates()
    cmd("setblock %d %d %d air" % (x, y0 + 1, z), 1.0)
    u = updates()
    check(u.get((x, y0 + 2, z)) == "air", "the upper half of a tall plant goes with the lower one: " + str(u.get((x, y0 + 2, z))))
    # 4. gravel stack falls together
    cmd("setblock %d %d %d gravel" % (x, y0 + 5, z))
    cmd("setblock %d %d %d gravel" % (x, y0 + 6, z), 2.0)
    u = updates()
    check(u.get((x, y0 + 1, z)) == "gravel" and u.get((x, y0 + 2, z)) == "gravel", "two gravel blocks fall and stack: %s %s" % (u.get((x, y0 + 1, z)), u.get((x, y0 + 2, z))))
    cmd("fill %d %d %d %d %d %d air" % (x, y0 + 1, z, x, y0 + 12, z))
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
