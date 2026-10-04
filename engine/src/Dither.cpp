#include "ac/Dither.h"

namespace ac {

namespace {
struct Rng // xorshift32: fast, deterministic, adequate for dither
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    inline double uniform() // [-0.5, 0.5)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return double(s) / 4294967296.0 - 0.5;
    }
};
} // namespace

std::vector<std::vector<int32_t>> quantise(const AudioBuffer& b, int bits, DitherType type, uint32_t seed)
{
    bits = clampv(bits, 8, 32);
    const double scale = std::ldexp(1.0, bits - 1);
    const double maxv = scale - 1.0, minv = -scale;
    // Shaped dither coefficients (Wannamaker 1992, 3-tap F-weighted) only make
    // sense near 44.1/48 kHz; elsewhere fall back to flat TPDF.
    const bool shaped = type == DitherType::TpdfShaped && b.sampleRate < 50000.0;
    const double h1 = 1.623, h2 = -0.982, h3 = 0.109;

    std::vector<std::vector<int32_t>> out(static_cast<size_t>(b.numChannels()));
    for (int c = 0; c < b.numChannels(); ++c)
    {
        Rng rng(seed + uint32_t(c) * 0x9E3779B9u);
        double e1 = 0, e2 = 0, e3 = 0;
        auto& o = out[size_t(c)];
        o.resize(b.numFrames());
        const float* x = b.channel(c);
        for (size_t i = 0; i < b.numFrames(); ++i)
        {
            double v = double(x[i]) * scale;
            if (shaped) v -= h1 * e1 + h2 * e2 + h3 * e3;
            double d = 0.0;
            if (type != DitherType::None) d = rng.uniform() + rng.uniform();
            double q = std::floor(v + d + 0.5);
            q = clampv(q, minv, maxv);
            if (shaped)
            {
                e3 = e2;
                e2 = e1;
                e1 = clampv(q - v, -4.0, 4.0); // bounded so clipping cannot destabilise the loop
            }
            o[i] = int32_t(q);
        }
    }
    return out;
}

AudioBuffer quantiseToFloat(const AudioBuffer& b, int bits, DitherType type, uint32_t seed)
{
    auto q = quantise(b, bits, type, seed);
    AudioBuffer out(b.numChannels(), b.numFrames(), b.sampleRate);
    const double inv = 1.0 / std::ldexp(1.0, clampv(bits, 8, 32) - 1);
    for (int c = 0; c < b.numChannels(); ++c)
        for (size_t i = 0; i < b.numFrames(); ++i) out.channel(c)[i] = float(double(q[size_t(c)][i]) * inv);
    return out;
}

} // namespace ac
