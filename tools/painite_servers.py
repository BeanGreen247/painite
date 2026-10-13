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


def is_locked(directory):
    """True when a server is running on this folder (its world holds the session.lock write lock)."""
    import fcntl
    for lock in [os.path.join(directory, n, "session.lock") for n in (props(directory).get("level-name", "world"),)]:
        try:
            fd = os.open(lock, os.O_RDWR)
        except OSError:
            continue
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return True
        finally:
            os.close(fd)
    return False


def remove(directory, delete_files=False):
    """Forget the server in this folder everywhere (run.sh list and CreeperCrest config). With delete_files the folder is deleted too,
    but only when it looks like a Painite server (server.properties inside) and nothing is running on it. Returns a message."""
    real = os.path.realpath(directory)
    reg = load()
    for sid in [k for k, v in reg.items() if os.path.realpath(v.get("dir", "")) == real]:
        del reg[sid]
    save(reg)
    msg = ["removed from the run.sh list"]
    cc = creepercrest_dir()
    if cc:
        path = os.path.join(cc, "config.json")
        try:
            cfg = json.load(open(path))
            gone = [k for k, v in cfg.get("servers", {}).items() if os.path.realpath(os.path.expanduser(v.get("directory", ""))) == real]
            for k in gone:
                del cfg["servers"][k]
            if gone:
                json.dump(cfg, open(path, "w"), indent=2)
                msg.append("removed from CreeperCrest (restart it to refresh)")
        except (OSError, ValueError):
            pass
    if delete_files:
        if not os.path.isfile(os.path.join(real, "server.properties")):
            msg.append("folder NOT deleted: no server.properties in it")
        elif is_locked(real):
            msg.append("folder NOT deleted: a server is running on it")
        else:
            import shutil
            shutil.rmtree(real)
            msg.append("folder deleted")
    return "; ".join(msg)
