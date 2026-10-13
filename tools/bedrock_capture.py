#!/usr/bin/env python3
"""Record the post-login packet sequence of a real Bedrock Dedicated Server (offline, flat world).
usage: tools/bedrock_capture.py HOST PORT OUT [seconds]
OUT ending in .json dumps the raw packet list; otherwise Painite's data file (data/bedrock-<version>.bin) is written.
BDS must run with online-mode=false, transport=raknet, level-type=FLAT. Nothing from the capture is committed.
"""
import json, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bedrockclient as b


def _s(x):
    d = x.encode()
    return b.varuint(len(d)) + d


def rd_varuint(d, i):
    return b.read_varuint(d, i)


def parse_subchunk_entries(pl):
    """Split a SubChunk packet (protocol 2168+ layout, cache off) into {offset(x,y,z): entry bytes after the offset}."""
    i = 1                                    # cache flag
    _, i = rd_varuint(pl, i)                 # dimension (zigzag)
    i += 12                                  # center x, y, z (i32 LE)
    n, i = rd_varuint(pl, i)
    out = {}
    for _ in range(n):
        start = i
        off = tuple(x - 256 if x > 127 else x for x in pl[i:i + 3])
        i += 3
        i += 1                               # result
        if pl[i]:
            ln, j = rd_varuint(pl, i + 1)
            i = j + ln
        else:
            i += 1
        for _ in range(2):                   # height map, render height map
            has = pl[i + 1]
            i += 2
            if has:
                for _ in range(16):
                    ln, i = rd_varuint(pl, i)
                    i += ln
        if pl[i]:
            i += 9
        else:
            i += 1
        out[off] = pl[start + 3:i]
    assert i == len(pl), f"subchunk packet not fully consumed: {i} != {len(pl)}"
    return out


CAPTURE_NAME = "PainiteCapture"


def build(out, rec, protocol):
    """Write Painite's Bedrock data file from a recorded session."""
    def first(pid):
        return next(i for i, (p, _) in enumerate(rec) if p == pid)

    i_info, i_stack, i_npu = first(6), first(7), first(121)
    records = []                                  # (kind, id, payload)
    records.append((1, 6, bytes.fromhex(rec[i_info][1])))
    records.append((2, 7, bytes.fromhex(rec[i_stack][1])))
    for p, h in rec[i_stack + 1:i_npu]:
        records.append((3, p, bytes.fromhex(h)))
    # per-player extras that precede the first mob: structure/entity data, attributes, health
    seen = set()
    for p, h in rec[i_npu:]:
        if p in (123, 39, 314, 29, 42) and p not in seen:
            seen.add(p)
            records.append((4, p, bytes.fromhex(h)))
        if p == 2:
            break
    lc = bytes.fromhex(next(h for p, h in rec if p == 58))
    _, j = rd_varuint(lc, 0)
    _, j = rd_varuint(lc, j)
    records.append((5, 58, lc[j:]))               # LevelChunk: everything after the x, z varints
    sub = next(bytes.fromhex(h) for p, h in rec if p == 174)
    for off, entry in parse_subchunk_entries(sub).items():
        records.append((6, 64 + off[1] - 0, entry))   # id = 64 + sub chunk y offset (request center y is -4)
    name = CAPTURE_NAME.encode()
    records.append((7, 1, name))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"PNBE" + struct.pack("<II", 1, protocol) + struct.pack("<I", len(records)))
        for kind, pid, pl in records:
            f.write(struct.pack("<BHI", kind, pid, len(pl)) + pl)
    print(f"wrote {out}: {len(records)} records, {sum(len(r[2]) for r in records)} bytes")


def main():
    host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 12
    c = b.BedrockClient(host, port)
    c.connect()
    c.request_settings()
    c.login("PainiteCapture", f"{host}:{port}")
    rec = []
    runtime_id = None
    requested = False
    t_end = time.time() + 60
    last = time.time()
    while time.time() < t_end and time.time() - last < secs:
        pk = c.recv_packets(1.0)
        if pk is None:
            continue
        last = time.time()
        for pid, pl in pk:
            rec.append((pid, pl.hex()))
            if pid == 156:
                print('VIOLATION', pl)
            if pid == 3:
                ln, i = b.read_varuint(pl)
                c.start_encryption(pl[i:i + ln].decode())
                c.send_packets([(4, b"")])
            elif pid == 6:
                c.send_packets([(8, b.varuint(2) + _s("downloadingfinished"))])
            elif pid == 7:
                c.send_packets([(8, b.varuint(3) + _s("resourcepackstackfinished"))])
            elif pid == 11:
                _, i = b.read_varuint(pl)          # unique entity id (zigzag varint64)
                runtime_id, i = b.read_varuint(pl, i)
                c.send_packets([(129, b"\x00"), (69, b.varuint(b.zigzag(4)) + bytes([4]))])
            elif pid == 58 and not requested:
                requested = True
                for cx in (0, 1):
                    offs = b''.join(bytes([0, k, 0]) for k in range(24))
                    c.send_packets([(175, b.varuint(b.zigzag(0)) + b.varuint(24) + offs + struct.pack('<iii', cx, -4, 0))])
            elif pid == 2 and len(pl) == 4 and struct.unpack(">i", pl)[0] == 3:  # player spawn
                c.send_packets([(113, b.varuint(runtime_id or 1))])
    if out.endswith(".json"):
        json.dump(rec, open(out, "w"))
        print(f"recorded {len(rec)} packets; runtime id {runtime_id}")
        return
    build(out, rec, c.protocol)


main()
