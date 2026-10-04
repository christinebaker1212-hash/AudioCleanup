#include "ac/Oversampling.h"

namespace ac {

namespace {
double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double y = x * x / 4.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= y / (double(k) * double(k));
        sum += term;
        if (term < 1e-14 * sum) break;
    }
    return sum;
}
} // namespace

std::vector<double> kaiserSincLowpass(int n, double fc, double beta)
{
    std::vector<double> h(static_cast<size_t>(n));
    const double m = double(n - 1) / 2.0;
    const double i0b = besselI0(beta);
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = double(i) - m;
        const double sinc = t == 0.0 ? 2.0 * fc : std::sin(kTwoPi * fc * t) / (kPi * t);
        const double r = m > 0 ? t / m : 0.0;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h[size_t(i)] = sinc * w;
        sum += h[size_t(i)];
    }
    for (auto& v : h) v /= sum; // unity DC gain
    return h;
}

void Upsampler::setup(int factor, int tapsPerPhase, double beta, double cutoffFraction)
{
    L_ = std::max(1, factor);
    T_ = std::max(1, tapsPerPhase);
    N_ = L_ * T_ + 1;
    const auto h = kaiserSincLowpass(N_, 0.5 / L_ * cutoffFraction, beta);
    phases_.assign(size_t(L_), {});
    for (int p = 0; p < L_; ++p)
        for (int j = p; j < N_; j += L_) phases_[size_t(p)].push_back(h[size_t(j)] * L_);
    histLen_ = T_ + 1;
    reset();
}

void Upsampler::reset()
{
    hist_.assign(size_t(histLen_) * 2, 0.0);
    histPos_ = 0;
}

void Upsampler::process(double x, double* out)
{
    // hist_ is a doubled circular buffer so a contiguous window is always available.
    histPos_ = (histPos_ == 0 ? histLen_ : histPos_) - 1;
    hist_[size_t(histPos_)] = x;
    hist_[size_t(histPos_ + histLen_)] = x;
    const double* xs = &hist_[size_t(histPos_)]; // xs[i] = x[n - i]
    for (int p = 0; p < L_; ++p)
    {
        const auto& ph = phases_[size_t(p)];
        double acc = 0.0;
        for (size_t i = 0; i < ph.size(); ++i) acc += ph[i] * xs[i];
        out[p] = acc;
    }
}

void Downsampler::setup(int factor, int tapsPerPhase, double beta, double cutoffFraction)
{
    L_ = std::max(1, factor);
    N_ = L_ * std::max(1, tapsPerPhase) + 1;
    h_ = kaiserSincLowpass(N_, 0.5 / L_ * cutoffFraction, beta);
    reset();
}

void Downsampler::reset()
{
    hist_.assign(size_t(N_) * 2, 0.0);
    pos_ = 0;
}

double Downsampler::process(const double* in)
{
    for (int i = 0; i < L_; ++i)
    {
        pos_ = (pos_ == 0 ? N_ : pos_) - 1;
        hist_[size_t(pos_)] = in[i];
        hist_[size_t(pos_ + N_)] = in[i];
    }
    const double* xs = &hist_[size_t(pos_)]; // xs[j] = y[m - j], m = newest
    double acc = 0.0;
    for (int j = 0; j < N_; ++j) acc += h_[size_t(j)] * xs[j];
    return acc;
}

void Oversampler::setup(int factor, int tapsPerPhase)
{
    L_ = std::max(1, factor);
    T_ = tapsPerPhase;
    up_.setup(L_, tapsPerPhase);
    down_.setup(L_, tapsPerPhase);
    buf_.assign(size_t(L_), 0.0);
}

void Oversampler::reset()
{
    up_.reset();
    down_.reset();
}

void TruePeakDetector::setup(int tapsPerPhase)
{
    // Slightly wider passband (0.98 of Nyquist) so near-Nyquist content is not under-read.
    up_.setup(4, tapsPerPhase, 8.0, 0.98);
    // Upsampled sample k of input n sits at base time n - T/2 + k/4 ... the
    // group delay is T/2 base samples (N = 4T+1). Align the raw sample too.
    latency_ = (tapsPerPhase + 1) / 2;
    delay_.assign(size_t(latency_ + 1), 0.0);
    dpos_ = 0;
    reset();
}

void TruePeakDetector::reset()
{
    up_.reset();
    std::fill(delay_.begin(), delay_.end(), 0.0);
    dpos_ = 0;
}

double TruePeakDetector::process(double x)
{
    double o[4];
    up_.process(x, o);
    delay_[size_t(dpos_)] = x;
    const int rd = (dpos_ + 1) % int(delay_.size());
    const double raw = delay_[size_t(rd)];
    dpos_ = rd;
    double m = std::abs(raw);
    for (double v : o) m = std::max(m, std::abs(v));
    return m;
}

} // namespace ac
