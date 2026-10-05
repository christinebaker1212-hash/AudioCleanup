// End-to-end validation of the adaptive pipeline: delivery targets, channel
// and timing preservation, silence/short clips, preview/export consistency,
// overrides, cancellation, batch consistency, user presets.

#include "TestFramework.h"
#include "TestSignals.h"

#include "AudioIO.h"

#include <ac/Pipeline.h>
#include <ac/Resampler.h>
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

TEST("formats: MP3 import (minimp3, gapless) and Ogg Vorbis round trip")
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
    // The LAME tag's encoder delay and padding are removed: exactly 2 s.
    CHECK(mp3.numFrames() == 88200);
    if (mp3.numFrames() > 44100)
    {
        const double a = ts::toneAmplitude(mp3.channel(0) + 22050, 22050, 1000, 44100);
        const double thd = ts::thdN(mp3.channel(0) + 22050, 22050, 1000, 44100);
        REPORT("MP3 decode: 1 kHz at %.2f dBFS (expected -6.02), THD+N %.1f dB", gainToDb(a), thd);
        CHECK_NEAR(gainToDb(a), -6.02, 0.3);
        CHECK_LE(thd, -40.0);
    }
    CHECK(af::isSupportedExtension("x.mp3") && af::isSupportedExtension("x.OGG") && af::isSupportedExtension("x.opus") &&
          !af::isSupportedExtension("x.txt"));

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

TEST("formats: hard MP3s (MPEG-2, VBR without Xing header, ID3v1), Ogg Opus, misnamed files")
{
    std::string err;
    af::SourceInfo info;
    // MPEG-2 (22.05 kHz) VBR with no Xing/Info header and ID3v2 + ID3v1 tags: a
    // length estimated from the first frame truncated these; MPEG-2 did not open.
    AudioBuffer a;
    CHECK_MSG(af::loadAudio(std::string(AF_TEST_DATA_DIR) + "/sine1k_mono_22k05_1s_vbr_noxing.mp3", a, err, &info), err);
    REPORT("VBR, no Xing: %s, %d ch, %.0f Hz, %.3f s", info.formatName.c_str(), a.numChannels(), a.sampleRate, a.durationSeconds());
    CHECK(a.numChannels() == 1 && a.sampleRate == 22050.0);
    // Fixture tones are at -24.08 dBFS. Without a tag the encoder delay and
    // padding stay (a few MPEG-2 frames), but nothing is cut off.
    CHECK_GE(a.durationSeconds(), 1.0);
    CHECK_LE(a.durationSeconds(), 1.1);
    if (a.numFrames() > 16000)
        CHECK_NEAR(gainToDb(ts::toneAmplitude(a.channel(0) + 5000, 11025, 1000, 22050)), -24.08, 0.3);

    // Ogg Opus, and the same file under an .ogg name.
    const auto opusPath = std::string(AF_TEST_DATA_DIR) + "/sine1k_stereo_48k_1s.opus";
    AudioBuffer o;
    CHECK_MSG(af::loadAudio(opusPath, o, err, &info), err);
    REPORT("Opus: %s, %d ch, %.0f Hz, %.3f s", info.formatName.c_str(), o.numChannels(), o.sampleRate, o.durationSeconds());
    CHECK(o.numChannels() == 2 && o.sampleRate == 48000.0);
    CHECK(o.numFrames() == 48000); // pre-skip and end trimming applied
    if (o.numFrames() == 48000)
    {
        const double lvl = gainToDb(ts::toneAmplitude(o.channel(1) + 12000, 24000, 1000, 48000));
        REPORT("Opus decode: 1 kHz at %.2f dBFS (expected -24.08)", lvl);
        CHECK_NEAR(lvl, -24.08, 0.3);
    }
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("af_test_misnamed");
    dir.createDirectory();
    const auto asOgg = dir.getChildFile("voice_note.ogg");
    CHECK(juce::File(opusPath).copyFileTo(asOgg));
    AudioBuffer o2;
    CHECK_MSG(af::loadAudio(asOgg.getFullPathName().toStdString(), o2, err), err);
    CHECK(o2.numFrames() == o.numFrames());

    // A WAV with an .mp3 name opens by content.
    AudioBuffer w = ts::sine(44100, 2, 0.5, 440, 0.25);
    af::ExportOptions ex;
    ex.bitDepth = 32;
    const auto wavPath = dir.getChildFile("tmp.wav").getFullPathName().toStdString();
    CHECK_MSG(af::saveAudio(wavPath, w, ex, err), err);
    const auto misnamed = dir.getChildFile("really_a_wav.mp3");
    CHECK(juce::File(juce::String(wavPath)).moveFileTo(misnamed));
    AudioBuffer w2;
    CHECK_MSG(af::loadAudio(misnamed.getFullPathName().toStdString(), w2, err), err);
    CHECK(w2.numFrames() == w.numFrames() && ts::maxAbsDiff(w, w2) == 0.0);

    // Garbage is refused with a message, not a crash.
    const auto junk = dir.getChildFile("junk.mp3");
    junk.replaceWithText("this is not audio at all");
    AudioBuffer j;
    CHECK(!af::loadAudio(junk.getFullPathName().toStdString(), j, err) && !err.empty());
    dir.deleteRecursively();
}

TEST("formats: MP3 export (LAME) round trip is gapless, aligned and level-exact")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("af_test_mp3");
    dir.createDirectory();
    std::string err;
    af::SourceInfo info;
    CHECK(af::mp3QualityOptions().size() >= 4);
    struct Case { double sr; int ch; int quality; double expectRate; };
    for (const Case c : { Case { 44100, 2, 0, 44100 }, Case { 48000, 2, 1, 48000 }, Case { 96000, 1, 0, 48000 }, Case { 44100, 1, 4, 44100 } })
    {
        AudioBuffer src = ts::pseudoMusic(c.sr, 4.0, 77, -16);
        if (c.ch == 1)
        {
            AudioBuffer m(1, src.numFrames(), src.sampleRate);
            for (size_t i = 0; i < src.numFrames(); ++i) m.channel(0)[i] = 0.5f * (src.channel(0)[i] + src.channel(1)[i]);
            src = m;
        }
        af::ExportOptions ex;
        ex.format = af::FileFormat::Mp3;
        ex.mp3QualityIndex = c.quality;
        const auto path = dir.getChildFile("out" + juce::String(int(c.sr)) + ".mp3").getFullPathName().toStdString();
        CHECK_MSG(af::saveAudio(path, src, ex, err), err);
        AudioBuffer back;
        CHECK_MSG(af::loadAudio(path, back, err, &info), err);
        const double expectFrames = double(src.numFrames()) * c.expectRate / c.sr;
        // Rates above 48 kHz are resampled first; compare with the band-limited source.
        const AudioBuffer ref = c.sr == c.expectRate ? src : resample(src, c.expectRate);
        REPORT("MP3 %s @ %.0f Hz %d ch -> %s, %.0f Hz, %zu frames (expected %.0f); level %.2f -> %.2f dBFS",
               af::mp3QualityOptions()[size_t(c.quality)].c_str(), c.sr, c.ch, info.formatName.c_str(), back.sampleRate,
               back.numFrames(), expectFrames, ts::rmsDb(ref), ts::rmsDb(back));
        CHECK(back.numChannels() == c.ch);
        CHECK(back.sampleRate == c.expectRate);
        CHECK_NEAR(double(back.numFrames()), expectFrames, 1.0); // gapless: no priming or padding
        // 192 kbps low-passes at ~19 kHz, where this synthetic signal still has energy.
        CHECK_NEAR(ts::rmsDb(back), ts::rmsDb(ref), c.quality == 4 ? 0.4 : 0.1);
        CHECK(ts::bestLag(ref.vec(0), back.vec(0), 3000) == 0);
    }
    // Five channels cannot be MP3: a clear refusal, no file left behind.
    AudioBuffer five(5, 4800, 48000);
    af::ExportOptions ex;
    ex.format = af::FileFormat::Mp3;
    const auto bad = dir.getChildFile("five.mp3");
    CHECK(!af::saveAudio(bad.getFullPathName().toStdString(), five, ex, err) && !bad.existsAsFile());
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

TEST("tempo: detects the beat of drum patterns, reports no pulse on noise")
{
    ts::Rng rng(77);
    for (double bpm : { 92.0, 128.0, 174.0 })
    {
        // Kick on every beat, hat on eighths, over a quiet noise bed.
        AudioBuffer x(2, size_t(SR * 20.0), SR);
        const double beat = 60.0 / bpm;
        for (size_t i = 0; i < x.numFrames(); ++i)
        {
            const double t = double(i) / SR;
            const double tb = std::fmod(t, beat), te = std::fmod(t, beat * 0.5);
            const double kick = 0.6 * std::exp(-tb * 30.0) * std::sin(2 * kPi * (50 + 80 * std::exp(-tb * 40)) * tb);
            const double hat = 0.15 * std::exp(-te * 80.0) * rng.gauss();
            const float v = float(kick + hat + 0.01 * rng.gauss());
            x.channel(0)[i] = x.channel(1)[i] = v;
        }
        const auto tp = estimateTempo(x);
        REPORT("%.0f BPM pattern -> %.1f BPM, confidence %.2f", bpm, tp.bpm, tp.confidence);
        // Half/double time is the same note-length grid for timing purposes.
        const double err = std::min({ std::abs(tp.bpm - bpm), std::abs(2 * tp.bpm - bpm), std::abs(tp.bpm - 2 * bpm) });
        CHECK_LE(err, 1.0);
        CHECK(tp.reliable());
    }
    AudioBuffer noise(2, size_t(SR * 20.0), SR);
    for (int c = 0; c < 2; ++c)
        for (auto& v : noise.vec(c)) v = float(0.1 * rng.gauss());
    const auto tn = estimateTempo(noise);
    REPORT("white noise -> %.1f BPM, confidence %.2f", tn.bpm, tn.confidence);
    CHECK(!tn.reliable());
}

TEST("tempo sync: compressor and limiter releases land on the note grid of the detected tempo")
{
    ts::Rng rng(5);
    const double bpm = 128.0, beat = 60.0 / 128.0;
    AudioBuffer x(2, size_t(SR * 16.0), SR);
    for (size_t i = 0; i < x.numFrames(); ++i)
    {
        const double t = double(i) / SR, tb = std::fmod(t, beat), te = std::fmod(t, beat * 0.5);
        const double kick = 0.5 * std::exp(-tb * 25.0) * std::sin(2 * kPi * (55 + 70 * std::exp(-tb * 40)) * tb);
        const double pad = 0.05 * std::sin(2 * kPi * 220 * t) + 0.04 * std::sin(2 * kPi * 330 * t);
        const float v = float(kick + pad + 0.12 * std::exp(-te * 70.0) * rng.gauss());
        x.channel(0)[i] = v;
        x.channel(1)[i] = float(v * 0.9 + 0.01 * rng.gauss());
    }
    UserControls uc;
    ProcessResult r = run(x, "music.warm", uc);
    const double rel = r.plan.settings.compressor.releaseMs, slow = r.plan.limiterSlowReleaseMs;
    auto onGrid = [&](double ms) {
        for (double m : { 0.125, 0.25, 0.5, 1.0, 2.0 })
            if (std::abs(ms - 60000.0 / bpm * m) < 1e-6 || std::abs(ms - 30000.0 / bpm * m) < 1e-6 || std::abs(ms - 120000.0 / bpm * m) < 1e-6)
                return true;
        return false;
    };
    uc.tempoSync = false;
    ProcessResult off = run(x, "music.warm", uc);
    REPORT("tempo %.1f BPM: compressor release %.1f ms (preset %.1f), limiter sustained %.1f ms (preset %.1f)", r.analysis.tempo.bpm, rel,
           off.plan.settings.compressor.releaseMs, slow, off.plan.limiterSlowReleaseMs);
    CHECK(r.analysis.tempo.reliable());
    CHECK(onGrid(rel));
    CHECK(onGrid(slow));
    CHECK(std::abs(rel - off.plan.settings.compressor.releaseMs) > 1.0); // actually moved onto the grid
    CHECK_NEAR(off.plan.settings.compressor.releaseMs, findPreset("music.warm")->compStyle.releaseMs, 1e-9);
    CHECK_LE(r.outputStats.truePeakDb, findPreset("music.warm")->ceilingDbTP + 0.1);
}

TEST("mid/side: width by frequency moves to the reference; side low-cut leaves the mid untouched")
{
    // Source: wide lows, narrow highs. Reference: the opposite (typical master).
    auto shape = [](const AudioBuffer& in, double loW, double hiW) {
        AudioBuffer o = in;
        Biquad lpS(BiquadCoeffs::lowpass(800, 0.707, SR)); // complementary split: hi = s - lo (no crossover notch)
        for (size_t i = 0; i < o.numFrames(); ++i)
        {
            const double m = 0.5 * (in.channel(0)[i] + in.channel(1)[i]), s = 0.5 * (in.channel(0)[i] - in.channel(1)[i]);
            const double lo = lpS.process(s);
            const double s2 = loW * lo + hiW * (s - lo);
            o.channel(0)[i] = float(m + s2);
            o.channel(1)[i] = float(m - s2);
        }
        return o;
    };
    const AudioBuffer base = ts::pseudoMusic(SR, 12.0, 909, -18);
    AudioBuffer ref = shape(base, 0.7, 1.3), src = shape(base, 1.3, 0.75);
    auto profile = std::make_shared<ReferenceProfile>(analyzeReference(ref, "ref"));
    UserControls uc;
    uc.reference = profile;
    uc.referenceAmount = 1.0;
    ProcessResult r = run(src, "music.transparent", uc);
    const auto hz = longTermSpectrum(src).bandHz;
    auto dev = [&](const AudioBuffer& b) {
        const auto sb = sideToMidByBand(b);
        double acc = 0;
        int n = 0;
        for (size_t i = 0; i < hz.size(); ++i)
            if (hz[i] >= 150 && hz[i] <= 10000 && sb[i] > -40 && profile->sideToMidBandDb[i] > -40)
            {
                acc += std::pow(sb[i] - profile->sideToMidBandDb[i], 2);
                ++n;
            }
        return n ? std::sqrt(acc / n) : 0.0;
    };
    const double before = dev(src), after = dev(r.output);
    REPORT("side/mid by band deviation from reference %.2f -> %.2f dB rms", before, after);
    CHECK_LE(after, 0.6 * before);

    // Side low-cut: mid identical below the cut, side reduced at 30 Hz.
    StereoSettings st;
    st.sideLowCutHz = 80.0;
    StereoProcessor sp(st);
    AudioBuffer x(2, size_t(SR * 2), SR);
    for (size_t i = 0; i < x.numFrames(); ++i)
    {
        const double t = double(i) / SR;
        const double m = 0.3 * std::sin(2 * kPi * 30 * t), s = 0.2 * std::sin(2 * kPi * 30 * t + 1.0);
        x.channel(0)[i] = float(m + s);
        x.channel(1)[i] = float(m - s);
    }
    AudioBuffer y = ts::render(sp, x);
    double midErr = 0, sideIn = 0, sideOut = 0;
    for (size_t i = size_t(SR * 0.5); i < x.numFrames(); ++i)
    {
        const double mi = 0.5 * (x.channel(0)[i] + x.channel(1)[i]), mo = 0.5 * (y.channel(0)[i] + y.channel(1)[i]);
        midErr = std::max(midErr, std::abs(mi - mo));
        sideIn += std::pow(0.5 * (x.channel(0)[i] - x.channel(1)[i]), 2);
        sideOut += std::pow(0.5 * (y.channel(0)[i] - y.channel(1)[i]), 2);
    }
    const double cut = 10 * std::log10(sideIn / sideOut);
    REPORT("side low-cut 80 Hz: 30 Hz side reduced %.1f dB (2nd-order theory %.1f dB), mid max error %.2g", cut,
           10 * std::log10(1 + std::pow(80.0 / 30.0, 4)), midErr);
    CHECK_GE(cut, 15.0);
    CHECK_LE(midErr, 1e-6);
}
