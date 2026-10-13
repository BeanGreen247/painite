import struct


def rv(d, i):
    n = s = 0
    while True:
        b = d[i]
        i += 1
        n |= (b & 127) << s
        s += 7
        if not b & 128:
            return n, i


def container(d, i, n, minbits_blocks):
    bits = d[i]
    i += 1
    pal = None
    if bits == 0:
        v, i = rv(d, i)
        return [v] * n, i
    if (minbits_blocks and bits <= 8) or (not minbits_blocks and bits <= 3):
        cnt, i = rv(d, i)
        pal = []
        for _ in range(cnt):
            v, i = rv(d, i)
            pal.append(v)
    per = 64 // bits
    nl = (n + per - 1) // per
    out = []
    for k in range(n):
        lo = struct.unpack(">Q", d[i + (k // per) * 8:i + (k // per) * 8 + 8])[0]
        v = (lo >> ((k % per) * bits)) & ((1 << bits) - 1)
        out.append(pal[v] if pal is not None else v)
    return out, i + nl * 8


def parse(d):
    x, z = struct.unpack(">ii", d[:8])
    i = 8
    cnt, i = rv(d, i)
    for _ in range(cnt):
        _, i = rv(d, i)
        ln, i = rv(d, i)
        i += ln * 8
    sz, i = rv(d, i)
    end = i + sz
    secs = []
    while i < end:
        nonair, fluid = struct.unpack(">hh", d[i:i + 4])
        i += 4
        blocks, i = container(d, i, 4096, True)
        biomes, i = container(d, i, 64, False)
        secs.append((nonair, fluid, blocks, biomes))
    return (x, z), secs, d[end:]


