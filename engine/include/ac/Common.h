#pragma once
// Shared numeric helpers for the AudioFinisher engine.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace ac {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr float kSilenceDb = -150.0f;

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g, double floorDb = kSilenceDb)
{
    return g > 0.0 ? std::max(floorDb, 20.0 * std::log10(g)) : floorDb;
}
inline double powerToDb(double p, double floorDb = kSilenceDb)
{
    return p > 0.0 ? std::max(floorDb, 10.0 * std::log10(p)) : floorDb;
}

template <typename T>
T clampv(T v, T lo, T hi) { return std::min(hi, std::max(lo, v)); }

/** One-pole smoothing coefficient for a time constant in milliseconds. */
inline double timeCoeff(double ms, double sampleRate)
{
    if (ms <= 0.0) return 0.0;
    return std::exp(-1.0 / (0.001 * ms * sampleRate));
}

/** Progress / cancellation plumbing used by every long-running operation. */
struct Job
{
    std::function<void(double fraction, const std::string& what)> progress;
    const std::atomic<bool>* cancel = nullptr;

    bool cancelled() const { return cancel != nullptr && cancel->load(std::memory_order_relaxed); }
    void report(double f, const std::string& what) const
    {
        if (progress) progress(clampv(f, 0.0, 1.0), what);
    }
};

struct CancelledException
{
};

inline void throwIfCancelled(const Job& job)
{
    if (job.cancelled()) throw CancelledException{};
}

/** Percentile of a copy of the data (p in 0..100). */
inline double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    p = clampv(p, 0.0, 100.0);
    const double idx = p / 100.0 * double(v.size() - 1);
    const size_t lo = size_t(std::floor(idx));
    const size_t hi = std::min(v.size() - 1, lo + 1);
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(lo), v.end());
    const double a = v[lo];
    double b = a;
    if (hi != lo)
    {
        b = *std::min_element(v.begin() + std::ptrdiff_t(lo + 1), v.end());
    }
    return a + (b - a) * (idx - double(lo));
}

inline double median(std::vector<double> v) { return percentile(std::move(v), 50.0); }

} // namespace ac
