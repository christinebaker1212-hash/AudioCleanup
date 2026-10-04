#pragma once
// Linear-phase polyphase FIR oversampling (Kaiser-windowed sinc).
//
// Up- and down-sampling filters are identical, each N = L*T + 1 taps at the
// high rate, so the round trip delay is exactly T base-rate samples.
// With T = 64 and beta = 10 the transition band spans 0.45..0.55 fs(base)
// with > 95 dB stop-band rejection.

#include "Common.h"

#include <vector>

namespace ac {

std::vector<double> kaiserSincLowpass(int numTaps, double cutoffNormalised /* cycles/sample */, double beta);

class Upsampler
{
public:
    void setup(int factor, int tapsPerPhase, double beta = 10.0, double cutoffFraction = 1.0);
    void reset();
    int factor() const { return L_; }
    /** Group delay at base rate (may be fractional: T/2 for N = L*T + 1). */
    double latencyBase() const { return double(N_ - 1) / 2.0 / double(L_); }
    /** Produces factor() output samples for one input sample. */
    void process(double x, double* out);

private:
    int L_ = 1, T_ = 1, N_ = 1;
    std::vector<std::vector<double>> phases_;
    std::vector<double> hist_; // circular, size = taps per phase (max)
    int histLen_ = 1, histPos_ = 0;
};

class Downsampler
{
public:
    void setup(int factor, int tapsPerPhase, double beta = 10.0, double cutoffFraction = 1.0);
    void reset();
    /** Consumes factor() high-rate samples, returns one base-rate sample. */
    double process(const double* in);

private:
    int L_ = 1, N_ = 1;
    std::vector<double> h_;
    std::vector<double> hist_; // linear double buffer for speed
    int pos_ = 0;
};

/** Oversampled waveshaping helper: up -> f() -> down, per channel.
    Latency is tapsPerPhase base samples. */
class Oversampler
{
public:
    void setup(int factor, int tapsPerPhase);
    void reset();
    int latency() const { return T_; }
    int factor() const { return L_; }
    template <typename F>
    double process(double x, F&& f)
    {
        up_.process(x, buf_.data());
        for (int i = 0; i < L_; ++i) buf_[size_t(i)] = f(buf_[size_t(i)]);
        return down_.process(buf_.data());
    }

private:
    int L_ = 1, T_ = 0;
    Upsampler up_;
    Downsampler down_;
    std::vector<double> buf_;
};

/** Streaming 4x true-peak estimator (BS.1770-style interpolation, longer FIR).
    For each input sample returns max |x| over the sample and the 3 interpolated
    points preceding it, delayed by latency() samples. */
class TruePeakDetector
{
public:
    void setup(int tapsPerPhase = 24);
    void reset();
    int latency() const { return latency_; }
    double process(double x);

private:
    Upsampler up_;
    int latency_ = 0;
    std::vector<double> delay_;
    int dpos_ = 0;
};

} // namespace ac
