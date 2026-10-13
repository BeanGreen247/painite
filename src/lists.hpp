// whitelist.json, banned-players.json, banned-ips.json and ops.json in the vanilla formats (read, edit from commands, write back).
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pn {

struct ListEntry {
    std::string uuid, name;        // uuid with dashes (may be empty when only a name is known)
    std::string created, source, expires = "forever", reason;  // bans
    int level = 4;                 // ops
    bool bypass = false;           // ops: bypassesPlayerLimit
    std::string ip;                // banned-ips
};

class Lists {
public:
    void load_all();
    void reload_if_changed();
    // whitelist
    bool whitelisted(const std::string& name, const std::string& uuid) const;
    bool whitelist_add(const std::string& name, const std::string& uuid);
    bool whitelist_remove(const std::string& name);
    std::vector<std::string> whitelist_names() const;
    // bans
    bool banned(const std::string& name, const std::string& uuid, std::string* reason = nullptr) const;
    bool ip_banned(const std::string& ip, std::string* reason = nullptr) const;
    bool ban_add(const std::string& name, const std::string& uuid, const std::string& source, const std::string& reason);
    bool ban_ip_add(const std::string& ip, const std::string& source, const std::string& reason);
    bool pardon(const std::string& name);
    bool pardon_ip(const std::string& ip);
    std::vector<ListEntry> bans() const;
    std::vector<ListEntry> ip_bans() const;
    // ops
    int op_level(const std::string& name, const std::string& uuid) const;  // 0 if not an operator
    bool op_add(const std::string& name, const std::string& uuid, int level);
    bool op_remove(const std::string& name);
    std::vector<std::string> op_names() const;

private:
    static std::string iso_now();
    bool read_file(const std::string& path, std::vector<ListEntry>& out, int kind);
    void write_file(const std::string& path, const std::vector<ListEntry>& v, int kind) const;
    mutable std::mutex mu_;
    std::vector<ListEntry> white_, bans_, ipbans_, ops_;
    int64_t mt_[4] = {0, 0, 0, 0};
};

}  // namespace pn
