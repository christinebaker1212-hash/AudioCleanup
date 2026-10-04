#include "ac/Filters.h"

namespace ac {

namespace {
struct Rbj
{
    double w0, cw, sw, alpha;
    Rbj(double f, double q, double sr)
    {
        f = clampv(f, 1.0, 0.4999 * sr);
        w0 = kTwoPi * f / sr;
        cw = std::cos(w0);
        sw = std::sin(w0);
        alpha = sw / (2.0 * std::max(1e-6, q));
    }
};

BiquadCoeffs norm(double b0, double b1, double b2, double a0, double a1, double a2)
{
    BiquadCoeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}
} // namespace

BiquadCoeffs BiquadCoeffs::lowpass(double f, double q, double sr)
{
    Rbj r(f, q, sr);
    return norm((1 - r.cw) / 2, 1 - r.cw, (1 - r.cw) / 2, 1 + r.alpha, -2 * r.cw, 1 - r.alpha);
}
BiquadCoeffs BiquadCoeffs::highpass(double f, double q, double sr)
{
    Rbj r(f, q, sr);
    return norm((1 + r.cw) / 2, -(1 + r.cw), (1 + r.cw) / 2, 1 + r.alpha, -2 * r.cw, 1 - r.alpha);
}
BiquadCoeffs BiquadCoeffs::bandpass(double f, double q, double sr)
{
    Rbj r(f, q, sr);
    return norm(r.alpha, 0, -r.alpha, 1 + r.alpha, -2 * r.cw, 1 - r.alpha);
}
BiquadCoeffs BiquadCoeffs::peaking(double f, double q, double gainDb, double sr)
{
    Rbj r(f, q, sr);
    const double A = std::pow(10.0, gainDb / 40.0);
    return norm(1 + r.alpha * A, -2 * r.cw, 1 - r.alpha * A, 1 + r.alpha / A, -2 * r.cw, 1 - r.alpha / A);
}
BiquadCoeffs BiquadCoeffs::lowShelf(double f, double q, double gainDb, double sr)
{
    Rbj r(f, q, sr);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double sa = 2 * std::sqrt(A) * r.alpha;
    return norm(A * ((A + 1) - (A - 1) * r.cw + sa), 2 * A * ((A - 1) - (A + 1) * r.cw), A * ((A + 1) - (A - 1) * r.cw - sa),
                (A + 1) + (A - 1) * r.cw + sa, -2 * ((A - 1) + (A + 1) * r.cw), (A + 1) + (A - 1) * r.cw - sa);
}
BiquadCoeffs BiquadCoeffs::highShelf(double f, double q, double gainDb, double sr)
{
    Rbj r(f, q, sr);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double sa = 2 * std::sqrt(A) * r.alpha;
    return norm(A * ((A + 1) + (A - 1) * r.cw + sa), -2 * A * ((A - 1) + (A + 1) * r.cw), A * ((A + 1) + (A - 1) * r.cw - sa),
                (A + 1) - (A - 1) * r.cw + sa, 2 * ((A - 1) - (A + 1) * r.cw), (A + 1) - (A - 1) * r.cw - sa);
}
BiquadCoeffs BiquadCoeffs::allpass(double f, double q, double sr)
{
    Rbj r(f, q, sr);
    return norm(1 - r.alpha, -2 * r.cw, 1 + r.alpha, 1 + r.alpha, -2 * r.cw, 1 - r.alpha);
}
BiquadCoeffs BiquadCoeffs::onePoleHighpass(double f, double sr)
{
    // Bilinear-transformed 1st-order high-pass.
    const double k = std::tan(kPi * clampv(f, 0.1, 0.49 * sr) / sr);
    BiquadCoeffs c;
    const double a0 = 1 + k;
    c.b0 = 1 / a0;
    c.b1 = -1 / a0;
    c.b2 = 0;
    c.a1 = (k - 1) / a0;
    c.a2 = 0;
    return c;
}

std::complex<double> BiquadCoeffs::response(double f, double sr) const
{
    const std::complex<double> z1 = std::polar(1.0, -kTwoPi * f / sr);
    const std::complex<double> z2 = z1 * z1;
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
}

std::vector<double> butterworthQs(int order)
{
    std::vector<double> qs;
    const int pairs = order / 2;
    for (int k = 0; k < pairs; ++k)
    {
        const double theta = kPi * (2.0 * k + 1.0) / (2.0 * order);
        qs.push_back(1.0 / (2.0 * std::sin(theta)));
    }
    return qs;
}

void BiquadCascade::setHighpass(double f, int order, double sr)
{
    // Butterworth pole pairs; odd orders add the real pole as a 1st-order section.
    sections_.clear();
    if (order % 2 == 1) sections_.emplace_back(BiquadCoeffs::onePoleHighpass(f, sr));
    for (int k = 0; k < order / 2; ++k)
    {
        const double theta = kPi * (2.0 * k + 1.0) / (2.0 * order);
        sections_.emplace_back(BiquadCoeffs::highpass(f, 1.0 / (2.0 * std::sin(theta)), sr));
    }
}

void BiquadCascade::setLowpass(double f, int order, double sr)
{
    sections_.clear();
    for (double q : butterworthQs(std::max(2, order - (order % 2))))
        sections_.emplace_back(BiquadCoeffs::lowpass(f, q, sr));
}

void BiquadCascade::setSections(std::vector<BiquadCoeffs> c)
{
    sections_.clear();
    for (auto& s : c) sections_.emplace_back(s);
}

std::complex<double> BiquadCascade::response(double f, double sr) const
{
    std::complex<double> h = 1.0;
    for (auto& s : sections_) h *= s.coeffs().response(f, sr);
    return h;
}

SvfCoeffs SvfCoeffs::make(SvfType t, double f, double q, double gainDb, double sr)
{
    f = clampv(f, 1.0, 0.4999 * sr);
    return fromG(t, std::tan(kPi * f / sr), q, gainDb);
}

SvfCoeffs SvfCoeffs::fromG(SvfType t, double g, double q, double gainDb)
{
    q = std::max(0.025, q);
    const double A = std::exp(gainDb * (2.302585092994046 / 40.0));
    double k = 1.0 / q;
    SvfCoeffs c;
    switch (t)
    {
        case SvfType::Lowpass: c.m0 = 0; c.m1 = 0; c.m2 = 1; break;
        case SvfType::Highpass: c.m0 = 1; c.m1 = -k; c.m2 = -1; break;
        case SvfType::Bandpass: c.m0 = 0; c.m1 = k; c.m2 = 0; break; // 0 dB at f
        case SvfType::Notch: c.m0 = 1; c.m1 = -k; c.m2 = 0; break;
        case SvfType::Allpass: c.m0 = 1; c.m1 = -2 * k; c.m2 = 0; break;
        case SvfType::Bell:
            k = 1.0 / (q * A);
            c.m0 = 1; c.m1 = k * (A * A - 1); c.m2 = 0;
            break;
        case SvfType::LowShelf:
            g = g / std::sqrt(A);
            c.m0 = 1; c.m1 = k * (A - 1); c.m2 = A * A - 1;
            break;
        case SvfType::HighShelf:
            g = g * std::sqrt(A);
            c.m0 = A * A; c.m1 = k * (1 - A) * A; c.m2 = 1 - A * A;
            break;
    }
    c.a1 = 1.0 / (1.0 + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

std::complex<double> SvfCoeffs::response(double f, double sr) const
{
    // Recover g and k from a1..a3: a2/a1 = g, a1 = 1/(1+g(g+k)).
    const double g = a2 / a1;
    const double k = (1.0 / a1 - 1.0) / g - g;
    // Trapezoidal integrator maps s -> (1/g) * tan-warped; evaluate the analog
    // prototype at the bilinear-warped frequency (exact for TPT structures).
    const double w = std::tan(kPi * f / sr) / g; // normalised analog frequency
    const std::complex<double> s(0.0, w);
    const std::complex<double> den = s * s + k * s + 1.0;
    const std::complex<double> lp = 1.0 / den;
    const std::complex<double> bp = s / den;
    return m0 + m1 * bp + m2 * lp;
}

void LR4Crossover::set(double f, double sr)
{
    const double q = std::sqrt(0.5);
    lp1_.setCoeffs(BiquadCoeffs::lowpass(f, q, sr));
    lp2_.setCoeffs(BiquadCoeffs::lowpass(f, q, sr));
    hp1_.setCoeffs(BiquadCoeffs::highpass(f, q, sr));
    hp2_.setCoeffs(BiquadCoeffs::highpass(f, q, sr));
    // LR4: (1 + s^4) / (s^2 + sqrt2 s + 1)^2 = (s^2 - sqrt2 s + 1) / (s^2 + sqrt2 s + 1),
    // i.e. a 2nd-order allpass with Butterworth Q. Same prewarp => exact digitally.
    ap_.setCoeffs(BiquadCoeffs::allpass(f, q, sr));
}

void LR4Crossover::reset()
{
    lp1_.reset(); lp2_.reset(); hp1_.reset(); hp2_.reset(); ap_.reset();
}

} // namespace ac
