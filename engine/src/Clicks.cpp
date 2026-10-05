#include "ac/Restoration.h"
#include "ac/Fft.h"

#include <algorithm>
#include <atomic>
#include <complex>
#include <thread>

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
            // Periodicity test: glottal pulses and similar periodic excitation
            // produce residual peaks of comparable size at regular spacing.
            // Collect comparable local peaks within +/-20 ms; reject when they
            // are dense or regularly spaced. Isolated clicks (or a few
            // irregular ones) pass.
            std::vector<int> nbs;
            const int lo = std::max(1, i - W), hi = std::min(len - 1, lastHit + W);
            for (int t = lo; t < hi; ++t)
            {
                if (t >= i - 3 && t <= lastHit + 3) continue;
                const double v = stat[size_t(t)];
                if (v >= pk / 3.0 && v >= stat[size_t(t - 1)] && v >= stat[size_t(t + 1)]) nbs.push_back(t - i);
            }
            // merge peaks closer than 1 ms (one excitation)
            std::vector<int> peaksAt;
            for (int d : nbs)
                if (peaksAt.empty() || d - peaksAt.back() > int(0.001 * sr)) peaksAt.push_back(d);
            bool periodic = peaksAt.size() >= 4;
            for (size_t u = 0; u < peaksAt.size() && !periodic; ++u)
                for (size_t v = u + 1; v < peaksAt.size() && !periodic; ++v)
                {
                    const double a1 = peaksAt[u], a2 = peaksAt[v];
                    const double d1 = std::abs(a1), d2 = std::abs(a2);
                    if (d1 < 1 || d2 < 1) continue;
                    // one on each side at similar distance, or two on one side at 1x and 2x
                    if (a1 * a2 < 0 && std::abs(d1 - d2) < 0.2 * std::max(d1, d2)) periodic = true;
                    if (a1 * a2 > 0 && std::abs(std::max(d1, d2) - 2.0 * std::min(d1, d2)) < 0.2 * std::max(d1, d2)) periodic = true;
                }
            const bool isolated = !periodic;
            // Refine the edges: the forward residual becomes large exactly at
            // the first corrupted sample and the backward residual at the
            // last, while min(ef, eb) under-reads near both edges.
            int a0 = i, b0 = lastHit;
            const double low = 0.5 * thr;
            while (a0 > 0 && i - a0 < maxLen && ef[size_t(a0 - 1)] > low) --a0;
            while (b0 + 1 < len && b0 - lastHit < maxLen && eb[size_t(b0 + 1)] > low) ++b0;
            Event e{ s0i + a0 - 1, s0i + b0 + 1 };
            e.a = std::max(0, e.a);
            e.b = std::min(n - 1, e.b);
            i = lastHit + 1;
            if (!isolated || pk < s.minPeakRatio * thr) continue;
            if (e.b - e.a + 1 > maxLen) { ++rejected; continue; }
            if (s.protectTransients)
            {
                // Onset test on the median power of 8 ms windows either side:
                // robust to the click's own samples, while a genuine onset
                // raises the power of the whole following window.
                const int w = int(0.008 * sr);
                std::vector<double> pre, post;
                for (int t = std::max(0, e.a - w); t < e.a; ++t) pre.push_back(x[size_t(t)] * x[size_t(t)]);
                for (int t = e.b + 1; t < std::min(n, e.b + 1 + w); ++t) post.push_back(x[size_t(t)] * x[size_t(t)]);
                const double mpre = pre.empty() ? 0.0 : median(pre), mpost = post.empty() ? 0.0 : median(post);
                if (mpost > s.onsetRatio * mpre + 1e-12) { ++rejected; continue; } // onset: programme, not a click
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

namespace {

/** Clipped-sample mask: 0 reliable, 1 clipped high, 2 clipped low. Runs longer
    than maxRun are left as reliable (flat for too long to be clipping). */
std::vector<uint8_t> clipMask(const std::vector<double>& x, double hi, double lo, int maxRun, int& runs)
{
    const double th = hi * 0.9995, tl = lo * 0.9995; // lo is negative
    std::vector<uint8_t> m(x.size(), 0);
    runs = 0;
    size_t i = 0;
    while (i < x.size())
    {
        const bool up = x[i] >= th, dn = x[i] <= tl;
        if (!up && !dn) { ++i; continue; }
        size_t j = i;
        while (j < x.size() && (up ? x[j] >= th : x[j] <= tl)) ++j;
        if (int(j - i) <= maxRun)
        {
            std::fill(m.begin() + std::ptrdiff_t(i), m.begin() + std::ptrdiff_t(j), uint8_t(up ? 1 : 2));
            ++runs;
        }
        i = j;
    }
    return m;
}

/** One A-SPADE frame on the Hann-windowed block (windowing keeps tonal
    content sparse in the DFT). y: N clipped samples, mask as above; the clip
    bounds scale with the window. out: N samples of the windowed estimate. */
void aspadeFrame(RealFft& fft, const double* yIn, const float* w, const uint8_t* mask, int N, double hiIn, double loIn, double eps,
                 int maxIter, double* out)
{
    std::vector<double> yw(static_cast<size_t>(N)), hiW(static_cast<size_t>(N)), loW(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
    {
        yw[size_t(i)] = w[i] * yIn[i];
        hiW[size_t(i)] = w[i] * hiIn;
        loW[size_t(i)] = w[i] * loIn;
    }
    const double* y = yw.data();
    const int M = fft.size(), B = fft.numBins();
    std::vector<float> buf(size_t(M), 0.0f);
    std::vector<std::complex<float>> a(static_cast<size_t>(B)), z(static_cast<size_t>(B)), u(static_cast<size_t>(B)), t(static_cast<size_t>(B));
    std::vector<float> mag(static_cast<size_t>(B));
    std::vector<double> x(y, y + N);
    auto analyse = [&] {
        for (int i = 0; i < N; ++i) buf[size_t(i)] = float(x[size_t(i)]);
        std::fill(buf.begin() + N, buf.end(), 0.0f);
        fft.forward(buf.data(), a.data());
    };
    analyse();
    // Coefficients are in unnormalised DFT units: the Parseval frame norm is |.|^2 / M,
    // with interior bins counted twice (conjugate half of the spectrum).
    const double epsU = eps * eps * double(M);
    int k = 1;
    for (int it = 0; it < maxIter; ++it)
    {
        // z = H_k(A x + u): keep the k largest coefficients.
        for (int b = 0; b < B; ++b)
        {
            z[size_t(b)] = a[size_t(b)] + u[size_t(b)];
            mag[size_t(b)] = std::norm(z[size_t(b)]);
        }
        if (k < B)
        {
            std::vector<float> sorted(mag);
            std::nth_element(sorted.begin(), sorted.begin() + (B - k), sorted.end());
            const float thr = sorted[size_t(B - k)];
            int kept = 0;
            for (int b = 0; b < B; ++b)
            {
                if (mag[size_t(b)] >= thr && kept < k) ++kept;
                else z[size_t(b)] = 0.0f;
            }
        }
        // x = proj_consistent(A^H (z - u)); A^H A = I, so this is the exact minimiser.
        for (int b = 0; b < B; ++b) t[size_t(b)] = z[size_t(b)] - u[size_t(b)];
        fft.inverse(t.data(), buf.data());
        for (int i = 0; i < N; ++i)
        {
            const double v = buf[size_t(i)];
            x[size_t(i)] = mask[i] == 0 ? y[i] : mask[i] == 1 ? std::max(v, hiW[size_t(i)]) : std::min(v, loW[size_t(i)]);
        }
        analyse();
        double r = 0;
        for (int b = 0; b < B; ++b)
        {
            const auto d = a[size_t(b)] - z[size_t(b)];
            r += (b == 0 || b == B - 1 ? 1.0 : 2.0) * std::norm(d);
            u[size_t(b)] += d;
        }
        if (r <= epsU) break;
        // Sparsity grows by one coefficient per iteration (the published schedule)
        // up to k = 100, then by ~1 % so dense frames finish in bounded time.
        k += std::max(1, k / 100);
    }
    std::copy(x.begin(), x.end(), out);
}

} // namespace

void Declipper::processAr(std::vector<double>& x, double hi, double lo, int maxRun)
{
    const int p = autoOrder(sr_, s_.arOrder, 20, 80, 40.0);
    const double th = hi * 0.9995, tl = lo * 0.9995;
    const int n = int(x.size());
    int i = 0;
    while (i < n)
    {
        if (x[size_t(i)] < th && x[size_t(i)] > tl) { ++i; continue; }
        int j = i;
        const double sgn = x[size_t(i)] > 0 ? 1.0 : -1.0;
        const double level = sgn > 0 ? hi : -lo;
        while (j < n && (sgn > 0 ? x[size_t(j)] >= th : x[size_t(j)] <= tl)) ++j;
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
                    if (trial[size_t(t)] * sgn < level)
                    {
                        seg[size_t(t)] = sgn * level;
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
            const double maxGain = dbToGain(s_.maxBoostDb);
            for (int t = i - s; t < j - s; ++t)
                x[size_t(s + t)] = sgn * clampv(seg[size_t(t)] * sgn, level, maxGain * level);
            ++repaired_;
        }
        i = j;
    }
}

void Declipper::processSparse(std::vector<double>& x, double hi, double lo, int maxRun, const Job& job)
{
    int runs = 0;
    const auto mask = clipMask(x, hi, lo, maxRun, runs);
    if (runs == 0) return;
    repaired_ += runs;
    // ~23 ms frames (1024 at 44.1/48 kHz), hop N/4, DFT redundancy 2.
    int N = 1024;
    if (sr_ < 32000) N = 512;
    if (sr_ > 64000) N = 2048;
    const int hop = N / 4;
    const int n = int(x.size());
    const auto win = hannWindow(N);
    // Frames that contain at least one clipped sample (all frames covering a
    // clipped sample are processed, so every rebuilt sample is fully covered).
    std::vector<int> starts;
    {
        std::vector<int> prefix(size_t(n) + 1, 0);
        for (int i = 0; i < n; ++i) prefix[size_t(i) + 1] = prefix[size_t(i)] + (mask[size_t(i)] ? 1 : 0);
        for (int st = -N + hop; st < n; st += hop)
        {
            const int a = std::max(0, st), b = std::min(n, st + N);
            if (b > a && prefix[size_t(b)] - prefix[size_t(a)] > 0) starts.push_back(st);
        }
    }
    if (starts.empty()) return;
    // Stopping tolerance: Zaviska et al. use 0.1 for unit-peak signals and 1024-sample
    // frames; here it scales with the clip level and the frame length.
    const double eps = 0.1 * std::max(hi, -lo) * std::sqrt(double(N) / 1024.0);
    const int maxIter = 600;
    const double maxGain = dbToGain(s_.maxBoostDb);

    int nThreads = s_.threads > 0 ? s_.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    nThreads = std::max(1, std::min(nThreads, int(starts.size() / 8) + 1));
    std::vector<std::vector<double>> acc(static_cast<size_t>(nThreads)), wsum(static_cast<size_t>(nThreads));
    std::vector<int> base(static_cast<size_t>(nThreads));
    std::atomic<bool> stop { false };
    auto worker = [&](int t) {
        const size_t f0 = starts.size() * size_t(t) / size_t(nThreads), f1 = starts.size() * size_t(t + 1) / size_t(nThreads);
        if (f0 >= f1) return;
        const int lo0 = starts[f0], hi0 = starts[f1 - 1] + N;
        base[size_t(t)] = lo0;
        acc[size_t(t)].assign(size_t(hi0 - lo0), 0.0);
        wsum[size_t(t)].assign(size_t(hi0 - lo0), 0.0);
        RealFft fft(2 * N);
        std::vector<double> y(static_cast<size_t>(N)), out(static_cast<size_t>(N));
        std::vector<uint8_t> m(static_cast<size_t>(N));
        for (size_t f = f0; f < f1 && !stop.load(std::memory_order_relaxed); ++f)
        {
            const int st = starts[f];
            for (int i = 0; i < N; ++i)
            {
                const int g = st + i;
                const bool in = g >= 0 && g < n;
                y[size_t(i)] = in ? x[size_t(g)] : 0.0;
                m[size_t(i)] = in ? mask[size_t(g)] : 0;
            }
            aspadeFrame(fft, y.data(), win.data(), m.data(), N, hi, lo, eps, maxIter, out.data());
            // out is already windowed: sum(w x) / sum(w) restores x.
            for (int i = 0; i < N; ++i)
            {
                const size_t o = size_t(st + i - lo0);
                acc[size_t(t)][o] += out[size_t(i)];
                wsum[size_t(t)][o] += win[size_t(i)];
            }
            if (job.cancelled()) stop = true;
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 1; t < nThreads; ++t) pool.emplace_back(worker, t);
        worker(0);
        for (auto& th : pool) th.join();
    }
    throwIfCancelled(job);
    std::vector<double> num(size_t(n), 0.0), den(size_t(n), 0.0);
    for (int t = 0; t < nThreads; ++t)
        for (size_t i = 0; i < acc[size_t(t)].size(); ++i)
        {
            const int g = base[size_t(t)] + int(i);
            if (g < 0 || g >= n) continue;
            num[size_t(g)] += acc[size_t(t)][i];
            den[size_t(g)] += wsum[size_t(t)][i];
        }
    // Only clipped samples change; each is a weighted mean of consistent
    // frame estimates, so it stays beyond the clip level.
    for (int i = 0; i < n; ++i)
    {
        const uint8_t mk = mask[size_t(i)];
        if (!mk || den[size_t(i)] <= 1e-9) continue;
        const double v = num[size_t(i)] / den[size_t(i)];
        x[size_t(i)] = mk == 1 ? clampv(v, hi, maxGain * hi) : clampv(v, maxGain * lo, lo);
    }
}

void Declipper::processOffline(AudioBuffer& b, const Job& job)
{
    repaired_ = 0;
    sr_ = b.sampleRate;
    const int maxRun = std::max(2, int(s_.maxRunMs * 0.001 * sr_));
    const double hi = s_.clipLevel;
    const double lo = -(s_.clipLevelNeg > 0.0 ? s_.clipLevelNeg : s_.clipLevel);
    for (int c = 0; c < b.numChannels(); ++c)
    {
        std::vector<double> x(b.vec(c).begin(), b.vec(c).end());
        if (s_.method == DeclipMethod::AR) processAr(x, hi, lo, maxRun);
        else processSparse(x, hi, lo, maxRun, job);
        for (size_t t = 0; t < x.size(); ++t) b.channel(c)[t] = float(x[t]);
        throwIfCancelled(job);
    }
}

} // namespace ac
