// Core DSP validation: filters, EQ, dynamics, limiter, oversampling,
// resampling, dither, loudness metering, timing and block invariance.

#include "TestFramework.h"
#include "TestSignals.h"

#include <ac/Dither.h>
#include <ac/Dynamics.h>
#include <ac/Eq.h>
#include <ac/Fft.h>
#include <ac/Loudness.h>
#include <ac/Oversampling.h>
#include <ac/Resampler.h>
#include <ac/Restoration.h>

#include <memory>

using namespace ac;

namespace {
constexpr double SR = 48000.0;

/** Magnitude response (dB) of a processor measured from its impulse response. */
std::vector<double> measuredResponse(Processor& p, double sr, const std::vector<double>& freqs, int n = 65536)
{
    AudioBuffer imp(1, size_t(n), sr);
    imp.channel(0)[0] = 1.0f;
    AudioBuffer y = ts::render(p, imp, 512, false);
    std::vector<double> out;
    for (double f : freqs)
    {
        double re = 0, im = 0;
        for (int i = 0; i < n; ++i)
        {
            re += y.channel(0)[i] * std::cos(kTwoPi * f * i / sr);
            im -= y.channel(0)[i] * std::sin(kTwoPi * f * i / sr);
        }
        out.push_back(gainToDb(std::sqrt(re * re + im * im), -300));
    }
    return out;
}

std::vector<double> logFreqs(double lo, double hi, int n)
{
    std::vector<double> f;
    for (int i = 0; i < n; ++i) f.push_back(lo * std::pow(hi / lo, double(i) / (n - 1)));
    return f;
}
} // namespace

// ------------------------------------------------------------------ Bypass
TEST("bypass transparency: neutral processors are bit-exact")
{
    AudioBuffer x = ts::pseudoMusic(SR, 3.0, 7);
    struct Item { const char* name; std::unique_ptr<Processor> p; double tol; };
    std::vector<Item> items;
    items.push_back({ "FilterStage(no-op)", std::make_unique<FilterStage>(FilterSettings{}), 0.0 });
    items.push_back({ "ParametricEq(0 dB)", std::make_unique<ParametricEq>(std::vector<EqBand>{ { SvfType::Bell, 1000, 1, 0.0, true } }), 0.0 });
    CompressorSettings cs; cs.thresholdDb = 20.0; cs.lookaheadMs = 3.0;
    items.push_back({ "Compressor(above max)", std::make_unique<Compressor>(cs), 0.0 });
    LimiterSettings ls; ls.ceilingDbTP = 6.0;
    items.push_back({ "Limiter(ceiling above TP)", std::make_unique<TruePeakLimiter>(ls), 0.0 });
    PlosiveSettings ps;
    DeEssSettings ds; ds.thresholdDb = 60.0;
    items.push_back({ "DeEsser(idle)", std::make_unique<DeEsser>(ds), 0.0 });
    items.push_back({ "Stereo(width 1)", std::make_unique<StereoProcessor>(StereoSettings{}), 1e-7 });
    ExpanderSettings es; es.thresholdDb = -120;
    items.push_back({ "Expander(below floor)", std::make_unique<Expander>(es), 0.0 });
    TransientSettings tsx;
    items.push_back({ "TransientShaper(0 dB)", std::make_unique<TransientShaper>(tsx), 0.0 });
    // Plosive control is a voice processor: check idle transparency on speech.
    {
        AudioBuffer sp = ts::pseudoSpeech(SR, 4.0, 3, -20);
        PlosiveReducer pr(ps);
        AudioBuffer y = ts::render(pr, sp, 512, false);
        REPORT("%-28s max |diff| = %.3g", "PlosiveReducer(speech)", ts::maxAbsDiff(sp, y));
        CHECK(ts::maxAbsDiff(sp, y) == 0.0);
    }
    for (auto& it : items)
    {
        AudioBuffer y = ts::render(*it.p, x, 512, false);
        const double d = ts::maxAbsDiff(x, y);
        REPORT("%-28s max |diff| = %.3g", it.name, d);
        CHECK_MSG(d <= it.tol, it.name);
    }
}

TEST("bypass transparency: STFT engine reconstructs perfectly at unity gain")
{
    AudioBuffer x = ts::pseudoMusic(SR, 3.0, 11);
    DenoiseSettings d;
    d.profile.fftSize = 2048;
    d.profile.psd.assign(1025, 0.0f); // zero noise -> gain 1
    d.reductionDb = 0.0;
    d.profile.valid = true;
    SpectralDenoiser dn(d);
    AudioBuffer y = ts::render(dn, x, 333, false);
    AudioBuffer diff = y;
    ts::add(diff, x, -1.0);
    const double residual = ts::rmsDb(diff, 4096, x.numFrames() - 8192) - ts::rmsDb(x);
    REPORT("STFT round-trip residual %.1f dB re signal", residual);
    CHECK_LE(residual, -120.0);
}

// --------------------------------------------------------------------- EQ
TEST("EQ response: SVF bands match analytic response, bell/shelf gains exact")
{
    std::vector<EqBand> bands = { { SvfType::Bell, 1000, 1.4, 6.0, true },
                                  { SvfType::LowShelf, 120, 0.7, -4.0, true },
                                  { SvfType::HighShelf, 8000, 0.7, 3.0, true },
                                  { SvfType::Bell, 15000, 2.0, -5.0, true } };
    ParametricEq eq(bands);
    const auto f = logFreqs(20, 22000, 60);
    const auto meas = measuredResponse(eq, SR, f);
    double worst = 0;
    for (size_t i = 0; i < f.size(); ++i) worst = std::max(worst, std::abs(meas[i] - ParametricEq::responseDb(bands, f[i], SR)));
    REPORT("max |measured - analytic| = %.4f dB over 20 Hz - 22 kHz", worst);
    CHECK_LE(worst, 0.01);
    // Gains at the centre / plateau.
    std::vector<EqBand> bell = { { SvfType::Bell, 1000, 1.0, 6.0, true } };
    ParametricEq e2(bell);
    CHECK_NEAR(measuredResponse(e2, SR, { 1000.0 })[0], 6.0, 0.01);
    std::vector<EqBand> ls = { { SvfType::LowShelf, 200, 0.707, -6.0, true } };
    ParametricEq e3(ls);
    CHECK_NEAR(measuredResponse(e3, SR, { 20.0 })[0], -6.0, 0.05);
    CHECK_NEAR(measuredResponse(e3, SR, { 200.0 })[0], -3.0, 0.05);
    CHECK_NEAR(measuredResponse(e3, SR, { 5000.0 })[0], 0.0, 0.05);
}

TEST("EQ response: Butterworth high-pass -3 dB at fc and correct slope")
{
    for (int order : { 2, 3, 4, 8 })
    {
        FilterSettings fs;
        fs.hpfHz = 100.0;
        fs.hpfOrder = order;
        FilterStage f(fs);
        const auto m = measuredResponse(f, SR, { 100.0, 50.0, 25.0, 2000.0 }, 1 << 17);
        auto bw = [order](double ratio) { return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * order)); };
        REPORT("order %d: %.2f dB @fc, %.2f dB @fc/2 (theory %.2f), %.2f dB @fc/4 (theory %.2f)", order, m[0], m[1], bw(2), m[2], bw(4));
        CHECK_NEAR(m[0], -3.01, 0.05);
        CHECK_NEAR(m[1], bw(2), 0.1);
        CHECK_NEAR(m[2], bw(4), 0.2);
        CHECK_NEAR(m[3], 0.0, 0.01);
    }
}

TEST("EQ: DC offset is removed exactly")
{
    AudioBuffer x = ts::sine(SR, 2, 1.0, 440, 0.5);
    for (int c = 0; c < 2; ++c)
        for (auto& v : x.vec(c)) v += 0.01f * float(c + 1);
    FilterSettings fs;
    fs.dcOffset = { 0.01, 0.02 };
    FilterStage f(fs);
    AudioBuffer y = ts::render(f, x);
    double m0 = 0, m1 = 0;
    for (size_t i = 0; i < y.numFrames(); ++i) { m0 += y.channel(0)[i]; m1 += y.channel(1)[i]; }
    CHECK_LE(std::abs(m0 / y.numFrames()), 1e-6);
    CHECK_LE(std::abs(m1 / y.numFrames()), 1e-6);
}

TEST("multiband crossover: bands sum flat (allpass) within 0.01 dB")
{
    MultibandSettings ms;
    for (auto& b : ms.band) b.thresholdDb = 50.0; // no gain reduction
    MultibandCompressor mb(ms);
    const auto f = logFreqs(20, 20000, 50);
    const auto m = measuredResponse(mb, SR, f);
    double worst = 0;
    for (double v : m) worst = std::max(worst, std::abs(v));
    REPORT("max deviation %.4f dB (LR4 120 Hz / 2.5 kHz, allpass compensated)", worst);
    CHECK_LE(worst, 0.01);
}

TEST("dynamic EQ: band cut follows level, bounded by range")
{
    DynamicEqBand b;
    b.type = SvfType::Bell; b.freq = 3000; b.q = 1.5; b.thresholdDb = -30; b.ratio = 100; b.rangeDb = 6;
    DynamicEq d({ b });
    AudioBuffer loud = ts::sine(SR, 1, 1.0, 3000, 0.5);
    AudioBuffer y = ts::render(d, loud);
    const double in = ts::toneAmplitude(loud.channel(0) + 24000, 24000, 3000, SR);
    const double out = ts::toneAmplitude(y.channel(0) + 24000, 24000, 3000, SR);
    REPORT("loud 3 kHz tone reduced by %.2f dB (range 6 dB)", gainToDb(in / out));
    CHECK_NEAR(gainToDb(in / out), 6.0, 0.3);
    DynamicEq d2({ b });
    AudioBuffer quiet = ts::sine(SR, 1, 1.0, 3000, 0.001);
    AudioBuffer y2 = ts::render(d2, quiet);
    CHECK_LE(ts::maxAbsDiff(quiet, y2), 1e-6);
}

// --------------------------------------------------------------- Dynamics
TEST("compressor: static curve, soft knee and steady-state gain reduction")
{
    CompressorSettings s;
    s.thresholdDb = -20; s.ratio = 4; s.kneeDb = 0;
    CHECK_NEAR(Compressor::gainComputer(-10, s), -7.5, 1e-9);
    CHECK_NEAR(Compressor::gainComputer(-30, s), 0.0, 1e-9);
    s.kneeDb = 10;
    CHECK_NEAR(Compressor::gainComputer(-20, s), (1.0 / 4 - 1) * 25.0 / 20.0, 1e-9); // knee midpoint
    CHECK_NEAR(Compressor::gainComputer(-26, s), 0.0, 1e-9);
    // Steady state with a peak detector on a square-ish signal (constant |x|).
    s.kneeDb = 0; s.detector = DetectorMode::Peak; s.attackMs = 1; s.releaseMs = 50;
    Compressor c(s);
    AudioBuffer x(1, size_t(SR), SR);
    for (size_t i = 0; i < x.numFrames(); ++i) x.channel(0)[i] = (i / 24) % 2 ? 0.316228f : -0.316228f; // -10 dBFS
    AudioBuffer y = ts::render(c, x);
    const double g = gainToDb(std::abs(y.channel(0)[40000]) / 0.316228);
    REPORT("steady GR %.3f dB (expected -7.5)", g);
    CHECK_NEAR(g, -7.5, 0.05);
}

TEST("compressor: attack and release envelopes follow time constants")
{
    CompressorSettings s;
    s.thresholdDb = -40; s.ratio = 1000; s.kneeDb = 0; s.detector = DetectorMode::Peak;
    s.attackMs = 10; s.releaseMs = 100; s.maxGrDb = 100;
    Compressor c(s);
    // DC-like step: 0.01 (-40 dB) -> 0.1 (-20 dB) at 0.5 s -> back at 1.5 s.
    AudioBuffer x(1, size_t(2.5 * SR), SR);
    for (size_t i = 0; i < x.numFrames(); ++i) x.channel(0)[i] = (i >= size_t(0.5 * SR) && i < size_t(1.5 * SR)) ? 0.1f : 0.01f;
    AudioBuffer y = ts::render(c, x);
    auto grAt = [&](double t) { const size_t i = size_t(t * SR); return -gainToDb(y.channel(0)[i] / x.channel(0)[i]); };
    // After one attack time constant the GR is 63.2 % of the 20 dB step.
    const double a1 = grAt(0.5 + 0.010);
    const double r1 = grAt(1.5 + 0.100);
    REPORT("GR after 1 tau attack: %.2f dB (expect 12.64); GR after 1 tau release: %.2f dB (expect 7.36)", a1, r1);
    CHECK_NEAR(a1, 20.0 * (1 - std::exp(-1.0)), 0.3);
    CHECK_NEAR(r1, 20.0 * std::exp(-1.0), 0.3);
}

TEST("compressor: stereo link applies identical gain; unlinked leaves quiet channel alone")
{
    AudioBuffer x(2, size_t(SR), SR);
    for (size_t i = 0; i < x.numFrames(); ++i)
    {
        x.channel(0)[i] = float(0.5 * std::sin(kTwoPi * 300 * i / SR));
        x.channel(1)[i] = float(0.01 * std::sin(kTwoPi * 300 * i / SR));
    }
    CompressorSettings s;
    s.thresholdDb = -30; s.ratio = 4; s.stereoLink = 1.0;
    Compressor linked(s);
    AudioBuffer y = ts::render(linked, x);
    const double gL = ts::toneAmplitude(y.channel(0) + 24000, 24000, 300, SR) / 0.5;
    const double gR = ts::toneAmplitude(y.channel(1) + 24000, 24000, 300, SR) / 0.01;
    REPORT("linked: L %.2f dB, R %.2f dB", gainToDb(gL), gainToDb(gR));
    CHECK_NEAR(gainToDb(gL), gainToDb(gR), 0.01);
    CHECK_LE(gainToDb(gL), -10.0);
    s.stereoLink = 0.0;
    Compressor un(s);
    AudioBuffer z = ts::render(un, x);
    const double uR = ts::toneAmplitude(z.channel(1) + 24000, 24000, 300, SR) / 0.01;
    REPORT("unlinked: R %.3f dB", gainToDb(uR));
    CHECK_NEAR(gainToDb(uR), 0.0, 0.01);
}

TEST("limiter: true-peak ceiling holds (no overshoot) on hostile material")
{
    struct Sig { const char* name; AudioBuffer b; };
    std::vector<Sig> sigs;
    sigs.push_back({ "pseudo-music +12 dB", ts::pseudoMusic(SR, 6, 3, -6.0) });
    // Near-Nyquist sine with phase offset: inter-sample peaks 3 dB above samples.
    sigs.push_back({ "fs/4 sine 45deg", ts::sine(SR, 2, 2, SR / 4, 1.5, kPi / 4) });
    sigs.push_back({ "19 kHz bursts", ts::sine(SR, 2, 2, 19000, 1.2) });
    AudioBuffer sq(2, size_t(2 * SR), SR);
    for (size_t i = 0; i < sq.numFrames(); ++i) sq.channel(0)[i] = sq.channel(1)[i] = (i / 37) % 2 ? 1.4f : -1.4f;
    sigs.push_back({ "square 649 Hz", sq });
    sigs.push_back({ "impacts", ts::impacts(SR, 2, 4, 0.3, 5) });
    for (auto& s : sigs)
    {
        for (double ceil : { -1.0, -0.1 })
        {
            LimiterSettings ls;
            ls.ceilingDbTP = ceil;
            ls.inputGainDb = 6.0;
            TruePeakLimiter lim(ls);
            AudioBuffer y = ts::render(lim, s.b);
            const double tp = measureLoudness(y, true, false).truePeakDb;
            REPORT("%-20s ceiling %.1f -> TP %.3f dBTP (overshoot %.3f dB)", s.name, ceil, tp, tp - ceil);
            CHECK_LE(tp, ceil + 0.1);
        }
    }
}

TEST("limiter: transparent below ceiling, low distortion when limiting sustained tones")
{
    LimiterSettings ls;
    ls.ceilingDbTP = -1.0;
    TruePeakLimiter lim(ls);
    AudioBuffer x = ts::sine(SR, 1, 2.0, 1000, dbToGain(-12));
    AudioBuffer y = ts::render(lim, x);
    CHECK_LE(ts::maxAbsDiff(x, y), 0.0);
    // 6 dB over the ceiling: once gain settles, THD+N must be very low.
    TruePeakLimiter lim2(ls);
    AudioBuffer x2 = ts::sine(SR, 1, 3.0, 1000, dbToGain(5));
    AudioBuffer y2 = ts::render(lim2, x2);
    const double thd = ts::thdN(y2.channel(0) + size_t(2.0 * SR), size_t(0.5 * SR), 1000, SR);
    const double lvl = gainToDb(ts::toneAmplitude(y2.channel(0) + size_t(2.0 * SR), size_t(0.5 * SR), 1000, SR));
    REPORT("sustained 1 kHz +5 dBFS -> level %.2f dBFS, THD+N %.1f dB", lvl, thd);
    CHECK_LE(thd, -80.0);
    CHECK_LE(lvl, -1.0 + 0.05);
}

TEST("limiter + compressor + saturator: latency compensated (impulse stays put)")
{
    AudioBuffer x(1, size_t(SR), SR);
    x.channel(0)[24000] = 0.5f;
    LimiterSettings ls;
    CompressorSettings cs; cs.lookaheadMs = 5; cs.thresholdDb = 10;
    SaturationSettings ss; ss.driveDb = 0.1;
    DeEssSettings ds;
    TruePeakLimiter a(ls);
    Compressor b(cs);
    Saturator c(ss);
    DeEsser d(ds);
    PlosiveReducer e;
    for (Processor* p : std::initializer_list<Processor*>{ &a, &b, &c, &d, &e })
    {
        AudioBuffer y = ts::render(*p, x);
        size_t pk = 0;
        for (size_t i = 0; i < y.numFrames(); ++i)
            if (std::abs(y.channel(0)[i]) > std::abs(y.channel(0)[pk])) pk = i;
        CHECK_MSG(pk == 24000, "peak at " + std::to_string(pk) + " (latency " + std::to_string(p->latency()) + ")");
    }
}

TEST("block-size independence: identical output for any block partitioning")
{
    AudioBuffer x = ts::pseudoMusic(SR, 2.0, 21, -10);
    std::vector<std::function<std::unique_ptr<Processor>()>> makers = {
        [] { CompressorSettings c; c.thresholdDb = -30; c.lookaheadMs = 2; return std::make_unique<Compressor>(c); },
        [] { return std::make_unique<TruePeakLimiter>(LimiterSettings{}); },
        [] { MultibandSettings m; for (auto& b : m.band) b.thresholdDb = -30; return std::make_unique<MultibandCompressor>(m); },
        [] { return std::make_unique<Saturator>(SaturationSettings{}); },
        [] { DynamicEqBand b; b.thresholdDb = -40; return std::make_unique<DynamicEq>(std::vector<DynamicEqBand>{ b }); },
        [] { DeEssSettings d; d.thresholdDb = -20; return std::make_unique<DeEsser>(d); },
        [] { ExpanderSettings e; e.thresholdDb = -20; return std::make_unique<Expander>(e); },
        [] { TransientSettings t; t.attackDb = 6; return std::make_unique<TransientShaper>(t); },
        [] { DereverbSettings d; return std::make_unique<Dereverberator>(d); },
        [] { ResonanceSettings r; r.thresholdDb = 1.0; return std::make_unique<ResonanceSuppressor>(r); },
        [] { DenoiseSettings d; d.profile.fftSize = 2048; d.profile.psd.assign(1025, 1e-3f); d.profile.valid = true; return std::make_unique<SpectralDenoiser>(d); },
    };
    for (auto& mk : makers)
    {
        auto p1 = mk(), p2 = mk(), p3 = mk();
        AudioBuffer a = ts::render(*p1, x, 4096);
        AudioBuffer b = ts::render(*p2, x, 1);
        AudioBuffer c = ts::render(*p3, x, 777);
        CHECK(ts::maxAbsDiff(a, b) == 0.0);
        CHECK(ts::maxAbsDiff(a, c) == 0.0);
    }
}

// ------------------------------------------------------- Oversampling/SRC
TEST("oversampler: flat passband, integer latency, alias rejection")
{
    Oversampler os;
    os.setup(4, 32);
    AudioBuffer x = ts::sine(SR, 1, 1.0, 15000, 0.5);
    std::vector<float> y(x.numFrames());
    for (size_t i = 0; i < y.size(); ++i) y[i] = float(os.process(x.channel(0)[i], [](double v) { return v; }));
    const double a = ts::toneAmplitude(y.data() + 10000, 24000, 15000, SR);
    REPORT("15 kHz through 4x up/down: %.4f dB, latency %d", gainToDb(a / 0.5), os.latency());
    CHECK_NEAR(gainToDb(a / 0.5), 0.0, 0.02);
    // Alias test: a hard clipper at 4x on 7 kHz; aliased components must be small.
    Oversampler os2;
    os2.setup(4, 32);
    AudioBuffer s = ts::sine(SR, 1, 1.0, 7000, 0.9);
    std::vector<float> z(s.numFrames());
    for (size_t i = 0; i < z.size(); ++i) z[i] = float(os2.process(s.channel(0)[i], [](double v) { return std::tanh(3 * v); }));
    // 3rd harmonic 21 kHz is legal; the 5th (35 kHz) would alias to 13 kHz.
    const double al = ts::toneAmplitude(z.data() + 10000, 24000, 48000 - 35000, SR);
    REPORT("aliased 5th harmonic at 13 kHz: %.1f dB", gainToDb(al));
    CHECK_LE(gainToDb(al), -60.0);
}

TEST("resampling: level, frequency, timing and THD+N")
{
    for (auto pr : { std::pair<double, double>{ 44100, 48000 }, { 48000, 44100 }, { 48000, 96000 }, { 96000, 44100 } })
    {
        AudioBuffer x = ts::sine(pr.first, 1, 2.0, 997, 0.5);
        x.channel(0)[size_t(pr.first)] += 0.4f; // timing marker at t = 1 s
        AudioBuffer y = resample(x, pr.second);
        CHECK(y.numFrames() == size_t(std::llround(2.0 * pr.second)));
        const size_t st = size_t(0.2 * pr.second);
        const double a = ts::toneAmplitude(y.channel(0) + st, size_t(0.5 * pr.second), 997, pr.second);
        const double thd = ts::thdN(y.channel(0) + st, size_t(0.5 * pr.second), 997, pr.second);
        // timing: subtract the sine and find the marker
        size_t pk = 0;
        double best = 0;
        for (size_t i = size_t(0.9 * pr.second); i < size_t(1.1 * pr.second); ++i)
        {
            const double v = std::abs(y.channel(0)[i] - 0.5 * std::sin(kTwoPi * 997 * double(i) / pr.second));
            if (v > best) { best = v; pk = i; }
        }
        REPORT("%.0f -> %.0f: level %.4f dB, THD+N %.1f dB, marker at %zu (expect %zu)", pr.first, pr.second,
               gainToDb(a / 0.5), thd, pk, size_t(pr.second));
        CHECK_NEAR(gainToDb(a / 0.5), 0.0, 0.01);
        CHECK_LE(thd, -120.0);
        CHECK_LE(std::abs(double(pk) - pr.second), 1.0);
    }
}

// ------------------------------------------------------------------ Dither
TEST("dither: TPDF level, shaped spectrum, determinism")
{
    AudioBuffer silence(1, size_t(SR * 2), SR);
    AudioBuffer q = quantiseToFloat(silence, 16, DitherType::Tpdf);
    const double lvl = ts::rmsDb(q);
    // TPDF (2 LSB p-p) + rounding: total error variance 1/12 + 1/6 = 1/4 LSB^2.
    REPORT("16-bit TPDF on silence: %.2f dBFS (theory %.2f)", lvl, gainToDb(0.5 / 32768.0));
    CHECK_NEAR(lvl, gainToDb(0.5 / 32768.0), 0.2);
    // Shaped dither: less noise in the 1-5 kHz region than flat TPDF.
    AudioBuffer tone = ts::sine(44100, 1, 2.0, 1000, 0.25);
    tone.sampleRate = 44100;
    AudioBuffer f = quantiseToFloat(tone, 16, DitherType::Tpdf);
    AudioBuffer s = quantiseToFloat(tone, 16, DitherType::TpdfShaped);
    auto bandNoise = [&](const AudioBuffer& qb) {
        AudioBuffer e = qb;
        ts::add(e, tone, -1.0);
        Biquad bp(BiquadCoeffs::bandpass(3000, 1.0, 44100));
        double sum = 0;
        for (size_t i = 0; i < e.numFrames(); ++i) { const double v = bp.process(e.channel(0)[i]); sum += v * v; }
        return powerToDb(sum / e.numFrames());
    };
    const double nf = bandNoise(f), ns = bandNoise(s);
    REPORT("3 kHz band error: flat %.1f dB, shaped %.1f dB", nf, ns);
    CHECK_LE(ns, nf - 4.0);
    AudioBuffer f2 = quantiseToFloat(tone, 16, DitherType::Tpdf);
    CHECK(ts::maxAbsDiff(f, f2) == 0.0);
}

// ---------------------------------------------------------------- Loudness
TEST("loudness: EBU Tech 3341 reference levels and true peak")
{
    // EBU Tech 3341 case 1/2: a stereo 1 kHz sine at -23 dBFS (peak, both
    // channels) reads -23.0 LUFS; -20 dBFS reads -20.0 LUFS.
    for (double lv : { -23.0, -20.0 })
    {
        AudioBuffer y = ts::sine(SR, 2, 20.0, 1000, dbToGain(lv));
        const auto st = measureLoudness(y);
        REPORT("stereo 1 kHz sine at %.0f dBFS -> %.2f LUFS", lv, st.integrated);
        CHECK_NEAR(st.integrated, lv, 0.1);
    }
    // True peak of an fs/4 sine at 45 degrees: samples at 0.707 A, true peak = A.
    AudioBuffer tp = ts::sine(SR, 1, 1.0, SR / 4, 0.5, kPi / 4);
    const auto t2 = measureLoudness(tp);
    REPORT("fs/4 sine @45deg: sample peak %.2f dBFS, true peak %.2f dBTP (A = %.2f)", t2.samplePeakDb, t2.truePeakDb, gainToDb(0.5));
    CHECK_NEAR(t2.truePeakDb, gainToDb(0.5), 0.15); // 4x interpolating meters may over-read slightly
    CHECK_NEAR(t2.samplePeakDb, gainToDb(0.5) - 3.01, 0.05);
    // Our limiter detector agrees with the meter.
    TruePeakDetector d;
    d.setup(24);
    double m = 0;
    for (size_t i = 0; i < tp.numFrames(); ++i) m = std::max(m, d.process(tp.channel(0)[i]));
    CHECK_NEAR(gainToDb(m), gainToDb(0.5), 0.15);
    CHECK_GE(gainToDb(m), gainToDb(0.5) - 0.02); // never under-reads (limiter safety)
}

TEST("loudness: silence and very short clips are handled")
{
    AudioBuffer s(2, 4800, SR);
    const auto st = measureLoudness(s);
    CHECK(!st.integratedValid());
    AudioBuffer shortClip = ts::sine(SR, 1, 0.1, 1000, 0.5);
    const auto s2 = measureLoudness(shortClip);
    CHECK(s2.truePeakDb > -7.0);
}
