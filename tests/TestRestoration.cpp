// Restoration validation: denoiser accuracy + artifact metrics, hum, clicks,
// declipping, plosives, de-essing, dereverberation, neural enhancer timing.

#include "TestFramework.h"
#include "TestSignals.h"

#include <ac/Analysis.h>
#include <ac/Fft.h>
#include <ac/Restoration.h>

using namespace ac;

namespace {
constexpr double SR = 48000.0;

/** Mask of samples where the clean signal is silent (gaps), dilated by 60 ms
    (the STFT window and release smoothing legitimately reach that far). */
std::vector<bool> gapMask(const AudioBuffer& clean)
{
    const size_t n = clean.numFrames();
    std::vector<bool> active(n, false);
    const size_t d = size_t(0.06 * clean.sampleRate);
    for (size_t i = 0; i < n; ++i)
        if (std::abs(clean.channel(0)[i]) > 1e-4)
            for (size_t j = (i > d ? i - d : 0); j < std::min(n, i + d); ++j) active[j] = true;
    std::vector<bool> gap(n);
    for (size_t i = 0; i < n; ++i) gap[i] = !active[i];
    return gap;
}

double maskedRmsDb(const AudioBuffer& b, const std::vector<bool>& m, bool want = true)
{
    double s = 0;
    size_t k = 0;
    for (size_t i = 0; i < std::min(m.size(), b.numFrames()); ++i)
        if (m[i] == want) { s += double(b.channel(0)[i]) * b.channel(0)[i]; ++k; }
    return k ? powerToDb(s / k, -300) : -300;
}

/** Std-dev (dB) of the log-magnitude spectrogram over gap frames: rises with musical noise. */
double spectralRoughnessDb(const AudioBuffer& b, const std::vector<bool>& gap)
{
    const int N = 1024;
    RealFft fft(N);
    auto w = hannWindow(N);
    std::vector<float> fr(static_cast<size_t>(N));
    std::vector<std::complex<float>> sp(static_cast<size_t>(N / 2 + 1));
    std::vector<double> vals;
    for (size_t s = 0; s + N < b.numFrames(); s += N / 2)
    {
        bool allGap = true;
        for (size_t i = s; i < s + N; i += 64) allGap = allGap && gap[i];
        if (!allGap) continue;
        for (int i = 0; i < N; ++i) fr[size_t(i)] = b.channel(0)[s + size_t(i)] * w[size_t(i)];
        fft.forward(fr.data(), sp.data());
        for (int k = 20; k < N / 2 - 20; ++k) vals.push_back(10 * std::log10(std::norm(sp[size_t(k)]) + 1e-30));
    }
    double m = 0;
    for (double v : vals) m += v;
    m /= std::max<size_t>(1, vals.size());
    double var = 0;
    for (double v : vals) var += (v - m) * (v - m);
    return std::sqrt(var / std::max<size_t>(1, vals.size()));
}
} // namespace

TEST("noise profile: estimated floor matches the true noise level")
{
    for (bool pink : { false, true })
    {
        AudioBuffer clean = ts::pseudoSpeech(SR, 10.0, 3, -20);
        AudioBuffer mix = clean;
        ts::add(mix, ts::noise(SR, 1, 10.0, -55, 99, pink));
        const auto np = NoiseProfile::estimate(mix, 2048);
        REPORT("%s noise -55 dBFS -> estimated %.2f dBFS, flatness %.2f, stationarity %.2f", pink ? "pink" : "white", np.levelDb,
               np.flatness, np.stationarity);
        CHECK_NEAR(np.levelDb, -55.0, 1.5);
        CHECK_GE(np.stationarity, 0.7);
    }
}

TEST("denoiser: reduction depth, speech preservation, no musical noise")
{
    for (double noiseDb : { -50.0, -35.0 })
    {
        AudioBuffer clean = ts::pseudoSpeech(SR, 12.0, 5, -20);
        AudioBuffer nz = ts::noise(SR, 1, 12.0, noiseDb, 17, true);
        AudioBuffer mix = clean;
        ts::add(mix, nz);
        DenoiseSettings d;
        d.profile = NoiseProfile::estimate(mix, 2048);
        d.reductionDb = 15.0;
        d.oversubtraction = 1.2;
        SpectralDenoiser dn(d);
        AudioBuffer out = ts::render(dn, mix);
        const auto gap = gapMask(clean);
        const double inGap = maskedRmsDb(mix, gap), outGap = maskedRmsDb(out, gap);
        AudioBuffer errIn = mix, errOut = out;
        ts::add(errIn, clean, -1.0);
        ts::add(errOut, clean, -1.0);
        const double snrIn = ts::rmsDb(clean) - ts::rmsDb(errIn), snrOut = ts::rmsDb(clean) - ts::rmsDb(errOut);
        const double activeIn = maskedRmsDb(clean, gap, false), activeOut = maskedRmsDb(out, gap, false);
        const double roughIn = spectralRoughnessDb(nz, gap), roughOut = spectralRoughnessDb(out, gap);
        AudioBuffer removed = mix;
        ts::add(removed, out, -1.0);
        double dot = 0, cc = 0;
        for (size_t i = 0; i < clean.numFrames(); ++i) { dot += removed.channel(0)[i] * clean.channel(0)[i]; cc += clean.channel(0)[i] * clean.channel(0)[i]; }
        REPORT("pink noise %.0f dBFS: gaps %.1f -> %.1f dB (%.1f dB reduction, bound 15); SNR %.1f -> %.1f dB", noiseDb, inGap, outGap,
               inGap - outGap, snrIn, snrOut);
        REPORT("   active level change %.2f dB; speech leakage into removed %.3f; residual roughness %.2f dB vs noise %.2f dB",
               activeOut - activeIn, dot / cc, roughOut, roughIn);
        CHECK_GE(inGap - outGap, 11.0);
        CHECK_LE(inGap - outGap, 15.5); // never exceeds the attenuation bound
        CHECK_GE(snrOut - snrIn, noiseDb > -40 ? 3.5 : 2.0); // waveform SNR also counts suppression of low-SNR speech parts
        CHECK_NEAR(activeOut - activeIn, 0.0, 1.0);
        CHECK_LE(dot / cc, 0.05);
        CHECK_LE(roughOut, roughIn + 1.5);
    }
}

TEST("denoiser: adaptive tracking follows a noise level change")
{
    AudioBuffer clean = ts::pseudoSpeech(SR, 16.0, 8, -20);
    AudioBuffer nz = ts::noise(SR, 1, 16.0, -55, 4, false);
    for (size_t i = size_t(8 * SR); i < nz.numFrames(); ++i) nz.channel(0)[i] *= 4.0f; // +12 dB at 8 s
    AudioBuffer mix = clean;
    ts::add(mix, nz);
    DenoiseSettings d;
    d.profile = NoiseProfile::estimateFromRegion(mix, 2048, 0, size_t(4 * SR)); // profile of the quiet part only
    d.reductionDb = 15;
    d.adaptive = true;
    SpectralDenoiser dn(d);
    AudioBuffer out = ts::render(dn, mix);
    auto gap = gapMask(clean);
    for (size_t i = 0; i < size_t(10 * SR); ++i) gap[i] = false; // look at the loud-noise half after adaptation
    const double red = maskedRmsDb(mix, gap) - maskedRmsDb(out, gap);
    REPORT("after +12 dB noise step: %.1f dB reduction in gaps", red);
    CHECK_GE(red, 8.0);
}

TEST("hum: detection of 60 Hz / drifting 50 Hz series and removal depth")
{
    for (double f0 : { 60.0, 50.3 })
    {
        AudioBuffer clean = ts::pseudoSpeech(SR, 10.0, 12, -22);
        AudioBuffer tone = ts::sine(SR, 1, 10.0, 1000, 0.05);
        ts::add(clean, tone);
        AudioBuffer mix = clean;
        const double amps[] = { 0.01, 0.006, 0.004, 0.0, 0.003 };
        for (int k = 1; k <= 5; ++k)
            if (amps[k - 1] > 0) ts::add(mix, ts::sine(SR, 1, 10.0, f0 * k, amps[k - 1], 0.3 * k));
        ts::add(mix, ts::noise(SR, 1, 10.0, -70, 3));
        const auto a = analyze(mix, Category::Voice);
        REPORT("f0 %.2f: detected %d at %.3f Hz with %zu harmonics", f0, int(a.hum.detected), a.hum.f0, a.hum.harmonics.size());
        CHECK(a.hum.detected);
        CHECK_NEAR(a.hum.f0, f0, 0.1);
        HumSettings hs;
        hs.fundamentalHz = a.hum.f0;
        for (auto& h : a.hum.harmonics) hs.harmonics.push_back({ h.freq, h.prominenceDb + 3, std::max(1.5, 0.004 * h.freq) });
        HumRemover hr(hs);
        AudioBuffer out = ts::render(hr, mix);
        const size_t st = size_t(2 * SR), n = size_t(6 * SR);
        for (int k : { 1, 2, 3, 5 })
        {
            const double before = ts::toneAmplitude(mix.channel(0) + st, n, f0 * k, SR);
            const double after = ts::toneAmplitude(out.channel(0) + st, n, f0 * k, SR);
            bool notched = false;
            for (auto& h : hs.harmonics) notched |= std::abs(h.freq - f0 * k) < 1.0;
            REPORT("   harmonic %d (%s): %.1f dB reduction", k, notched ? "detected" : "masked by programme, left alone", gainToDb(before / after));
            double prom = 0;
        for (auto& h : a.hum.harmonics)
            if (std::abs(h.freq - f0 * k) < 1.0) prom = h.prominenceDb;
        // Depth is bounded by the measured prominence: hum is taken down to the programme floor.
        if (notched) CHECK_GE(gainToDb(before / after), std::min(15.0, prom));
            if (k <= 2) CHECK(notched);
        }
        const double t0 = ts::toneAmplitude(mix.channel(0) + st, n, 1000, SR), t1 = ts::toneAmplitude(out.channel(0) + st, n, 1000, SR);
        CHECK_NEAR(gainToDb(t1 / t0), 0.0, 0.05);
    }
    // No false detection on clean material.
    const auto clean = analyze(ts::pseudoSpeech(SR, 10.0, 77, -20), Category::Voice);
    CHECK(!clean.hum.detected);
}

TEST("clicks: detection, AR repair accuracy, no false repairs on speech/transients")
{
    AudioBuffer clean = ts::pseudoMusic(SR, 6.0, 31, -20);
    AudioBuffer mono(1, clean.numFrames(), SR);
    mono.vec(0) = clean.vec(0);
    AudioBuffer mix = mono;
    ts::Rng r(5);
    std::vector<size_t> pos;
    for (int k = 0; k < 30; ++k)
    {
        const size_t p = size_t((0.2 + 5.6 * r.uniform()) * SR);
        const int w = 1 + int(r.uniform() * 6);
        const double amp = (r.uniform() < 0.5 ? -1 : 1) * (0.1 + 0.3 * r.uniform());
        for (int i = 0; i < w; ++i) mix.channel(0)[p + size_t(i)] += float(amp * (1.0 - 0.5 * i / w));
        pos.push_back(p);
    }
    ClickRemover cr;
    AudioBuffer out = mix;
    cr.prepare(SR, 1);
    cr.processOffline(out, {});
    double eIn = 0, eOut = 0;
    int fixed = 0;
    for (size_t p : pos)
    {
        double a = 0, b = 0;
        for (size_t i = p - 48; i < p + 48; ++i)
        {
            a += std::pow(mix.channel(0)[i] - mono.channel(0)[i], 2);
            b += std::pow(out.channel(0)[i] - mono.channel(0)[i], 2);
        }
        if (std::getenv("AF_DEBUG")) REPORT("click @%zu: err %.1f -> %.1f dB", p, powerToDb(a), powerToDb(b));
        if (powerToDb(b) < powerToDb(a) - 40.0) ++fixed;
        eIn += a;
        eOut += b;
    }
    REPORT("30 clicks inserted: %d repair operations, %d clicks restored to < -40 dB error, total click error reduced %.1f dB",
           cr.repairedCount(), fixed, powerToDb(eIn / eOut));
    REPORT("(a click within a few ms after a drum onset is deliberately left: it is masked and indistinguishable from the attack)");
    CHECK_GE(fixed, 28);
    CHECK_LE(cr.repairedCount(), 32);
    // Clean material must stay untouched.
    for (auto* name : { "speech", "impacts" })
    {
        AudioBuffer c = std::string(name) == "speech" ? ts::pseudoSpeech(SR, 8.0, 9, -18) : ts::impacts(SR, 1, 8.0, 0.25, 4);
        ClickRemover cr2;
        AudioBuffer o = c;
        cr2.prepare(SR, 1);
        cr2.processOffline(o, {});
        REPORT("clean %s: %d repairs (%d onsets protected)", name, cr2.repairedCount(), cr2.protectedCount());
        CHECK_LE(cr2.repairedCount(), 2);
    }
}

TEST("declip: reconstruction error reduced")
{
    AudioBuffer orig(1, size_t(SR), SR);
    for (size_t i = 0; i < orig.numFrames(); ++i)
        orig.channel(0)[i] = float(0.9 * std::sin(kTwoPi * 220 * i / SR) + 0.15 * std::sin(kTwoPi * 660 * i / SR + 0.4));
    AudioBuffer clip = orig;
    for (auto& v : clip.vec(0)) v = clampv(v, -0.75f, 0.75f);
    DeclipSettings ds;
    ds.clipLevel = 0.75;
    ds.maxRunMs = 3.0;
    Declipper dc(ds);
    AudioBuffer out = clip;
    dc.prepare(SR, 1);
    dc.processOffline(out, {});
    AudioBuffer e1 = clip, e2 = out;
    ts::add(e1, orig, -1);
    ts::add(e2, orig, -1);
    REPORT("%d runs; clipping error %.1f dB -> %.1f dB", dc.repairedRuns(), ts::rmsDb(e1), ts::rmsDb(e2));
    CHECK_GE(dc.repairedRuns(), 100);
    CHECK_LE(ts::rmsDb(e2), ts::rmsDb(e1) - 10.0);
}

TEST("plosives: synthetic pops detected and attenuated, clean speech untouched")
{
    AudioBuffer clean = ts::pseudoSpeech(SR, 10.0, 41, -20);
    AudioBuffer mix = clean;
    std::vector<size_t> pops = { size_t(1.0 * SR), size_t(3.1 * SR), size_t(5.2 * SR), size_t(7.3 * SR), size_t(9.0 * SR) };
    ts::Rng r(3);
    Biquad lp(BiquadCoeffs::lowpass(90, 0.7, SR));
    for (size_t p : pops)
        for (size_t i = 0; i < size_t(0.05 * SR); ++i)
        {
            const double t = double(i) / SR;
            const double v = 0.25 * std::exp(-t * 40) * std::sin(kTwoPi * 45 * t) + 0.5 * lp.process(r.gauss()) * std::exp(-t * 60);
            mix.channel(0)[p + i] += float(v);
        }
    PlosiveSettings ps;
    ps.detectHz = 100;
    ps.applyHz = 150;
    PlosiveReducer pr(ps);
    AudioBuffer out = ts::render(pr, mix);
    double before = 0, after = 0;
    Biquad m1(BiquadCoeffs::lowpass(120, 0.7, SR)), m2(BiquadCoeffs::lowpass(120, 0.7, SR));
    std::vector<float> l1(mix.numFrames()), l2(mix.numFrames());
    for (size_t i = 0; i < mix.numFrames(); ++i) { l1[i] = float(m1.process(mix.channel(0)[i])); l2[i] = float(m2.process(out.channel(0)[i])); }
    for (size_t p : pops)
        for (size_t i = p; i < p + size_t(0.05 * SR); ++i) { before += l1[i] * l1[i]; after += l2[i] * l2[i]; }
    REPORT("%d events; LF energy in pops reduced by %.1f dB", pr.eventCount(), powerToDb(before / after));
    CHECK_GE(pr.eventCount(), 4);
    CHECK_GE(powerToDb(before / after), 6.0);
    PlosiveReducer pr2(ps);
    AudioBuffer o2 = ts::render(pr2, clean);
    REPORT("clean pseudo-speech: %d events, max diff %.2g", pr2.eventCount(), ts::maxAbsDiff(clean, o2));
    CHECK_LE(pr2.eventCount(), 1);
}

TEST("de-esser: sibilants reduced, vowels unaffected")
{
    // Voiced-only pseudo speech plus loud "s" bursts at known positions.
    AudioBuffer x = ts::pseudoSpeech(SR, 10.0, 51, -20, false);
    std::vector<size_t> ss;
    ts::Rng r(9);
    Biquad hp(BiquadCoeffs::highpass(5000, 0.7, SR));
    for (double t = 0.7; t < 9.5; t += 1.3)
    {
        const size_t p = size_t(t * SR);
        ss.push_back(p);
        for (size_t i = 0; i < size_t(0.12 * SR); ++i)
            x.channel(0)[p + i] = float(0.4 * std::sin(kPi * double(i) / (0.12 * SR)) * hp.process(r.gauss()));
    }
    std::vector<bool> isS(x.numFrames(), false);
    for (size_t p : ss)
        for (size_t i = (p > 2400 ? p - 2400 : 0); i < std::min(x.numFrames(), p + size_t(0.12 * SR) + 2400); ++i) isS[i] = true;
    DeEssSettings d;
    d.thresholdDb = 0.0;
    d.maxReductionDb = 8.0;
    d.absFloorDb = -60;
    DeEsser de(d);
    AudioBuffer out = ts::render(de, x);
    double sIn = 0, sOut = 0, vIn = 0, vOut = 0;
    for (size_t i = 0; i < x.numFrames(); ++i)
    {
        const double a = double(x.channel(0)[i]) * x.channel(0)[i], b = double(out.channel(0)[i]) * out.channel(0)[i];
        if (isS[i]) { sIn += a; sOut += b; }
        else { vIn += a; vOut += b; }
    }
    REPORT("sibilant bursts reduced %.1f dB; voiced passages changed %.2f dB; max GR %.1f dB", powerToDb(sIn / sOut),
           powerToDb(vOut / vIn), de.grTrace()->maxValue());
    CHECK_GE(powerToDb(sIn / sOut), 3.0);
    CHECK_NEAR(powerToDb(vOut / vIn), 0.0, 0.3);
}

TEST("dereverb: RT60 estimate and late tail suppression")
{
    AudioBuffer dry = ts::pseudoSpeech(SR, 12.0, 61, -20);
    // Exponentially decaying noise IR, RT60 0.8 s, plus direct path.
    const double rt = 0.8;
    const size_t L = size_t(rt * SR);
    std::vector<float> ir(L);
    ts::Rng r(7);
    // Diffuse tail scaled for a direct-to-reverberant ratio of about 0 dB.
    for (size_t i = 0; i < L; ++i) ir[i] = float(0.02 * r.gauss() * std::exp(-6.9078 * double(i) / (rt * SR)));
    ir[0] = 1.0f;
    AudioBuffer wet(1, dry.numFrames(), SR);
    // FFT convolution would be faster; direct sparse convolution over the speech is fine for a test.
    {
        const int N = 1 << 16;
        RealFft fft(N);
        std::vector<float> hpad(static_cast<size_t>(N), 0.0f);
        std::copy(ir.begin(), ir.end(), hpad.begin());
        std::vector<std::complex<float>> H(static_cast<size_t>(N / 2 + 1)), X(static_cast<size_t>(N / 2 + 1));
        fft.forward(hpad.data(), H.data());
        const size_t B = size_t(N) - L;
        std::vector<float> blk(static_cast<size_t>(N)), y(static_cast<size_t>(N));
        for (size_t s = 0; s < dry.numFrames(); s += B)
        {
            std::fill(blk.begin(), blk.end(), 0.0f);
            for (size_t i = 0; i < B && s + i < dry.numFrames(); ++i) blk[i] = dry.channel(0)[s + i];
            fft.forward(blk.data(), X.data());
            for (size_t k = 0; k < X.size(); ++k) X[k] *= H[k];
            fft.inverse(X.data(), y.data());
            for (size_t i = 0; i < size_t(N) && s + i < wet.numFrames(); ++i) wet.channel(0)[s + i] += y[i];
        }
    }
    const auto a = analyze(wet, Category::Voice);
    REPORT("RT60 true %.2f s, estimated %.2f s", rt, a.rt60);
    CHECK(a.rt60 > 0.4 && a.rt60 < 1.6);
    DereverbSettings ds;
    ds.rt60 = a.rt60 > 0 ? a.rt60 : 0.8;
    ds.reductionDb = 10;
    Dereverberator dr(ds);
    AudioBuffer out = ts::render(dr, wet);
    const auto gap = gapMask(dry);
    const double tailIn = maskedRmsDb(wet, gap), tailOut = maskedRmsDb(out, gap);
    const double actIn = maskedRmsDb(wet, gap, false), actOut = maskedRmsDb(out, gap, false);
    REPORT("reverb tail in gaps reduced %.1f dB; active level change %.2f dB", tailIn - tailOut, actOut - actIn);
    CHECK_GE(tailIn - tailOut, 3.0);
    CHECK_GE(actOut - actIn, -3.0);
}

TEST("neural speech enhancer: latency compensated, noise reduced")
{
    AudioBuffer clean = ts::pseudoSpeech(SR, 8.0, 71, -20);
    AudioBuffer mix = clean;
    ts::add(mix, ts::noise(SR, 1, 8.0, -40, 8, true));
    NeuralDenoiseSettings ns;
    ns.mix = 1.0;
    NeuralSpeechDenoiser nd(ns);
    AudioBuffer out = mix;
    nd.prepare(SR, 1);
    nd.processOffline(out, {});
    const int lag = ts::bestLag(clean.vec(0), out.vec(0), 2000);
    const auto gap = gapMask(clean);
    REPORT("alignment lag %d samples; gap noise %.1f -> %.1f dB", lag, maskedRmsDb(mix, gap), maskedRmsDb(out, gap));
    CHECK(std::abs(lag) <= 1);
    CHECK_GE(maskedRmsDb(mix, gap) - maskedRmsDb(out, gap), 10.0);
    // Works at other sample rates (internal r8brain conversion), still aligned.
    AudioBuffer c44 = ts::pseudoSpeech(44100, 5.0, 72, -20);
    AudioBuffer m44 = c44;
    ts::add(m44, ts::noise(44100, 1, 5.0, -40, 9, true));
    NeuralSpeechDenoiser nd2(ns);
    nd2.prepare(44100, 1);
    nd2.processOffline(m44, {});
    const int lag44 = ts::bestLag(c44.vec(0), m44.vec(0), 2000);
    REPORT("44.1 kHz lag %d", lag44);
    CHECK(std::abs(lag44) <= 1);
}
