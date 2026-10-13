#!/usr/bin/env python3 -I
"""Run Mojang's Bedrock Dedicated Server with two scripted clients and print what the watcher receives while the
mover joins, walks, chats, runs a /setblock command and leaves. Used to learn exact packet layouts.
usage: tools/bedrock_capture_entities.py [--out FILE]
"""
import argparse, importlib.util, json, os, re, shutil, struct, subprocess, sys, tempfile, time, zipfile
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import bedrockclient as b

spec = importlib.util.spec_from_file_location("fbd", os.path.join(here, "fetch_bedrock_data.py"))
fbd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fbd)


def _s(x):
    d = x.encode()
    return b.varuint(len(d)) + d


def join(port, name):
    c = b.BedrockClient("127.0.0.1", port)
    c.connect()
    c.request_settings()
    c.login(name, f"127.0.0.1:{port}")
    c.runtime_id = None
    c.spawned = False
    c.pos = None
    return c


def handle(c, pid, pl):
    if pid == 3:
        ln, i = b.read_varuint(pl)
        c.start_encryption(pl[i:i + ln].decode())
        c.send_packets([(4, b"")])
    elif pid == 6:
        c.send_packets([(8, b.varuint(2) + _s("downloadingfinished"))])
    elif pid == 7:
        c.send_packets([(8, b.varuint(3) + _s("resourcepackstackfinished"))])
    elif pid == 11:
        _, i = b.read_varuint(pl)
        c.runtime_id, i = b.read_varuint(pl, i)
        _, i = b.read_varuint(pl, i)
        c.pos = struct.unpack("<fff", pl[i:i + 12])
        c.send_packets([(129, b"\x00"), (69, b.varuint(b.zigzag(4)) + bytes([4]))])
    elif pid == 2 and len(pl) == 4 and struct.unpack(">i", pl)[0] == 3 and not c.spawned:
        c.spawned = True
        c.send_packets([(113, b.varuint(c.runtime_id or 1))])
    elif pid == 175:
        pass


def pump(c, secs, log=None, label=""):
    end = time.time() + secs
    while time.time() < end:
        pk = c.recv_packets(0.3)
        if pk is None:
            continue
        for pid, pl in pk:
            handle(c, pid, pl)
            if pid in (5, 156) and log is not None:
                log.append((label + "-DISCONNECT/VIOLATION", pid, pl))
            if log is not None and pid not in (58, 174, 121, 74, 54, 200, 49, 29, 55):
                log.append((label, pid, pl))


def auth_input(c, x, y, z, yaw, pitch, flags=(), tick=1, block_actions=b"\x00"):
    """PlayerAuthInput as of 1.26.40+: input flags are a list of ordinals, the optional parts carry a presence byte."""
    d = struct.pack("<ff", pitch, yaw) + struct.pack("<fff", x, y, z) + struct.pack("<ff", 0, 0) + struct.pack("<f", yaw)
    d += b.varuint(len(flags)) + b"".join(b.varuint(b.zigzag(f)) for f in flags)
    d += b.varuint(1) + b.varuint(0) + b.varuint(b.zigzag(1)) + struct.pack("<ff", 0, 0) + b.varuint(tick) + struct.pack("<fff", 0, 0, 0)
    d += b"\x00\x00" + block_actions + b"\x00\x00"
    d += struct.pack("<ff", 0, 0) + struct.pack("<fff", 0, 0, 0) + struct.pack("<ff", 0, 0)
    c.send_packets([(144, d)])


def scenario(port):
    rec = []
    W = join(port, "Watcher")
    pump(W, 6)
    M = join(port, "Mover")
    pump(M, 6)
    pump(W, 2, rec, "after-mover-join")
    x, y, z = M.pos
    for k in range(1, 6):
        auth_input(M, x + k * 0.2, y, z, 90.0, 10.0, tick=k)
        pump(M, 0.1, rec, "mover-own")
    pump(W, 1.5, rec, "after-walk")
    auth_input(M, x + 1, y, z, 10.0, -20.0, flags=(8, 27), tick=20)   # sneaking + start_sneaking, rotate
    pump(M, 0.2)
    pump(W, 1.0, rec, "after-sneak-rotate")
    M.send_packets([(9, bytes([1, 0, 1]) + _s("Mover") + _s("hello bedrock") + _s("") + _s(""))])
    pump(W, 1.0, rec, "after-chat")
    M.send_packets([(77, _s("/setblock ~2 ~ ~ stone") + b"\x00" + b.varuint(0) + b"\x00" + _s("") + b"\x00")])
    pump(W, 1.5, rec, "after-setblock")
    M.send_packets([(44, bytes([1]) + b.varuint(M.runtime_id) + struct.pack("<f", 0) + b"\x00")])   # swing
    pump(W, 1.0, rec, "after-swing")
    M.s.close()
    pump(W, 2.0, rec, "after-leave")
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--port", type=int, default=19188)
    ap.add_argument("--attach", action="store_true", help="use a Bedrock server already listening on --port (offline, creative, cheats on)")
    a = ap.parse_args()
    out = open(a.out, "w") if a.out else sys.stdout
    if a.attach:
        rec = scenario(a.port)
    else:
        import urllib.request
        req = urllib.request.Request(fbd.LINKS, headers={"User-Agent": "painite-data-tool"})
        links = json.load(urllib.request.urlopen(req, timeout=60))["result"]["links"]
        url = next(l["downloadUrl"] for l in links if l["downloadType"] == "serverBedrockLinux")
        work = tempfile.mkdtemp(prefix="painite-bds-")
        try:
            zpath = os.path.join(work, "bds.zip")
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=600) as r, open(zpath, "wb") as f:
                shutil.copyfileobj(r, f)
            run = os.path.join(work, "run")
            with zipfile.ZipFile(zpath) as z:
                z.extractall(run)
            exe = os.path.join(run, "bedrock_server")
            os.chmod(exe, 0o755)
            props = os.path.join(run, "server.properties")
            text = open(props).read()
            for k, v in {"online-mode": "false", "allow-list": "false", "server-port": str(a.port), "server-portv6": str(a.port + 1), "transport": "raknet",
                         "level-name": "capture", "view-distance": "6", "level-type": "FLAT", "gamemode": "creative", "allow-cheats": "true",
                         "default-player-permission-level": "operator"}.items():
                if re.search(rf"^{re.escape(k)}=", text, re.M):
                    text = re.sub(rf"^{re.escape(k)}=.*$", f"{k}={v}", text, flags=re.M)
                else:
                    text += f"\n{k}={v}\n"
            open(props, "w").write(text)
            log = open(os.path.join(work, "bds.log"), "w")
            srv = subprocess.Popen([exe], cwd=run, env=dict(os.environ, LD_LIBRARY_PATH=run), stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(120):
                    if "Server started" in open(os.path.join(work, "bds.log")).read():
                        break
                    time.sleep(1)
                time.sleep(1)
                rec = scenario(a.port)
            finally:
                srv.terminate()
                try:
                    srv.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    srv.kill()
        finally:
            shutil.rmtree(work, ignore_errors=True)
    for label, pid, pl in rec:
        print(f"[{label}] id {pid} ({len(pl)} bytes): {pl[:900].hex(' ')}", file=out)


if __name__ == "__main__":
    main()
