#pragma once

#include <SharkSDR/export.h>

#include <cstdint>
#include <string>
#include <vector>

namespace SharkSDR {

enum class LogLevel { Debug, Info, Warning, Error };

struct LogEntry {
    double time = 0.0;  // seconds since start
    LogLevel level = LogLevel::Info;
    std::string source;
    std::string message;
};

// Thread-safe application log. Messages are printed to stderr and kept in a
// bounded in-memory buffer that is shown in the "Log" window.
SHARKSDR_API void log(LogLevel level, const std::string& source, const std::string& message);

// Returns entries with index >= `from` (indices grow monotonically).
// `next` receives the index to pass on the following call.
SHARKSDR_API std::vector<LogEntry> logEntries(uint64_t from, uint64_t* next);
SHARKSDR_API void clearLog();

inline void logInfo(const std::string& src, const std::string& msg) { log(LogLevel::Info, src, msg); }
inline void logWarning(const std::string& src, const std::string& msg) { log(LogLevel::Warning, src, msg); }
inline void logError(const std::string& src, const std::string& msg) { log(LogLevel::Error, src, msg); }

}  // namespace SharkSDR
