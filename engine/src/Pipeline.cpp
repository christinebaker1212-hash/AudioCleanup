#include "ac/Pipeline.h"

#include "Planning.h"
#include "ac/Resampler.h"

#include <chrono>
#include <sstream>

namespace ac {

const char* sectionName(Section s)
{
    switch (s)
    {
        case Section::Cleanup: return "Cleanup";
        case Section::Tone: return "Tone";
        case Section::Dynamics: return "Dynamics";
        case Section::Output: return "Output";
    }
    return "?";
}

namespace {
struct StageInfo
{
    const char* name;
    const char* key;
    Section section;
};
const StageInfo kStages[] = {
    { "DC / High-pass", "filter", Section::Cleanup },
    { "De-clip", "declip", Section::Cleanup },
    { "De-click", "declick", Section::Cleanup },
    { "De-hum", "dehum", Section::Cleanup },
    { "Spectral denoise", "denoise", Section::Cleanup },
    { "Neural speech enhance", "neural", Section::Cleanup },
    { "De-reverb", "dereverb", Section::Cleanup },
    { "Plosive control", "plosive", Section::Cleanup },
    { "De-ess", "deess", Section::Cleanup },
    { "Corrective EQ", "eq", Section::Tone },
    { "Dynamic EQ", "dyneq", Section::Tone },
    { "Level rider", "leveler", Section::Dynamics },
    { "Expander", "expander", Section::Dynamics },
    { "Transient shaper", "transient", Section::Dynamics },
    { "Compressor", "compressor", Section::Dynamics },
    { "Multiband", "multiband", Section::Dynamics },
    { "Saturation", "saturation", Section::Tone },
    { "Stereo", "stereo", Section::Tone },
    { "Loudness + TP limiter", "limiter", Section::Output },
};
static_assert(sizeof(kStages) / sizeof(kStages[0]) == size_t(StageId::Count));
} // namespace

const char* stageName(StageId id) { return kStages[size_t(id)].name; }
const char* stageKey(StageId id) { return kStages[size_t(id)].key; }
Section stageSection(StageId id) { return kStages[size_t(id)].section; }
std::optional<StageId> stageFromKey(const std::string& key)
{
    for (int i = 0; i < int(StageId::Count); ++i)
        if (key == kStages[i].key) return StageId(i);
    return std::nullopt;
}

double loudnessMetric(const AudioBuffer& b, LoudnessMode m, LoudnessStats* out)
{
    const auto st = measureLoudness(b, true, false);
    if (out) *out = st;
    switch (m)
    {
        case LoudnessMode::Unchanged:
        case LoudnessMode::Peak: return st.truePeakDb > -140 ? st.truePeakDb : -std::numeric_limits<double>::infinity();
        case LoudnessMode::Integrated: return st.integrated;
        case LoudnessMode::MaxMomentary: return st.maxMomentary;
        case LoudnessMode::MaxShortTerm: return st.maxShortTerm;
    }
    return st.integrated;
}

std::vector<double> batchConsistencyTargets(const std::vector<double>& metrics, double groupTarget, double preserve)
{
    std::vector<double> finite;
    for (double v : metrics)
        if (std::isfinite(v)) finite.push_back(v);
    std::vector<double> out(metrics.size(), groupTarget);
    if (finite.empty()) return out;
    const double med = median(finite);
    preserve = clampv(preserve, 0.0, 1.0);
    for (size_t i = 0; i < metrics.size(); ++i)
        if (std::isfinite(metrics[i])) out[i] = groupTarget + preserve * (metrics[i] - med);
    return out;
}

namespace {

std::unique_ptr<Processor> makeProcessor(StageId id, const ChainSettings& s)
{
    switch (id)
    {
        case StageId::Filter: return std::make_unique<FilterStage>(s.filter);
        case StageId::Declip: return std::make_unique<Declipper>(s.declip);
        case StageId::Declick: return std::make_unique<ClickRemover>(s.declick);
        case StageId::Dehum: return std::make_unique<HumRemover>(s.dehum);
        case StageId::Denoise: return std::make_unique<SpectralDenoiser>(s.denoise);
        case StageId::Neural: return std::make_unique<NeuralSpeechDenoiser>(s.neural);
        case StageId::Dereverb: return std::make_unique<Dereverberator>(s.dereverb);
        case StageId::Plosive: return std::make_unique<PlosiveReducer>(s.plosive);
        case StageId::DeEss: return std::make_unique<DeEsser>(s.deess);
        case StageId::Eq: return std::make_unique<ParametricEq>(s.eq);
        case StageId::DynEq: return std::make_unique<DynamicEq>(s.dyneq);
        case StageId::Leveler: return std::make_unique<Leveler>(s.leveler);
        case StageId::Expander: return std::make_unique<Expander>(s.expander);
        case StageId::Transient: return std::make_unique<TransientShaper>(s.transient);
        case StageId::Compressor: return std::make_unique<Compressor>(s.compressor);
        case StageId::Multiband: return std::make_unique<MultibandCompressor>(s.multiband);
        case StageId::Saturation: return std::make_unique<Saturator>(s.saturation);
        case StageId::Stereo: return std::make_unique<StereoProcessor>(s.stereo);
        case StageId::Limiter: return std::make_unique<TruePeakLimiter>(s.limiter);
        case StageId::Count: break;
    }
    return nullptr;
}

/** Render one stage; returns metrics (GR trace aligned to output time). */
AudioBuffer runStage(StageId id, const ChainSettings& s, const AudioBuffer& in, StageMetrics& m, Processor** keep,
                     std::unique_ptr<Processor>& holder, const Job& job)
{
    const auto t0 = std::chrono::steady_clock::now();
    holder = makeProcessor(id, s);
    RenderOptions ro;
    AudioBuffer out = renderProcessor(*holder, in, ro, job);
    m.id = id;
    m.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (const GrTrace* tr = holder->grTrace())
    {
        const int hop = tr->hop();
        const size_t skip = holder->isOffline() ? 0 : size_t(std::lround(double(holder->latency()) / hop));
        const auto& v = tr->values();
        m.grTrace.assign(v.begin() + long(std::min(skip, v.size())), v.end());
        for (float g : m.grTrace) m.maxGrDb = std::max(m.maxGrDb, double(g));
    }
    if (keep) *keep = holder.get();
    return out;
}

/** Loss (dB) of energy in the loudest frames between two aligned buffers. */
double activeLossDb(const AudioBuffer& a, const AudioBuffer& b, double& legitDb, double snrDb)
{
    const size_t hop = std::max<size_t>(1, size_t(a.sampleRate * 0.05));
    const size_t n = std::min(a.numFrames(), b.numFrames());
    std::vector<std::pair<double, double>> fr;
    for (size_t s = 0; s + hop <= n; s += hop)
    {
        double ea = 0, eb = 0;
        for (int c = 0; c < a.numChannels(); ++c)
            for (size_t i = s; i < s + hop; ++i)
            {
                ea += double(a.channel(c)[i]) * a.channel(c)[i];
                eb += double(b.channel(c)[i]) * b.channel(c)[i];
            }
        fr.emplace_back(ea, eb);
    }
    if (fr.empty()) return 0.0;
    std::sort(fr.begin(), fr.end(), [](auto& x, auto& y) { return x.first > y.first; });
    const size_t take = std::max<size_t>(1, fr.size() / 5); // loudest 20 %
    double sa = 0, sb = 0;
    for (size_t i = 0; i < take; ++i) { sa += fr[i].first; sb += fr[i].second; }
    legitDb = 10.0 * std::log10(1.0 + std::pow(10.0, -snrDb / 10.0));
    return powerToDb(sa, -300) - powerToDb(sb, -300);
}

void padTo(AudioBuffer& b, size_t n)
{
    if (b.numFrames() < n) b.resizeFrames(n);
}

double grP99(const std::vector<float>& tr)
{
    std::vector<double> v;
    for (float g : tr)
        if (g > 0.01f) v.push_back(g);
    if (v.empty()) return 0.0;
    // P99 over all hops, counting zero-GR hops too.
    const size_t zeros = tr.size() - v.size();
    std::vector<double> all(v);
    all.insert(all.end(), zeros, 0.0);
    return percentile(all, 99.0);
}

} // namespace

ProcessResult process(const ProcessRequest& req, const Job& job)
{
    if (!req.input || !req.preset) throw std::invalid_argument("process: missing input or preset");
    const AudioBuffer& input = *req.input;
    const PresetDef& P = *req.preset;
    const UserControls& uc = req.controls;
    ProcessResult R;

    Job sub = job;
    auto phase = [&](double a, double b, const std::string& what) {
        sub.progress = [&job, a, b, what](double f, const std::string& w) { job.report(a + (b - a) * f, w.empty() ? what : w); };
    };

    phase(0.0, 0.2, "Analysing");
    R.analysis = req.analysis ? *req.analysis : analyze(input, P.category, sub);
    const AnalysisReport& A = R.analysis;

    Plan& plan = R.plan;
    plan.preset = &P;
    initPlan(plan);
    plan.loudnessMode = uc.loudnessMode.value_or(P.loudnessMode);
    plan.targetLufs = uc.targetLufs.value_or(P.targetLufs);
    plan.ceilingDbTP = uc.ceilingDbTP.value_or(P.ceilingDbTP);
    plan.outputSampleRate = uc.outputSampleRate > 0 ? uc.outputSampleRate : input.sampleRate;
    plan.log.push_back("Preset " + P.name + " (" + categoryName(P.category) + "). Source analysis:\n" + describe(A));

    if (A.silent || input.empty())
    {
        for (auto& s : plan.stages) s.reason = "Silent input: passed through unchanged.";
        R.output = resample(input, plan.outputSampleRate, job);
        R.reference = R.output;
        R.removed = AudioBuffer(R.output.numChannels(), R.output.numFrames(), R.output.sampleRate);
        R.outputStats = R.referenceStats = measureLoudness(R.output);
        plan.log.push_back("Silent input: no processing applied.");
        job.report(1.0, "Done");
        return R;
    }

    planCleanupTone(plan, A, P, uc, input);
    throwIfCancelled(job);

    auto& S = plan.settings;
    AudioBuffer x = input;
    std::unique_ptr<Processor> holder;
    auto runIfEnabled = [&](StageId id, double p0, double p1) {
        if (!plan.stage(id).enabled) return;
        phase(p0, p1, stageName(id));
        StageMetrics m;
        x = runStage(id, S, x, m, nullptr, holder, sub);
        R.metrics.push_back(std::move(m));
        throwIfCancelled(job);
    };

    // ------------------------------------------------------------ Cleanup
    const StageId cleanup[] = { StageId::Filter, StageId::Declip, StageId::Declick, StageId::Dehum, StageId::Denoise,
                                StageId::Neural, StageId::Dereverb, StageId::Plosive, StageId::DeEss };
    double prog = 0.2;
    for (StageId id : cleanup)
    {
        if (id == StageId::Denoise && plan.stage(id).enabled)
        {
            // Render, measure artifact bound, correct (at most twice).
            phase(prog, prog + 0.06, stageName(id));
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                StageMetrics m;
                AudioBuffer y = runStage(id, S, x, m, nullptr, holder, sub);
                double legit = 0;
                const double loss = activeLossDb(x, y, legit, A.snrDb);
                const double bound = P.maxActiveLossDb + legit;
                auto* dn = static_cast<SpectralDenoiser*>(holder.get());
                plan.log.push_back(fmt("Denoise pass %d: frame attenuation median %.1f dB / P95 %.1f dB, loud-frame loss %.2f dB (bound %.2f).",
                                       attempt + 1, dn->medianFrameAttenuationDb(), dn->p95FrameAttenuationDb(), loss, bound));
                if (loss <= bound || attempt == 2)
                {
                    if (loss > bound) plan.log.push_back("Denoise: artifact bound still exceeded after 2 corrections; kept gentlest setting.");
                    x = std::move(y);
                    R.metrics.push_back(std::move(m));
                    break;
                }
                S.denoise.oversubtraction = std::max(0.8, S.denoise.oversubtraction * 0.75);
                S.denoise.reductionDb *= 0.7;
                auto& sp = plan.stage(id);
                sp.reason += fmt(" Correction: loud-frame loss %.2f dB > %.2f dB -> over-subtraction %.2f, max attenuation %.1f dB.",
                                 loss, bound, S.denoise.oversubtraction, S.denoise.reductionDb);
                plan.log.push_back("Denoise corrected: " + sp.reason);
            }
            throwIfCancelled(job);
        }
        else runIfEnabled(id, prog, prog + 0.06);
        prog += 0.3 / 9.0;
    }
    // What cleanup removed (latency-aligned, so a plain difference).
    AudioBuffer removed(input.numChannels(), x.numFrames(), input.sampleRate);
    for (int c = 0; c < input.numChannels(); ++c)
        for (size_t i = 0; i < x.numFrames(); ++i)
            removed.channel(c)[i] = (i < input.numFrames() ? input.channel(c)[i] : 0.0f) - x.channel(c)[i];

    // --------------------------------------------------------------- Tone
    runIfEnabled(StageId::Eq, 0.5, 0.55);
    runIfEnabled(StageId::DynEq, 0.55, 0.6);

    // ----------------------------------------------------------- Dynamics
    phase(0.6, 0.62, "Measuring for dynamics");
    planDynamics(plan, A, P, uc, x);
    runIfEnabled(StageId::Leveler, 0.62, 0.66);
    runIfEnabled(StageId::Expander, 0.66, 0.68);
    runIfEnabled(StageId::Transient, 0.68, 0.7);
    const AudioBuffer preComp = x;
    const size_t metricsPreComp = R.metrics.size();
    auto runBus = [&] {
        runIfEnabled(StageId::Compressor, 0.7, 0.72);
        runIfEnabled(StageId::Multiband, 0.72, 0.76);
        runIfEnabled(StageId::Saturation, 0.76, 0.8);
        runIfEnabled(StageId::Stereo, 0.8, 0.81);
    };
    runBus();

    // Feasibility feedback: if delivering the loudness target would need more
    // limiting than the preset allows, ask the compressor for more (bounded),
    // re-render the bus and re-check. Never applies when the user forced the
    // compressor or the dynamics macro is zero.
    {
        auto& cs = plan.stage(StageId::Compressor);
        const bool loudMode = plan.loudnessMode == LoudnessMode::Integrated || plan.loudnessMode == LoudnessMode::MaxShortTerm ||
                              plan.loudnessMode == LoudnessMode::MaxMomentary;
        const bool allowed = cs.enabled && !cs.forced && uc.dynamics > 0 && uc.sectionOn[int(Section::Output)] && loudMode;
        double compTarget = P.compTargetGrDb * clampv(uc.dynamics, 0.0, 2.0);
        const double compCap = std::min(P.compTargetGrDb * 2.5, P.compTargetGrDb + 6.0) * clampv(uc.dynamics, 0.0, 1.0);
        double lastNeed = 1e9;
        for (int it = 0; allowed && it < 2; ++it)
        {
            // Probe at the delivery rate (true-peak behaviour depends on it).
            const AudioBuffer xp = resample(x, plan.outputSampleRate);
            const double m = loudnessMetric(xp, plan.loudnessMode);
            if (!std::isfinite(m)) break;
            LimiterSettings probe;
            probe.inputGainDb = plan.targetLufs - m;
            probe.ceilingDbTP = plan.ceilingDbTP;
            probe.lookaheadMs = P.limiterLookaheadMs;
            probe.releaseFastMs = P.limiterReleaseMs;
            probe.releaseSlowMs = P.limiterReleaseMs * 6.0;
            TruePeakLimiter tl(probe);
            RenderOptions ro;
            renderProcessor(tl, xp, ro, {});
            const double need = grP99(tl.grTrace()->values());
            // Leave the limiter ~25 % headroom inside its bound.
            const double aim = 0.75 * P.maxLimiterGrDb;
            if (need <= aim + 0.1) break;
            const double newTarget = std::min(compCap, compTarget + (need - aim) * 1.2);
            if (newTarget <= compTarget + 0.1) break;
            if (it > 0 && need > lastNeed - 0.2)
            {
                plan.log.push_back(fmt("Compressor feedback stopped: more compression no longer reduces the limiting need (%.2f dB) - "
                                       "remaining peaks are too short for the compressor.", need));
                break;
            }
            lastNeed = need;
            compTarget = newTarget;
            double p95 = 0;
            const double gate = rmsLevelStats(preComp, 50.0, -90.0).p75 - 30.0;
            S.compressor.thresholdDb = compressorThreshold(preComp, S.compressor, compTarget, gate, &p95);
            S.compressor.maxGrDb = std::max(S.compressor.maxGrDb, compTarget * 2.5);
            const std::string msg = fmt("Correction: target needs %.1f dB limiting (> %.1f bound) -> compressor GR target %.1f dB, threshold %.1f dB.",
                                        need, P.maxLimiterGrDb, compTarget, S.compressor.thresholdDb);
            cs.reason += " " + msg;
            for (auto& kv : cs.params)
                if (kv.first == "Threshold") kv.second = fmt("%.1f dB", S.compressor.thresholdDb);
            plan.log.push_back(msg);
            x = preComp;
            R.metrics.resize(metricsPreComp);
            runBus();
        }
    }

    // ---------------------------------------------------- Resample + Output
    if (std::abs(plan.outputSampleRate - x.sampleRate) > 1e-6)
    {
        phase(0.81, 0.84, "Resampling");
        plan.log.push_back(fmt("Resampled %.0f -> %.0f Hz (r8brain, linear phase, 180 dB).", x.sampleRate, plan.outputSampleRate));
        x = resample(x, plan.outputSampleRate, sub);
        removed = resample(removed, plan.outputSampleRate, sub);
    }

    auto& lim = plan.stage(StageId::Limiter);
    LimiterSettings& L = S.limiter;
    L.lookaheadMs = P.limiterLookaheadMs;
    L.releaseFastMs = P.limiterReleaseMs;
    L.releaseSlowMs = P.limiterReleaseMs * 6.0;
    L.ceilingDbTP = plan.ceilingDbTP;
    LoudnessMode mode = plan.loudnessMode;
    LoudnessStats xs;
    double metric = loudnessMetric(x, mode, &xs);
    if (!std::isfinite(metric) && mode != LoudnessMode::Unchanged && mode != LoudnessMode::Peak)
    {
        plan.log.push_back(fmt("%s loudness undefined for this clip (too short/quiet) -> peak normalisation instead.",
                               loudnessModeName(mode)));
        mode = LoudnessMode::Peak;
        metric = loudnessMetric(x, mode, &xs);
    }
    double gain = 0.0;
    if (mode == LoudnessMode::Peak) gain = plan.ceilingDbTP - xs.truePeakDb;
    else if (mode != LoudnessMode::Unchanged) gain = plan.targetLufs - metric;
    if (gain > 30.0)
    {
        plan.log.push_back(fmt("Required gain %.1f dB capped at +30 dB (avoids lifting noise).", gain));
        gain = 30.0;
    }
    const bool outOn = uc.sectionOn[int(Section::Output)];
    auto ovIt = uc.overrides.find(StageId::Limiter);
    const bool limOff = ovIt != uc.overrides.end() && ovIt->second == Override::Off;
    lim.enabled = outOn && !limOff;
    if (!lim.enabled)
    {
        lim.reason = outOn ? "Bypassed by user: no loudness normalisation or peak protection." : "Output section switched off.";
        R.output = x;
        plan.log.push_back("Output stage bypassed.");
    }
    else
    {
        phase(0.84, 0.97, "Loudness + true-peak limiting");
        double ceil = plan.ceilingDbTP;
        AudioBuffer y;
        StageMetrics m;
        LoudnessStats ys;
        double ym = 0;
        double p99 = 0;
        for (int it = 0; it < 6; ++it)
        {
            L.inputGainDb = gain;
            L.ceilingDbTP = ceil;
            m = StageMetrics{};
            y = runStage(StageId::Limiter, S, x, m, nullptr, holder, sub);
            ym = loudnessMetric(y, mode, &ys);
            p99 = grP99(m.grTrace);
            plan.log.push_back(fmt("Output pass %d: gain %+.2f dB, ceiling %.2f -> %s %.2f, TP %.2f dBTP, limiter GR P99 %.2f / max %.2f dB.",
                                   it + 1, gain, ceil, loudnessModeName(mode), ym, ys.truePeakDb, p99, m.maxGrDb));
            bool changed = false;
            if (ys.truePeakDb > plan.ceilingDbTP + 0.05)
            {
                ceil -= ys.truePeakDb - plan.ceilingDbTP + 0.03;
                changed = true;
            }
            if (mode == LoudnessMode::Integrated || mode == LoudnessMode::MaxMomentary || mode == LoudnessMode::MaxShortTerm)
            {
                const double err = plan.targetLufs - ym;
                const double maxBound = P.maxLimiterGrDb + 6.0; // isolated peaks may take more, within reason
                if (p99 > P.maxLimiterGrDb + 0.1 || m.maxGrDb > maxBound + 0.1)
                {
                    gain -= std::max(p99 - P.maxLimiterGrDb, m.maxGrDb - maxBound);
                    R.targetReached = false;
                    changed = true;
                }
                else if (std::abs(err) > 0.15)
                {
                    if (err > 0 && p99 >= P.maxLimiterGrDb - 0.05) R.targetReached = false;
                    else
                    {
                        gain += err;
                        changed = true;
                    }
                }
            }
            if (!changed) break;
        }
        R.output = std::move(y);
        R.outputLoudnessValue = ym;
        R.metrics.push_back(m);
        lim.params.clear();
        lim.params.emplace_back("Mode", loudnessModeName(mode));
        lim.params.emplace_back("Target", mode == LoudnessMode::Peak ? fmt("%.1f dBTP", plan.ceilingDbTP)
                                          : mode == LoudnessMode::Unchanged ? std::string("level unchanged")
                                                                            : fmt("%.1f LUFS", plan.targetLufs));
        lim.params.emplace_back("Gain", fmt("%+.2f dB", gain));
        lim.params.emplace_back("Ceiling", fmt("%.2f dBTP (internal %.2f)", plan.ceilingDbTP, ceil));
        lim.params.emplace_back("Lookahead", fmt("%.1f ms", L.lookaheadMs));
        lim.params.emplace_back("Release", fmt("%.0f ms fast / %.0f ms sustained", L.releaseFastMs, L.releaseSlowMs));
        lim.params.emplace_back("Detection", "4x oversampled true peak, linked");
        lim.params.emplace_back("GR bound", fmt("P99 <= %.1f dB", P.maxLimiterGrDb));
        lim.reason = fmt("%s %.2f -> %.2f; limiter GR P99 %.2f dB (bound %.1f).", loudnessModeName(mode), metric, ym, p99, P.maxLimiterGrDb);
        if (!R.targetReached)
        {
            lim.reason += fmt(" Target %.1f not reachable within the preset's limiting bound; delivered %.2f.", plan.targetLufs, ym);
            plan.log.push_back(lim.reason);
        }
    }

    // ---------------------------------------------------------- Reference
    phase(0.97, 1.0, "Measuring");
    R.reference = resample(input, plan.outputSampleRate, sub);
    const size_t n = std::max(R.reference.numFrames(), R.output.numFrames());
    padTo(R.reference, n);
    padTo(R.output, n);
    padTo(removed, n);
    removed.resizeFrames(n);
    R.removed = std::move(removed);
    R.outputStats = measureLoudness(R.output);
    R.referenceStats = measureLoudness(R.reference);
    if (R.outputStats.integratedValid() && R.referenceStats.integratedValid())
        R.matchGainDb = R.outputStats.integrated - R.referenceStats.integrated;
    else if (std::isfinite(R.outputStats.maxMomentary) && std::isfinite(R.referenceStats.maxMomentary))
        R.matchGainDb = R.outputStats.maxMomentary - R.referenceStats.maxMomentary;
    else R.matchGainDb = R.outputStats.samplePeakDb - R.referenceStats.samplePeakDb;

    // Combined gain-reduction trace (dynamics + limiter), 10 ms hops.
    size_t hops = 0;
    for (auto& m : R.metrics)
        if (stageSection(m.id) == Section::Dynamics || m.id == StageId::Limiter || m.id == StageId::DynEq || m.id == StageId::DeEss)
            hops = std::max(hops, m.grTrace.size());
    R.grTrace.assign(hops, 0.0f);
    for (auto& m : R.metrics)
        if (stageSection(m.id) == Section::Dynamics || m.id == StageId::Limiter)
            for (size_t i = 0; i < m.grTrace.size(); ++i) R.grTrace[i] += m.grTrace[i];
    plan.log.push_back(fmt("Result: %.2f LUFS (LRA %.1f), TP %.2f dBTP; A/B match gain for original %+.2f dB.", R.outputStats.integrated,
                           R.outputStats.lra, R.outputStats.truePeakDb, R.matchGainDb));
    job.report(1.0, "Done");
    return R;
}

std::string describePlan(const Plan& p)
{
    std::ostringstream o;
    o << "Preset: " << (p.preset ? p.preset->name : "?") << "  | delivery: " << loudnessModeName(p.loudnessMode) << " "
      << p.targetLufs << " / ceiling " << p.ceilingDbTP << " dBTP @ " << p.outputSampleRate << " Hz\n";
    Section last = Section::Output;
    bool first = true;
    for (auto& s : p.stages)
    {
        const Section sec = stageSection(s.id);
        if (first || sec != last) o << "== " << sectionName(sec) << " ==\n";
        first = false;
        last = sec;
        o << (s.enabled ? "  [ON ] " : s.available ? "  [off] " : "  [n/a] ") << stageName(s.id) << ": " << s.reason << "\n";
        if (s.enabled)
            for (auto& kv : s.params) o << "          " << kv.first << ": " << kv.second << "\n";
    }
    o << "-- log --\n";
    for (auto& l : p.log) o << "  " << l << "\n";
    return o.str();
}

} // namespace ac
