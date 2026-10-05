// Decision engine: derives every stage's enable state and settings from the
// analysis, bounded by the preset's constraints and scaled by the user's
// Cleanup / Tone / Dynamics macros. Every decision records its reason.

#include "Planning.h"

#include <cstdarg>
#include <cstdio>

namespace ac {

std::string fmt(const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

namespace {

void decide(StagePlan& sp, bool autoOn, const std::string& reason, const UserControls& uc)
{
    sp.reason = reason;
    if (!sp.available)
    {
        sp.enabled = false;
        return;
    }
    if (!uc.sectionOn[int(stageSection(sp.id))])
    {
        sp.enabled = false;
        sp.reason = std::string(sectionName(stageSection(sp.id))) + " section switched off. (" + reason + ")";
        return;
    }
    auto it = uc.overrides.find(sp.id);
    const Override ov = it == uc.overrides.end() ? Override::Auto : it->second;
    if (ov == Override::Off)
    {
        sp.enabled = false;
        sp.forced = true;
        sp.reason = "Bypassed by user. (Auto: " + std::string(autoOn ? "on" : "off") + " - " + reason + ")";
    }
    else if (ov == Override::On)
    {
        sp.enabled = true;
        sp.forced = !autoOn;
        sp.reason = (autoOn ? "" : "Forced on by user. (Auto: off - ") + reason + (autoOn ? "" : ")");
    }
    else sp.enabled = autoOn;
}

void param(StagePlan& sp, const std::string& k, const std::string& v) { sp.params.emplace_back(k, v); }

double eqResponseAt(const std::vector<EqBand>& bands, double f, double sr) { return ParametricEq::responseDb(bands, f, sr); }

/** Fit a small set of broad EQ bands to a deviation curve (dB per 1/3-oct band). */
std::vector<EqBand> fitEq(const std::vector<double>& hz, std::vector<double> dev, double maxBoost, double maxCut,
                          double sr, Category cat, double minBellHz, double maxHighBoost, double maxLowCut, int maxBells = 3)
{
    // Intent guards applied to the target before fitting...
    for (size_t i = 0; i < hz.size(); ++i)
    {
        if (hz[i] >= 1500.0) dev[i] = std::min(dev[i], maxHighBoost);
        if (hz[i] <= 300.0) dev[i] = std::max(dev[i], -maxLowCut);
    }
    std::vector<EqBand> bands;
    const size_t n = hz.size();
    auto avg = [&](double lo, double hi) {
        double s = 0;
        int c = 0;
        for (size_t i = 0; i < n; ++i)
            if (hz[i] >= lo && hz[i] <= hi) { s += dev[i]; ++c; }
        return c ? s / c : 0.0;
    };
    const double lsF = cat == Category::Voice ? 150.0 : 90.0;
    const double ls = clampv(avg(30.0, lsF * 0.8), -maxCut, maxBoost);
    if (std::abs(ls) >= 0.5) bands.push_back({ SvfType::LowShelf, lsF, 0.6, ls, true });
    const double hsF = 8000.0;
    if (hsF < 0.4 * sr)
    {
        const double hs = clampv(avg(hsF * 1.2, 16000.0), -maxCut, maxBoost);
        if (std::abs(hs) >= 0.5) bands.push_back({ SvfType::HighShelf, hsF, 0.6, hs, true });
    }
    std::vector<bool> used(n, false);
    for (int it = 0; it < maxBells; ++it)
    {
        size_t best = n;
        double bestAbs = 1.0;
        std::vector<double> res(n);
        for (size_t i = 0; i < n; ++i) res[i] = dev[i] - eqResponseAt(bands, hz[i], sr);
        auto conflicts = [&](size_t i) {
            // Never place a bell that fights an existing band within an octave
            // (opposite-signed overlapping bands waste headroom and smear phase).
            for (auto& b : bands)
                if (std::abs(std::log2(hz[i] / b.freq)) < 1.0 && b.gainDb * res[i] < 0) return true;
            return false;
        };
        for (size_t i = 0; i < n; ++i)
            if (!used[i] && hz[i] >= minBellHz && hz[i] <= 7000.0 && std::abs(res[i]) > bestAbs && !conflicts(i))
            {
                bestAbs = std::abs(res[i]);
                best = i;
            }
        if (best == n) break;
        for (size_t j = (best > 0 ? best - 1 : 0); j <= std::min(n - 1, best + 1); ++j) used[j] = true;
        const double g = res[best];
        size_t lo = best, hi = best;
        while (lo > 0 && res[lo - 1] * g > 0 && std::abs(res[lo - 1]) > std::abs(g) / 2) --lo;
        while (hi + 1 < n && res[hi + 1] * g > 0 && std::abs(res[hi + 1]) > std::abs(g) / 2) ++hi;
        const double bw = std::max(hz[hi] * std::pow(2.0, 1.0 / 6.0) - hz[lo] * std::pow(2.0, -1.0 / 6.0), hz[best] * 0.3);
        const double q = clampv(hz[best] / bw, 0.5, 2.0);
        bands.push_back({ SvfType::Bell, hz[best], q, clampv(g * 0.85, -maxCut, maxBoost), true });
    }
    // Enforce the boost/cut bounds on the combined response.
    double mx = 0, mn = 0;
    for (double f : hz)
    {
        const double r = eqResponseAt(bands, f, sr);
        mx = std::max(mx, r);
        mn = std::min(mn, r);
    }
    if (mx > maxBoost && mx > 0)
        for (auto& b : bands)
            if (b.gainDb > 0) b.gainDb *= maxBoost / mx;
    if (-mn > maxCut && mn < 0)
        for (auto& b : bands)
            if (b.gainDb < 0) b.gainDb *= maxCut / -mn;
    // ...and to the fitted bands (a wide bell can still reach into a guarded region).
    for (auto& b : bands)
    {
        if (b.freq >= 1200.0 && b.gainDb > maxHighBoost) b.gainDb = maxHighBoost;
        if (b.freq <= 350.0 && b.gainDb < -maxLowCut) b.gainDb = -maxLowCut;
    }
    std::vector<EqBand> out;
    for (auto& b : bands)
        if (std::abs(b.gainDb) >= 0.3) out.push_back(b);
    return out;
}

std::vector<EqBand> resonanceCuts(const SpectrumInfo& s, int maxCuts, double maxCut, double strength, double minHz,
                                  std::vector<std::string>& why)
{
    struct Peak { double f, ex, q; };
    std::vector<Peak> peaks;
    const auto& f = s.fineHz;
    const auto& d = s.fineDb;
    for (size_t i = 12; i + 12 < f.size(); ++i)
    {
        if (f[i] < minHz || f[i] > 8000) continue;
        double tr = 0;
        for (size_t j = i - 12; j <= i + 12; ++j) tr += d[j];
        tr /= 25.0;
        const double ex = d[i] - tr;
        if (ex < 5.0 || d[i] < d[i - 1] || d[i] < d[i + 1]) continue;
        size_t lo = i, hi = i;
        while (lo > i - 12 && d[lo - 1] - tr > ex / 2) --lo;
        while (hi < i + 12 && d[hi + 1] - tr > ex / 2) ++hi;
        const double bw = std::max(f[hi] - f[lo], f[i] * 0.03);
        peaks.push_back({ f[i], ex, clampv(f[i] / bw, 2.0, 12.0) });
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.ex > b.ex; });
    std::vector<EqBand> out;
    for (auto& p : peaks)
    {
        if (int(out.size()) >= maxCuts) break;
        bool near = false;
        for (auto& o : out)
            if (std::abs(std::log2(o.freq / p.f)) < 0.33) near = true;
        if (near) continue;
        const double g = -std::min(maxCut, (p.ex - 3.0) * 0.8 * strength);
        if (g > -0.5) continue;
        out.push_back({ SvfType::Bell, p.f, p.q, g, true });
        why.push_back(fmt("resonance +%.1f dB at %.0f Hz", p.ex, p.f));
    }
    return out;
}

std::string bandsText(const std::vector<EqBand>& b)
{
    std::string s;
    for (auto& e : b)
    {
        const char* t = e.type == SvfType::LowShelf ? "LS" : e.type == SvfType::HighShelf ? "HS" : "Bell";
        s += fmt("%s %.0f Hz %+.1f dB Q%.2f; ", t, e.freq, e.gainDb, e.q);
    }
    return s.empty() ? "flat" : s;
}

/** Threshold such that the static curve yields `gr` dB of reduction at level L. */
double thresholdFor(double L, double gr, const CompressorSettings& cs)
{
    double lo = L - 60, hi = L;
    CompressorSettings s = cs;
    s.maxGrDb = 100;
    for (int i = 0; i < 60; ++i)
    {
        s.thresholdDb = 0.5 * (lo + hi);
        const double g = -Compressor::gainComputer(L, s);
        if (g > gr) lo = s.thresholdDb;
        else hi = s.thresholdDb;
    }
    return 0.5 * (lo + hi);
}

} // namespace

double compressorThreshold(const AudioBuffer& x, const CompressorSettings& c, double targetGr, double gateDb, double* p95Out)
{
    const double winMs = c.detector == DetectorMode::Rms ? c.rmsWindowMs : 1.0;
    const auto st = rmsLevelStats(x, winMs, gateDb, c.sidechainHpfHz);
    if (p95Out) *p95Out = st.p95;
    return thresholdFor(st.p95, std::max(0.1, targetGr), c);
}

std::vector<EqBand> fitEqBands(const std::vector<double>& hz, const std::vector<double>& dev, double maxBoost, double maxCut, double sr,
                               Category cat, double minBellHz, int maxBells)
{
    return fitEq(hz, dev, maxBoost, maxCut, sr, cat, minBellHz, 99.0, 99.0, maxBells);
}

std::vector<double> referenceDeviation(const SpectrumInfo& src, const SpectrumInfo& ref, double* rmsOut)
{
    const size_t n = src.bandHz.size();
    std::vector<double> dev(n, 0.0);
    std::vector<bool> have(n, false);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < ref.bandHz.size(); ++j)
            if (std::abs(ref.bandHz[j] / src.bandHz[i] - 1.0) < 0.02 && ref.bandAbsDb[j] > -150 && src.bandAbsDb[i] > -150)
            {
                dev[i] = ref.bandDb[j] - src.bandDb[i];
                have[i] = true;
            }
    double m = 0;
    int c = 0;
    for (size_t i = 0; i < n; ++i)
        if (have[i] && src.bandHz[i] >= 200 && src.bandHz[i] <= 4000) { m += dev[i]; ++c; }
    m = c ? m / c : 0;
    for (size_t i = 0; i < n; ++i) dev[i] = have[i] ? dev[i] - m : 0.0;
    std::vector<double> sm(n);
    for (size_t i = 0; i < n; ++i)
    {
        const double l = i ? dev[i - 1] : dev[i], r = i + 1 < n ? dev[i + 1] : dev[i];
        sm[i] = 0.25 * l + 0.5 * dev[i] + 0.25 * r;
    }
    if (rmsOut)
    {
        double ss = 0;
        int k = 0;
        for (size_t i = 0; i < n; ++i)
            if (have[i] && src.bandHz[i] >= 100 && src.bandHz[i] <= 12000) { ss += dev[i] * dev[i]; ++k; }
        *rmsOut = k ? std::sqrt(ss / k) : 0.0;
    }
    return sm;
}

void initPlan(Plan& plan)
{
    plan.stages.clear();
    for (int i = 0; i < int(StageId::Count); ++i)
    {
        StagePlan sp;
        sp.id = StageId(i);
        plan.stages.push_back(sp);
    }
}

void planCleanupTone(Plan& plan, const AnalysisReport& a, const PresetDef& p, const UserControls& uc, const AudioBuffer& in)
{
    auto& S = plan.settings;
    const Category cat = p.category;
    const double sr = in.sampleRate;
    const double cl = clampv(uc.cleanup, 0.0, 2.0);
    const double tn = clampv(uc.tone, 0.0, 2.0);
    const bool voice = cat == Category::Voice;
    const bool music = cat == Category::Music;

    // ------------------------------------------------------------- Filter
    {
        auto& sp = plan.stage(StageId::Filter);
        auto& fs = S.filter;
        const bool dc = a.dcOffsetMaxDb > -80.0;
        if (dc) fs.dcOffset = a.dcOffset;
        std::string why;
        if (dc) why += fmt("DC offset %.1f dBFS removed exactly. ", a.dcOffsetMaxDb);
        double hpf = 0;
        int order = p.hpfOrder;
        if (voice)
        {
            if (a.speech.f0LowHz > 0)
            {
                hpf = clampv(0.7 * a.speech.f0LowHz, 50.0, p.hpfMaxHz);
                why += fmt("Voice F0 P10 %.0f Hz -> high-pass %.0f Hz (below the voice). ", a.speech.f0LowHz, hpf);
            }
            else
            {
                hpf = std::min(75.0, p.hpfMaxHz);
                why += fmt("No reliable F0; default voice high-pass %.0f Hz. ", hpf);
            }
            if (a.spectrum.rumbleRelDb > -20.0)
            {
                order = std::max(order, 4);
                why += fmt("Rumble %.0f dB rel. -> %d dB/oct. ", a.spectrum.rumbleRelDb, order * 6);
            }
        }
        else
        {
            const double lim = music ? -35.0 : -30.0;
            if (a.spectrum.subsonicRelDb > lim)
            {
                hpf = p.hpfMaxHz;
                why += fmt("Subsonic energy %.0f dB rel. -> %.0f Hz high-pass. ", a.spectrum.subsonicRelDb, hpf);
            }
            else if (p.hpfAlways)
            {
                hpf = p.hpfMaxHz;
                why += fmt("Preset band-limit at %.0f Hz. ", hpf);
            }
            else why += fmt("No subsonic content (%.0f dB rel.): no high-pass, low end untouched. ", a.spectrum.subsonicRelDb);
        }
        fs.hpfHz = hpf;
        fs.hpfOrder = order;
        sp.params = {};
        param(sp, "DC offset", dc ? fmt("%.6f", a.dcOffset.empty() ? 0.0 : a.dcOffset[0]) : "none");
        param(sp, "High-pass", hpf > 0 ? fmt("%.1f Hz, %d dB/oct Butterworth", hpf, order * 6) : "off");
        decide(sp, dc || hpf > 0, why, uc);
    }

    // ------------------------------------------------------------- Declip
    {
        auto& sp = plan.stage(StageId::Declip);
        S.declip.clipLevel = a.clipping.level;
        S.declip.maxRunMs = voice ? 3.0 : 2.0;
        param(sp, "Clip level", fmt("%.2f dBFS", gainToDb(a.clipping.level)));
        param(sp, "Max run", fmt("%.1f ms", S.declip.maxRunMs));
        decide(sp, a.clipping.likely,
               a.clipping.likely ? fmt("%d clipped runs (%.3f%% of samples): AR reconstruction above the clip level.",
                                       a.clipping.runs, a.clipping.clippedPercent)
                                 : fmt("No clipping detected (%d flat runs).", a.clipping.runs),
               uc);
    }

    // ------------------------------------------------------------ Declick
    {
        auto& sp = plan.stage(StageId::Declick);
        S.declick = clickSettingsFor(cat);
        S.declick.threshold = p.clickThreshold * (1.25 - 0.25 * cl);
        param(sp, "Threshold", fmt("%.1f residual sigma", S.declick.threshold));
        param(sp, "Max click length", fmt("%.1f ms", S.declick.maxClickMs));
        param(sp, "Transient protection", fmt("on (onset ratio %.0f, min peak %.1fx threshold)", S.declick.onsetRatio, S.declick.minPeakRatio));
        const bool on = a.clicksPerMinute >= p.clickMinPerMinute;
        decide(sp, on,
               fmt("%.1f impulsive events/min detected (bypass below %.1f/min).", a.clicksPerMinute, p.clickMinPerMinute), uc);
    }

    // -------------------------------------------------------------- Dehum
    {
        auto& sp = plan.stage(StageId::Dehum);
        auto& hs = S.dehum;
        hs.fundamentalHz = a.hum.f0;
        hs.harmonics.clear();
        const double minProm = music ? 10.0 : 6.0;
        for (auto& h : a.hum.harmonics)
        {
            if (h.prominenceDb < minProm) continue;
            const double depth = std::min(p.humMaxDepthDb, (h.prominenceDb + 3.0) * clampv(cl, 0.5, 1.5));
            hs.harmonics.push_back({ h.freq, depth, std::max(1.5, 0.004 * h.freq) });
        }
        std::string list;
        for (auto& h : hs.harmonics) list += fmt("%.1f Hz -%.0f dB; ", h.freq, h.depthDb);
        param(sp, "Fundamental", a.hum.detected ? fmt("%.2f Hz", a.hum.f0) : "-");
        param(sp, "Notches", list.empty() ? "none" : list);
        decide(sp, a.hum.detected && !hs.harmonics.empty(),
               a.hum.detected ? fmt("Mains hum at %.2f Hz, %zu harmonics notched with depth = measured prominence.", a.hum.f0,
                                    hs.harmonics.size())
                              : "No mains hum detected.",
               uc);
    }

    // ------------------------------------------------------------ Denoise
    {
        auto& sp = plan.stage(StageId::Denoise);
        auto& ds = S.denoise;
        const int N = StftProcessor::defaultFftSize(sr);
        if (uc.noiseRegion)
            ds.profile = NoiseProfile::estimateFromRegion(in, N, uc.noiseRegion->first, uc.noiseRegion->second);
        else ds.profile = a.noise;
        const double needed = ds.profile.levelDb - p.targetNoiseFloorDb;
        const double maxNr = music ? std::min(p.maxNoiseReductionDb, 8.0) : p.maxNoiseReductionDb;
        double red = clampv(needed * cl, 0.0, maxNr);
        ds.reductionDb = std::max(red, 3.0);
        ds.oversubtraction = p.denoiseStrength * (0.85 + 0.15 * cl);
        ds.adaptive = ds.profile.stationarity < 0.6;
        ds.ddAlpha = music || p.id == "sfx.ambience" ? 0.98 : (p.id == "voice.rescue" ? 0.96 : 0.97);
        ds.releaseMs = music ? 80.0 : 60.0;
        ds.freqSmoothing = music ? 0.03 : 0.04;
        ds.lowCutoffHz = p.id == "sfx.ambience" ? 150.0 : 0.0;
        ds.linkChannels = true;
        param(sp, "Noise floor", fmt("%.1f dBFS (flatness %.2f, stationarity %.2f)%s", ds.profile.levelDb, ds.profile.flatness,
                                     ds.profile.stationarity, uc.noiseRegion ? " [user region]" : ""));
        param(sp, "Max attenuation", fmt("%.1f dB (preset bound %.0f dB)", ds.reductionDb, maxNr));
        param(sp, "Over-subtraction", fmt("%.2f", ds.oversubtraction));
        param(sp, "Gain rule", "MMSE-LSA, decision-directed a-priori SNR");
        param(sp, "DD smoothing", fmt("%.2f", ds.ddAlpha));
        param(sp, "Tracking", ds.adaptive ? "adaptive (minimum statistics)" : "static profile");
        param(sp, "FFT", fmt("%d, hop %d, sqrt-Hann", N, N / 4));
        param(sp, "Release", fmt("%.0f ms", ds.releaseMs));
        param(sp, "Stereo", "linked gains (image preserved)");
        if (ds.lowCutoffHz > 0) param(sp, "Untouched below", fmt("%.0f Hz", ds.lowCutoffHz));
        bool on;
        std::string why;
        if (!ds.profile.valid) { on = false; why = "No usable noise estimate (digital silence / too short)."; }
        else if (!p.denoiseDefault)
        {
            on = false;
            why = fmt("Music: noise reduction off by default (floor %.1f dBFS, SNR %.0f dB).", ds.profile.levelDb, a.snrDb);
        }
        else if (a.snrDb >= p.minSnrForNoNr)
        {
            on = false;
            why = fmt("SNR %.0f dB >= %.0f dB: floor already clean.", a.snrDb, p.minSnrForNoNr);
        }
        else if (red < 3.0)
        {
            on = false;
            why = fmt("Noise floor %.1f dBFS already near the %.0f dBFS target.", ds.profile.levelDb, p.targetNoiseFloorDb);
        }
        else
        {
            on = true;
            why = fmt("Noise floor %.1f dBFS, SNR %.0f dB -> reduce by up to %.1f dB toward %.0f dBFS.", ds.profile.levelDb,
                      a.snrDb, red, p.targetNoiseFloorDb);
        }
        decide(sp, on, why, uc);
    }

    // ------------------------------------------------------------- Neural
    {
        auto& sp = plan.stage(StageId::Neural);
        sp.available = voice;
        S.neural.mix = clampv(p.neuralMix * clampv(cl, 0.5, 1.2), 0.0, 1.0);
        S.neural.maxAttenuationDb = p.maxNoiseReductionDb + 6.0;
        param(sp, "Model", "RNNoise 0.2 (bundled), 48 kHz");
        param(sp, "Mix", fmt("%.0f%%", S.neural.mix * 100));
        param(sp, "Attenuation bound", fmt("%.0f dB", S.neural.maxAttenuationDb));
        if (!voice) decide(sp, false, "Speech model: available for Voice Clip only.", uc);
        else
        {
            const bool on = p.neuralBelowSnrDb > 0 && a.snrDb < p.neuralBelowSnrDb;
            decide(sp, on,
                   p.neuralBelowSnrDb > 0 ? fmt("SNR %.0f dB %s %.0f dB auto threshold.", a.snrDb, on ? "<" : ">=", p.neuralBelowSnrDb)
                                          : "Optional: enable for non-stationary noise (preset leaves it off).",
                   uc);
        }
    }

    // ----------------------------------------------------------- Dereverb
    {
        auto& sp = plan.stage(StageId::Dereverb);
        S.dereverb.rt60 = a.rt60 > 0 ? a.rt60 : 0.5;
        S.dereverb.reductionDb = p.dereverbMaxDb * clampv(cl, 0.25, 1.0);
        S.dereverb.fftSize = StftProcessor::defaultFftSize(sr);
        param(sp, "RT60", a.rt60 > 0 ? fmt("%.2f s (estimated)", a.rt60) : "0.50 s (default; not measurable)");
        param(sp, "Max attenuation", fmt("%.1f dB", S.dereverb.reductionDb));
        param(sp, "Late onset", fmt("%.0f ms", S.dereverb.delayMs));
        const bool on = p.dereverbAutoAboveRt60 > 0 && a.rt60 > p.dereverbAutoAboveRt60;
        decide(sp, on,
               p.dereverbAutoAboveRt60 > 0 ? fmt("RT60 %.2f s vs auto threshold %.2f s.", a.rt60, p.dereverbAutoAboveRt60)
                                           : "Optional processor (off by default for this preset).",
               uc);
    }

    // ------------------------------------------------------------ Plosive
    {
        auto& sp = plan.stage(StageId::Plosive);
        sp.available = voice;
        S.plosive.maxReductionDb = p.maxPlosiveDb * clampv(cl, 0.3, 1.0);
        S.plosive.detectHz = a.speech.f0LowHz > 0 ? clampv(0.8 * a.speech.f0LowHz, 60.0, 150.0) : 100.0;
        S.plosive.applyHz = clampv(S.plosive.detectHz * 1.5, 100.0, 200.0);
        S.plosive.sensitivityDb = 10.0 - 2.0 * (cl - 1.0);
        param(sp, "Detection band", fmt("< %.0f Hz (below the voice F0)", S.plosive.detectHz));
        param(sp, "Attenuated band", fmt("< %.0f Hz", S.plosive.applyHz));
        param(sp, "Trigger", fmt("sub-F0 / voice-band excess > %.1f dB over running baseline", S.plosive.sensitivityDb));
        param(sp, "Max reduction", fmt("%.1f dB", S.plosive.maxReductionDb));
        if (!voice) decide(sp, false, "Voice only.", uc);
        else
            decide(sp, a.speech.plosiveCandidates > 0, fmt("%d plosive bursts detected.", a.speech.plosiveCandidates), uc);
    }

    // -------------------------------------------------------------- DeEss
    {
        auto& sp = plan.stage(StageId::DeEss);
        sp.available = voice;
        auto& d = S.deess;
        const double target = p.sibilanceTargetDb - 2.0 * (cl - 1.0);
        d.thresholdDb = target;
        d.detectLowHz = std::min(4000.0, 0.4 * sr);
        d.freqHz = a.speech.sibilanceFreqHz;
        d.useBell = p.id == "voice.natural";
        d.maxReductionDb = clampv((a.speech.sibilanceP98Db - target + 2.0) * clampv(cl, 0.3, 1.5), 2.0, p.maxDeEssDb);
        d.absFloorDb = a.activeLevelDb - 35.0;
        param(sp, "Mode", d.useBell ? fmt("dynamic bell at %.0f Hz", d.freqHz) : fmt("split-band shelf above %.0f Hz", d.detectLowHz));
        param(sp, "Threshold", fmt("sibilance/voice ratio %.1f dB", d.thresholdDb));
        param(sp, "Max reduction", fmt("%.1f dB", d.maxReductionDb));
        param(sp, "Lookahead", fmt("%.1f ms", d.lookaheadMs));
        if (!voice) decide(sp, false, "Voice only.", uc);
        else
        {
            const bool on = a.speech.sibilanceP98Db > target;
            decide(sp, on,
                   fmt("Sibilance ratio P98 %.1f dB (median %.1f) vs target %.1f dB; centre %.0f Hz.", a.speech.sibilanceP98Db,
                       a.speech.sibilanceMedianDb, target, a.speech.sibilanceFreqHz),
                   uc);
        }
    }

    // ---------------------------------------------------------------- EQ
    {
        auto& sp = plan.stage(StageId::Eq);
        std::vector<std::string> why;
        std::vector<EqBand> bands;
        const auto& si = a.spectrum;
        // Voice identity: the fundamental region (up to ~1.3 x median F0) is never
        // re-shaped, and resonance search starts above the second harmonic.
        const double identityHz = voice ? std::max(150.0, 1.6 * a.speech.f0MedianHz) : 0.0;
        const double resonanceMinHz = voice ? std::max(300.0, 2.2 * a.speech.f0MedianHz) : 120.0;
        if (uc.reference && !si.bandHz.empty() && tn > 0)
        {
            // Reference matching: the approved track defines the target.
            double rmsBefore = 0;
            auto dev = referenceDeviation(si, uc.reference->spectrum, &rmsBefore);
            double peakBand = -300;
            for (double v : si.bandAbsDb) peakBand = std::max(peakBand, v);
            const double strength = clampv(uc.referenceAmount, 0.0, 1.0) * std::min(tn, 1.5);
            int protectedBands = 0;
            for (size_t i = 0; i < dev.size(); ++i)
            {
                const bool nearNoise = si.noiseBandDb[i] > -199 && si.bandAbsDb[i] - si.noiseBandDb[i] < 12.0;
                const bool absent = si.bandAbsDb[i] < peakBand - 60.0;
                if ((nearNoise || absent) && dev[i] > 0) { dev[i] = 0; ++protectedBands; }
                if (voice && si.bandHz[i] < identityHz) dev[i] = 0.0;
                dev[i] *= strength;
            }
            const double maxB = std::min(6.0, std::max(p.maxEqBoostDb, 3.0) * 1.5);
            const double maxC = std::min(6.0, std::max(p.maxEqCutDb, 3.0) * 1.5);
            bands = fitEq(si.bandHz, dev, maxB, maxC, sr, cat, voice ? identityHz : 60.0, 99.0, 99.0, 5);
            why.push_back(fmt("matching reference '%s': spectral deviation %.2f dB rms, %.0f%% of it corrected", uc.reference->name.c_str(),
                              rmsBefore, strength * 100));
            if (voice) why.push_back(fmt("fundamental region below %.0f Hz left untouched (vocal identity)", identityHz));
            if (protectedBands) why.push_back(fmt("%d bands not boosted (near noise floor or no content)", protectedBands));
            param(sp, "Target", "reference track (bounds +" + fmt("%.1f/-%.1f dB", maxB, maxC) + ")");
        }
        else if (p.useTargetCurve && !si.bandHz.empty() && p.toneStrength * tn > 0)
        {
            std::vector<double> dev(si.bandHz.size(), 0.0);
            double peakBand = -300;
            for (double v : si.bandAbsDb) peakBand = std::max(peakBand, v);
            for (size_t i = 0; i < si.bandHz.size(); ++i)
            {
                double t = categoryTargetDb(cat, si.bandHz[i]);
                for (auto& o : p.targetOffsets)
                    if (std::abs(std::log2(si.bandHz[i] / o.first)) < 0.5) t += o.second * (1.0 - 2.0 * std::abs(std::log2(si.bandHz[i] / o.first)));
                dev[i] = t - si.bandDb[i];
            }
            // Level-neutral: remove mean deviation 200 Hz..4 kHz.
            double m = 0;
            int c = 0;
            for (size_t i = 0; i < dev.size(); ++i)
                if (si.bandHz[i] >= 200 && si.bandHz[i] <= 4000) { m += dev[i]; ++c; }
            m = c ? m / c : 0;
            for (auto& d : dev) d -= m;
            // Smooth over neighbouring bands, then protect noise and absent content.
            std::vector<double> sm(dev.size());
            for (size_t i = 0; i < dev.size(); ++i)
            {
                const double l = i ? dev[i - 1] : dev[i], r = i + 1 < dev.size() ? dev[i + 1] : dev[i];
                sm[i] = 0.25 * l + 0.5 * dev[i] + 0.25 * r;
            }
            int protectedBands = 0;
            for (size_t i = 0; i < sm.size(); ++i)
            {
                const bool nearNoise = si.noiseBandDb[i] > -199 && si.bandAbsDb[i] - si.noiseBandDb[i] < 12.0;
                const bool absent = si.bandAbsDb[i] < peakBand - 60.0;
                if ((nearNoise || absent) && sm[i] > 0) { sm[i] = 0; ++protectedBands; }
                if (voice && si.bandHz[i] < identityHz) sm[i] = 0.0;
                sm[i] *= p.toneStrength * tn;
            }
            bands = fitEq(si.bandHz, sm, p.maxEqBoostDb, p.maxEqCutDb, sr, cat, voice ? identityHz : 100.0, p.maxHighBoostDb, p.maxLowCutDb);
            if (voice) why.push_back(fmt("fundamental region below %.0f Hz left untouched (vocal identity)", identityHz));
            why.push_back(fmt("%.0f%% of the deviation from the %s target corrected (tilt %.1f dB/oct measured)",
                              p.toneStrength * tn * 100, categoryName(cat), si.tiltDbPerOct));
            if (protectedBands) why.push_back(fmt("%d bands not boosted (near noise floor or no content)", protectedBands));
        }
        else if (!p.targetOffsets.empty() && tn > 0)
        {
            for (auto& o : p.targetOffsets)
            {
                SvfType t = o.first <= 100 ? SvfType::LowShelf : o.first >= 6000 ? SvfType::HighShelf : SvfType::Bell;
                bands.push_back({ t, o.first, t == SvfType::Bell ? 1.0 : 0.7, clampv(o.second * tn, -p.maxEqCutDb, p.maxEqBoostDb), true });
            }
            why.push_back("preset tonal intent applied (sound effects have no universal target curve)");
        }
        if (p.maxResonanceCuts > 0 && tn > 0)
        {
            auto rc = resonanceCuts(si, p.maxResonanceCuts, p.maxEqCutDb, clampv(tn, 0.0, 1.5), resonanceMinHz, why);
            bands.insert(bands.end(), rc.begin(), rc.end());
        }
        if (std::abs(uc.tiltDb) > 0.05)
        {
            bands.push_back({ SvfType::LowShelf, 250.0, 0.5, -uc.tiltDb / 2, true });
            bands.push_back({ SvfType::HighShelf, 4000.0, 0.5, uc.tiltDb / 2, true });
            why.push_back(fmt("user tilt %+.1f dB", uc.tiltDb));
        }
        S.eq = bands;
        param(sp, "Bands", bandsText(bands));
        param(sp, "Bounds", fmt("+%.1f / -%.1f dB", p.maxEqBoostDb, p.maxEqCutDb));
        if (p.maxHighBoostDb < 50 || p.maxLowCutDb < 50)
            param(sp, "Intent guards", fmt("boost >1.5 kHz <= %.1f dB, cut <300 Hz <= %.1f dB", std::min(p.maxHighBoostDb, p.maxEqBoostDb),
                                           std::min(p.maxLowCutDb, p.maxEqCutDb)));
        std::string w;
        for (auto& s : why) w += s + "; ";
        bool any = false;
        for (auto& b : bands) any |= std::abs(b.gainDb) >= 0.3;
        decide(sp, any, any ? w : "Spectral balance already within tolerance of the target; " + w, uc);
    }

    // ------------------------------------------------------------- DynEq
    {
        auto& sp = plan.stage(StageId::DynEq);
        S.dyneq.clear();
        std::string why;
        const double gate = a.activeLevelDb - 40.0;
        if (p.harshnessRangeDb > 0 && tn > 0)
        {
            double f = 3500, best = -1e9;
            const auto& fh = a.spectrum.fineHz;
            const auto& fd = a.spectrum.fineDb;
            for (size_t i = 12; i + 12 < fh.size(); ++i)
                if (fh[i] >= 2000 && fh[i] <= 5000)
                {
                    double tr = 0;
                    for (size_t j = i - 12; j <= i + 12; ++j) tr += fd[j];
                    if (fd[i] - tr / 25 > best) { best = fd[i] - tr / 25; f = fh[i]; }
                }
            const auto pc = bandLevelPercentiles(in, SvfType::Bandpass, f, 1.5, gate);
            DynamicEqBand b;
            b.type = SvfType::Bell;
            b.freq = f;
            b.q = 1.5;
            b.thresholdDb = pc[1] + eqResponseAt(S.eq, f, sr);
            b.ratio = 3.0;
            b.rangeDb = p.harshnessRangeDb * std::min(tn, 1.5);
            b.attackMs = 3;
            b.releaseMs = 60;
            S.dyneq.push_back(b);
            why += fmt("Harshness band %.0f Hz: threshold at its P75 level %.1f dB, range %.1f dB. ", f, b.thresholdDb, b.rangeDb);
        }
        if (p.boomRangeDb > 0 && tn > 0)
        {
            const double f = voice ? 150.0 : 100.0;
            const auto pc = bandLevelPercentiles(in, SvfType::Lowpass, f, 0.707, gate);
            DynamicEqBand b;
            b.type = SvfType::LowShelf;
            b.freq = f;
            b.q = 0.707;
            b.thresholdDb = pc[1] + eqResponseAt(S.eq, f * 0.6, sr);
            b.ratio = 2.5;
            b.rangeDb = p.boomRangeDb * std::min(tn, 1.5);
            b.attackMs = 10;
            b.releaseMs = 120;
            S.dyneq.push_back(b);
            why += fmt("Low-end/proximity control below %.0f Hz above P75 (%.1f dB), range %.1f dB. ", f, b.thresholdDb, b.rangeDb);
        }
        for (auto& b : S.dyneq)
            param(sp, fmt("%s %.0f Hz", b.type == SvfType::Bell ? "Bell" : "Low shelf", b.freq),
                  fmt("thr %.1f dB, ratio %.1f, range %.1f dB, %g/%g ms", b.thresholdDb, b.ratio, b.rangeDb, b.attackMs, b.releaseMs));
        decide(sp, !S.dyneq.empty(), S.dyneq.empty() ? "Preset uses no dynamic EQ." : why, uc);
    }

    // -------------------------------------------------------- Saturation
    {
        auto& sp = plan.stage(StageId::Saturation);
        auto& s = S.saturation;
        double drive = p.saturationDriveDb * tn;
        std::string why = fmt("Preset character drive %.1f dB x tone %.0f%%", p.saturationDriveDb, tn * 100);
        if (drive > 0 && a.crestDb < 12.0)
        {
            drive *= 0.6;
            why += fmt("; crest factor only %.1f dB -> drive reduced to keep transients", a.crestDb);
        }
        s.driveDb = drive;
        s.asymmetry = p.saturationAsymmetry;
        s.mix = p.saturationMix;
        s.oversample = 4;
        param(sp, "Drive", fmt("%.1f dB", s.driveDb));
        param(sp, "Asymmetry (even harmonics)", fmt("%.2f", s.asymmetry));
        param(sp, "Mix", fmt("%.0f%%", s.mix * 100));
        param(sp, "Oversampling", "4x linear-phase FIR (Kaiser, 64 taps/phase)");
        decide(sp, drive >= 0.5, drive >= 0.5 ? why + "." : "No saturation in this preset.", uc);
    }

    // ------------------------------------------------------------- Stereo
    {
        auto& sp = plan.stage(StageId::Stereo);
        sp.available = in.numChannels() == 2;
        auto& s = S.stereo;
        bool on = false;
        std::string why = fmt("Image preserved (correlation %.2f, <150 Hz %.2f).", a.stereo.correlation, a.stereo.lowCorrelation);
        if (sp.available && p.bassMonoOnPhaseIssues && !a.stereo.dualMono && a.stereo.lowCorrelation < 0.2)
        {
            s.monoBelowHz = 100.0;
            on = true;
            why = fmt("Low-frequency correlation %.2f (phase-unstable bass) -> side removed below 100 Hz only.", a.stereo.lowCorrelation);
        }
        if (sp.available && uc.reference && uc.reference->stereo && !a.stereo.dualMono && !voice)
        {
            const double diff = uc.reference->sideToMidDb - a.stereo.sideToMidDb;
            if (std::abs(diff) > 1.0)
            {
                s.width = clampv(dbToGain(diff * clampv(uc.referenceAmount, 0.0, 1.0)), 0.75, 1.35);
                on = true;
                why = fmt("Reference side/mid %.1f dB vs source %.1f dB -> width %.2f (bounded 0.75-1.35).", uc.reference->sideToMidDb,
                          a.stereo.sideToMidDb, s.width);
            }
        }
        if (sp.available && voice && std::abs(a.stereo.balanceDb) > 2.0 && a.stereo.correlation > 0.7)
        {
            s.balanceDb = -a.stereo.balanceDb;
            on = true;
            why = fmt("Voice off-centre by %.1f dB -> balance corrected.", a.stereo.balanceDb);
        }
        param(sp, "Width", fmt("%.2f", s.width));
        param(sp, "Mono below", s.monoBelowHz > 0 ? fmt("%.0f Hz (LR4)", s.monoBelowHz) : "off");
        param(sp, "Balance", fmt("%+.1f dB", s.balanceDb));
        decide(sp, on, sp.available ? why : "Mono or multichannel source.", uc);
    }
}

void planDynamics(Plan& plan, const AnalysisReport& a, const PresetDef& p, const UserControls& uc, const AudioBuffer& x1)
{
    auto& S = plan.settings;
    const double dy = clampv(uc.dynamics, 0.0, 2.0);
    const bool voice = p.category == Category::Voice;
    const auto ls1 = measureLoudness(x1, false, true);
    const auto noise1 = NoiseProfile::estimate(x1, StftProcessor::defaultFftSize(x1.sampleRate));
    double active1 = kSilenceDb;
    {
        const auto st = rmsLevelStats(x1, 50.0, -90.0);
        active1 = st.p75;
    }
    plan.log.push_back(fmt("After cleanup+tone: %.1f LUFS, LRA %.1f LU, noise floor %.1f dBFS.", ls1.integrated, ls1.lra, noise1.levelDb));

    // ------------------------------------------------------------ Leveler
    {
        auto& sp = plan.stage(StageId::Leveler);
        auto& L = S.leveler;
        const auto lc = loudnessCurve(x1, 0.1);
        std::vector<double> act;
        for (size_t i = 0; i < lc.momentary.size(); ++i)
        {
            const size_t vi = i * 10;
            const bool vadOk = a.speech.vad.empty() || (vi < a.speech.vad.size() && a.speech.vad[vi] > 0.5f);
            if (vadOk && lc.momentary[i] > -60.0 && lc.momentary[i] > ls1.integrated - 20.0) act.push_back(lc.momentary[i]);
        }
        const double spread = act.size() > 10 ? percentile(act, 90) - percentile(act, 10) : 0.0;
        L.targetDb = act.empty() ? ls1.integrated : median(act);
        L.maxBoostDb = L.maxCutDb = p.levelerRangeDb * std::min(dy, 1.0);
        L.strength = clampv(p.levelerStrength * dy, 0.0, 1.0);
        L.gateDb = std::max(noise1.levelDb + 12.0, L.targetDb - 25.0);
        L.windowMs = 400;
        L.smoothingMs = voice ? 500 : 1500;
        param(sp, "Target", fmt("%.1f LUFS (median active short-term level)", L.targetDb));
        param(sp, "Range", fmt("+/-%.1f dB", L.maxBoostDb));
        param(sp, "Strength", fmt("%.0f%%", L.strength * 100));
        param(sp, "Pause gate", fmt("%.1f LUFS (gain frozen below)", L.gateDb));
        param(sp, "Smoothing", fmt("%.0f ms zero-phase", L.smoothingMs));
        bool on = p.levelerRangeDb > 0 && L.maxBoostDb > 0;
        std::string why;
        if (p.levelerRangeDb <= 0) why = "Preset does not ride level (musical/natural dynamics preserved).";
        else if (a.durationS < 6.0) { on = false; why = "Clip shorter than 6 s: no level riding."; }
        else if (spread < 3.0) { on = false; why = fmt("Active level spread %.1f dB (P10-P90) already consistent.", spread); }
        else why = fmt("Active level spread %.1f dB (P10-P90) -> ride toward median within +/-%.1f dB.", spread, L.maxBoostDb);
        decide(sp, on, why, uc);
    }

    // ----------------------------------------------------------- Expander
    {
        auto& sp = plan.stage(StageId::Expander);
        auto& e = S.expander;
        e.thresholdDb = noise1.levelDb + 15.0;
        e.rangeDb = p.maxNoiseGateDb * std::min(dy, 1.0);
        e.ratio = 2.0;
        param(sp, "Threshold", fmt("%.1f dBFS peak (noise %.1f + 15)", e.thresholdDb, noise1.levelDb));
        param(sp, "Range", fmt("%.1f dB (bounded, never a hard gate)", e.rangeDb));
        param(sp, "Attack/Hold/Release", fmt("%g / %g / %g ms", e.attackMs, e.holdMs, e.releaseMs));
        bool on = e.rangeDb > 0;
        std::string why;
        if (p.maxNoiseGateDb <= 0) why = "Preset keeps the floor continuous (no downward expansion).";
        else if (!noise1.valid || noise1.levelDb < -85.0) { on = false; why = fmt("Floor already %.1f dBFS.", noise1.levelDb); }
        else if (active1 - noise1.levelDb < 20.0)
        {
            on = false;
            why = fmt("Only %.0f dB between programme and floor: expansion would chatter.", active1 - noise1.levelDb);
        }
        else why = fmt("Floor %.1f dBFS between events -> lowered by up to %.0f dB.", noise1.levelDb, e.rangeDb);
        decide(sp, on, why, uc);
    }

    // ---------------------------------------------------------- Transient
    {
        auto& sp = plan.stage(StageId::Transient);
        auto& t = S.transient;
        t.attackDb = p.transientAttackDb * dy;
        t.sustainDb = p.transientSustainDb * dy;
        std::string why = fmt("Preset attack emphasis %.1f dB x dynamics %.0f%%.", p.transientAttackDb, dy * 100);
        if (t.attackDb > 0 && a.transients.peakToLoudnessDb > 18.0)
        {
            t.attackDb *= 0.5;
            why += fmt(" Source already very transient (peak-to-loudness %.1f dB): halved.", a.transients.peakToLoudnessDb);
        }
        param(sp, "Attack", fmt("%+.1f dB", t.attackDb));
        param(sp, "Sustain", fmt("%+.1f dB", t.sustainDb));
        const bool on = std::abs(t.attackDb) >= 0.5 || std::abs(t.sustainDb) >= 0.5;
        decide(sp, on, on ? why : "No transient shaping in this preset.", uc);
    }

    // --------------------------------------------------------- Compressor
    {
        auto& sp = plan.stage(StageId::Compressor);
        auto& c = S.compressor;
        c = p.compStyle;
        double target = p.compTargetGrDb * dy;
        std::string why;
        const double plr1 = ls1.integratedValid() ? measureLoudness(x1, true, false).truePeakDb - ls1.integrated : 20.0;
        if (target > 0 && plr1 < 9.0)
        {
            target *= 0.5;
            why = fmt("Material already dense (PLR %.1f dB): target GR halved. ", plr1);
        }
        double p95 = 0;
        c.thresholdDb = compressorThreshold(x1, c, target, active1 - 30.0, &p95);
        c.maxGrDb = std::max(6.0, target * 2.5);
        c.makeupDb = 0.0;
        param(sp, "Threshold", fmt("%.1f dB", c.thresholdDb));
        param(sp, "Ratio", fmt("%.1f:1", c.ratio));
        param(sp, "Knee", fmt("%.0f dB soft", c.kneeDb));
        param(sp, "Attack/Release", fmt("%.0f / %.0f ms", c.attackMs, c.releaseMs));
        param(sp, "Detector", c.detector == DetectorMode::Rms ? fmt("RMS %.0f ms", c.rmsWindowMs) : "peak");
        param(sp, "Stereo link", fmt("%.0f%%", c.stereoLink * 100));
        param(sp, "Sidechain HPF", c.sidechainHpfHz > 0 ? fmt("%.0f Hz", c.sidechainHpfHz) : "off");
        param(sp, "Mix", fmt("%.0f%%", c.mix * 100));
        param(sp, "GR bound", fmt("%.1f dB", c.maxGrDb));
        const bool on = target > 0.05;
        why += on ? fmt("Detector P95 %.1f dB -> threshold for ~%.1f dB GR on loud passages.", p95, target)
                  : "Preset leaves dynamics uncompressed.";
        decide(sp, on, why, uc);
    }

    // ---------------------------------------------------------- Multiband
    {
        auto& sp = plan.stage(StageId::Multiband);
        auto& m = S.multiband;
        m.xoverLowHz = p.mbXoverLowHz;
        m.xoverHighHz = p.mbXoverHighHz;
        // Split x1 into bands to measure each band's detector distribution.
        AudioBuffer bands[3];
        for (auto& b : bands) b = AudioBuffer(x1.numChannels(), x1.numFrames(), x1.sampleRate);
        for (int c = 0; c < x1.numChannels(); ++c)
        {
            LR4Crossover x1c, x2c;
            x1c.set(m.xoverLowHz, x1.sampleRate);
            x2c.set(m.xoverHighHz, x1.sampleRate);
            for (size_t i = 0; i < x1.numFrames(); ++i)
            {
                double lo, rest, mid, hi;
                x1c.process(x1.channel(c)[i], lo, rest);
                x2c.process(rest, mid, hi);
                bands[0].channel(c)[i] = float(lo);
                bands[1].channel(c)[i] = float(mid);
                bands[2].channel(c)[i] = float(hi);
            }
        }
        const double atk[3] = { 30, 15, 8 }, rel[3] = { 200, 120, 80 };
        std::string thr;
        for (int b = 0; b < 3; ++b)
        {
            auto& cs = m.band[b];
            cs.ratio = 2.5;
            cs.kneeDb = 6;
            cs.attackMs = atk[b];
            cs.releaseMs = rel[b];
            cs.detector = DetectorMode::Rms;
            cs.rmsWindowMs = 10;
            cs.stereoLink = 1.0;
            const auto st = rmsLevelStats(bands[b], 10.0, active1 - 40.0);
            const double t = p.mbTargetGrDb[b] * dy;
            cs.thresholdDb = thresholdFor(st.p95, std::max(0.1, t), cs);
            cs.maxGrDb = std::max(4.0, t * 2.5);
            thr += fmt("%s thr %.1f (P95 %.1f, ~%.1f dB GR); ", b == 0 ? "Low" : b == 1 ? "Mid" : "High", cs.thresholdDb, st.p95, t);
        }
        param(sp, "Crossovers", fmt("%.0f / %.0f Hz, Linkwitz-Riley 24 dB/oct, allpass-compensated", m.xoverLowHz, m.xoverHighHz));
        param(sp, "Bands", thr);
        param(sp, "Ratio", "2.5:1, 6 dB knee; attack 30/15/8 ms; release 200/120/80 ms");
        decide(sp, p.multibandDefault && dy > 0,
               p.multibandDefault ? "Preset uses multiband density; thresholds from per-band detector statistics."
                                  : "Optional (off for this preset).",
               uc);
    }
}

} // namespace ac
