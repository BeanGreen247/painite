#!/usr/bin/env python3 -I
"""Generate a real vanilla world (default terrain, fixed seed) for testing Painite's anvil reader, and
record the chunk packets the vanilla server itself sends so the block data can be compared.

usage: tools/make_test_world.py --out DIR [--version 26.3] [--radius 5]
writes DIR/world (the vanilla world), DIR/vanilla_chunks.bin (x z len data ...) from the vanilla server.
"""
import argparse, importlib.util, json, os, shutil, struct, subprocess, sys, tempfile, time, uuid

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("fetch", os.path.join(here, "fetch_vanilla_data.py"))
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="26.3")
    ap.add_argument("--radius", type=int, default=5)
    ap.add_argument("--port", type=int, default=25597)
    a = ap.parse_args()
    man = json.loads(fetch.get(fetch.MANIFEST))
    vurl = next(v["url"] for v in man["versions"] if v["id"] == a.version)
    vj = json.loads(fetch.get(vurl))
    work = tempfile.mkdtemp(prefix="painite-world-")
    try:
        jar = os.path.join(work, "server.jar")
        fetch.get(vj["downloads"]["server"]["url"], jar)
        java = fetch.java_for(vj["javaVersion"]["majorVersion"], work)
        run = os.path.join(work, "run")
        os.makedirs(run)
        open(os.path.join(run, "eula.txt"), "w").write("eula=true\n")
        open(os.path.join(run, "server.properties"), "w").write(
            f"online-mode=false\nserver-port={a.port}\nwhite-list=false\nlevel-seed=painite\nview-distance={a.radius}\nspawn-protection=0\n"
            "spawn-monsters=false\nspawn-animals=false\ngenerate-structures=false\nnetwork-compression-threshold=-1\n")
        log = open(os.path.join(work, "server.log"), "w")
        srv = subprocess.Popen([java, "-Xmx1G", "-jar", jar, "nogui"], cwd=run, stdout=log, stderr=subprocess.STDOUT)
        chunks = {}
        try:
            for _ in range(180):
                if "Done" in open(os.path.join(work, "server.log")).read():
                    break
                time.sleep(1)
            else:
                sys.exit("vanilla server did not start")
            c = fetch.Conn(a.port)
            host = b"localhost"
            vi, rvi = fetch.vi, fetch.rvi
            c.send(0, vi(0) + vi(len(host)) + host + struct.pack(">H", a.port) + vi(1))
            c.send(0)
            _, d = c.recv()
            n, i = rvi(d)
            proto = json.loads(d[i:i + n])["version"]["protocol"]
            c = fetch.Conn(a.port)
            c.send(0, vi(proto) + vi(len(host)) + host + struct.pack(">H", a.port) + vi(2))
            name = b"WorldMaker"
            c.send(0, vi(len(name)) + name + uuid.UUID(int=2).bytes)
            ids = {}
            for line in open(os.path.join(here, "..", "data", f"vanilla-{a.version}.ids")):
                t = line.split()
                if t and t[0] == "id":
                    ids[(t[1], t[2], t[3])] = int(t[4])
            while True:
                pid, d = c.recv()
                if pid == 3:
                    c.thr = rvi(d)[0]
                elif pid == 2:
                    break
            c.send(ids[("login", "s", "login_acknowledged")])
            while True:
                pid, d = c.recv()
                if pid == ids[("configuration", "c", "select_known_packs")]:
                    c.send(ids[("configuration", "s", "select_known_packs")], d)
                elif pid == ids[("configuration", "c", "keep_alive")]:
                    c.send(ids[("configuration", "s", "keep_alive")], d)
                elif pid == ids[("configuration", "c", "finish_configuration")]:
                    c.send(ids[("configuration", "s", "finish_configuration")])
                    break
            ka_c, ka_s = ids[("play", "c", "keep_alive")], ids[("play", "s", "keep_alive")]
            lc = ids[("play", "c", "level_chunk_with_light")]
            end = time.time() + 40
            last = time.time()
            while time.time() < end:
                pid, d = c.recv()
                if pid == ka_c:
                    c.send(ka_s, d)
                elif pid == lc:
                    x, z = struct.unpack(">ii", d[:8])
                    chunks[(x, z)] = d
                    last = time.time()
                if len(chunks) >= (2 * a.radius + 1) ** 2 and time.time() - last > 3:
                    break
            print("recorded", len(chunks), "chunk packets")
        finally:
            srv.terminate()
            srv.wait(timeout=120)
        os.makedirs(a.out, exist_ok=True)
        dst = os.path.join(a.out, "world")
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(os.path.join(run, "world"), dst)
        with open(os.path.join(a.out, "vanilla_chunks.bin"), "wb") as f:
            for (x, z), d in chunks.items():
                f.write(struct.pack("<iiI", x, z, len(d)) + d)
        print("world written to", dst)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
