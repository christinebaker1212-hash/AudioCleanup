#pragma once
// Tone processors: utility filtering, parametric EQ, dynamic EQ, oversampled
// saturation and stereo image tools.

#include "Filters.h"
#include "Oversampling.h"
#include "Processor.h"

namespace ac {

struct FilterSettings
{
    std::vector<double> dcOffset;  ///< per-channel offset subtracted exactly (from analysis)
    double hpfHz = 0.0;            ///< 0 = off
    int hpfOrder = 2;              ///< Butterworth order 1..8 (6 dB/oct per order)
    double lpfHz = 0.0;            ///< 0 = off
    int lpfOrder = 2;
};

/** DC offset removal + Butterworth high/low-pass. */
class FilterStage : public Processor
{
public:
    explicit FilterStage(const FilterSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int tail() const override;
    void process(float* const* ch, int nch, int n) override;

private:
    FilterSettings s_;
    double sr_ = 48000;
    std::vector<BiquadCascade> hp_, lp_;
};

struct EqBand
{
    SvfType type = SvfType::Bell;
    double freq = 1000.0;
    double q = 0.707;
    double gainDb = 0.0;
    bool enabled = true;
};

/** Static parametric EQ built from Cytomic SVF sections. */
class ParametricEq : public Processor
{
public:
    explicit ParametricEq(std::vector<EqBand> bands = {}) : bands_(std::move(bands)) {}
    void prepare(double sr, int ch) override;
    int tail() const override { return int(sr_ * 0.05); }
    void process(float* const* ch, int nch, int n) override;
    /** Analytic magnitude response (dB) of the configured EQ. */
    static double responseDb(const std::vector<EqBand>& bands, double f, double sr);

private:
    std::vector<EqBand> bands_;
    double sr_ = 48000;
    std::vector<std::vector<Svf>> f_; // [channel][band]
};

struct DynamicEqBand
{
    SvfType type = SvfType::Bell;  ///< Bell, LowShelf or HighShelf
    double freq = 3000.0;
    double q = 1.0;
    double staticGainDb = 0.0;
    double thresholdDb = -30.0;    ///< detector band level (dBFS RMS)
    double ratio = 3.0;
    double rangeDb = 6.0;          ///< max dynamic cut (dB, positive number)
    double attackMs = 5.0;
    double releaseMs = 80.0;
    bool enabled = true;
};

/** Dynamic EQ: each band's gain is static + a cut driven by the band's own
    level (linked across channels). Coefficients update per sample (TPT SVF
    is modulation-safe) so there are no zipper or block-boundary artifacts. */
class DynamicEq : public Processor
{
public:
    explicit DynamicEq(std::vector<DynamicEqBand> b = {}) : bands_(std::move(b)) {}
    void prepare(double sr, int ch) override;
    int tail() const override { return int(sr_ * 0.05); }
    void process(float* const* ch, int nch, int n) override;
    const GrTrace* grTrace() const override { return &trace_; }

private:
    std::vector<DynamicEqBand> bands_;
    double sr_ = 48000;
    struct BandState
    {
        double g0 = 0, env = 0, gr = 0, aEnv = 0, aA = 0, aR = 0;
        std::vector<Svf> det, flt;
    };
    std::vector<BandState> st_;
    GrTrace trace_;
};

struct SaturationSettings
{
    double driveDb = 3.0;      ///< input drive into the curve
    double asymmetry = 0.0;    ///< 0 = odd harmonics only (tape-like), up to 0.5 adds even (tube-like)
    double mix = 1.0;
    double outputDb = 0.0;
    int oversample = 4;        ///< 1, 2, 4 or 8
};

/** Oversampled waveshaper normalised to unity small-signal gain, so drive
    changes harmonic density and peak shape rather than level. */
class Saturator : public Processor
{
public:
    explicit Saturator(const SaturationSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    int latency() const override { return os_.empty() ? 0 : os_[0].latency(); }
    int tail() const override { return latency() + int(sr_ * 0.05); }
    void process(float* const* ch, int nch, int n) override;
    double shape(double x) const;

private:
    SaturationSettings s_;
    double sr_ = 48000, d_ = 1, b_ = 0, off_ = 0, norm_ = 1;
    std::vector<Oversampler> os_;
    std::vector<Biquad> dc_;
    std::vector<std::vector<float>> dry_;
    int dpos_ = 0;
};

struct StereoSettings
{
    double width = 1.0;          ///< side gain (1 = unchanged)
    double monoBelowHz = 0.0;    ///< 0 = off; collapse side below this (LR4)
    double balanceDb = 0.0;      ///< + = raise right / lower left (split evenly)
    std::vector<EqBand> sideEq;  ///< EQ applied to the side (S) signal only (width by frequency)
    double sideLowCutHz = 0.0;   ///< 0 = off; 12 dB/oct high-pass on the side signal (tighter low end)
};

/** M/S width, bass mono-isation, side EQ (width by frequency), side low-cut and balance correction (stereo only). */
class StereoProcessor : public Processor
{
public:
    explicit StereoProcessor(const StereoSettings& s = {}) : s_(s) {}
    void prepare(double sr, int ch) override;
    void process(float* const* ch, int nch, int n) override;

private:
    StereoSettings s_;
    double sr_ = 48000;
    LR4Crossover x_;
    Biquad apMid_;
    std::vector<Svf> sf_;
    Svf sideHp_;
};

} // namespace ac
