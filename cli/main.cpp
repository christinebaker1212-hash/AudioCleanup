// AudioFinisher command-line renderer: same engine as the GUI, for batch work,
// automated listening sets and regression tests.

#include "AudioIO.h"

#include <ac/Pipeline.h>
#include <ac/Fft.h>
#include <ac/Resampler.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace ac;

namespace {

void usage()
{
    std::puts(
        "AudioFinisherCLI\n"
        "  presets                                   list presets\n"
        "  analyze <in> [--category voice|sfx|music]\n"
        "  process <in> <out> --preset <id> [options]\n"
        "  batch <outdir> <in...> --preset <id> [--consistency 0..1] [options]\n"
        "  compare <candidate> <reference>          measure a result against an approved reference\n"
        "options:\n"
        "  --lufs <v> --ceiling <dBTP> --mode unchanged|peak|integrated|momentary|shortterm\n"
        "  --sr <Hz> --bits 16|24|32 --format wav|flac|ogg|mp3 --dither none|tpdf|shaped\n"
        "  --ogg-quality 0..10   --mp3-quality 0..5 (0 = 320 kbps CBR, 1 = V0, 2 = 256, 3 = V2, 4 = 192, 5 = 128)\n"
        "  inputs: WAV, FLAC, AIFF, MP3/MP2, Ogg Vorbis, Ogg Opus (.ogg/.opus)\n"
        "  --cleanup <0..2> --tone <0..2> --dynamics <0..2> --tilt <dB>\n"
        "  --on a,b  --off a,b   force stages (keys: filter declip declick dehum denoise neural\n"
        "                        dereverb plosive deess eq dyneq resonance leveler expander transient\n"
        "                        compressor multiband saturation stereo limiter)\n"
        "  --no-cleanup --no-tone --no-dynamics --no-output\n"
        "  --noise-region <startSec>:<endSec>\n"
        "  --match <file> [--match-amount 0..1] [--no-match-loudness] [--no-match-dynamics]  match a reference track\n"
        "  --removed <file>  --reference <file> (loudness-matched original)  --plan (print plan)  --no-tempo-sync\n"
        "  --user-preset <json>  load a user preset file instead of --preset");
}

Category parseCategory(const std::string& s)
{
    if (s == "sfx" || s == "effect") return Category::SoundEffect;
    if (s == "music") return Category::Music;
    return Category::Voice;
}

struct Args
{
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = "") const
    {
        auto it = opt.find(k);
        return it == opt.end() ? d : it->second;
    }
};

Args parse(int argc, char** argv)
{
    Args a;
    for (int i = 2; i < argc; ++i)
    {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0)
        {
            const std::string k = s.substr(2);
            const bool flag = k == "plan" || k.rfind("no-", 0) == 0;
            if (!flag && i + 1 < argc) a.opt[k] = argv[++i];
            else a.opt[k] = "1";
        }
        else a.pos.push_back(s);
    }
    return a;
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ','))
        if (!t.empty()) out.push_back(t);
    return out;
}

bool controlsFromArgs(const Args& a, UserControls& uc, af::ExportOptions& ex, std::string& err)
{
    if (a.has("lufs")) uc.targetLufs = std::stod(a.get("lufs"));
    if (a.has("ceiling")) uc.ceilingDbTP = std::stod(a.get("ceiling"));
    if (a.has("no-tempo-sync")) uc.tempoSync = false;
    if (a.has("mode"))
    {
        const auto m = a.get("mode");
        if (m == "unchanged") uc.loudnessMode = LoudnessMode::Unchanged;
        else if (m == "peak") uc.loudnessMode = LoudnessMode::Peak;
        else if (m == "integrated") uc.loudnessMode = LoudnessMode::Integrated;
        else if (m == "momentary") uc.loudnessMode = LoudnessMode::MaxMomentary;
        else if (m == "shortterm") uc.loudnessMode = LoudnessMode::MaxShortTerm;
        else { err = "bad --mode"; return false; }
    }
    if (a.has("sr")) uc.outputSampleRate = std::stod(a.get("sr"));
    if (a.has("cleanup")) uc.cleanup = std::stod(a.get("cleanup"));
    if (a.has("tone")) uc.tone = std::stod(a.get("tone"));
    if (a.has("dynamics")) uc.dynamics = std::stod(a.get("dynamics"));
    if (a.has("tilt")) uc.tiltDb = std::stod(a.get("tilt"));
    uc.sectionOn[0] = !a.has("no-cleanup");
    uc.sectionOn[1] = !a.has("no-tone");
    uc.sectionOn[2] = !a.has("no-dynamics");
    uc.sectionOn[3] = !a.has("no-output");
    for (auto& k : split(a.get("on")))
    {
        auto id = stageFromKey(k);
        if (!id) { err = "unknown stage " + k; return false; }
        uc.overrides[*id] = Override::On;
    }
    for (auto& k : split(a.get("off")))
    {
        auto id = stageFromKey(k);
        if (!id) { err = "unknown stage " + k; return false; }
        uc.overrides[*id] = Override::Off;
    }
    ex.bitDepth = a.has("bits") ? std::stoi(a.get("bits")) : 24;
    const auto fmtName = a.get("format", "wav");
    ex.format = fmtName == "flac" ? af::FileFormat::Flac
              : fmtName == "ogg"  ? af::FileFormat::Ogg
              : fmtName == "mp3"  ? af::FileFormat::Mp3
                                  : af::FileFormat::Wav;
    if (a.has("ogg-quality")) ex.oggQualityIndex = std::stoi(a.get("ogg-quality"));
    if (a.has("mp3-quality")) ex.mp3QualityIndex = std::stoi(a.get("mp3-quality"));
    const auto d = a.get("dither", "tpdf");
    ex.dither = d == "none" ? DitherType::None : d == "shaped" ? DitherType::TpdfShaped : DitherType::Tpdf;
    return true;
}

const PresetDef* resolvePreset(const Args& a, PresetDef& storage, std::string& err)
{
    if (a.has("user-preset"))
    {
        std::ifstream f(a.get("user-preset"));
        std::stringstream ss;
        ss << f.rdbuf();
        if (!presetFromJson(ss.str(), storage, &err)) return nullptr;
        return &storage;
    }
    const PresetDef* p = findPreset(a.get("preset", "voice.natural"));
    if (!p) err = "unknown preset " + a.get("preset");
    return p;
}

int runProcess(const Args& a)
{
    if (a.pos.size() < 2) { usage(); return 2; }
    std::string err;
    PresetDef userP;
    const PresetDef* P = resolvePreset(a, userP, err);
    if (!P) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    AudioBuffer in;
    if (!af::loadAudio(a.pos[0], in, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    ProcessRequest rq;
    rq.input = &in;
    rq.preset = P;
    rq.category = P->category;
    af::ExportOptions ex;
    if (!controlsFromArgs(a, rq.controls, ex, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (a.has("match"))
    {
        AudioBuffer refAudio;
        if (!af::loadAudio(a.get("match"), refAudio, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::string nm = a.get("match");
        if (auto sl = nm.find_last_of("/\\"); sl != std::string::npos) nm = nm.substr(sl + 1);
        rq.controls.reference = std::make_shared<ReferenceProfile>(analyzeReference(refAudio, nm));
        if (a.has("match-amount")) rq.controls.referenceAmount = std::stod(a.get("match-amount"));
        rq.controls.matchReferenceLoudness = !a.has("no-match-loudness");
        rq.controls.matchReferenceDynamics = !a.has("no-match-dynamics");
    }
    if (a.has("noise-region"))
    {
        const auto r = a.get("noise-region");
        const auto c = r.find(':');
        const double s0 = std::stod(r.substr(0, c)), s1 = std::stod(r.substr(c + 1));
        rq.controls.noiseRegion = std::make_pair(size_t(s0 * in.sampleRate), size_t(s1 * in.sampleRate));
    }
    Job job;
    int lastPct = -1;
    job.progress = [&](double f, const std::string& w) {
        const int pct = int(f * 100);
        if (pct / 10 != lastPct / 10) std::fprintf(stderr, "  %3d%% %s\n", pct, w.c_str());
        lastPct = pct;
    };
    const auto t0 = std::chrono::steady_clock::now();
    ProcessResult r = process(rq, job);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (a.has("plan")) std::printf("%s\n", describePlan(r.plan).c_str());
    if (!a.has("format"))
    {
        // No --format: follow the output file's extension (out.mp3 -> MP3).
        const auto& o = a.pos[1];
        const auto dot = o.find_last_of('.');
        std::string e = dot == std::string::npos ? "" : o.substr(dot + 1);
        for (auto& ch : e) ch = char(std::tolower((unsigned char)ch));
        if (e == "flac") ex.format = af::FileFormat::Flac;
        else if (e == "ogg" || e == "oga") ex.format = af::FileFormat::Ogg;
        else if (e == "mp3") ex.format = af::FileFormat::Mp3;
    }
    if (!af::saveAudio(a.pos[1], r.output, ex, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (a.has("removed"))
    {
        af::ExportOptions fx;
        fx.bitDepth = 32;
        af::saveAudio(a.get("removed"), r.removed, fx, err);
    }
    if (a.has("reference"))
    {
        AudioBuffer ref = r.reference;
        ref.applyGain(dbToGain(r.matchGainDb));
        af::ExportOptions fx;
        fx.bitDepth = 32;
        af::saveAudio(a.get("reference"), ref, fx, err);
    }
    std::printf("%s -> %s  [%s]  in %.2f LUFS / %.2f dBTP  ->  out %.2f LUFS / %.2f dBTP / LRA %.1f  (%.2fx realtime)\n",
                a.pos[0].c_str(), a.pos[1].c_str(), P->name.c_str(), r.referenceStats.integrated, r.referenceStats.truePeakDb,
                r.outputStats.integrated, r.outputStats.truePeakDb, r.outputStats.lra, in.durationSeconds() / std::max(secs, 1e-6));
    return 0;
}

/** Objective comparison of a candidate render against an engineer-approved
    reference: alignment, loudness/dynamics/peak deltas, 1/3-octave balance
    difference after loudness matching, stereo image and residual level. */
int runCompare(const Args& a)
{
    if (a.pos.size() < 2) { usage(); return 2; }
    AudioBuffer cand, ref;
    std::string err;
    if (!af::loadAudio(a.pos[0], cand, err) || !af::loadAudio(a.pos[1], ref, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (std::abs(cand.sampleRate - ref.sampleRate) > 0.5) cand = resample(cand, ref.sampleRate);
    // Alignment by GCC-PHAT (whitened cross-correlation): robust when one
    // signal has been equalised, compressed, denoised or dereverberated.
    const auto mc = cand.mixdown(), mr = ref.mixdown();
    const int maxLag = int(0.5 * ref.sampleRate);
    int N = 1 << 12;
    const size_t span = std::min<size_t>(std::min(mc.size(), mr.size()), size_t(1) << 20);
    while (size_t(N) < 2 * span && N < (1 << 22)) N <<= 1;
    RealFft fft(N);
    std::vector<float> bufA(static_cast<size_t>(N), 0.0f), bufB(static_cast<size_t>(N), 0.0f);
    std::copy(mr.begin(), mr.begin() + long(span), bufA.begin());
    std::copy(mc.begin(), mc.begin() + long(span), bufB.begin());
    std::vector<std::complex<float>> A(static_cast<size_t>(N / 2 + 1)), B(static_cast<size_t>(N / 2 + 1));
    fft.forward(bufA.data(), A.data());
    fft.forward(bufB.data(), B.data());
    for (size_t k = 0; k < A.size(); ++k)
    {
        const std::complex<float> c = std::conj(A[k]) * B[k];
        const float m = std::abs(c);
        A[k] = m > 1e-20f ? c / m : std::complex<float>(0.0f, 0.0f);
    }
    fft.inverse(A.data(), bufA.data());
    int lag = 0;
    float best = -1e30f;
    for (int L = -maxLag; L <= maxLag; ++L)
    {
        const float v = bufA[size_t((L + N) % N)];
        if (v > best) { best = v; lag = L; }
    }
    AudioBuffer aligned(cand.numChannels(), ref.numFrames(), ref.sampleRate);
    for (int c = 0; c < cand.numChannels(); ++c)
        for (size_t i = 0; i < ref.numFrames(); ++i)
        {
            const long j = long(i) + lag;
            aligned.channel(c)[i] = j >= 0 && size_t(j) < cand.numFrames() ? cand.channel(c)[size_t(j)] : 0.0f;
        }
    const auto lc = measureLoudness(aligned), lr = measureLoudness(ref);
    std::printf("alignment: candidate offset %+d samples (%.2f ms)\n", lag, 1000.0 * lag / ref.sampleRate);
    std::printf("%-22s %12s %12s %10s\n", "metric", "candidate", "reference", "delta");
    auto row = [](const char* k, double c, double r) { std::printf("%-22s %12.2f %12.2f %+10.2f\n", k, c, r, c - r); };
    row("integrated LUFS", lc.integrated, lr.integrated);
    row("LRA LU", lc.lra, lr.lra);
    row("true peak dBTP", lc.truePeakDb, lr.truePeakDb);
    row("max short-term LUFS", lc.maxShortTerm, lr.maxShortTerm);
    row("PLR dB", lc.truePeakDb - lc.integrated, lr.truePeakDb - lr.integrated);
    // Loudness-match, then spectral balance and residual.
    const double g = lc.integratedValid() && lr.integratedValid() ? dbToGain(lr.integrated - lc.integrated) : 1.0;
    aligned.applyGain(g);
    const auto sc = longTermSpectrum(aligned), sr = longTermSpectrum(ref);
    std::printf("\n1/3-octave balance after loudness matching (candidate - reference, dB):\n");
    double worst = 0, sumSq = 0;
    int cnt = 0;
    for (size_t i = 0; i < std::min(sc.bandHz.size(), sr.bandHz.size()); ++i)
    {
        if (sr.bandAbsDb[i] < -120) continue;
        const double d = sc.bandAbsDb[i] - sr.bandAbsDb[i];
        std::printf("  %7.0f Hz %+6.2f%s", sc.bandHz[i], d, (cnt % 4 == 3) ? "\n" : "");
        worst = std::max(worst, std::abs(d));
        sumSq += d * d;
        ++cnt;
    }
    std::printf("\n  rms deviation %.2f dB, worst band %.2f dB\n", std::sqrt(sumSq / std::max(1, cnt)), worst);
    if (ref.numChannels() == 2 && aligned.numChannels() == 2)
    {
        auto corr = [](const AudioBuffer& b) {
            double ll = 0, rr = 0, lr2 = 0;
            for (size_t i = 0; i < b.numFrames(); ++i) { ll += double(b.channel(0)[i]) * b.channel(0)[i]; rr += double(b.channel(1)[i]) * b.channel(1)[i]; lr2 += double(b.channel(0)[i]) * b.channel(1)[i]; }
            return ll > 0 && rr > 0 ? lr2 / std::sqrt(ll * rr) : 1.0;
        };
        row("stereo correlation", corr(aligned), corr(ref));
    }
    AudioBuffer diff = aligned;
    double e = 0, er = 0;
    for (int c = 0; c < std::min(diff.numChannels(), ref.numChannels()); ++c)
        for (size_t i = 0; i < ref.numFrames(); ++i)
        {
            const double d = double(diff.channel(c)[i]) - ref.channel(c)[i];
            e += d * d;
            er += double(ref.channel(c)[i]) * ref.channel(c)[i];
        }
    std::printf("residual after alignment + loudness match: %.1f dB re reference (lower = closer; < -30 dB is near-identical)\n",
                powerToDb(e / std::max(er, 1e-30), -300));
    return 0;
}

int runBatch(const Args& a)
{
    if (a.pos.size() < 2) { usage(); return 2; }
    std::string err;
    PresetDef userP;
    const PresetDef* P = resolvePreset(a, userP, err);
    if (!P) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    const std::string outdir = a.pos[0];
    std::vector<std::string> files(a.pos.begin() + 1, a.pos.end());
    UserControls base;
    af::ExportOptions ex;
    if (!controlsFromArgs(a, base, ex, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    const bool consistency = a.has("consistency");
    const double preserve = consistency ? std::stod(a.get("consistency")) : 1.0;

    // Pass 1 (consistency): analyse every asset with the preset's metric.
    std::vector<AudioBuffer> ins(files.size());
    std::vector<double> metrics(files.size(), -INFINITY);
    const LoudnessMode mode = base.loudnessMode.value_or(P->loudnessMode);
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!af::loadAudio(files[i], ins[i], err)) { std::fprintf(stderr, "%s\n", err.c_str()); continue; }
        if (consistency) metrics[i] = loudnessMetric(ins[i], mode);
    }
    std::vector<double> targets(files.size(), base.targetLufs.value_or(P->targetLufs));
    if (consistency)
        targets = batchConsistencyTargets(metrics, base.targetLufs.value_or(P->targetLufs), preserve);
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (ins[i].empty()) continue;
        ProcessRequest rq;
        rq.input = &ins[i];
        rq.preset = P;
        rq.category = P->category;
        rq.controls = base;
        if (consistency) rq.controls.targetLufs = targets[i];
        ProcessResult r = process(rq, {});
        std::string name = files[i];
        const auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const auto dot = name.find_last_of('.');
        name = name.substr(0, dot) + af::extensionFor(ex.format);
        const std::string out = outdir + "/" + name;
        if (!af::saveAudio(out, r.output, ex, err)) std::fprintf(stderr, "%s\n", err.c_str());
        std::printf("%-40s in %7.2f -> target %7.2f -> out %7.2f (%s), TP %.2f\n", name.c_str(), metrics[i], targets[i],
                    r.outputLoudnessValue, loudnessModeName(mode), r.outputStats.truePeakDb);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    const Args a = parse(argc, argv);
    try
    {
        if (cmd == "presets")
        {
            for (auto& p : allPresets())
                std::printf("%-20s %-14s %-26s %s %.1f, ceiling %.1f dBTP\n", p.id.c_str(), categoryName(p.category), p.name.c_str(),
                            loudnessModeName(p.loudnessMode), p.targetLufs, p.ceilingDbTP);
            return 0;
        }
        if (cmd == "analyze")
        {
            if (a.pos.empty()) { usage(); return 2; }
            AudioBuffer in;
            std::string err;
            af::SourceInfo si;
            if (!af::loadAudio(a.pos[0], in, err, &si)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
            std::printf("%s (%s, %d bit%s)\n", a.pos[0].c_str(), si.formatName.c_str(), si.bitsPerSample, si.floatingPoint ? " float" : "");
            std::printf("%s", describe(analyze(in, parseCategory(a.get("category", "voice")))).c_str());
            return 0;
        }
        if (cmd == "process") return runProcess(a);
        if (cmd == "batch") return runBatch(a);
        if (cmd == "compare") return runCompare(a);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
