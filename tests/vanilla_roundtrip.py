#!/usr/bin/env python3 -I
"""Edit a world with Painite, then start Mojang's own server on the saved world and check it reads the edit
(chunks Painite wrote must be loadable by vanilla). usage: tests/vanilla_roundtrip.py --world DIR/world [--version 26.3]
Needs: a running Painite is NOT required; the script starts and stops its own on a copy of the world.
"""
import argparse, importlib.util, json, os, shutil, struct, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
tools = os.path.join(here, "..", "tools")
sys.path.insert(0, here)
import smoke
import world_compare_lib as wc

spec = importlib.util.spec_from_file_location("fetch", os.path.join(tools, "fetch_vanilla_data.py"))
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


def play_bot(port, ids, proto, name, secs, want_chunks):
    c = smoke.C(port)
    c.send(0, smoke.vi(proto) + smoke.vs("localhost") + struct.pack(">H", port) + smoke.vi(2))
    c.send(0, smoke.vs(name) + b"\0" * 16)
    while True:
        pid, d = c.recv()
        if pid == 3:
            c.thr = smoke.rvi(d)[0]
        elif pid == 2:
            break
        elif pid == 0:
            sys.exit("login refused " + d.decode(errors="replace"))
    c.send(3)
    while True:
        pid, d = c.recv()
        if pid == ids[("configuration", "c", "select_known_packs")]:
            c.send(ids[("configuration", "s", "select_known_packs")], d)
        elif pid == ids[("configuration", "c", "keep_alive")]:
            c.send(ids[("configuration", "s", "keep_alive")], d)
        elif pid == ids[("configuration", "c", "finish_configuration")]:
            c.send(ids[("configuration", "s", "finish_configuration")])
            break
    lc = ids[("play", "c", "level_chunk_with_light")]
    got = {}
    end = time.time() + secs
    c.s.settimeout(1.0)
    while time.time() < end and len(got) < want_chunks:
        try:
            pid, d = c.recv()
        except OSError:
            continue
        if pid == ids[("play", "c", "keep_alive")]:
            c.send(ids[("play", "s", "keep_alive")], d)
        elif pid == ids[("play", "c", "chunk_batch_finished")]:
            c.send(ids[("play", "s", "chunk_batch_received")], struct.pack(">f", 20.0))
        elif pid == lc:
            k, secs_, _ = wc.parse(d)
            got[k] = secs_
    return got, c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", required=True)
    ap.add_argument("--version", default="26.3")
    ap.add_argument("--painite", default=os.path.join(here, "..", "build", "painite"))
    ap.add_argument("--ids")
    a = ap.parse_args()
    ids = smoke.load_ids(a.ids or os.path.join(here, "..", "data", f"vanilla-{a.version}.ids"))
    proto = int(open(a.ids or os.path.join(here, "..", "data", f"vanilla-{a.version}.ids")).read().split("protocol ")[1].split()[0])
    work = tempfile.mkdtemp(prefix="painite-rt-")
    try:
        run = os.path.join(work, "run")
        os.makedirs(run)
        shutil.copytree(a.world, os.path.join(run, "world"))
        port = 28299
        open(os.path.join(run, "server.properties"), "w").write(f"server-port={port}\nonline-mode=false\nlevel-name=world\ngamemode=creative\n")
        open(os.path.join(run, "ops.json"), "w").write('[{"uuid":"00000000-0000-0000-0000-000000000000","name":"Editor","level":4,"bypassesPlayerLimit":false}]')
        # Painite session: spawn, fill a gold cube and a column of mixed blocks, then stop (saves)
        p = subprocess.Popen([a.painite, "--no-bedrock"], cwd=run, stdout=open(os.path.join(work, "p.log"), "w"), stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                socket_ok = smoke.C(port)
                socket_ok.s.close()
                break
            except OSError:
                time.sleep(0.1)
        got0, c = play_bot(port, ids, proto, "Editor", 8, 60)
        sx, sz = 0, 0
        for line in open(os.path.join(run, "world", "level.dat"), "rb"):
            pass
        # edit near the first chunk the server sent (the spawn chunk is the middle of the received set)
        xs = sorted({k[0] for k in got0}); zs = sorted({k[1] for k in got0})
        cx, cz = xs[len(xs) // 2], zs[len(zs) // 2]
        bx, bz = cx * 16 + 3, cz * 16 + 5
        cmds = [f"setblock {bx} 120 {bz} gold_block", f"setblock {bx + 1} 120 {bz} oak_log[axis=x]", f"setblock {bx + 2} 120 {bz} oak_stairs[facing=east,half=top]",
                f"fill {bx} 121 {bz} {bx + 3} 123 {bz + 3} diamond_block", f"setblock {bx} 100 {bz} water"]
        for cmd in cmds:
            c.send(ids[("play", "s", "chat_command")], smoke.vs(cmd))
            time.sleep(0.3)
        time.sleep(1.0)
        c.s.close()
        time.sleep(0.5)
        p.terminate()
        p.wait(timeout=30)
        print("painite saved:", open(os.path.join(work, "p.log")).read().count("Saved") or "(no save line expected)")
        # Painite again, fresh process: do the edits survive a restart?
        p = subprocess.Popen([a.painite, "--no-bedrock"], cwd=run, stdout=open(os.path.join(work, "p2.log"), "w"), stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                s_ = smoke.C(port); s_.s.close(); break
            except OSError:
                time.sleep(0.1)
        got1, c = play_bot(port, ids, proto, "Checker", 8, 60)
        c.s.close()
        p.terminate()
        p.wait(timeout=30)
        names = {}
        for line in open(os.path.join(here, "..", "data", f"vanilla-{a.version}.blocks")):
            t = line.rstrip("\n").split("\t")
            names[int(t[0])] = t[1][10:] + ("[" + t[2] + "]" if t[2] else "")
        def at(secs, x, y, z):
            si = (y + 64) >> 4
            return names[secs[si][2][((y + 64) & 15) * 256 + (z & 15) * 16 + (x & 15)]]
        ok = True
        def chk(cond, msg):
            nonlocal ok
            print(("PASS " if cond else "FAIL ") + msg)
            ok = ok and cond
        ch = got1.get((cx, cz))
        chk(ch is not None, "edited chunk is sent after the restart")
        if ch:
            chk(at(ch, bx, 120, bz) == "gold_block", "gold block survived the restart: " + at(ch, bx, 120, bz))
            chk(at(ch, bx + 1, 120, bz) == "oak_log[axis=x]", "log axis survived: " + at(ch, bx + 1, 120, bz))
            chk(at(ch, bx + 2, 120, bz).startswith("oak_stairs[") and "facing=east" in at(ch, bx + 2, 120, bz) and "half=top" in at(ch, bx + 2, 120, bz), "stairs properties survived: " + at(ch, bx + 2, 120, bz))
            chk(at(ch, bx + 3, 123, bz + 3) == "diamond_block", "fill survived")
            chk(at(ch, bx, 100, bz).startswith("water"), "water survived: " + at(ch, bx, 100, bz))
        # now Mojang's server reads the world Painite saved
        man = json.loads(fetch.get(fetch.MANIFEST))
        vj = json.loads(fetch.get(next(v["url"] for v in man["versions"] if v["id"] == a.version)))
        jar = os.path.join(work, "server.jar")
        fetch.get(vj["downloads"]["server"]["url"], jar)
        java = fetch.java_for(vj["javaVersion"]["majorVersion"], work)
        open(os.path.join(run, "server.properties"), "w").write(f"server-port={port}\nonline-mode=false\nlevel-name=world\nwhite-list=false\nnetwork-compression-threshold=-1\nview-distance=8\n")
        open(os.path.join(run, "eula.txt"), "w").write("eula=true\n")
        log = open(os.path.join(work, "v.log"), "w")
        v = subprocess.Popen([java, "-Xmx1G", "-jar", jar, "nogui"], cwd=run, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(180):
                if "Done" in open(os.path.join(work, "v.log")).read():
                    break
                time.sleep(1)
            vc, c = None, None
            got2, c = play_bot(port, ids, proto, "VanillaCheck", 25, 400)
            c.s.close()
            chv = got2.get((cx, cz))
            chk(chv is not None, "vanilla serves the edited chunk")
            if chv:
                chk(at(chv, bx, 120, bz) == "gold_block", "vanilla sees the gold block: " + at(chv, bx, 120, bz))
                chk(at(chv, bx + 1, 120, bz) == "oak_log[axis=x]", "vanilla sees the oak log axis: " + at(chv, bx + 1, 120, bz))
                chk("facing=east" in at(chv, bx + 2, 120, bz) and "half=top" in at(chv, bx + 2, 120, bz), "vanilla sees the stairs: " + at(chv, bx + 2, 120, bz))
                chk(at(chv, bx + 3, 123, bz + 3) == "diamond_block", "vanilla sees the filled diamond blocks")
        finally:
            v.terminate()
            v.wait(timeout=60)
            txt = open(os.path.join(work, "v.log")).read()
            if "Exception" in txt or "Failed" in txt:
                print("vanilla log mentions problems:")
                print("\n".join(l for l in txt.split("\n") if "Exception" in l or "Failed" in l or "ERROR" in l)[:1500])
        print("OK" if ok else "FAILED")
        sys.exit(0 if ok else 1)
    finally:
        shutil.rmtree(work, ignore_errors=True)


main()
