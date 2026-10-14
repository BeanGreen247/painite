#include "server.hpp"
#include "jclient.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace pt {

int Server::free_port() {
    static std::mutex m;
    static std::atomic<int> next{0};
    std::lock_guard<std::mutex> l(m);
    for (int tries = 0; tries < 200; tries++) {
        int p = 30000 + int((uint32_t(getpid()) * 7919u + uint32_t(next++) * 31u) % 20000u);
        int t = ::socket(AF_INET, SOCK_STREAM, 0), u = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(p));
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        bool ok = ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::bind(u, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        ::close(t);
        ::close(u);
        if (ok) return p;
    }
    throw std::runtime_error("no free port");
}

Server::Server(const std::string& binary, const std::string& tag, const ServerOpts& o) : binary_(binary), opts_(o) {
    static std::atomic<int> n{0};
    const char* base = getenv("PAINITE_TEST_TMP");
    dir = std::string(base ? base : "/tmp") + "/painite-test-" + std::to_string(getpid()) + "-" + tag + "-" + std::to_string(n++);
    fs::remove_all(dir);
    fs::create_directories(dir);
    port = free_port();
    if (!o.world_src.empty()) fs::copy(o.world_src + "/world", dir + "/world", fs::copy_options::recursive);
    {
        std::ofstream f(dir + "/server.properties");
        f << "server-port=" << port << "\nonline-mode=false\nlevel-name=world\nmax-players=200\n" << o.properties;
    }
    if (!o.ops.empty()) {
        std::ofstream f(dir + "/ops.json");
        f << "[";
        for (size_t i = 0; i < o.ops.size(); i++) {
            Bytes u = offline_uuid(o.ops[i]);
            char s[40];
            snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            f << (i ? "," : "") << "{\"uuid\":\"" << s << "\",\"name\":\"" << o.ops[i] << "\",\"level\":4,\"bypassesPlayerLimit\":false}";
        }
        f << "]";
    }
    if (!o.toml.empty()) { std::ofstream f(dir + "/painite.toml"); f << o.toml; }
    spawn();
    if (o.wait_java) wait_ready();
}

void Server::spawn() {
    const ServerOpts& o = opts_;
    pid_ = fork();
    if (pid_ == 0) {
        if (chdir(dir.c_str()) != 0) _exit(126);
        int fd = ::open((dir + "/out.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1);
        dup2(fd, 2);
        int nul = ::open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        std::vector<std::string> a = {binary_, "nogui"};
        if (!o.bedrock) a.push_back("--no-bedrock");
        for (auto& x : o.args) a.push_back(x);
        std::vector<char*> av;
        for (auto& s : a) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        execv(binary_.c_str(), av.data());
        _exit(127);
    }
}

void Server::wait_ready() {
    int64_t end = now_ms() + 20000;
    for (;;) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(port));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        ::close(fd);
        if (up) break;
        if (!alive()) throw std::runtime_error("server exited at start: " + log());
        if (now_ms() > end) throw std::runtime_error("server did not open its port: " + log());
        usleep(10000);
    }
}


bool Server::alive() const {
    if (pid_ <= 0) return false;
    int st;
    return waitpid(pid_, &st, WNOHANG) == 0;
}

void Server::restart() {
    stop();
    spawn();
    wait_ready();
}

void Server::stop() {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    int64_t end = now_ms() + 8000;
    int st;
    while (waitpid(pid_, &st, WNOHANG) == 0 && now_ms() < end) usleep(10000);
    if (waitpid(pid_, &st, WNOHANG) == 0) { kill(pid_, SIGKILL); waitpid(pid_, &st, 0); }
    pid_ = -1;
}

Server::~Server() {
    stop();
    std::error_code ec;
    if (!getenv("PAINITE_TEST_KEEP")) fs::remove_all(dir, ec);
}

std::string Server::log() const {
    std::ifstream f(dir + "/out.log");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > 1500 ? s.substr(s.size() - 1500) : s;
}

}  // namespace pt

namespace pt {

Proc::Proc(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_path) : log_(log_path) {
    pid_ = fork();
    if (pid_ == 0) {
        if (chdir(cwd.c_str()) != 0) _exit(126);
        int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1);
        dup2(fd, 2);
        int nul = ::open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        std::vector<char*> av;
        for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
}

Proc::~Proc() { terminate(5000); }

bool Proc::alive() const {
    if (pid_ <= 0) return false;
    int st;
    return waitpid(pid_, &st, WNOHANG) == 0;
}

void Proc::terminate(int timeout_ms) {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    int64_t end = now_ms() + timeout_ms;
    int st;
    while (waitpid(pid_, &st, WNOHANG) == 0 && now_ms() < end) usleep(20000);
    if (waitpid(pid_, &st, WNOHANG) == 0) { kill(pid_, SIGKILL); waitpid(pid_, &st, 0); }
    pid_ = -1;
}

std::string Proc::log() const {
    std::ifstream f(log_);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace pt
