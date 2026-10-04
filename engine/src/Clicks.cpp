#include "ac/Restoration.h"

namespace ac {

namespace ar {

std::vector<double> fit(const double* x, int n, int order)
{
    std::vector<double> a(static_cast<size_t>(order) + 1, 0.0);
    a[0] = 1.0;
    if (n <= order + 1) return a;
    // Hann-windowed autocorrelation.
    std::vector<double> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[size_t(i)] = x[i] * (0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / n));
    std::vector<double> r(static_cast<size_t>(order) + 1, 0.0);
    for (int k = 0; k <= order; ++k)
    {
        double s = 0;
        for (int i = k; i < n; ++i) s += w[size_t(i)] * w[size_t(i - k)];
        r[size_t(k)] = s;
    }
    if (r[0] <= 1e-20) return a;
    r[0] *= 1.0 + 1e-6; // white-noise correction for numerical stability
    // Levinson-Durbin.
    double err = r[0];
    std::vector<double> tmp(static_cast<size_t>(order) + 1);
    for (int i = 1; i <= order; ++i)
    {
        double acc = r[size_t(i)];
        for (int j = 1; j < i; ++j) acc += a[size_t(j)] * r[size_t(i - j)];
        const double k = -acc / err;
        tmp = a;
        for (int j = 1; j < i; ++j) a[size_t(j)] = tmp[size_t(j)] + k * tmp[size_t(i - j)];
        a[size_t(i)] = k;
        err *= (1.0 - k * k);
        if (err <= 0) break;
    }
    return a;
}

bool interpolate(std::vector<double>& x, const std::vector<int>& missing, const std::vector<double>& a)
{
    if (missing.empty()) return true;
    const int p = int(a.size()) - 1;
    const int n = int(x.size());
    const int m = int(missing.size());
    const int lo = missing.front(), hi = missing.back();
    const int r0 = std::max(p, lo), r1 = std::min(n - 1, hi + p);
    if (r1 < r0) return false;
    std::vector<int> col(static_cast<size_t>(n), -1);
    for (int j = 0; j < m; ++j) col[size_t(missing[size_t(j)])] = j;

    // Normal equations: (Au^T Au) xu = -Au^T b
    std::vector<double> M(static_cast<size_t>(m) * size_t(m), 0.0), rhs(size_t(m), 0.0);
    std::vector<std::pair<int, double>> urow;
    for (int row = r0; row <= r1; ++row)
    {
        double b = 0;
        urow.clear();
        for (int k = 0; k <= p; ++k)
        {
            const int idx = row - k;
            if (idx < 0) continue;
            const int cj = col[size_t(idx)];
            if (cj >= 0) urow.emplace_back(cj, a[size_t(k)]);
            else b += a[size_t(k)] * x[size_t(idx)];
        }
        for (auto& [ci, vi] : urow)
        {
            rhs[size_t(ci)] -= vi * b;
            for (auto& [cj, vj] : urow) M[size_t(ci) * size_t(m) + size_t(cj)] += vi * vj;
        }
    }
    // Cholesky solve with light diagonal loading.
    double tr = 0;
    for (int i = 0; i < m; ++i) tr += M[size_t(i) * size_t(m) + size_t(i)];
    const double load = 1e-9 * (tr / m + 1e-30);
    for (int i = 0; i < m; ++i) M[size_t(i) * size_t(m) + size_t(i)] += load;
    std::vector<double> L(static_cast<size_t>(m) * size_t(m), 0.0);
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            double s = M[size_t(i) * size_t(m) + size_t(j)];
            for (int k = 0; k < j; ++k) s -= L[size_t(i) * size_t(m) + size_t(k)] * L[size_t(j) * size_t(m) + size_t(k)];
            if (i == j)
            {
                if (s <= 0) return false;
                L[size_t(i) * size_t(m) + size_t(i)] = std::sqrt(s);
            }
            else L[size_t(i) * size_t(m) + size_t(j)] = s / L[size_t(j) * size_t(m) + size_t(j)];
        }
    }
    std::vector<double> y(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i)
    {
        double s = rhs[size_t(i)];
        for (int k = 0; k < i; ++k) s -= L[size_t(i) * size_t(m) + size_t(k)] * y[size_t(k)];
        y[size_t(i)] = s / L[size_t(i) * size_t(m) + size_t(i)];
    }
    for (int i = m - 1; i >= 0; --i)
    {
        double s = y[size_t(i)];
        for (int k = i + 1; k < m; ++k) s -= L[size_t(k) * size_t(m) + size_t(i)] * y[size_t(k)];
        y[size_t(i)] = s / L[size_t(i) * size_t(m) + size_t(i)];
    }
    for (int j = 0; j < m; ++j) x[size_t(missing[size_t(j)])] = y[size_t(j)];
    return true;
}

} // namespace ar

namespace {

int autoOrder(double sr, int requested, int lo, int hi, double base)
{
    if (requested > 0) return requested;
    return clampv(int(std::lround(base * sr / 48000.0)), lo, hi);
}

struct Event
{
    int a, b;
};

/** Detect impulsive events in x[from, to). Returns accepted events and the
    number rejected as transients/too long. */
std::vector<Event> detect(const std::vector<double>& x, double sr, const ClickSettings& s, size_t from, size_t to,
                          int& rejected)
{
    std::vector<Event> out;
    const int p = autoOrder(sr, s.arOrder, 16, 64, 32.0);
    const int Bk = std::max(2048, 8 * p);
    const int C = 4 * p;
    const int maxLen = std::max(4, int(s.maxClickMs * 0.001 * sr));
    const int n = int(x.size());
    std::vector<double> ef, eb, stat;
    for (size_t s0 = from; s0 < to; s0 += size_t(Bk))
    {
        const int bs = int(s0);
        const int be = std::min(int(to), bs + Bk);
        const int ra = std::max(0, bs - C), re = std::min(n, be + C);
        if (re - ra < 4 * p) continue;
        const auto a = ar::fit(&x[size_t(ra)], re - ra, p);
        // Residual statistic over the block plus an isolation margin W on both sides.
        const int W = int(0.02 * sr);
        const int s0i = std::max(0, bs - W);
        const int sEnd = std::min(n, be + C / 2 + W);
        const int len = sEnd - s0i;
        const int off = bs - s0i; // index of bs inside the arrays
        ef.assign(size_t(len), 0.0);
        eb.assign(size_t(len), 0.0);
        stat.assign(size_t(len), 0.0);
        for (int i = 0; i < len; ++i)
        {
            const int t = s0i + i;
            double f = 0, bk = 0;
            for (int k = 0; k <= p; ++k)
            {
                if (t - k >= 0) f += a[size_t(k)] * x[size_t(t - k)];
                if (t + k < n) bk += a[size_t(k)] * x[size_t(t + k)];
            }
            ef[size_t(i)] = std::abs(f);
            eb[size_t(i)] = std::abs(bk);
            stat[size_t(i)] = std::min(ef[size_t(i)], eb[size_t(i)]);
        }
        std::vector<double> mags(ef.begin() + off, ef.begin() + off + (be - bs));
        const double sigma = 1.4826 * median(mags);
        if (sigma < 1e-7) continue;
        const double thr = s.threshold * sigma;
        int i = off;
        while (i < off + (be - bs))
        {
            if (stat[size_t(i)] <= thr) { ++i; continue; }
            int j = i, lastHit = i;
            double pk = 0;
            while (j < len && j - lastHit <= 4)
            {
                if (stat[size_t(j)] > thr) lastHit = j;
                pk = std::max(pk, stat[size_t(j)]);
                ++j;
            }
            // Isolation: a click stands far above every other residual peak
            // within +/-20 ms. Periodic excitation (glottal pulses, plucked
            // or bowed notes) has comparable neighbours and is rejected.
            double nb = 0;
            for (int t = std::max(0, i - W); t < std::min(len, lastHit + W + 1); ++t)
                if (t < i - 3 || t > lastHit + 3) nb = std::max(nb, stat[size_t(t)]);
            const bool isolated = pk > 3.0 * nb;
            Event e{ s0i + i - 2, s0i + lastHit + 2 };
            e.a = std::max(0, e.a);
            e.b = std::min(n - 1, e.b);
            i = lastHit + 1;
            if (!isolated) continue;
            if (e.b - e.a + 1 > maxLen) { ++rejected; continue; }
            if (s.protectTransients)
            {
                const int w1 = int(0.005 * sr), w2 = int(0.012 * sr);
                double pre = 0, post = 0;
                int np = 0, nq = 0;
                for (int t = std::max(0, e.a - w1); t < e.a; ++t) { pre += x[size_t(t)] * x[size_t(t)]; ++np; }
                for (int t = e.b + 1; t < std::min(n, e.b + 1 + w2); ++t) { post += x[size_t(t)] * x[size_t(t)]; ++nq; }
                pre = np ? pre / np : 0;
                post = nq ? post / nq : 0;
                if (post > 4.0 * pre + 1e-12) { ++rejected; continue; } // onset: program, not a click
            }
            out.push_back(e);
        }
    }
    return out;
}

bool repairEvent(std::vector<double>& x, int a, int b, int p)
{
    const int n = int(x.size());
    const int ctx = 3 * p + (b - a);
    const int s = std::max(0, a - ctx), e = std::min(n, b + 1 + ctx);
    std::vector<double> seg(x.begin() + s, x.begin() + e);
    // Bridge the gap linearly for the AR fit so the click does not bias it.
    const int la = a - s, lb = b - s;
    const double va = la > 0 ? seg[size_t(la - 1)] : 0.0, vb = lb + 1 < int(seg.size()) ? seg[size_t(lb + 1)] : 0.0;
    std::vector<double> fitSeg = seg;
    for (int t = la; t <= lb; ++t) fitSeg[size_t(t)] = va + (vb - va) * double(t - la + 1) / double(lb - la + 2);
    const auto coeffs = ar::fit(fitSeg.data(), int(fitSeg.size()), std::min(p, int(seg.size()) / 4));
    std::vector<int> miss;
    for (int t = la; t <= lb; ++t) miss.push_back(t);
    if (!ar::interpolate(seg, miss, coeffs)) return false;
    for (int t = la; t <= lb; ++t) x[size_t(s + t)] = seg[size_t(t)];
    return true;
}

} // namespace

void ClickRemover::processOffline(AudioBuffer& b, const Job& job)
{
    repaired_ = protected_ = 0;
    const double sr = b.sampleRate;
    const int p = autoOrder(sr, s_.arOrder, 16, 64, 32.0);
    for (int c = 0; c < b.numChannels(); ++c)
    {
        std::vector<double> x(b.vec(c).begin(), b.vec(c).end());
        const size_t chunk = size_t(sr * 2.0);
        for (size_t from = 0; from < x.size(); from += chunk)
        {
            const size_t to = std::min(x.size(), from + chunk);
            int rej = 0;
            auto ev = detect(x, sr, s_, from, to, rej);
            protected_ += rej;
            for (auto& e : ev)
                if (repairEvent(x, e.a, e.b, p)) ++repaired_;
            throwIfCancelled(job);
            job.report((double(c) + double(to) / double(x.size())) / b.numChannels(), "Click repair");
        }
        for (size_t i = 0; i < x.size(); ++i) b.channel(c)[i] = float(x[i]);
    }
}

int ClickRemover::countClicks(const std::vector<float>& xf, double sr, const ClickSettings& s, int maxSeconds)
{
    std::vector<double> x(xf.begin(), xf.end());
    const size_t total = x.size();
    const size_t want = std::min(total, size_t(maxSeconds * sr));
    // Evenly spaced 5 s excerpts.
    const size_t ex = std::min(want, size_t(5.0 * sr));
    if (ex == 0) return 0;
    const size_t count = std::max<size_t>(1, want / ex);
    int events = 0;
    for (size_t k = 0; k < count; ++k)
    {
        const size_t from = count == 1 ? 0 : (total - ex) * k / (count - 1);
        int rej = 0;
        events += int(detect(x, sr, s, from, from + ex, rej).size());
    }
    return events;
}

void Declipper::processOffline(AudioBuffer& b, const Job& job)
{
    repaired_ = 0;
    const double sr = b.sampleRate;
    const int p = autoOrder(sr, s_.arOrder, 20, 80, 40.0);
    const int maxRun = std::max(2, int(s_.maxRunMs * 0.001 * sr));
    const double lvl = s_.clipLevel * 0.9995;
    for (int c = 0; c < b.numChannels(); ++c)
    {
        std::vector<double> x(b.vec(c).begin(), b.vec(c).end());
        const int n = int(x.size());
        int i = 0;
        while (i < n)
        {
            if (std::abs(x[size_t(i)]) < lvl) { ++i; continue; }
            int j = i;
            const double sgn = x[size_t(i)] > 0 ? 1.0 : -1.0;
            while (j < n && std::abs(x[size_t(j)]) >= lvl && x[size_t(j)] * sgn > 0) ++j;
            const int len = j - i;
            if (len >= 2 && len <= maxRun)
            {
                const int ctx = 3 * p;
                const int s = std::max(0, i - ctx), e = std::min(n, j + ctx);
                std::vector<double> seg(x.begin() + s, x.begin() + e);
                const auto coeffs = ar::fit(seg.data(), int(seg.size()), std::min(p, int(seg.size()) / 4));
                std::vector<int> miss;
                for (int t = i - s; t < j - s; ++t) miss.push_back(t);
                // Active set: samples that fall back inside the clip level are pinned to it.
                for (int it = 0; it < 6 && !miss.empty(); ++it)
                {
                    std::vector<double> trial = seg;
                    if (!ar::interpolate(trial, miss, coeffs)) break;
                    std::vector<int> keep;
                    bool changed = false;
                    for (int t : miss)
                    {
                        if (trial[size_t(t)] * sgn < s_.clipLevel)
                        {
                            seg[size_t(t)] = sgn * s_.clipLevel;
                            changed = true;
                        }
                        else
                        {
                            seg[size_t(t)] = trial[size_t(t)];
                            keep.push_back(t);
                        }
                    }
                    if (!changed) break;
                    miss = keep;
                }
                // Bound reconstruction to a plausible overshoot (+6 dB over the clip level).
                for (int t = i - s; t < j - s; ++t)
                    x[size_t(s + t)] = sgn * clampv(seg[size_t(t)] * sgn, s_.clipLevel, 2.0 * s_.clipLevel);
                ++repaired_;
            }
            i = j;
        }
        for (int t = 0; t < n; ++t) b.channel(c)[size_t(t)] = float(x[size_t(t)]);
        throwIfCancelled(job);
    }
}

} // namespace ac
