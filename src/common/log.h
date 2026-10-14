// Structured logging: "<time> <level> [component] message".
// Sinks: stderr, optional file, and an in-memory ring readable over IPC (diagnostics).
#pragma once

#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace bta {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

const char* LogLevelName(LogLevel l);

struct LogEntry {
    uint64_t seq;
    uint64_t unixMs;
    LogLevel level;
    std::string component;
    std::string message;
};

class Logger {
public:
    static Logger& Instance();

    void SetMinLevel(LogLevel l) { min_ = l; }
    LogLevel MinLevel() const { return min_; }
    void EnableStderr(bool on) { stderr_ = on; }
    bool OpenFile(const std::wstring& path);  // appends; returns false on failure
    void CloseFile();

    void Write(LogLevel level, const char* component, std::string message);

    // Entries with seq > afterSeq, at most maxCount (newest kept).
    std::vector<LogEntry> Snapshot(uint64_t afterSeq = 0, size_t maxCount = 500) const;

private:
    Logger() = default;
    mutable std::mutex mu_;
    std::deque<LogEntry> ring_;
    size_t ringCap_ = 2000;
    uint64_t seq_ = 0;
    LogLevel min_ = LogLevel::Info;
    bool stderr_ = true;
    void* file_ = nullptr;  // HANDLE
};

template <typename... Args>
void Log(LogLevel level, const char* component, std::format_string<Args...> fmt, Args&&... args)
{
    Logger& lg = Logger::Instance();
    if (level < lg.MinLevel()) return;
    lg.Write(level, component, std::format(fmt, std::forward<Args>(args)...));
}

#define BTA_LOGT(comp, ...) ::bta::Log(::bta::LogLevel::Trace, comp, __VA_ARGS__)
#define BTA_LOGD(comp, ...) ::bta::Log(::bta::LogLevel::Debug, comp, __VA_ARGS__)
#define BTA_LOGI(comp, ...) ::bta::Log(::bta::LogLevel::Info, comp, __VA_ARGS__)
#define BTA_LOGW(comp, ...) ::bta::Log(::bta::LogLevel::Warn, comp, __VA_ARGS__)
#define BTA_LOGE(comp, ...) ::bta::Log(::bta::LogLevel::Error, comp, __VA_ARGS__)

}  // namespace bta
