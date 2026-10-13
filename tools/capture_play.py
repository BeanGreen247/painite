#!/usr/bin/env python3 -I
"""Run a vanilla server with two scripted bots and print the play packets the observer receives while the
actor joins, moves, turns, places and breaks a block, chats and leaves. Used to learn exact packet layouts.
usage: tools/capture_play.py [--version 26.3] [--out FILE]   (needs data/vanilla-<ver>.ids)
"""
import argparse, importlib.util, json, os, shutil, struct, subprocess, sys, tempfile, time, uuid

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("fetch", os.path.join(here, "fetch_vanilla_data.py"))
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)
vi, rvi = fetch.vi, fetch.rvi


def load_ids(path):
    ids, names = {}, {}
    for line in open(path):
        t = line.split()
        if t and t[0] == "id":
            ids[(t[1], t[2], t[3])] = int(t[4])
            names[(t[1], t[2], int(t[4]))] = t[3]
    return ids, names


def login(port, name, ids, proto):
    c = fetch.Conn(port)
    host = b"localhost"
    c.send(0, vi(proto) + vi(len(host)) + host + struct.pack(">H", port) + vi(2))
    nb = name.encode()
    c.send(0, vi(len(nb)) + nb + uuid.uuid3(uuid.NAMESPACE_DNS, "OfflinePlayer:" + name).bytes)
    while True:
        pid, d = c.recv()
        if pid == 3:
            c.thr = rvi(d)[0]
        elif pid == 2:
            break
        elif pid == 0:
            sys.exit("login refused: " + d.decode(errors="replace"))
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
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="26.3")
    ap.add_argument("--out")
    ap.add_argument("--port", type=int, default=25596)
    a = ap.parse_args()
    ids, names = load_ids(os.path.join(here, "..", "data", f"vanilla-{a.version}.ids"))
    man = json.loads(fetch.get(fetch.MANIFEST))
    vj = json.loads(fetch.get(next(v["url"] for v in man["versions"] if v["id"] == a.version)))
    work = tempfile.mkdtemp(prefix="painite-cap-")
    out = open(a.out, "w") if a.out else sys.stdout
    try:
        jar = os.path.join(work, "server.jar")
        fetch.get(vj["downloads"]["server"]["url"], jar)
        java = fetch.java_for(vj["javaVersion"]["majorVersion"], work)
        run = os.path.join(work, "run")
        os.makedirs(run)
        open(os.path.join(run, "eula.txt"), "w").write("eula=true\n")
        open(os.path.join(run, "server.properties"), "w").write(
            f"online-mode=false\nwhite-list=false\nserver-port={a.port}\nlevel-type=minecraft\\:flat\ngamemode=creative\nforce-gamemode=true\nspawn-protection=0\n"
            "view-distance=4\nspawn-monsters=false\nspawn-animals=false\ndifficulty=peaceful\ngenerate-structures=false\nnetwork-compression-threshold=-1\nenforce-secure-profile=false\n")
        srv = subprocess.Popen([java, "-Xmx1G", "-jar", jar, "nogui"], cwd=run, stdout=open(os.path.join(work, "s.log"), "w"), stderr=subprocess.STDOUT)
        try:
            for _ in range(180):
                if "Done" in open(os.path.join(work, "s.log")).read():
                    break
                time.sleep(1)
            c = fetch.Conn(a.port)
            host = b"localhost"
            c.send(0, vi(0) + vi(len(host)) + host + struct.pack(">H", a.port) + vi(1))
            c.send(0)
            _, d = c.recv()
            n, i = rvi(d)
            proto = json.loads(d[i:i + n])["version"]["protocol"]
            obs = login(a.port, "Observer", ids, proto)
            act = None
            obs.s.settimeout(0.5)
            S = lambda n: ids[("play", "s", n)]
            Cb = lambda n: ids[("play", "c", n)]

            def pump(conn, secs, log=None, label=""):
                end = time.time() + secs
                while time.time() < end:
                    try:
                        pid, d = conn.recv()
                    except (TimeoutError, OSError):
                        continue
                    except EOFError:
                        print("EOF", label, file=sys.stderr)
                        return
                    nm = names.get(("play", "c", pid), str(pid))
                    if os.environ.get("DBG") and conn is obs:
                        print("obs got", nm, len(d), file=sys.stderr)
                    if nm == "disconnect":
                        print("DISCONNECT", label, d, file=sys.stderr)
                    if nm == "player_position":
                        tid, k = rvi(d)
                        conn.spawn = struct.unpack(">ddd", d[k:k + 24])
                    if nm == "keep_alive":
                        conn.send(S("keep_alive"), d)
                    elif nm == "chunk_batch_finished":
                        conn.send(S("chunk_batch_received"), struct.pack(">f", 10.0))
                    elif log is not None and nm not in ("level_chunk_with_light", "chunk_batch_start", "chunk_batch_finished", "set_time", "update_attributes", "container_set_content", "set_cursor_item", "set_entity_data", "update_recipes", "update_advancements", "recipe_book_add", "commands", "tab_list", "server_data", "initialize_border", "game_event", "set_chunk_cache_center", "set_chunk_cache_radius", "set_simulation_distance", "set_default_spawn_position", "ticking_state", "ticking_step", "set_held_slot", "set_experience", "set_health", "player_abilities", "player_position", "login", "update_tags", "waypoint", "sound", "game_rule_values", "select_advancements_tab"):
                        log.append((label, nm, d))
            pump(obs, 3)
            log = []
            act = login(a.port, "Actor", ids, proto)
            act.s.settimeout(0.5)
            pump(act, 3)
            pump(obs, 2, log, "observer-after-actor-join")
            # actor: walk, turn, place and break a block, chat
            for cc in (obs, act):
                _send = cc.send
                def safe(pid, data=b"", _s=_send):
                    try:
                        _s(pid, data)
                    except OSError as e:
                        print("send failed", names.get(("play", "s", pid), pid), e, file=sys.stderr)
                cc.send = safe
            def pos_rot(x, y, z, yaw, pitch, flags=1):
                return struct.pack(">dddffB", x, y, z, yaw, pitch, flags)
            x0, y0, z0 = act.spawn
            print("spawn", act.spawn, file=sys.stderr)
            for k in range(1, 6):
                act.send(S("move_player_pos"), struct.pack(">dddB", x0 + k * 0.1, y0, z0, 1))
                act.send(S("client_tick_end"))
                pump(act, 0.05, None, "move%d" % k)
            pump(obs, 1, log, "observer-after-small-moves")
            act.send(S("move_player_pos_rot"), pos_rot(x0 + 3, y0, z0 + 2, 90.0, 20.0))
            pump(obs, 1, log, "observer-after-move-rot")
            act.send(S("move_player_rot"), struct.pack(">ffB", 180.0, -10.0, 1))
            pump(obs, 1, log, "observer-after-rot-only")
            act.send(S("move_player_pos"), struct.pack(">dddB", x0 + 7, y0, z0, 1))
            pump(obs, 1, log, "observer-after-big-move")
            # creative inventory: stone block into hotbar slot 0 (slot 36), select it
            item_stone = vi(64) + vi(ids and 1) + vi(0) + vi(0)  # count 64, item id 1 (stone), no components
            act.send(S("set_creative_mode_slot"), struct.pack(">h", 36) + item_stone)
            act.send(S("set_carried_item"), struct.pack(">h", 0))
            pump(act, 0.5)
            import math
            tx, tz = math.floor(x0 + 2.5) + 1, math.floor(z0 + 2.5)
            pos = lambda x, y, z: struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))
            act.send(S("move_player_pos"), struct.pack(">dddB", x0 + 2.5, y0, z0 + 2.5, 1))
            pump(act, 0.3)
            # place on top face of the grass block at (3,-61,3): hand, pos, face(up=1), cursor, inside, worldborder, sequence
            act.send(S("use_item_on"), vi(0) + pos(tx, -61, tz) + vi(1) + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + vi(1))
            pump(act, 0.5, log, "actor-own-after-place")
            pump(obs, 1, log, "observer-after-place")
            # break it (creative: start destroy = instant)
            act.send(S("player_action"), vi(0) + pos(tx, -60, tz) + b"\x01" + vi(2))
            pump(act, 0.5, log, "actor-own-after-break")
            pump(obs, 1, log, "observer-after-break")
            act.send(S("punch"), vi(0))
            pump(obs, 1, log, "observer-after-swing")
            act.send(S("chat_command"), vi(len(b"say hello")) + b"say hello")
            pump(obs, 1, log, "observer-after-say")
            act.s.close()
            pump(obs, 2, log, "observer-after-actor-leave")
        finally:
            srv.terminate()
            srv.wait(timeout=60)
        for label, nm, d in log:
            print(f"[{label}] {nm} ({len(d)} bytes): {d[:96].hex(' ')}", file=out)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
