#!/usr/bin/env python3
"""UDP relay that drops and delays datagrams, to exercise RakNet resends on a bad link.
usage: tests/udp_lossy_proxy.py LISTEN_PORT TARGET_PORT LOSS_PERCENT DELAY_MS
"""
import heapq, random, select, socket, sys, time

listen, target, loss, delay = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]) / 1000
front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
front.bind(("127.0.0.1", listen))
back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
back.bind(("127.0.0.1", 0))
client = None
q = []
n = 0
while True:
    timeout = max(0, q[0][0] - time.time()) if q else 0.5
    r, _, _ = select.select([front, back], [], [], timeout)
    for s in r:
        data, addr = s.recvfrom(4096)
        if random.random() * 100 < loss:
            continue
        due = time.time() + delay + random.random() * delay * 0.3
        n += 1
        if s is front:
            client = addr
            heapq.heappush(q, (due, n, back, ("127.0.0.1", target), data))
        elif client:
            heapq.heappush(q, (due, n, front, client, data))
    now = time.time()
    while q and q[0][0] <= now:
        _, _, sock, dst, data = heapq.heappop(q)
        sock.sendto(data, dst)
