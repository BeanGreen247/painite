#!/usr/bin/env python3 -I
"""Print an NBT file (gzip level.dat / playerdata, or a raw file) as text. usage: nbtdump.py FILE [maxlen]"""
import gzip, struct, sys


def rd(d, i, t, depth, ml):
    if t == 1: return struct.unpack(">b", d[i:i+1])[0], i + 1
    if t == 2: return struct.unpack(">h", d[i:i+2])[0], i + 2
    if t == 3: return struct.unpack(">i", d[i:i+4])[0], i + 4
    if t == 4: return struct.unpack(">q", d[i:i+8])[0], i + 8
    if t == 5: return struct.unpack(">f", d[i:i+4])[0], i + 4
    if t == 6: return struct.unpack(">d", d[i:i+8])[0], i + 8
    if t == 7:
        n = struct.unpack(">i", d[i:i+4])[0]; return f"<{n} bytes>", i + 4 + n
    if t == 8:
        n = struct.unpack(">H", d[i:i+2])[0]; return d[i+2:i+2+n].decode(errors="replace"), i + 2 + n
    if t == 9:
        et = d[i]; n = struct.unpack(">i", d[i+1:i+5])[0]; i += 5; out = []
        for _ in range(n):
            v, i = rd(d, i, et, depth + 1, ml); out.append(v)
        return out, i
    if t == 10:
        out = {}
        while d[i] != 0:
            tt = d[i]; n = struct.unpack(">H", d[i+1:i+3])[0]; name = d[i+3:i+3+n].decode(); i += 3 + n
            out[name], i = rd(d, i, tt, depth + 1, ml)
        return out, i + 1
    if t == 11:
        n = struct.unpack(">i", d[i:i+4])[0]; return list(struct.unpack(f">{n}i", d[i+4:i+4+4*n])), i + 4 + 4 * n
    if t == 12:
        n = struct.unpack(">i", d[i:i+4])[0]; return f"<{n} longs>", i + 4 + 8 * n
    raise ValueError(t)


def show(v, ind=0, ml=120):
    p = "  " * ind
    if isinstance(v, dict):
        for k, x in v.items():
            if isinstance(x, (dict, list)) and x and not (isinstance(x, list) and all(not isinstance(y, (dict, list)) for y in x) and len(str(x)) < ml):
                print(f"{p}{k}:"); show(x, ind + 1, ml)
            else:
                print(f"{p}{k}: {str(x)[:ml]}")
    elif isinstance(v, list):
        for x in v[:20]:
            if isinstance(x, (dict, list)): print(f"{p}-"); show(x, ind + 1, ml)
            else: print(f"{p}- {str(x)[:ml]}")
        if len(v) > 20: print(f"{p}... {len(v)} entries")


raw = open(sys.argv[1], "rb").read()
try: raw = gzip.decompress(raw)
except Exception: pass
t = raw[0]; n = struct.unpack(">H", raw[1:3])[0]
v, _ = rd(raw, 3 + n, t, 0, 0)
show(v, 0, int(sys.argv[2]) if len(sys.argv) > 2 else 120)
