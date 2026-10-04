#include "ac/Dynamics.h"

namespace ac {

// ---------------------------------------------------------------- K-weighting
void KWeighting::prepare(double sr)
{
    // BS.1770-4 filter re-derived for any sample rate from its analog prototype
    // (same derivation as libebur128).
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(kPi * f0 / sr);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    BiquadCoeffs p;
    p.b0 = (Vh + Vb * K / Q + K * K) / a0;
    p.b1 = 2.0 * (K * K - Vh) / a0;
    p.b2 = (Vh - Vb * K / Q + K * K) / a0;
    p.a1 = 2.0 * (K * K - 1.0) / a0;
    p.a2 = (1.0 - K / Q + K * K) / a0;
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(kPi * f0 / sr);
    a0 = 1.0 + K / Q + K * K;
    BiquadCoeffs r;
    r.b0 = 1.0; r.b1 = -2.0; r.b2 = 1.0;
    r.a1 = 2.0 * (K * K - 1.0) / a0;
    r.a2 = (1.0 - K / Q + K * K) / a0;
    a_.setCoeffs(p);
    b_.setCoeffs(r);
    reset();
}

// ----------------------------------------------------------------- Compressor
double Compressor::gainComputer(double x, const CompressorSettings& s)
{
    const double T = s.thresholdDb, R = std::max(1.0, s.ratio), W = std::max(0.0, s.kneeDb);
    const double d = x - T;
    double y;
    if (2.0 * d < -W) y = x;
    else if (W > 0.0 && 2.0 * std::abs(d) <= W)
    {
        const double t = d + W / 2.0;
        y = x + (1.0 / R - 1.0) * t * t / (2.0 * W);
    }
    else y = T + d / R;
    return std::max(-s.maxGrDb, y - x);
}

void Compressor::prepare(double sr, int ch)
{
    sr_ = sr;
    nch_ = ch;
    aA_ = timeCoeff(s_.attackMs, sr);
    aR_ = timeCoeff(s_.releaseMs, sr);
    aRms_ = timeCoeff(s_.rmsWindowMs, sr);
    rms_.assign(size_t(ch), 0.0);
    grState_.assign(size_t(ch), 0.0);
    scHpf_.assign(size_t(ch), Biquad{});
    if (s_.sidechainHpfHz > 0)
        for (auto& b : scHpf_) b.setCoeffs(BiquadCoeffs::highpass(s_.sidechainHpfHz, std::sqrt(0.5), sr));
    la_ = int(std::lround(s_.lookaheadMs * 0.001 * sr));
    delay_.assign(size_t(ch), std::vector<float>(size_t(la_ + 1), 0.0f));
    dpos_ = 0;
    trace_.reset(sr);
}

void Compressor::computeGains(const double* det, int nch, double* gains)
{
    double lv[64];
    double maxDb = -400.0;
    for (int c = 0; c < nch; ++c)
    {
        double level;
        if (s_.detector == DetectorMode::Rms)
        {
            rms_[size_t(c)] = aRms_ * rms_[size_t(c)] + (1.0 - aRms_) * det[c] * det[c];
            level = std::sqrt(rms_[size_t(c)]);
        }
        else level = std::abs(det[c]);
        lv[c] = gainToDb(level, -400.0);
        maxDb = std::max(maxDb, lv[c]);
    }
    float grMax = 0.0f;
    for (int c = 0; c < nch; ++c)
    {
        const double use = (1.0 - s_.stereoLink) * lv[c] + s_.stereoLink * maxDb;
        const double target = -gainComputer(use, s_);
        double& st = grState_[size_t(c)];
        st = target > st ? aA_ * st + (1.0 - aA_) * target : aR_ * st + (1.0 - aR_) * target;
        gains[c] = dbToGain(-st + s_.makeupDb);
        grMax = std::max(grMax, float(st));
    }
    trace_.push(grMax);
}

void Compressor::process(float* const* ch, int nch, int n)
{
    double det[64], g[64];
    nch = std::min(nch, 64);
    const double mix = clampv(s_.mix, 0.0, 1.0);
    const int dl = la_ + 1;
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < nch; ++c)
        {
            const double x = ch[c][i];
            det[c] = s_.sidechainHpfHz > 0 ? scHpf_[size_t(c)].process(x) : x;
        }
        computeGains(det, nch, g);
        const int rd = (dpos_ + 1) % dl;
        for (int c = 0; c < nch; ++c)
        {
            auto& d = delay_[size_t(c)];
            d[size_t(dpos_)] = ch[c][i];
            const double dry = la_ > 0 ? d[size_t(rd)] : ch[c][i];
            ch[c][i] = float(dry * ((1.0 - mix) + mix * g[c]));
        }
        dpos_ = rd;
    }
}

// ------------------------------------------------------- Multiband compressor
void MultibandCompressor::prepare(double sr, int ch)
{
    sr_ = sr;
    nch_ = ch;
    x1_.assign(size_t(ch), LR4Crossover{});
    x2_.assign(size_t(ch), LR4Crossover{});
    const double lo = clampv(s_.xoverLowHz, 20.0, 0.4 * sr);
    const double hi = clampv(s_.xoverHighHz, lo * 1.5, 0.45 * sr);
    for (int c = 0; c < ch; ++c)
    {
        x1_[size_t(c)].set(lo, sr);
        x2_[size_t(c)].set(hi, sr);
    }
    double la = 0;
    for (auto& b : s_.band) la = std::max(la, b.lookaheadMs);
    for (int b = 0; b < 3; ++b)
    {
        auto cs = s_.band[b];
        cs.lookaheadMs = la;
        cs.mix = 1.0;
        comp_[b].setSettings(cs);
        comp_[b].prepare(sr, ch);
    }
    la_ = comp_[0].latency();
    trace_.reset(sr);
}

void MultibandCompressor::process(float* const* ch, int nch, int n)
{
    for (auto& bb : bandBuf_)
    {
        bb.resize(size_t(nch));
        for (auto& v : bb) v.resize(size_t(n));
    }
    for (int c = 0; c < nch; ++c)
    {
        for (int i = 0; i < n; ++i)
        {
            double low, rest, mid, high;
            x1_[size_t(c)].process(ch[c][i], low, rest);
            low = x2_[size_t(c)].allpass(low);
            x2_[size_t(c)].process(rest, mid, high);
            bandBuf_[0][size_t(c)][size_t(i)] = float(low);
            bandBuf_[1][size_t(c)][size_t(i)] = float(mid);
            bandBuf_[2][size_t(c)][size_t(i)] = float(high);
        }
    }
    std::vector<float*> ptrs(static_cast<size_t>(nch));
    for (int b = 0; b < 3; ++b)
    {
        for (int c = 0; c < nch; ++c) ptrs[size_t(c)] = bandBuf_[b][size_t(c)].data();
        comp_[b].process(ptrs.data(), nch, n);
    }
    for (int c = 0; c < nch; ++c)
        for (int i = 0; i < n; ++i)
            ch[c][i] = bandBuf_[0][size_t(c)][size_t(i)] + bandBuf_[1][size_t(c)][size_t(i)] + bandBuf_[2][size_t(c)][size_t(i)];
    // Trace: the max of band traces, sampled as they grow.
    const auto& t0 = comp_[0].grTrace()->values();
    while (trace_.values().size() < t0.size())
    {
        const size_t k = trace_.values().size();
        float m = 0;
        for (auto& cp : comp_)
            if (k < cp.grTrace()->values().size()) m = std::max(m, cp.grTrace()->values()[k]);
        for (int i = 0; i < trace_.hop(); ++i) trace_.push(m);
    }
}

// ------------------------------------------------------------------- Expander
void Expander::prepare(double sr, int ch)
{
    sr_ = sr;
    nch_ = ch;
    aEnv_ = timeCoeff(5.0, sr);
    aA_ = timeCoeff(s_.attackMs, sr);
    aR_ = timeCoeff(s_.releaseMs, sr);
    holdSamples_ = int(s_.holdMs * 0.001 * sr);
    holdCount_ = holdSamples_; // start open: no gating before the first measurement
    env_ = 0;
    gr_ = 0;
    la_ = int(std::lround(s_.lookaheadMs * 0.001 * sr));
    delay_.assign(size_t(ch), std::vector<float>(size_t(la_ + 1), 0.0f));
    dpos_ = 0;
    trace_.reset(sr);
}

void Expander::process(float* const* ch, int nch, int n)
{
    const int dl = la_ + 1;
    for (int i = 0; i < n; ++i)
    {
        double pk = 0;
        for (int c = 0; c < nch; ++c) pk = std::max(pk, double(std::abs(ch[c][i])));
        // Peak envelope: instant attack, 5 ms release.
        env_ = pk > env_ ? pk : aEnv_ * env_ + (1.0 - aEnv_) * pk;
        const double lv = gainToDb(env_, -200.0);
        double target = 0.0;
        if (lv >= s_.thresholdDb) holdCount_ = holdSamples_;
        if (holdCount_ > 0) --holdCount_;
        else
        {
            const double d = s_.thresholdDb - lv; // > 0 below threshold
            const double W = s_.kneeDb;
            double below = d;
            if (W > 0 && d < W / 2 && d > -W / 2) below = (d + W / 2) * (d + W / 2) / (2 * W);
            else if (d <= -W / 2) below = 0;
            target = std::min(s_.rangeDb, below * (s_.ratio - 1.0));
        }
        // Closing (more GR) follows release; opening follows attack.
        gr_ = target > gr_ ? aR_ * gr_ + (1.0 - aR_) * target : aA_ * gr_ + (1.0 - aA_) * target;
        const double g = dbToGain(-gr_);
        trace_.push(float(gr_));
        const int rd = (dpos_ + 1) % dl;
        for (int c = 0; c < nch; ++c)
        {
            auto& d = delay_[size_t(c)];
            d[size_t(dpos_)] = ch[c][i];
            ch[c][i] = float((la_ > 0 ? d[size_t(rd)] : ch[c][i]) * g);
        }
        dpos_ = rd;
    }
}

// ----------------------------------------------------------- Transient shaper
void TransientShaper::prepare(double sr, int ch)
{
    sr_ = sr;
    nch_ = ch;
    aF_ = timeCoeff(0.5, sr);
    aS_ = timeCoeff(25.0, sr);
    rF_ = timeCoeff(40.0, sr);
    rS_ = timeCoeff(300.0, sr);
    aG_ = timeCoeff(1.0, sr);
    fast_ = slow_ = fastR_ = slowR_ = 0;
    g_ = 0;
    la_ = int(std::lround(0.001 * sr));
    delay_.assign(size_t(ch), std::vector<float>(size_t(la_ + 1), 0.0f));
    dpos_ = 0;
}

void TransientShaper::process(float* const* ch, int nch, int n)
{
    const int dl = la_ + 1;
    auto follow = [](double& e, double x, double a, double r) { e = x > e ? a * e + (1 - a) * x : r * e + (1 - r) * x; };
    for (int i = 0; i < n; ++i)
    {
        double pk = 0;
        for (int c = 0; c < nch; ++c) pk = std::max(pk, double(std::abs(ch[c][i])));
        follow(fast_, pk, aF_, rF_);   // fast attack
        follow(slow_, pk, aS_, rF_);   // slow attack, same release
        follow(fastR_, pk, aF_, rF_);  // fast release
        follow(slowR_, pk, aF_, rS_);  // slow release, same attack
        const double fdb = gainToDb(fast_, -200.0);
        const double att = std::max(0.0, fdb - gainToDb(slow_, -200.0));
        const double sus = std::max(0.0, gainToDb(slowR_, -200.0) - gainToDb(fastR_, -200.0));
        const double gate = clampv((fdb + 70.0) / 20.0, 0.0, 1.0); // ignore near-silence
        const double target = gate * (s_.attackDb * clampv(att * s_.sensitivity / 12.0, 0.0, 1.0) +
                                      s_.sustainDb * clampv(sus * s_.sensitivity / 12.0, 0.0, 1.0));
        g_ = aG_ * g_ + (1.0 - aG_) * target;
        const double g = dbToGain(g_);
        const int rd = (dpos_ + 1) % dl;
        for (int c = 0; c < nch; ++c)
        {
            auto& d = delay_[size_t(c)];
            d[size_t(dpos_)] = ch[c][i];
            ch[c][i] = float(d[size_t(rd)] * g);
        }
        dpos_ = rd;
    }
}

// -------------------------------------------------------------------- Limiter
void TruePeakLimiter::prepare(double sr, int ch)
{
    sr_ = sr;
    nch_ = ch;
    inGain_ = dbToGain(s_.inputGainDb);
    ceil_ = dbToGain(s_.ceilingDbTP);
    W_ = std::max(1, int(std::lround(s_.lookaheadMs * 0.001 * sr)));
    tp_.assign(size_t(ch), TruePeakDetector{});
    for (auto& t : tp_) t.setup(24);
    const int D = tp_.empty() ? 0 : tp_[0].latency();
    la_ = D + W_;
    dlen_ = la_ + 1;
    delay_.assign(size_t(ch), std::vector<float>(size_t(dlen_), 0.0f));
    dpos_ = 0;
    minq_.clear();
    n_ = 0;
    box_.assign(size_t(W_), 1.0);
    boxPos_ = 0;
    boxSum_ = double(W_);
    rel_ = 1.0;
    sustain_ = 0.0;
    aFast_ = timeCoeff(s_.releaseFastMs, sr);
    aSlow_ = timeCoeff(s_.releaseSlowMs, sr);
    aSus_ = timeCoeff(200.0, sr);
    trace_.reset(sr);
}

void TruePeakLimiter::process(float* const* ch, int nch, int n)
{
    for (int i = 0; i < n; ++i)
    {
        double pk = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            const double x = double(ch[c][i]) * inGain_;
            pk = std::max(pk, tp_[size_t(c)].process(x));
            delay_[size_t(c)][size_t(dpos_)] = float(x);
        }
        const double req = pk > ceil_ ? ceil_ / pk : 1.0;

        // Sliding minimum of the required gain over the last W+1 samples.
        while (!minq_.empty() && minq_.back().second >= req) minq_.pop_back();
        minq_.emplace_back(n_, req);
        while (minq_.front().first < n_ - W_) minq_.pop_front();
        const double held = minq_.front().second;

        // Program-dependent release: sustained reduction -> slower release.
        const double grDb = -gainToDb(held, -200.0);
        sustain_ = aSus_ * sustain_ + (1.0 - aSus_) * grDb;
        const double blend = clampv(sustain_ / 3.0, 0.0, 1.0);
        const double a = aFast_ + (aSlow_ - aFast_) * blend;
        rel_ = held < rel_ ? held : held + (rel_ - held) * a;

        // Box filter of length W (exactly covered by the hold window).
        boxSum_ += rel_ - box_[size_t(boxPos_)];
        box_[size_t(boxPos_)] = rel_;
        if (++boxPos_ >= W_)
        {
            boxPos_ = 0;
            double s = 0;
            for (double v : box_) s += v; // re-sum to cancel rounding drift
            boxSum_ = s;
        }
        const double g = std::min(1.0, boxSum_ / double(W_));
        trace_.push(float(-gainToDb(g, -200.0)));

        const int rd = (dpos_ + 1) % dlen_;
        for (int c = 0; c < nch; ++c) ch[c][i] = float(double(delay_[size_t(c)][size_t(rd)]) * g);
        dpos_ = rd;
        ++n_;
    }
}

// --------------------------------------------------------------- SoftClipper
void SoftClipper::prepare(double sr, int ch)
{
    g_ = dbToGain(gainDb_);
    c_ = dbToGain(ceilDb_);
    k_ = clampv(knee_, 0.3, 0.99) * c_;
    os_.assign(size_t(ch), Oversampler{});
    for (auto& o : os_) o.setup(4, 32);
    trace_.reset(sr);
}

double SoftClipper::shape(double x) const
{
    const double a = std::abs(x);
    if (a <= k_) return x;
    const double r = c_ - k_;
    return (x > 0 ? 1.0 : -1.0) * (k_ + r * std::tanh((a - k_) / r));
}

void SoftClipper::process(float* const* ch, int nch, int n)
{
    auto f = [this](double v) { return shape(v); };
    for (int i = 0; i < n; ++i)
    {
        // Static-curve reduction at this sample (for the GR trace / bound).
        double red = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            const double v = std::abs(double(ch[c][i]) * g_);
            if (v > k_) red = std::max(red, gainToDb(v) - gainToDb(std::abs(shape(v))));
            ch[c][i] = float(os_[size_t(c)].process(double(ch[c][i]) * g_, f));
        }
        trace_.push(float(red));
    }
}

// ------------------------------------------------------------------- Leveler
void Leveler::processOffline(AudioBuffer& b, const Job& job)
{
    const double sr = b.sampleRate;
    sr_ = sr;
    trace_.reset(sr);
    curve_.clear();
    const size_t N = b.numFrames();
    const int H = std::max(1, int(std::lround(sr * 0.01)));
    const size_t hops = (N + size_t(H) - 1) / size_t(H);
    if (hops == 0) return;

    // K-weighted mean square per hop (channel sum, BS.1770 style).
    std::vector<double> ms(hops, 0.0);
    for (int c = 0; c < b.numChannels(); ++c)
    {
        KWeighting kw;
        kw.prepare(sr);
        const float* x = b.channel(c);
        for (size_t i = 0; i < N; ++i)
        {
            const double y = kw.process(x[i]);
            ms[i / size_t(H)] += y * y;
        }
    }
    for (size_t k = 0; k < hops; ++k)
    {
        const size_t len = std::min(size_t(H), N - k * size_t(H));
        ms[k] /= double(len);
    }
    throwIfCancelled(job);

    // Centered moving average over the measurement window.
    const int half = std::max(1, int(s_.windowMs / 10.0 / 2.0));
    std::vector<double> lvl(hops);
    std::vector<double> pre(hops + 1, 0.0);
    for (size_t k = 0; k < hops; ++k) pre[k + 1] = pre[k] + ms[k];
    for (size_t k = 0; k < hops; ++k)
    {
        const size_t a = k >= size_t(half) ? k - size_t(half) : 0;
        const size_t e = std::min(hops, k + size_t(half) + 1);
        lvl[k] = -0.691 + powerToDb((pre[e] - pre[a]) / double(e - a), -200.0);
    }

    // Desired correction on active hops; pauses are filled by interpolation.
    std::vector<double> d(hops, std::numeric_limits<double>::quiet_NaN());
    for (size_t k = 0; k < hops; ++k)
        if (lvl[k] > s_.gateDb)
            d[k] = clampv(s_.strength * (s_.targetDb - lvl[k]), -s_.maxCutDb, s_.maxBoostDb);
    long last = -1;
    for (size_t k = 0; k < hops; ++k)
    {
        if (std::isnan(d[k])) continue;
        if (last < 0)
            for (size_t j = 0; j < k; ++j) d[j] = d[k];
        else
            for (size_t j = size_t(last) + 1; j < k; ++j)
            {
                const double t = double(j - size_t(last)) / double(k - size_t(last));
                // Hold the earlier gain through most of a pause, then glide.
                const double tt = clampv((t - 0.5) * 2.0, 0.0, 1.0);
                d[j] = d[size_t(last)] + (d[k] - d[size_t(last)]) * tt;
            }
        last = long(k);
    }
    if (last < 0) std::fill(d.begin(), d.end(), 0.0);
    else
        for (size_t j = size_t(last) + 1; j < hops; ++j) d[j] = d[size_t(last)];

    // Zero-phase one-pole smoothing (forward + backward).
    const double a = std::exp(-10.0 / std::max(10.0, s_.smoothingMs));
    for (size_t k = 1; k < hops; ++k) d[k] = a * d[k - 1] + (1 - a) * d[k];
    for (size_t k = hops - 1; k-- > 0;) d[k] = a * d[k + 1] + (1 - a) * d[k];

    curve_.resize(hops);
    for (size_t k = 0; k < hops; ++k) curve_[k] = float(d[k]);

    // Apply, interpolating in dB between hop centres.
    for (size_t i = 0; i < N; ++i)
    {
        const double pos = (double(i) + 0.5) / double(H) - 0.5;
        const size_t k0 = size_t(clampv(std::floor(pos), 0.0, double(hops - 1)));
        const size_t k1 = std::min(hops - 1, k0 + 1);
        const double t = clampv(pos - double(k0), 0.0, 1.0);
        const double gdb = d[k0] + (d[k1] - d[k0]) * t;
        const double g = dbToGain(gdb);
        for (int c = 0; c < b.numChannels(); ++c) b.channel(c)[i] = float(b.channel(c)[i] * g);
        trace_.push(float(std::max(0.0, -gdb)));
    }
}

} // namespace ac
