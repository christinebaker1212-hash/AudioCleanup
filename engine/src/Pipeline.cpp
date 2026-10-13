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
    { "Echo / delay", "echo", Section::Tone },
    { "Reverb", "reverb", Section::Tone },
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
        case StageId::Echo: return std::make_unique<EchoProcessor>(s.echo);
        case StageId::Reverb: return std::make_unique<ConvolutionReverb>(s.reverb);
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

/** Duplicate a mono buffer to stereo (no-op otherwise). */
AudioBuffer toStereo(const AudioBuffer& b)
{
    if (b.numChannels() != 1) return b;
    AudioBuffer s(2, b.numFrames(), b.sampleRate);
    s.vec(0) = b.vec(0);
    s.vec(1) = b.vec(0);
    return s;
}

/** Side-minus-mid power (dB); -100 for mono. */
double sideToMidDb(const AudioBuffer& b)
{
    if (b.numChannels() != 2) return -100.0;
    double m = 0, sd = 0;
    for (size_t i = 0; i < b.numFrames(); ++i)
    {
        const double l = b.channel(0)[i], r = b.channel(1)[i];
        m += (l + r) * (l + r);
        sd += (l - r) * (l - r);
    }
    return powerToDb(sd / std::max(m, 1e-30), -100.0);
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
    if (uc.fxReference && uc.fxLoudness)
    {
        // Sound effects are delivered by their loudest moment: take the reference's.
        const FxProfile& F = *uc.fxReference;
        if (std::isfinite(F.loudness.maxMomentary))
        {
            plan.loudnessMode = LoudnessMode::MaxMomentary;
            plan.targetLufs = F.loudness.maxMomentary;
        }
        else
        {
            plan.loudnessMode = LoudnessMode::Peak;
            plan.targetLufs = std::min(F.loudness.truePeakDb, plan.ceilingDbTP);
        }
        plan.log.push_back(fmt("Effect reference '%s': delivery %s %.2f, as the reference.", F.name.c_str(),
                               loudnessModeName(plan.loudnessMode), plan.targetLufs));
    }
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
    // With an effects reference the reference sets tone and dynamics: the
    // preset's own stages step aside (a stage the user forced stays).
    auto fxStepAside = [&](std::initializer_list<StageId> ids) {
        if (!uc.fxReference) return;
        for (StageId id : ids)
        {
            auto& sp = plan.stage(id);
            if (sp.forced || !sp.enabled) continue;
            sp.enabled = false;
            sp.reason = "Effect matching sets this from the reference clip. (Preset decision: " + sp.reason + ")";
        }
    };
    fxStepAside({ StageId::Eq, StageId::DynEq, StageId::Resonance, StageId::Saturation, StageId::Stereo });
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
    // De-clip sees the raw samples: filtering or DC removal first would bend the
    // flat plateaus away from the measured clip level.
    const StageId cleanup[] = { StageId::Declip, StageId::Filter, StageId::Declick, StageId::Dehum, StageId::Denoise,
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
    fxStepAside({ StageId::Leveler, StageId::Expander, StageId::Transient, StageId::Compressor, StageId::Multiband, StageId::Saturation,
                  StageId::Stereo });
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
        runIfEnabled(StageId::Echo, 0.81, 0.812);
        runIfEnabled(StageId::Reverb, 0.812, 0.815);
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


    // ------------------------------------------------- Effect matching (SFX)
    // Put the effects estimated on the reference clip on this one, in the
    // order a sound designer would chain them (band limit + EQ, dynamics,
    // echo, reverb, width), then close the loops on the rendered result:
    // punch (peak-to-loudness), reverb level (tail re event peak), width and
    // tone are measured on the output exactly as on the reference.
    bool fxUpmixed = false;
    if (uc.fxReference)
    {
        phase(0.81, 0.84, "Effect matching");
        const FxProfile& F = *uc.fxReference;
        const double amt = clampv(uc.fxAmount, 0.0, 1.0);
        auto userOff = [&](StageId id) {
            auto it = uc.overrides.find(id);
            return it != uc.overrides.end() && it->second == Override::Off;
        };
        plan.log.push_back("Effect reference '" + F.name + "':\n" + describeEffects(F));
        if (F.stereo && x.numChannels() == 1 && uc.fxWidth && amt > 0)
        {
            x = toStereo(x);
            fxUpmixed = true;
            plan.log.push_back("Mono input made stereo to carry the reference's stereo image.");
        }
        const AudioBuffer fxIn = x;
        ChainSettings FS = S;
        FS.eq.clear();
        FS.transient = TransientSettings{};
        FS.echo = EchoSettings{};
        FS.reverb = ReverbSettings{};
        FS.stereo = StereoSettings{};
        bool useEq = false, useTrans = false, useEcho = false, useRev = false, useStereo = false;
        std::vector<EqBand> limitBands;
        const bool toneOk = uc.fxTone && !userOff(StageId::Eq) && amt > 0;
        if (toneOk && F.bandLimit.hpfHz > 0)
            for (auto& b : butterworthBands(SvfType::Highpass, F.bandLimit.hpfHz, F.bandLimit.hpfOrder)) limitBands.push_back(b);
        if (toneOk && F.bandLimit.lpfHz > 0 && F.bandLimit.lpfHz < 0.45 * x.sampleRate)
            for (auto& b : butterworthBands(SvfType::Lowpass, F.bandLimit.lpfHz, F.bandLimit.lpfOrder)) limitBands.push_back(b);
        FS.eq = limitBands;
        useEq = !limitBands.empty();
        if (uc.fxEcho && F.echo.found && !userOff(StageId::Echo) && amt > 0)
        {
            FS.echo.delayMs = F.echo.delayMs;
            FS.echo.feedback = F.echo.feedback;
            FS.echo.levelDb = F.echo.levelDb + gainToDb(amt);
            FS.echo.pingPong = F.echo.pingPong && x.numChannels() == 2;
            FS.echo.dampingHz = F.echo.dampingHz;
            useEcho = true;
        }
        if (uc.fxReverb && F.reverb.found && !userOff(StageId::Reverb) && amt > 0)
        {
            FS.reverb.rt60 = F.reverb.rt60;
            FS.reverb.bandHz = F.reverb.bandHz;
            FS.reverb.bandRt60 = F.reverb.bandRt60;
            FS.reverb.preDelayMs = 10.0;
            FS.reverb.wetDb = F.reverb.tailLevelDb + 6.0; // first guess; the loop below sets it
            FS.reverb.stereo = x.numChannels() == 2;
            useRev = true;
        }
        auto renderOn = [&](const AudioBuffer& in, const ChainSettings& cs, std::vector<StageMetrics>* keep, bool withEcho) {
            AudioBuffer y = in;
            const std::pair<StageId, bool> chain[] = { { StageId::Eq, useEq },     { StageId::Transient, useTrans }, { StageId::Echo, useEcho && withEcho },
                                                       { StageId::Reverb, useRev }, { StageId::Stereo, useStereo } };
            for (auto [id, on] : chain)
            {
                if (!on) continue;
                StageMetrics m;
                y = runStage(id, cs, y, m, nullptr, holder, sub);
                if (keep) keep->push_back(std::move(m));
            }
            return y;
        };
        auto render = [&](const ChainSettings& cs, std::vector<StageMetrics>* keep, bool withEcho = true) {
            return renderOn(fxIn, cs, keep, withEcho);
        };

        // 1. Reverb level: the output's tail (re each event's early energy) to the reference's.
        // Reverb and echo fill the gaps between events, so they come before the
        // punch match; one more pass afterwards absorbs what compression moved.
        double tailOut = -100;
        bool tailMeasured = false;
        auto matchReverbLevel = [&] {
            if (!useRev) return;
            const double want = F.reverb.tailLevelDb + gainToDb(amt);
            // Calibrated on this clip's loudest event, isolated (5 ms before its
            // peak to 60 ms after, then silence): "on one hit of this sound, the
            // tail sits as on the reference's events". Well defined on any
            // material; in a dense clip every event already carries the
            // previous ones' reverb, so the full clip cannot be read this way.
            const size_t n = fxIn.numFrames(), sr = size_t(fxIn.sampleRate);
            size_t pk = 0;
            float pv = 0;
            for (int c = 0; c < fxIn.numChannels(); ++c)
                for (size_t i = 0; i < n; ++i)
                    if (std::abs(fxIn.channel(c)[i]) > pv) { pv = std::abs(fxIn.channel(c)[i]); pk = i; }
            const size_t a0 = pk > sr / 200 ? pk - sr / 200 : 0, a1 = std::min(n, pk + sr * 60 / 1000);
            AudioBuffer probe(fxIn.numChannels(), (a1 - a0) + 3 * sr, fxIn.sampleRate);
            const size_t fade = std::max<size_t>(1, sr / 1000);
            for (int c = 0; c < probe.numChannels(); ++c)
                for (size_t i = a0; i < a1; ++i)
                {
                    const double g = std::min({ 1.0, double(i - a0) / double(fade), double(a1 - i) / double(fade) });
                    probe.channel(c)[i - a0] = float(fxIn.channel(c)[i] * g);
                }
            // Start where the hit still stands out (the tail then follows the wet
            // level ~1:1); if a step makes the decay unmeasurable, extrapolate
            // once from the last reading.
            FS.reverb.wetDb = -10.0;
            for (int it = 0; it < 6; ++it)
            {
                // Without the echo, as the reference's reverb was measured with its echo removed.
                const auto m = measureReverb(renderOn(probe, FS, nullptr, false));
                if (m.segments == 0)
                {
                    if (tailMeasured) FS.reverb.wetDb = clampv(FS.reverb.wetDb, -40.0, 12.0);
                    break;
                }
                tailMeasured = true;
                tailOut = m.tailLevelDb;
                const double err = want - tailOut;
                if (std::abs(err) < 0.75) break;
                FS.reverb.wetDb = clampv(FS.reverb.wetDb + err, -40.0, 12.0);
            }
        };
        matchReverbLevel();

        // 2. Dynamics: the events' body (mean level 10-150 ms after each peak,
        // effects included) toward the reference's. Compression on the reference
        // shows up as this envelope (fuller with a fast attack, more pointed with
        // a slow one); it is reproduced with the transient shaper's sustain, in
        // closed loop, keeping the best setting seen.
        double bodyOut = 0, bodyStart = 0;
        bool bodyMeasured = false;
        const bool dynOk = uc.fxDynamics && amt > 0 && F.bodyDb < 0 && !userOff(StageId::Transient);
        auto matchSustain = [&] {
            if (!dynOk) return;
            auto measure = [&] { return eventBodyDb(render(FS, nullptr)); };
            const double start = measure();
            if (start >= 0) return; // no events with a free decay in this clip
            if (!bodyMeasured) bodyStart = start;
            bodyMeasured = true;
            const double want = start + (F.bodyDb - start) * amt;
            double cur = start, bestErr = std::abs(want - start);
            TransientSettings best = FS.transient;
            for (int it = 0; it < 8 && bestErr > 0.75; ++it)
            {
                const double err = want - cur;
                const TransientSettings before = FS.transient;
                FS.transient.sustainDb = clampv(FS.transient.sustainDb + err, -15.0, 15.0);
                if (std::abs(FS.transient.sustainDb - before.sustainDb) < 0.1) break;
                useTrans = true;
                const double now = measure();
                if (now >= 0 || std::abs(want - now) > std::abs(err) - 0.1)
                {
                    FS.transient = before; // no progress: stop
                    break;
                }
                cur = now;
                if (std::abs(want - cur) < bestErr)
                {
                    bestErr = std::abs(want - cur);
                    best = FS.transient;
                }
            }
            FS.transient = best;
            useTrans = std::abs(FS.transient.sustainDb) > 0.2 || std::abs(FS.transient.attackDb) > 0.2;
        };
        // The two interact (more reverb fills the body, softer onsets change the
        // early energy): alternate.
        for (int round = 0; round < 2; ++round)
        {
            matchSustain();
            matchReverbLevel();
        }

        if (dynOk && bodyMeasured) bodyOut = eventBodyDb(render(FS, nullptr));

        // 3. Width: side/mid of the result toward the reference's.
        double smOut = -100;
        if (uc.fxWidth && F.stereo && x.numChannels() == 2 && !userOff(StageId::Stereo) && amt > 0)
        {
            smOut = sideToMidDb(render(FS, nullptr));
            if (smOut > -45.0)
            {
                const double d = clampv((F.sideToMidDb - smOut) * amt, -12.0, 9.0);
                if (std::abs(d) > 0.5)
                {
                    FS.stereo.width = dbToGain(d);
                    useStereo = true;
                }
            }
        }

        // 4. Tone: long-term spectrum of the result toward the reference's
        // (60 % of the deviation, bounded), on top of the band limits.
        double toneBefore = 0, toneAfter = 0, toneWeight = 1.0;
        if (toneOk)
        {
            const double lo = F.bandLimit.hpfHz > 0 ? F.bandLimit.hpfHz * 1.3 : 40.0;
            const double hi = F.bandLimit.lpfHz > 0 ? F.bandLimit.lpfHz / 1.3 : 16000.0;
            for (int it = 0; it < 2; ++it)
            {
                const auto sp = longTermSpectrum(render(FS, nullptr));
                double rms = 0;
                auto dev = referenceDeviation(sp, F.spectrum, &rms);
                if (it == 0)
                {
                    toneBefore = rms;
                    // Spectra this far apart are different sounds (a whistle and a
                    // drum), not different EQ: tone matching fades out from 4 to
                    // 8 dB rms and only clear filter effects (band limits) remain.
                    toneWeight = clampv((8.0 - rms) / 4.0, 0.0, 1.0);
                }
                if (toneWeight <= 0.0) break;
                for (size_t i = 0; i < dev.size(); ++i)
                {
                    const bool outside = sp.bandHz[i] < lo || sp.bandHz[i] > hi;
                    const bool absent = sp.bandAbsDb[i] < -100.0;
                    dev[i] = outside || (absent && dev[i] > 0) ? 0.0 : dev[i] * 0.6 * amt * toneWeight;
                }
                auto bands = fitEqBands(sp.bandHz, dev, 6.0, 9.0, x.sampleRate, Category::SoundEffect, 60.0, 4);
                if (bands.empty()) break;
                for (auto& b : bands) FS.eq.push_back(b);
                useEq = true;
            }
            double rms = 0;
            referenceDeviation(longTermSpectrum(render(FS, nullptr)), F.spectrum, &rms);
            toneAfter = rms;
        }

        // Final render (metrics kept) and the plan.
        std::vector<StageMetrics> ms;
        x = render(FS, &ms);
        for (auto& m : ms) R.metrics.push_back(std::move(m));
        S.eq = FS.eq;
        S.transient = FS.transient;
        S.echo = FS.echo;
        S.reverb = FS.reverb;
        S.stereo = FS.stereo;
        auto set = [&](StageId id, bool on, const std::string& why) {
            auto& sp = plan.stage(id);
            sp.enabled = on;
            sp.available = true;
            sp.reason = why;
        };
        {
            auto& sp = plan.stage(StageId::Eq);
            std::string why;
            if (!limitBands.empty())
                why += fmt("Band limit from the reference's spectrum edges:%s%s. ",
                           F.bandLimit.hpfHz > 0 ? fmt(" high-pass %.0f Hz (%d dB/oct)", F.bandLimit.hpfHz, 6 * F.bandLimit.hpfOrder).c_str() : "",
                           F.bandLimit.lpfHz > 0 ? fmt(" low-pass %.0f Hz (%d dB/oct)", F.bandLimit.lpfHz, 6 * F.bandLimit.lpfOrder).c_str() : "");
            if (toneOk && toneWeight <= 0.0)
                why += fmt("Tone left as it is: the spectra differ by %.1f dB rms, i.e. different kinds of sound rather than different EQ.", toneBefore);
            else if (toneOk)
                why += fmt("Tone moved %.0f%% toward the reference: balance deviation %.1f -> %.1f dB rms (bounded +6/-9 dB).", 60 * amt * toneWeight,
                           toneBefore, toneAfter);
            set(StageId::Eq, useEq, toneOk ? "Effect matching. " + why : "Effect matching: tone not matched (switched off).");
            sp.params.clear();
            std::string list;
            for (auto& b : FS.eq)
                list += b.type == SvfType::Highpass ? fmt("HP %.0f Hz; ", b.freq)
                        : b.type == SvfType::Lowpass ? fmt("LP %.0f Hz; ", b.freq)
                                                     : fmt("%.0f Hz %+.1f dB; ", b.freq, b.gainDb);
            sp.params.emplace_back("Bands", list.empty() ? "none" : list);
        }
        {
            auto& ts = plan.stage(StageId::Transient);
            ts.params.clear();
            if (!uc.fxDynamics) set(StageId::Transient, false, "Effect matching: dynamics not matched (switched off).");
            else if (F.bodyDb >= 0)
                set(StageId::Transient, false, "Effect matching: the reference is not made of separate hits (needs 3+ events with free decays); dynamics left as they are.");
            else if (!bodyMeasured)
                set(StageId::Transient, false, "Effect matching: this clip is not made of separate hits (needs 3+ events with free decays); dynamics left as they are.");
            else
            {
                const std::string sus = fmt("Event body %.1f -> %.1f dB re peak; reference %.1f dB", bodyStart, bodyOut, F.bodyDb);
                set(StageId::Transient, useTrans,
                    useTrans ? fmt("Effect matching: envelope %s to match the reference (its compression / shaping). ",
                                   FS.transient.sustainDb > 0 ? "made fuller" : "tightened") + sus + "."
                             : "Effect matching: the envelope already matches. " + sus + ".");
                if (useTrans) ts.params.emplace_back("Sustain", fmt("%+.1f dB", FS.transient.sustainDb));
            }
        }
        {
            auto& sp = plan.stage(StageId::Echo);
            sp.params.clear();
            if (useEcho)
            {
                set(StageId::Echo, true, fmt("Echo found on the reference (confidence %.0f%%).", 100 * F.echo.confidence));
                sp.params.emplace_back("Delay", fmt("%.1f ms", FS.echo.delayMs));
                sp.params.emplace_back("Feedback", fmt("%.2f", FS.echo.feedback));
                sp.params.emplace_back("First repeat", fmt("%.1f dB", FS.echo.levelDb));
                if (FS.echo.pingPong) sp.params.emplace_back("Stereo", "ping-pong");
                if (FS.echo.dampingHz > 0) sp.params.emplace_back("Damping", fmt("above %.0f Hz", FS.echo.dampingHz));
            }
            else set(StageId::Echo, false, F.echo.found ? "Echo found on the reference but switched off." : "No echo found on the reference.");
        }
        {
            auto& sp = plan.stage(StageId::Reverb);
            sp.params.clear();
            if (useRev)
            {
                set(StageId::Reverb, true,
                    fmt("Reverb found on the reference: RT60 %.2f s, tail %.1f dB re the event (confidence %.0f%%). %s", F.reverb.rt60, F.reverb.tailLevelDb,
                        100 * F.reverb.confidence,
                        tailMeasured ? fmt("Level set in closed loop on this clip's loudest hit: tail %.1f dB re the event.", tailOut).c_str()
                                     : "Level not measurable on this clip: first estimate kept."));
                std::string bands;
                for (size_t i = 0; i < FS.reverb.bandHz.size(); ++i)
                    if (FS.reverb.bandRt60[i] > 0) bands += fmt("%.0f Hz %.2f s; ", FS.reverb.bandHz[i], FS.reverb.bandRt60[i]);
                sp.params.emplace_back("Decay per band", bands.empty() ? fmt("%.2f s", FS.reverb.rt60) : bands);
                sp.params.emplace_back("Wet", fmt("%.1f dB re dry", FS.reverb.wetDb));
                sp.params.emplace_back("Pre-delay", fmt("%.0f ms", FS.reverb.preDelayMs));
                sp.params.emplace_back("Impulse response", "decorrelated noise, per-band exponential decay (synthesised)");
            }
            else set(StageId::Reverb, false, F.reverb.found ? "Reverb found on the reference but switched off." : "No reverb found on the reference.");
        }
        {
            auto& sp = plan.stage(StageId::Stereo);
            sp.params.clear();
            if (useStereo)
            {
                set(StageId::Stereo, true, fmt("Effect matching: side/mid %.1f dB -> reference %.1f dB.", smOut, F.sideToMidDb));
                sp.params.emplace_back("Width", fmt("%.2f", FS.stereo.width));
            }
            else if (fxUpmixed) set(StageId::Stereo, false, "Effect matching: mono input made stereo; the stereo reverb/echo carry the width.");
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
    if (fxUpmixed)
    {
        R.reference = toStereo(R.reference);
        removed = toStereo(removed);
    }
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
