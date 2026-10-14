#include "jclient.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

namespace pt {

std::shared_ptr<Ids> Ids::load(const std::string& path) {
    auto ids = std::make_shared<Ids>();
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "id") {
            std::string st, dir, name;
            int n;
            ls >> st >> dir >> name >> n;
            ids->id[st + "/" + dir + "/" + name] = n;
            ids->names[st + "/" + dir][n] = name;
        } else if (kind == "reg") {
            std::string r, name;
            int n;
            ls >> r >> name >> n;
            if (r == "item") ids->items[name] = n;
            else if (r == "entity_type") ids->entity_types[name] = n;
        } else if (kind == "protocol") ls >> ids->protocol;
    }
    return ids;
}

int Ids::operator()(const char* state, const char* dir, const std::string& name) const {
    auto it = id.find(std::string(state) + "/" + dir + "/" + name);
    if (it == id.end()) throw std::runtime_error("unknown packet " + std::string(state) + "/" + dir + "/" + name);
    return it->second;
}

std::string Ids::name(const char* state, const char* dir, int pid) const {
    auto a = names.find(std::string(state) + "/" + dir);
    if (a == names.end()) return std::to_string(pid);
    auto b = a->second.find(pid);
    return b == a->second.end() ? std::to_string(pid) : b->second;
}

JClient::~JClient() {
    close();
    if (enc_) EVP_CIPHER_CTX_free(enc_);
    if (dec_) EVP_CIPHER_CTX_free(dec_);
}

void JClient::connect(int port, int timeout_ms) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int64_t end = now_ms() + timeout_ms;
    while (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        if (now_ms() > end) throw std::runtime_error("cannot connect to port " + std::to_string(port));
        ::close(fd_);
        usleep(20000);
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    }
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void JClient::close() {
    if (fd_ >= 0) {
        if (getenv("PT_RST")) { linger l{1, 0}; setsockopt(fd_, SOL_SOCKET, SO_LINGER, &l, sizeof l); }   // reset instead of a clean close
        ::close(fd_);
        fd_ = -1;
    }
}

void JClient::write_all(const Bytes& in) {
    Bytes b = in;
    if (enc_) {
        int n = 0;
        EVP_EncryptUpdate(enc_, b.data(), &n, in.data(), int(in.size()));
    }
    size_t off = 0;
    while (off < b.size()) {
        ssize_t n = ::send(fd_, b.data() + off, b.size() - off, MSG_NOSIGNAL);
        if (n <= 0) { if (errno == EINTR) continue; throw std::runtime_error("send failed"); }
        off += size_t(n);
    }
}

void JClient::send(int pid, const Bytes& data) {
    Wr p;
    p.varint(uint32_t(pid)).raw(data);
    Wr f;
    if (threshold >= 0) { Wr q; q.varint(0).raw(p.b); f.varint(uint32_t(q.b.size())).raw(q.b); }
    else f.varint(uint32_t(p.b.size())).raw(p.b);
    write_all(f.b);
}

bool JClient::fill(size_t need, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    while (buf_.size() - pos_ < need) {
        int left = int(end - now_ms());
        if (left < 0) left = 0;
        pollfd pf{fd_, POLLIN, 0};
        int r = ::poll(&pf, 1, left);
        if (r == 0) return false;
        if (r < 0) { if (errno == EINTR) continue; throw std::runtime_error("poll failed"); }
        uint8_t tmp[65536];
        ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n <= 0) { eof = true; throw std::runtime_error("connection closed"); }
        if (dec_) {
            int o = 0;
            EVP_DecryptUpdate(dec_, tmp, &o, tmp, int(n));
        }
        if (pos_ > 0 && pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
        buf_.insert(buf_.end(), tmp, tmp + n);
    }
    return true;
}

bool JClient::recv(Packet& out, int timeout_ms) {
    // length prefix: read byte by byte but only commit when the whole frame is there
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        size_t avail = buf_.size() - pos_;
        uint32_t len = 0;
        size_t hdr = 0;
        bool have_len = false;
        for (size_t i = 0; i < avail && i < 5; i++) {
            uint8_t c = buf_[pos_ + i];
            len |= uint32_t(c & 0x7f) << (7 * i);
            if (!(c & 0x80)) { hdr = i + 1; have_len = true; break; }
        }
        if (have_len && avail >= hdr + len) {
            Rd r(buf_.data() + pos_ + hdr, len);
            Bytes body(buf_.begin() + long(pos_ + hdr), buf_.begin() + long(pos_ + hdr + len));
            pos_ += hdr + len;
            if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
            if (threshold >= 0) {
                Rd br(body);
                uint32_t dl = br.varint();
                size_t off = size_t(br.p - body.data());
                if (dl == 0) body.erase(body.begin(), body.begin() + long(off));
                else {
                    Bytes un(dl);
                    uLongf n = dl;
                    if (uncompress(un.data(), &n, body.data() + off, uLong(body.size() - off)) != Z_OK) throw std::runtime_error("bad zlib packet");
                    un.resize(n);
                    body.swap(un);
                }
            }
            Rd pr(body);
            out.id = int(pr.varint());
            out.d.assign(pr.p, pr.e);
            return true;
        }
        int left = int(end - now_ms());
        if (left <= 0) return false;
        size_t want = have_len ? hdr + len - avail : 1;
        if (!fill(avail + want, left)) return false;
    }
}

void JClient::start_crypto(const uint8_t key[16]) {
    enc_ = EVP_CIPHER_CTX_new();
    dec_ = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(enc_, EVP_aes_128_cfb8(), nullptr, key, key);
    EVP_DecryptInit_ex(dec_, EVP_aes_128_cfb8(), nullptr, key, key);
}

Bytes offline_uuid(const std::string& name) {
    std::string in = "OfflinePlayer:" + name;
    uint8_t h[16];
    unsigned hl = 0;
    EVP_Digest(in.data(), in.size(), h, &hl, EVP_md5(), nullptr);
    h[6] = (h[6] & 0x0f) | 0x30;
    h[8] = (h[8] & 0x3f) | 0x80;
    return Bytes(h, h + 16);
}

Bytes java_login(JClient& c, int port, int protocol, const std::string& name, const Bytes& uuid16) {
    c.connect(port);
    Wr hs;
    hs.varint(uint32_t(protocol)).str("localhost").be16(uint16_t(port)).varint(2);
    c.send(0, hs.b);
    Wr ls;
    ls.str(name).raw(uuid16);
    c.send(0, ls.b);
    Bytes success;
    for (;;) {
        Packet p;
        if (!c.recv(p, 15000)) throw std::runtime_error("login timed out");
        if (p.id == 3) { Rd r(p.d); c.threshold = int(r.varint()); }
        else if (p.id == 2) { success = p.d; break; }
        else if (p.id == 0) throw std::runtime_error("login refused: " + std::string(p.d.begin(), p.d.end()));
    }
    c.send(3);
    return success;
}

int java_configure(JClient& c, const Ids& ids) {
    int n = 0;
    for (;;) {
        Packet p;
        if (!c.recv(p, 15000)) throw std::runtime_error("configuration timed out");
        n++;
        if (p.id == ids("configuration", "c", "select_known_packs")) c.send(ids("configuration", "s", "select_known_packs"), p.d);
        else if (p.id == ids("configuration", "c", "keep_alive")) c.send(ids("configuration", "s", "keep_alive"), p.d);
        else if (p.id == ids("configuration", "c", "finish_configuration")) { c.send(ids("configuration", "s", "finish_configuration")); return n; }
    }
}

}  // namespace pt
