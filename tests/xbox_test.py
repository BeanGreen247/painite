#!/usr/bin/env python3 -I
"""Bedrock online mode against a local fake Minecraft authorization service (HTTPS, self-signed CA, RS256 JWKS).
A token signed with the fake key and bound to the client's key logs in; a forged, expired or unbound token is refused.
usage: tests/xbox_test.py [--binary build/painite]
"""
import argparse, base64, http.server, json, os, shutil, ssl, struct, subprocess, sys, tempfile, threading, time, uuid
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import bedrockclient as bc
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

HOST = "localhost:28444"


def b64u(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def login(c, name, token_for, server):
    """Like BedrockClient.login but with a caller supplied identity token."""
    key = c.key = ec.generate_private_key(ec.SECP384R1())
    pub = base64.b64encode(key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)).decode()

    def jwt(payload):
        h = b64u(json.dumps({"alg": "ES384", "x5u": pub}).encode())
        p = b64u(json.dumps(payload).encode())
        r, s = decode_dss_signature(key.sign(f"{h}.{p}".encode(), ec.ECDSA(hashes.SHA384())))
        return f"{h}.{p}." + b64u(r.to_bytes(48, "big") + s.to_bytes(48, "big"))

    client = jwt({**bc.default_skin(), "ClientRandomId": int(time.time() * 1000), "CurrentInputMode": 1, "DefaultInputMode": 1, "DeviceId": str(uuid.uuid4()),
                  "DeviceModel": "PainiteBot", "DeviceOS": 12, "GameVersion": "1.26.51", "GuiScale": -1, "LanguageCode": "en_GB", "ServerAddress": server,
                  "ThirdPartyName": name, "SelfSignedId": str(uuid.uuid4()), "PlatformOfflineId": "", "PlatformOnlineId": "", "PlayFabId": "0", "UIProfile": 0})
    auth = json.dumps({"Certificate": json.dumps({"chain": [""]}), "AuthenticationType": 0, "Token": token_for(pub)}).encode()
    cl = client.encode()
    inner = struct.pack("<I", len(auth)) + auth + struct.pack("<I", len(cl)) + cl
    c.send_packets([(1, struct.pack(">i", c.protocol) + bc.varuint(len(inner)) + inner)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "painite"))
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="painite-xbox-")
    srv = None
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    try:
        cert, keyf = os.path.join(tmp, "c.pem"), os.path.join(tmp, "k.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", keyf, "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
        signing = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        other = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        nums = signing.public_key().public_numbers()
        jwk = {"kty": "RSA", "use": "sig", "kid": "TESTKEY", "n": b64u(nums.n.to_bytes(256, "big")), "e": b64u(nums.e.to_bytes(3, "big"))}

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *x):
                pass

            def do_GET(self):
                body = json.dumps({"keys": [jwk]}).encode() if self.path.endswith("/.well-known/keys") else b"{}"
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        httpd = http.server.HTTPServer(("127.0.0.1", 28444), H)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, keyf)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        def token(key, gamertag, xuid, cpk_override=None, exp_delta=3600, iss=None):
            def tok(pub):
                now = int(time.time())
                h = b64u(json.dumps({"alg": "RS256", "kid": "TESTKEY", "typ": "JWT"}).encode())
                p = b64u(json.dumps({"iss": iss or f"https://{HOST}/", "aud": "api://auth-minecraft-services/multiplayer", "xid": xuid, "xname": gamertag,
                                     "cpk": cpk_override or pub, "nbf": now - 10, "iat": now, "exp": now + exp_delta}).encode())
                sig = key.sign(f"{h}.{p}".encode(), padding.PKCS1v15(), hashes.SHA256())
                return f"{h}.{p}.{b64u(sig)}"
            return tok

        port = 28445
        open(os.path.join(tmp, "server.properties"), "w").write(f"server-port={port}\nonline-mode=true\n")
        open(os.path.join(tmp, "painite.toml"), "w").write(f'[auth]\nxbox_host = "{HOST}"\nsession_ca_file = "{cert}"\n')
        data_dir = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data"))
        os.symlink(data_dir, os.path.join(tmp, "data"))
        srv = subprocess.Popen([os.path.abspath(a.binary)], cwd=tmp, stdout=open(os.path.join(tmp, "out.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
        for _ in range(50):
            time.sleep(0.2)
            if "Done" in open(os.path.join(tmp, "out.log")).read():
                break

        def attempt(label, tok, expect_ok):
            c = bc.BedrockClient("127.0.0.1", port)
            c.connect()
            c.request_settings()
            login(c, "Xbx" + label[:4], tok, f"127.0.0.1:{port}")
            got_ok = got_bad = False
            msg = b""
            end = time.time() + 8
            while time.time() < end and not (got_ok or got_bad):
                pk = c.recv_packets(0.5)
                for pid, pl in pk or []:
                    if pid == 3:
                        got_ok = True   # server-to-client handshake means the login was accepted
                    elif pid == 5:
                        got_bad = True
                        msg = pl
            check(got_ok == expect_ok and got_bad == (not expect_ok), f"{label}: {'accepted' if expect_ok else 'refused'}" + (f" ({msg[3:].decode(errors='replace').strip()[:70]})" if got_bad else ""))
            c.s.close()

        attempt("valid", token(signing, "RealGamer", "2535400000000001"), True)
        attempt("forged signature", token(other, "Forger", "2535400000000002"), False)
        attempt("expired", token(signing, "Slowpoke", "2535400000000003", exp_delta=-7200), False)
        attempt("unbound key", token(signing, "Thief", "2535400000000004", cpk_override="MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEfake"), False)
        attempt("wrong issuer", token(signing, "Impostor", "2535400000000005", iss="https://evil.example/"), False)
    finally:
        if srv:
            srv.terminate()
            try:
                srv.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()
        log = os.path.join(tmp, "out.log")
        if os.path.exists(log) and "-v" in sys.argv:
            print(open(log).read()[-800:])
        shutil.rmtree(tmp, ignore_errors=True)
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()
