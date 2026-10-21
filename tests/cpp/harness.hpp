// Test registry and context. A group owns one server process; groups run in parallel, the tests of a group one after the other.
#pragma once
#include "jbot.hpp"
#include "server.hpp"
#include <functional>
#include <mutex>
#include <sstream>

namespace pt {

struct Env {
    std::string binary, data_dir, test_world, tmp;
    Server* srv = nullptr;
    int port() const { return srv ? srv->port : 0; }
    std::shared_ptr<Ids> ids(const std::string& version) const;     // "26.3", "26.2" or "26.1"
    int protocol(const std::string& version) const { return version == "26.1" ? 775 : version == "26.2" ? 776 : 777; }
};

struct Ctx {
    std::string name;
    std::ostringstream out;
    bool ok = true;
    bool verbose = false;
    void check(bool c, const std::string& msg) {
        out << (c ? "  ok    " : "  FAIL  ") << msg << "\n";
        if (!c) ok = false;
    }
    void note(const std::string& s) { out << "  " << s << "\n"; }
};

using TestFn = std::function<void(Ctx&, Env&)>;

struct TestDef {
    std::string name;
    TestFn fn;
};

struct Group {
    std::string name;
    bool needs_server = true;
    std::function<ServerOpts(const Env&)> opts;
    std::vector<TestDef> tests;
};

using Registry = std::vector<Group>;

// each test file adds its groups
void reg_java_basic(Registry&);
void reg_java_game(Registry&);
void reg_dimensions(Registry&);
void reg_bedrock(Registry&);
void reg_online(Registry&);
void reg_world(Registry&);

std::string version_ids_path(const Env& e, const std::string& version);

}  // namespace pt
