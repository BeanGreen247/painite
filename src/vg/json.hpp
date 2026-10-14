// A small JSON reader for Mojang's data files (numbers keep their text so large integers and floats can both be read exactly).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace pn::vg {

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0;
    int64_t integer = 0;
    bool is_int = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool is_null() const { return type == Null; }
    bool is_obj() const { return type == Object; }
    bool is_arr() const { return type == Array; }
    bool is_str() const { return type == String; }
    bool is_num() const { return type == Number; }
    bool is_bool() const { return type == Bool; }
    const Json* find(const std::string& key) const;
    const Json& at(const std::string& key) const;            // throws when missing
    bool has(const std::string& key) const { return find(key) != nullptr; }
    double number(double def = 0) const { return type == Number ? num : def; }
    float float_value(float def = 0) const { return type == Number ? strtof(str.c_str(), nullptr) : def; }   // Float.parseFloat of the written number
    int64_t int_value(int64_t def = 0) const { return type == Number ? (is_int ? integer : int64_t(num)) : def; }
    std::string string(const std::string& def = "") const { return type == String ? str : def; }
    bool boolean(bool def = false) const { return type == Bool ? b : def; }
    double get_number(const std::string& key, double def) const { const Json* j = find(key); return j ? j->number(def) : def; }
    int64_t get_int(const std::string& key, int64_t def) const { const Json* j = find(key); return j ? j->int_value(def) : def; }
    std::string get_string(const std::string& key, const std::string& def = "") const { const Json* j = find(key); return j ? j->string(def) : def; }
    bool get_bool(const std::string& key, bool def) const { const Json* j = find(key); return j ? j->boolean(def) : def; }

    static Json parse(const std::string& text);              // throws std::runtime_error
    static Json parse_file(const std::string& path);
};

}  // namespace pn::vg
