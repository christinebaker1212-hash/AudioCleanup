#include "ac/Eq.h"

namespace ac {

// --------------------------------------------------------------- FilterStage
void FilterStage::prepare(double sr, int ch)
{
    sr_ = sr;
    hp_.assign(size_t(ch), BiquadCascade{});
    lp_.assign(size_t(ch), BiquadCascade{});
    for (int c = 0; c < ch; ++c)
    {
        if (s_.hpfHz > 0) hp_[size_t(c)].setHighpass(s_.hpfHz, clampv(s_.hpfOrder, 1, 8), sr);
        if (s_.lpfHz > 0 && s_.lpfHz < 0.49 * sr) lp_[size_t(c)].setLowpass(s_.lpfHz, clampv(s_.lpfOrder, 2, 8), sr);
    }
}

int FilterStage::tail() const
{
    // Ringing of the lowest pole decays within a few periods.
    const double f = s_.hpfHz > 0 ? s_.hpfHz : 1000.0;
    return int(sr_ * std::min(0.5, 8.0 / f));
}

void FilterStage::process(float* const* ch, int nch, int n)
{
    for (int c = 0; c < nch; ++c)
    {
        const double off = c < int(s_.dcOffset.size()) ? s_.dcOffset[size_t(c)] : 0.0;
        auto& hp = hp_[size_t(c)];
        auto& lp = lp_[size_t(c)];
        float* d = ch[c];
        for (int i = 0; i < n; ++i)
        {
            double x = double(d[i]) - off;
            if (!hp.empty()) x = hp.process(x);
            if (!lp.empty()) x = lp.process(x);
            d[i] = float(x);
        }
    }
}

// ------------------------------------------------------------- ParametricEq
void ParametricEq::prepare(double sr, int ch)
{
    sr_ = sr;
    f_.assign(size_t(ch), {});
    for (auto& chain : f_)
        for (auto& b : bands_)
            if (b.enabled)
            {
                Svf s;
                s.set(b.type, b.freq, b.q, b.gainDb, sr);
                chain.push_back(s);
            }
}

void ParametricEq::process(float* const* ch, int nch, int n)
{
    for (int c = 0; c < nch; ++c)
    {
        auto& chain = f_[size_t(c)];
        if (chain.empty()) continue;
        float* d = ch[c];
        for (int i = 0; i < n; ++i)
        {
            double x = d[i];
            for (auto& s : chain) x = s.process(x);
            d[i] = float(x);
        }
    }
}

double ParametricEq::responseDb(const std::vector<EqBand>& bands, double f, double sr)
{
    std::complex<double> h = 1.0;
    for (auto& b : bands)
        if (b.enabled) h *= SvfCoeffs::make(b.type, b.freq, b.q, b.gainDb, sr).response(f, sr);
    return gainToDb(std::abs(h), -300.0);
}

// ---------------------------------------------------------------- DynamicEq
void DynamicEq::prepare(double sr, int ch)
{
    sr_ = sr;
    st_.assign(bands_.size(), BandState{});
    for (size_t b = 0; b < bands_.size(); ++b)
    {
        auto& bd = bands_[b];
        auto& s = st_[b];
        const double f = clampv(bd.freq, 10.0, 0.49 * sr);
        s.g0 = std::tan(kPi * f / sr);
        s.aEnv = timeCoeff(5.0, sr);
        s.aA = timeCoeff(bd.attackMs, sr);
        s.aR = timeCoeff(bd.releaseMs, sr);
        s.det.assign(size_t(ch), Svf{});
        s.flt.assign(size_t(ch), Svf{});
        SvfType detType = SvfType::Bandpass;
        if (bd.type == SvfType::HighShelf) detType = SvfType::Highpass;
        if (bd.type == SvfType::LowShelf) detType = SvfType::Lowpass;
        for (auto& d : s.det) d.setCoeffs(SvfCoeffs::fromG(detType, s.g0, bd.type == SvfType::Bell ? bd.q : 0.707, 0.0));
        for (auto& fl : s.flt) fl.setCoeffs(SvfCoeffs::fromG(bd.type, s.g0, bd.q, bd.staticGainDb));
    }
    trace_.reset(sr);
}

void DynamicEq::process(float* const* ch, int nch, int n)
{
    for (int i = 0; i < n; ++i)
    {
        float grMax = 0.0f;
        for (size_t b = 0; b < bands_.size(); ++b)
        {
            const auto& bd = bands_[b];
            if (!bd.enabled) continue;
            auto& s = st_[b];
            double pw = 0.0;
            for (int c = 0; c < nch; ++c)
            {
                const double v = s.det[size_t(c)].process(ch[c][i]);
                pw = std::max(pw, v * v);
            }
            s.env = s.aEnv * s.env + (1.0 - s.aEnv) * pw;
            const double lv = powerToDb(s.env, -200.0);
            const double over = std::max(0.0, lv - bd.thresholdDb);
            const double target = std::min(bd.rangeDb, over * (1.0 - 1.0 / std::max(1.0, bd.ratio)));
            s.gr = target > s.gr ? s.aA * s.gr + (1.0 - s.aA) * target : s.aR * s.gr + (1.0 - s.aR) * target;
            grMax = std::max(grMax, float(s.gr));
            const SvfCoeffs cf = SvfCoeffs::fromG(bd.type, s.g0, bd.q, bd.staticGainDb - s.gr);
            for (int c = 0; c < nch; ++c)
            {
                auto& fl = s.flt[size_t(c)];
                fl.setCoeffs(cf);
                ch[c][i] = float(fl.process(ch[c][i]));
            }
        }
        trace_.push(grMax);
    }
}

// ---------------------------------------------------------------- Saturator
double Saturator::shape(double x) const
{
    // f(x) = (tanh(d(x+b)) - tanh(db)) / (d sech^2(db)): f(0)=0, f'(0)=1.
    return (std::tanh(d_ * (x + b_)) - off_) * norm_;
}

void Saturator::prepare(double sr, int ch)
{
    sr_ = sr;
    d_ = std::max(1e-3, dbToGain(s_.driveDb) - 1.0 + 0.25); // drive 0 dB -> gentle curve
    b_ = clampv(s_.asymmetry, 0.0, 0.5);
    off_ = std::tanh(d_ * b_);
    norm_ = 1.0 / (d_ * (1.0 - off_ * off_));
    const int L = s_.oversample <= 1 ? 1 : s_.oversample <= 2 ? 2 : s_.oversample <= 4 ? 4 : 8;
    os_.assign(size_t(ch), Oversampler{});
    for (auto& o : os_) o.setup(L, 32);
    dc_.assign(size_t(ch), Biquad(BiquadCoeffs::onePoleHighpass(5.0, sr)));
    dry_.assign(size_t(ch), std::vector<float>(size_t(latency() + 1), 0.0f));
    dpos_ = 0;
}

void Saturator::process(float* const* ch, int nch, int n)
{
    const double mix = clampv(s_.mix, 0.0, 1.0);
    const double out = dbToGain(s_.outputDb);
    const int dl = latency() + 1;
    auto f = [this](double v) { return shape(v); };
    for (int i = 0; i < n; ++i)
    {
        const int rd = (dpos_ + 1) % dl;
        for (int c = 0; c < nch; ++c)
        {
            const double x = ch[c][i];
            dry_[size_t(c)][size_t(dpos_)] = float(x);
            double wet = os_[size_t(c)].process(x, f);
            if (b_ > 0) wet = dc_[size_t(c)].process(wet);
            const double dry = dry_[size_t(c)][size_t(rd)];
            ch[c][i] = float((dry * (1.0 - mix) + wet * mix) * out);
        }
        dpos_ = rd;
    }
}

// ----------------------------------------------------------- StereoProcessor
void StereoProcessor::prepare(double sr, int ch)
{
    sr_ = sr;
    (void)ch;
    if (s_.monoBelowHz > 0)
    {
        x_.set(s_.monoBelowHz, sr);
        apMid_.setCoeffs(BiquadCoeffs::allpass(s_.monoBelowHz, std::sqrt(0.5), sr));
    }
    x_.reset();
    apMid_.reset();
    sf_.clear();
    for (const auto& b : s_.sideEq)
        if (b.enabled) { sf_.emplace_back(); sf_.back().set(b.type, b.freq, b.q, b.gainDb, sr); }
    if (s_.sideLowCutHz > 0) sideHp_.set(SvfType::Highpass, s_.sideLowCutHz, std::sqrt(0.5), 0.0, sr);
    sideHp_.reset();
}

void StereoProcessor::process(float* const* ch, int nch, int n)
{
    if (nch != 2) return;
    const double gl = dbToGain(-s_.balanceDb / 2.0), gr = dbToGain(s_.balanceDb / 2.0);
    for (int i = 0; i < n; ++i)
    {
        const double l = ch[0][i] * gl, r = ch[1][i] * gr;
        double m = 0.5 * (l + r), s = 0.5 * (l - r);
        if (s_.monoBelowHz > 0)
        {
            double lo, hi;
            x_.process(s, lo, hi);
            s = hi;
            m = apMid_.process(m);
        }
        for (auto& f : sf_) s = f.process(s);
        if (s_.sideLowCutHz > 0) s = sideHp_.process(s);
        s *= s_.width;
        ch[0][i] = float(m + s);
        ch[1][i] = float(m - s);
    }
}

} // namespace ac
