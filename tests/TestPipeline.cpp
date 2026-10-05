// End-to-end validation of the adaptive pipeline: delivery targets, channel
// and timing preservation, silence/short clips, preview/export consistency,
// overrides, cancellation, batch consistency, user presets.

#include "TestFramework.h"
#include "TestSignals.h"

#include "AudioIO.h"

#include <ac/Pipeline.h>
#include <ac/StemMixer.h>

#include <juce_core/juce_core.h>

using namespace ac;

namespace {
constexpr double SR = 48000.0;

ProcessResult run(const AudioBuffer& in, const std::string& preset, UserControls uc = {})
{
    ProcessRequest rq;
    rq.input = &in;
    rq.preset = findPreset(preset);
    rq.category = rq.preset->category;
    rq.controls = uc;
    return process(rq, {});
}

AudioBuffer materialFor(Category c, uint64_t seed)
{
    if (c == Category::Voice)
    {
        AudioBuffer v = ts::pseudoSpeech(SR, 12.0, seed, -26);
        ts::add(v, ts::noise(SR, 1, 12.0, -58, seed + 1, true));
        return v;
    }
    if (c == Category::Music) return ts::pseudoMusic(SR, 12.0, seed, -20);
    AudioBuffer s = ts::impacts(SR, 2, 3.0, 0.7, seed);
    ts::add(s, ts::noise(SR, 2, 3.0, -60, seed + 2));
    return s;
}
} // namespace

TEST("pipeline: every preset meets its true-peak ceiling and loudness target (or reports why not)")
{
    for (auto& p : allPresets())
    {
        AudioBuffer in = materialFor(p.category, 100 + uint64_t(&p - &allPresets()[0]));
        ProcessResult r = run(in, p.id);
        const double tp = r.outputStats.truePeakDb;
        const bool loudMode = p.loudnessMode != LoudnessMode::Peak && p.loudnessMode != LoudnessMode::Unchanged;
        REPORT("%-18s %-14s -> %s %.2f (target %.1f%s), TP %.2f (ceiling %.1f), LRA %.1f", p.id.c_str(), categoryName(p.category),
               loudnessModeName(r.plan.loudnessMode), r.outputLoudnessValue, p.targetLufs, r.targetReached ? "" : ", NOT reached: bound",
               tp, p.ceilingDbTP, r.outputStats.lra);
        CHECK_MSG(tp <= p.ceilingDbTP + 0.1, p.id);
        if (loudMode && r.targetReached) CHECK_NEAR(r.outputLoudnessValue, p.targetLufs, 0.3);
        if (!r.targetReached) CHECK_MSG(r.outputLoudnessValue < p.targetLufs, p.id + " flagged but loud enough");
        if (p.loudnessMode == LoudnessMode::Peak) CHECK_NEAR(tp, p.ceilingDbTP, 0.15);
        for (int c = 0; c < r.output.numChannels(); ++c)
            for (float v : r.output.vec(c)) CHECK_MSG(std::isfinite(v), "non-finite sample");
    }
}

TEST("pipeline: all sections off is an exact pass-through")
{
    AudioBuffer in = ts::pseudoMusic(SR, 4.0, 3);
    UserControls uc;
    for (bool& b : uc.sectionOn) b = false;
    ProcessResult r = run(in, "music.loud", uc);
    CHECK(r.output.numFrames() >= in.numFrames());
    CHECK(ts::maxAbsDiff(in, r.output, 0, in.numFrames()) == 0.0);
}

TEST("pipeline: channel configuration, length and timing preserved")
{
    for (int ch : { 1, 2, 5 })
    {
        AudioBuffer in = ts::impacts(SR, ch, 3.0, 0.5, 9);
        ProcessResult r = run(in, "sfx.transparent");
        CHECK(r.output.numChannels() == ch);
        CHECK(r.output.numFrames() >= in.numFrames());
        CHECK(r.output.numFrames() <= in.numFrames() + size_t(SR)); // bounded tail
        const int lag = ts::bestLag(in.vec(0), r.output.vec(0), 2000);
        REPORT("%d channels: lag %d samples, length %zu -> %zu", ch, lag, in.numFrames(), r.output.numFrames());
        CHECK(lag == 0);
    }
    // Full voice chain (denoise STFT, lookaheads, resampling to 44.1 kHz) stays aligned.
    AudioBuffer v = materialFor(Category::Voice, 5);
    UserControls uc;
    uc.outputSampleRate = 44100;
    ProcessResult r = run(v, "voice.studio", uc);
    const int lag = ts::bestLag(r.reference.vec(0), r.output.vec(0), 4000);
    REPORT("voice.studio 48k -> 44.1k: lag vs reference %d samples", lag);
    CHECK(std::abs(lag) <= 1);
    CHECK(r.output.sampleRate == 44100);
}

TEST("pipeline: silence and very short clips")
{
    AudioBuffer silence(2, size_t(SR), SR);
    ProcessResult r = run(silence, "music.loud");
    CHECK(r.output.peak() == 0.0f);
    AudioBuffer blip = ts::sine(SR, 1, 0.08, 1500, 0.3);
    for (size_t i = 0; i < blip.numFrames(); ++i) blip.channel(0)[i] *= float(std::exp(-double(i) / (0.01 * SR)));
    for (auto* id : { "sfx.punchy", "sfx.game", "voice.natural", "music.transparent" })
    {
        ProcessResult rb = run(blip, id);
        REPORT("80 ms blip, %s: mode %s, TP %.2f dBTP", id, loudnessModeName(rb.plan.loudnessMode), rb.outputStats.truePeakDb);
        CHECK(rb.output.numFrames() >= blip.numFrames());
        CHECK(rb.outputStats.truePeakDb <= findPreset(id)->ceilingDbTP + 0.1);
        CHECK(rb.outputStats.truePeakDb > -40.0);
    }
}

TEST("pipeline: preview/export consistency (float bit-exact, PCM = engine dither)")
{
    AudioBuffer in = materialFor(Category::Music, 7);
    ProcessResult r = run(in, "music.transparent");
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("af_test_io");
    dir.createDirectory();
    std::string err;
    af::ExportOptions f32;
    f32.bitDepth = 32;
    const std::string p32 = dir.getChildFile("out32.wav").getFullPathName().toStdString();
    CHECK(af::saveAudio(p32, r.output, f32, err));
    AudioBuffer back;
    CHECK(af::loadAudio(p32, back, err));
    CHECK(back.numFrames() == r.output.numFrames());
    CHECK(ts::maxAbsDiff(back, r.output) == 0.0);
    for (auto fmt : { af::FileFormat::Wav, af::FileFormat::Flac })
    {
        af::ExportOptions p24;
        p24.bitDepth = 24;
        p24.format = fmt;
        const std::string path = dir.getChildFile(fmt == af::FileFormat::Wav ? "out24.wav" : "out24.flac").getFullPathName().toStdString();
        CHECK(af::saveAudio(path, r.output, p24, err));
        AudioBuffer b24;
        CHECK(af::loadAudio(path, b24, err));
        const AudioBuffer expect = quantiseToFloat(r.output, 24, DitherType::Tpdf);
        REPORT("%s 24-bit round trip max diff vs engine quantiser: %.3g", fmt == af::FileFormat::Wav ? "WAV" : "FLAC", ts::maxAbsDiff(b24, expect));
        CHECK(ts::maxAbsDiff(b24, expect) < 1e-9);
    }
    dir.deleteRecursively();
}

TEST("pipeline: user overrides, macros and cancellation")
{
    AudioBuffer in = materialFor(Category::Voice, 21);
    UserControls uc;
    uc.overrides[StageId::Compressor] = Override::Off;
    uc.overrides[StageId::Dereverb] = Override::On;
    ProcessResult r = run(in, "voice.studio", uc);
    CHECK(!r.plan.stage(StageId::Compressor).enabled);
    CHECK(r.plan.stage(StageId::Compressor).reason.find("user") != std::string::npos);
    CHECK(r.plan.stage(StageId::Dereverb).enabled);
    // Dynamics macro at 0 disables the compressor/leveler automatically.
    UserControls z;
    z.dynamics = 0.0;
    ProcessResult r0 = run(in, "voice.studio", z);
    CHECK(!r0.plan.stage(StageId::Compressor).enabled);
    // Cancellation.
    std::atomic<bool> cancel{ true };
    Job job;
    job.cancel = &cancel;
    ProcessRequest rq;
    rq.input = &in;
    rq.preset = findPreset("voice.studio");
    bool threw = false;
    try { process(rq, job); } catch (const CancelledException&) { threw = true; }
    CHECK(threw);
}

TEST("pipeline: removed-noise audition signal is the exact cleanup difference")
{
    AudioBuffer in = materialFor(Category::Voice, 33);
    UserControls uc;
    uc.sectionOn[1] = uc.sectionOn[2] = uc.sectionOn[3] = false; // cleanup only
    ProcessResult r = run(in, "voice.rescue", uc);
    AudioBuffer sum = r.output;
    ts::add(sum, r.removed);
    CHECK(ts::maxAbsDiff(sum, in, 0, in.numFrames()) < 1e-6);
    REPORT("removed signal level %.1f dBFS (input noise -58 dBFS)", ts::rmsDb(r.removed));
}

TEST("batch consistency: pulls toward the group target, keeps a fraction of differences")
{
    const auto t = batchConsistencyTargets({ -10.0, -14.0, -20.0, -INFINITY }, -14.0, 0.5);
    CHECK_NEAR(t[0], -12.0, 1e-9);
    CHECK_NEAR(t[1], -14.0, 1e-9);
    CHECK_NEAR(t[2], -17.0, 1e-9);
    CHECK_NEAR(t[3], -14.0, 1e-9);
}

TEST("user presets: JSON round trip")
{
    PresetDef p = *findPreset("music.warm");
    p.id = "user.mywarm";
    p.name = "My \"Warm\" master";
    p.targetLufs = -10.5;
    p.targetOffsets = { { 100, 1.5 }, { 9000, -2 } };
    p.compStyle.attackMs = 17;
    const std::string js = presetToJson(p);
    PresetDef q;
    std::string err;
    CHECK(presetFromJson(js, q, &err));
    CHECK(q.id == p.id);
    CHECK(q.name == p.name);
    CHECK(q.category == Category::Music);
    CHECK_NEAR(q.targetLufs, -10.5, 1e-9);
    CHECK_NEAR(q.compStyle.attackMs, 17, 1e-9);
    CHECK(q.targetOffsets.size() == 2);
    CHECK_NEAR(q.targetOffsets[1].second, -2, 1e-9);
}

TEST("stem mixer: role-aware stems, buses, master; aligned with the static mix")
{
    AudioBuffer vox = ts::pseudoSpeech(SR, 8.0, 91, -24);
    ts::add(vox, ts::noise(SR, 1, 8.0, -62, 3, true));
    AudioBuffer music = ts::pseudoMusic(SR, 8.0, 92, -22);
    AudioBuffer drums = ts::impacts(SR, 1, 8.0, 0.5, 93);
    StemMixRequest rq;
    rq.stems.push_back({ &vox, "Lead Vox", guessRole("Lead Vox"), 0.0, 0.0, 0, false, true });
    rq.stems.push_back({ &music, "Keys", guessRole("Keys"), -3.0, 0.0, 1, false, true });
    rq.stems.push_back({ &drums, "Drums", guessRole("Drums"), -6.0, 0.2, 1, false, true });
    CHECK(rq.stems[0].role == StemRole::LeadVocal);
    CHECK(rq.stems[1].role == StemRole::Keys);
    CHECK(rq.stems[2].role == StemRole::Drums);
    rq.masterPreset = findPreset("music.transparent");
    StemMixResult r = mixStems(rq, {});
    for (auto& l : r.log) REPORT("%s", l.c_str());
    CHECK(r.master.output.numChannels() == 2);
    CHECK_LE(r.master.outputStats.truePeakDb, -1.0 + 0.1);
    const int lag = ts::bestLag(r.master.reference.vec(0), r.master.output.vec(0), 4000);
    REPORT("master %.2f LUFS, TP %.2f dBTP, lag vs static mix %d", r.master.outputStats.integrated, r.master.outputStats.truePeakDb, lag);
    CHECK(lag == 0);
}

TEST("formats: MP3 import (bundled decoder) and Ogg Vorbis round trip")
{
    // MP3 fixture: 2 s, 1 kHz sine at -6 dBFS, stereo, 44.1 kHz, 192 kbps (LAME).
    AudioBuffer mp3;
    std::string err;
    af::SourceInfo info;
    const std::string path = std::string(AF_TEST_DATA_DIR) + "/sine1k_stereo_44k1_2s.mp3";
    CHECK_MSG(af::loadAudio(path, mp3, err, &info), err);
    REPORT("MP3: %s, %d ch, %.0f Hz, %.3f s", info.formatName.c_str(), mp3.numChannels(), mp3.sampleRate, mp3.durationSeconds());
    CHECK(mp3.numChannels() == 2);
    CHECK(mp3.sampleRate == 44100.0);
    CHECK_NEAR(mp3.durationSeconds(), 2.0, 0.08); // encoder priming/padding
    if (mp3.numFrames() > 44100)
    {
        const double a = ts::toneAmplitude(mp3.channel(0) + 22050, 22050, 1000, 44100);
        const double thd = ts::thdN(mp3.channel(0) + 22050, 22050, 1000, 44100);
        REPORT("MP3 decode: 1 kHz at %.2f dBFS (expected -6.02), THD+N %.1f dB", gainToDb(a), thd);
        CHECK_NEAR(gainToDb(a), -6.02, 0.3);
        CHECK_LE(thd, -40.0);
    }
    CHECK(af::isSupportedExtension("x.mp3") && af::isSupportedExtension("x.OGG") && !af::isSupportedExtension("x.txt"));

    // Ogg Vorbis: encode a processed-like signal, decode, check level/length/spectrum.
    AudioBuffer src = ts::pseudoMusic(48000, 4.0, 5, -16);
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("af_test_ogg");
    dir.createDirectory();
    const std::string op = dir.getChildFile("out.ogg").getFullPathName().toStdString();
    af::ExportOptions ex;
    ex.format = af::FileFormat::Ogg;
    CHECK_MSG(af::saveAudio(op, src, ex, err), err);
    AudioBuffer back;
    CHECK_MSG(af::loadAudio(op, back, err, &info), err);
    REPORT("Ogg: %s, %zu -> %zu frames, %d ch; level %.2f -> %.2f dBFS", info.formatName.c_str(), src.numFrames(), back.numFrames(),
           back.numChannels(), ts::rmsDb(src), ts::rmsDb(back));
    CHECK(back.numChannels() == 2);
    CHECK(back.sampleRate == 48000.0);
    CHECK(std::llabs((long long)back.numFrames() - (long long)src.numFrames()) <= 2048);
    CHECK_NEAR(ts::rmsDb(back), ts::rmsDb(src), 0.3);
    CHECK(ts::bestLag(src.vec(0), back.vec(0), 2000) == 0);
    dir.deleteRecursively();
}

TEST("reference matching: tonal balance, loudness and width move to the reference (bounded)")
{
    // Reference = "professional" master; mix = same material darker, narrower, 9 dB quieter.
    AudioBuffer ref = ts::pseudoMusic(SR, 12.0, 123, -16);
    {
        // A real master peaks at or below its ceiling.
        LimiterSettings ls;
        ls.ceilingDbTP = -1.0;
        ls.inputGainDb = 3.0;
        TruePeakLimiter lim(ls);
        ref = ts::render(lim, ref);
    }
    AudioBuffer mix = ref;
    for (int c = 0; c < 2; ++c)
    {
        Biquad lp(BiquadCoeffs::lowpass(2500, 0.707, SR));
        for (auto& v : mix.vec(c)) v = float(0.55 * v + 0.45 * lp.process(v));
    }
    for (size_t i = 0; i < mix.numFrames(); ++i)
    {
        const double m = 0.5 * (mix.channel(0)[i] + mix.channel(1)[i]), s = 0.5 * (mix.channel(0)[i] - mix.channel(1)[i]) * 0.6;
        mix.channel(0)[i] = float((m + s) * dbToGain(-9));
        mix.channel(1)[i] = float((m - s) * dbToGain(-9));
    }
    auto profile = std::make_shared<ReferenceProfile>(analyzeReference(ref, "ref"));
    UserControls uc;
    uc.reference = profile;
    ProcessResult r = run(mix, "music.transparent", uc);
    double before = 0, after = 0;
    referenceDeviationRms(mix, *profile, before);
    referenceDeviationRms(r.output, *profile, after);
    const double smBefore = analyzeReference(mix).sideToMidDb, smAfter = analyzeReference(r.output).sideToMidDb;
    REPORT("tonal deviation %.2f -> %.2f dB rms; loudness %.2f (ref %.2f); side/mid %.1f -> %.1f dB (ref %.1f)", before, after,
           r.outputStats.integrated, profile->loudness.integrated, smBefore, smAfter, profile->sideToMidDb);
    CHECK_LE(after, 0.5 * before);
    CHECK_NEAR(r.outputStats.integrated, profile->loudness.integrated, 0.5);
    CHECK_GE(smAfter, smBefore + 1.5);
    CHECK_LE(r.outputStats.truePeakDb, findPreset("music.transparent")->ceilingDbTP + 0.1);
}

TEST("reference dynamics: LRA and short-term crest move toward a dense reference (closed loop, bounded)")
{
    // Source: wide macro dynamics (quiet and loud sections). Reference: the same
    // material compressed and limited hard, as a dense commercial master.
    AudioBuffer src = ts::pseudoMusic(SR, 48.0, 321, -20);
    for (int c = 0; c < src.numChannels(); ++c)
        for (size_t i = 0; i < src.numFrames(); ++i)
        {
            const double t = double(i) / SR;
            src.channel(c)[i] *= float(dbToGain(std::fmod(t, 8.0) < 4.0 ? -9.0 : 0.0));
        }
    AudioBuffer ref = src;
    {
        CompressorSettings cs;
        cs.ratio = 4.0;
        cs.attackMs = 10;
        cs.releaseMs = 300;
        cs.thresholdDb = -38;
        cs.detector = DetectorMode::Rms;
        cs.rmsWindowMs = 50;
        cs.maxGrDb = 30;
        Compressor comp(cs);
        ref = ts::render(comp, ref);
        const double li = measureLoudness(ref, false, false).integrated;
        LimiterSettings ls;
        ls.ceilingDbTP = -1.0;
        ls.inputGainDb = -9.0 - li;
        TruePeakLimiter lim(ls);
        ref = ts::render(lim, ref);
    }
    auto profile = std::make_shared<ReferenceProfile>(analyzeReference(ref, "dense"));
    UserControls uc;
    uc.reference = profile;
    uc.referenceAmount = 1.0;
    ProcessResult on = run(src, "music.transparent", uc);
    uc.matchReferenceDynamics = false;
    ProcessResult off = run(src, "music.transparent", uc);
    const double crestOn = shortTermCrestDb(on.output), crestOff = shortTermCrestDb(off.output);
    REPORT("reference LRA %.1f LU, crest %.1f dB, %.1f LUFS; matched LRA %.1f, crest %.1f, %.2f LUFS; without dynamics match LRA %.1f, crest %.1f, %.2f LUFS",
           profile->loudness.lra, profile->crestDb, profile->loudness.integrated, on.outputStats.lra, crestOn, on.outputStats.integrated,
           off.outputStats.lra, crestOff, off.outputStats.integrated);
    CHECK_LE(std::abs(on.outputStats.lra - profile->loudness.lra), 0.6 * std::abs(off.outputStats.lra - profile->loudness.lra));
    CHECK_LE(std::abs(on.outputStats.integrated - profile->loudness.integrated), 0.5);
    CHECK_LE(std::abs(crestOn - profile->crestDb), std::abs(crestOff - profile->crestDb) + 0.3);
    CHECK_LE(on.outputStats.truePeakDb, -1.0 + 0.1);
}
