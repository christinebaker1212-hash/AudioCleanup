#pragma once
// Effect matching: estimate the effects heard on a reference clip (reverb,
// echo/delay, band limiting, compression/punch, stereo width, loudness) and
// the processors that put matching effects on another clip.
//
// The reference is only heard finished, so every estimate is blind: it comes
// from how the clip decays, repeats, peaks and is spread over frequency. A
// slowly ringing source (a gong) and reverb, or a repeated sample and an echo,
// look alike; the estimates say how confident they are.

#include "Analysis.h"
#include "AudioBuffer.h"
#include "Common.h"
#include "Eq.h"
#include "Loudness.h"
#include "Processor.h"

#include <string>
#include <vector>

namespace ac {

// ================================================================= Estimates
struct ReverbEstimate
{
    bool found = false;
    double rt60 = 0.0;                 ///< s, 500 Hz-2 kHz average
    std::vector<double> bandHz;        ///< octave centres 125 Hz .. 8 kHz
    std::vector<double> bandRt60;      ///< s per band (0 = not measurable)
    double tailLevelDb = -100.0;       ///< reverb tail extrapolated to the event start, dB re the event's first 50 ms (wet amount)
    int segments = 0;                  ///< free decays measured
    double confidence = 0.0;           ///< 0..1
};

struct EchoEstimate
{
    bool found = false;
    double delayMs = 0.0;
    double feedback = 0.0;             ///< 0..0.9, gain of each further repeat
    double levelDb = -100.0;           ///< first repeat re the dry sound
    bool pingPong = false;             ///< repeats alternate between channels
    double dampingHz = 0.0;            ///< repeats lose highs above this (0 = not damped)
    double confidence = 0.0;           ///< peak prominence, 0..1
};

struct BandLimitEstimate
{
    double hpfHz = 0.0;                ///< 0 = none found
    int hpfOrder = 0;
    double lpfHz = 0.0;
    int lpfOrder = 0;
};

struct FxProfile
{
    std::string name;
    double sampleRate = 48000.0;
    int channels = 0;
    double durationS = 0.0;
    SpectrumInfo spectrum;             ///< active-frame 1/3-octave LTAS
    BandLimitEstimate bandLimit;
    ReverbEstimate reverb;
    EchoEstimate echo;
    LoudnessStats loudness;
    double peakDb = -100.0;            ///< sample peak
    double plrDb = 0.0;                ///< sample peak - max momentary loudness
    double crestDb = 0.0;              ///< short-term crest (50 ms blocks)
    double bodyDb = 0.0;               ///< events' mean level 10-150 ms after their peak, re the peak (sustain / compression); 0 = no events
    bool stereo = false;
    double sideToMidDb = -100.0;
    std::vector<std::string> notes;
};

/** Estimate the effects on a reference clip. */
FxProfile analyzeEffects(const AudioBuffer& ref, const std::string& name = {}, const Job& job = {});

/** One-line-per-effect summary of what was found. */
std::string describeEffects(const FxProfile& p);

/** Reverb descriptor of any buffer (also used for closed-loop matching). */
ReverbEstimate measureReverb(const AudioBuffer& b);
/** Echo descriptor of any buffer. */
EchoEstimate measureEcho(const AudioBuffer& b);
/** Steep high/low-pass edges of the long-term spectrum. */
BandLimitEstimate measureBandLimit(const AudioBuffer& b);
/** Median over events of the mean level 10-150 ms after each peak, re that
    peak (dB, negative); 0 unless the clip has at least 3 separate events with
    free decays (hits), where the body is well defined. Sustain and
    fast-attack compression raise it, tightening and slow-attack compression lower it. */
double eventBodyDb(const AudioBuffer& b);
/** Sample peak minus max momentary loudness (falls back to peak - RMS for clips under 0.4 s). */
double peakToLoudnessDb(const AudioBuffer& b);

// ================================================================ Processors
struct ReverbSettings
{
    double rt60 = 1.0;                 ///< s, used where bandRt60 is empty or 0
    std::vector<double> bandHz;        ///< decay per frequency (octave centres)
    std::vector<double> bandRt60;
    double preDelayMs = 10.0;
    double wetDb = -12.0;              ///< reverb level re the dry signal
    double dryDb = 0.0;
    bool stereo = true;                ///< decorrelated left/right tails on stereo buffers
    uint32_t seed = 0x5eed;
};

/** Convolution reverb with a synthesised impulse response: decorrelated noise
    whose decay per octave band follows bandRt60, a 10 ms build-up and a
    pre-delay. Offline (FFT overlap-add); the tail is appended. */
class ConvolutionReverb : public Processor
{
public:
    explicit ConvolutionReverb(const ReverbSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override { sr_ = sr; (void)ch; }
    bool isOffline() const override { return true; }
    void process(float* const*, int, int) override {}
    void processOffline(AudioBuffer& b, const Job& job) override;
    /** The impulse response (wet part only, unit scale) for a sample rate / channel. */
    static std::vector<float> impulseResponse(const ReverbSettings& s, double sr, int channel);

private:
    ReverbSettings s_;
    double sr_ = 48000;
};

struct EchoSettings
{
    double delayMs = 250.0;
    double feedback = 0.35;            ///< 0..0.95
    double levelDb = -6.0;             ///< first repeat re the dry signal
    bool pingPong = false;             ///< stereo: repeats alternate left/right
    double dampingHz = 0.0;            ///< 1-pole low-pass in the feedback path (0 = off)
};

/** Feedback delay (tape-style echo). Offline so the repeats' tail is appended. */
class EchoProcessor : public Processor
{
public:
    explicit EchoProcessor(const EchoSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override { sr_ = sr; (void)ch; }
    bool isOffline() const override { return true; }
    void process(float* const*, int, int) override {}
    void processOffline(AudioBuffer& b, const Job& job) override;

private:
    EchoSettings s_;
    double sr_ = 48000;
};

/** Butterworth high/low-pass as EQ bands (cascaded SVF sections with
    Butterworth Qs), so band limits live in the parametric EQ. */
std::vector<EqBand> butterworthBands(SvfType type, double freq, int order);

} // namespace ac
