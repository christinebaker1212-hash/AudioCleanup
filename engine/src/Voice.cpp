#include "ac/Restoration.h"

namespace ac {

// ----------------------------------------------------------------- HumRemover
void HumRemover::prepare(double sr, int ch)
{
    sr_ = sr;
    f_.assign(size_t(ch), {});
    for (auto& chain : f_)
        for (auto& h : s_.harmonics)
        {
            if (h.freq <= 0 || h.freq > 0.45 * sr || h.depthDb <= 0) continue;
            const double q = h.freq / std::max(0.5, h.bandwidthHz);
            chain.emplace_back(BiquadCoeffs::peaking(h.freq, q, -h.depthDb, sr));
        }
}

void HumRemover::process(float* const* ch, int nch, int n)
{
    for (int c = 0; c < nch; ++c)
    {
        auto& chain = f_[size_t(c)];
        if (chain.empty()) continue;
        float* d = ch[c];
        for (int i = 0; i < n; ++i)
        {
            double x = d[i];
            for (auto& b : chain) x = b.process(x);
            d[i] = float(x);
        }
    }
}

// ------------------------------------------------------------- PlosiveReducer
void PlosiveReducer::prepare(double sr, int ch)
{
    sr_ = sr;
    const double q = std::sqrt(0.5);
    detLp_.assign(size_t(ch), BiquadCascade{});
    for (auto& d : detLp_) d.setLowpass(clampv(s_.detectHz, 40.0, 300.0), 4, sr);
    refHp_.assign(size_t(ch), Biquad(BiquadCoeffs::highpass(300.0, q, sr)));
    refLp_.assign(size_t(ch), Biquad(BiquadCoeffs::lowpass(std::min(3000.0, 0.45 * sr), q, sr)));
    applyLp_.assign(size_t(ch), Biquad(BiquadCoeffs::lowpass(clampv(s_.applyHz, 60.0, 400.0), q, sr)));
    aEnv_ = timeCoeff(2.0, sr);
    rEnv_ = timeCoeff(40.0, sr);
    aBase_ = timeCoeff(2000.0, sr);
    aA_ = timeCoeff(1.0, sr);
    aR_ = timeCoeff(s_.releaseMs, sr);
    lfEnv_ = refEnv_ = base_ = gr_ = 0;
    lfBase_ = -100.0;
    baseInit_ = false;
    inEvent_ = false;
    events_ = 0;
    la_ = int(std::lround(s_.lookaheadMs * 0.001 * sr));
    delay_.assign(size_t(ch), std::vector<float>(size_t(la_ + 1), 0.0f));
    dpos_ = 0;
    trace_.reset(sr);
}

void PlosiveReducer::process(float* const* ch, int nch, int n)
{
    const int dl = la_ + 1;
    auto follow = [](double& e, double x, double a, double r) { e = x > e ? a * e + (1 - a) * x : r * e + (1 - r) * x; };
    for (int i = 0; i < n; ++i)
    {
        double lf = 0, rf = 0;
        for (int c = 0; c < nch; ++c)
        {
            const double x = ch[c][i];
            lf = std::max(lf, std::abs(detLp_[size_t(c)].process(x)));
            rf = std::max(rf, std::abs(refLp_[size_t(c)].process(refHp_[size_t(c)].process(x))));
        }
        follow(lfEnv_, lf, aEnv_, rEnv_);
        follow(refEnv_, rf, aEnv_, rEnv_);
        const double lfDb = gainToDb(lfEnv_, -200.0);
        const double rel = lfDb - gainToDb(refEnv_ + 1e-9, -200.0);
        const bool present = lfDb > -70.0;
        if (!baseInit_ && present)
        {
            base_ = std::min(rel, -20.0);
            lfBase_ = lfDb;
            baseInit_ = true;
        }
        const double excess = rel - base_ - s_.sensitivityDb;
        // A pop is a sudden sub-fundamental burst: it must (1) rise well above
        // the running LF floor (rejects steady hum/rumble when the voice
        // pauses), (2) exceed the usual LF/voice ratio, and (3) reach at least
        // near the voice-band level.
        const bool onset = lfDb > lfBase_ + s_.sensitivityDb;
        const bool plosive = baseInit_ && onset && excess > 0.0 && rel > -12.0 && lfDb > -50.0;
        const double target = plosive ? std::min(s_.maxReductionDb, std::min(excess, lfDb - lfBase_ - s_.sensitivityDb)) : 0.0;
        if (present && !plosive && baseInit_)
        {
            base_ = aBase_ * base_ + (1.0 - aBase_) * rel;
            lfBase_ = aBase_ * lfBase_ + (1.0 - aBase_) * lfDb;
        }
        gr_ = target > gr_ ? aA_ * gr_ + (1.0 - aA_) * target : aR_ * gr_ + (1.0 - aR_) * target;
        if (!inEvent_ && gr_ > 3.0)
        {
            inEvent_ = true;
            ++events_;
        }
        else if (inEvent_ && gr_ < 1.0) inEvent_ = false;
        trace_.push(float(gr_));
        const double k = 1.0 - dbToGain(-gr_);
        const int rd = (dpos_ + 1) % dl;
        for (int c = 0; c < nch; ++c)
        {
            auto& d = delay_[size_t(c)];
            d[size_t(dpos_)] = ch[c][i];
            const double xd = d[size_t(rd)];
            const double low = applyLp_[size_t(c)].process(xd);
            ch[c][i] = float(k > 0.0 ? xd - k * low : xd);
        }
        dpos_ = rd;
    }
}

// --------------------------------------------------------------------- DeEsser
void DeEsser::prepare(double sr, int ch)
{
    sr_ = sr;
    const double q = std::sqrt(0.5);
    const double detLo = clampv(s_.detectLowHz, 1500.0, 0.4 * sr);
    sibHp_.assign(size_t(ch), Biquad(BiquadCoeffs::highpass(detLo, q, sr)));
    refHp_.assign(size_t(ch), Biquad(BiquadCoeffs::highpass(300.0, q, sr)));
    refLp_.assign(size_t(ch), Biquad(BiquadCoeffs::lowpass(std::min(3000.0, 0.45 * sr), q, sr)));
    const double f = s_.useBell ? clampv(s_.freqHz, 2000.0, 0.45 * sr) : detLo;
    g0_ = std::tan(kPi * f / sr);
    flt_.assign(size_t(ch), Svf{});
    for (auto& fl : flt_) fl.setCoeffs(SvfCoeffs::fromG(s_.useBell ? SvfType::Bell : SvfType::HighShelf, g0_, s_.useBell ? 1.5 : 0.707, 0.0));
    aEnv_ = timeCoeff(3.0, sr);
    aA_ = timeCoeff(s_.attackMs, sr);
    aR_ = timeCoeff(s_.releaseMs, sr);
    sibEnv_ = refEnv_ = gr_ = 0;
    la_ = int(std::lround(s_.lookaheadMs * 0.001 * sr));
    delay_.assign(size_t(ch), std::vector<float>(size_t(la_ + 1), 0.0f));
    dpos_ = 0;
    trace_.reset(sr);
}

void DeEsser::process(float* const* ch, int nch, int n)
{
    const int dl = la_ + 1;
    const SvfType type = s_.useBell ? SvfType::Bell : SvfType::HighShelf;
    const double q = s_.useBell ? 1.5 : 0.707;
    for (int i = 0; i < n; ++i)
    {
        double sp = 0, rp = 0;
        for (int c = 0; c < nch; ++c)
        {
            const double x = ch[c][i];
            const double sv = sibHp_[size_t(c)].process(x);
            const double rv = refLp_[size_t(c)].process(refHp_[size_t(c)].process(x));
            sp = std::max(sp, sv * sv);
            rp = std::max(rp, rv * rv);
        }
        sibEnv_ = aEnv_ * sibEnv_ + (1 - aEnv_) * sp;
        refEnv_ = aEnv_ * refEnv_ + (1 - aEnv_) * rp;
        const double sibDb = powerToDb(sibEnv_, -200.0);
        const double ratioDb = sibDb - powerToDb(refEnv_ + 1e-12, -200.0);
        double target = 0.0;
        if (sibDb > s_.absFloorDb)
            target = std::min(s_.maxReductionDb, std::max(0.0, (ratioDb - s_.thresholdDb) * (1.0 - 1.0 / std::max(1.0, s_.ratio))));
        gr_ = target > gr_ ? aA_ * gr_ + (1 - aA_) * target : aR_ * gr_ + (1 - aR_) * target;
        trace_.push(float(gr_));
        const SvfCoeffs cf = SvfCoeffs::fromG(type, g0_, q, -gr_);
        const int rd = (dpos_ + 1) % dl;
        for (int c = 0; c < nch; ++c)
        {
            auto& d = delay_[size_t(c)];
            d[size_t(dpos_)] = ch[c][i];
            auto& fl = flt_[size_t(c)];
            fl.setCoeffs(cf);
            ch[c][i] = float(fl.process(d[size_t(rd)]));
        }
        dpos_ = rd;
    }
}

} // namespace ac
