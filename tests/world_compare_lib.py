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




def skip_nbt(d, i, t=None):
    """Skips one network NBT tag (nameless root, type byte first); returns the new index."""
    if t is None:
        t = d[i]
        i += 1
        if t == 0:
            return i
    sizes = {1: 1, 2: 2, 3: 4, 4: 8, 5: 4, 6: 8}
    if t in sizes:
        return i + sizes[t]
    if t == 7:
        n = struct.unpack(">i", d[i:i + 4])[0]
        return i + 4 + n
    if t == 8:
        n = struct.unpack(">H", d[i:i + 2])[0]
        return i + 2 + n
    if t == 9:
        et = d[i]
        n = struct.unpack(">i", d[i + 1:i + 5])[0]
        i += 5
        for _ in range(n):
            i = skip_nbt(d, i, et)
        return i
    if t == 10:
        while d[i] != 0:
            tt = d[i]
            ln = struct.unpack(">H", d[i + 1:i + 3])[0]
            i += 3 + ln
            i = skip_nbt(d, i, tt)
        return i + 1
    if t == 11:
        n = struct.unpack(">i", d[i:i + 4])[0]
        return i + 4 + 4 * n
    if t == 12:
        n = struct.unpack(">i", d[i:i + 4])[0]
        return i + 4 + 8 * n
    raise ValueError(t)


def parse_tail(rest):
    """Block entities and light data after the sections. Returns a structural summary and checks the packet is consumed exactly."""
    i = 0
    nbe, i = rv(rest, i)
    for _ in range(nbe):
        i += 1 + 2                    # packed xz, y
        _, i = rv(rest, i)            # type
        i = skip_nbt(rest, i)
    masks = []
    for _ in range(4):
        n, i = rv(rest, i)
        masks.append(int.from_bytes(rest[i:i + n], "little"))
        i += n
    arrays = []
    for _ in range(2):
        n, i = rv(rest, i)
        for _k in range(n):
            ln, i = rv(rest, i)
            assert ln == 2048
            i += ln
        arrays.append(n)
    ok = i == len(rest)
    return {"block_entities": nbe, "masks": masks, "arrays": arrays, "exact": ok}
