#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "testing.h"

namespace bt {

bool g_hardwareTests = false;
static int g_failures = 0;

std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> r;
    return r;
}

void ReportFailure(const char* file, int line, const std::string& msg)
{
    ++g_failures;
    const char* base = strrchr(file, '\\');
    printf("    FAIL %s:%d: %s\n", base ? base + 1 : file, line, msg.c_str());
}

}  // namespace bt

// Usage: bta_tests [--filter substr] [--hardware] [--verbose]
int main(int argc, char** argv)
{
    std::string filter;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "--hardware")) bt::g_hardwareTests = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
    }
    bta::Logger::Instance().EnableStderr(verbose);
    bta::Logger::Instance().SetMinLevel(verbose ? bta::LogLevel::Debug : bta::LogLevel::Warn);

    int passed = 0, failed = 0, skipped = 0;
    for (auto& t : bt::Registry()) {
        std::string full = std::string(t.suite) + "." + t.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        int before = bt::g_failures;
        auto t0 = std::chrono::steady_clock::now();
        bool skip = false;
        std::string why;
        try {
            t.fn();
        } catch (const bt::RequireFailed&) {
        } catch (const bt::Skipped& s) {
            skip = true;
            why = s.why;
        } catch (const std::exception& e) {
            bt::ReportFailure(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (skip) {
            ++skipped;
            printf("[ SKIP ] %s (%s)\n", full.c_str(), why.c_str());
        } else if (bt::g_failures == before) {
            ++passed;
            printf("[  OK  ] %s (%.0f ms)\n", full.c_str(), ms);
        } else {
            ++failed;
            printf("[FAILED] %s (%.0f ms)\n", full.c_str(), ms);
        }
        fflush(stdout);
    }
    printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
