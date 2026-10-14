#include "common/log.h"

#include <windows.h>

#include <chrono>
#include <cstdio>

namespace bta {

const char* LogLevelName(LogLevel l)
{
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}

Logger& Logger::Instance()
{
    static Logger inst;
    return inst;
}

bool Logger::OpenFile(const std::wstring& path)
{
    std::lock_guard lk(mu_);
    if (file_) CloseHandle(file_);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    file_ = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    return file_ != nullptr;
}

void Logger::CloseFile()
{
    std::lock_guard lk(mu_);
    if (file_) CloseHandle(file_);
    file_ = nullptr;
}

void Logger::Write(LogLevel level, const char* component, std::string message)
{
    using namespace std::chrono;
    const uint64_t nowMs = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} {:<5} [{}] {}\r\n", st.wHour, st.wMinute,
                                   st.wSecond, st.wMilliseconds, LogLevelName(level), component, message);

    std::lock_guard lk(mu_);
    ring_.push_back(LogEntry{++seq_, nowMs, level, component, std::move(message)});
    while (ring_.size() > ringCap_) ring_.pop_front();
    if (stderr_) {
        fwrite(line.data(), 1, line.size(), stderr);
        fflush(stderr);
    }
    if (file_) {
        DWORD written;
        WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}

std::vector<LogEntry> Logger::Snapshot(uint64_t afterSeq, size_t maxCount) const
{
    std::lock_guard lk(mu_);
    std::vector<LogEntry> out;
    for (const auto& e : ring_)
        if (e.seq > afterSeq) out.push_back(e);
    if (out.size() > maxCount) out.erase(out.begin(), out.end() - maxCount);
    return out;
}

}  // namespace bta
