#!/usr/bin/env python3
"""Terminal UI: pick a Painite server, start it, watch its log and status, type console commands.
Keys in the menu: up/down, Enter start, q quit. While a server runs: type a command + Enter, PgUp/PgDn scroll the log,
Ctrl-C (or /stop) stops the server and returns to the menu."""
import curses, os, queue, re, signal, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import painite_servers as ps

BIN_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "painite")
STAT_RE = re.compile(r"^TPS ([\d.]+), tick ([\d.]+) ms, players (\d+), loaded chunks (\d+), block updates (\d+) \((\d+) changed, (\d+) queued, (\d+) region threads\), players (\d+)/(\d+), workers (.*?), memory (\d+)/(\d+) MiB, cores (.*)$")
LIST_RE = re.compile(r"There are (\d+) of a max of (\d+) players online:(.*)$")
LOG_RE = re.compile(r"^\[[\d:]+\] \[[\d:]+ (INFO|WARN|ERROR)\]: (.*)$")


def lan_ips():
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return ips or ["127.0.0.1"]


class Running:
    def __init__(self, sid, name, directory):
        self.name, self.dir = name, directory
        binary = os.path.join(directory, "painite")
        if not os.path.exists(binary):
            binary = os.path.abspath(BIN_DEFAULT)
        self.proc = subprocess.Popen([binary, "nogui"], cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines, self.q = [], queue.Queue()
        self.stat, self.players, self.stat_time = None, [], 0
        self.started = time.time()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.q.put(line.rstrip("\n"))
        self.q.put(None)

    def send(self, cmd):
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError):
            pass

    def drain(self):
        while True:
            try:
                line = self.q.get_nowait()
            except queue.Empty:
                return
            if line is None:
                continue
            m = LOG_RE.match(line)
            msg = m.group(2) if m else line
            s = STAT_RE.match(msg)
            if s:
                self.stat, self.stat_time = s.groups(), time.time()
                continue                       # status polls are not log content
            l = LIST_RE.match(msg)
            if l:
                self.players = [n.strip() for n in l.group(3).split(",") if n.strip()]
                continue
            self.lines.append(line)
            if len(self.lines) > 5000:
                del self.lines[:1000]

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.alive():
            self.send("stop")
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.terminate()


def color_for(line):
    if "ERROR" in line:
        return 3
    if "WARN" in line:
        return 2
    return 0


def put(win, y, x, text, attr=0, width=None):
    h, w = win.getmaxyx()
    if y < 0 or y >= h or x >= w:
        return
    text = text[: (width if width is not None else w - x - 1)]
    try:
        win.addstr(y, x, text, attr)
    except curses.error:
        pass


def draw_run(win, r, props, inp, scroll):
    win.erase()
    h, w = win.getmaxyx()
    port, motd = props.get("server-port", "25565"), props.get("motd", "")
    ips = ", ".join(f"{ip}:{port}" for ip in lan_ips())
    up = int(time.time() - r.started)
    put(win, 0, 0, f" {r.name} ".ljust(w - 1), curses.A_REVERSE | curses.A_BOLD)
    put(win, 1, 1, f"MOTD: {motd}")
    put(win, 2, 1, f"Connect: {ips} (Java TCP, Bedrock UDP)   uptime {up // 3600}h{up % 3600 // 60:02d}m{up % 60:02d}s   mode {'online' if props.get('online-mode', 'true') == 'true' else 'offline'}")
    row = 3
    s = r.stat
    if s:
        tps, ms, pl, ch, bu, bc, bq, rt, pc, pm, workers, mem, memcap, cores = s
        tps_f = float(tps)
        tcol = curses.color_pair(1) if tps_f >= 19 else curses.color_pair(2) if tps_f >= 15 else curses.color_pair(3)
        cells = [("TPS", tps, tcol), ("Tick", f"{ms} ms", 0), ("Players", f"{pc}/{pm}", 0), ("Chunks", ch, 0),
                 ("Memory", f"{mem}/{memcap} MiB", 0), ("Workers", workers, 0), ("Block updates", f"{bu} ({bc} changed, {bq} queued)", 0),
                 ("Region threads", rt, 0), ("Cores", cores, 0)]
        colw = max(24, min(44, (w - 2) // max(1, (w - 2) // 36)))
        per = max(1, (w - 2) // colw)
        for i, (k, v, attr) in enumerate(cells):
            y, x = row + i // per, 1 + (i % per) * colw
            put(win, y, x, f"{k}: ", curses.A_DIM)
            put(win, y, x + len(k) + 2, str(v), attr | curses.A_BOLD, colw - len(k) - 3)
        row += (len(cells) + per - 1) // per
    else:
        put(win, row, 1, "waiting for the first status ...", curses.A_DIM)
        row += 1
    names = ", ".join(r.players) if r.players else "nobody online"
    put(win, row, 1, f"Online: {names}")
    row += 1
    put(win, row, 0, "-" * (w - 1), curses.A_DIM)
    row += 1
    logh = h - row - 2
    if logh > 0:
        lines = r.lines
        end = len(lines) - scroll
        view = lines[max(0, end - logh): end]
        for i, line in enumerate(view):
            put(win, row + i, 0, line, curses.color_pair(color_for(line)))
    status = "" if r.alive() else "  [server stopped, press Enter for the menu]"
    put(win, h - 2, 0, ("scrolled up " + str(scroll) + " lines  " if scroll else "") + "PgUp/PgDn scroll, Ctrl-C stops the server" + status, curses.A_DIM)
    put(win, h - 1, 0, "> " + inp, curses.A_BOLD)
    win.refresh()


def run_server(stdscr, sid, name, directory):
    r = Running(sid, name, directory)
    props = ps.props(directory)
    inp, scroll, last_poll = "", 0, 0
    stdscr.timeout(200)
    while True:
        r.drain()
        if r.alive() and time.time() - last_poll > 2:
            last_poll = time.time()
            r.send("tps")
            r.send("list")
        draw_run(stdscr, r, props, inp, scroll)
        try:
            k = stdscr.getch()
        except KeyboardInterrupt:
            k = 3
        if k == -1:
            continue
        if k == 3:
            stdscr.addstr(0, 0, " stopping ... ".ljust(10), curses.A_REVERSE)
            stdscr.refresh()
            r.stop()
            return
        if k == curses.KEY_RESIZE:
            stdscr.erase()
        elif k == curses.KEY_PPAGE:
            scroll = min(len(r.lines), scroll + 10)
        elif k == curses.KEY_NPAGE:
            scroll = max(0, scroll - 10)
        elif k in (curses.KEY_ENTER, 10, 13):
            if not r.alive():
                return
            if inp.strip():
                if inp.strip().lstrip("/") == "stop":
                    r.send("stop")
                else:
                    r.send(inp)
                r.lines.append("> " + inp)
            inp, scroll = "", 0
        elif k in (curses.KEY_BACKSPACE, 127, 8):
            inp = inp[:-1]
        elif 32 <= k < 256:
            inp += chr(k)


def menu(stdscr):
    sel = 0
    stdscr.timeout(-1)
    while True:
        servers = ps.all_servers()
        stdscr.erase()
        h, w = stdscr.getmaxyx()
        put(stdscr, 0, 0, " Painite servers ".ljust(w - 1), curses.A_REVERSE | curses.A_BOLD)
        if not servers:
            put(stdscr, 2, 2, "No servers yet. Run ./setup-server.sh to create one.")
        for i, (sid, name, d) in enumerate(servers):
            p = ps.props(d)
            line = f"{name}   port {p.get('server-port', '25565')}   {p.get('gamemode', 'survival')}   {d}"
            put(stdscr, 2 + i, 2, line, curses.A_REVERSE if i == sel else 0)
        put(stdscr, h - 1, 0, "Up/Down choose   Enter start   q quit", curses.A_DIM)
        stdscr.refresh()
        k = stdscr.getch()
        if k in (ord("q"), 27):
            return
        if k == curses.KEY_UP:
            sel = max(0, sel - 1)
        elif k == curses.KEY_DOWN:
            sel = min(max(0, len(servers) - 1), sel + 1)
        elif k in (curses.KEY_ENTER, 10, 13) and servers:
            run_server(stdscr, *servers[sel])
            stdscr.timeout(-1)


def main(stdscr):
    curses.curs_set(0)
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_GREEN, -1)
    curses.init_pair(2, curses.COLOR_YELLOW, -1)
    curses.init_pair(3, curses.COLOR_RED, -1)
    signal.signal(signal.SIGINT, signal.default_int_handler)
    menu(stdscr)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--list":
        for sid, name, d in ps.all_servers():
            print(f"{sid}\t{name}\t{d}")
        sys.exit(0)
    curses.wrapper(main)
