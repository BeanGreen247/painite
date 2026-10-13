#include "lists.hpp"
#include "json.hpp"
#include "util.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace pn {

static const char* kFiles[4] = {"whitelist.json", "banned-players.json", "banned-ips.json", "ops.json"};

static int64_t mtime_of(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 ? static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec : 0;
}

static std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char ch : s) {
        if (ch == '"') o += "\\\"";
        else if (ch == '\\') o += "\\\\";
        else if (ch == '\n') o += "\\n";
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += static_cast<char>(ch);
    }
    return o;
}

std::string Lists::iso_now() {
    time_t t = time(nullptr);
    tm tmv;
    localtime_r(&t, &tmv);
    char b[48];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %z", &tmv);
    return b;
}

static std::string gets(const JV& o, const char* k) {
    const JV* v = o.get(k);
    return v && v->t == JV::Str ? v->s : std::string();
}

bool Lists::read_file(const std::string& path, std::vector<ListEntry>& out, int kind) {
    out.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    JP p{s.data(), s.data() + s.size()};
    JV root = p.val();
    if (p.bad || root.t != JV::Arr) return false;
    for (auto& e : root.a) {
        if (e.t != JV::Obj) continue;
        ListEntry le;
        le.uuid = gets(e, "uuid");
        le.name = gets(e, "name");
        le.ip = gets(e, "ip");
        le.created = gets(e, "created");
        le.source = gets(e, "source");
        std::string ex = gets(e, "expires");
        if (!ex.empty()) le.expires = ex;
        le.reason = gets(e, "reason");
        if (kind == 3) {
            if (const JV* l = e.get("level")) le.level = static_cast<int>(strtol(l->s.c_str(), nullptr, 10));
            if (const JV* b = e.get("bypassesPlayerLimit")) le.bypass = b->t == JV::Bool && b->s == "true";
        }
        out.push_back(std::move(le));
    }
    return true;
}

void Lists::write_file(const std::string& path, const std::vector<ListEntry>& v, int kind) const {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) {
        const ListEntry& e = v[i];
        o += i ? ",\n  {" : "\n  {";
        if (kind == 2) o += "\"ip\": \"" + jesc(e.ip) + "\"";
        else o += "\"uuid\": \"" + jesc(e.uuid) + "\", \"name\": \"" + jesc(e.name) + "\"";
        if (kind == 3) o += ", \"level\": " + std::to_string(e.level) + ", \"bypassesPlayerLimit\": " + (e.bypass ? "true" : "false");
        if (kind == 1 || kind == 2)
            o += ", \"created\": \"" + jesc(e.created) + "\", \"source\": \"" + jesc(e.source) + "\", \"expires\": \"" + jesc(e.expires) + "\", \"reason\": \"" + jesc(e.reason) + "\"";
        o += "}";
    }
    o += v.empty() ? "]\n" : "\n]\n";
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << o;
    }
    rename(tmp.c_str(), path.c_str());
}

void Lists::load_all() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<ListEntry>* t[4] = {&white_, &bans_, &ipbans_, &ops_};
    for (int i = 0; i < 4; i++) {
        read_file(kFiles[i], *t[i], i);
        mt_[i] = mtime_of(kFiles[i]);
    }
}

void Lists::reload_if_changed() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<ListEntry>* t[4] = {&white_, &bans_, &ipbans_, &ops_};
    for (int i = 0; i < 4; i++) {
        int64_t m = mtime_of(kFiles[i]);
        if (m != mt_[i]) {
            std::vector<ListEntry> v;
            if (read_file(kFiles[i], v, i)) *t[i] = std::move(v);
            mt_[i] = m;
        }
    }
}

static bool name_eq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

static bool expired(const ListEntry& e) {
    if (e.expires.empty() || e.expires == "forever") return false;
    tm t{};
    if (!strptime(e.expires.c_str(), "%Y-%m-%d %H:%M:%S", &t)) return false;
    t.tm_isdst = -1;
    return mktime(&t) < time(nullptr);
}

bool Lists::whitelisted(const std::string& name, const std::string& uuid) const {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : white_) if ((!uuid.empty() && e.uuid == uuid) || name_eq(e.name, name)) return true;
    return false;
}

bool Lists::whitelist_add(const std::string& name, const std::string& uuid) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : white_) if (name_eq(e.name, name)) return false;
    ListEntry e;
    e.name = name;
    e.uuid = uuid;
    white_.push_back(e);
    write_file(kFiles[0], white_, 0);
    mt_[0] = mtime_of(kFiles[0]);
    return true;
}

bool Lists::whitelist_remove(const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::remove_if(white_.begin(), white_.end(), [&](const ListEntry& e) { return name_eq(e.name, name); });
    if (it == white_.end()) return false;
    white_.erase(it, white_.end());
    write_file(kFiles[0], white_, 0);
    mt_[0] = mtime_of(kFiles[0]);
    return true;
}

std::vector<std::string> Lists::whitelist_names() const {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<std::string> o;
    for (auto& e : white_) o.push_back(e.name);
    return o;
}

bool Lists::banned(const std::string& name, const std::string& uuid, std::string* reason) const {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : bans_)
        if (((!uuid.empty() && e.uuid == uuid) || name_eq(e.name, name)) && !expired(e)) {
            if (reason) *reason = e.reason;
            return true;
        }
    return false;
}

bool Lists::ip_banned(const std::string& ip, std::string* reason) const {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : ipbans_)
        if (e.ip == ip && !expired(e)) {
            if (reason) *reason = e.reason;
            return true;
        }
    return false;
}

bool Lists::ban_add(const std::string& name, const std::string& uuid, const std::string& source, const std::string& reason) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : bans_) if (name_eq(e.name, name)) return false;
    ListEntry e;
    e.name = name;
    e.uuid = uuid;
    e.created = iso_now();
    e.source = source;
    e.reason = reason.empty() ? "Banned by an operator." : reason;
    bans_.push_back(e);
    write_file(kFiles[1], bans_, 1);
    mt_[1] = mtime_of(kFiles[1]);
    return true;
}

bool Lists::ban_ip_add(const std::string& ip, const std::string& source, const std::string& reason) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : ipbans_) if (e.ip == ip) return false;
    ListEntry e;
    e.ip = ip;
    e.created = iso_now();
    e.source = source;
    e.reason = reason.empty() ? "Banned by an operator." : reason;
    ipbans_.push_back(e);
    write_file(kFiles[2], ipbans_, 2);
    mt_[2] = mtime_of(kFiles[2]);
    return true;
}

bool Lists::pardon(const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::remove_if(bans_.begin(), bans_.end(), [&](const ListEntry& e) { return name_eq(e.name, name); });
    if (it == bans_.end()) return false;
    bans_.erase(it, bans_.end());
    write_file(kFiles[1], bans_, 1);
    mt_[1] = mtime_of(kFiles[1]);
    return true;
}

bool Lists::pardon_ip(const std::string& ip) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::remove_if(ipbans_.begin(), ipbans_.end(), [&](const ListEntry& e) { return e.ip == ip; });
    if (it == ipbans_.end()) return false;
    ipbans_.erase(it, ipbans_.end());
    write_file(kFiles[2], ipbans_, 2);
    mt_[2] = mtime_of(kFiles[2]);
    return true;
}

std::vector<ListEntry> Lists::bans() const { std::lock_guard<std::mutex> l(mu_); return bans_; }
std::vector<ListEntry> Lists::ip_bans() const { std::lock_guard<std::mutex> l(mu_); return ipbans_; }

int Lists::op_level(const std::string& name, const std::string& uuid) const {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : ops_) if ((!uuid.empty() && e.uuid == uuid) || name_eq(e.name, name)) return e.level;
    return 0;
}

bool Lists::op_add(const std::string& name, const std::string& uuid, int level) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& e : ops_) if (name_eq(e.name, name)) return false;
    ListEntry e;
    e.name = name;
    e.uuid = uuid;
    e.level = level;
    ops_.push_back(e);
    write_file(kFiles[3], ops_, 3);
    mt_[3] = mtime_of(kFiles[3]);
    return true;
}

bool Lists::op_remove(const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::remove_if(ops_.begin(), ops_.end(), [&](const ListEntry& e) { return name_eq(e.name, name); });
    if (it == ops_.end()) return false;
    ops_.erase(it, ops_.end());
    write_file(kFiles[3], ops_, 3);
    mt_[3] = mtime_of(kFiles[3]);
    return true;
}

std::vector<std::string> Lists::op_names() const {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<std::string> o;
    for (auto& e : ops_) o.push_back(e.name);
    return o;
}

}  // namespace pn
