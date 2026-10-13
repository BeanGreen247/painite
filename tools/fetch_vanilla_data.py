#!/usr/bin/env python3 -I
"""Generate Painite's protocol data from Mojang's own server jar.

Downloads the vanilla server for a release, runs its data generator for the packet ID
table, starts it briefly in a temp directory, joins as an offline bot and records the
configuration packets (registries, tags, ...) plus one flat-world chunk. Nothing from
Mojang is stored in the repository; output goes to data/ which is git-ignored.

usage: tools/fetch_vanilla_data.py [--version 26.3] [--out data] [--keep]
"""
import argparse, json, os, shutil, socket, struct, subprocess, sys, tarfile, tempfile, time, urllib.request, uuid, zlib

MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"


def get(url, dest=None):
    req = urllib.request.Request(url, headers={"User-Agent": "painite-data-tool"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    if dest:
        open(dest, "wb").write(data)
    return data


def vi(n):
    o = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            o += bytes([b | 0x80])
        else:
            return o + bytes([b])


def rvi(d, i=0):
    n = s = 0
    while True:
        b = d[i]
        i += 1
        n |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return n, i


class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buf = b""
        self.thr = -1

    def _need(self, k):
        while len(self.buf) < k:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("server closed the connection")
            self.buf += d
        r, self.buf = self.buf[:k], self.buf[k:]
        return r

    def send(self, pid, data=b""):
        p = vi(pid) + data
        if self.thr >= 0:
            p = vi(0) + p
        self.s.sendall(vi(len(p)) + p)

    def recv(self):
        n = s = 0
        while True:
            b = self._need(1)[0]
            n |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                break
        p = self._need(n)
        if self.thr >= 0:
            dl, i = rvi(p)
            p = p[i:] if dl == 0 else zlib.decompress(p[i:])
        pid, i = rvi(p)
        return pid, p[i:]


def java_for(major, work):
    out = subprocess.run(["java", "-version"], capture_output=True, text=True)
    txt = out.stderr + out.stdout
    try:
        cur = int(txt.split('"')[1].split(".")[0])
    except Exception:
        cur = 0
    if cur >= major:
        return "java"
    print(f"downloading Temurin JRE {major} ...")
    tgz = os.path.join(work, "jre.tgz")
    get(f"https://api.adoptium.net/v3/binary/latest/{major}/ga/linux/x64/jre/hotspot/normal/eclipse", tgz)
    with tarfile.open(tgz) as t:
        t.extractall(os.path.join(work, "jre"), filter="data")
    root = os.path.join(work, "jre")
    return os.path.join(root, os.listdir(root)[0], "bin", "java")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version")
    ap.add_argument("--out", default="data")
    ap.add_argument("--port", type=int, default=25599)
    a = ap.parse_args()
    man = json.loads(get(MANIFEST))
    ver = a.version or man["latest"]["release"]
    vurl = next(v["url"] for v in man["versions"] if v["id"] == ver)
    vj = json.loads(get(vurl))
    major = vj["javaVersion"]["majorVersion"]
    work = tempfile.mkdtemp(prefix="painite-data-")
    try:
        jar = os.path.join(work, "server.jar")
        print("downloading server.jar", ver)
        get(vj["downloads"]["server"]["url"], jar)
        java = java_for(major, work)
        gen = os.path.join(work, "gen")
        subprocess.run([java, "-DbundlerMainClass=net.minecraft.data.Main", "-jar", jar, "--reports", "--server", "--output", gen],
                       check=True, capture_output=True, cwd=work)
        packets = json.load(open(os.path.join(gen, "reports", "packets.json")))
        run = os.path.join(work, "run")
        os.makedirs(run)
        open(os.path.join(run, "eula.txt"), "w").write("eula=true\n")
        open(os.path.join(run, "server.properties"), "w").write(
            f"online-mode=false\nserver-port={a.port}\nwhite-list=false\nenforce-whitelist=false\n"
            "generator-settings={}\nlevel-type=minecraft\\:flat\nview-distance=4\nspawn-protection=0\n")
        log = open(os.path.join(work, "server.log"), "w")
        srv = subprocess.Popen([java, "-Xmx1G", "-jar", jar, "nogui"], cwd=run, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if "Done" in open(os.path.join(work, "server.log")).read():
                    break
                time.sleep(1)
            else:
                sys.exit("vanilla server did not start")
            c = Conn(a.port)
            host = b"localhost"
            c.send(0, vi(0) + vi(len(host)) + host + struct.pack(">H", a.port) + vi(1))
            c.send(0)
            _, d = c.recv()
            n, i = rvi(d)
            proto = json.loads(d[i:i + n])["version"]["protocol"]
            print("protocol", proto, flush=True)
            c = Conn(a.port)
            c.send(0, vi(proto) + vi(len(host)) + host + struct.pack(">H", a.port) + vi(2))
            name = b"PainiteData"
            c.send(0, vi(len(name)) + name + uuid.UUID(int=1).bytes)
            login_finished = b""
            while True:
                pid, d = c.recv()
                if pid == 3:
                    c.thr = rvi(d)[0]
                elif pid == 2:
                    login_finished = d
                    break
                elif pid == 0:
                    sys.exit("login refused: " + d.decode(errors="replace"))
            def pid_of(state, direction, name):
                return packets[state][direction]["minecraft:" + name]["protocol_id"]

            cf_known_c, cf_known_s = pid_of("configuration", "clientbound", "select_known_packs"), pid_of("configuration", "serverbound", "select_known_packs")
            cf_ka_c, cf_ka_s = pid_of("configuration", "clientbound", "keep_alive"), pid_of("configuration", "serverbound", "keep_alive")
            cf_fin_c, cf_fin_s = pid_of("configuration", "clientbound", "finish_configuration"), pid_of("configuration", "serverbound", "finish_configuration")
            c.send(pid_of("login", "serverbound", "login_acknowledged"))
            cfg = []
            while True:
                pid, d = c.recv()
                cfg.append(vi(pid) + d)
                if pid == cf_known_c:
                    c.send(cf_known_s, d)
                elif pid == cf_ka_c:
                    c.send(cf_ka_s, d)
                elif pid == cf_fin_c:
                    c.send(cf_fin_s)
                    break
            chunks, login, set_time = [], None, None
            samples = {}
            want = ["player_position", "set_default_spawn_position", "game_event", "set_chunk_cache_center", "player_abilities"]
            want_ids = {pid_of("play", "clientbound", n): n for n in want}
            end = time.time() + 8
            while time.time() < end:
                pid, d = c.recv()
                if pid == pid_of("play", "clientbound", "keep_alive"):
                    c.send(pid_of("play", "serverbound", "keep_alive"), d)
                elif pid == pid_of("play", "clientbound", "level_chunk_with_light"):
                    chunks.append(d)
                elif pid == pid_of("play", "clientbound", "login"):
                    login = d
                elif pid == pid_of("play", "clientbound", "set_time") and set_time is None:
                    set_time = d
                elif pid in want_ids and want_ids[pid] not in samples:
                    samples[want_ids[pid]] = d
            if not chunks:
                sys.exit("no chunk captured")
            # most common size = plain flat chunk without structures
            sizes = [len(x) for x in chunks]
            best = max(set(sizes), key=sizes.count)
            tpl = next(x for x in chunks if len(x) == best)[8:]
        finally:
            srv.terminate()
            srv.wait(timeout=60)

        # registry ids needed for constants
        def reg_index(regname, entry):
            for p in cfg:
                pid, i = rvi(p)
                if pid != 7:
                    continue
                n, i = rvi(p, i)
                if p[i:i + n].decode() != regname:
                    continue
                i += n
                cnt, i = rvi(p, i)
                for k in range(cnt):
                    n, i = rvi(p, i)
                    nm = p[i:i + n].decode()
                    i += n
                    has = p[i]
                    i += 1
                    if has:
                        raise RuntimeError("registry entry carries inline data; known-pack path expected")
                    if nm == entry:
                        return k
            raise KeyError(entry)

        os.makedirs(a.out, exist_ok=True)
        base = os.path.join(a.out, f"vanilla-{ver}")
        with open(base + ".bin", "wb") as f:
            f.write(b"PNTE" + struct.pack("<II", proto, len(cfg)))
            for p in cfg:
                f.write(struct.pack("<I", len(p)) + p)
            f.write(struct.pack("<I", len(tpl)) + tpl)
            blobs = {"set_time": set_time, "login_finished_sample": login_finished, "play_login_sample": login}
            for k, v in samples.items():
                blobs["play_" + k] = v
            f.write(struct.pack("<I", len(blobs)))
            for k, v in blobs.items():
                f.write(bytes([len(k)]) + k.encode() + struct.pack("<I", len(v)) + v)
        # login packet tail (sea level etc.) is decoded from the capture so we match vanilla
        i = 4 + 1
        n, i = rvi(login, i)
        for _ in range(n):
            l, i = rvi(login, i)
            i += l
        for _ in range(3):
            _, i = rvi(login, i)
        i += 3
        dim_type, i = rvi(login, i)
        with open(base + ".ids", "w") as f:
            f.write(f"version {ver}\nprotocol {proto}\n")
            f.write(f"const dim_type_overworld {dim_type}\n")
            f.write(f"const biome_plains {reg_index('minecraft:worldgen/biome', 'minecraft:plains')}\n")
            for st in ("handshake", "status", "login", "configuration", "play"):
                for d, short in (("clientbound", "c"), ("serverbound", "s")):
                    for k, v in packets.get(st, {}).get(d, {}).items():
                        f.write(f"id {st} {short} {k.split(':')[1]} {v['protocol_id']}\n")
        print("wrote", base + ".bin", "and .ids")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
