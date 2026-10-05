#pragma once
// Source analysis: everything the decision engine needs to choose stages and
// derive their settings.

#include "AudioBuffer.h"
#include "Loudness.h"
#include "Restoration.h"

#include <string>
#include <vector>

namespace ac {

enum class Category { Voice, SoundEffect, Music };
const char* categoryName(Category c);

struct HumInfo
{
    bool detected = false;
    double f0 = 0.0;
    struct Harmonic { double freq; double prominenceDb; };
    std::vector<Harmonic> harmonics; ///< harmonics with prominence > 6 dB
};

struct ClippingInfo
{
    int runs = 0;                ///< flat runs of >= 3 samples at the max level
    double clippedPercent = 0.0;
    double level = 1.0;          ///< plateau level (linear)
    bool likely = false;
};

struct SpeechInfo
{
    bool analysed = false;
    double activeRatio = 0.0;     ///< fraction of time with voice activity
    double activeLevelDb = -100;  ///< median short-term K-weighted level while active
    double f0MedianHz = 0.0;
    double f0LowHz = 0.0;         ///< 10th percentile
    double sibilanceFreqHz = 6500.0;
    double sibilanceMedianDb = -20.0; ///< sibilance/voice band ratio, median over active frames
    double sibilanceP98Db = -20.0;
    int plosiveCandidates = 0;
    std::vector<float> vad;       ///< per 10 ms probability (48 kHz frames)
};

struct TransientInfo
{
    double attackMs = 0.0;        ///< onset (-30 dB) to peak (-1 dB) of main event
    double decayMs = 0.0;         ///< peak to -30 dB
    double peakToLoudnessDb = 0.0; ///< true peak minus max momentary loudness
    double activeDurationS = 0.0;
    int onsetCount = 0;
    double envelopeCrestDb = 0.0;
};

/** Tempo from the onset-strength autocorrelation (music only). */
struct TempoInfo
{
    double bpm = 0.0;          ///< 0 = not estimated
    double confidence = 0.0;   ///< normalised autocorrelation at the beat lag (0..1); >= 0.2 is a clear pulse
    bool reliable() const { return bpm > 0 && confidence >= 0.2; }
};

/** Estimate tempo (60-200 BPM): log-magnitude spectral flux at 200 frames/s,
    autocorrelation with a 4-harmonic comb and a broad prior around 120 BPM.
    Like any beat tracker it can report half or double time (174 vs 87 BPM);
    for timing purposes those give the same note-length grid. */
TempoInfo estimateTempo(const AudioBuffer& b);

struct StereoInfo
{
    bool isStereo = false;
    bool dualMono = false;
    double correlation = 1.0;
    double correlationP5 = 1.0;   ///< 5th percentile of 100 ms correlation
    double lowCorrelation = 1.0;  ///< below 150 Hz
    double sideToMidDb = -100.0;
    double balanceDb = 0.0;       ///< R - L energy
};

struct SpectrumInfo
{
    std::vector<double> bandHz;   ///< 1/3-octave centres 25 Hz .. 16 kHz
    std::vector<double> bandDb;   ///< active-frame LTAS, normalised to 0 dB mean over 100 Hz..10 kHz
    std::vector<double> bandAbsDb; ///< absolute band power (dBFS)
    std::vector<double> noiseBandDb; ///< noise profile in the same bands (dBFS)
    double tiltDbPerOct = 0.0;    ///< regression 100 Hz..10 kHz
    double subsonicRelDb = -100;  ///< < 20 Hz energy relative to 40-200 Hz
    double rumbleRelDb = -100;    ///< < 60 Hz relative to 100-500 Hz
    std::vector<double> fineHz, fineDb; ///< 1/12-octave smoothed LTAS for resonance detection
};

struct AnalysisReport
{
    double sampleRate = 48000;
    int channels = 0;
    double durationS = 0.0;
    size_t frames = 0;
    bool silent = false;
    bool veryShort = false;       ///< < 0.4 s: integrated loudness undefined
    LoudnessStats loudness;
    double rmsDb = kSilenceDb;
    double crestDb = 0.0;
    double plrDb = 0.0;
    std::vector<double> dcOffset;
    double dcOffsetMaxDb = kSilenceDb;
    ClippingInfo clipping;
    NoiseProfile noise;
    double activeLevelDb = kSilenceDb; ///< mean RMS of louder half of frames
    double snrDb = 100.0;
    HumInfo hum;
    int clickCount = 0;
    double clicksPerMinute = 0.0;
    SpeechInfo speech;
    TransientInfo transients;
    TempoInfo tempo;
    StereoInfo stereo;
    SpectrumInfo spectrum;
    double rt60 = 0.0;            ///< 0 = could not be estimated
    std::vector<std::string> notes;
};

AnalysisReport analyze(const AudioBuffer& b, Category cat, const Job& job = {});

/** Click detection criteria per category. Music and effects contain genuine
    sharp transients (sticks, mallets, plucks), so only very short, strong,
    non-onset events count as clicks there. */
ClickSettings clickSettingsFor(Category c);

/** Detector statistics for a band (SVF band-pass, Q, 5 ms power envelope,
    linked across channels) - same detector as DynamicEq. Returns dB
    percentiles {p50, p75, p90, p95, p99} over frames above gateDb. */
std::vector<double> bandLevelPercentiles(const AudioBuffer& b, SvfType detector, double freq, double q, double gateDb);

/** RMS detector level percentiles (window ms) over active 10 ms hops. */
struct LevelStats
{
    double p10 = -100, p50 = -100, p75 = -100, p90 = -100, p95 = -100, p99 = -100;
    double activeFraction = 0;
};
LevelStats rmsLevelStats(const AudioBuffer& b, double windowMs, double gateDb, double hpfHz = 0.0);

/** What reference matching needs from an approved / professional track. */
struct ReferenceProfile
{
    std::string name;
    double sampleRate = 48000;
    SpectrumInfo spectrum;   ///< active-frame 1/3-octave LTAS (normalised as in SpectrumInfo)
    LoudnessStats loudness;
    double plrDb = 0.0;      ///< true peak - integrated loudness
    bool stereo = false;
    double sideToMidDb = -100.0;
    double correlation = 1.0;
    double crestDb = 0.0;    ///< short-term crest: median peak-to-RMS of active 50 ms blocks (punch / density)
};

/** Short-term crest (dB): median sample-peak minus RMS over the active 50 ms
    blocks (within 20 dB of the loud P95 block). Level independent; limiting,
    clipping and fast compression lower it, transient emphasis raises it. */
double shortTermCrestDb(const AudioBuffer& b);

/** Analyse a reference track for matching (tonal balance, loudness, dynamics, width). */
ReferenceProfile analyzeReference(const AudioBuffer& b, const std::string& name = {});

/** Long-term average spectrum (active frames) of any buffer. */
SpectrumInfo longTermSpectrum(const AudioBuffer& b);

/** Human-readable multi-line summary. */
std::string describe(const AnalysisReport& r);

} // namespace ac
