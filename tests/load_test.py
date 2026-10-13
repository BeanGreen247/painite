#!/usr/bin/env python3 -I
"""Many Java bots join, wander, build and chat for a while; reports server CPU, memory and tick time.
usage: tests/load_test.py --port P --pid SERVER_PID [--bots 40] [--secs 20]   (offline mode; "Bobby" in ops.json for the /tps readout)
"""
import argparse, math, os, random, struct, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_test as gt

vi, rvi, vs = gt.vi, gt.rvi, gt.vs


def cpu_secs(pid):
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")


def thread_cpu(pid):
    out = {}
    for t in os.listdir(f"/proc/{pid}/task"):
        try:
            f = open(f"/proc/{pid}/task/{t}/stat").read().rsplit(")", 1)
            comm = f[0].split("(", 1)[1]
            r = f[1].split()
            out[t] = (comm, (int(r[11]) + int(r[12])) / os.sysconf("SC_CLK_TCK"))
        except OSError:
            pass
    return out


def rss_mb(pid):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS"):
            return int(line.split()[1]) / 1024


def bot(port, proto, ids, name, secs, stats, items):
    try:
        b = gt.Bot(port, proto, ids, name)
        b.pump(1.5)
        x, y, z = b.pos
        end = time.time() + secs
        ang = random.random() * 6.28
        n = 0
        b.c.send(b.S("set_creative_mode_slot"), struct.pack(">h", 36) + vi(64) + vi(items["stone"]) + vi(0) + vi(0))
        while time.time() < end:
            ang += random.uniform(-0.3, 0.3)
            x += math.cos(ang) * 0.25
            z += math.sin(ang) * 0.25
            b.c.send(b.S("move_player_pos_rot"), struct.pack(">dddffB", x, y, z, math.degrees(ang), 0.0, 1))
            b.c.send(b.S("client_tick_end"))
            n += 1
            if n % 40 == 0:
                b.c.send(b.S("use_item_on"), vi(0) + gt.block_pos(int(x) + 1, int(y) - 1, int(z)) + vi(1) + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + vi(n))
            if n % 100 == 0:
                b.c.send(b.S("chat"), vs("hello " + name) + struct.pack(">qq", int(time.time() * 1000), 0) + b"\x00" + vi(0) + b"\x00\x00\x00" + b"\x00")
            b.pump(0.05)
        stats["ok"] += 1
        stats["chunks"] += sum(1 for _ in [])
    except Exception as e:
        stats["err"].append(repr(e))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--bots", type=int, default=40)
    ap.add_argument("--secs", type=int, default=20)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = gt.smoke.load_ids(a.ids)
    items = {}
    for line in open(a.ids):
        t = line.split()
        if t and t[0] == "reg" and t[1] == "item":
            items[t[2]] = int(t[3])
    stats = {"ok": 0, "err": [], "chunks": 0}
    c0, t0 = cpu_secs(a.pid), time.time()
    ths = [threading.Thread(target=bot, args=(a.port, a.protocol, ids, f"Load{i:03d}", a.secs, stats, items)) for i in range(a.bots)]
    for i, t in enumerate(ths):
        t.start()
        time.sleep(0.15)
    time.sleep(3)
    cj, tj = cpu_secs(a.pid), time.time()
    th_j = thread_cpu(a.pid)
    print(f"join phase: {cj - c0:.1f}s CPU in {tj - t0:.1f}s")
    op = gt.Bot(a.port, a.protocol, ids, "Bobby")
    time.sleep(a.secs * 0.6)
    op.pump(1)
    op.c.send(op.S("chat_command"), vs("tps"))
    op.pump(1.0)
    tps = [d for n, d in op.log if n == "system_chat"]
    for t in ths:
        t.join()
    c1, t1 = cpu_secs(a.pid), time.time()
    th_e = thread_cpu(a.pid)
    print("per thread in the steady phase:", ", ".join(f"{th_e[t][0]}#{t}={th_e[t][1] - th_j.get(t, ('', 0))[1]:.1f}s" for t in th_e if th_e[t][1] - th_j.get(t, ('', 0))[1] > 0.05))
    print(f"steady phase: {c1 - cj:.1f}s CPU in {t1 - tj:.1f}s = {100 * (c1 - cj) / (t1 - tj):.0f}% of one core")
    print(f"{a.bots} bots, {a.secs}s of movement: {stats['ok']} finished, {len(stats['err'])} errors {stats['err'][:2]}")
    print(f"server CPU {(c1 - c0):.1f}s over {(t1 - t0):.1f}s = {100 * (c1 - c0) / (t1 - t0):.0f}% of one core, RSS {rss_mb(a.pid):.0f} MB")
    for d in tps[-1:]:
        print("tps readout:", d[d.find(b"TPS"):d.find(b"TPS") + 100].decode(errors="replace"))
    sys.exit(0 if stats["ok"] == a.bots else 1)


main()
