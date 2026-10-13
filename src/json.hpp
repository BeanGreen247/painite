// Minimal JSON reader (enough for session-server replies and Bedrock login tokens).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace pn {

struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    std::string s;
    std::vector<JV> a;
    std::vector<std::pair<std::string, JV>> o;
    const JV* get(const char* k) const {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct JP {
    const char* p;
    const char* e;
    bool bad = false;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) { o += static_cast<char>(0xc0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += static_cast<char>(0xe0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 63)); o += static_cast<char>(0x80 | (c & 63)); }
        else { o += static_cast<char>(0xf0 | (c >> 18)); o += static_cast<char>(0x80 | ((c >> 12) & 63)); o += static_cast<char>(0x80 | ((c >> 6) & 63)); o += static_cast<char>(0x80 | (c & 63)); }
    }
    std::string str() {
        std::string o;
        if (p >= e || *p != '"') { bad = true; return o; }
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': o += '\r'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case 'u': {
                        if (e - p < 5) { bad = true; return o; }
                        uint32_t c = static_cast<uint32_t>(strtoul(std::string(p + 1, 4).c_str(), nullptr, 16));
                        p += 4;
                        utf8(o, c);
                        break;
                    }
                    default: o += *p;
                }
                p++;
            } else o += *p++;
        }
        if (p >= e) bad = true; else p++;
        return o;
    }
    JV val(int depth = 0) {
        JV v;
        if (depth > 16) { bad = true; return v; }
        ws();
        if (p >= e) { bad = true; return v; }
        if (*p == '{') {
            v.t = JV::Obj; p++; ws();
            if (p < e && *p == '}') { p++; return v; }
            while (!bad) {
                ws();
                std::string k = str();
                ws();
                if (p >= e || *p != ':') { bad = true; break; }
                p++;
                v.o.emplace_back(std::move(k), val(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                bad = true;
            }
        } else if (*p == '[') {
            v.t = JV::Arr; p++; ws();
            if (p < e && *p == ']') { p++; return v; }
            while (!bad) {
                v.a.push_back(val(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                bad = true;
            }
        } else if (*p == '"') { v.t = JV::Str; v.s = str(); }
        else {
            const char* b = p;
            while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;
            v.s.assign(b, p);
            v.t = (v.s == "true" || v.s == "false") ? JV::Bool : v.s == "null" ? JV::Null : JV::Num;
        }
        return v;
    }
};

}  // namespace pn
