// Minimal self-registering test framework (no external dependency).
#pragma once

#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace bt {

struct TestCase {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& Registry();
void ReportFailure(const char* file, int line, const std::string& msg);
struct Registrar {
    Registrar(const char* s, const char* n, std::function<void()> f) { Registry().push_back({s, n, std::move(f)}); }
};
struct RequireFailed {};
// Set by the runner: true when hardware-dependent tests may run.
extern bool g_hardwareTests;

}  // namespace bt

#define BT_CAT2(a, b) a##b
#define BT_CAT(a, b) BT_CAT2(a, b)
#define TEST(suite, name)                                                             \
    static void BT_CAT(test_, BT_CAT(suite, name))();                                 \
    static ::bt::Registrar BT_CAT(reg_, BT_CAT(suite, name))(#suite, #name, BT_CAT(test_, BT_CAT(suite, name))); \
    static void BT_CAT(test_, BT_CAT(suite, name))()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) ::bt::ReportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)
#define REQUIRE(cond)                                                              \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ::bt::ReportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");         \
            throw ::bt::RequireFailed{};                                           \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                       \
    do {                                                                                                     \
        auto _va = (a);                                                                                      \
        auto _vb = (b);                                                                                      \
        if (!(_va == _vb))                                                                                   \
            ::bt::ReportFailure(__FILE__, __LINE__, std::format("CHECK_EQ({}, {}): {} != {}", #a, #b, _va, _vb)); \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                              \
    do {                                                                                                   \
        double _va = (a), _vb = (b);                                                                       \
        if (std::fabs(_va - _vb) > (tol))                                                                  \
            ::bt::ReportFailure(__FILE__, __LINE__, std::format("CHECK_NEAR({}, {}): {} vs {}", #a, #b, _va, _vb)); \
    } while (0)
#define REQUIRE_OK(expr)                                                                                     \
    do {                                                                                                     \
        auto _st = (expr);                                                                                   \
        if (!_st.ok()) {                                                                                     \
            ::bt::ReportFailure(__FILE__, __LINE__, std::string("REQUIRE_OK(" #expr "): ") + _st.ToString()); \
            throw ::bt::RequireFailed{};                                                                     \
        }                                                                                                    \
    } while (0)
#define SKIP(msg) throw ::bt::Skipped{msg}

namespace bt {
struct Skipped {
    std::string why;
};
}  // namespace bt
