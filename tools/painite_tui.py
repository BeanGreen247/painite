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
        self.tick, self.tasks, self.workers, self.pstat = None, {}, {}, None
        self.perf_t = 0.0
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
            if msg.startswith("PERF "):
                self._perf(msg.split())
                continue
            self.lines.append(line)
            if len(self.lines) > 5000:
                del self.lines[:1000]

    def _perf(self, f):
        now = time.time()
        if f[1] == "tick":
            self.tick = (float(f[2]), float(f[3]), float(f[4]), float(f[5]))
        elif f[1] == "task":
            interval = max(0.5, now - self.perf_t) if self.perf_t else 2.0
            self.tasks[f[2]] = (int(f[3]) / interval, float(f[4]), float(f[5]))
        elif f[1] == "worker":
            self.workers[int(f[2])] = (float(f[3]), int(f[4]), float(f[5]), float(f[6]))
        elif f[1] == "process":
            self.pstat = (float(f[2]), float(f[3]), int(f[4]), int(f[5]), int(f[6]))
        elif f[1] == "players":
            self.players = [n for n in (f[2].split(",") if len(f) > 2 else []) if n]
            self.perf_t = now      # players is the last line of a report

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


def bar(frac, width):
    frac = max(0.0, min(1.0, frac))
    n = int(round(frac * width))
    return "[" + "#" * n + "." * (width - n) + "]"


def us(v):
    return f"{v:.0f} us" if v < 1000 else f"{v / 1000:.2f} ms"


def panels(r, w):
    """Lines of the performance panels (text, attr) for a terminal w columns wide."""
    out = []
    bw = max(8, min(30, w // 4))
    if r.tick:
        avg, p99, mx, pct = r.tick
        col = curses.color_pair(1) if pct < 40 else curses.color_pair(2) if pct < 80 else curses.color_pair(3)
        out.append((f" Tick  avg {us(avg)}  p99 {us(p99)}  max {us(mx)}   budget used {pct:.1f}% of 50 ms {bar(pct / 100, bw)}", col))
    if r.workers:
        out.append((" Workers (one epoll loop per core; busy = time handling packets, not waiting)", curses.A_BOLD))
        for wid in sorted(r.workers):
            busy, conns, kin, kout = r.workers[wid]
            out.append((f"  #{wid} busy {busy:5.1f}% {bar(busy / 100, bw)}  conns {conns:3d}  in {kin:7.1f} KB/s  out {kout:8.1f} KB/s", 0))
    if r.pstat:
        cpu, rss, thr, chunks, pl = r.pstat
        per = f"   per player: {cpu / pl:.2f}% CPU, {rss / pl:.0f} MB" if pl else ""
        out.append((f" Process  cpu {cpu:.1f}% of one core  rss {rss:.0f} MB  threads {thr}  chunks loaded {chunks}{per}", 0))
    if r.tasks and w >= 70:
        out.append((" Tasks (since the last 2 s poll)                          calls/s       avg        max", curses.A_BOLD))
        for name in sorted(r.tasks):
            rate, avg, mx = r.tasks[name]
            out.append((f"  {name:<26}{rate:>31.1f} {us(avg):>10} {us(mx):>10}", 0))
    return out


WHY = [
    "Why a native rewrite (the live figures above come from this server)",
    "",
    "A Java server has to fit everything in one tick thread plus a garbage collector. Painite is built around what limits a Minecraft server:",
    "",
    "* Tick budget: a tick must finish in 50 ms. The 'budget used' figure on the dashboard is how much of that this server spends right now.",
    "  Block updates are sharded per 512x512 region and run in parallel; nothing in the tick waits for the network.",
    "* Network: each core runs its own epoll loop (SO_REUSEPORT), pinned to its CPU; workers start and retire with load inside a memory cap.",
    "  A chunk is encoded and compressed once and sent to every client that needs it.",
    "* Memory: no JVM heap, no garbage collection pauses. The 'per player' lines show real RSS and CPU divided by players online.",
    "* Both editions in one process: Java and Bedrock players share the same world, entities and block state, no proxy such as Geyser in between.",
    "* Startup: seconds, not minutes; an idle server uses well under 1% of one core.",
    "",
    "Be honest about the other side: Painite has no plugins, mobs, terrain generation or survival inventory yet.",
    "Compare it on your own machine: run Paper on the same world and use tests/load_test.py against both (bots, CPU and memory are printed).",
    "",
    "Press F2 to go back.",
]


def draw_why(win, r):
    win.erase()
    h, w = win.getmaxyx()
    put(win, 0, 0, " Why a rewrite ".ljust(w - 1), curses.A_REVERSE | curses.A_BOLD)
    row = 2
    if r.tick:
        put(win, row, 2, f"Right now: tick uses {r.tick[3]:.1f}% of its 50 ms budget (avg {us(r.tick[0])}, p99 {us(r.tick[1])}).", curses.A_BOLD)
        row += 1
    if r.pstat:
        cpu, rss, thr, chunks, pl = r.pstat
        put(win, row, 2, f"Process: {cpu:.1f}% of one core, {rss:.0f} MB resident, {thr} threads, {chunks} chunks in memory, {pl} players.", curses.A_BOLD)
        row += 1
    if r.workers:
        busy = ", ".join(f"#{k} {v[0]:.0f}%" for k, v in sorted(r.workers.items()))
        put(win, row, 2, f"Network workers busy: {busy}", curses.A_BOLD)
        row += 1
    row += 1
    import textwrap
    for line in WHY:
        for chunk in (textwrap.wrap(line, max(20, w - 6), subsequent_indent="    ") or [""]):
            put(win, row, 2, chunk, curses.A_BOLD if line.startswith("Why") else 0)
            row += 1
    win.refresh()


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
        cells = [("TPS", tps, tcol), ("Players", f"{pc}/{pm}", 0), ("Memory", f"{mem}/{memcap} MiB", 0), ("Block updates", f"{bu} ({bc} changed, {bq} queued)", 0),
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
    put(win, row, 1, "Online: " + (", ".join(r.players) if r.players else "nobody online"))
    row += 1
    budget = max(0, h - row - 5)           # keep at least 4 log lines
    pl_lines = panels(r, w)
    for text, attr in pl_lines[:budget]:
        put(win, row, 0, text, attr)
        row += 1
    put(win, row, 0, "-" * (w - 1), curses.A_DIM)
    row += 1
    logh = h - row - 2
    if logh > 0:
        lines = r.lines
        end = len(lines) - scroll
        for i, line in enumerate(lines[max(0, end - logh): end]):
            put(win, row + i, 0, line, curses.color_pair(color_for(line)))
    status = "" if r.alive() else "  [server stopped, press Enter for the menu]"
    put(win, h - 2, 0, ("scrolled up " + str(scroll) + " lines  " if scroll else "") + "PgUp/PgDn scroll  F2 why a rewrite  Ctrl-C stops the server" + status, curses.A_DIM)
    put(win, h - 1, 0, "> " + inp, curses.A_BOLD)
    win.refresh()


def run_server(stdscr, sid, name, directory):
    r = Running(sid, name, directory)
    props = ps.props(directory)
    inp, scroll, last_poll, why = "", 0, 0, False
    stdscr.timeout(200)
    while True:
        r.drain()
        if r.alive() and time.time() - last_poll > 2:
            last_poll = time.time()
            r.send("tps")
            r.send("perf")
        if why:
            draw_why(stdscr, r)
        else:
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
        if k == curses.KEY_F2:
            why = not why
            continue
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
