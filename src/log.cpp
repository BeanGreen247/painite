#include "log.hpp"
#include "util.hpp"
#include <cstdio>
#include <ctime>
#include <mutex>

namespace pn {

static std::mutex g_mu;
static FILE* g_file = nullptr;

std::string now_hms() {
    time_t t = time(nullptr);
    tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return buf;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return std::string(s.substr(a, b - a));
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) if (c >= 'A' && c <= 'Z') c += 32;
    return r;
}

void log_open(const std::string& path) {
    std::lock_guard<std::mutex> l(g_mu);
    g_file = fopen(path.c_str(), "w");
}

void log(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> l(g_mu);
    char head[64];
    std::string t = now_hms();
    snprintf(head, sizeof head, "[%s] [%s %s]: ", t.c_str(), t.c_str(), level);
    fprintf(stdout, "%s%s\n", head, msg.c_str());
    fflush(stdout);
    bool polled = msg.compare(0, 5, "PERF ") == 0 || msg.compare(0, 4, "TPS ") == 0;   // the dashboard asks every couple of seconds; keep latest.log readable
    if (g_file && !polled) { fprintf(g_file, "%s%s\n", head, msg.c_str()); fflush(g_file); }
}

}  // namespace pn
