#include <SharkSDR/log.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

namespace SharkSDR {
namespace {

constexpr size_t kMaxEntries = 2000;

struct LogState {
    std::mutex mutex;
    std::deque<LogEntry> entries;
    uint64_t firstIndex = 0;  // index of entries.front()
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& state() {
    static LogState s;
    return s;
}

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
    }
    return "";
}

}  // namespace

void log(LogLevel level, const std::string& source, const std::string& message) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    LogEntry e;
    e.time = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    e.level = level;
    e.source = source;
    e.message = message;
    std::fprintf(stderr, "[%9.3f] %-7s %s: %s\n", e.time, levelName(level), source.c_str(), message.c_str());
    s.entries.push_back(std::move(e));
    while (s.entries.size() > kMaxEntries) {
        s.entries.pop_front();
        s.firstIndex++;
    }
}

std::vector<LogEntry> logEntries(uint64_t from, uint64_t* next) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<LogEntry> out;
    uint64_t begin = from < s.firstIndex ? s.firstIndex : from;
    for (uint64_t i = begin; i < s.firstIndex + s.entries.size(); i++) out.push_back(s.entries[size_t(i - s.firstIndex)]);
    if (next) *next = s.firstIndex + s.entries.size();
    return out;
}

void clearLog() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.firstIndex += s.entries.size();
    s.entries.clear();
}

}  // namespace SharkSDR
