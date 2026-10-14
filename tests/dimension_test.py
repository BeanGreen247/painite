#!/usr/bin/env python3 -I
"""Dimensions over Java: /execute in <dim> run tp, respawn packet, chunks of the new dimension, nether portal lighting and travel.
usage: tests/dimension_test.py --port P [--protocol 777] [--ids data/vanilla-26.3.ids]   (offline-mode server, Bobby must be an operator, gamemode=creative)
"""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke, game_test
import world_compare_lib as wc

vi, rvi, vs = smoke.vi, smoke.rvi, smoke.vs


class Counting(game_test.Bot):
    """Bot that also counts the chunks it receives and remembers the last position."""
    chunks = 0

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
                self.log.append((nm, d))
            elif nm == "level_chunk_with_light":
                self.chunks += 1
                self.last_chunk = d
            else:
                self.log.append((nm, d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--protocol", type=int, default=777)
    ap.add_argument("--ids", default="data/vanilla-26.3.ids")
    a = ap.parse_args()
    ids = smoke.load_ids(a.ids)
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    B = Counting(a.port, a.protocol, ids, "Dimmy")
    B.pump(2.5)
    check(B.chunks > 10, f"overworld chunks arrive ({B.chunks})")
    B.take("respawn")
    for dim, y, label in (("the_nether", 80, "the Nether"), ("the_end", 70, "the End"), ("overworld", 100, "the overworld")):
        before = B.chunks
        B.c.send(B.S("chat_command"), vs(f"execute in minecraft:{dim} run tp @s 0 {y} 0"))
        B.pump(3.0)
        check(len(B.take("respawn")) == 1, f"respawn packet when entering {label}")
        check(B.chunks - before > 10, f"chunks of {label} arrive ({B.chunks - before})")
        check(B.pos is not None and abs(B.pos[1] - y) < 0.01, f"teleported to y {y} in {label}")
        _, secs, rest = wc.parse(B.last_chunk)
        want = 24 if dim == "overworld" else 16
        check(len(secs) == want, f"a chunk of {label} has {len(secs)} sections (expected {want})")
        check(wc.parse_tail(rest)["exact"], f"the light data of a {label} chunk is well formed")
    B.take("system_chat")
    for kind in ("stronghold", "village"):
        B.c.send(B.S("chat_command"), vs("locate structure " + kind))
        B.pump(1.5)
        sc = B.take("system_chat")
        # an existing (Mojang generated) world has no generator for its overworld, so only generated worlds can answer
        check(any(b"nearest" in d or b"no generated structures" in d for d in sc), f"/locate answers for a {kind}")
    # a nether portal: an obsidian frame in the air, lit with flint and steel, then walked into and back out of
    items = {}
    for line in open(a.ids):
        t = line.split()
        if t and t[0] == "reg" and t[1] == "item":
            items[t[2]] = int(t[3])

    def cmd(text, wait=0.4):
        B.c.send(B.S("chat_command"), vs(text))
        B.pump(wait)

    cmd("tp @s 300 150 298", 2.5)
    B.take("respawn")
    cmd("fill 296 149 296 306 149 306 stone")
    cmd("fill 300 150 300 303 150 300 obsidian")
    cmd("fill 300 154 300 303 154 300 obsidian")
    cmd("fill 300 151 300 300 153 300 obsidian")
    cmd("fill 303 151 300 303 153 300 obsidian")
    cmd("fill 301 151 300 302 153 300 air")
    cmd("tp @s 301.5 150 298.5", 1.0)
    B.c.send(B.S("set_creative_mode_slot"), struct.pack(">h", 36) + vi(1) + vi(items["flint_and_steel"]) + vi(0) + vi(0))
    B.pump(0.3)
    pos = struct.pack(">Q", ((301 & 0x3FFFFFF) << 38) | ((300 & 0x3FFFFFF) << 12) | (150 & 0xFFF))
    B.c.send(B.S("use_item_on"), vi(0) + pos + vi(1) + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + vi(1))
    B.pump(0.5)
    ups = B.take("block_update")
    check(len(ups) >= 6, f"lighting the frame changes its 6 inside blocks ({len(ups)} block updates)")
    B.c.send(B.S("move_player_pos"), struct.pack(">dddB", 301.5, 151.0, 300.5, 1))
    B.pump(2.5)
    check(len(B.take("respawn")) == 1, "walking into the portal enters the Nether")
    check(B.pos is not None and abs(B.pos[0] - 301.5 / 8) < 24 and abs(B.pos[2] - 300.5 / 8) < 24, f"arrived near the scaled position ({B.pos[0]:.1f}, {B.pos[1]:.1f}, {B.pos[2]:.1f})")
    nx, ny, nz = B.pos
    B.c.send(B.S("move_player_pos"), struct.pack(">dddB", nx, ny, nz + 4.0, 1))
    B.pump(1.5)
    B.c.send(B.S("move_player_pos"), struct.pack(">dddB", nx, ny, nz, 1))
    B.pump(2.5)
    check(len(B.take("respawn")) == 1, "walking into the Nether side leads back to the overworld")
    check(B.pos is not None and abs(B.pos[0] - 301.5) < 40 and abs(B.pos[2] - 300.5) < 40, f"arrived near the original portal ({B.pos[0]:.1f}, {B.pos[1]:.1f}, {B.pos[2]:.1f})")
    # an End portal: twelve frames in the air, eyes put in with the held eye of ender, then in and out of the End
    cmd("tp @s 400 150 395", 2.5)
    B.take("respawn")
    cmd("fill 394 149 394 406 149 406 stone")
    for i in range(3):
        cmd(f"setblock {399 + i} 150 398 end_portal_frame[eye=false,facing=south]")
        cmd(f"setblock {399 + i} 150 402 end_portal_frame[eye=false,facing=north]")
        cmd(f"setblock 398 150 {399 + i} end_portal_frame[eye=false,facing=east]")
        cmd(f"setblock 402 150 {399 + i} end_portal_frame[eye=false,facing=west]")
    cmd("tp @s 400.5 150 395.5", 1.0)
    B.take("block_update")
    B.c.send(B.S("set_creative_mode_slot"), struct.pack(">h", 36) + vi(1) + vi(items["ender_eye"]) + vi(0) + vi(0))
    B.pump(0.3)
    frames = [(399 + i, 398) for i in range(3)] + [(399 + i, 402) for i in range(3)] + [(398, 399 + i) for i in range(3)] + [(402, 399 + i) for i in range(3)]
    for fx, fz in frames:
        p2 = struct.pack(">Q", ((fx & 0x3FFFFFF) << 38) | ((fz & 0x3FFFFFF) << 12) | (150 & 0xFFF))
        B.c.send(B.S("use_item_on"), vi(0) + p2 + vi(1) + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + vi(1))
        B.pump(0.12)
    B.pump(0.5)
    ups = B.take("block_update")
    check(len(ups) >= 12 + 9, f"twelve eyes and nine portal blocks ({len(ups)} block updates)")
    B.c.send(B.S("move_player_pos"), struct.pack(">dddB", 400.5, 150.0, 400.5, 1))
    B.pump(2.5)
    check(len(B.take("respawn")) == 1, "the End portal leads to the End")
    check(B.pos is not None and abs(B.pos[0] - 100.5) < 1 and abs(B.pos[1] - 49) < 1, f"arrived on the obsidian platform ({B.pos[0]:.1f}, {B.pos[1]:.1f}, {B.pos[2]:.1f})")
    B.c.send(B.S("chat_command"), vs("execute in minecraft:the_end run tp @s 1.5 66 0.5"))
    B.pump(2.5)
    check(len(B.take("respawn")) == 1, "the exit fountain leads back to the overworld")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
