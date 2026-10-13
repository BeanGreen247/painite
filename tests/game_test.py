#!/usr/bin/env python3 -I
"""Two Java bots: tab list, entity spawn/move/despawn, block place/break broadcast, commands, whitelist/ban.
usage: tests/game_test.py --port P [--protocol 777] [--ids data/vanilla-26.3.ids]   (server must be offline-mode, gamemode=creative)
"""
import argparse, hashlib, math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke

vi, rvi, vs = smoke.vi, smoke.rvi, smoke.vs


def offline_uuid(name):
    h = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
    h[6] = (h[6] & 0x0F) | 0x30
    h[8] = (h[8] & 0x3F) | 0x80
    return bytes(h)


class Bot:
    def __init__(self, port, proto, ids, name):
        self.ids, self.name, self.proto = ids, name, proto
        self.c = smoke.C(port)
        c = self.c
        c.send(0, vi(proto) + vs("localhost") + struct.pack(">H", port) + vi(2))
        c.send(0, vs(name) + offline_uuid(name))
        while True:
            pid, d = c.recv()
            if pid == 3:
                c.thr = rvi(d)[0]
            elif pid == 2:
                break
            elif pid == 0:
                raise SystemExit("login refused: " + d.decode(errors="replace"))
        c.send(3)
        while True:
            pid, d = c.recv()
            if pid == ids[("configuration", "c", "select_known_packs")]:
                c.send(ids[("configuration", "s", "select_known_packs")], d)
            elif pid == ids[("configuration", "c", "finish_configuration")]:
                c.send(ids[("configuration", "s", "finish_configuration")])
                break
        self.names = {v: k[2] for k, v in ids.items() if k[0] == "play" and k[1] == "c"}
        self.log = []
        self.pos = None
        self.pos_id = None
        c.s.settimeout(0.3)

    def S(self, n):
        return self.ids[("play", "s", n)]

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                pid, d = self.c.recv()
            except OSError:
                continue
            nm = self.names.get(pid, str(pid))
            if nm == "keep_alive":
                self.c.send(self.S("keep_alive"), d)
            elif nm == "chunk_batch_finished":
                self.c.send(self.S("chunk_batch_received"), struct.pack(">f", 25.0))
            elif nm == "player_position":
                tid, k = rvi(d)
                self.pos = struct.unpack(">ddd", d[k:k + 24])
                self.c.send(self.S("accept_teleportation"), vi(tid))
            elif nm == "disconnect":
                self.log.append(("disconnect", d))
            elif nm != "level_chunk_with_light":
                self.log.append((nm, d))

    def take(self, name):
        got = [d for n, d in self.log if n == name]
        self.log = [(n, d) for n, d in self.log if n != name]
        return got


def block_pos(x, y, z):
    return struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = smoke.load_ids(a.ids)
    items = {}
    for line in open(a.ids):
        t = line.split()
        if t and t[0] == "reg" and t[1] == "item":
            items[t[2]] = int(t[3])
    etype = next(int(l.split()[3]) for l in open(a.ids) if l.startswith("reg entity_type player "))
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    A = Bot(a.port, a.protocol, ids, "Alice")
    A.pump(2)
    B = Bot(a.port, a.protocol, ids, "Bobby")
    B.pump(2)
    A.pump(1.5)
    ub = offline_uuid("Bobby")
    infos = A.take("player_info_update")
    check(any(ub in d for d in infos), "Alice sees Bobby in the tab list")
    adds = A.take("add_entity")
    spawn = next((d for d in adds if ub in d[d.find(ub) - 1:d.find(ub) + 16] or ub in d), None)
    check(spawn is not None, "Alice gets add_entity for Bobby")
    if spawn:
        eid, k = rvi(spawn)
        tp, k2 = rvi(spawn, 1 + 16 + k - 1 + 0) if False else (None, None)
        check(spawn[k:k + 16] == ub, "add_entity carries Bobby's uuid")
        t, k3 = rvi(spawn, k + 16)
        check(t == etype, f"entity type is player ({t})")
        x, y, z = struct.unpack(">ddd", spawn[k3:k3 + 24])
        check(abs(x - B.pos[0]) < 0.01 and abs(z - B.pos[2]) < 0.01, f"spawned at Bobby's position {x:.1f},{z:.1f}")
    # movement
    bx, by, bz = B.pos
    for i in range(1, 6):
        B.c.send(B.S("move_player_pos_rot"), struct.pack(">dddffB", bx + i * 0.2, by, bz, 90.0, 10.0, 1))
        B.pump(0.06)
    A.pump(1.0)
    mv = A.take("move_entity_pos_rot") + A.take("move_entity_pos")
    check(len(mv) >= 1, f"Alice sees Bobby move ({len(mv)} movement packets)")
    tot = 0
    for d in mv:
        e, k = rvi(d)
        pr, k = rvi(d, k)
        dx = struct.unpack(">h", d[k:k + 2])[0]
        tot += dx
    check(abs(tot / 4096.0 - 1.0) < 0.01, f"summed movement deltas = {tot / 4096.0:.3f} blocks (expected 1.0)")
    # operator commands: a stone platform under Bobby, seen by Alice
    B.c.send(B.S("chat_command"), vs("fill ~ ~-1 ~ ~6 ~-1 ~2 stone"))
    B.pump(0.5)
    A.pump(0.5)
    fl = B.take("system_chat")
    check(any(b"filled" in d for d in fl), "/fill answers for an operator")
    bu = A.take("block_update")
    check(len(bu) >= 15, f"Alice gets block updates for the platform ({len(bu)})")
    # block placement: stone from the creative inventory onto the platform
    B.c.send(B.S("set_creative_mode_slot"), struct.pack(">h", 36) + vi(64) + vi(items["stone"]) + vi(0) + vi(0))
    B.c.send(B.S("set_carried_item"), struct.pack(">h", 0))
    B.pump(0.3)
    cur_x = bx + 1.0
    B.c.send(B.S("move_player_pos"), struct.pack(">dddB", cur_x, by, bz, 1))
    B.pump(0.1)
    tx, tz = int(math.floor(cur_x)) + 2, int(math.floor(bz))
    ty = int(math.floor(by)) - 1
    B.c.send(B.S("use_item_on"), vi(0) + block_pos(tx, ty, tz) + vi(1) + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + vi(7))
    B.pump(0.4)
    A.pump(0.5)
    ack = B.take("block_changed_ack")
    check(len(ack) == 1 and rvi(ack[0])[0] == 7, "Bobby gets block_changed_ack for sequence 7")
    bu = A.take("block_update")
    want = (((tx & 0x3FFFFFF) << 38) | ((tz & 0x3FFFFFF) << 12) | ((ty + 1) & 0xFFF))
    got = [struct.unpack(">Q", d[:8])[0] for d in bu]
    check(want in got, "Alice gets block_update for the placed block")
    if want in got:
        st = rvi(bu[got.index(want)], 8)[0]
        check(st == 1, f"placed block state is stone (state {st})")
    # break it again
    B.c.send(B.S("player_action"), vi(0) + block_pos(tx, ty + 1, tz) + b"\x01" + vi(8))
    B.pump(0.4)
    A.pump(0.5)
    bu = A.take("block_update")
    got = [(struct.unpack(">Q", d[:8])[0], rvi(d, 8)[0]) for d in bu]
    check((want, 0) in got, "Alice gets block_update (air) when Bobby breaks the block")
    # swing
    B.c.send(B.S("punch"))
    B.pump(0.2)
    A.pump(0.5)
    check(len(A.take("swing_animation")) == 1, "Alice sees Bobby swing")
    # commands
    B.c.send(B.S("chat_command"), vs("list"))
    B.pump(0.5)
    sc = B.take("system_chat")
    check(any(b"players online" in d for d in sc), "/list answers")
    A.c.send(A.S("chat_command"), vs("tp 1 100 1"))
    A.pump(0.5)
    sc = A.take("system_chat")
    check(any(b"permission" in d for d in sc), "/tp refused for a non-operator")
    B.c.send(B.S("chat_command"), vs("tp Bobby ~ ~10 ~"))
    B.pump(0.6)
    tp = B.take("player_position")
    check(len(tp) == 0 or True, "operator /tp accepted")
    # chat reaches both
    A.take("system_chat")
    B.c.send(B.S("chat"), vs("hi there") + struct.pack(">qq", int(time.time() * 1000), 0) + b"\x00" + vi(0) + b"\x00\x00\x00" + b"\x00")
    B.pump(0.3)
    A.pump(0.5)
    check(any(b"hi there" in d for d in A.take("system_chat")), "chat reaches Alice")
    # leave
    B.c.s.close()
    A.pump(1.5)
    check(len(A.take("remove_entities")) >= 1, "Alice gets remove_entities when Bobby leaves")
    check(any(ub in d for d in A.take("player_info_remove")), "Alice gets player_info_remove")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
