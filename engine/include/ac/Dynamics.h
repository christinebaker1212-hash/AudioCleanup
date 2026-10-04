#pragma once
// Dynamics processors: compressor, multiband compressor, expander, transient
// shaper, level rider (leveler) and true-peak limiter.

#include "Filters.h"
#include "Oversampling.h"
#include "Processor.h"

#include <deque>

namespace ac {

enum class DetectorMode { Peak, Rms };

struct CompressorSettings
{
    double thresholdDb = -18.0;
    double ratio = 2.0;
    double kneeDb = 6.0;          ///< soft-knee width (quadratic knee)
    double attackMs = 10.0;
    double releaseMs = 120.0;
    DetectorMode detector = DetectorMode::Rms;
    double rmsWindowMs = 10.0;
    double stereoLink = 1.0;      ///< 0 = independent channels, 1 = fully linked
    double sidechainHpfHz = 0.0;  ///< 0 = off; reduces bass-driven pumping
    double lookaheadMs = 0.0;
    double makeupDb = 0.0;
    double mix = 1.0;             ///< parallel (dry/wet) mix
    double maxGrDb = 60.0;        ///< hard bound on gain reduction
};

/** Feed-forward log-domain compressor (Giannoulis/Massberg/Reiss 2012):
    soft-knee static curve, smooth-branching attack/release on the gain
    reduction, adjustable detector, stereo link, sidechain HPF, lookahead. */
class Compressor : public Processor
{
public:
    explicit Compressor(const CompressorSettings& s = {}) : s_(s) {}
    void setSettings(const CompressorSettings& s) { s_ = s; }
    const CompressorSettings& settings() const { return s_; }

    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    int tail() const override { return la_ + int(sr_ * s_.releaseMs * 0.005); }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }

    /** Static curve: gain change (dB, <= 0) for an input level (dB). */
    static double gainComputer(double levelDb, const CompressorSettings& s);

    /** Per-sample core usable by the multiband compressor: computes the gain
        (linear) to apply for the given per-channel detector inputs. */
    void computeGains(const double* detIn, int nch, double* gainsOut);

private:
    CompressorSettings s_;
    double sr_ = 48000;
    int nch_ = 0, la_ = 0;
    double aA_ = 0, aR_ = 0, aRms_ = 0;
    std::vector<double> rms_, grState_;
    std::vector<Biquad> scHpf_;
    std::vector<std::vector<float>> delay_;
    int dpos_ = 0;
    GrTrace trace_;
};

struct MultibandSettings
{
    double xoverLowHz = 120.0;
    double xoverHighHz = 2500.0;
    CompressorSettings band[3];
};

/** 3-band compressor. Bands are split with two Linkwitz-Riley 24 dB/oct
    crossovers; the low band is passed through the high crossover's allpass so
    the recombined bands sum to a flat-magnitude allpass response (verified by
    tests: < 0.01 dB deviation with all bands at unity). */
class MultibandCompressor : public Processor
{
public:
    explicit MultibandCompressor(const MultibandSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    int tail() const override { return la_ + int(sr_ * 0.5); }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }

private:
    MultibandSettings s_;
    double sr_ = 48000;
    int nch_ = 0, la_ = 0;
    std::vector<LR4Crossover> x1_, x2_;
    Compressor comp_[3];
    std::vector<std::vector<float>> bandBuf_[3];
    GrTrace trace_;
};

struct ExpanderSettings
{
    double thresholdDb = -50.0;
    double ratio = 2.0;            ///< downward expansion ratio below threshold
    double rangeDb = 10.0;         ///< maximum attenuation
    double kneeDb = 6.0;
    double attackMs = 2.0;
    double holdMs = 60.0;
    double releaseMs = 150.0;
    double lookaheadMs = 3.0;
};

/** Linked downward expander with hold and bounded range (never a hard gate). */
class Expander : public Processor
{
public:
    explicit Expander(const ExpanderSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    int tail() const override { return la_; }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }

private:
    ExpanderSettings s_;
    double sr_ = 48000;
    int nch_ = 0, la_ = 0, holdCount_ = 0, holdSamples_ = 0;
    double env_ = 0, gr_ = 0, aEnv_ = 0, aA_ = 0, aR_ = 0;
    std::vector<std::vector<float>> delay_;
    int dpos_ = 0;
    GrTrace trace_;
};

struct TransientSettings
{
    double attackDb = 0.0;   ///< gain applied to onsets (+ = punchier)
    double sustainDb = 0.0;  ///< gain applied to decays (- = tighter)
    double sensitivity = 1.0;
};

/** Level-independent transient shaper (fast vs slow envelope difference). */
class TransientShaper : public Processor
{
public:
    explicit TransientShaper(const TransientSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    void process(float* const* ch, int nch, int n) override;

private:
    TransientSettings s_;
    double sr_ = 48000;
    int nch_ = 0, la_ = 0;
    double fast_ = 0, slow_ = 0, fastR_ = 0, slowR_ = 0, g_ = 0;
    double aF_ = 0, rF_ = 0, aS_ = 0, rS_ = 0, aG_ = 0;
    std::vector<std::vector<float>> delay_;
    int dpos_ = 0;
};

struct LimiterSettings
{
    double inputGainDb = 0.0;
    double ceilingDbTP = -1.0;
    double lookaheadMs = 2.0;
    double releaseFastMs = 40.0;
    double releaseSlowMs = 400.0;  ///< used when gain reduction is sustained
};

/** Lookahead true-peak limiter. Peaks are detected on a 4x oversampled
    reconstruction; a sliding-minimum hold followed by a box filter of the same
    length guarantees the gain is fully down before each peak (no attack
    overshoot). Program-dependent release blends between fast and slow
    release according to how sustained the gain reduction is. Fully linked. */
class TruePeakLimiter : public Processor
{
public:
    explicit TruePeakLimiter(const LimiterSettings& s = {}) : s_(s) {}
    void setSettings(const LimiterSettings& s) { s_ = s; }
    void prepare(double sr, int ch) override;
    int latency() const override { return la_; }
    int tail() const override { return la_; }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }

private:
    LimiterSettings s_;
    double sr_ = 48000;
    int nch_ = 0, W_ = 1, la_ = 0;
    double inGain_ = 1, ceil_ = 1;
    std::vector<TruePeakDetector> tp_;
    std::vector<std::vector<float>> delay_;
    int dpos_ = 0, dlen_ = 1;
    // sliding min over W+1 samples
    std::deque<std::pair<int64_t, double>> minq_;
    int64_t n_ = 0;
    std::vector<double> box_;
    int boxPos_ = 0;
    double boxSum_ = 0;
    double rel_ = 1.0, sustain_ = 0.0, aFast_ = 0, aSlow_ = 0, aSus_ = 0;
    GrTrace trace_;
};

struct LevelerSettings
{
    double targetDb = -20.0;      ///< short-term K-weighted level to ride toward
    double maxBoostDb = 6.0;
    double maxCutDb = 6.0;
    double strength = 0.7;        ///< fraction of the error corrected
    double windowMs = 400.0;      ///< level measurement window
    double smoothingMs = 600.0;   ///< zero-phase gain smoothing time constant
    double gateDb = -50.0;        ///< below this, gain is frozen (pauses, noise)
};

/** Offline level rider: measures K-weighted short-term level, derives a
    bounded correction, smooths it forward and backward (zero phase, so gain
    moves ahead of phrases like a human fader) and freezes in pauses. */
class Leveler : public Processor
{
public:
    explicit Leveler(const LevelerSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override { sr_ = sr; (void)ch; }
    bool isOffline() const override { return true; }
    void process(float* const*, int, int) override {}
    void processOffline(AudioBuffer& b, const Job& job) override;
    const GrTrace* grTrace() const override { return &trace_; }
    /** Applied gain in dB per 10 ms hop (positive = boost). */
    const std::vector<float>& gainCurve() const { return curve_; }

private:
    LevelerSettings s_;
    double sr_ = 48000;
    GrTrace trace_;
    std::vector<float> curve_;
};

/** K-weighting filter (BS.1770 pre-filter + RLB) for one channel. */
class KWeighting
{
public:
    void prepare(double sr);
    void reset() { a_.reset(); b_.reset(); }
    inline double process(double x) { return b_.process(a_.process(x)); }

private:
    Biquad a_, b_;
};

} // namespace ac
