#include "json.hpp"
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace pn::vg {

const Json* Json::find(const std::string& key) const {
    for (auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

const Json& Json::at(const std::string& key) const {
    const Json* j = find(key);
    if (!j) throw std::runtime_error("missing json member \"" + key + "\"");
    return *j;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& t) : s(t) {}
    [[noreturn]] void fail(const char* m) { throw std::runtime_error(std::string("json: ") + m + " at " + std::to_string(i)); }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    Json value() {
        ws();
        if (i >= s.size()) fail("unexpected end");
        char c = s[i];
        Json j;
        if (c == '{') {
            i++;
            j.type = Json::Object;
            ws();
            if (s[i] == '}') { i++; return j; }
            for (;;) {
                ws();
                std::string k = string();
                ws();
                if (s[i] != ':') fail("expected :");
                i++;
                j.obj.emplace_back(std::move(k), value());
                ws();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; break; }
                fail("expected , or }");
            }
        } else if (c == '[') {
            i++;
            j.type = Json::Array;
            ws();
            if (s[i] == ']') { i++; return j; }
            for (;;) {
                j.arr.push_back(value());
                ws();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; break; }
                fail("expected , or ]");
            }
        } else if (c == '"') {
            j.type = Json::String;
            j.str = string();
        } else if (s.compare(i, 4, "true") == 0) { i += 4; j.type = Json::Bool; j.b = true; }
        else if (s.compare(i, 5, "false") == 0) { i += 5; j.type = Json::Bool; j.b = false; }
        else if (s.compare(i, 4, "null") == 0) { i += 4; }
        else {
            size_t st = i;
            bool frac = false;
            if (s[i] == '-') i++;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) { if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') frac = true; i++; }
            if (i == st) fail("bad value");
            std::string t = s.substr(st, i - st);
            j.type = Json::Number;
            j.str = t;   // the text, so floats can be read with strtof like Java does
            j.num = strtod(t.c_str(), nullptr);
            if (!frac) { j.is_int = true; j.integer = strtoll(t.c_str(), nullptr, 10); }
        }
        return j;
    }
    std::string string() {
        if (s[i] != '"') fail("expected string");
        i++;
        std::string o;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { o.push_back(c); continue; }
            char e = s[i++];
            switch (e) {
                case 'n': o.push_back('\n'); break;
                case 't': o.push_back('\t'); break;
                case 'r': o.push_back('\r'); break;
                case 'b': o.push_back('\b'); break;
                case 'f': o.push_back('\f'); break;
                case 'u': {
                    unsigned cp = unsigned(strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) o.push_back(char(cp));
                    else if (cp < 0x800) { o.push_back(char(0xC0 | (cp >> 6))); o.push_back(char(0x80 | (cp & 0x3f))); }
                    else { o.push_back(char(0xE0 | (cp >> 12))); o.push_back(char(0x80 | ((cp >> 6) & 0x3f))); o.push_back(char(0x80 | (cp & 0x3f))); }
                    break;
                }
                default: o.push_back(e);
            }
        }
        if (i >= s.size()) fail("unterminated string");
        i++;
        return o;
    }
};

}  // namespace

Json Json::parse(const std::string& text) {
    Parser p(text);
    Json j = p.value();
    return j;
}

Json Json::parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(text);
}

}  // namespace pn::vg
