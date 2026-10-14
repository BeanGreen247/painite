"""Small Bedrock Edition test client: RakNet session layer, batch/compression, offline (self-signed) login.
Used by tools/bedrock_capture.py (record a real Bedrock Dedicated Server) (the test suite has its own C++ client in tests/cpp/bclient.cpp).
Standard library plus the `cryptography` package (for the ES384 login JWTs).
"""
import base64, json, os, random, socket, struct, time, uuid, zlib

MAGIC = bytes.fromhex("00ffff00fefefefefdfdfdfd12345678")


def varuint(n):
    o = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            o += bytes([b | 0x80])
        else:
            return o + bytes([b])


def read_varuint(d, i=0):
    n = s = 0
    while True:
        b = d[i]
        i += 1
        n |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return n, i


def zigzag(n):
    return (n << 1) ^ (n >> 31) if n >= 0 else ((-n) * 2 - 1)


def _addr7(ip="127.0.0.1", port=0):
    return bytes([4]) + bytes((~int(x)) & 0xFF for x in ip.split(".")) + struct.pack(">H", port)


def default_skin():
    """The stock Steve persona skin fields (a Mojang asset, so it is downloaded on demand and not stored in the repo)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "steve-skin.json")
    if not os.path.exists(path):
        import urllib.request
        url = "https://raw.githubusercontent.com/PrismarineJS/minecraft-data/master/data/bedrock/1.21.70/steve.json"
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with urllib.request.urlopen(url, timeout=60) as r:
            open(path, "wb").write(r.read())
    return json.load(open(path))


class RakClient:
    def __init__(self, host, port, mtu=1400, log=None):
        self.addr = (host, port)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(5)
        self.guid = random.getrandbits(63)
        self.mtu = mtu
        self.seq = 0
        self.rel = 0
        self.ordidx = 0
        self.split_id = 0
        self.splits = {}
        self.game = []          # received 0xfe payloads
        self.connected = False
        self.closed = False
        self.log = log or (lambda *a: None)
        self.sent = {}          # datagram seq -> bytes (for NACK resend)
        self.seen_rel = set()   # reliable frame indexes already processed (resends carry the same index)
        self.next_order = 0
        self.order_buf = {}

    def _recv_raw(self, timeout):
        self.s.settimeout(timeout)
        try:
            return self.s.recvfrom(4096)[0]
        except socket.timeout:
            return None

    def connect(self):
        pad = self.mtu - 28 - 1 - 16 - 1
        d = None
        for _ in range(8):          # real RakNet clients retry the open connection requests
            self.s.sendto(b"\x05" + MAGIC + bytes([11]) + b"\0" * pad, self.addr)
            d = self._recv_raw(1.0)
            if d and d[0] == 0x06:
                break
        if not d or d[0] != 0x06:
            raise RuntimeError(f"no open connection reply 1: {d[:1].hex() if d else None}")
        self.mtu = min(self.mtu, struct.unpack(">H", d[-2:])[0])
        d = None
        for _ in range(8):
            self.s.sendto(b"\x07" + MAGIC + _addr7(self.addr[0], self.addr[1]) + struct.pack(">H", self.mtu) + struct.pack(">q", self.guid), self.addr)
            d = self._recv_raw(1.0)
            if d and d[0] == 0x08:
                break
        if not d or d[0] != 0x08:
            raise RuntimeError(f"no open connection reply 2: {d[:1].hex() if d else None}")
        self.send_frame(b"\x09" + struct.pack(">q", self.guid) + struct.pack(">q", int(time.time() * 1000)) + b"\0", 3)
        end = time.time() + 15
        while not self.connected and time.time() < end:
            self.poll(0.5)
        if not self.connected:
            raise RuntimeError("RakNet connection request not accepted")

    # ---- framing ----
    def _frame(self, payload, rel, split=None):
        h = bytes([(rel << 5) | (0x10 if split else 0)]) + struct.pack(">H", len(payload) * 8)
        if rel in (2, 3, 4, 6, 7):
            h += struct.pack("<I", self.rel)[:3]
            self.rel += 1
        if rel in (1, 4):
            h += b"\0\0\0"
        if rel in (1, 3, 4, 7):
            h += struct.pack("<I", self.ordidx)[:3] + b"\0"
        if split:
            h += struct.pack(">IHI", *split)
        return h + payload

    def _send_datagram(self, frames):
        d = b"\x84" + struct.pack("<I", self.seq)[:3] + frames
        self.sent[self.seq] = (d, time.time())
        self.seq += 1
        self.s.sendto(d, self.addr)

    def send_frame(self, payload, rel=3):
        maxp = self.mtu - 28 - 4 - 24
        if len(payload) <= maxp:
            self._send_datagram(self._frame(payload, rel))
            if rel in (1, 3, 4, 7):
                self.ordidx += 1
            return
        parts = [payload[i:i + maxp] for i in range(0, len(payload), maxp)]
        sid = self.split_id
        self.split_id = (self.split_id + 1) & 0xFFFF
        for i, p in enumerate(parts):
            self._send_datagram(self._frame(p, rel, (len(parts), sid, i)))
        if rel in (1, 3, 4, 7):
            self.ordidx += 1

    def _ack(self, seqs):
        out = b"\xc0" + struct.pack(">H", len(seqs))
        for q in seqs:
            out += b"\x01" + struct.pack("<I", q)[:3]
        self.s.sendto(out, self.addr)

    def _handle_payload(self, p):
        if not p:
            return
        t = p[0]
        if t == 0xFE:
            self.game.append(p[1:])
        elif t == 0x00:
            self.send_frame(b"\x03" + p[1:9] + struct.pack(">q", int(time.time() * 1000)), 0)
        elif t == 0x10:
            self.send_frame(b"\x13" + _addr7(self.addr[0], self.addr[1]) + _addr7() * 10 + struct.pack(">qq", 0, int(time.time() * 1000)), 3)
            self.connected = True
        elif t == 0x15:
            self.closed = True
        else:
            self.log("raknet internal", hex(t))

    def _resend_stale(self):
        now = time.time()
        for q, (d, t) in list(self.sent.items()):
            if now - t > 0.6:
                self.s.sendto(d, self.addr)
                self.sent[q] = (d, now)

    def poll(self, timeout=0.5):
        self._resend_stale()
        d = self._recv_raw(timeout)
        if d is None:
            return False
        t = d[0]
        if t == 0xC0:
            cnt = struct.unpack(">H", d[1:3])[0]
            i = 3
            for _ in range(cnt):
                single = d[i]
                lo = struct.unpack("<I", d[i + 1:i + 4] + b"\0")[0]
                hi = lo
                i += 4
                if not single:
                    hi = struct.unpack("<I", d[i:i + 3] + b"\0")[0]
                    i += 3
                for q in range(lo, hi + 1):
                    self.sent.pop(q, None)
            return True
        if t == 0xA0:  # NACK: resend what the server missed
            cnt = struct.unpack(">H", d[1:3])[0]
            i = 3
            for _ in range(cnt):
                single = d[i]
                lo = struct.unpack("<I", d[i + 1:i + 4] + b"\0")[0]
                hi = lo
                i += 4
                if not single:
                    hi = struct.unpack("<I", d[i:i + 3] + b"\0")[0]
                    i += 3
                for q in range(lo, hi + 1):
                    if q in self.sent:
                        self.s.sendto(self.sent[q][0], self.addr)
            return True
        if not t & 0x80:
            return True
        seq = struct.unpack("<I", d[1:4] + b"\0")[0]
        self._ack([seq])
        i = 4
        while i < len(d):
            fl = d[i]
            rel = fl >> 5
            split = bool(fl & 0x10)
            ln = (struct.unpack(">H", d[i + 1:i + 3])[0] + 7) // 8
            i += 3
            rel_idx = None
            if rel in (2, 3, 4, 6, 7):
                rel_idx = struct.unpack("<I", d[i:i + 3] + b"\0")[0]
                i += 3
            if rel in (1, 4):
                i += 3
            order_idx = None
            if rel in (1, 3, 4, 7):
                order_idx = struct.unpack("<I", d[i:i + 3] + b"\0")[0]
                i += 4
            sp = None
            if split:
                sp = struct.unpack(">IHI", d[i:i + 10])
                i += 10
            body = d[i:i + ln]
            i += ln
            if rel_idx is not None:
                if rel_idx in self.seen_rel:
                    continue
                self.seen_rel.add(rel_idx)
            if sp:
                cnt, sid, idx = sp
                ent = self.splits.setdefault(sid, {"n": cnt, "p": {}})
                ent["p"][idx] = body
                if len(ent["p"]) == ent["n"]:
                    body = b"".join(ent["p"][k] for k in range(ent["n"]))
                    del self.splits[sid]
                else:
                    continue
            if order_idx is None:
                self._handle_payload(body)
            else:
                self.order_buf[order_idx] = body
                while self.next_order in self.order_buf:
                    self._handle_payload(self.order_buf.pop(self.next_order))
                    self.next_order += 1
        return True


class BedrockClient(RakClient):
    def __init__(self, host, port, protocol=2193, **kw):
        super().__init__(host, port, **kw)
        self.protocol = protocol
        self.compress = False
        self.threshold = 0
        self.algo = 0
        self.pending = []
        self.enc = self.dec = None
        self.enc_key = None
        self.send_count = self.recv_count = 0

    def start_encryption(self, handshake_jwt):
        """Handle ServerToClientHandshake: ECDH(P-384) + salt -> AES-256-CTR with a per-batch SHA-256 trailer."""
        import hashlib
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        h, p, _ = handshake_jwt.split(".")
        pad = lambda x: x + "=" * (-len(x) % 4)
        header = json.loads(base64.urlsafe_b64decode(pad(h)))
        salt = base64.b64decode(json.loads(base64.urlsafe_b64decode(pad(p)))["salt"])
        server_pub = serialization.load_der_public_key(base64.b64decode(header["x5u"]))
        secret = self.key.exchange(ec.ECDH(), server_pub)
        self.enc_key = hashlib.sha256(salt + secret).digest()
        iv = self.enc_key[:12] + b"\0\0\0\2"
        self.enc = Cipher(algorithms.AES(self.enc_key), modes.CTR(iv)).encryptor()
        self.dec = Cipher(algorithms.AES(self.enc_key), modes.CTR(iv)).decryptor()

    def _trailer(self, data, counter):
        import hashlib
        return hashlib.sha256(struct.pack("<Q", counter) + data + self.enc_key).digest()[:8]

    def send_packets(self, pkts):
        body = b""
        for pid, pl in pkts:
            pk = varuint(pid) + pl
            body += varuint(len(pk)) + pk
        if self.compress:
            if len(body) >= self.threshold and self.algo == 0:
                c = zlib.compressobj(wbits=-15)
                data = b"\0" + c.compress(body) + c.flush()
            else:
                data = b"\xff" + body
            if self.enc:
                data = self.enc.update(data + self._trailer(data, self.send_count))
                self.send_count += 1
            self.send_frame(b"\xfe" + data, 3)
        else:
            self.send_frame(b"\xfe" + body, 3)

    def recv_packets(self, timeout=5):
        """Return decoded (id, payload) tuples from the next batch, or None on timeout."""
        end = time.time() + timeout
        while not self.game and time.time() < end and not self.closed:
            self.poll(0.2)
        if not self.game:
            return None
        data = self.game.pop(0)
        if self.dec:
            data = self.dec.update(data)[:-8]   # trailer is not verified here
            self.recv_count += 1
        if self.compress:
            algo, data = data[0], data[1:]
            if algo == 0:
                data = zlib.decompressobj(-15).decompress(data)
            elif algo != 0xFF:
                raise RuntimeError(f"unsupported compression {algo}")
        out = []
        i = 0
        while i < len(data):
            ln, i = read_varuint(data, i)
            pk = data[i:i + ln]
            i += ln
            hdr, j = read_varuint(pk)
            out.append((hdr & 0x3FF, pk[j:]))
        return out

    def request_settings(self):
        self.send_packets([(193, struct.pack(">i", self.protocol))])
        pk = self.recv_packets()
        if not pk or pk[0][0] != 143:
            raise RuntimeError(f"expected NetworkSettings, got {pk[0][0] if pk else None}")
        d = pk[0][1]
        self.threshold, self.algo = struct.unpack("<HH", d[:4])
        self.compress = True
        return d

    def login(self, name="PainiteBot", server="127.0.0.1:19132"):
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
        key = self.key = ec.generate_private_key(ec.SECP384R1())
        pub = base64.b64encode(key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)).decode()

        def b64u(b):
            return base64.urlsafe_b64encode(b).rstrip(b"=").decode()

        def jwt(payload):
            h = b64u(json.dumps({"alg": "ES384", "x5u": pub}).encode())
            p = b64u(json.dumps(payload).encode())
            r, s = decode_dss_signature(key.sign(f"{h}.{p}".encode(), ec.ECDSA(hashes.SHA384())))
            return f"{h}.{p}." + b64u(r.to_bytes(48, "big") + s.to_bytes(48, "big"))

        now = int(time.time())
        ident = str(uuid.uuid4())
        # protocol >= 944 "OpenID offline" login, as used by PrismarineJS/bedrock-protocol: the identity is a
        # self-signed multiplayer token, the certificate chain is empty
        def jwt_claims(payload):
            return jwt(payload)

        token = jwt({"cpk": pub, "xid": "0", "xname": name, "identity": ident, "leguuid": ident, "mid": "", "iss": "self",
                     "aud": "api://auth-minecraft-services/multiplayer", "nbf": now, "iat": now, "exp": now + 3600})
        client = jwt({**default_skin(),
                      "ClientRandomId": int(time.time() * 1000), "CurrentInputMode": 1, "DefaultInputMode": 1, "DeviceId": str(uuid.uuid4()),
                      "DeviceModel": "PainiteBot", "DeviceOS": 12, "FilterProfanity": False, "GameVersion": "1.26.51", "GuiScale": -1,
                      "LanguageCode": "en_GB", "GraphicsMode": 1, "PlatformOfflineId": "", "PlatformOnlineId": "",
                      "PlayFabId": uuid.uuid4().hex[:16], "SelfSignedId": str(uuid.uuid4()), "ServerAddress": server, "ThirdPartyName": name,
                      "UIProfile": 0, "IsEditorMode": False, "ClientIsEditorCapable": False, "ClientEditorConnectionIntent": 0,
                      "TrustedSkin": False, "OverrideSkin": False, "CompatibleWithClientSideChunkGen": False, "MaxViewDistance": 0,
                      "MemoryTier": 0, "PlatformType": 0})
        auth = json.dumps({"Certificate": json.dumps({"chain": [""]}), "AuthenticationType": 2, "Token": token}).encode()
        cl = client.encode()
        inner = struct.pack("<I", len(auth)) + auth + struct.pack("<I", len(cl)) + cl
        self.send_packets([(1, struct.pack(">i", self.protocol) + varuint(len(inner)) + inner)])
        return ident
