#!/usr/bin/env python3 -I
"""Java and Bedrock bots in one server: tab list, spawn, movement, block edits and chat across editions.
usage: tests/crossplay_test.py --port P   (offline mode, creative; ops.json must make "Alice" and ".Bedi" operators for the /fill step)
"""
import argparse, math, os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bedrockclient as bc
import game_test as gt   # reuses the Java bot (its main() is guarded)

vi, rvi, vs = gt.vi, gt.rvi, gt.vs


def pstr(x):
    d = x.encode()
    return bc.varuint(len(d)) + d


class BedBot:
    def __init__(self, port, name):
        self.c = bc.BedrockClient("127.0.0.1", port)
        self.name = name
        self.log = []
        self.rt = None
        self.pos = None
        self.spawned = False
        c = self.c
        c.connect()
        c.request_settings()
        c.login(name, f"127.0.0.1:{port}")

    def pump(self, secs):
        c = self.c
        end = time.time() + secs
        while time.time() < end:
            pk = c.recv_packets(0.25)
            if pk is None:
                continue
            for pid, pl in pk:
                if pid == 3:
                    ln, i = bc.read_varuint(pl)
                    c.start_encryption(pl[i:i + ln].decode())
                    c.send_packets([(4, b"")])
                elif pid == 6:
                    c.send_packets([(8, bc.varuint(2) + pstr("downloadingfinished"))])
                elif pid == 7:
                    c.send_packets([(8, bc.varuint(3) + pstr("resourcepackstackfinished"))])
                elif pid == 11:
                    _, i = bc.read_varuint(pl)
                    self.rt, i = bc.read_varuint(pl, i)
                    _, i = bc.read_varuint(pl, i)
                    self.pos = struct.unpack("<fff", pl[i:i + 12])
                    c.send_packets([(129, b"\x00"), (69, bc.varuint(bc.zigzag(4)) + bytes([4]))])
                elif pid == 45 and len(pl) >= 13 and pl[12] == 1:
                    self.pos = struct.unpack("<fff", pl[:12])
                elif pid == 58:
                    if not hasattr(self, "asked"):
                        self.asked = True
                elif pid == 2 and len(pl) == 4 and struct.unpack(">i", pl)[0] == 3 and not self.spawned:
                    self.spawned = True
                    c.send_packets([(113, bc.varuint(self.rt or 1))])
                elif pid in (12, 14, 19, 21, 9, 44, 63, 10, 5, 156):
                    self.log.append((pid, pl))

    def take(self, pid):
        got = [p for i, p in self.log if i == pid]
        self.log = [(i, p) for i, p in self.log if i != pid]
        return got

    def auth_input(self, x, y, z, yaw, pitch, tick, flags=()):
        d = struct.pack("<ff", pitch, yaw) + struct.pack("<fff", x, y, z) + struct.pack("<ff", 0, 0) + struct.pack("<f", yaw)
        d += bc.varuint(len(flags)) + b"".join(bc.varuint(bc.zigzag(f)) for f in flags)
        d += bc.varuint(1) + bc.varuint(0) + bc.varuint(bc.zigzag(1)) + struct.pack("<ff", 0, 0) + bc.varuint(tick) + struct.pack("<fff", 0, 0, 0)
        d += b"\x00\x00\x00\x00\x00" + struct.pack("<ff", 0, 0) + struct.pack("<fff", 0, 0, 0) + struct.pack("<ff", 0, 0)
        self.c.send_packets([(144, d)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = gt.smoke.load_ids(a.ids)
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    J = gt.Bot(a.port, a.protocol, ids, "Alice")
    J.pump(2)
    bname = "Bed%d" % (int(time.time()) % 1000)
    B = BedBot(a.port, bname)
    B.pump(8)
    J.pump(2)
    B.pump(1)
    check(B.spawned and B.pos is not None, f"Bedrock bot spawned at {B.pos}")
    # Java sees the Bedrock player
    infos = J.take("player_info_update")
    check(any(("." + bname).encode() in d for d in infos), "Java: tab list has the Bedrock player")
    adds = J.take("add_entity")
    check(len(adds) >= 1, "Java: add_entity for the Bedrock player")
    # Bedrock sees the Java player
    pl_list = B.take(63)
    check(any(b"Alice" in p for p in pl_list), "Bedrock: PlayerList has Alice")
    check(any(b"Alice" in p and b"painite-" in p and b"geometry.humanoid.custom" in p for p in pl_list), "Bedrock: Alice has a classic default skin (64x64, standard geometry)")
    ap_ = B.take(12)
    check(len(ap_) >= 1 and b"Alice" in ap_[0], "Bedrock: AddPlayer for Alice")
    # Java moves, Bedrock follows
    jx, jy, jz = J.pos
    for k in range(1, 6):
        J.c.send(J.S("move_player_pos_rot"), struct.pack(">dddffB", jx + k * 0.25, jy, jz, 45.0, 5.0, 1))
        J.pump(0.07)
    B.pump(1.5)
    mp = B.take(19)
    check(len(mp) >= 1, f"Bedrock: MovePlayer packets for Alice ({len(mp)})")
    if mp:
        rid, i = bc.read_varuint(mp[-1])
        x, y, z = struct.unpack("<fff", mp[-1][i:i + 12])
        check(abs(x - (jx + 1.25)) < 0.05 and abs(y - (jy + 1.62)) < 0.05, f"Bedrock sees Alice at {x:.2f},{y:.2f} (Java position {jx + 1.25:.2f},{jy:.2f} + eye height)")
    # Bedrock moves, Java follows
    bx, by, bz = B.pos
    for k in range(1, 6):
        B.auth_input(bx + k * 0.2, by, bz, 90.0, 10.0, k)
        B.pump(0.07)
    J.pump(1.5)
    mv = J.take("move_entity_pos_rot") + J.take("move_entity_pos") + J.take("move_entity_rot")
    check(len(mv) >= 1, f"Java: movement packets for the Bedrock player ({len(mv)})")
    # block edits across editions: the Java operator fills a platform, the Bedrock bot must see UpdateBlock
    B.take(21)
    J.c.send(J.S("chat_command"), vs(f"setblock ~ ~3 ~ {gt.PLATFORM}"))
    J.pump(0.5)
    B.pump(1.0)
    ub = B.take(21)
    check(len(ub) >= 1, f"Bedrock: UpdateBlock after a Java /setblock ({len(ub)})")
    if ub:
        p = ub[0]
        _, i = bc.read_varuint(p)
        _, i = bc.read_varuint(p, i)
        _, i = bc.read_varuint(p, i)
        rid, i = bc.read_varuint(p, i)
        rid = struct.unpack("<i", struct.pack("<I", rid))[0]
        tbl = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "blocks-bedrock.bin"), "rb").read()
        n = struct.unpack("<I", tbl[4:8])[0]
        ids_ = struct.unpack(f"<{n}i", tbl[8:8 + 4 * n])
        names = {}
        for line in open(a.ids.replace(".ids", ".blocks")):
            t = line.rstrip("\n").split("\t")
            names[int(t[0])] = t[1]
        match = [names[k] for k, v in enumerate(ids_) if v == rid]
        check("minecraft:" + gt.PLATFORM in match, f"UpdateBlock runtime id {rid} maps back to {match[:2]}")
    # chat both ways
    J.take("system_chat")
    B.c.send_packets([(9, bytes([0, 1, 1]) + pstr(B.name) + pstr("hi from bedrock") + pstr("") + pstr("") + b"\x00")])
    B.pump(0.5)
    J.pump(1.0)
    check(any(b"hi from bedrock" in d for d in J.take("system_chat")), "Java sees the Bedrock player's chat")
    J.c.send(J.S("chat"), vs("hi from java") + struct.pack(">qq", int(time.time() * 1000), 0) + b"\x00" + vi(0) + b"\x00\x00\x00" + b"\x00")
    J.pump(0.5)
    B.pump(1.0)
    check(any(b"hi from java" in p for p in B.take(9)), "Bedrock sees the Java player's chat")
    # leave
    B.c.send_frame(b"\x15", 3)   # RakNet disconnect notification
    time.sleep(0.2)
    B.c.s.close()
    J.pump(3)
    check(len(J.take("remove_entities")) >= 1, "Java: remove_entities when the Bedrock player leaves")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
