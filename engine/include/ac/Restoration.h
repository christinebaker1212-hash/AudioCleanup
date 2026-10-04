#pragma once
// Restoration processors.

#include "Fft.h"
#include "Filters.h"
#include "Processor.h"

#include <complex>
#include <memory>

namespace ac {

// ===================================================================== STFT
/** Streaming STFT engine: sqrt-Hann analysis and synthesis windows, 75 %
    overlap (perfect reconstruction when the spectrum is unmodified). The
    callback sees all channels' spectra for a frame at once so gains can be
    linked across channels. Latency = fftSize samples. */
class StftProcessor : public Processor
{
public:
    explicit StftProcessor(int fftSize) : N_(fftSize), H_(fftSize / 4) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return N_; }
    int tail() const override { return N_; }
    void process(float* const* ch, int nch, int n) override;

    int fftSize() const { return N_; }
    int hop() const { return H_; }
    double sampleRate() const { return sr_; }

    /** Pick an FFT size of ~43 ms (2048 @ 48 kHz) for a sample rate. */
    static int defaultFftSize(double sr);

protected:
    virtual void onPrepare() {}
    /** Modify spectra in place; spectra[c] has fftSize/2+1 bins. */
    virtual void processFrame(std::vector<std::complex<float>*>& spectra) = 0;

    int numChannels() const { return nch_; }

private:
    int N_, H_;
    double sr_ = 48000;
    int nch_ = 0;
    std::unique_ptr<RealFft> fft_;
    std::vector<float> win_;
    std::vector<std::vector<float>> in_, acc_, outq_;
    std::vector<std::vector<std::complex<float>>> spec_;
    std::vector<float> frame_;
    int inCount_ = 0, outPos_ = 0;
};

/** Per-bin noise power estimate from the quietest frames of a signal.
    Uses the same window/FFT size as the denoiser so units match. */
struct NoiseProfile
{
    int fftSize = 2048;
    double sampleRate = 48000;
    std::vector<float> psd;         ///< mean |X|^2 per bin (channel-averaged)
    double levelDb = kSilenceDb;    ///< broadband noise level, dBFS RMS
    double flatness = 0.0;          ///< spectral flatness (1 = white, ~0 = tonal)
    double stationarity = 1.0;      ///< 1 = very steady, 0 = fluctuating
    bool valid = false;

    static NoiseProfile estimate(const AudioBuffer& b, int fftSize, double quietPercent = 15.0);
    /** Restrict estimation to [start,end) sample ranges (e.g. user-selected noise-only region). */
    static NoiseProfile estimateFromRegion(const AudioBuffer& b, int fftSize, size_t start, size_t end);
};

struct DenoiseSettings
{
    NoiseProfile profile;
    double reductionDb = 12.0;   ///< maximum attenuation (gain floor)
    double oversubtraction = 1.3; ///< noise PSD scaling (strength)
    double ddAlpha = 0.97;       ///< decision-directed smoothing (higher = fewer artifacts)
    double releaseMs = 60.0;     ///< gain fall smoothing (onsets pass instantly)
    double freqSmoothing = 0.04; ///< relative bandwidth of gain smoothing across bins
    bool adaptive = false;       ///< track non-stationary noise (minimum statistics)
    bool linkChannels = true;    ///< one gain for all channels: preserves the stereo image
    double lowCutoffHz = 0.0;    ///< bins below are left untouched (e.g. preserve LF ambience)
};

/** Spectral denoiser: decision-directed a-priori SNR with MMSE log-spectral
    amplitude gain (Ephraim-Malah 1985), bounded by a gain floor, smoothed in
    frequency (constant-relative-bandwidth) and in time (asymmetric). */
class SpectralDenoiser : public StftProcessor
{
public:
    explicit SpectralDenoiser(const DenoiseSettings& s)
        : StftProcessor(s.profile.fftSize), s_(s) {}
    const DenoiseSettings& settings() const { return s_; }
    /** Median over frames of the per-frame attenuation (dB, power weighted within a frame). */
    double medianFrameAttenuationDb() const;
    /** 95th percentile of per-frame attenuation (dB): what noise-only passages receive. */
    double p95FrameAttenuationDb() const;

protected:
    void onPrepare() override;
    void processFrame(std::vector<std::complex<float>*>& spectra) override;

private:
    DenoiseSettings s_;
    int bins_ = 0;
    std::vector<double> noise_, prevGainSq_, prevPost_, gain_, smoothG_, tmp_, pw_;
    // minimum statistics
    std::vector<double> psm_, curMin_;
    std::vector<std::vector<double>> subMins_;
    int subFrames_ = 0, subCount_ = 0, subIdx_ = 0;
    double aRel_ = 0;
    double attnNum_ = 0, attnDen_ = 0;
    std::vector<double> frameAttn_;
    bool first_ = true;
};

struct DereverbSettings
{
    double rt60 = 0.5;           ///< seconds (estimated by analysis)
    double reductionDb = 8.0;    ///< max attenuation of late reverb
    double strength = 1.0;       ///< late-reverb estimate scaling
    double delayMs = 50.0;       ///< start of "late" reverberation
    int fftSize = 2048;
};

/** Statistical late-reverberation suppression (Lebart et al. 2001 /
    Habets): late reverb PSD is predicted from the signal PSD delayed by
    delayMs and attenuated by the exponential decay implied by RT60; a
    bounded LSA gain removes it. Early reflections and direct sound stay. */
class Dereverberator : public StftProcessor
{
public:
    explicit Dereverberator(const DereverbSettings& s) : StftProcessor(s.fftSize), s_(s) {}

protected:
    void onPrepare() override;
    void processFrame(std::vector<std::complex<float>*>& spectra) override;

private:
    DereverbSettings s_;
    int bins_ = 0, delayFrames_ = 1;
    double decay_ = 0;
    std::vector<std::vector<double>> hist_; // ring of smoothed PSD frames
    int histPos_ = 0;
    std::vector<double> psd_, prevGainSq_, prevPost_, gain_;
};

/** MMSE-LSA gain for a-priori SNR xi and a-posteriori SNR gamma. */
double lsaGain(double xi, double gamma);

// ====================================================================== Hum
struct HumSettings
{
    double fundamentalHz = 0.0;  ///< 50/60 Hz (refined by analysis), 0 = off
    struct Harmonic { double freq; double depthDb; double bandwidthHz; };
    std::vector<Harmonic> harmonics;
};

/** Bank of narrow bounded-depth notches (peaking cuts) at detected mains
    harmonics. Depth is set per harmonic from its measured prominence so
    program material between harmonics is untouched. */
class HumRemover : public Processor
{
public:
    explicit HumRemover(const HumSettings& s) : s_(s) {}
    void prepare(double sr, int ch) override;
    int tail() const override { return int(sr_ * 0.3); }
    void process(float* const* ch, int nch, int n) override;

private:
    HumSettings s_;
    double sr_ = 48000;
    std::vector<std::vector<Biquad>> f_;
};

// ============================================================ Clicks/clips
struct ClickSettings
{
    double threshold = 7.0;      ///< detection threshold in robust residual sigmas
    double maxClickMs = 2.0;     ///< longer events are treated as program (not repaired)
    int arOrder = 0;             ///< 0 = automatic from sample rate
    bool protectTransients = true;
};

/** Click/crackle removal: AR(p) forward/backward prediction residual detects
    impulsive events; events that are followed by sustained energy (musical
    onsets) are left alone; the rest are reconstructed by least-squares AR
    interpolation (Janssen/Veldhuis, Godsill & Rayner). */
class ClickRemover : public Processor
{
public:
    explicit ClickRemover(const ClickSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override { sr_ = sr; (void)ch; }
    bool isOffline() const override { return true; }
    void process(float* const*, int, int) override {}
    void processOffline(AudioBuffer& b, const Job& job) override;
    int repairedCount() const { return repaired_; }
    int protectedCount() const { return protected_; }

    /** Detection only (used by analysis). Returns event count. */
    static int countClicks(const std::vector<float>& x, double sr, const ClickSettings& s, int maxSeconds = 30);

private:
    ClickSettings s_;
    double sr_ = 48000;
    int repaired_ = 0, protected_ = 0;
};

struct DeclipSettings
{
    double clipLevel = 0.99;     ///< absolute sample level treated as clipped
    double maxRunMs = 2.0;       ///< longer flat runs are left (cannot be credibly rebuilt)
    int arOrder = 0;
};

/** Reconstructs clipped runs by AR interpolation constrained to exceed the
    clip level with the clipped polarity (active-set iteration). */
class Declipper : public Processor
{
public:
    explicit Declipper(const DeclipSettings& s) : s_(s) {}
    void prepare(double sr, int ch) override { sr_ = sr; (void)ch; }
    bool isOffline() const override { return true; }
    void process(float* const*, int, int) override {}
    void processOffline(AudioBuffer& b, const Job& job) override;
    int repairedRuns() const { return repaired_; }

private:
    DeclipSettings s_;
    double sr_ = 48000;
    int repaired_ = 0;
};

/** AR helpers shared by click/clip repair. */
namespace ar {
/** Autocorrelation-method LPC: returns a[0..p] with a[0] = 1 (prediction-error filter). */
std::vector<double> fit(const double* x, int n, int order);
/** Least-squares AR interpolation of x[missing...] in place. */
bool interpolate(std::vector<double>& x, const std::vector<int>& missing, const std::vector<double>& a);
} // namespace ar

// =========================================================== Voice dynamics
struct PlosiveSettings
{
    double detectHz = 120.0;     ///< energy below this (below the voice's F0) is treated as non-voice
    double applyHz = 160.0;      ///< low band attenuated during a plosive
    double sensitivityDb = 10.0; ///< LF/voice-band excess over the running baseline that triggers
    double maxReductionDb = 12.0;
    double lookaheadMs = 5.0;
    double releaseMs = 70.0;
};

/** Plosive (pop) suppressor. Detection compares energy below the speaker's
    fundamental (where voiced speech has almost none) with the 300 Hz-3 kHz
    voice band, relative to a slowly tracked baseline; only bursts trigger.
    During a burst the low band is attenuated through a complementary split
    (out = x - (1-g) LP(x)), so the processor is bit-transparent when idle. */
class PlosiveReducer : public Processor
{
public:
    explicit PlosiveReducer(const PlosiveSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }
    int eventCount() const { return events_; }

private:
    PlosiveSettings s_;
    double sr_ = 48000;
    int la_ = 0, dpos_ = 0, events_ = 0;
    bool inEvent_ = false;
    std::vector<BiquadCascade> detLp_;
    std::vector<Biquad> refHp_, refLp_, applyLp_;
    double lfEnv_ = 0, refEnv_ = 0, base_ = 0, lfBase_ = -100, gr_ = 0, aEnv_ = 0, rEnv_ = 0, aBase_ = 0, aA_ = 0, aR_ = 0;
    bool baseInit_ = false;
    std::vector<std::vector<float>> delay_;
    GrTrace trace_;
};

struct DeEssSettings
{
    double freqHz = 6500.0;       ///< sibilance centre (from analysis)
    double detectLowHz = 4000.0;  ///< sidechain high-pass
    double thresholdDb = 6.0;     ///< sibilance-to-voice ratio above which reduction starts
    double absFloorDb = -50.0;    ///< ignore sibilance band below this level
    double maxReductionDb = 6.0;
    double ratio = 3.0;
    bool useBell = false;         ///< false: dynamic high-shelf from detectLowHz; true: bell at freqHz
    double lookaheadMs = 2.0;
    double attackMs = 1.0;
    double releaseMs = 50.0;
};

/** Adaptive de-esser: compares sibilance-band energy to the voice band so its
    action follows the ratio (level-independent), then cuts only the upper
    band with a per-sample-modulated SVF shelf/bell (no phase-split artifacts). */
class DeEsser : public Processor
{
public:
    explicit DeEsser(const DeEssSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    int tail() const override { return la_ + int(sr_ * 0.02); }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }

private:
    DeEssSettings s_;
    double sr_ = 48000, g0_ = 0;
    int la_ = 0, dpos_ = 0;
    std::vector<Biquad> sibHp_, refLp_, refHp_;
    std::vector<Svf> flt_;
    double sibEnv_ = 0, refEnv_ = 0, gr_ = 0, aEnv_ = 0, aA_ = 0, aR_ = 0;
    std::vector<std::vector<float>> delay_;
    GrTrace trace_;
};

// ================================================================ Neural
struct NeuralDenoiseSettings
{
    double mix = 1.0;              ///< wet/dry
    double maxAttenuationDb = 30.0; ///< residual of original mixed back to bound removal
};

/** RNNoise 0.2 recurrent-network speech enhancer (bundled model, 48 kHz
    internally; other rates are converted with r8brain). Speech only. */
class NeuralSpeechDenoiser : public Processor
{
public:
    explicit NeuralSpeechDenoiser(const NeuralDenoiseSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override { sr_ = sr; (void)ch; }
    bool isOffline() const override { return true; }
    void process(float* const*, int, int) override {}
    void processOffline(AudioBuffer& b, const Job& job) override;

    /** Per-10ms-frame voice activity probability from the model (48 kHz frames),
        averaged across channels. */
    static std::vector<float> voiceActivity(const AudioBuffer& b, const Job& job = {});
    /** Model latency at 48 kHz in samples (measured, compensated). */
    static int modelLatency48k();

private:
    NeuralDenoiseSettings s_;
    double sr_ = 48000;
};

} // namespace ac
