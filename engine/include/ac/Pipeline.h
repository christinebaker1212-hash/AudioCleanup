#pragma once
// Adaptive decision engine and renderer:
//   analyse -> select treatment -> render -> measure -> bounded corrections.

#include "Analysis.h"
#include "Dither.h"
#include "Dynamics.h"
#include "Eq.h"
#include "Presets.h"
#include "Restoration.h"

#include <map>
#include <optional>

namespace ac {

enum class Section { Cleanup, Tone, Dynamics, Output };
const char* sectionName(Section s);

enum class StageId
{
    Filter, Declip, Declick, Dehum, Denoise, Neural, Dereverb, Plosive, DeEss,
    Eq, DynEq, Leveler, Expander, Transient, Compressor, Multiband, Saturation, Stereo,
    Limiter,
    Count
};
const char* stageName(StageId id);
const char* stageKey(StageId id);           ///< stable identifier for presets/CLI
std::optional<StageId> stageFromKey(const std::string& key);
Section stageSection(StageId id);

enum class Override { Auto, On, Off };

struct UserControls
{
    bool sectionOn[4] = { true, true, true, true }; ///< Cleanup, Tone, Dynamics, Output
    double cleanup = 1.0;   ///< 0..2 macro: depth of repair (NR depth, de-ess, sensitivity)
    double tone = 1.0;      ///< 0..2 macro: strength of tonal correction / character
    double dynamics = 1.0;  ///< 0..2 macro: amount of compression / riding
    double tiltDb = 0.0;    ///< tone tilt around 1 kHz (+ = brighter)
    std::optional<LoudnessMode> loudnessMode;
    std::optional<double> targetLufs;
    std::optional<double> ceilingDbTP;
    double outputSampleRate = 0.0; ///< 0 = keep source rate
    std::map<StageId, Override> overrides;
    /** Optional user-selected noise-only region (samples) for the noise profile. */
    std::optional<std::pair<size_t, size_t>> noiseRegion;
};

struct StagePlan
{
    StageId id = StageId::Filter;
    bool enabled = false;
    bool available = true;      ///< false: not applicable to this category / not implemented
    bool forced = false;        ///< user override applied
    std::string reason;
    std::vector<std::pair<std::string, std::string>> params; ///< actual DSP settings, for display
};

/** All typed processor settings for one render. */
struct ChainSettings
{
    FilterSettings filter;
    DeclipSettings declip;
    ClickSettings declick;
    HumSettings dehum;
    DenoiseSettings denoise;
    NeuralDenoiseSettings neural;
    DereverbSettings dereverb;
    PlosiveSettings plosive;
    DeEssSettings deess;
    std::vector<EqBand> eq;
    std::vector<DynamicEqBand> dyneq;
    LevelerSettings leveler;
    ExpanderSettings expander;
    TransientSettings transient;
    CompressorSettings compressor;
    MultibandSettings multiband;
    SaturationSettings saturation;
    StereoSettings stereo;
    LimiterSettings limiter;
};

struct Plan
{
    const PresetDef* preset = nullptr;
    std::vector<StagePlan> stages; ///< processing order, one per StageId
    ChainSettings settings;
    LoudnessMode loudnessMode = LoudnessMode::Integrated;
    double targetLufs = -16.0;
    double ceilingDbTP = -1.0;
    double outputSampleRate = 48000.0;
    std::vector<std::string> log;  ///< measurements and corrections, in order

    StagePlan& stage(StageId id) { return stages[size_t(id)]; }
    const StagePlan& stage(StageId id) const { return stages[size_t(id)]; }
};

struct StageMetrics
{
    StageId id;
    double maxGrDb = 0.0;
    double milliseconds = 0.0;
    std::vector<float> grTrace; ///< 10 ms hops, aligned to the output timeline
};

struct ProcessResult
{
    AnalysisReport analysis;
    Plan plan;
    AudioBuffer output;      ///< processed, at plan.outputSampleRate, float
    AudioBuffer reference;   ///< original resampled to the output rate, same length (zero-padded tail)
    AudioBuffer removed;     ///< what the cleanup section removed (input - cleaned), same timeline
    LoudnessStats referenceStats;
    LoudnessStats outputStats;
    /** Gain (dB) to apply to `reference` so A/B comparisons are loudness matched. */
    double matchGainDb = 0.0;
    std::vector<float> grTrace;          ///< total dynamics + limiter gain reduction per 10 ms hop
    std::vector<StageMetrics> metrics;
    double outputLoudnessValue = 0.0;    ///< value of the selected loudness metric on the output
    bool targetReached = true;
};

struct ProcessRequest
{
    const AudioBuffer* input = nullptr;
    Category category = Category::Voice;
    const PresetDef* preset = nullptr;
    UserControls controls;
    const AnalysisReport* analysis = nullptr; ///< reuse a previous analysis of the same input
};

/** Full adaptive render. Throws CancelledException if job.cancel is set. */
ProcessResult process(const ProcessRequest& req, const Job& job = {});

/** Value of the given loudness metric (LUFS or dBTP for Peak). Returns -inf if undefined. */
double loudnessMetric(const AudioBuffer& b, LoudnessMode m, LoudnessStats* statsOut = nullptr);

/** Multi-line text dump of a plan (CLI / logs). */
std::string describePlan(const Plan& p);

/** Batch consistency (Game Asset): given each asset's loudness metric, return
    per-asset target offsets that pull them toward the group target while
    keeping `preserve` (0..1) of each asset's deviation from the group median. */
std::vector<double> batchConsistencyTargets(const std::vector<double>& metrics, double groupTarget, double preserve);

} // namespace ac
