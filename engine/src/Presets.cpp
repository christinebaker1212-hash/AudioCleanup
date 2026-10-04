#include "ac/Presets.h"

#include <cstdio>
#include <map>
#include <sstream>

namespace ac {

const char* loudnessModeName(LoudnessMode m)
{
    switch (m)
    {
        case LoudnessMode::Unchanged: return "Unchanged";
        case LoudnessMode::Peak: return "Peak";
        case LoudnessMode::Integrated: return "Integrated";
        case LoudnessMode::MaxMomentary: return "Max momentary";
        case LoudnessMode::MaxShortTerm: return "Max short-term";
    }
    return "?";
}

namespace {

double interpLog(const std::vector<std::pair<double, double>>& pts, double hz)
{
    if (pts.empty()) return 0.0;
    if (hz <= pts.front().first) return pts.front().second;
    if (hz >= pts.back().first) return pts.back().second;
    for (size_t i = 1; i < pts.size(); ++i)
        if (hz <= pts[i].first)
        {
            const double t = std::log(hz / pts[i - 1].first) / std::log(pts[i].first / pts[i - 1].first);
            return pts[i - 1].second + t * (pts[i].second - pts[i - 1].second);
        }
    return pts.back().second;
}

CompressorSettings comp(double ratio, double attack, double release, double knee, DetectorMode det, double rmsMs,
                        double scHpf, double link = 1.0, double mix = 1.0, double lookahead = 0.0)
{
    CompressorSettings c;
    c.ratio = ratio;
    c.attackMs = attack;
    c.releaseMs = release;
    c.kneeDb = knee;
    c.detector = det;
    c.rmsWindowMs = rmsMs;
    c.sidechainHpfHz = scHpf;
    c.stereoLink = link;
    c.mix = mix;
    c.lookaheadMs = lookahead;
    return c;
}

std::vector<PresetDef> build()
{
    std::vector<PresetDef> v;

    // ============================== VOICE ==============================
    PresetDef base;
    base.category = Category::Voice;
    base.loudnessMode = LoudnessMode::Integrated;
    base.hpfAlways = true;
    base.clickMinPerMinute = 2.0;
    base.compStyle = comp(3.0, 8.0, 120.0, 6.0, DetectorMode::Rms, 8.0, 80.0);

    {
        PresetDef p = base;
        p.id = "voice.natural";
        p.name = "Natural Dialogue";
        p.description = "Transparent dialogue cleanup for film/doc/interview. Keeps room tone continuous, breaths and "
                        "consonants intact; gentle level riding, light compression, EBU R128 delivery.";
        p.targetLufs = -23.0; p.ceilingDbTP = -2.0; p.maxLimiterGrDb = 2.5;
        p.maxNoiseReductionDb = 10.0; p.targetNoiseFloorDb = -66.0; p.minSnrForNoNr = 50.0; p.denoiseStrength = 1.05;
        p.maxActiveLossDb = 0.75;
        p.sibilanceTargetDb = 9.0; p.maxDeEssDb = 4.0; p.maxPlosiveDb = 9.0;
        p.hpfMaxHz = 80.0; p.hpfOrder = 2;
        p.toneStrength = 0.3; p.maxEqBoostDb = 2.0; p.maxEqCutDb = 3.0;
        p.levelerRangeDb = 4.0; p.levelerStrength = 0.5;
        p.compStyle = comp(2.0, 15.0, 180.0, 8.0, DetectorMode::Rms, 10.0, 90.0);
        p.compTargetGrDb = 2.5;
        p.maxHighBoostDb = 1.5;
        v.push_back(p);
    }
    {
        PresetDef p = base;
        p.id = "voice.studio";
        p.name = "Studio Voiceover";
        p.description = "Clean, close, polished VO: quiet floor between phrases, controlled sibilance, consistent "
                        "level, -16 LUFS online delivery.";
        p.targetLufs = -16.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 4.0;
        p.maxNoiseReductionDb = 15.0; p.targetNoiseFloorDb = -72.0; p.denoiseStrength = 1.25;
        p.sibilanceTargetDb = 6.0; p.maxDeEssDb = 7.0;
        p.hpfMaxHz = 90.0; p.hpfOrder = 3;
        p.toneStrength = 0.5; p.maxEqBoostDb = 3.0; p.maxEqCutDb = 5.0;
        p.targetOffsets = { { 120, 0.5 }, { 10000, 1.0 }, { 14000, 1.5 } };
        p.maxResonanceCuts = 2;
        p.boomRangeDb = 3.0;
        p.levelerRangeDb = 6.0; p.levelerStrength = 0.6;
        p.maxNoiseGateDb = 6.0;
        p.compStyle = comp(3.0, 6.0, 100.0, 6.0, DetectorMode::Rms, 8.0, 90.0);
        p.compTargetGrDb = 4.0;
        v.push_back(p);
    }
    {
        PresetDef p = base;
        p.id = "voice.broadcast";
        p.name = "Broadcast Presence";
        p.description = "Forward, intelligible, dense voice for radio/TV promos: presence lift, firm multiband "
                        "control, tight de-essing; EBU R128 -23 LUFS / -1 dBTP by default.";
        p.targetLufs = -23.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 4.0;
        p.maxNoiseReductionDb = 14.0; p.targetNoiseFloorDb = -70.0; p.denoiseStrength = 1.2;
        p.sibilanceTargetDb = 5.0; p.maxDeEssDb = 8.0;
        p.hpfMaxHz = 100.0; p.hpfOrder = 4;
        p.toneStrength = 0.55; p.maxEqBoostDb = 4.0; p.maxEqCutDb = 5.0;
        p.targetOffsets = { { 200, -1.0 }, { 300, -1.0 }, { 2500, 2.0 }, { 4500, 2.0 }, { 8000, 0.5 } };
        p.maxResonanceCuts = 2;
        p.harshnessRangeDb = 3.0; p.boomRangeDb = 3.0;
        p.levelerRangeDb = 8.0; p.levelerStrength = 0.8;
        p.compStyle = comp(4.0, 5.0, 80.0, 6.0, DetectorMode::Rms, 5.0, 100.0);
        p.compTargetGrDb = 5.0;
        p.multibandDefault = true; p.mbTargetGrDb[0] = 3.0; p.mbTargetGrDb[1] = 2.0; p.mbTargetGrDb[2] = 3.0;
        p.mbXoverLowHz = 200.0; p.mbXoverHighHz = 3000.0;
        v.push_back(p);
    }
    {
        PresetDef p = base;
        p.id = "voice.warm";
        p.name = "Warm/Intimate";
        p.description = "Close, soft, intimate read: low-mid body, smoothed top, gentle tape-style saturation, "
                        "slow compression that keeps breath detail.";
        p.targetLufs = -18.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 3.0;
        p.maxNoiseReductionDb = 12.0; p.targetNoiseFloorDb = -70.0; p.denoiseStrength = 1.15;
        p.sibilanceTargetDb = 4.0; p.maxDeEssDb = 8.0;
        p.hpfMaxHz = 70.0; p.hpfOrder = 2;
        p.toneStrength = 0.45; p.maxEqBoostDb = 3.0; p.maxEqCutDb = 4.0;
        p.targetOffsets = { { 150, 1.5 }, { 250, 1.5 }, { 3500, -1.5 }, { 6000, -2.0 }, { 12000, -2.5 } };
        p.saturationDriveDb = 3.0; p.saturationAsymmetry = 0.12; p.saturationMix = 0.7;
        p.levelerRangeDb = 5.0; p.levelerStrength = 0.6;
        p.compStyle = comp(2.5, 25.0, 250.0, 10.0, DetectorMode::Rms, 20.0, 80.0);
        p.compTargetGrDb = 3.5;
        p.maxHighBoostDb = 0.5; p.maxLowCutDb = 1.0;
        v.push_back(p);
    }
    {
        PresetDef p = base;
        p.id = "voice.rescue";
        p.name = "Noisy Recording Rescue";
        p.description = "Maximum intelligibility from poor recordings: deep adaptive noise reduction plus the bundled "
                        "neural speech enhancer, hum/click repair, optional dereverb, intelligibility EQ.";
        p.targetLufs = -18.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 5.0;
        p.maxNoiseReductionDb = 24.0; p.targetNoiseFloorDb = -72.0; p.minSnrForNoNr = 60.0; p.denoiseStrength = 1.5;
        p.maxActiveLossDb = 2.0;
        p.neuralBelowSnrDb = 30.0; p.neuralMix = 0.85;
        p.sibilanceTargetDb = 7.0; p.maxDeEssDb = 6.0;
        p.hpfMaxHz = 110.0; p.hpfOrder = 4;
        p.clickMinPerMinute = 1.0;
        p.dereverbAutoAboveRt60 = 0.7; p.dereverbMaxDb = 10.0;
        p.toneStrength = 0.6; p.maxEqBoostDb = 4.0; p.maxEqCutDb = 6.0;
        p.targetOffsets = { { 250, -1.0 }, { 3000, 1.5 } };
        p.maxResonanceCuts = 3;
        p.maxNoiseGateDb = 8.0;
        p.levelerRangeDb = 10.0; p.levelerStrength = 0.85;
        p.compStyle = comp(3.0, 6.0, 120.0, 6.0, DetectorMode::Rms, 8.0, 120.0);
        p.compTargetGrDb = 4.0;
        v.push_back(p);
    }

    // =========================== SOUND EFFECTS ===========================
    PresetDef sfx;
    sfx.category = Category::SoundEffect;
    sfx.hpfAlways = false;
    sfx.hpfMaxHz = 20.0;
    sfx.hpfOrder = 2;
    sfx.useTargetCurve = false;
    sfx.denoiseStrength = 1.1;
    sfx.maxActiveLossDb = 1.0;
    sfx.clickThreshold = 8.0;
    sfx.clickMinPerMinute = 2.0;
    sfx.sibilanceTargetDb = 99.0;
    sfx.compStyle = comp(2.0, 10.0, 150.0, 6.0, DetectorMode::Peak, 1.0, 0.0);
    {
        PresetDef p = sfx;
        p.id = "sfx.transparent";
        p.name = "Transparent Cleanup";
        p.description = "Technical repair only (DC, hum, clicks, moderate noise) with peak normalisation; tone, "
                        "dynamics, attacks and tails untouched.";
        p.loudnessMode = LoudnessMode::Peak; p.targetLufs = -1.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 0.5;
        p.maxNoiseReductionDb = 8.0; p.targetNoiseFloorDb = -75.0; p.minSnrForNoNr = 50.0;
        p.toneStrength = 0.0; p.maxEqBoostDb = 0.0; p.maxEqCutDb = 0.0;
        v.push_back(p);
    }
    {
        PresetDef p = sfx;
        p.id = "sfx.punchy";
        p.name = "Punchy Impact";
        p.description = "Bigger hits: transient emphasis, parallel compression for body, low-end and presence "
                        "lift, saturation for density; tails preserved.";
        p.loudnessMode = LoudnessMode::MaxMomentary; p.targetLufs = -14.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 5.0;
        p.limiterLookaheadMs = 1.0; p.limiterReleaseMs = 30.0;
        p.maxNoiseReductionDb = 8.0; p.targetNoiseFloorDb = -70.0;
        p.toneStrength = 0.0; p.maxEqBoostDb = 3.0; p.maxEqCutDb = 3.0;
        p.targetOffsets = { { 60, 2.0 }, { 100, 1.5 }, { 400, -1.0 }, { 3500, 1.5 } };
        p.useTargetCurve = false;
        p.saturationDriveDb = 4.0; p.saturationAsymmetry = 0.05; p.saturationMix = 0.6;
        p.transientAttackDb = 4.0; p.transientSustainDb = 0.0;
        p.compStyle = comp(4.0, 12.0, 120.0, 6.0, DetectorMode::Rms, 5.0, 0.0, 1.0, 0.4);
        p.compTargetGrDb = 8.0;
        v.push_back(p);
    }
    {
        PresetDef p = sfx;
        p.id = "sfx.foley";
        p.name = "Detailed Foley";
        p.description = "Close, detailed Foley: quiet floor between moves (bounded downward expansion), "
                        "resonance control, gentle detail lift, light transient definition.";
        p.loudnessMode = LoudnessMode::MaxMomentary; p.targetLufs = -18.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 2.0;
        p.maxNoiseReductionDb = 14.0; p.targetNoiseFloorDb = -75.0; p.denoiseStrength = 1.25;
        p.maxEqBoostDb = 2.5; p.maxEqCutDb = 4.0;
        p.targetOffsets = { { 300, -1.0 }, { 6000, 1.5 }, { 10000, 1.5 } };
        p.maxResonanceCuts = 2;
        p.maxNoiseGateDb = 8.0;
        p.transientAttackDb = 1.5;
        p.hpfAlways = true; p.hpfMaxHz = 35.0; p.hpfOrder = 2;
        v.push_back(p);
    }
    {
        PresetDef p = sfx;
        p.id = "sfx.ambience";
        p.name = "Ambience Preservation";
        p.description = "Backgrounds and room tones: only clear defects (hum, clicks, DC) are fixed; noise "
                        "reduction is minimal and low-frequency air is preserved; stereo image untouched.";
        p.loudnessMode = LoudnessMode::Integrated; p.targetLufs = -26.0; p.ceilingDbTP = -2.0; p.maxLimiterGrDb = 1.0;
        p.maxNoiseReductionDb = 4.0; p.targetNoiseFloorDb = -85.0; p.minSnrForNoNr = 25.0; p.denoiseStrength = 1.0;
        p.maxActiveLossDb = 0.5;
        p.toneStrength = 0.0; p.maxEqBoostDb = 0.0; p.maxEqCutDb = 0.0;
        p.clickThreshold = 9.0;
        v.push_back(p);
    }
    {
        PresetDef p = sfx;
        p.id = "sfx.game";
        p.name = "Game Asset Consistency";
        p.description = "Uniform delivery for game assets: consistent peak loudness, true-peak ceiling and gentle "
                        "spectral normalisation. Batch mode aligns a set while keeping intended differences.";
        p.loudnessMode = LoudnessMode::MaxMomentary; p.targetLufs = -16.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 3.0;
        p.maxNoiseReductionDb = 10.0; p.targetNoiseFloorDb = -72.0;
        p.toneStrength = 0.0; p.maxEqBoostDb = 2.0; p.maxEqCutDb = 3.0;
        p.maxResonanceCuts = 1;
        p.compStyle = comp(2.0, 5.0, 100.0, 6.0, DetectorMode::Peak, 1.0, 40.0);
        p.compTargetGrDb = 2.0;
        p.hpfAlways = true; p.hpfMaxHz = 25.0;
        v.push_back(p);
    }

    // =============================== MUSIC ===============================
    PresetDef mu;
    mu.category = Category::Music;
    mu.denoiseDefault = false;
    mu.hpfAlways = false;
    mu.hpfMaxHz = 18.0;
    mu.hpfOrder = 2;
    mu.sibilanceTargetDb = 99.0;
    mu.clickThreshold = 9.0;
    mu.clickMinPerMinute = 6.0;
    mu.humMaxDepthDb = 15.0;
    mu.bassMonoOnPhaseIssues = true;
    mu.limiterLookaheadMs = 3.0;
    mu.limiterReleaseMs = 80.0;
    mu.compStyle = comp(1.5, 30.0, 300.0, 10.0, DetectorMode::Rms, 30.0, 60.0);
    {
        PresetDef p = mu;
        p.id = "music.transparent";
        p.name = "Transparent Master";
        p.description = "Minimal-intervention master: small broad tonal correction, very gentle glue, clean "
                        "true-peak limiting to -14 LUFS.";
        p.targetLufs = -14.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 3.0;
        p.toneStrength = 0.3; p.maxEqBoostDb = 1.5; p.maxEqCutDb = 2.0;
        p.compTargetGrDb = 1.5;
        v.push_back(p);
    }
    {
        PresetDef p = mu;
        p.id = "music.warm";
        p.name = "Warm/Glue";
        p.description = "Bus-compressor glue (slow attack, auto-ish release), subtle tape saturation and a slightly "
                        "darker, fuller balance.";
        p.targetLufs = -12.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 4.0;
        p.toneStrength = 0.4; p.maxEqBoostDb = 2.0; p.maxEqCutDb = 2.5;
        p.targetOffsets = { { 120, 1.0 }, { 250, 0.5 }, { 4000, -0.5 }, { 12000, -1.0 } };
        p.saturationDriveDb = 3.0; p.saturationAsymmetry = 0.08; p.saturationMix = 0.5;
        p.compStyle = comp(2.0, 30.0, 250.0, 8.0, DetectorMode::Rms, 20.0, 80.0);
        p.compTargetGrDb = 2.5;
        p.maxHighBoostDb = 0.5; p.maxLowCutDb = 1.0;
        v.push_back(p);
    }
    {
        PresetDef p = mu;
        p.id = "music.punchy";
        p.name = "Punchy";
        p.description = "Impact-forward master: controlled low end, transient-friendly compression (slower attack), "
                        "mild transient emphasis, presence.";
        p.targetLufs = -11.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 5.0;
        p.toneStrength = 0.4; p.maxEqBoostDb = 2.0; p.maxEqCutDb = 2.5;
        p.targetOffsets = { { 60, 1.0 }, { 300, -0.5 }, { 4000, 0.8 } };
        p.boomRangeDb = 2.0;
        p.transientAttackDb = 1.5;
        p.compStyle = comp(2.5, 40.0, 150.0, 6.0, DetectorMode::Rms, 10.0, 90.0);
        p.compTargetGrDb = 3.0;
        v.push_back(p);
    }
    {
        PresetDef p = mu;
        p.id = "music.loud";
        p.name = "Loud/Dense";
        p.description = "Competitive loudness: multiband density, soft saturation before the limiter, firmer "
                        "limiting within a hard gain-reduction bound.";
        p.targetLufs = -9.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 7.0;
        p.limiterReleaseMs = 50.0;
        p.toneStrength = 0.4; p.maxEqBoostDb = 2.0; p.maxEqCutDb = 3.0;
        p.targetOffsets = { { 50, 0.8 }, { 10000, 0.8 } };
        p.harshnessRangeDb = 2.0;
        p.saturationDriveDb = 6.0; p.saturationAsymmetry = 0.0; p.saturationMix = 1.0;
        p.compStyle = comp(2.0, 20.0, 200.0, 8.0, DetectorMode::Rms, 15.0, 80.0);
        p.compTargetGrDb = 2.5;
        p.multibandDefault = true; p.mbTargetGrDb[0] = 3.0; p.mbTargetGrDb[1] = 2.5; p.mbTargetGrDb[2] = 3.0;
        v.push_back(p);
    }
    {
        PresetDef p = mu;
        p.id = "music.dynamic";
        p.name = "Dynamic/Open";
        p.description = "For acoustic, jazz and classical: preserves crest factor and micro-dynamics; minimal "
                        "compression, peak-safe delivery at -16 LUFS.";
        p.targetLufs = -16.0; p.ceilingDbTP = -1.0; p.maxLimiterGrDb = 1.5;
        p.toneStrength = 0.25; p.maxEqBoostDb = 1.0; p.maxEqCutDb = 1.5;
        p.compStyle = comp(1.3, 50.0, 400.0, 12.0, DetectorMode::Rms, 50.0, 60.0);
        p.compTargetGrDb = 0.0;
        p.maxHighBoostDb = 1.0;
        v.push_back(p);
    }
    return v;
}

} // namespace

const std::vector<PresetDef>& allPresets()
{
    static const std::vector<PresetDef> v = build();
    return v;
}

const PresetDef* findPreset(const std::string& id)
{
    for (auto& p : allPresets())
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<const PresetDef*> presetsFor(Category c)
{
    std::vector<const PresetDef*> out;
    for (auto& p : allPresets())
        if (p.category == c) out.push_back(&p);
    return out;
}

double categoryTargetDb(Category c, double hz)
{
    // Long-term average spectrum shapes (relative dB). Voice follows the
    // shape of published LTASS data (peak 250-500 Hz, ~-6 dB/oct above 1 kHz);
    // music follows the average commercial-mix tilt (~-4.5 dB/oct above 200 Hz).
    static const std::vector<std::pair<double, double>> voice = {
        { 40, -24 }, { 63, -14 }, { 100, -5 }, { 160, 0 }, { 250, 2 }, { 500, 2 }, { 1000, 0 },
        { 2000, -5 }, { 4000, -10 }, { 8000, -16 }, { 12000, -22 }, { 16000, -30 } };
    static const std::vector<std::pair<double, double>> music = {
        { 30, 2 }, { 60, 4 }, { 100, 4 }, { 200, 2 }, { 400, -1.5 }, { 800, -4.5 }, { 1600, -7.5 },
        { 3200, -11 }, { 6400, -15 }, { 10000, -19 }, { 16000, -27 } };
    switch (c)
    {
        case Category::Voice: return interpLog(voice, hz);
        case Category::Music: return interpLog(music, hz);
        case Category::SoundEffect: return 0.0;
    }
    return 0.0;
}

// ------------------------------------------------------------------ JSON
namespace {
std::string esc(const std::string& s)
{
    std::string o;
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\') o += '\\';
        if (ch == '\n') { o += "\\n"; continue; }
        o += ch;
    }
    return o;
}

/** Minimal flat JSON object reader: string / number / bool / [[a,b],...] values. */
struct FlatJson
{
    std::map<std::string, std::string> v;
    bool parse(const std::string& s, std::string* err)
    {
        size_t i = 0;
        auto ws = [&] { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; };
        auto str = [&](std::string& out) {
            if (s[i] != '"') return false;
            ++i;
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                {
                    ++i;
                    out += s[i] == 'n' ? '\n' : s[i];
                }
                else out += s[i];
                ++i;
            }
            ++i;
            return true;
        };
        ws();
        if (i >= s.size() || s[i] != '{') { if (err) *err = "expected {"; return false; }
        ++i;
        while (true)
        {
            ws();
            if (i < s.size() && s[i] == '}') return true;
            std::string key;
            if (!str(key)) { if (err) *err = "expected key"; return false; }
            ws();
            if (s[i] != ':') { if (err) *err = "expected :"; return false; }
            ++i;
            ws();
            std::string val;
            if (s[i] == '"') str(val);
            else
            {
                int depth = 0;
                while (i < s.size())
                {
                    if (s[i] == '[') ++depth;
                    if (s[i] == ']') --depth;
                    if (depth == 0 && (s[i] == ',' || s[i] == '}')) break;
                    val += s[i++];
                }
            }
            v[key] = val;
            ws();
            if (i < s.size() && s[i] == ',') ++i;
            if (i >= s.size()) { if (err) *err = "unterminated"; return false; }
        }
    }
    double num(const std::string& k, double d) const
    {
        auto it = v.find(k);
        return it == v.end() ? d : std::atof(it->second.c_str());
    }
    std::string text(const std::string& k, const std::string& d) const
    {
        auto it = v.find(k);
        return it == v.end() ? d : it->second;
    }
};

#define AC_NUM_FIELDS(X)                                                                                              \
    X(targetLufs) X(ceilingDbTP) X(maxLimiterGrDb) X(limiterLookaheadMs) X(limiterReleaseMs) X(maxNoiseReductionDb)   \
    X(targetNoiseFloorDb) X(minSnrForNoNr) X(denoiseStrength) X(maxActiveLossDb) X(neuralBelowSnrDb) X(neuralMix)     \
    X(sibilanceTargetDb) X(maxDeEssDb) X(maxPlosiveDb) X(hpfMaxHz) X(clickThreshold) X(clickMinPerMinute)             \
    X(humMaxDepthDb) X(dereverbAutoAboveRt60) X(dereverbMaxDb) X(maxNoiseGateDb) X(toneStrength) X(maxEqBoostDb)      \
    X(maxEqCutDb) X(maxHighBoostDb) X(maxLowCutDb) X(harshnessRangeDb) X(boomRangeDb) X(saturationDriveDb) X(saturationAsymmetry) X(saturationMix)     \
    X(levelerRangeDb) X(levelerStrength) X(transientAttackDb) X(transientSustainDb) X(compTargetGrDb)                  \
    X(mbXoverLowHz) X(mbXoverHighHz)
} // namespace

std::string presetToJson(const PresetDef& p)
{
    std::ostringstream o;
    o.precision(10);
    o << "{\n  \"id\": \"" << esc(p.id) << "\",\n  \"name\": \"" << esc(p.name) << "\",\n  \"description\": \""
      << esc(p.description) << "\",\n  \"category\": " << int(p.category) << ",\n  \"loudnessMode\": " << int(p.loudnessMode)
      << ",\n  \"hpfOrder\": " << p.hpfOrder << ",\n  \"denoiseDefault\": " << (p.denoiseDefault ? 1 : 0)
      << ",\n  \"hpfAlways\": " << (p.hpfAlways ? 1 : 0) << ",\n  \"useTargetCurve\": " << (p.useTargetCurve ? 1 : 0)
      << ",\n  \"maxResonanceCuts\": " << p.maxResonanceCuts << ",\n  \"multibandDefault\": " << (p.multibandDefault ? 1 : 0)
      << ",\n  \"bassMonoOnPhaseIssues\": " << (p.bassMonoOnPhaseIssues ? 1 : 0);
#define AC_W(f) o << ",\n  \"" #f "\": " << p.f;
    AC_NUM_FIELDS(AC_W)
#undef AC_W
    const auto& c = p.compStyle;
    o << ",\n  \"compRatio\": " << c.ratio << ",\n  \"compAttackMs\": " << c.attackMs << ",\n  \"compReleaseMs\": "
      << c.releaseMs << ",\n  \"compKneeDb\": " << c.kneeDb << ",\n  \"compDetectorRms\": "
      << (c.detector == DetectorMode::Rms ? 1 : 0) << ",\n  \"compRmsMs\": " << c.rmsWindowMs << ",\n  \"compScHpfHz\": "
      << c.sidechainHpfHz << ",\n  \"compLink\": " << c.stereoLink << ",\n  \"compMix\": " << c.mix
      << ",\n  \"compLookaheadMs\": " << c.lookaheadMs;
    o << ",\n  \"mbTarget\": [" << p.mbTargetGrDb[0] << "," << p.mbTargetGrDb[1] << "," << p.mbTargetGrDb[2] << "]";
    o << ",\n  \"targetOffsets\": [";
    for (size_t i = 0; i < p.targetOffsets.size(); ++i)
        o << (i ? "," : "") << "[" << p.targetOffsets[i].first << "," << p.targetOffsets[i].second << "]";
    o << "]\n}\n";
    return o.str();
}

bool presetFromJson(const std::string& json, PresetDef& p, std::string* error)
{
    FlatJson j;
    if (!j.parse(json, error)) return false;
    p.id = j.text("id", p.id);
    p.name = j.text("name", p.name);
    p.description = j.text("description", p.description);
    p.category = Category(int(j.num("category", int(p.category))));
    p.loudnessMode = LoudnessMode(int(j.num("loudnessMode", int(p.loudnessMode))));
    p.hpfOrder = int(j.num("hpfOrder", p.hpfOrder));
    p.denoiseDefault = j.num("denoiseDefault", p.denoiseDefault) != 0;
    p.hpfAlways = j.num("hpfAlways", p.hpfAlways) != 0;
    p.useTargetCurve = j.num("useTargetCurve", p.useTargetCurve) != 0;
    p.maxResonanceCuts = int(j.num("maxResonanceCuts", p.maxResonanceCuts));
    p.multibandDefault = j.num("multibandDefault", p.multibandDefault) != 0;
    p.bassMonoOnPhaseIssues = j.num("bassMonoOnPhaseIssues", p.bassMonoOnPhaseIssues) != 0;
#define AC_R(f) p.f = j.num(#f, p.f);
    AC_NUM_FIELDS(AC_R)
#undef AC_R
    auto& c = p.compStyle;
    c.ratio = j.num("compRatio", c.ratio);
    c.attackMs = j.num("compAttackMs", c.attackMs);
    c.releaseMs = j.num("compReleaseMs", c.releaseMs);
    c.kneeDb = j.num("compKneeDb", c.kneeDb);
    c.detector = j.num("compDetectorRms", c.detector == DetectorMode::Rms) != 0 ? DetectorMode::Rms : DetectorMode::Peak;
    c.rmsWindowMs = j.num("compRmsMs", c.rmsWindowMs);
    c.sidechainHpfHz = j.num("compScHpfHz", c.sidechainHpfHz);
    c.stereoLink = j.num("compLink", c.stereoLink);
    c.mix = j.num("compMix", c.mix);
    c.lookaheadMs = j.num("compLookaheadMs", c.lookaheadMs);
    auto nums = [](const std::string& s) {
        std::vector<double> out;
        std::string cur;
        for (char ch : s)
        {
            if (std::isdigit((unsigned char)ch) || ch == '.' || ch == '-' || ch == 'e' || ch == 'E' || ch == '+') cur += ch;
            else if (!cur.empty()) { out.push_back(std::atof(cur.c_str())); cur.clear(); }
        }
        if (!cur.empty()) out.push_back(std::atof(cur.c_str()));
        return out;
    };
    auto mb = nums(j.text("mbTarget", ""));
    for (size_t i = 0; i < 3 && i < mb.size(); ++i) p.mbTargetGrDb[i] = mb[i];
    if (j.v.count("targetOffsets"))
    {
        auto t = nums(j.text("targetOffsets", ""));
        p.targetOffsets.clear();
        for (size_t i = 0; i + 1 < t.size(); i += 2) p.targetOffsets.emplace_back(t[i], t[i + 1]);
    }
    return true;
}

} // namespace ac
