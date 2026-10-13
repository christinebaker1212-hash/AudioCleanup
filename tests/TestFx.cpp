// Effect matching: estimates of reverb, echo and band limits on references
// made with known settings (by generators independent of the engine's own
// effect processors), no false detections on dry material, and the engine's
// processors read back by the same estimators.

#include "TestFramework.h"
#include "TestSignals.h"

#include <ac/Fft.h>
#include <ac/Filters.h>
#include <ac/FxMatch.h>
#include <ac/Dynamics.h>
#include <ac/Pipeline.h>

#include <complex>

using namespace ac;

namespace {
constexpr double SR = 48000.0;

/** Dry knocks: sharp filtered-noise hits (amplitude decay e^-70t, ~0.1 s). */
AudioBuffer knocks(int ch, double seconds, double interval, uint64_t seed)
{
    ts::Rng r(seed);
    AudioBuffer b(ch, size_t(seconds * SR), SR);
    for (size_t s = size_t(0.05 * SR); s < b.numFrames(); s += size_t(interval * SR))
    {
        const double amp = 0.3 + 0.4 * r.uniform();
        Biquad lp(BiquadCoeffs::lowpass(1500 + 4000 * r.uniform(), 0.7, SR));
        for (size_t i = s; i < std::min(b.numFrames(), s + size_t(0.3 * SR)); ++i)
        {
            const double v = amp * std::exp(-70.0 * double(i - s) / SR) * lp.process(r.gauss());
            for (int c = 0; c < ch; ++c) b.channel(c)[i] += float(v);
        }
    }
    return b;
}

/** Full-length FFT convolution (test-local). */
std::vector<float> convolve(const float* x, size_t n, const std::vector<float>& h)
{
    size_t M = 32;
    while (M < n + h.size()) M <<= 1;
    RealFft fft{ int(M) };
    std::vector<float> a(M, 0.0f), b(M, 0.0f);
    std::copy(x, x + n, a.begin());
    std::copy(h.begin(), h.end(), b.begin());
    std::vector<std::complex<float>> A(size_t(fft.numBins())), B(size_t(fft.numBins()));
    fft.forward(a.data(), A.data());
    fft.forward(b.data(), B.data());
    for (size_t k = 0; k < A.size(); ++k) A[k] *= B[k];
    fft.inverse(A.data(), a.data());
    a.resize(n + h.size() - 1);
    return a;
}

/** A room unlike the engine's reverb: two bands (split at 1 kHz, 4th-order
    Butterworth) of time-domain decaying noise, energy-normalised, direct sound
    plus the tail at drrDb below it, decorrelated per channel. */
AudioBuffer room(const AudioBuffer& x, double rtLow, double rtHigh, double drrDb, uint64_t seed)
{
    const size_t L = size_t(1.3 * std::max(rtLow, rtHigh) * SR);
    AudioBuffer out(x.numChannels(), x.numFrames() + L, SR);
    for (int c = 0; c < x.numChannels(); ++c)
    {
        ts::Rng r(seed + uint64_t(c) * 101);
        BiquadCascade lo, hi;
        lo.setLowpass(1000, 4, SR);
        hi.setHighpass(1000, 4, SR);
        std::vector<float> h(L);
        double e = 0;
        for (size_t i = 0; i < L; ++i)
        {
            const double t = double(i) / SR;
            const double v = lo.process(r.gauss()) * std::exp(-6.9078 * t / rtLow) + hi.process(r.gauss()) * std::exp(-6.9078 * t / rtHigh);
            h[i] = float(v * std::min(1.0, t / 0.005));
            e += double(h[i]) * h[i];
        }
        const float g = float(dbToGain(-drrDb) / std::sqrt(e));
        for (auto& v : h) v *= g;
        h.insert(h.begin(), size_t(0.012 * SR), 0.0f); // pre-delay
        h[0] = 1.0f;                                  // direct sound
        const auto y = convolve(x.channel(c), x.numFrames(), h);
        std::copy(y.begin(), y.begin() + long(out.numFrames()), out.channel(c));
    }
    return out;
}

/** Test-local feedback echo: y = x + level * sum g^(k-1) x(t - k D). */
AudioBuffer echo(const AudioBuffer& x, double ms, double levelDb, double fb)
{
    const size_t D = size_t(ms * 0.001 * SR);
    AudioBuffer out(x.numChannels(), x.numFrames() + 12 * D, SR);
    for (int c = 0; c < x.numChannels(); ++c)
    {
        for (size_t i = 0; i < x.numFrames(); ++i) out.channel(c)[i] += x.channel(c)[i];
        double g = dbToGain(levelDb);
        for (int k = 1; k <= 12; ++k, g *= fb)
            for (size_t i = 0; i < x.numFrames(); ++i) out.channel(c)[i + size_t(k) * D] += float(g * x.channel(c)[i]);
    }
    return out;
}

AudioBuffer bandLimit(const AudioBuffer& x, double hp, double lp, int order)
{
    AudioBuffer y = x;
    for (int c = 0; c < y.numChannels(); ++c)
    {
        BiquadCascade h, l;
        h.setHighpass(hp, order, SR);
        l.setLowpass(lp, order, SR);
        for (auto& v : y.vec(c)) v = float(l.process(h.process(v)));
    }
    return y;
}

double relErr(double a, double b) { return std::abs(a - b) / b; }
} // namespace

TEST("fx match: dry material shows no reverb, echo or band limit")
{
    struct Case { const char* name; AudioBuffer b; };
    std::vector<Case> cases;
    cases.push_back({ "knocks", knocks(2, 6.0, 0.9, 3) });
    cases.push_back({ "speech", ts::pseudoSpeech(SR, 8.0, 4, -20) });
    cases.push_back({ "music", ts::pseudoMusic(SR, 8.0, 5, -18) });
    for (auto& c : cases)
    {
        const auto p = analyzeEffects(c.b, c.name);
        REPORT("%s:\n%s", c.name, describeEffects(p).c_str());
        CHECK(!p.reverb.found);
        CHECK(!p.echo.found);
        CHECK(p.bandLimit.hpfHz == 0.0 && p.bandLimit.lpfHz == 0.0);
    }
}

TEST("fx match: reverb, echo and band limits estimated from known references")
{
    const AudioBuffer dry = knocks(2, 8.0, 1.6, 7);
    // Reverb: 1.6 s below 1 kHz, 0.9 s above, tail as loud as the direct sound.
    {
        const auto p = analyzeEffects(room(dry, 1.6, 0.9, 0.0, 11), "room");
        REPORT("room 1.6/0.9 s:\n%s", describeEffects(p).c_str());
        CHECK(p.reverb.found);
        CHECK(!p.echo.found);
        double lo = 0, hi = 0;
        for (size_t i = 0; i < p.reverb.bandHz.size(); ++i)
        {
            if (p.reverb.bandHz[i] == 250) lo = p.reverb.bandRt60[i];
            if (p.reverb.bandHz[i] == 4000) hi = p.reverb.bandRt60[i];
        }
        CHECK_LE(relErr(lo, 1.6), 0.25);
        CHECK_LE(relErr(hi, 0.9), 0.25);
    }
    // Echo: 320 ms, first repeat -4 dB, feedback 0.4.
    {
        const auto p = analyzeEffects(echo(dry, 320.0, -4.0, 0.4), "echo");
        REPORT("echo 320 ms -4 dB fb 0.4:\n%s", describeEffects(p).c_str());
        CHECK(p.echo.found);
        CHECK_NEAR(p.echo.delayMs, 320.0, 1.0);
        CHECK_NEAR(p.echo.levelDb, -4.0, 2.0);
        CHECK_NEAR(p.echo.feedback, 0.4, 0.12);
    }
    // Telephone band: 300 Hz - 3.4 kHz, 6th order.
    {
        const auto sp = ts::pseudoSpeech(SR, 8.0, 9, -20);
        const auto p = analyzeEffects(bandLimit(sp, 300, 3400, 6), "phone");
        REPORT("telephone 300-3400 Hz:\n%s", describeEffects(p).c_str());
        CHECK_NEAR(std::log2(p.bandLimit.hpfHz / 300.0), 0.0, 0.3);
        CHECK_NEAR(std::log2(p.bandLimit.lpfHz / 3400.0), 0.0, 0.3);
    }
}

TEST("fx match: the engine's reverb and echo read back as set")
{
    const AudioBuffer dry = knocks(2, 8.0, 1.6, 21);
    {
        ReverbSettings rs;
        rs.bandHz = { 250, 1000, 4000 };
        rs.bandRt60 = { 1.4, 1.0, 0.6 };
        rs.wetDb = -3.0;
        ConvolutionReverb rv(rs);
        AudioBuffer y = dry;
        rv.prepare(SR, 2);
        rv.processOffline(y, {});
        const auto e = measureReverb(y);
        std::string bands;
        for (size_t i = 0; i < e.bandHz.size(); ++i) bands += " " + std::to_string(int(e.bandHz[i])) + ":" + std::to_string(e.bandRt60[i]).substr(0, 4);
        REPORT("engine reverb 1.4/1.0/0.6 s -> RT60 %.2f s, bands%s, tail %.1f dB", e.rt60, bands.c_str(), e.tailLevelDb);
        CHECK(e.found);
        CHECK_LE(relErr(e.rt60, 1.0), 0.2);
    }
    {
        EchoSettings es;
        es.delayMs = 250;
        es.feedback = 0.3;
        es.levelDb = -6;
        EchoProcessor ep(es);
        AudioBuffer y = dry;
        ep.prepare(SR, 2);
        ep.processOffline(y, {});
        const auto e = measureEcho(y);
        REPORT("engine echo 250 ms -6 dB fb 0.3 -> %.1f ms, %.1f dB, fb %.2f", e.delayMs, e.levelDb, e.feedback);
        CHECK(e.found);
        CHECK_NEAR(e.delayMs, 250.0, 1.0);
        CHECK_NEAR(e.levelDb, -6.0, 2.0);
        CHECK_NEAR(e.feedback, 0.3, 0.12);
    }
}

TEST("fx match: a dry clip takes on the reference's reverb, echo, band limit and loudness")
{
    // Reference: knocks in a room (1.2 s / 0.7 s) with a 280 ms echo, stereo.
    const AudioBuffer refDry = knocks(2, 8.0, 1.7, 31);
    const AudioBuffer ref = room(echo(refDry, 280.0, -5.0, 0.35), 1.2, 0.7, 2.0, 32);
    auto F = std::make_shared<FxProfile>(analyzeEffects(ref, "room+echo"));
    // Target: different dry hits (other seed, other spacing), mono.
    AudioBuffer target = knocks(1, 6.0, 1.3, 33);
    target.applyGain(0.3);
    ProcessRequest rq;
    rq.input = &target;
    rq.preset = findPreset("sfx.transparent");
    rq.category = Category::SoundEffect;
    rq.controls.fxReference = F;
    ProcessResult r = process(rq, {});
    const auto out = analyzeEffects(r.output, "output");
    REPORT("reference:\n%s", describeEffects(*F).c_str());
    REPORT("output:\n%s", describeEffects(out).c_str());
    REPORT("%s", describePlan(r.plan).c_str());
    CHECK(r.plan.stage(StageId::Reverb).enabled);
    CHECK(r.plan.stage(StageId::Echo).enabled);
    CHECK(r.output.numChannels() == 2);
    CHECK(out.reverb.found);
    CHECK_LE(relErr(out.reverb.rt60, F->reverb.rt60), 0.25);
    CHECK_NEAR(out.reverb.tailLevelDb, F->reverb.tailLevelDb, 3.0);
    CHECK(out.echo.found);
    CHECK_NEAR(out.echo.delayMs, F->echo.delayMs, 1.5);
    CHECK_NEAR(out.echo.levelDb, F->echo.levelDb, 3.0);
    CHECK_NEAR(out.loudness.maxMomentary, F->loudness.maxMomentary, 1.0);
    CHECK_NEAR(out.bodyDb, F->bodyDb, 1.5);

    // Telephone reference: band limit carried over, nothing else invented.
    const auto phone = bandLimit(ts::pseudoSpeech(SR, 8.0, 34, -20), 300, 3400, 6);
    rq.controls.fxReference = std::make_shared<FxProfile>(analyzeEffects(phone, "phone"));
    AudioBuffer voice = ts::pseudoSpeech(SR, 6.0, 35, -26);
    rq.input = &voice;
    ProcessResult r2 = process(rq, {});
    const auto out2 = analyzeEffects(r2.output, "output");
    REPORT("telephone match -> %s", describeEffects(out2).c_str());
    CHECK(!r2.plan.stage(StageId::Reverb).enabled && !r2.plan.stage(StageId::Echo).enabled);
    CHECK_NEAR(std::log2(out2.bandLimit.hpfHz / 300.0), 0.0, 0.4);
    CHECK_NEAR(std::log2(out2.bandLimit.lpfHz / 3400.0), 0.0, 0.4);
}

TEST("fx match: the hits' envelope (sustain / compression) carried over")
{
    // Known envelope change, test-local: the decay of every hit multiplied by
    // e^(k t) for 150 ms after each onset (k > 0 fuller, as fast-attack
    // compression does; k < 0 tighter).
    auto shape = [](const AudioBuffer& x, double k, double interval) {
        AudioBuffer y = x;
        const size_t step = size_t(interval * SR), start = size_t(0.05 * SR);
        for (int c = 0; c < y.numChannels(); ++c)
            for (size_t i = start; i < y.numFrames(); ++i)
            {
                const double t = double((i - start) % step) / SR;
                y.channel(c)[i] = float(y.channel(c)[i] * std::exp(k * std::min(t, 0.15)));
            }
        return y;
    };
    ProcessRequest rq;
    rq.preset = findPreset("sfx.transparent");
    rq.category = Category::SoundEffect;
    for (double k : { 12.0, -12.0 })
    {
        const AudioBuffer ref = shape(ts::impacts(SR, 1, 8.0, 0.9, 61), k, 0.9);
        auto F = std::make_shared<FxProfile>(analyzeEffects(ref, "shaped hits"));
        AudioBuffer target = ts::impacts(SR, 1, 6.0, 0.8, 62);
        rq.input = &target;
        rq.controls.fxReference = F;
        ProcessResult r = process(rq, {});
        const double before = eventBodyDb(target), after = eventBodyDb(r.output);
        const auto& tsp = r.plan.stage(StageId::Transient);
        REPORT("hits %s: body %.1f -> %.1f dB re peak (reference %.1f); %s", k > 0 ? "fuller" : "tighter", before, after, F->bodyDb,
               tsp.params.empty() ? tsp.reason.c_str() : tsp.params[0].second.c_str());
        CHECK_LE(std::abs(after - F->bodyDb), std::max(1.5, 0.4 * std::abs(before - F->bodyDb)));
    }
}
