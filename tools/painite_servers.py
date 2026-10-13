"""The list of Painite servers on this machine: ~/.config/painite/servers.json, plus native-binary servers found in CreeperCrest."""
import json, os

REG = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "painite", "servers.json")


def load():
    try:
        return json.load(open(REG))
    except (OSError, ValueError):
        return {}


def save(reg):
    os.makedirs(os.path.dirname(REG), exist_ok=True)
    json.dump(reg, open(REG, "w"), indent=2)


def add(sid, name, directory):
    reg = load()
    reg[sid] = {"name": name, "dir": os.path.abspath(os.path.expanduser(directory))}
    save(reg)


def creepercrest_dir():
    p = os.path.expanduser("~/creepercrest")
    return p if os.path.isfile(os.path.join(p, "config.json")) else ""


def all_servers():
    """[(id, name, dir)] from the registry and from CreeperCrest (native runtime only), skipping folders that no longer exist."""
    out, seen = [], set()
    for sid, s in load().items():
        d = s.get("dir", "")
        if os.path.isdir(d):
            out.append((sid, s.get("name", sid), d))
            seen.add(os.path.realpath(d))
    cc = creepercrest_dir()
    if cc:
        try:
            for sid, s in json.load(open(os.path.join(cc, "config.json"))).get("servers", {}).items():
                d = os.path.expanduser(s.get("directory", ""))
                if s.get("runtime") == "native" and os.path.isdir(d) and os.path.realpath(d) not in seen:
                    out.append((sid, s.get("name", sid) + " (CreeperCrest)", d))
        except (OSError, ValueError):
            pass
    return out


def props(directory):
    p = {}
    try:
        for line in open(os.path.join(directory, "server.properties")):
            if "=" in line and not line.startswith("#"):
                k, v = line.rstrip("\n").split("=", 1)
                p[k.strip()] = v
    except OSError:
        pass
    return p
