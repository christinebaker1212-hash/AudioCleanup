#pragma once
// Treatment presets: sonic intent + processing constraints. Presets do not
// hold fixed chains; the decision engine derives every setting from the
// analysis and clamps it to these bounds.

#include "Analysis.h"
#include "Dynamics.h"

#include <string>
#include <utility>
#include <vector>

namespace ac {

enum class LoudnessMode
{
    Unchanged,      ///< keep level; limiter only protects the ceiling
    Peak,           ///< normalise true peak to the ceiling
    Integrated,     ///< BS.1770 integrated loudness (programme)
    MaxMomentary,   ///< loudest 400 ms window (short effects)
    MaxShortTerm    ///< loudest 3 s window
};
const char* loudnessModeName(LoudnessMode m);

struct PresetDef
{
    std::string id, name, description;
    Category category = Category::Voice;

    // ---- Output / delivery
    LoudnessMode loudnessMode = LoudnessMode::Integrated;
    double targetLufs = -16.0;
    double ceilingDbTP = -1.0;
    double maxLimiterGrDb = 3.0;     ///< bound on limiter gain reduction (P99 of active hops)
    double limiterLookaheadMs = 2.0;
    double limiterReleaseMs = 60.0;

    // ---- Cleanup bounds
    bool denoiseDefault = true;
    double maxNoiseReductionDb = 12.0;
    double targetNoiseFloorDb = -70.0; ///< desired residual noise floor (dBFS RMS) after NR
    double minSnrForNoNr = 55.0;       ///< above this SNR, NR is bypassed
    double denoiseStrength = 1.2;      ///< base over-subtraction
    double maxActiveLossDb = 1.0;      ///< artifact bound: allowed level loss in active frames
    double neuralBelowSnrDb = 0.0;     ///< auto-enable neural speech denoiser below this SNR (0 = never auto)
    double neuralMix = 0.85;
    double sibilanceTargetDb = 4.0;    ///< de-esser engages when P98 sibilance ratio exceeds this
    double maxDeEssDb = 6.0;
    double maxPlosiveDb = 12.0;
    double hpfMaxHz = 90.0;
    int hpfOrder = 2;
    bool hpfAlways = true;             ///< voice: always band-limit below the voice
    double clickThreshold = 7.0;       ///< residual sigmas
    double clickMinPerMinute = 3.0;    ///< below this rate the de-clicker is bypassed
    double humMaxDepthDb = 30.0;
    double dereverbAutoAboveRt60 = 0.0; ///< auto-enable dereverb above this RT60 (0 = manual only)
    double dereverbMaxDb = 8.0;
    double maxNoiseGateDb = 0.0;       ///< expander range (0 = off)

    // ---- Tone
    double toneStrength = 0.4;         ///< fraction of deviation from target corrected
    double maxEqBoostDb = 3.0;
    double maxEqCutDb = 4.0;
    std::vector<std::pair<double, double>> targetOffsets; ///< (Hz, dB) intent added to the category curve
    bool useTargetCurve = true;
    int maxResonanceCuts = 0;
    double harshnessRangeDb = 0.0;     ///< dynamic EQ 2-5 kHz (0 = off)
    double boomRangeDb = 0.0;          ///< dynamic low shelf (0 = off)
    double saturationDriveDb = 0.0;    ///< 0 = off
    double saturationAsymmetry = 0.0;
    double saturationMix = 1.0;
    bool bassMonoOnPhaseIssues = false;

    // ---- Dynamics
    double levelerRangeDb = 0.0;       ///< 0 = off
    double levelerStrength = 0.6;
    double transientAttackDb = 0.0;
    double transientSustainDb = 0.0;
    CompressorSettings compStyle;
    double compTargetGrDb = 0.0;       ///< GR at the P95 detector level (0 = compressor off)
    bool multibandDefault = false;
    double mbTargetGrDb[3] = { 2.0, 1.5, 2.0 };
    double mbXoverLowHz = 120.0, mbXoverHighHz = 2500.0;
};

const std::vector<PresetDef>& allPresets();
const PresetDef* findPreset(const std::string& id);
std::vector<const PresetDef*> presetsFor(Category c);

/** Category target curve (relative dB at a frequency, LTAS shape). */
double categoryTargetDb(Category c, double hz);

/** Serialise / parse presets as simple JSON (user presets). */
std::string presetToJson(const PresetDef& p);
bool presetFromJson(const std::string& json, PresetDef& out, std::string* error = nullptr);

} // namespace ac
