#include "TestFramework.h"

#include <chrono>
#include <cstring>

namespace t {
std::vector<Case>& registry()
{
    static std::vector<Case> r;
    return r;
}
int failures = 0;
int checks = 0;
static int caseFailures = 0;
void fail(const char* file, int line, const std::string& msg)
{
    ++failures;
    ++caseFailures;
    const char* f = std::strrchr(file, '/');
    std::printf("    FAIL %s:%d  %s\n", f ? f + 1 : file, line, msg.c_str());
}
std::string str(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}
} // namespace t

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedCases = 0;
    for (auto& c : t::registry())
    {
        if (filter && !std::strstr(c.name, filter)) continue;
        t::caseFailures = 0;
        const auto t0 = std::chrono::steady_clock::now();
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        try
        {
            c.fn();
        }
        catch (const std::exception& e)
        {
            t::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[ %s ] %s (%.0f ms)\n", t::caseFailures ? "FAIL" : " OK ", c.name, ms);
        ++run;
        if (t::caseFailures) ++failedCases;
    }
    std::printf("\n%d cases, %d checks, %d failed checks, %d failed cases\n", run, t::checks, t::failures, failedCases);
    return t::failures == 0 ? 0 : 1;
}
