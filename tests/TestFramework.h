#pragma once
// Minimal self-registering test framework (no external dependency).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case
{
    const char* name;
    std::function<void()> fn;
};
std::vector<Case>& registry();
struct Registrar
{
    Registrar(const char* n, std::function<void()> f) { registry().push_back({ n, std::move(f) }); }
};
extern int failures;
extern int checks;
void fail(const char* file, int line, const std::string& msg);
std::string str(double v);
} // namespace t

#define AF_CAT2(a, b) a##b
#define AF_CAT(a, b) AF_CAT2(a, b)
#define TEST(name)                                                                  \
    static void AF_CAT(test_, __LINE__)();                                          \
    static t::Registrar AF_CAT(reg_, __LINE__)(name, &AF_CAT(test_, __LINE__));     \
    static void AF_CAT(test_, __LINE__)()

#define CHECK(cond)                                                                 \
    do { ++t::checks; if (!(cond)) t::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_MSG(cond, msg)                                                        \
    do { ++t::checks; if (!(cond)) t::fail(__FILE__, __LINE__, std::string(#cond) + " :: " + (msg)); } while (0)
#define CHECK_NEAR(a, b, tol)                                                       \
    do { ++t::checks; const double va_ = (a), vb_ = (b);                           \
         if (!(std::abs(va_ - vb_) <= (tol)))                                       \
             t::fail(__FILE__, __LINE__, std::string(#a " ~= " #b "  (") + t::str(va_) + " vs " + t::str(vb_) + ", tol " + t::str(tol) + ")"); } while (0)
#define CHECK_LE(a, b)                                                              \
    do { ++t::checks; const double va_ = (a), vb_ = (b);                           \
         if (!(va_ <= vb_)) t::fail(__FILE__, __LINE__, std::string(#a " <= " #b "  (") + t::str(va_) + " vs " + t::str(vb_) + ")"); } while (0)
#define CHECK_GE(a, b)                                                              \
    do { ++t::checks; const double va_ = (a), vb_ = (b);                           \
         if (!(va_ >= vb_)) t::fail(__FILE__, __LINE__, std::string(#a " >= " #b "  (") + t::str(va_) + " vs " + t::str(vb_) + ")"); } while (0)
#define REPORT(...) do { std::printf("      "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
