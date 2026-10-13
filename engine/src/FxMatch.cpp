#include "ac/FxMatch.h"

#include "ac/Fft.h"
#include "ac/Filters.h"
#include "ac/Restoration.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <complex>
#include <cstdio>
#include <numeric>
#include <random>

namespace ac {

namespace {

std::string fmt(const char* f, ...)
{
    char b[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

size_t nextPow2(size_t n)
{
    size_t m = 32;
    while (m < n) m <<= 1;
    return m;
}

std::vector<double> monoOf(const AudioBuffer& b)
{
    std::vector<double> m(b.numFrames(), 0.0);
    const int nch = b.numChannels();
    for (int c = 0; c < nch; ++c)
        for (size_t i = 0; i < m.size(); ++i) m[i] += b.channel(c)[i] / nch;
    return m;
}

double pctl(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    const size_t k = std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + long(k), v.end());
    return v[k];
}

double med(std::vector<double> v) { return pctl(std::move(v), 0.5); }

/** Power envelope in dB: 10 ms RMS every 5 ms. */
std::vector<double> envelopeDb(const std::vector<double>& x, double sr, double hopS = 0.005, double winS = 0.010)
{
    const size_t hop = std::max<size_t>(1, size_t(hopS * sr)), win = std::max<size_t>(1, size_t(winS * sr));
    std::vector<double> e;
    if (x.size() < win) return e;
    std::vector<double> cs(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) cs[i + 1] = cs[i] + x[i] * x[i];
    for (size_t s = 0; s + win <= x.size(); s += hop) e.push_back(powerToDb((cs[s + win] - cs[s]) / double(win), -150.0));
    return e;
}

/** Octave band-pass (two cascaded RBJ band-passes, Q = sqrt(2)). */
std::vector<double> octaveBand(const std::vector<double>& x, double fc, double sr)
{
    Biquad a(BiquadCoeffs::bandpass(fc, std::sqrt(2.0), sr)), b(BiquadCoeffs::bandpass(fc, std::sqrt(2.0), sr));
    std::vector<double> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = b.process(a.process(x[i]));
    return y;
}

struct Line { double slope = 0, icpt = 0, sse = 0; };

Line fitLine(const std::vector<double>& t, const std::vector<double>& y, size_t a, size_t b)
{
    Line l;
    const double n = double(b - a);
    if (n < 2) return l;
    double st = 0, sy = 0, stt = 0, sty = 0;
    for (size_t i = a; i < b; ++i)
    {
        st += t[i];
        sy += y[i];
        stt += t[i] * t[i];
        sty += t[i] * y[i];
    }
    const double den = n * stt - st * st;
    if (std::abs(den) < 1e-12) return l;
    l.slope = (n * sty - st * sy) / den;
    l.icpt = (sy - l.slope * st) / n;
    for (size_t i = a; i < b; ++i)
    {
        const double r = y[i] - (l.icpt + l.slope * t[i]);
        l.sse += r * r;
    }
    return l;
}

/** Late decay of one segment (dB curve from its peak): the slower slope of a
    two-line fit (direct-sound decay, then tail), or the single slope when one
    line explains the decay. Returns slope (dB/s, negative), the tail line's
    value at t = 0 relative to the peak, and R^2 of the late fit. */
struct Decay { double slope = 0, tailDb = 0, r2 = 0; bool ok = false, knee = false; };

Decay lateDecay(const std::vector<double>& d, double hopS)
{
    Decay out;
    const size_t n = d.size();
    if (n < 10) return out;
    std::vector<double> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = double(i) * hopS;
    const Line one = fitLine(t, d, 0, n);
    double best = one.sse;
    Line late = one;
    size_t knee = 0;
    for (size_t k = 3; k + 6 <= n; ++k)
    {
        const Line a = fitLine(t, d, 0, k), b = fitLine(t, d, k, n);
        // A tail decays more slowly than the direct sound it follows.
        if (b.slope <= a.slope) continue;
        if (a.sse + b.sse < best)
        {
            best = a.sse + b.sse;
            late = b;
            knee = k;
        }
    }
    if (knee > 0 && best > 0.6 * one.sse) late = one, knee = 0; // the knee must earn its keep
    if (late.slope >= -1.0) return out;                           // not decaying
    double ss = 0, mu = 0;
    for (size_t i = knee; i < n; ++i) mu += d[i];
    mu /= double(n - knee);
    for (size_t i = knee; i < n; ++i) ss += (d[i] - mu) * (d[i] - mu);
    const Line lateFit = knee ? fitLine(t, d, knee, n) : one;
    out.slope = late.slope;
    // Tail level re the event's early energy (first 50 ms), not its sharpest
    // frame: transient shaping then leaves the reverb amount reading unchanged.
    const size_t early = std::min(n, size_t(std::lround(0.05 / hopS)));
    double ee = 0;
    for (size_t i = 0; i < early; ++i) ee += std::pow(10.0, d[i] / 10.0);
    out.tailDb = late.icpt - 10.0 * std::log10(ee / double(early));
    out.r2 = ss > 0 ? std::max(0.0, 1.0 - lateFit.sse / ss) : 0.0;
    out.knee = knee > 0;
    out.ok = true;
    return out;
}

/** +/-7.5 ms moving average of a decay curve (dB) after its first 10 ms (the
    onset keeps its shape): a linear decay is unchanged, frame-to-frame
    fluctuation of a diffuse tail is averaged out. */
void smoothDecay(std::vector<double>& d, double hopS)
{
    const size_t hw = std::max<size_t>(1, size_t(0.0075 / hopS)), keep = size_t(0.01 / hopS);
    const std::vector<double> src = d;
    for (size_t k = keep; k < d.size(); ++k)
    {
        const size_t a = std::max(keep, k > hw ? k - hw : 0), b = std::min(d.size() - 1, k + hw);
        double acc = 0;
        for (size_t q = a; q <= b; ++q) acc += src[q];
        d[k] = acc / double(b - a + 1);
    }
}

/** Free-decay segments of an envelope: [peak, end) frame ranges. */
std::vector<std::pair<size_t, size_t>> decaySegments(const std::vector<double>& env, double hopS, double& floorDb)
{
    std::vector<std::pair<size_t, size_t>> seg;
    if (env.size() < 10) return seg;
    floorDb = pctl(env, 0.1);
    const double top = *std::max_element(env.begin(), env.end());
    const size_t hw = std::max<size_t>(1, size_t(0.0125 / hopS)), lag = std::max<size_t>(1, size_t(0.03 / hopS));
    std::vector<double> sm(env.size());
    for (size_t k = 0; k < env.size(); ++k)
    {
        double acc = 0;
        size_t c = 0;
        for (size_t q = (k > hw ? k - hw : 0); q <= std::min(env.size() - 1, k + hw); ++q, ++c) acc += env[q];
        sm[k] = acc / double(c);
    }
    const size_t nb = size_t(0.02 / hopS) + 1, maxLen = size_t(8.0 / hopS);
    size_t i = 0;
    while (i < env.size())
    {
        const size_t lo = i > nb ? i - nb : 0, hi = std::min(env.size(), i + nb + 1);
        bool isMax = env[i] >= floorDb + 25.0 && env[i] >= top - 40.0;
        for (size_t j = lo; j < hi && isMax; ++j) isMax = env[j] <= env[i];
        if (!isMax) { ++i; continue; }
        // Walk the decay until the next onset or the noise floor. An onset is a
        // rise of the ~25 ms smoothed envelope over its value 30 ms earlier:
        // a diffuse reverb tail dips and recovers by several dB from frame to
        // frame but never rises like that, while a new event (even a soft
        // note under the tail) does. (The raw envelope is used until the
        // smoothing window lies wholly after the peak: averaged with the
        // silence before an onset, the smoothed level would read too low.)
        double mn = env[i];
        size_t j = i + 1;
        for (; j < env.size() && j - i < maxLen; ++j)
        {
            const double v = j >= i + hw ? sm[j] : env[j];
            if (j >= i + hw + lag && sm[j] - sm[j - lag] > 6.0) break; // onset
            if (v > mn + 10.0) break;                                  // a much louder event
            if (v < floorDb + 4.0) break;                              // reached the noise floor
            mn = std::min(mn, v);
        }
        if (env[i] - mn >= 20.0 && double(j - i) * hopS >= 0.06) seg.emplace_back(i, j);
        i = std::max(i + 1, j);
    }
    return seg;
}

/** LPC residual (order 24) of a signal: removes short-term correlation so the
    autocorrelation at echo lags is sharp. */
std::vector<double> whiten(const std::vector<double>& x)
{
    const int p = 24;
    const auto a = ar::fit(x.data(), int(x.size()), p);
    std::vector<double> e(x.size(), 0.0);
    for (size_t i = 0; i < x.size(); ++i)
    {
        double s = x[i];
        for (int k = 1; k <= p && size_t(k) <= i; ++k) s += a[size_t(k)] * x[i - size_t(k)];
        e[i] = s;
    }
    return e;
}

/** Normalised cross-correlation r[k] = <a(t), b(t+k)> / sqrt(<a,a><b,b>), k >= 0. */
std::vector<double> xcorr(const std::vector<double>& a, const std::vector<double>& b, size_t maxLag)
{
    const size_t n = std::min(a.size(), b.size());
    const size_t M = nextPow2(n + maxLag + 1);
    RealFft fft{ int(M) };
    std::vector<float> fa(M, 0.0f), fb(M, 0.0f);
    for (size_t i = 0; i < n; ++i)
    {
        fa[i] = float(a[i]);
        fb[i] = float(b[i]);
    }
    std::vector<std::complex<float>> A(size_t(fft.numBins())), B(size_t(fft.numBins()));
    fft.forward(fa.data(), A.data());
    fft.forward(fb.data(), B.data());
    for (size_t k = 0; k < A.size(); ++k) A[k] = std::conj(A[k]) * B[k];
    fft.inverse(A.data(), fa.data());
    double ea = 0, eb = 0;
    for (size_t i = 0; i < n; ++i)
    {
        ea += a[i] * a[i];
        eb += b[i] * b[i];
    }
    const double norm = std::sqrt(ea * eb) + 1e-30;
    std::vector<double> r(maxLag + 1);
    for (size_t k = 0; k <= maxLag; ++k) r[k] = double(fa[k]) / norm; // the inverse already yields the correlation sum
    return r;
}

/** Peak of r within +/- w samples of position p. */
double peakNear(const std::vector<double>& r, double p, size_t w, double* where = nullptr)
{
    const long c = long(std::lround(p));
    double best = -1;
    for (long k = c - long(w); k <= c + long(w); ++k)
        if (k > 0 && size_t(k) < r.size() && r[size_t(k)] > best)
        {
            best = r[size_t(k)];
            if (where) *where = double(k);
        }
    return best;
}

/** Normalised autocorrelation of an echo impulse response at lags tau, 2tau, 3tau. */
void echoModel(double w, double g, double rho[3])
{
    double h[48];
    h[0] = 1.0;
    for (int k = 1; k < 48; ++k) h[k] = w * std::pow(g, k - 1);
    double r0 = 0;
    for (double v : h) r0 += v * v;
    for (int m = 1; m <= 3; ++m)
    {
        double s = 0;
        for (int k = 0; k + m < 48; ++k) s += h[k] * h[k + m];
        rho[m - 1] = s / r0;
    }
}

} // namespace

// ======================================================================= Analysis
ReverbEstimate measureReverb(const AudioBuffer& b)
{
    ReverbEstimate r;
    const double sr = b.sampleRate, hopS = 0.005;
    const auto x = monoOf(b);
    const auto env = envelopeDb(x, sr, hopS);
    double floorDb = -150;
    const auto segs = decaySegments(env, hopS, floorDb);
    std::vector<double> rts, tails, w;
    int knees = 0;
    for (auto [p, e] : segs)
    {
        std::vector<double> d(env.begin() + long(p), env.begin() + long(e));
        // Usable range: above the floor (+6 dB) and within 60 dB of the peak (as
        // reverberation time is measured; digital silence would add a cliff).
        while (!d.empty() && (d.back() < floorDb + 6.0 || d.back() < d.front() - 60.0)) d.pop_back();
        smoothDecay(d, hopS);
        const Decay dc = lateDecay(d, hopS);
        if (!dc.ok || dc.r2 < 0.5) continue;
        rts.push_back(-60.0 / dc.slope);
        tails.push_back(dc.tailDb);
        w.push_back(dc.r2);
        knees += dc.knee ? 1 : 0;
    }
    r.segments = int(rts.size());
    if (rts.empty()) return r;
    r.rt60 = med(rts);
    r.tailLevelDb = med(tails);
    // Per octave band, over the same segments.
    for (double fc : { 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 })
    {
        if (fc > 0.4 * sr) break;
        const auto be = envelopeDb(octaveBand(x, fc, sr), sr, hopS);
        double bf = pctl(be, 0.1);
        std::vector<double> brt;
        for (auto [p, e] : segs)
        {
            if (e > be.size()) continue;
            std::vector<double> d(be.begin() + long(p), be.begin() + long(e));
            // The band's own peak may come a little later than the broadband one.
            const auto mx = std::max_element(d.begin(), d.begin() + long(std::min<size_t>(d.size(), 8)));
            d.erase(d.begin(), mx);
            while (!d.empty() && (d.back() < bf + 6.0 || d.back() < d.front() - 60.0)) d.pop_back();
            if (d.size() < 12 || d.front() - d.back() < 12.0) continue;
            smoothDecay(d, hopS);
            const Decay dc = lateDecay(d, hopS);
            if (dc.ok && dc.r2 >= 0.5) brt.push_back(-60.0 / dc.slope);
        }
        r.bandHz.push_back(fc);
        r.bandRt60.push_back(brt.empty() ? 0.0 : med(brt));
    }
    double mid = 0;
    int n = 0;
    for (size_t i = 0; i < r.bandHz.size(); ++i)
        if (r.bandHz[i] >= 500 && r.bandHz[i] <= 2000 && r.bandRt60[i] > 0) { mid += r.bandRt60[i]; ++n; }
    if (n) r.rt60 = mid / n;
    // Reverb on an impulsive sound decays in two slopes (direct sound, then a
    // slower tail); a dry source that rings decays in one. A single slope
    // counts as reverb only when it is long.
    const bool twoSlope = 2 * knees >= r.segments;
    r.found = (twoSlope && r.rt60 >= 0.2) || r.rt60 >= 0.5;
    r.confidence = clampv(double(r.segments) / 3.0, 0.0, 1.0) * clampv(med(w), 0.0, 1.0);
    return r;
}

EchoEstimate measureEcho(const AudioBuffer& b)
{
    EchoEstimate e;
    const double sr = b.sampleRate;
    const size_t n = std::min(b.numFrames(), size_t(60.0 * sr));
    const size_t lo = size_t(0.04 * sr), hi = std::min(size_t(1.6 * sr), size_t(double(n) / 2.5));
    if (hi <= lo + 10) return e;
    auto mono = monoOf(b);
    mono.resize(n);
    const auto wm = whiten(mono);
    const auto r = xcorr(wm, wm, size_t(3.2 * double(hi)) < n ? size_t(3.2 * double(hi)) : n - 1);
    size_t k1 = lo;
    for (size_t k = lo; k < hi; ++k)
        if (r[k] > r[k1]) k1 = k;
    std::vector<double> range(r.begin() + long(lo), r.begin() + long(hi));
    const double centre = med(range);
    std::vector<double> dev(range.size());
    for (size_t i = 0; i < range.size(); ++i) dev[i] = std::abs(range[i] - centre);
    const double mad = 1.4826 * med(dev) + 1e-9;
    const double r1 = r[k1], z = (r1 - centre) / mad;
    e.confidence = clampv((z - 8.0) / 12.0, 0.0, 1.0);
    if (r1 < 0.05 || z < 10.0) return e;
    // Sub-sample delay by parabolic interpolation.
    double tau = double(k1);
    if (k1 > 0 && k1 + 1 < r.size())
    {
        const double a = r[k1 - 1], c = r[k1 + 1], den = a - 2 * r1 + c;
        if (std::abs(den) > 1e-12) tau += 0.5 * (a - c) / den;
    }
    const size_t tol = std::max<size_t>(2, size_t(0.0005 * sr));
    const double r2 = std::max(0.0, peakNear(r, 2 * tau, tol)), r3 = std::max(0.0, peakNear(r, 3 * tau, tol));
    // Fit level w and feedback g of h = 1 + w * sum g^(k-1) z^(-k tau).
    double bestErr = 1e9, bw = 0, bg = 0;
    for (int iw = 0; iw <= 80; ++iw)
    {
        const double w = 0.02 * std::pow(75.0, iw / 80.0); // 0.02 .. 1.5
        for (int ig = 0; ig <= 45; ++ig)
        {
            const double g = 0.02 * ig;
            double rho[3];
            echoModel(w, g, rho);
            const double err = (rho[0] - r1) * (rho[0] - r1) + (rho[1] - r2) * (rho[1] - r2) + 0.5 * (rho[2] - r3) * (rho[2] - r3);
            if (err < bestErr) { bestErr = err; bw = w; bg = g; }
        }
    }
    // An echo's repeats decay. Repeats that do not (feedback at the top of the
    // range, later lags as strong as the first) are a loop or a steady rhythm
    // of identical sounds, not an effect.
    if (bg >= 0.85 || r2 >= 0.9 * r1)
    {
        e.confidence = 0.0;
        e.delayMs = 1000.0 * tau / sr; // reported as repetition, not applied
        return e;
    }
    // An echo more than 20 dB down is inaudible as an effect (and is what a
    // repeated phrase in the material looks like).
    if (gainToDb(bw) < -20.0)
    {
        e.confidence = 0.0;
        return e;
    }
    e.found = true;
    e.delayMs = 1000.0 * tau / sr;
    e.levelDb = gainToDb(bw);
    e.feedback = bg;
    // Damping: repeats correlate less in the highs than in the lows.
    {
        std::vector<double> loB(n), hiB(n);
        Biquad l1(BiquadCoeffs::lowpass(1500, 0.707, sr)), l2(BiquadCoeffs::lowpass(1500, 0.707, sr));
        Biquad h1(BiquadCoeffs::highpass(3000, 0.707, sr)), h2(BiquadCoeffs::highpass(3000, 0.707, sr));
        for (size_t i = 0; i < n; ++i)
        {
            loB[i] = l2.process(l1.process(wm[i]));
            hiB[i] = h2.process(h1.process(wm[i]));
        }
        const double rl = peakNear(xcorr(loB, loB, k1 + tol + 1), tau, tol), rh = peakNear(xcorr(hiB, hiB, k1 + tol + 1), tau, tol);
        if (rl > 0.05 && rh < 0.75 * rl)
        {
            // One-pole low-pass whose 5 kHz / 700 Hz gain ratio equals rh / rl.
            const double q = std::max(0.05, rh / rl);
            double bestF = 0, bestD = 1e9;
            for (double fc = 300; fc < 20000; fc *= 1.05)
            {
                auto g = [&](double f) { return 1.0 / std::sqrt(1.0 + (f / fc) * (f / fc)); };
                const double d = std::abs(g(5000) / g(700) - q);
                if (d < bestD) { bestD = d; bestF = fc; }
            }
            e.dampingHz = bestF;
        }
    }
    // Ping-pong: on stereo input the repeat shows up across channels.
    if (b.numChannels() == 2)
    {
        std::vector<double> L(b.channel(0), b.channel(0) + n), R(b.channel(1), b.channel(1) + n);
        const auto wl = whiten(L), wr = whiten(R);
        const size_t ml = k1 + tol + 1;
        const double rll = peakNear(xcorr(wl, wl, ml), tau, tol), rrr = peakNear(xcorr(wr, wr, ml), tau, tol);
        const double rlr = peakNear(xcorr(wl, wr, ml), tau, tol), rrl = peakNear(xcorr(wr, wl, ml), tau, tol);
        e.pingPong = std::max(rlr, rrl) > 1.5 * std::max(rll, rrr) && std::max(rlr, rrl) > 0.05;
    }
    return e;
}

BandLimitEstimate measureBandLimit(const AudioBuffer& b)
{
    BandLimitEstimate out;
    const double sr = b.sampleRate;
    const int N = 4096;
    const auto x = monoOf(b);
    if (x.size() < size_t(N)) return out;
    RealFft fft(N);
    const auto win = hannWindow(N);
    std::vector<float> buf(N);
    std::vector<std::complex<float>> X(size_t(fft.numBins()));
    std::vector<std::vector<double>> frames;
    std::vector<double> energy;
    for (size_t s = 0; s + size_t(N) <= x.size(); s += size_t(N / 2))
    {
        double e = 0;
        for (int i = 0; i < N; ++i)
        {
            buf[size_t(i)] = float(x[s + size_t(i)] * win[size_t(i)]);
            e += double(buf[size_t(i)]) * buf[size_t(i)];
        }
        fft.forward(buf.data(), X.data());
        std::vector<double> p(X.size());
        for (size_t k = 0; k < X.size(); ++k) p[k] = std::norm(X[k]);
        frames.push_back(std::move(p));
        energy.push_back(e);
    }
    const double emax = *std::max_element(energy.begin(), energy.end());
    std::vector<double> psd(size_t(fft.numBins()), 0.0);
    for (size_t f = 0; f < frames.size(); ++f)
        if (energy[f] > emax * 1e-3)
            for (size_t k = 0; k < psd.size(); ++k) psd[k] += frames[f][k];
    // 1/12-octave grid, each point the mean power over +/- 1/12 octave.
    std::vector<double> gf, S;
    for (double f = 20.0; f < 0.47 * sr; f *= std::pow(2.0, 1.0 / 12.0))
    {
        const size_t a = size_t(f * std::pow(2.0, -1.0 / 12.0) / sr * N), c = std::min(psd.size() - 1, size_t(f * std::pow(2.0, 1.0 / 12.0) / sr * N) + 1);
        double s = 0;
        for (size_t k = a; k <= c; ++k) s += psd[k];
        gf.push_back(f);
        S.push_back(powerToDb(s / double(c - a + 1), -300.0));
    }
    const size_t n = S.size();
    if (n < 40) return out;
    const double smax = *std::max_element(S.begin(), S.end());
    auto medianOf = [&](long a, long c) {
        std::vector<double> v;
        for (long i = std::max(0L, a); i <= std::min(long(n) - 1, c); ++i) v.push_back(S[size_t(i)]);
        return med(v);
    };
    // A deliberate filter: a steep edge (>= 14 dB within half an octave) at the
    // end of a passband that is itself not falling steeply. Its cutoff is where
    // the asymptote of the edge meets the passband level (true of Butterworth
    // filters), and the asymptote's slope gives the order (6 dB/oct per order).
    auto fitEdge = [&](const std::vector<size_t>& idx, double plateau, double& fc, int& order) {
        std::vector<double> t, y;
        for (size_t j : idx)
        {
            t.push_back(std::log2(gf[j]));
            y.push_back(S[j]);
        }
        if (t.size() < 3) return false;
        const Line l = fitLine(t, y, 0, t.size());
        if (std::abs(l.slope) < 24.0) return false; // not steep enough to be a filter
        fc = std::pow(2.0, (plateau - l.icpt) / l.slope);
        order = clampv(int(std::lround(std::abs(l.slope) / 12.0)) * 2, 2, 8);
        return true;
    };
    for (size_t i = 18; i + 6 < n; ++i)
    {
        if (gf[i] < 1500.0 || gf[i] > 0.42 * sr) continue;
        if (S[i] < smax - 40.0 || S[i] - S[i + 6] < 14.0) continue;
        if (S[i - 12] - S[i] > 15.0) continue;
        if (medianOf(long(i) - 18, long(i)) < smax - 25.0) continue;
        const double plateau = medianOf(long(i) - 30, long(i) - 6); // two octaves of passband
        std::vector<size_t> edge;
        for (size_t j = i > 6 ? i - 6 : 0; j < n && S[j] > plateau - 45.0; ++j)
            if (S[j] <= plateau - 12.0 && S[j] >= plateau - 40.0) edge.push_back(j); // asymptotic part
        double fc = 0;
        int order = 0;
        if (fitEdge(edge, plateau, fc, order) && fc > 1000.0)
        {
            out.lpfHz = fc;
            out.lpfOrder = order;
        }
        break;
    }
    for (size_t i = n - 19; i >= 6; --i)
    {
        if (gf[i] > 1000.0 || gf[i] < 40.0) continue;
        if (S[i] < smax - 40.0 || S[i] - S[i - 6] < 14.0) continue;
        if (S[i + 12] - S[i] > 18.0) continue;
        if (medianOf(long(i), long(i) + 18) < smax - 25.0) continue;
        const double plateau = medianOf(long(i) + 6, long(i) + 30); // two octaves of passband
        std::vector<size_t> edge;
        for (long j = long(i) + 6; j >= 0 && S[size_t(j)] > plateau - 45.0; --j)
            if (S[size_t(j)] <= plateau - 12.0 && S[size_t(j)] >= plateau - 40.0) edge.push_back(size_t(j)); // asymptotic part
        double fc = 0;
        int order = 0;
        if (fitEdge(edge, plateau, fc, order) && fc < 1500.0)
        {
            out.hpfHz = fc;
            out.hpfOrder = order;
        }
        break;
    }
    return out;
}

double eventBodyDb(const AudioBuffer& b)
{
    // Each event's mean level 10-150 ms after its peak (5 ms RMS every 1 ms),
    // re that peak; the median over events. Averaging over the body makes it
    // monotonic in sustain / compression, unlike a single threshold crossing.
    const double hopS = 0.001;
    const auto env = envelopeDb(monoOf(b), b.sampleRate, hopS, 0.005);
    double floorDb = -150;
    const auto segs = decaySegments(env, hopS, floorDb);
    std::vector<double> body;
    for (auto [p, e] : segs)
    {
        const size_t a = p + 10, z = std::min({ e, p + 150, env.size() });
        if (z <= a + 20) continue;
        double s = 0;
        for (size_t j = a; j < z; ++j) s += std::max(env[j], floorDb) - env[p];
        body.push_back(s / double(z - a));
    }
    // Only event-based material (separate hits) has a meaningful body.
    return body.size() < 3 ? 0.0 : med(body);
}

double peakToLoudnessDb(const AudioBuffer& b)
{
    const auto ls = measureLoudness(b, false, false);
    if (std::isfinite(ls.maxMomentary)) return ls.samplePeakDb - ls.maxMomentary;
    double s = 0;
    for (int c = 0; c < b.numChannels(); ++c)
        for (float v : b.vec(c)) s += double(v) * v;
    return ls.samplePeakDb - powerToDb(s / double(std::max<size_t>(1, b.numFrames() * size_t(b.numChannels()))));
}

FxProfile analyzeEffects(const AudioBuffer& ref, const std::string& name, const Job& job)
{
    FxProfile p;
    p.name = name;
    p.sampleRate = ref.sampleRate;
    p.channels = ref.numChannels();
    p.durationS = ref.durationSeconds();
    if (ref.empty()) return p;
    job.report(0.1, "Tone");
    p.spectrum = longTermSpectrum(ref);
    p.bandLimit = measureBandLimit(ref);
    job.report(0.3, "Echo");
    p.echo = measureEcho(ref);
    job.report(0.6, "Reverb");
    if (p.echo.found)
    {
        // Echo repeats would read as a slower decay: measure the reverb on the
        // reference with the echo removed by its exact inverse,
        // (1 - g z^-D) / (1 + (w - g) z^-D) for h = 1 + w sum g^(k-1) z^(-kD).
        AudioBuffer dry = ref;
        const size_t D = size_t(std::lround(p.echo.delayMs * 0.001 * ref.sampleRate));
        const double w = dbToGain(p.echo.levelDb), g = p.echo.feedback;
        if (std::abs(w - g) < 0.95)
            for (int c = 0; c < dry.numChannels(); ++c)
            {
                const float* x = ref.channel(c);
                float* y = dry.channel(c);
                for (size_t i = 0; i < dry.numFrames(); ++i)
                {
                    double v = x[i];
                    if (i >= D) v += -g * x[i - D] - (w - g) * y[i - D];
                    y[i] = float(v);
                }
            }
        p.reverb = measureReverb(dry);
    }
    else p.reverb = measureReverb(ref);
    throwIfCancelled(job);
    p.loudness = measureLoudness(ref, true, false);
    p.peakDb = p.loudness.samplePeakDb;
    p.plrDb = peakToLoudnessDb(ref);
    p.crestDb = shortTermCrestDb(ref);
    p.bodyDb = eventBodyDb(ref);
    if (ref.numChannels() == 2)
    {
        double m = 0, s = 0;
        for (size_t i = 0; i < ref.numFrames(); ++i)
        {
            const double l = ref.channel(0)[i], r = ref.channel(1)[i];
            m += 0.25 * (l + r) * (l + r);
            s += 0.25 * (l - r) * (l - r);
        }
        p.sideToMidDb = powerToDb(s / std::max(m, 1e-30), -100.0);
        p.stereo = p.sideToMidDb > -40.0;
    }
    if (p.reverb.found && p.reverb.segments < 2)
        p.notes.push_back("Reverb measured on a single decay: a slowly ringing source would read the same way.");
    if (p.echo.found && p.echo.confidence < 0.5)
        p.notes.push_back("The echo peak is modest: a repeated sample would read the same way.");
    if (!p.reverb.found && p.reverb.segments == 0)
        p.notes.push_back("No free decay in the reference (sustained sound): reverb cannot be measured from it.");
    job.report(1.0, "Done");
    return p;
}

std::string describeEffects(const FxProfile& p)
{
    std::string s;
    if (p.bandLimit.hpfHz > 0 || p.bandLimit.lpfHz > 0)
        s += fmt("Band limit: %s%s\n", p.bandLimit.hpfHz > 0 ? fmt("high-pass %.0f Hz (%d dB/oct) ", p.bandLimit.hpfHz, 6 * p.bandLimit.hpfOrder).c_str() : "",
                 p.bandLimit.lpfHz > 0 ? fmt("low-pass %.0f Hz (%d dB/oct)", p.bandLimit.lpfHz, 6 * p.bandLimit.lpfOrder).c_str() : "");
    else s += "Band limit: none\n";
    if (p.reverb.found)
    {
        std::string bands;
        for (size_t i = 0; i < p.reverb.bandHz.size(); ++i)
            if (p.reverb.bandRt60[i] > 0) bands += fmt(" %.0f:%.2f", p.reverb.bandHz[i], p.reverb.bandRt60[i]);
        s += fmt("Reverb: RT60 %.2f s (per band Hz:s%s), tail %.1f dB re the event, %d decays, confidence %.0f%%\n", p.reverb.rt60, bands.c_str(),
                 p.reverb.tailLevelDb, p.reverb.segments, 100 * p.reverb.confidence);
    }
    else s += fmt("Reverb: none found (%d decays measured%s)\n", p.reverb.segments, p.reverb.segments ? fmt(", RT60 %.2f s", p.reverb.rt60).c_str() : "");
    if (p.echo.found)
        s += fmt("Echo: %.1f ms, first repeat %.1f dB, feedback %.2f%s%s, confidence %.0f%%\n", p.echo.delayMs, p.echo.levelDb, p.echo.feedback,
                 p.echo.pingPong ? ", ping-pong" : "", p.echo.dampingHz > 0 ? fmt(", damped above %.0f Hz", p.echo.dampingHz).c_str() : "",
                 100 * p.echo.confidence);
    else s += "Echo: none found\n";
    s += fmt("Dynamics: event body %.1f dB re peak (sustain), short-term crest %.1f dB, peak-to-loudness %.1f dB; max momentary %.1f LUFS, "
             "peak %.1f dBFS\n",
             p.bodyDb, p.crestDb, p.plrDb, p.loudness.maxMomentary, p.peakDb);
    s += p.stereo ? fmt("Stereo: side/mid %.1f dB\n", p.sideToMidDb) : std::string("Stereo: mono\n");
    for (auto& n : p.notes) s += "Note: " + n + "\n";
    return s;
}

// ===================================================================== Processors
std::vector<EqBand> butterworthBands(SvfType type, double freq, int order)
{
    std::vector<EqBand> out;
    const int n = std::max(2, order + (order & 1));
    for (double q : butterworthQs(n)) out.push_back({ type, freq, q, 0.0, true });
    return out;
}

std::vector<float> ConvolutionReverb::impulseResponse(const ReverbSettings& s, double sr, int channel)
{
    // Decay time per frequency: log-frequency interpolation of the band values.
    auto rtAt = [&](double f) {
        std::vector<std::pair<double, double>> pts;
        for (size_t i = 0; i < std::min(s.bandHz.size(), s.bandRt60.size()); ++i)
            if (s.bandRt60[i] > 0.05) pts.emplace_back(s.bandHz[i], s.bandRt60[i]);
        if (pts.empty()) return std::max(0.05, s.rt60);
        if (f <= pts.front().first) return pts.front().second;
        if (f >= pts.back().first) return pts.back().second;
        for (size_t i = 1; i < pts.size(); ++i)
            if (f <= pts[i].first)
            {
                const double u = std::log(f / pts[i - 1].first) / std::log(pts[i].first / pts[i - 1].first);
                return pts[i - 1].second * std::pow(pts[i].second / pts[i - 1].second, u);
            }
        return pts.back().second;
    };
    double maxRt = std::max(0.05, s.rt60);
    for (double v : s.bandRt60) maxRt = std::max(maxRt, v);
    maxRt = std::min(maxRt, 12.0);
    // STFT synthesis: complex Gaussian noise with a per-bin exponential decay.
    const int N = 1024, hop = 256;
    const size_t len = size_t(maxRt * 1.1 * sr) + size_t(N);
    std::vector<float> h(len + size_t(N), 0.0f);
    RealFft fft(N);
    std::vector<float> win(N), frame(N);
    for (int i = 0; i < N; ++i) win[size_t(i)] = float(std::sqrt(0.5 - 0.5 * std::cos(kTwoPi * i / N)));
    std::vector<double> decayPerHop(size_t(fft.numBins()));
    for (int k = 0; k < fft.numBins(); ++k)
    {
        const double f = std::max(20.0, double(k) * sr / N);
        decayPerHop[size_t(k)] = std::exp(-6.907755 * double(hop) / sr / rtAt(f)); // amplitude per hop
    }
    std::mt19937 rng(s.seed + uint32_t(channel) * 7919u);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<std::complex<float>> X(size_t(fft.numBins()));
    std::vector<double> amp(size_t(fft.numBins()), 1.0);
    for (size_t pos = 0; pos + size_t(N) <= h.size(); pos += size_t(hop))
    {
        bool any = false;
        for (size_t k = 0; k < X.size(); ++k)
        {
            X[k] = std::complex<float>(nd(rng), nd(rng)) * float(amp[k]);
            amp[k] *= decayPerHop[k];
            any |= amp[k] > 1e-5;
        }
        X[0] = 0.0f;
        fft.inverse(X.data(), frame.data());
        for (int i = 0; i < N; ++i) h[pos + size_t(i)] += frame[size_t(i)] * win[size_t(i)];
        if (!any) break;
    }
    h.resize(len);
    // 10 ms build-up, pre-delay, unit energy.
    const size_t ramp = size_t(0.01 * sr);
    for (size_t i = 0; i < std::min(ramp, h.size()); ++i) h[i] *= float(double(i) / double(ramp));
    double e = 0;
    for (float v : h) e += double(v) * v;
    const float g = float(1.0 / std::sqrt(std::max(e, 1e-30)));
    for (auto& v : h) v *= g;
    const size_t pre = size_t(std::max(0.0, s.preDelayMs) * 0.001 * sr);
    h.insert(h.begin(), pre, 0.0f);
    // Trim the decayed end (below -100 dB of the start energy density).
    size_t end = h.size();
    while (end > pre + ramp && std::abs(h[end - 1]) < 1e-6f) --end;
    h.resize(end);
    return h;
}

void ConvolutionReverb::processOffline(AudioBuffer& b, const Job& job)
{
    const int nch = b.numChannels();
    const size_t n = b.numFrames();
    if (n == 0) return;
    const double wet = dbToGain(s_.wetDb), dry = dbToGain(s_.dryDb);
    std::vector<std::vector<float>> irs;
    for (int c = 0; c < nch; ++c) irs.push_back(impulseResponse(s_, b.sampleRate, s_.stereo ? c : 0));
    const size_t L = irs[0].size();
    AudioBuffer out(nch, n + L, b.sampleRate);
    // Overlap-add: blocks of B samples against the whole IR in a 2B FFT (B >= L).
    const size_t B = nextPow2(std::max<size_t>(L, 4096));
    RealFft fft{ int(2 * B) };
    std::vector<float> buf(2 * B);
    std::vector<std::complex<float>> H(size_t(fft.numBins())), X(size_t(fft.numBins()));
    for (int c = 0; c < nch; ++c)
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        std::copy(irs[size_t(c)].begin(), irs[size_t(c)].end(), buf.begin());
        fft.forward(buf.data(), H.data());
        const float* x = b.channel(c);
        float* y = out.channel(c);
        for (size_t i = 0; i < n; ++i) y[i] = float(dry * x[i]);
        for (size_t s = 0; s < n; s += B)
        {
            std::fill(buf.begin(), buf.end(), 0.0f);
            const size_t m = std::min(B, n - s);
            for (size_t i = 0; i < m; ++i) buf[i] = x[s + i];
            fft.forward(buf.data(), X.data());
            for (size_t k = 0; k < X.size(); ++k) X[k] *= H[k];
            fft.inverse(X.data(), buf.data());
            for (size_t i = 0; i < m + L - 1 && s + i < out.numFrames(); ++i) y[s + i] += float(wet * buf[i]);
            throwIfCancelled(job);
        }
        job.report(double(c + 1) / nch, "Reverb");
    }
    trimSilentTail(out, n, -100.0);
    b = std::move(out);
}

void EchoProcessor::processOffline(AudioBuffer& b, const Job& job)
{
    const int nch = b.numChannels();
    const size_t n = b.numFrames();
    const size_t D = std::max<size_t>(1, size_t(std::lround(s_.delayMs * 0.001 * b.sampleRate)));
    const double fb = clampv(s_.feedback, 0.0, 0.95), lvl = dbToGain(s_.levelDb);
    // Repeats until they are 90 dB down.
    int repeats = 1;
    if (fb > 0.001)
    {
        const double k = std::log(1e-4 / std::max(lvl, 1e-6)) / std::log(fb);
        repeats = clampv(int(std::ceil(k)) + 1, 1, 200);
    }
    const size_t total = n + size_t(repeats) * D;
    AudioBuffer out(nch, total, b.sampleRate);
    const double a = s_.dampingHz > 0 ? std::exp(-kTwoPi * s_.dampingHz / b.sampleRate) : 0.0;
    const bool pp = s_.pingPong && nch == 2;
    // Echo lines e_c[t] = LP(in_c[t - D] + fb * e_src[t - D]); out = x + lvl * e.
    std::vector<std::vector<double>> e(size_t(nch), std::vector<double>(total, 0.0));
    std::vector<double> lp(size_t(nch), 0.0);
    for (size_t t = D; t < total; ++t)
    {
        for (int c = 0; c < nch; ++c)
        {
            double in;
            if (pp) in = c == 0 && t - D < n ? 0.5 * (b.channel(0)[t - D] + b.channel(1)[t - D]) : 0.0;
            else in = t - D < n ? b.channel(c)[t - D] : 0.0;
            const int src = pp ? 1 - c : c;
            // Each pass through the line is damped once (first repeat included).
            double v = in + fb * e[size_t(src)][t - D];
            if (a > 0) v = lp[size_t(c)] = (1.0 - a) * v + a * lp[size_t(c)];
            e[size_t(c)][t] = v;
        }
        if ((t & 65535) == 0) throwIfCancelled(job);
    }
    for (int c = 0; c < nch; ++c)
        for (size_t t = 0; t < total; ++t) out.channel(c)[t] = float((t < n ? b.channel(c)[t] : 0.0) + lvl * e[size_t(c)][t]);
    trimSilentTail(out, n, -100.0);
    b = std::move(out);
}

} // namespace ac
