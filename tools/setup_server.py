#!/usr/bin/env python3
"""Interactive first-time setup: asks for the settings CreeperCrest's wizard asks for (minus the JAR download), builds Painite if needed,
creates the server folder, optionally copies a world in, and registers the server for run.sh and CreeperCrest."""
import json, os, re, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import painite_servers as ps

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
B, D, R = "\033[1m", "\033[2m", "\033[0m"


def ask(label, default="", validate=None, hint=""):
    while True:
        shown = f" [{default}]" if default != "" else ""
        try:
            v = input(f"{B}{label}{R}{D}{(' (' + hint + ')') if hint else ''}{R}{shown}: ").strip()
        except EOFError:
            sys.exit(1)
        v = v or str(default)
        if validate:
            err = validate(v)
            if err:
                print(f"  {err}")
                continue
        return v


def yesno(label, default):
    v = ask(label, "y" if default else "n", lambda s: None if s.lower() in ("y", "n", "yes", "no") else "answer y or n")
    return v.lower().startswith("y")


def choice(label, options, default):
    return ask(label, default, lambda s: None if s in options else "one of: " + ", ".join(options), "/".join(options))


def num(lo, hi):
    def v(s):
        return None if s.isdigit() and lo <= int(s) <= hi else f"a number from {lo} to {hi}"
    return v


def remove_mode():
    servers = ps.all_servers()
    if not servers:
        sys.exit("no servers registered")
    for i, (sid, name, d) in enumerate(servers, 1):
        print(f"{i}) {name}  {d}")
    n = ask("Number of the server to remove", "", lambda s: None if s.isdigit() and 1 <= int(s) <= len(servers) else "pick a number from the list")
    sid, name, d = servers[int(n) - 1]
    delete = yesno(f"Also DELETE the folder {d} including its world", False)
    if delete and ask("Type DELETE to confirm", "") != "DELETE":
        sys.exit("cancelled")
    print(ps.remove(d, delete))


def main():
    if "--remove" in sys.argv:
        return remove_mode()
    print(f"\n{B}Painite server setup{R}  (Enter keeps the value in brackets)\n")
    name = ask("Server name", "Painite")
    sid = re.sub(r"[^a-z0-9_-]+", "-", name.lower()).strip("-") or "painite"
    sid = ask("Id (for CreeperCrest and run.sh)", sid, lambda s: None if re.fullmatch(r"[a-z0-9_-]+", s) else "lowercase letters, digits, - and _")
    motd = ask("MOTD (shown in the server list)", "A Painite Server")
    directory = os.path.expanduser(ask("Server folder", f"~/{sid}-server"))
    if os.path.exists(os.path.join(directory, "server.properties")) and not yesno(f"{directory} already holds a server. Reconfigure it (properties are overwritten, the world stays)", False):
        sys.exit("nothing changed")
    print(f"\n{B}World and gameplay{R}  (these go into server.properties; entries marked * are stored but Painite does not act on them yet)")
    level = ask("World folder name", "world")
    wtype = choice("World type",
                   ["normal", "flat", "void", "large_biomes", "amplified"], "normal")
    seed = ask("World seed", "", hint="blank = random")
    gm = choice("Default game mode", ["survival", "creative", "adventure", "spectator"], "survival")
    diff = choice("Difficulty", ["peaceful", "easy", "normal", "hard"], "easy")
    maxp = ask("Max players", "20", num(1, 1000))
    port = ask("Java port (Bedrock uses the same number over UDP)", "25565", num(1024, 65535))
    view = ask("View distance (chunks)", "10", num(2, 32))
    sim = ask("Simulation distance (chunks)", "10", num(3, 32))
    prot = ask("Spawn protection (blocks, 0 = off)", "0", num(0, 1000))
    online = yesno("Online mode (verify accounts with Mojang/Xbox; no for cracked launchers or LAN tests)", True)
    pvp = yesno("Player vs player *", True)
    hardcore = yesno("Hardcore", False)
    wl = yesno("Whitelist", False)
    nether = yesno("Allow the Nether *", True)
    flight = yesno("Allow flight *", False)
    cmdb = yesno("Command blocks *", False)
    bedrock = yesno("Let Bedrock players join too", True)
    world = ""
    if yesno("Copy an existing world (Paper/vanilla 26.x) into the server", False):
        world = os.path.expanduser(ask("World folder to copy", "", lambda s: None if os.path.isdir(os.path.expanduser(s)) else "not a folder"))
    gen = choice("Chunks that are not on disk (auto: the world type's generator in a new world, void in an existing one; terrain: the built-in approximation of vanilla)", ["auto", "terrain", "flat", "void"], "auto")
    autosave = ask("Autosave every N seconds", "300", num(10, 86400))
    adv = yesno("Change CPU/worker/memory settings", False)
    cores, maxw, memcap = "all", "0", "1024"
    if adv:
        cores = ask("CPU cores", "all", hint="all, 0-3 or 0,2,4")
        maxw = ask("Max workers (0 = one per core)", "0", num(0, 256))
        memcap = ask("Memory cap in MiB for connections and chunk cache", "1024", num(64, 1048576))
    cc = ps.creepercrest_dir()
    reg_cc = bool(cc) and yesno(f"Add the server to CreeperCrest ({cc})", True)

    binary = os.path.join(ROOT, "build", "painite")
    if not os.access(binary, os.X_OK) or not os.path.isfile(os.path.join(ROOT, "data", "blocks-bedrock.bin")):
        print(f"\n{B}Building Painite and generating protocol data (downloads Mojang's servers, a few minutes) ...{R}")
        r = subprocess.run([os.path.join(ROOT, "build.sh"), "--data", "--java", "26.2,26.3"])
        if r.returncode:
            sys.exit("build failed")
    os.makedirs(directory, exist_ok=True)
    for link, target in (("painite", binary), ("data", os.path.join(ROOT, "data"))):
        p = os.path.join(directory, link)
        if os.path.lexists(p):
            os.remove(p)
        os.symlink(target, p)
    with open(os.path.join(directory, "server.properties"), "w") as f:
        tf = lambda v: "true" if v else "false"
        f.write(f"motd={motd}\nserver-port={port}\nmax-players={maxp}\nonline-mode={tf(online)}\ngamemode={gm}\ndifficulty={diff}\nlevel-name={level}\n"
                f"level-type=minecraft\\:{wtype}\nlevel-seed={seed}\nview-distance={view}\nsimulation-distance={sim}\nspawn-protection={prot}\npvp={tf(pvp)}\n"
                f"hardcore={tf(hardcore)}\nwhite-list={tf(wl)}\nallow-nether={tf(nether)}\nallow-flight={tf(flight)}\nenable-command-block={tf(cmdb)}\n")
    toml = open(os.path.join(ROOT, "painite.toml")).read()
    def setkey(text, key, val):
        return re.sub(rf"^({re.escape(key)}\s*=\s*)(.*?)(\s*#.*)?$", lambda m: m.group(1) + val + (m.group(3) or ""), text, count=1, flags=re.M)
    toml = setkey(toml, "bedrock", "true" if bedrock else "false")
    toml = setkey(toml, "generator", f'"{gen}"')
    toml = setkey(toml, "autosave_s", autosave)
    toml = setkey(toml, "cores", f'"{cores}"')
    toml = setkey(toml, "max", maxw)
    toml = setkey(toml, "memory_cap_mb", memcap)
    open(os.path.join(directory, "painite.toml"), "w").write(toml)
    if world:
        dest = os.path.join(directory, level)
        if os.path.exists(dest):
            if yesno(f"{dest} exists. Replace it with the copy", False):
                shutil.rmtree(dest)
            else:
                world = ""
        if world:
            print("copying the world ...")
            shutil.copytree(world, dest, symlinks=True)
            lock = os.path.join(dest, "session.lock")
            if os.path.exists(lock):
                os.remove(lock)
    ps.add(sid, name, directory)
    if reg_cc:
        path = os.path.join(cc, "config.json")
        cfg = json.load(open(path))
        if sid in cfg.setdefault("servers", {}):
            print(f"CreeperCrest already has an id '{sid}', not changed")
        else:
            cfg["servers"][sid] = {"name": name, "directory": directory, "jar": "painite", "runtime": "native", "memory_min_mb": 256, "memory_max_mb": 1024,
                                   "extra_args": "", "autostart": False, "allow_offline": not online}
            json.dump(cfg, open(path, "w"), indent=2)
            print("added to CreeperCrest (restart it to see the new server)")
    print(f"\n{B}Done.{R} Start it with ./run.sh (pick '{name}'), or by hand: cd {directory} && ./painite")
    print("Java and Bedrock clients connect to this machine's address on port " + port + ".")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("\ncancelled")
