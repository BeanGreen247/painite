// Paper-style log lines: "[HH:MM:SS] [HH:MM:SS LEVEL]: message", mirrored to logs/latest.log.
#pragma once
#include <string>

namespace pn {
void log_open(const std::string& path);
void log(const char* level, const std::string& msg);
inline void info(const std::string& m) { log("INFO", m); }
inline void warn(const std::string& m) { log("WARN", m); }
inline void error(const std::string& m) { log("ERROR", m); }
}  // namespace pn
