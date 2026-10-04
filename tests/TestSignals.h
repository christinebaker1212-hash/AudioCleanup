#pragma once
#include <ac/AudioBuffer.h>
#include <ac/Processor.h>

#include <cstdint>

namespace ts {

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uniform() // [0,1)
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return double(s >> 11) / double(1ull << 53);
    }
    double gauss()
    {
        const double u1 = std::max(1e-12, uniform()), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
};

ac::AudioBuffer sine(double sr, int ch, double seconds, double freq, double amp, double phase = 0.0);
ac::AudioBuffer noise(double sr, int ch, double seconds, double rmsDb, uint64_t seed, bool pink = false);
/** Speech-like test signal: jittered glottal pulses through moving formants,
    syllable envelopes, pauses and fricative bursts. Mono. */
ac::AudioBuffer pseudoSpeech(double sr, double seconds, uint64_t seed, double levelDb = -20.0, bool fricatives = true);
/** Music-like: sustained chords (harmonic partials) with stereo spread and percussive hits. */
ac::AudioBuffer pseudoMusic(double sr, double seconds, uint64_t seed, double levelDb = -18.0);
/** Decaying noise impacts (percussive SFX). */
ac::AudioBuffer impacts(double sr, int ch, double seconds, double intervalS, uint64_t seed);

void add(ac::AudioBuffer& a, const ac::AudioBuffer& b, double gain = 1.0);
double rmsDb(const ac::AudioBuffer& b, size_t start = 0, size_t len = SIZE_MAX, int ch = -1);
double maxAbsDiff(const ac::AudioBuffer& a, const ac::AudioBuffer& b, size_t start = 0, size_t len = SIZE_MAX);
/** Amplitude of a sinusoid at freq (Goertzel over the range). */
double toneAmplitude(const float* x, size_t n, double freq, double sr);
/** Lag (samples) maximising cross-correlation of b against a within +/- maxLag. */
int bestLag(const std::vector<float>& a, const std::vector<float>& b, int maxLag);
/** THD+N of a sine (dB re fundamental) by notching the fundamental with a fitted sinusoid. */
double thdN(const float* x, size_t n, double freq, double sr);

/** Render helper. */
ac::AudioBuffer render(ac::Processor& p, const ac::AudioBuffer& in, int block = 512, bool keepTail = false);

} // namespace ts
