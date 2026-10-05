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
    { "Resonance control", "resonance", Section::Tone },
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
        case StageId::Resonance: return std::make_unique<ResonanceSuppressor>(s.resonance);
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
    // Working copy: a reference may widen the delivery bounds (logged below).
    PresetDef Pw = *req.preset;
    const PresetDef& P = Pw;
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
    plan.preset = req.preset;
    initPlan(plan);
    plan.loudnessMode = uc.loudnessMode.value_or(P.loudnessMode);
    plan.targetLufs = uc.targetLufs.value_or(P.targetLufs);
    plan.ceilingDbTP = uc.ceilingDbTP.value_or(P.ceilingDbTP);
    plan.outputSampleRate = uc.outputSampleRate > 0 ? uc.outputSampleRate : input.sampleRate;
    if (uc.reference && uc.matchReferenceLoudness && uc.reference->loudness.integratedValid())
    {
        plan.loudnessMode = LoudnessMode::Integrated;
        plan.targetLufs = uc.reference->loudness.integrated;
        plan.log.push_back(fmt("Reference '%s': %.2f LUFS, LRA %.1f LU, PLR %.1f dB, TP %.2f dBTP -> delivery target %.2f LUFS.",
                               uc.reference->name.c_str(), uc.reference->loudness.integrated, uc.reference->loudness.lra,
                               uc.reference->plrDb, uc.reference->loudness.truePeakDb, plan.targetLufs));
        // A reference louder than the preset's intent shows how much density the
        // delivery needs: widen the limiter (and, for dense references, soft-clip)
        // bounds toward what such masters use, still bounded.
        const auto& rf = *uc.reference;
        if (uc.matchReferenceDynamics && rf.loudness.integrated > req.preset->targetLufs + 0.5 && rf.plrDb > 0)
        {
            const double lim = std::min(6.0, 3.0 + 0.75 * std::max(0.0, 11.0 - rf.plrDb));
            const double clip = rf.plrDb < 9.0 ? std::min(2.5, 1.0 + (9.0 - rf.plrDb)) : 0.0;
            if (lim > Pw.maxLimiterGrDb || clip > Pw.maxClipDb)
            {
                Pw.maxLimiterGrDb = std::max(Pw.maxLimiterGrDb, lim);
                Pw.maxClipDb = std::max(Pw.maxClipDb, clip);
                plan.log.push_back(fmt("Reference is louder than the preset intent (%.1f vs %.1f LUFS, PLR %.1f dB): limiter bound %.1f dB, "
                                       "soft-clip bound %.1f dB.", rf.loudness.integrated, req.preset->targetLufs, rf.plrDb,
                                       Pw.maxLimiterGrDb, Pw.maxClipDb));
            }
        }
    }
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
    if (plan.stage(StageId::Resonance).enabled)
    {
        // Render, check it only caught resonances (bounded mean attenuation), correct once.
        phase(0.6, 0.61, stageName(StageId::Resonance));
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            StageMetrics m;
            AudioBuffer y = runStage(StageId::Resonance, S, x, m, nullptr, holder, sub);
            auto* rs = static_cast<ResonanceSuppressor*>(holder.get());
            const double mean = rs->meanAttenuationDb(), peak = rs->maxFrameAttenuationDb();
            plan.log.push_back(fmt("Resonance control pass %d: mean attenuation %.2f dB, loudest frame %.2f dB (depth %.1f dB).", attempt + 1,
                                   mean, peak, S.resonance.depthDb));
            auto& sp = plan.stage(StageId::Resonance);
            if (mean <= 1.0 || attempt == 1 || sp.forced)
            {
                sp.params.emplace_back("Measured", fmt("mean %.2f dB, loudest frame %.2f dB", mean, peak));
                x = std::move(y);
                R.metrics.push_back(std::move(m));
                break;
            }
            // Broad attenuation means it is reshaping the tone, not catching resonances.
            S.resonance.depthDb *= 0.5;
            S.resonance.thresholdDb += 2.0;
            sp.reason += fmt(" Correction: mean attenuation %.2f dB > 1.0 dB -> depth %.1f dB, threshold %.1f dB.", mean, S.resonance.depthDb,
                             S.resonance.thresholdDb);
            plan.log.push_back(sp.reason);
        }
        throwIfCancelled(job);
    }

    // ----------------------------------------------------------- Dynamics
    phase(0.6, 0.62, "Measuring for dynamics");
    planDynamics(plan, A, P, uc, x);
    const AudioBuffer preDynamics = x;
    const size_t metricsPreDynamics = R.metrics.size();
    runIfEnabled(StageId::Leveler, 0.62, 0.66);
    runIfEnabled(StageId::Expander, 0.66, 0.68);
    runIfEnabled(StageId::Transient, 0.68, 0.7);
    AudioBuffer preComp = x;
    size_t metricsPreComp = R.metrics.size();
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
            probe.releaseSlowMs = plan.limiterSlowReleaseMs > 0 ? plan.limiterSlowReleaseMs : P.limiterReleaseMs * 6.0;
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
            const std::string msg = fmt("Correction: target would need %.1f dB limiting (aim %.1f dB = 75%% of the %.1f dB bound) -> compressor GR target %.1f dB, threshold %.1f dB.",
                                        need, aim, P.maxLimiterGrDb, compTarget, S.compressor.thresholdDb);
            cs.reason += " " + msg;
            for (auto& kv : cs.params)
                if (kv.first == "Threshold") kv.second = fmt("%.1f dB", S.compressor.thresholdDb);
            plan.log.push_back(msg);
            x = preComp;
            R.metrics.resize(metricsPreComp);
            runBus();
        }
    }

    // Closed-loop dynamics: with a reference, probe the delivered result
    // (limiter at the delivery rate and bounds) and move macro dynamics (LRA:
    // slow zero-phase level riding first, like fader automation, then bus
    // compression) and punch (short-term crest: transient emphasis) toward
    // the reference. Bounded; keeps the best probe.
    if (uc.reference && uc.matchReferenceDynamics && uc.dynamics > 0 && uc.sectionOn[int(Section::Dynamics)] &&
        uc.sectionOn[int(Section::Output)] &&
        (plan.loudnessMode == LoudnessMode::Integrated || plan.loudnessMode == LoudnessMode::MaxShortTerm ||
         plan.loudnessMode == LoudnessMode::MaxMomentary))
    {
        phase(0.81, 0.82, "Reference dynamics match");
        const ReferenceProfile& rf = *uc.reference;
        const double amt = clampv(uc.referenceAmount, 0.0, 1.0);
        const bool voice = P.category == Category::Voice;
        auto userOff = [&](StageId id) {
            auto it = uc.overrides.find(id);
            return it != uc.overrides.end() && it->second == Override::Off;
        };
        auto& lv = plan.stage(StageId::Leveler);
        auto& cs = plan.stage(StageId::Compressor);
        auto& ts = plan.stage(StageId::Transient);
        const bool canLevel = lv.available && !lv.forced && !userOff(StageId::Leveler) && A.durationS >= 6.0;
        const bool canComp = cs.available && !cs.forced && !userOff(StageId::Compressor);
        const bool canTrans = !voice && ts.available && !ts.forced && !userOff(StageId::Transient);
        struct Probe
        {
            double lra = 0, crest = 0, short_ = 0;
        };
        auto probe = [&](const AudioBuffer& xin) {
            Probe pr;
            const AudioBuffer xp = resample(xin, plan.outputSampleRate);
            const double m = loudnessMetric(xp, plan.loudnessMode);
            if (!std::isfinite(m)) return pr;
            LimiterSettings ls;
            ls.inputGainDb = plan.targetLufs - m;
            ls.ceilingDbTP = plan.ceilingDbTP;
            ls.lookaheadMs = P.limiterLookaheadMs;
            ls.releaseFastMs = P.limiterReleaseMs;
            ls.releaseSlowMs = plan.limiterSlowReleaseMs > 0 ? plan.limiterSlowReleaseMs : P.limiterReleaseMs * 6.0;
            AudioBuffer y;
            for (int k = 0; k < 2; ++k)
            {
                TruePeakLimiter tl(ls);
                RenderOptions ro;
                y = renderProcessor(tl, xp, ro, {});
                const double need = grP99(tl.grTrace()->values());
                const double bound = P.maxLimiterGrDb + std::max(0.0, P.maxClipDb);
                if (need <= bound + 0.1) break;
                ls.inputGainDb -= need - bound;
            }
            const auto st = measureLoudness(y, false, true);
            pr.lra = st.lra;
            pr.crest = shortTermCrestDb(y);
            pr.short_ = std::max(0.0, plan.targetLufs - loudnessMetric(y, plan.loudnessMode));
            return pr;
        };
        const Probe p0 = probe(x);
        const double tLra = p0.lra + amt * (rf.loudness.lra - p0.lra);
        const double tCrest = p0.crest + amt * (rf.crestDb - p0.crest);
        auto cost = [&](const Probe& q) {
            return std::abs(q.lra - tLra) + (voice ? 0.0 : std::abs(q.crest - tCrest)) + q.short_;
        };
        plan.log.push_back(fmt("Reference dynamics: probe LRA %.1f LU / crest %.1f dB vs reference %.1f / %.1f -> aim %.1f / %.1f (amount %.0f%%).",
                               p0.lra, p0.crest, rf.loudness.lra, rf.crestDb, tLra, tCrest, amt * 100));
        const bool lraOk = std::abs(p0.lra - tLra) <= 0.7;
        const bool crestOk = voice || std::abs(p0.crest - tCrest) <= 0.6;
        if (rf.loudness.lra > 0 && !(lraOk && crestOk))
        {
            struct Best
            {
                double cost;
                ChainSettings s;
                bool levOn, compOn, transOn;
                AudioBuffer x;
                std::vector<StageMetrics> metrics;
                Probe pr;
            } best{ cost(p0), S, lv.enabled, cs.enabled, ts.enabled, x, R.metrics, p0 };
            const bool levWasOn = lv.enabled, compWasOn = cs.enabled, transWasOn = ts.enabled;
            const double dy = clampv(uc.dynamics, 0.0, 2.0);
            // Rider: strength s scales the level spread by ~(1 - s) inside its range.
            double levStrength = lv.enabled ? S.leveler.strength : 0.0;
            double levRange = lv.enabled ? S.leveler.maxBoostDb : 0.0;
            const double levMax = voice ? 0.9 : 0.8;
            double compTarget = cs.enabled ? std::max(0.0, P.compTargetGrDb * dy) : 0.0;
            const double compMax = std::min(10.0, std::max(P.compTargetGrDb, 2.0) + 6.0);
            double attack = ts.enabled ? S.transient.attackDb : 0.0;
            const double attackMin = -3.0, attackMax = std::min(6.0, std::max(0.0, P.transientAttackDb) + 4.0);
            Probe cur = p0;
            for (int it = 0; it < 4; ++it)
            {
                bool moved = false;
                const double dL = cur.lra - tLra;
                if (std::abs(dL) > 0.7)
                {
                    // Macro first: ride level (multiplicative model on the spread).
                    if (canLevel && (dL > 0 ? levStrength < levMax - 0.01 : levStrength > 0.01))
                    {
                        const double keep = clampv((1.0 - levStrength) * tLra / std::max(0.5, cur.lra), 1.0 - levMax, 1.0);
                        const double ns = clampv(1.0 - keep, 0.0, levMax);
                        if (std::abs(ns - levStrength) > 0.02)
                        {
                            levStrength = ns;
                            levRange = clampv(std::max(levRange, 1.5 * std::max(0.0, cur.lra - tLra) + 2.0), 0.0, voice ? 10.0 : 8.0);
                            moved = true;
                        }
                    }
                    // Then the bus compressor for what riding cannot reach (or to back off).
                    if (!moved && canComp)
                    {
                        const double nt = clampv(compTarget + 1.5 * dL, 0.0, compMax);
                        if (std::abs(nt - compTarget) > 0.1) { compTarget = nt; moved = true; }
                    }
                }
                const double dC = tCrest - cur.crest;
                if (canTrans && std::abs(dC) > 0.6)
                {
                    const double na = clampv(attack + 1.2 * dC, attackMin, attackMax);
                    if (std::abs(na - attack) > 0.1) { attack = na; moved = true; }
                }
                if (!moved) break;
                // Re-render rider, expander, transient shaper and bus from the snapshot.
                x = preDynamics;
                R.metrics.resize(metricsPreDynamics);
                if (canLevel)
                {
                    S.leveler.strength = levStrength;
                    S.leveler.maxBoostDb = S.leveler.maxCutDb = levRange;
                    if (!voice)
                    {
                        // Music: ride on the short-term (3 s) level that LRA measures, with
                        // LRA's relative gate (a noise-floor gate is meaningless for dense mixes).
                        S.leveler.windowMs = 3000;
                        S.leveler.smoothingMs = 600; // ~1 s zero-phase glide, ahead of section changes
                        S.leveler.gateDb = S.leveler.targetDb - 20.0;
                    }
                    lv.enabled = levStrength > 0.02 && levRange > 0.5;
                }
                runIfEnabled(StageId::Leveler, 0.82, 0.83);
                runIfEnabled(StageId::Expander, 0.83, 0.835);
                S.transient.attackDb = attack;
                if (canTrans) ts.enabled = std::abs(attack) >= 0.25 || std::abs(S.transient.sustainDb) >= 0.5;
                runIfEnabled(StageId::Transient, 0.835, 0.84);
                preComp = x;
                metricsPreComp = R.metrics.size();
                if (canComp)
                {
                    cs.enabled = compTarget > 0.05;
                    if (cs.enabled)
                    {
                        const double gate = rmsLevelStats(preComp, 50.0, -90.0).p75 - 30.0;
                        S.compressor.thresholdDb = compressorThreshold(preComp, S.compressor, compTarget, gate, nullptr);
                        S.compressor.maxGrDb = std::max(6.0, compTarget * 2.5);
                    }
                }
                runBus();
                cur = probe(x);
                const double c = cost(cur);
                plan.log.push_back(fmt("Reference dynamics pass %d: rider %.0f%% / +/-%.1f dB, compressor GR target %.1f dB, transient attack %+.1f dB -> LRA %.1f, crest %.1f%s (cost %.2f).",
                                       it + 1, lv.enabled ? levStrength * 100 : 0.0, lv.enabled ? levRange : 0.0, cs.enabled ? compTarget : 0.0,
                                       ts.enabled ? attack : 0.0, cur.lra, cur.crest,
                                       cur.short_ > 0.05 ? fmt(", %.1f dB short of target", cur.short_).c_str() : "", c));
                if (c < best.cost - 0.05) best = Best{ c, S, lv.enabled, cs.enabled, ts.enabled, x, R.metrics, cur };
            }
            S = best.s;
            lv.enabled = best.levOn;
            cs.enabled = best.compOn;
            ts.enabled = best.transOn;
            x = std::move(best.x);
            R.metrics = std::move(best.metrics);
            const std::string summary = fmt("Reference dynamics: LRA %.1f -> %.1f LU, crest %.1f -> %.1f dB (aim %.1f / %.1f).", p0.lra,
                                            best.pr.lra, p0.crest, best.pr.crest, tLra, tCrest);
            plan.log.push_back(summary);
            // The reference shows this material can be delivered denser than the
            // preset's limiting bound: when its loudness is still out of reach,
            // widen the bound by the shortfall (hard cap 6 dB), logged.
            if (uc.matchReferenceLoudness && best.pr.short_ > 0.3 && Pw.maxLimiterGrDb < 6.0)
            {
                const double was = Pw.maxLimiterGrDb;
                // Limiting has diminishing returns: each dB of shortfall needs ~1.5 dB more GR.
                Pw.maxLimiterGrDb = std::min(6.0, Pw.maxLimiterGrDb + 1.5 * best.pr.short_);
                plan.log.push_back(fmt("Reference loudness %.1f dB out of reach after dynamics: limiter bound %.1f -> %.1f dB (cap 6).",
                                       best.pr.short_, was, Pw.maxLimiterGrDb));
            }
            auto setParam = [](StagePlan& sp, const std::string& k, const std::string& v) {
                for (auto& kv : sp.params)
                    if (kv.first == k) { kv.second = v; return; }
                sp.params.emplace_back(k, v);
            };
            if (lv.enabled)
            {
                lv.reason = levWasOn ? lv.reason + " " + summary : "Enabled to match the reference's macro dynamics. " + summary;
                setParam(lv, "Range", fmt("+/-%.1f dB", S.leveler.maxBoostDb));
                setParam(lv, "Strength", fmt("%.0f%%", S.leveler.strength * 100));
                setParam(lv, "Window", fmt("%.0f ms", S.leveler.windowMs));
                setParam(lv, "Smoothing", fmt("%.0f ms zero-phase", S.leveler.smoothingMs));
            }
            if (cs.enabled)
            {
                cs.reason = compWasOn ? cs.reason + " " + summary : "Enabled to match the reference's dynamics. " + summary;
                setParam(cs, "Threshold", fmt("%.1f dB", S.compressor.thresholdDb));
                setParam(cs, "GR bound", fmt("%.1f dB", S.compressor.maxGrDb));
            }
            if (ts.enabled)
            {
                const std::string m = fmt("Reference punch match: attack %+.1f dB.", S.transient.attackDb);
                ts.reason = transWasOn ? ts.reason + " " + m : "Enabled to match the reference's punch. " + m;
                setParam(ts, "Attack", fmt("%+.1f dB", S.transient.attackDb));
            }
        }
        else plan.log.push_back("Reference dynamics already within tolerance (LRA +/-0.7 LU, crest +/-0.6 dB).");
    }

    // Closed-loop tone: with a reference, measure what dynamics/saturation did to
    // the balance and apply a bounded residual correction before delivery.
    if (uc.reference && plan.stage(StageId::Eq).enabled)
    {
        phase(0.81, 0.82, "Reference residual match");
        const auto si = longTermSpectrum(x);
        double rms0 = 0;
        auto dev = referenceDeviation(si, uc.reference->spectrum, &rms0);
        const double identityHz = P.category == Category::Voice ? std::max(150.0, 1.6 * A.speech.f0MedianHz) : 60.0;
        const double strength = 0.7 * clampv(uc.referenceAmount, 0.0, 1.0) * std::min(clampv(uc.tone, 0.0, 2.0), 1.5);
        for (size_t i = 0; i < dev.size(); ++i) dev[i] = si.bandHz[i] < identityHz ? 0.0 : dev[i] * strength;
        auto bands = fitEqBands(si.bandHz, dev, 2.0, 2.0, x.sampleRate, P.category, identityHz, 4);
        bool any = false;
        for (auto& b : bands) any |= std::abs(b.gainDb) >= 0.3;
        if (any && rms0 > 0.75)
        {
            ParametricEq eq(bands);
            RenderOptions ro;
            ro.keepTail = false;
            AudioBuffer y = renderProcessor(eq, x, ro, sub);
            y.resizeFrames(x.numFrames());
            double rms1 = 0;
            referenceDeviation(longTermSpectrum(y), uc.reference->spectrum, &rms1);
            if (rms1 < rms0)
            {
                x = std::move(y);
                std::string list;
                for (auto& b : bands) list += fmt("%.0f Hz %+.1f; ", b.freq, b.gainDb);
                plan.stage(StageId::Eq).params.emplace_back("Residual match (after dynamics)", list);
                plan.log.push_back(fmt("Reference residual match: deviation %.2f -> %.2f dB rms after dynamics.", rms0, rms1));
            }
            else plan.log.push_back(fmt("Reference residual match skipped (would not improve: %.2f -> %.2f dB rms).", rms0, rms1));
        }
        else plan.log.push_back(fmt("Reference residual deviation %.2f dB rms after dynamics: no correction needed.", rms0));
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
    L.releaseSlowMs = plan.limiterSlowReleaseMs > 0 ? plan.limiterSlowReleaseMs : P.limiterReleaseMs * 6.0;
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
        // Loud presets may drive a bounded amount into an oversampled soft
        // clipper ahead of the limiter when the limiter bound alone cannot
        // reach the target (clip depth <= preset maxClipDb, logged).
        double clipDrive = 0.0;
        const bool loudMode = mode == LoudnessMode::Integrated || mode == LoudnessMode::MaxShortTerm || mode == LoudnessMode::MaxMomentary;
        if (!R.targetReached && loudMode && P.maxClipDb > 0 && uc.dynamics > 0)
        {
            // Clipper asymptote sits above the final ceiling by part of the
            // limiter's budget; the limiter then only catches what remains.
            const double clipCeil = plan.ceilingDbTP + 0.5 * P.maxLimiterGrDb;
            double g2 = gain, ceil2 = ceil;
            double lo = gain - 2.0 * P.maxClipDb, hi = std::numeric_limits<double>::quiet_NaN(); // bracket on drive
            bool accepted = false;
            for (int it = 0; it < 10; ++it)
            {
                SoftClipper clip(g2, clipCeil);
                RenderOptions ro;
                AudioBuffer xc = renderProcessor(clip, x, ro, sub);
                const double clipP99 = grP99(clip.grTrace()->values());
                ChainSettings S2 = S;
                S2.limiter.inputGainDb = 0.0;
                S2.limiter.ceilingDbTP = ceil2;
                StageMetrics m2;
                AudioBuffer y2 = runStage(StageId::Limiter, S2, xc, m2, nullptr, holder, sub);
                LoudnessStats ys2;
                const double ym2 = loudnessMetric(y2, mode, &ys2);
                const double limP99 = grP99(m2.grTrace);
                plan.log.push_back(fmt("Clip pass %d: gain %+.2f dB into soft clipper (asymptote %.2f dBTP): clip GR P99 %.2f dB, limiter GR P99 %.2f dB -> %s %.2f, TP %.2f.",
                                       it + 1, g2, clipCeil, clipP99, limP99, loudnessModeName(mode), ym2, ys2.truePeakDb));
                if (ys2.truePeakDb > plan.ceilingDbTP + 0.05)
                {
                    ceil2 -= ys2.truePeakDb - plan.ceilingDbTP + 0.03;
                    continue;
                }
                const bool within = clipP99 <= P.maxClipDb + 0.1 && limP99 <= P.maxLimiterGrDb + 0.1;
                if (within && ym2 > ym + 0.05)
                {
                    y = std::move(y2);
                    ym = ym2;
                    ys = ys2;
                    m = m2;
                    p99 = limP99;
                    clipDrive = clipP99;
                    accepted = true;
                }
                const double err = plan.targetLufs - ym2;
                if (within && std::abs(err) <= 0.15) break;
                if (within && err < 0) hi = g2; // overshot the target
                else if (within) lo = g2;
                else hi = g2;
                const double next = std::isnan(hi) ? g2 + err : 0.5 * (lo + hi);
                if (!std::isnan(hi) && hi - lo < 0.1) break;
                g2 = next;
            }
            if (accepted) R.targetReached = std::abs(plan.targetLufs - ym) <= 0.3;
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
        if (P.maxClipDb > 0)
            lim.params.emplace_back("Soft clip", clipDrive > 0 ? fmt("4x oversampled soft clipper, GR P99 %.2f dB (bound %.1f dB)", clipDrive, P.maxClipDb)
                                                               : std::string("not used"));
        lim.reason = fmt("%s %.2f -> %.2f; limiter GR P99 %.2f dB (bound %.1f)%s.", loudnessModeName(mode), metric, ym, p99, P.maxLimiterGrDb,
                         clipDrive > 0 ? fmt("; soft clipper GR P99 %.1f dB", clipDrive).c_str() : "");
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
    if (uc.reference && R.outputStats.integratedValid())
    {
        const auto& rf = *uc.reference;
        plan.log.push_back(fmt("Versus reference '%s': loudness %.2f / %.2f LUFS, LRA %.1f / %.1f LU, PLR %.1f / %.1f dB, crest %.1f / %.1f dB.",
                               rf.name.c_str(), R.outputStats.integrated, rf.loudness.integrated, R.outputStats.lra, rf.loudness.lra,
                               R.outputStats.truePeakDb - R.outputStats.integrated, rf.plrDb, shortTermCrestDb(R.output), rf.crestDb));
    }
    job.report(1.0, "Done");
    return R;
}

void referenceDeviationRms(const AudioBuffer& b, const ReferenceProfile& ref, double& rmsOut)
{
    referenceDeviation(longTermSpectrum(b), ref.spectrum, &rmsOut);
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
