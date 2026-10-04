#include "TestSignals.h"

#include <ac/Filters.h>

#include <cmath>

using namespace ac;

namespace ts {

AudioBuffer sine(double sr, int ch, double seconds, double freq, double amp, double phase)
{
    AudioBuffer b(ch, size_t(seconds * sr), sr);
    for (int c = 0; c < ch; ++c)
        for (size_t i = 0; i < b.numFrames(); ++i) b.channel(c)[i] = float(amp * std::sin(kTwoPi * freq * double(i) / sr + phase));
    return b;
}

AudioBuffer noise(double sr, int ch, double seconds, double rmsDbv, uint64_t seed, bool pink)
{
    AudioBuffer b(ch, size_t(seconds * sr), sr);
    const double g = dbToGain(rmsDbv);
    for (int c = 0; c < ch; ++c)
    {
        Rng r(seed + uint64_t(c) * 7919);
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (size_t i = 0; i < b.numFrames(); ++i)
        {
            double w = r.gauss();
            if (pink)
            {
                // Paul Kellet's refined pink filter (unity-ish gain normalised below).
                b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
                w = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11;
                b6 = w * 0.115926 / 0.11;
            }
            b.channel(c)[i] = float(w);
        }
    }
    // normalise to requested RMS
    const double cur = rmsDb(b);
    b.applyGain(g / dbToGain(cur));
    return b;
}

AudioBuffer pseudoSpeech(double sr, double seconds, uint64_t seed, double levelDb, bool fricatives)
{
    Rng r(seed);
    AudioBuffer b(1, size_t(seconds * sr), sr);
    float* x = b.channel(0);
    const double f0base = 110.0 + 80.0 * r.uniform();
    double phase = 0;
    size_t i = 0;
    Biquad f1, f2, f3, fric;
    while (i < b.numFrames())
    {
        // syllable: voiced 120-300 ms, optional fricative, then gap 40-300 ms
        const size_t vlen = size_t((0.12 + 0.18 * r.uniform()) * sr);
        const double F1 = 300 + 500 * r.uniform(), F2 = 900 + 1400 * r.uniform(), F3 = 2300 + 800 * r.uniform();
        f1.setCoeffs(BiquadCoeffs::bandpass(F1, 5, sr));
        f2.setCoeffs(BiquadCoeffs::bandpass(F2, 8, sr));
        f3.setCoeffs(BiquadCoeffs::bandpass(F3, 10, sr));
        const double f0 = f0base * (0.85 + 0.3 * r.uniform());
        for (size_t k = 0; k < vlen && i < b.numFrames(); ++k, ++i)
        {
            const double env = std::sin(kPi * double(k) / double(vlen));
            const double f0k = f0 * (1.0 + 0.05 * std::sin(kTwoPi * 5.0 * double(k) / sr));
            phase += f0k / sr;
            double src = 0;
            if (phase >= 1.0) { phase -= 1.0; src = 1.0; }
            src += 0.02 * r.gauss();
            const double y = 3.0 * f1.process(src) + 2.0 * f2.process(src) + 1.2 * f3.process(src);
            x[i] = float(env * y);
        }
        if (r.uniform() < 0.4 && fricatives)
        {
            fric.setCoeffs(BiquadCoeffs::highpass(4500 + 2000 * r.uniform(), 0.7, sr));
            const size_t flen = size_t((0.05 + 0.08 * r.uniform()) * sr);
            for (size_t k = 0; k < flen && i < b.numFrames(); ++k, ++i)
                x[i] = float(0.15 * std::sin(kPi * double(k) / double(flen)) * fric.process(r.gauss()));
        }
        const size_t gap = size_t((0.04 + (r.uniform() < 0.2 ? 0.5 : 0.15) * r.uniform()) * sr);
        i += gap;
    }
    const double cur = rmsDb(b);
    b.applyGain(dbToGain(levelDb - cur));
    return b;
}

AudioBuffer pseudoMusic(double sr, double seconds, uint64_t seed, double levelDb)
{
    Rng r(seed);
    AudioBuffer b(2, size_t(seconds * sr), sr);
    const double roots[] = { 110.0, 146.83, 130.81, 98.0 };
    const size_t bar = size_t(2.0 * sr);
    for (size_t s = 0, k = 0; s < b.numFrames(); s += bar, ++k)
    {
        const double root = roots[k % 4];
        const double notes[] = { root, root * 1.26, root * 1.5, root * 2.0, root * 2.52 };
        for (int ni = 0; ni < 5; ++ni)
        {
            const double pan = 0.2 + 0.6 * r.uniform();
            for (int h = 1; h <= 8; ++h)
            {
                const double f = notes[ni] * h;
                if (f > 0.45 * sr) break;
                const double a = 0.08 / h;
                const double ph = r.uniform() * kTwoPi;
                for (size_t i = s; i < std::min(b.numFrames(), s + bar); ++i)
                {
                    const double t = double(i - s) / sr;
                    const double env = std::exp(-t * 0.6) * std::min(1.0, t * 50.0);
                    const double v = a * env * std::sin(kTwoPi * f * double(i) / sr + ph);
                    b.channel(0)[i] += float(v * (1.0 - pan));
                    b.channel(1)[i] += float(v * pan);
                }
            }
        }
        // kick-ish hits every half bar
        for (int hit = 0; hit < 4; ++hit)
        {
            const size_t st = s + size_t(hit) * bar / 4;
            for (size_t i = st; i < std::min(b.numFrames(), st + size_t(0.25 * sr)); ++i)
            {
                const double t = double(i - st) / sr;
                const double v = 0.6 * std::exp(-t * 18.0) * std::sin(kTwoPi * (55.0 + 120.0 * std::exp(-t * 30.0)) * t);
                b.channel(0)[i] += float(v);
                b.channel(1)[i] += float(v);
            }
        }
    }
    const double cur = rmsDb(b);
    b.applyGain(dbToGain(levelDb - cur));
    return b;
}

AudioBuffer impacts(double sr, int ch, double seconds, double intervalS, uint64_t seed)
{
    Rng r(seed);
    AudioBuffer b(ch, size_t(seconds * sr), sr);
    const size_t step = size_t(intervalS * sr);
    for (size_t s = size_t(0.05 * sr); s < b.numFrames(); s += step)
    {
        const double amp = 0.3 + 0.5 * r.uniform();
        for (int c = 0; c < ch; ++c)
        {
            Biquad lp(BiquadCoeffs::lowpass(2000 + 3000 * r.uniform(), 0.7, sr));
            for (size_t i = s; i < std::min(b.numFrames(), s + step); ++i)
            {
                const double t = double(i - s) / sr;
                b.channel(c)[i] += float(amp * std::exp(-t * 25.0) * lp.process(r.gauss()));
            }
        }
    }
    return b;
}

void add(AudioBuffer& a, const AudioBuffer& b, double gain)
{
    for (int c = 0; c < a.numChannels(); ++c)
    {
        const int bc = std::min(c, b.numChannels() - 1);
        for (size_t i = 0; i < std::min(a.numFrames(), b.numFrames()); ++i) a.channel(c)[i] += float(b.channel(bc)[i] * gain);
    }
}

double rmsDb(const AudioBuffer& b, size_t start, size_t len, int ch)
{
    double s = 0;
    size_t n = 0;
    const size_t end = len == SIZE_MAX ? b.numFrames() : std::min(b.numFrames(), start + len);
    for (int c = 0; c < b.numChannels(); ++c)
    {
        if (ch >= 0 && c != ch) continue;
        for (size_t i = start; i < end; ++i) { s += double(b.channel(c)[i]) * b.channel(c)[i]; ++n; }
    }
    return n ? powerToDb(s / double(n), -300.0) : -300.0;
}

double maxAbsDiff(const AudioBuffer& a, const AudioBuffer& b, size_t start, size_t len)
{
    double m = 0;
    const size_t end = std::min({ a.numFrames(), b.numFrames(), len == SIZE_MAX ? SIZE_MAX : start + len });
    for (int c = 0; c < std::min(a.numChannels(), b.numChannels()); ++c)
        for (size_t i = start; i < end; ++i) m = std::max(m, double(std::abs(a.channel(c)[i] - b.channel(c)[i])));
    return m;
}

double toneAmplitude(const float* x, size_t n, double freq, double sr)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * double(i) / double(n));
        re += w * x[i] * std::cos(kTwoPi * freq * double(i) / sr);
        im -= w * x[i] * std::sin(kTwoPi * freq * double(i) / sr);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double(n) * 0.5);
}

int bestLag(const std::vector<float>& a, const std::vector<float>& b, int maxLag)
{
    double best = -1e300;
    int lag = 0;
    const int n = int(std::min(a.size(), b.size()));
    for (int L = -maxLag; L <= maxLag; ++L)
    {
        double s = 0;
        for (int i = std::max(0, -L); i < std::min(n, n - L); ++i) s += double(a[size_t(i)]) * b[size_t(i + L)];
        if (s > best) { best = s; lag = L; }
    }
    return lag;
}

double thdN(const float* x, size_t n, double freq, double sr)
{
    // Least-squares fit of a*sin + b*cos + dc, residual = THD+N.
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double s = std::sin(kTwoPi * freq * double(i) / sr), c = std::cos(kTwoPi * freq * double(i) / sr);
        ss += s * s; sc += s * c; cc += c * c; xs += x[i] * s; xc += x[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, bb = (xc * ss - xs * sc) / det;
    double res = 0, sig = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double f = a * std::sin(kTwoPi * freq * double(i) / sr) + bb * std::cos(kTwoPi * freq * double(i) / sr);
        res += (x[i] - f) * (x[i] - f);
        sig += f * f;
    }
    return powerToDb(res / std::max(sig, 1e-300), -300.0);
}

AudioBuffer render(Processor& p, const AudioBuffer& in, int block, bool keepTail)
{
    RenderOptions ro;
    ro.blockSize = block;
    ro.keepTail = keepTail;
    return renderProcessor(p, in, ro, {});
}

} // namespace ts
