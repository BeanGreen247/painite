// Block state registry. Internal block ids are the state ids of the newest loaded Java version; older versions map by name+properties.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {

struct BlockInfo {
    std::string name;       // minecraft:oak_log
    std::string props;      // axis=y (sorted, comma separated)
    uint8_t opacity = 15;   // light attenuation: 0 transparent, 15 opaque
    bool air = false, fluid = false, solid = true;
    bool replaceable = false;
    uint8_t emit = 0;       // block light emitted
};

class Blocks {
public:
    static Blocks& get();
    // files: vanilla-<ver>.blocks paths, protocol per file; the first (newest) defines internal ids
    bool load(const std::vector<std::pair<int, std::string>>& files, std::string& err);
    bool ready() const { return !info_.empty(); }
    size_t count() const { return info_.size(); }
    const BlockInfo& info(uint16_t id) const { return info_[id < info_.size() ? id : 0]; }
    // name+props -> internal id; unknown props fall back to the default state of that block, unknown block to air
    uint16_t find(const std::string& name, const std::string& props) const;
    uint16_t default_state(const std::string& name) const;   // 0 if unknown
    bool known(const std::string& name) const { return defaults_.count(name) != 0; }
    std::string key(uint16_t id) const { const auto& b = info(id); return b.props.empty() ? b.name : b.name + "[" + b.props + "]"; }
    // protocol state id for an internal id
    int proto_state(int protocol, uint16_t id) const;
    int proto_total(int protocol) const { auto it = total_.find(protocol); return it == total_.end() ? 0 : it->second; }
    uint16_t air() const { return 0; }
    uint16_t stone() const { return stone_; }

private:
    std::vector<BlockInfo> info_;
    std::unordered_map<std::string, uint16_t> by_key_;      // name[props] or name
    std::unordered_map<std::string, uint16_t> defaults_;
    std::unordered_map<int, std::vector<int>> proto_;       // protocol -> internal id -> state id
    std::unordered_map<int, int> total_;
    uint16_t stone_ = 1;
};

}  // namespace pn
