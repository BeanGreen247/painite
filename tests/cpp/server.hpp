// Starts a Painite server process in its own temporary directory for a test group.
#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

namespace pt {

struct ServerOpts {
    std::string properties;               // extra server.properties lines ("gamemode=creative\n")
    std::vector<std::string> ops;         // operator names (written to ops.json at level 4)
    std::string world_src;                // directory holding "world/" to copy in; empty = the server generates a new world
    std::string toml;                     // painite.toml contents
    std::vector<std::string> args;        // extra command line arguments
    bool bedrock = true;
    bool wait_java = true;                // wait for the Java TCP port before returning
};

class Server {
public:
    Server(const std::string& binary, const std::string& tag, const ServerOpts& o);
    ~Server();
    Server(const Server&) = delete;
    int port = 0;
    std::string dir;
    std::string log() const;
    bool alive() const;
    void stop();                          // clean stop through SIGTERM (the server saves the world)
    void restart();                       // start again in the same directory and port
    static int free_port();               // a port free for both TCP and UDP
private:
    void spawn();
    void wait_ready();
    std::string binary_;
    ServerOpts opts_;
    pid_t pid_ = -1;
};

// Any other process (Mojang's server, for instance) with its output in a log file.
class Proc {
public:
    Proc(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_path);
    ~Proc();
    bool alive() const;
    void terminate(int timeout_ms = 20000);
    std::string log() const;
private:
    pid_t pid_ = -1;
    std::string log_;
};

}  // namespace pt
