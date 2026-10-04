#pragma once
// IIR building blocks. All state and coefficients are double precision.
//
//  * Biquad      - transposed direct form II, RBJ / Butterworth designs.
//  * Svf         - Cytomic (Simper) trapezoidal state-variable filter. Its
//                  state stays well defined when coefficients change per sample,
//                  so it is used for every modulated filter (dynamic EQ,
//                  de-esser, parameter smoothing) without zipper noise.
//  * LR4         - Linkwitz-Riley 24 dB/oct crossover (two cascaded Butterworth
//                  2nd-order sections). LP + HP sums to a 2nd-order allpass.

#include "Common.h"

#include <complex>
#include <vector>

namespace ac {

struct BiquadCoeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static BiquadCoeffs lowpass(double f, double q, double sr);
    static BiquadCoeffs highpass(double f, double q, double sr);
    static BiquadCoeffs bandpass(double f, double q, double sr); // 0 dB peak gain
    static BiquadCoeffs peaking(double f, double q, double gainDb, double sr);
    static BiquadCoeffs lowShelf(double f, double q, double gainDb, double sr);
    static BiquadCoeffs highShelf(double f, double q, double gainDb, double sr);
    static BiquadCoeffs allpass(double f, double q, double sr);
    static BiquadCoeffs onePoleHighpass(double f, double sr); // as biquad (b2=a2=0)

    /** Complex frequency response at f Hz. */
    std::complex<double> response(double f, double sr) const;
    double magnitudeDb(double f, double sr) const { return gainToDb(std::abs(response(f, sr)), -300.0); }
};

class Biquad
{
public:
    Biquad() = default;
    explicit Biquad(const BiquadCoeffs& c) : c_(c) {}
    void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
    const BiquadCoeffs& coeffs() const { return c_; }
    void reset() { s1_ = s2_ = 0.0; }

    inline double process(double x)
    {
        const double y = c_.b0 * x + s1_;
        s1_ = c_.b1 * x - c_.a1 * y + s2_;
        s2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }
    void process(float* data, size_t n)
    {
        for (size_t i = 0; i < n; ++i) data[i] = float(process(double(data[i])));
    }

private:
    BiquadCoeffs c_;
    double s1_ = 0, s2_ = 0;
};

/** Q values for an Nth order Butterworth built from 2nd-order sections (N even). */
std::vector<double> butterworthQs(int order);

/** Cascade of biquads (e.g. Butterworth high-pass of order 2..8). */
class BiquadCascade
{
public:
    void setHighpass(double f, int order, double sr);
    void setLowpass(double f, int order, double sr);
    void setSections(std::vector<BiquadCoeffs> c);
    void reset()
    {
        for (auto& s : sections_) s.reset();
    }
    inline double process(double x)
    {
        for (auto& s : sections_) x = s.process(x);
        return x;
    }
    void process(float* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) d[i] = float(process(d[i]));
    }
    std::complex<double> response(double f, double sr) const;
    bool empty() const { return sections_.empty(); }

private:
    std::vector<Biquad> sections_;
};

enum class SvfType { Lowpass, Highpass, Bandpass, Notch, Allpass, Bell, LowShelf, HighShelf };

struct SvfCoeffs
{
    double a1 = 1, a2 = 0, a3 = 0, m0 = 1, m1 = 0, m2 = 0;
    static SvfCoeffs make(SvfType t, double f, double q, double gainDb, double sr);
    /** Same as make() with g0 = tan(pi f / sr) precomputed (cheap per-sample updates). */
    static SvfCoeffs fromG(SvfType t, double g0, double q, double gainDb);
    std::complex<double> response(double f, double sr) const;
};

class Svf
{
public:
    void setCoeffs(const SvfCoeffs& c) { c_ = c; }
    void set(SvfType t, double f, double q, double gainDb, double sr) { c_ = SvfCoeffs::make(t, f, q, gainDb, sr); }
    const SvfCoeffs& coeffs() const { return c_; }
    void reset() { ic1_ = ic2_ = 0.0; }

    inline double process(double v0)
    {
        const double v3 = v0 - ic2_;
        const double v1 = c_.a1 * ic1_ + c_.a2 * v3;
        const double v2 = ic2_ + c_.a2 * ic1_ + c_.a3 * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        return c_.m0 * v0 + c_.m1 * v1 + c_.m2 * v2;
    }

private:
    SvfCoeffs c_;
    double ic1_ = 0, ic2_ = 0;
};

/** Linkwitz-Riley 4th-order crossover for one channel. */
class LR4Crossover
{
public:
    void set(double f, double sr);
    void reset();
    /** Splits x into low and high; low + high == allpass(x). */
    inline void process(double x, double& low, double& high)
    {
        low = lp2_.process(lp1_.process(x));
        high = hp2_.process(hp1_.process(x));
    }
    /** Allpass with the same phase as low+high, for compensating other bands. */
    inline double allpass(double x) { return ap_.process(x); }

private:
    Biquad lp1_, lp2_, hp1_, hp2_, ap_;
};

} // namespace ac
