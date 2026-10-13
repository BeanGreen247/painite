#!/usr/bin/env python3
"""End-to-end online-mode test against a local fake session server (HTTPS, self-signed CA).
Covers: encryption request, RSA key exchange, AES/CFB8 stream, server-hash, hasJoined result with
skin properties, encrypted continuation into configuration, and the rejection path.
usage: tests/online_test.py [--binary build/painite]
"""
import argparse, hashlib, http.server, json, os, shutil, socket, ssl, struct, subprocess, sys, tempfile, threading, time, urllib.parse
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding
import warnings
warnings.filterwarnings('ignore')
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def vi(n):
    o = b""
    n &= 0xFFFFFFFF
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


def vs(x):
    b = x.encode()
    return vi(len(b)) + b


def mc_digest(data):
    h = int.from_bytes(hashlib.sha1(data).digest(), "big", signed=True)
    return format(h, "x") if h >= 0 else "-" + format(-h, "x")


class Client:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.buf = b""
        self.enc = self.dec = None

    def start_crypto(self, key):
        self.enc = Cipher(algorithms.AES(key), modes.CFB8(key)).encryptor()
        self.dec = Cipher(algorithms.AES(key), modes.CFB8(key)).decryptor()

    def need(self, k):
        while len(self.buf) < k:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("closed")
            self.buf += self.dec.update(d) if self.dec else d
        r, self.buf = self.buf[:k], self.buf[k:]
        return r

    def send(self, pid, data=b""):
        p = vi(pid) + data
        out = vi(len(p)) + p
        self.s.sendall(self.enc.update(out) if self.enc else out)

    def recv(self):
        n = s = 0
        while True:
            b = self.need(1)[0]
            n |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                break
        p = self.need(n)
        pid, i = rvi(p)
        return pid, p[i:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "..", "build", "painite"))
    ap.add_argument("--protocol", type=int, default=777)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="painite-online-")
    srv = None
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    try:
        cert, key = os.path.join(tmp, "c.pem"), os.path.join(tmp, "k.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
        seen = {}

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *x):
                pass

            def do_GET(self):
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                user, sid = q["username"][0], q["serverId"][0]
                seen["req"] = (user, sid)
                if user == "BadGuy" or sid != seen.get("expect"):
                    self.send_response(204)
                    self.end_headers()
                    return
                body = json.dumps({"id": "069a79f444e94726a5befca90e38aaf5", "name": user,
                                   "properties": [{"name": "textures", "value": "dGV4dA==", "signature": "c2ln"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        httpd = http.server.HTTPServer(("127.0.0.1", 28443), H)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        port = 25572
        open(os.path.join(tmp, "server.properties"), "w").write(f"server-port={port}\nonline-mode=true\n")
        open(os.path.join(tmp, "painite.toml"), "w").write(f'[auth]\nsession_host = "localhost:28443"\nsession_ca_file = "{cert}"\n')
        data_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "data"))
        srv = subprocess.Popen([os.path.abspath(a.binary), "--bedrock-port", "0", "--no-bedrock"], cwd=tmp, stdout=open(os.path.join(tmp, "out.log"), "w"),
                               stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
        time.sleep(1.5)

        for name, expect_ok in (("Online1", True), ("BadGuy", False)):
            c = Client(port)
            c.s.sendall(vi(0) + b"")  # placeholder replaced below
            c.s.close()
            c = Client(port)
            hs = vi(0) + vi(a.protocol) + vs("localhost") + struct.pack(">H", port) + vi(2)
            c.s.sendall(vi(len(hs)) + hs)
            c.send(0, vs(name) + b"\0" * 16)
            pid, d = c.recv()
            check(pid == 1, f"{name}: encryption request")
            n, i = rvi(d)
            i += n
            n, i = rvi(d, i)
            pub = d[i:i + n]
            i += n
            n, i = rvi(d, i)
            token = d[i:i + n]
            i += n
            check(d[i] == 1, f"{name}: should-authenticate flag set")
            pk = serialization.load_der_public_key(pub)
            secret = os.urandom(16)
            seen["expect"] = mc_digest(secret + pub)
            c.send(1, vi(128) + pk.encrypt(secret, padding.PKCS1v15()) + vi(128) + pk.encrypt(token, padding.PKCS1v15()))
            c.start_crypto(secret)
            pid, d = c.recv()
            if expect_ok:
                check(pid == 2, f"{name}: login success received over the encrypted stream")
                uuid = d[:16].hex()
                check(uuid == "069a79f444e94726a5befca90e38aaf5", "uuid taken from the session server")
                n, i = rvi(d, 16)
                check(d[i:i + n].decode() == name, "name from session server")
                i += n
                nprops, i = rvi(d, i)
                check(nprops == 1, "skin property forwarded")
                ids = {}
                for line in open(os.path.join(data_dir, "vanilla-26.3.ids")):
                    t = line.split()
                    if t and t[0] == "id":
                        ids[(t[1], t[2], t[3])] = int(t[4])
                c.send(3)
                got = 0
                finished = False
                while not finished:
                    pid, d = c.recv()
                    got += 1
                    if pid == ids[("configuration", "c", "select_known_packs")]:
                        c.send(ids[("configuration", "s", "select_known_packs")], d)
                    elif pid == ids[("configuration", "c", "finish_configuration")]:
                        finished = True
                check(finished and got > 10, f"full configuration ({got} packets) over the encrypted stream")
                c.send(ids[("configuration", "s", "finish_configuration")])
                pid, d = c.recv()
                check(pid == ids[("play", "c", "login")], "play login after configuration")
            else:
                check(pid == 0 and b"Failed to verify username" in d, f"{name}: rejected when session server answers 204")
            c.s.close()
        check(seen.get("req", ("", ""))[0] in ("Online1", "BadGuy"), "session server was queried")
    finally:
        if srv:
            srv.terminate()
            try:
                srv.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()
        print(open(os.path.join(tmp, "out.log")).read()[-600:] if os.path.exists(os.path.join(tmp, "out.log")) else "")
        shutil.rmtree(tmp, ignore_errors=True)
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
