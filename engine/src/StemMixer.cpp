#include "ac/StemMixer.h"

#include "ac/Resampler.h"

#include <cctype>
#include <cstdio>

namespace ac {

const char* stemRoleName(StemRole r)
{
    static const char* n[] = { "Lead vocal", "Backing vocal", "Dialogue", "Drums", "Bass", "Keys", "Guitar", "Synth", "Strings", "FX", "Ambience", "Other" };
    return n[std::min(size_t(r), size_t(StemRole::Count) - 1)];
}

int defaultBusFor(StemRole r)
{
    switch (r)
    {
        case StemRole::LeadVocal:
        case StemRole::BackingVocal:
        case StemRole::Dialogue: return 0;
        case StemRole::Fx:
        case StemRole::Ambience: return 2;
        default: return 1;
    }
}

std::vector<BusSpec> defaultBuses() { return { { "Vocals", 0.0, true }, { "Music", 0.0, true }, { "FX / Ambience", 0.0, false } }; }

StemRole guessRole(const std::string& nameIn)
{
    std::string n;
    for (char c : nameIn) n += char(std::tolower((unsigned char)c));
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    if (has("lead") && (has("vox") || has("voc"))) return StemRole::LeadVocal;
    if (has("bgv") || has("backing") || has("harm") || has("choir")) return StemRole::BackingVocal;
    if (has("vox") || has("vocal") || has("voice")) return StemRole::LeadVocal;
    if (has("dialog") || has("dx") || has("narr")) return StemRole::Dialogue;
    if (has("kick") || has("snare") || has("drum") || has("hat") || has("tom") || has("perc") || has("overhead") || has("cymbal")) return StemRole::Drums;
    if (has("bass") || has("sub")) return StemRole::Bass;
    if (has("piano") || has("key") || has("organ") || has("rhodes")) return StemRole::Keys;
    if (has("gtr") || has("guitar")) return StemRole::Guitar;
    if (has("synth") || has("pad") || has("lead")) return StemRole::Synth;
    if (has("string") || has("violin") || has("cello") || has("orch")) return StemRole::Strings;
    if (has("fx") || has("sfx") || has("riser") || has("impact")) return StemRole::Fx;
    if (has("amb") || has("room") || has("atmos")) return StemRole::Ambience;
    return StemRole::Other;
}

PresetDef stemPreset(StemRole r)
{
    PresetDef p;
    switch (r)
    {
        case StemRole::LeadVocal: p = *findPreset("voice.studio"); p.levelerRangeDb = 4.0; break;
        case StemRole::BackingVocal: p = *findPreset("voice.warm"); p.saturationDriveDb = 0.0; p.levelerRangeDb = 0.0; break;
        case StemRole::Dialogue: p = *findPreset("voice.natural"); break;
        default:
        {
            p = *findPreset("sfx.transparent");
            p.useTargetCurve = false;
            p.denoiseDefault = false; // instruments: like music, no NR unless forced
            p.compStyle.detector = DetectorMode::Rms;
            p.compStyle.rmsWindowMs = 10.0;
            switch (r)
            {
                case StemRole::Drums:
                    p.transientAttackDb = 1.5;
                    p.compStyle.ratio = 4.0; p.compStyle.attackMs = 15; p.compStyle.releaseMs = 120; p.compStyle.mix = 0.4;
                    p.compTargetGrDb = 6.0;
                    p.hpfAlways = true; p.hpfMaxHz = 25.0;
                    break;
                case StemRole::Bass:
                    p.compStyle.ratio = 3.0; p.compStyle.attackMs = 20; p.compStyle.releaseMs = 150;
                    p.compTargetGrDb = 3.0;
                    p.hpfAlways = true; p.hpfMaxHz = 28.0;
                    break;
                case StemRole::Guitar:
                    p.compStyle.ratio = 2.0; p.compStyle.attackMs = 15; p.compStyle.releaseMs = 150;
                    p.compTargetGrDb = 2.0;
                    p.hpfAlways = true; p.hpfMaxHz = 70.0;
                    p.maxResonanceCuts = 1; p.maxEqCutDb = 3.0;
                    break;
                case StemRole::Keys:
                case StemRole::Synth:
                case StemRole::Strings:
                    p.compStyle.ratio = 1.8; p.compStyle.attackMs = 25; p.compStyle.releaseMs = 200;
                    p.compTargetGrDb = 1.5;
                    p.hpfAlways = true; p.hpfMaxHz = 30.0;
                    break;
                default: break; // FX, ambience, other: cleanup only
            }
        }
    }
    p.id = std::string("stem.") + stemRoleName(r);
    p.loudnessMode = LoudnessMode::Unchanged;
    return p;
}

namespace {
/** Static mix of one stem into a stereo buffer (gain + constant-power pan). */
void addPanned(AudioBuffer& dst, const AudioBuffer& src, double gainDb, double pan)
{
    const double g = dbToGain(gainDb);
    const double th = (clampv(pan, -1.0, 1.0) + 1.0) * kPi / 4.0; // -3 dB at centre
    const double gl = std::cos(th) * std::sqrt(2.0), gr = std::sin(th) * std::sqrt(2.0);
    const size_t n = std::min(dst.numFrames(), src.numFrames());
    if (src.numChannels() == 1)
    {
        const double kl = g * gl / std::sqrt(2.0), kr = g * gr / std::sqrt(2.0);
        for (size_t i = 0; i < n; ++i)
        {
            dst.channel(0)[i] += float(src.channel(0)[i] * kl);
            dst.channel(1)[i] += float(src.channel(0)[i] * kr);
        }
    }
    else
    {
        // Stereo (or more): balance law, unity at centre.
        const double bl = std::min(1.0, 1.0 - pan), br = std::min(1.0, 1.0 + pan);
        for (size_t i = 0; i < n; ++i)
        {
            dst.channel(0)[i] += float(src.channel(0)[i] * g * bl);
            dst.channel(1)[i] += float(src.channel(1)[i] * g * br);
        }
    }
}

double glueThreshold(const AudioBuffer& b, const CompressorSettings& cs, double targetGr)
{
    const auto st = rmsLevelStats(b, cs.rmsWindowMs, -70.0, cs.sidechainHpfHz);
    double lo = st.p95 - 60, hi = st.p95;
    CompressorSettings s = cs;
    for (int i = 0; i < 50; ++i)
    {
        s.thresholdDb = 0.5 * (lo + hi);
        if (-Compressor::gainComputer(st.p95, s) > targetGr) lo = s.thresholdDb;
        else hi = s.thresholdDb;
    }
    return 0.5 * (lo + hi);
}
} // namespace

StemMixResult mixStems(const StemMixRequest& req, const Job& job)
{
    StemMixResult out;
    if (req.stems.empty() || !req.masterPreset) throw std::invalid_argument("mixStems: no stems or master preset");
    const double sr = req.stems.front().audio->sampleRate;
    size_t len = 0;
    for (auto& s : req.stems) len = std::max(len, size_t(std::llround(double(s.audio->numFrames()) * sr / s.audio->sampleRate)));
    const auto buses = req.buses.empty() ? defaultBuses() : req.buses;
    std::vector<AudioBuffer> busBuf(buses.size(), AudioBuffer(2, len, sr));
    char line[512];

    for (size_t i = 0; i < req.stems.size(); ++i)
    {
        const auto& st = req.stems[i];
        if (st.mute) continue;
        Job sub = job;
        sub.progress = [&job, i, n = req.stems.size()](double f, const std::string& w) {
            job.report(0.6 * (double(i) + f) / double(n), "Stem " + std::to_string(i + 1) + ": " + w);
        };
        AudioBuffer src = std::abs(st.audio->sampleRate - sr) > 1e-6 ? resample(*st.audio, sr, sub) : *st.audio;
        src.resizeFrames(len);
        AudioBuffer treated = src;
        if (st.process)
        {
            const PresetDef sp = stemPreset(st.role);
            ProcessRequest rq;
            rq.input = &src;
            rq.preset = &sp;
            rq.category = sp.category;
            rq.controls.sectionOn[int(Section::Output)] = false;
            ProcessResult pr = process(rq, sub);
            treated = std::move(pr.output);
            treated.resizeFrames(len);
            int on = 0;
            std::string stages;
            for (auto& s : pr.plan.stages)
                if (s.enabled) { ++on; stages += std::string(stageName(s.id)) + ", "; }
            std::snprintf(line, sizeof line, "Stem '%s' (%s): %d stages: %s", st.name.c_str(), stemRoleName(st.role), on, stages.c_str());
            out.log.push_back(line);
        }
        const int b = clampv(st.bus, 0, int(buses.size()) - 1);
        addPanned(busBuf[size_t(b)], treated, st.gainDb, st.pan);
        throwIfCancelled(job);
    }

    AudioBuffer master(2, len, sr);
    for (size_t b = 0; b < buses.size(); ++b)
    {
        AudioBuffer& bb = busBuf[b];
        if (bb.peak() <= 0.0f) continue;
        if (buses[b].glue)
        {
            CompressorSettings cs;
            cs.ratio = 2.0; cs.attackMs = 30.0; cs.releaseMs = 200.0; cs.kneeDb = 6.0;
            cs.detector = DetectorMode::Rms; cs.rmsWindowMs = 20.0; cs.sidechainHpfHz = 80.0; cs.stereoLink = 1.0;
            cs.thresholdDb = glueThreshold(bb, cs, 1.5);
            Compressor comp(cs);
            RenderOptions ro;
            ro.keepTail = false;
            bb = renderProcessor(comp, bb, ro, job);
            std::snprintf(line, sizeof line, "Bus '%s': glue 2:1, threshold %.1f dB (~1.5 dB GR on loud passages), max GR %.1f dB",
                          buses[b].name.c_str(), cs.thresholdDb, comp.grTrace()->maxValue());
            out.log.push_back(line);
        }
        const double g = dbToGain(buses[b].gainDb);
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < len; ++i) master.channel(c)[i] += float(bb.channel(c)[i] * g);
    }
    // Static mix uses the same gains, pans and bus gains (no processing): the A/B reference.
    AudioBuffer rawSum(2, len, sr);
    {
        AudioBuffer& raw = rawSum;
        for (size_t i = 0; i < req.stems.size(); ++i)
        {
            const auto& st = req.stems[i];
            if (st.mute) continue;
            AudioBuffer src = std::abs(st.audio->sampleRate - sr) > 1e-6 ? resample(*st.audio, sr) : *st.audio;
            src.resizeFrames(len);
            const int b = clampv(st.bus, 0, int(buses.size()) - 1);
            addPanned(raw, src, st.gainDb + buses[size_t(b)].gainDb, st.pan);
        }
    }

    Job mj = job;
    mj.progress = [&job](double f, const std::string& w) { job.report(0.6 + 0.4 * f, "Master: " + w); };
    ProcessRequest mrq;
    mrq.input = &master;
    mrq.preset = req.masterPreset;
    mrq.category = req.masterPreset->category;
    mrq.controls = req.masterControls;
    out.master = process(mrq, mj);
    // A/B against the static (unprocessed) mix.
    out.master.reference = resample(rawSum, out.master.plan.outputSampleRate);
    out.master.reference.resizeFrames(out.master.output.numFrames());
    out.master.referenceStats = measureLoudness(out.master.reference);
    if (out.master.referenceStats.integratedValid() && out.master.outputStats.integratedValid())
        out.master.matchGainDb = out.master.outputStats.integrated - out.master.referenceStats.integrated;
    out.master.removed = AudioBuffer(2, out.master.output.numFrames(), out.master.output.sampleRate);
    out.master.plan.log.insert(out.master.plan.log.begin(), out.log.begin(), out.log.end());
    return out;
}

} // namespace ac
