#!/usr/bin/env python3 -I
"""Checks that the packet layouts Painite writes still parse the vanilla samples recorded by
tools/fetch_vanilla_data.py (play Login and Login Success) for every data file in data/.
usage: tests/check_layouts.py [data_dir]
"""
import glob, os, struct, sys


def rvi(d, i):
    n = s = 0
    while True:
        b = d[i]
        i += 1
        n |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return n, i


def blobs(path):
    d = open(path, "rb").read()
    pos = 4
    proto, n = struct.unpack_from("<II", d, pos)
    pos += 8
    for _ in range(n):
        l, = struct.unpack_from("<I", d, pos)
        pos += 4 + l
    l, = struct.unpack_from("<I", d, pos)
    pos += 4 + l
    nb, = struct.unpack_from("<I", d, pos)
    pos += 4
    out = {}
    for _ in range(nb):
        nl = d[pos]
        pos += 1
        name = d[pos:pos + nl].decode()
        pos += nl
        l, = struct.unpack_from("<I", d, pos)
        pos += 4
        out[name] = d[pos:pos + l]
        pos += l
    return proto, out


def check_login_success(d):
    i = 16
    n, i = rvi(d, i)
    i += n
    np_, i = rvi(d, i)
    for _ in range(np_):
        for _ in range(2):
            n, i = rvi(d, i)
            i += n
        has = d[i]
        i += 1
        if has:
            n, i = rvi(d, i)
            i += n
    return i + 16 == len(d)


def check_play_login(d, proto):
    i = 4 + 1
    n, i = rvi(d, i)
    for _ in range(n):
        l, i = rvi(d, i)
        i += l
    for _ in range(3):
        _, i = rvi(d, i)       # max players, view distance, simulation distance
    i += 3                      # reduced debug, respawn screen, limited crafting
    _, i = rvi(d, i)            # dimension type
    l, i = rvi(d, i)
    i += l                      # dimension name
    i += 8                      # hashed seed
    if proto >= 777:
        _, i = rvi(d, i)
        _, i = rvi(d, i)        # game mode, previous game mode (varints)
    else:
        i += 2                  # game mode and previous game mode as single bytes
    i += 2                      # debug, flat
    has_death = d[i]
    i += 1
    if has_death:
        l, i = rvi(d, i)
        i += l + 8
    _, i = rvi(d, i)            # portal cooldown
    _, i = rvi(d, i)            # sea level
    i += 2                      # trailing booleans
    return i == len(d)


def main():
    ddir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "data")
    ok = True
    for f in sorted(glob.glob(os.path.join(ddir, "vanilla-*.bin"))):
        proto, b = blobs(f)
        fixed = {"play_player_position": 61, "play_game_event": 5, "play_player_abilities": 9, "play_set_default_spawn_position": 36}
        checks = [("login_finished_sample", check_login_success), ("play_login_sample", lambda d: check_play_login(d, proto))]
        checks += [(k, (lambda n: lambda d: len(d) == n)(v)) for k, v in fixed.items()]
        for name, fn in checks:
            if name not in b:
                print(f"SKIP {os.path.basename(f)} {name}: regenerate data")
                continue
            r = fn(b[name])
            print(("PASS " if r else "FAIL ") + f"{os.path.basename(f)} (protocol {proto}) {name}")
            ok = ok and r
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
