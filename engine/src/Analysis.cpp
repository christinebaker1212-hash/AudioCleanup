#include "ac/Analysis.h"

#include "ac/Dynamics.h"
#include "ac/Fft.h"
#include "ac/Resampler.h"

#include <cstdio>
#include <sstream>

namespace ac {

const char* categoryName(Category c)
{
    switch (c)
    {
        case Category::Voice: return "Voice Clip";
        case Category::SoundEffect: return "Sound Effect";
        case Category::Music: return "Music";
    }
    return "?";
}

namespace {

int pow2Near(double target)
{
    int n = 256;
    while (n < target * 0.75) n *= 2;
    return n;
}

// ------------------------------------------------------------------ hum
HumInfo detectHum(const std::vector<float>& mono, double sr)
{
    HumInfo info;
    const int N = clampv(pow2Near(sr / 1.4), 4096, 65536);
    if (mono.size() < size_t(N)) return info;
    RealFft fft(N);
    const auto win = hannWindow(N);
    const int bins = N / 2 + 1;
    std::vector<double> acc(static_cast<size_t>(bins), 0.0);
    std::vector<float> fr(static_cast<size_t>(N));
    std::vector<std::complex<float>> sp(static_cast<size_t>(bins));
    const size_t avail = mono.size() - size_t(N);
    const int frames = int(std::min<size_t>(120, avail / size_t(N / 2) + 1));
    for (int f = 0; f < frames; ++f)
    {
        const size_t s = frames == 1 ? 0 : avail * size_t(f) / size_t(frames - 1);
        for (int i = 0; i < N; ++i) fr[size_t(i)] = mono[s + size_t(i)] * win[size_t(i)];
        fft.forward(fr.data(), sp.data());
        for (int k = 0; k < bins; ++k) acc[size_t(k)] += std::norm(sp[size_t(k)]);
    }
    const double binHz = sr / N;
    auto prominence = [&](double f, double& peakFreq, double searchHz) {
        const int kc = int(std::lround(f / binHz));
        const int search = std::max(1, int(std::ceil(searchHz / binHz)));
        int kp = kc;
        for (int k = kc - search; k <= kc + search; ++k)
            if (k > 1 && k < bins - 1 && acc[size_t(k)] > acc[size_t(kp)]) kp = k;
        // parabolic interpolation on dB
        const double a = powerToDb(acc[size_t(kp - 1)]), b = powerToDb(acc[size_t(kp)]), c = powerToDb(acc[size_t(kp + 1)]);
        const double den = a - 2 * b + c;
        const double delta = std::abs(den) > 1e-12 ? clampv(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
        peakFreq = (kp + delta) * binHz;
        std::vector<double> nb;
        const int span = std::max(6, int(15.0 / binHz));
        for (int k = kp - span; k <= kp + span; ++k)
            if (k > 0 && k < bins && std::abs(k - kp) > 3) nb.push_back(acc[size_t(k)]);
        if (nb.empty()) return 0.0;
        return powerToDb(acc[size_t(kp)]) - powerToDb(median(nb));
    };
    double bestScore = 0;
    for (double nominal : { 50.0, 60.0 })
    {
        // Pass 1: refine f0 from the low harmonics (mains tolerance +/-1.2 %).
        double fsum = 0, wsum = 0;
        for (int k = 1; k <= 4; ++k)
        {
            double pf = 0;
            const double p = prominence(k * nominal, pf, std::max(2.0 * binHz, 0.012 * k * nominal));
            if (p > 10.0) { fsum += (pf / k) * p; wsum += p; }
        }
        if (wsum <= 0) continue;
        HumInfo cand;
        cand.f0 = fsum / wsum;
        // Pass 2: genuine mains harmonics sit at exact multiples of f0; accept
        // a peak only within +/-(0.5 Hz + k * 0.03 Hz) of k * f0 so programme
        // tones that happen to lie near a harmonic are never notched.
        double score = 0;
        int strong = 0;
        for (int k = 1; k * cand.f0 < std::min(1500.0, 0.45 * sr); ++k)
        {
            double pf = 0;
            const double tol = 0.5 + 0.03 * k + binHz * 0.25;
            const double p = prominence(k * cand.f0, pf, tol + binHz);
            if (std::abs(pf - k * cand.f0) > tol) continue;
            if (p > 6.0)
            {
                cand.harmonics.push_back({ pf, p });
                score += p - 6.0;
            }
            if (p > 10.0 && k <= 8) ++strong;
            if (k <= 2 && p > 15.0) strong += 2;
        }
        cand.detected = strong >= 2;
        if (cand.detected && score > bestScore)
        {
            bestScore = score;
            info = cand;
        }
    }
    // Re-centre harmonics on the refined fundamental (mains drift is shared).
    for (auto& h : info.harmonics)
    {
        const double k = std::round(h.freq / info.f0);
        if (std::abs(h.freq - k * info.f0) < 0.006 * h.freq) h.freq = 0.5 * (h.freq + k * info.f0);
    }
    return info;
}

// --------------------------------------------------------------- pitch
std::vector<double> pitchTrack(const std::vector<float>& mono, double sr, const std::vector<float>& vad, double gateRms)
{
    std::vector<double> f0s;
    AudioBuffer m(1, mono.size(), sr);
    m.vec(0) = mono;
    AudioBuffer d = resample(m, 16000.0);
    const auto& x = d.vec(0);
    const int W = 640, hop = 320, tmin = 40, tmax = 267;
    if (x.size() < size_t(W + tmax + 1)) return f0s;
    const size_t nFrames = (x.size() - size_t(W + tmax)) / size_t(hop);
    const size_t stride = std::max<size_t>(1, nFrames / 1500);
    std::vector<double> dd(static_cast<size_t>(tmax + 1));
    for (size_t f = 0; f < nFrames; f += stride)
    {
        const size_t s = f * size_t(hop);
        const double tSec = (double(s) + W / 2.0) / 16000.0;
        if (!vad.empty())
        {
            const size_t vi = std::min(vad.size() - 1, size_t(tSec * 100.0));
            if (vad[vi] < 0.7f) continue;
        }
        double e = 0;
        for (int j = 0; j < W; ++j) e += double(x[s + size_t(j)]) * x[s + size_t(j)];
        if (std::sqrt(e / W) < gateRms) continue;
        double run = 0;
        dd[0] = 1;
        int found = -1;
        for (int t = 1; t <= tmax; ++t)
        {
            double sum = 0;
            for (int j = 0; j < W; ++j)
            {
                const double v = double(x[s + size_t(j)]) - x[s + size_t(j + t)];
                sum += v * v;
            }
            run += sum;
            dd[size_t(t)] = run > 0 ? sum * t / run : 1.0;
        }
        for (int t = tmin; t < tmax; ++t)
            if (dd[size_t(t)] < 0.15)
            {
                while (t + 1 < tmax && dd[size_t(t + 1)] < dd[size_t(t)]) ++t;
                found = t;
                break;
            }
        if (found < 0) continue;
        const double a = dd[size_t(found - 1)], b = dd[size_t(found)], c = dd[size_t(found + 1)];
        const double den = a - 2 * b + c;
        const double tau = found + (std::abs(den) > 1e-12 ? clampv(0.5 * (a - c) / den, -0.5, 0.5) : 0.0);
        f0s.push_back(16000.0 / tau);
    }
    return f0s;
}

// ------------------------------------------------------------ transients
TransientInfo transientInfo(const AudioBuffer& b, double noiseDb, const LoudnessStats& ls)
{
    TransientInfo t;
    const double sr = b.sampleRate;
    const size_t hop = std::max<size_t>(1, size_t(sr * 0.001));
    const size_t hops = b.numFrames() / hop;
    if (hops < 5) return t;
    std::vector<double> env(hops, 0.0);
    for (size_t h = 0; h < hops; ++h)
    {
        double m = 0;
        for (int c = 0; c < b.numChannels(); ++c)
            for (size_t i = h * hop; i < (h + 1) * hop; ++i) m = std::max(m, double(std::abs(b.channel(c)[i])));
        env[h] = gainToDb(m, -200.0);
    }
    const size_t pk = size_t(std::max_element(env.begin(), env.end()) - env.begin());
    const double peak = env[pk];
    size_t on = pk;
    while (on > 0 && pk - on < 500 && env[on - 1] > peak - 30.0) --on;
    size_t reach = on;
    while (reach < pk && env[reach] < peak - 1.0) ++reach;
    t.attackMs = double(reach - on);
    // decay using a 10 ms running max to ignore waveform cycles
    size_t dk = pk;
    while (dk < hops)
    {
        double m = -200;
        for (size_t j = dk; j < std::min(hops, dk + 10); ++j) m = std::max(m, env[j]);
        if (m < peak - 30.0) break;
        ++dk;
    }
    t.decayMs = double(dk - pk);
    const double floorDb = std::max(noiseDb + 6.0, peak - 50.0);
    size_t active = 0;
    double esum = 0;
    for (double v : env)
        if (v > floorDb)
        {
            ++active;
            esum += std::pow(10.0, v / 10.0);
        }
    t.activeDurationS = double(active) * 0.001;
    t.envelopeCrestDb = active ? peak - powerToDb(esum / double(active)) : 0.0;
    // onsets: 10 ms RMS rise of > 9 dB within 30 ms
    std::vector<double> r10;
    for (size_t h = 0; h + 10 <= hops; h += 10)
    {
        double m = -200;
        for (size_t j = h; j < h + 10; ++j) m = std::max(m, env[j]);
        r10.push_back(m);
    }
    size_t last = 0;
    bool any = false;
    for (size_t i = 3; i < r10.size(); ++i)
    {
        const double prev = std::min({ r10[i - 1], r10[i - 2], r10[i - 3] });
        if (r10[i] - prev > 9.0 && r10[i] > peak - 40.0 && (!any || i - last >= 5))
        {
            ++t.onsetCount;
            last = i;
            any = true;
        }
    }
    if (t.onsetCount == 0 && peak > -100) t.onsetCount = 1;
    t.peakToLoudnessDb = std::isfinite(ls.maxMomentary) ? ls.truePeakDb - ls.maxMomentary : 0.0;
    return t;
}

// --------------------------------------------------------------- stereo
StereoInfo stereoInfo(const AudioBuffer& b)
{
    StereoInfo s;
    if (b.numChannels() != 2) return s;
    s.isStereo = true;
    const float* L = b.channel(0);
    const float* R = b.channel(1);
    const size_t n = b.numFrames();
    double ll = 0, rr = 0, lr = 0, mm = 0, ss = 0, maxDiff = 0, peak = 0;
    Biquad lpL(BiquadCoeffs::lowpass(150.0, 0.707, b.sampleRate)), lpR(BiquadCoeffs::lowpass(150.0, 0.707, b.sampleRate));
    double lll = 0, lrr = 0, llr = 0;
    std::vector<double> corr;
    const size_t hop = size_t(b.sampleRate * 0.1);
    double fl = 0, fr = 0, flr = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double l = L[i], r = R[i];
        ll += l * l; rr += r * r; lr += l * r;
        const double m = 0.5 * (l + r), sd = 0.5 * (l - r);
        mm += m * m; ss += sd * sd;
        maxDiff = std::max(maxDiff, std::abs(l - r));
        peak = std::max({ peak, std::abs(l), std::abs(r) });
        const double a = lpL.process(l), c = lpR.process(r);
        lll += a * a; lrr += c * c; llr += a * c;
        fl += l * l; fr += r * r; flr += l * r;
        if (hop > 0 && (i + 1) % hop == 0)
        {
            if (fl / double(hop) > 1e-6 && fr / double(hop) > 1e-6) corr.push_back(flr / std::sqrt(fl * fr));
            fl = fr = flr = 0;
        }
    }
    s.dualMono = maxDiff <= 1e-6 * std::max(peak, 1e-9) || maxDiff < 1e-9;
    s.correlation = (ll > 0 && rr > 0) ? lr / std::sqrt(ll * rr) : 1.0;
    s.lowCorrelation = (lll > 0 && lrr > 0) ? llr / std::sqrt(lll * lrr) : 1.0;
    s.sideToMidDb = powerToDb(ss, -200.0) - powerToDb(mm, -200.0);
    s.balanceDb = powerToDb(rr, -200.0) - powerToDb(ll, -200.0);
    s.correlationP5 = corr.empty() ? s.correlation : percentile(corr, 5.0);
    return s;
}

// -------------------------------------------------------------- spectrum
SpectrumInfo spectrumInfo(const AudioBuffer& b, const NoiseProfile& noise)
{
    SpectrumInfo si;
    const double sr = b.sampleRate;
    const int N = clampv(pow2Near(0.085 * sr), 1024, 16384);
    if (b.numFrames() < size_t(N)) return si;
    RealFft fft(N);
    const auto win = hannWindow(N);
    const int bins = N / 2 + 1;
    std::vector<float> fr(static_cast<size_t>(N));
    std::vector<std::complex<float>> sp(static_cast<size_t>(bins));
    const size_t hop = size_t(N / 2);
    const size_t nFrames = (b.numFrames() - size_t(N)) / hop + 1;
    const size_t stride = std::max<size_t>(1, nFrames / 3000);
    std::vector<std::vector<float>> spectra;
    std::vector<double> energies;
    for (size_t f = 0; f < nFrames; f += stride)
    {
        std::vector<float> p(static_cast<size_t>(bins), 0.0f);
        double e = 0;
        for (int c = 0; c < b.numChannels(); ++c)
        {
            const float* x = b.channel(c) + f * hop;
            for (int i = 0; i < N; ++i) fr[size_t(i)] = x[i] * win[size_t(i)];
            fft.forward(fr.data(), sp.data());
            for (int k = 0; k < bins; ++k)
            {
                const float v = std::norm(sp[size_t(k)]) / float(b.numChannels());
                p[size_t(k)] += v;
                e += v;
            }
        }
        spectra.push_back(std::move(p));
        energies.push_back(e);
    }
    const double thr = std::max(percentile(energies, 30.0), 1e-14);
    std::vector<double> ltas(static_cast<size_t>(bins), 0.0);
    int used = 0;
    for (size_t i = 0; i < spectra.size(); ++i)
        if (energies[i] >= thr)
        {
            for (int k = 0; k < bins; ++k) ltas[size_t(k)] += spectra[i][size_t(k)];
            ++used;
        }
    if (used == 0) return si;
    // Convert |X|^2 sums to band variance: 2|X|^2 / (N * sum(w^2)), sum(w^2) = 3N/8.
    const double norm = 2.0 / (double(N) * 0.375 * N) / used;
    for (auto& v : ltas) v *= norm;
    const double binHz = sr / N;
    // Noise profile (sqrt-Hann, sum(w^2) = N/2) in the same units.
    const int nN = noise.fftSize;
    const double nNorm = 2.0 / (double(nN) * 0.5 * nN);
    auto bandPow = [&](const std::vector<double>& v, double bhz, double lo, double hi) {
        double s = 0;
        for (int k = std::max(1, int(std::ceil(lo / bhz))); k <= std::min(int(v.size()) - 1, int(std::floor(hi / bhz))); ++k) s += v[size_t(k)];
        return s;
    };
    std::vector<double> nps;
    if (noise.valid)
        for (float v : noise.psd) nps.push_back(double(v) * nNorm);
    for (double fc = 25.0; fc <= 16000.0 * 1.01 && fc < 0.45 * sr; fc *= std::pow(2.0, 1.0 / 3.0))
    {
        const double lo = fc * std::pow(2.0, -1.0 / 6.0), hi = fc * std::pow(2.0, 1.0 / 6.0);
        si.bandHz.push_back(fc);
        si.bandAbsDb.push_back(powerToDb(bandPow(ltas, binHz, lo, hi), -200.0));
        si.noiseBandDb.push_back(nps.empty() ? -200.0 : powerToDb(bandPow(nps, sr / nN, lo, hi), -200.0));
    }
    double mean = 0;
    int cnt = 0;
    for (size_t i = 0; i < si.bandHz.size(); ++i)
        if (si.bandHz[i] >= 100 && si.bandHz[i] <= 10000) { mean += si.bandAbsDb[i]; ++cnt; }
    mean = cnt ? mean / cnt : 0.0;
    for (double v : si.bandAbsDb) si.bandDb.push_back(v - mean);
    // tilt regression
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int m = 0;
    for (size_t i = 0; i < si.bandHz.size(); ++i)
        if (si.bandHz[i] >= 100 && si.bandHz[i] <= 10000)
        {
            const double x = std::log2(si.bandHz[i]), y = si.bandDb[i];
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++m;
        }
    if (m > 2) si.tiltDbPerOct = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    si.subsonicRelDb = powerToDb(bandPow(ltas, binHz, 1, 20), -200) - powerToDb(bandPow(ltas, binHz, 40, 200), -200);
    si.rumbleRelDb = powerToDb(bandPow(ltas, binHz, 1, 60), -200) - powerToDb(bandPow(ltas, binHz, 100, 500), -200);
    // 1/12-octave smoothed fine LTAS (1/24-octave grid) for resonance detection.
    for (double f = 40.0; f < std::min(16000.0, 0.45 * sr); f *= std::pow(2.0, 1.0 / 24.0))
    {
        const double lo = f * std::pow(2.0, -1.0 / 24.0), hi = f * std::pow(2.0, 1.0 / 24.0);
        const double bw = std::max(hi - lo, binHz);
        const double p = bandPow(ltas, binHz, f - bw / 2, f + bw / 2) / std::max(1.0, bw / binHz);
        si.fineHz.push_back(f);
        si.fineDb.push_back(powerToDb(p, -200.0));
    }
    return si;
}

// -------------------------------------------------------------- rt60
double estimateRt60(const std::vector<float>& mono, double sr, double /*noiseDb*/)
{
    const size_t hop = size_t(sr * 0.01);
    if (hop == 0 || mono.size() < hop * 50) return 0.0;
    const size_t hops = mono.size() / hop;
    std::vector<double> pw(hops), env(hops);
    for (size_t h = 0; h < hops; ++h)
    {
        double s = 0;
        for (size_t i = h * hop; i < (h + 1) * hop; ++i) s += double(mono[i]) * mono[i];
        pw[h] = s / double(hop);
    }
    // 30 ms power smoothing: decaying reverberation is noise-like and its
    // 10 ms energy fluctuates by several dB frame to frame.
    for (size_t h = 0; h < hops; ++h)
    {
        const size_t a = h > 0 ? h - 1 : 0, b = std::min(hops - 1, h + 1);
        env[h] = powerToDb((pw[a] + pw[h] + pw[b]) / 3.0, -200.0);
    }
    // Floor from the quietest 5 % of 10 ms frames (the noise profile would
    // include the reverb tail itself and hide the decays).
    std::vector<double> sorted(env.begin(), env.end());
    const double floorDb = percentile(sorted, 5.0);
    std::vector<double> rts;
    size_t h = 5;
    while (h + 10 < hops)
    {
        bool isPeak = env[h] > floorDb + 25.0;
        for (size_t j = h - 5; j <= h + 5 && isPeak; ++j)
            if (env[j] > env[h]) isPeak = false;
        if (!isPeak) { ++h; continue; }
        const double start = env[h];
        size_t j = h + 1;
        double minSoFar = start;
        while (j < hops && env[j] < minSoFar + 2.0 && env[j] > std::max(start - 30.0, floorDb + 3.0))
        {
            minSoFar = std::min(minSoFar, env[j]);
            ++j;
        }
        if (minSoFar <= start - 12.0)
        {
            // regression over -3..-(up to 20) dB: early decay of free decays
            // (continuous speech rarely leaves more than ~15 dB of free decay).
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int m = 0;
            for (size_t k = h; k < j; ++k)
                if (env[k] <= start - 3.0 && env[k] >= std::max(start - 20.0, minSoFar))
                {
                    const double x = double(k - h) * 0.01;
                    sx += x; sy += env[k]; sxx += x * x; sxy += x * env[k]; ++m;
                }
            if (m >= 4)
            {
                const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
                if (slope < -5.0) rts.push_back(-60.0 / slope);
            }
        }
        h = j + 1;
    }
    if (rts.size() < 5) return 0.0;
    return clampv(median(rts), 0.1, 3.0);
}

} // namespace

std::vector<double> bandLevelPercentiles(const AudioBuffer& b, SvfType detector, double freq, double q, double gateDb)
{
    const double sr = b.sampleRate;
    std::vector<Svf> det(static_cast<size_t>(b.numChannels()));
    for (auto& d : det) d.set(detector, freq, q, 0.0, sr);
    const double a = timeCoeff(5.0, sr);
    double env = 0;
    std::vector<double> lv;
    const size_t hop = std::max<size_t>(1, size_t(sr * 0.005));
    for (size_t i = 0; i < b.numFrames(); ++i)
    {
        double pw = 0;
        for (int c = 0; c < b.numChannels(); ++c)
        {
            const double v = det[size_t(c)].process(b.channel(c)[i]);
            pw = std::max(pw, v * v);
        }
        env = a * env + (1 - a) * pw;
        if (i % hop == 0)
        {
            const double d = powerToDb(env, -200.0);
            if (d > gateDb) lv.push_back(d);
        }
    }
    if (lv.empty()) return { -200, -200, -200, -200, -200 };
    return { percentile(lv, 50), percentile(lv, 75), percentile(lv, 90), percentile(lv, 95), percentile(lv, 99) };
}

LevelStats rmsLevelStats(const AudioBuffer& b, double windowMs, double gateDb, double hpfHz)
{
    LevelStats st;
    const double sr = b.sampleRate;
    const double a = timeCoeff(windowMs, sr);
    std::vector<double> rms(static_cast<size_t>(b.numChannels()), 0.0);
    std::vector<Biquad> hp(static_cast<size_t>(b.numChannels()));
    if (hpfHz > 0)
        for (auto& h : hp) h.setCoeffs(BiquadCoeffs::highpass(hpfHz, 0.707, sr));
    std::vector<double> lv;
    const size_t hop = std::max<size_t>(1, size_t(sr * 0.01));
    size_t total = 0;
    for (size_t i = 0; i < b.numFrames(); ++i)
    {
        double mx = 0;
        for (int c = 0; c < b.numChannels(); ++c)
        {
            double x = b.channel(c)[i];
            if (hpfHz > 0) x = hp[size_t(c)].process(x);
            rms[size_t(c)] = a * rms[size_t(c)] + (1 - a) * x * x;
            mx = std::max(mx, rms[size_t(c)]);
        }
        if (i % hop == 0)
        {
            ++total;
            const double d = powerToDb(mx, -200.0);
            if (d > gateDb) lv.push_back(d);
        }
    }
    if (lv.empty()) return st;
    st.p10 = percentile(lv, 10); st.p50 = percentile(lv, 50); st.p75 = percentile(lv, 75);
    st.p90 = percentile(lv, 90); st.p95 = percentile(lv, 95); st.p99 = percentile(lv, 99);
    st.activeFraction = double(lv.size()) / double(std::max<size_t>(1, total));
    return st;
}

AnalysisReport analyze(const AudioBuffer& b, Category cat, const Job& job)
{
    AnalysisReport r;
    r.sampleRate = b.sampleRate;
    r.channels = b.numChannels();
    r.frames = b.numFrames();
    r.durationS = b.durationSeconds();
    if (b.empty())
    {
        r.silent = true;
        r.notes.push_back("Empty file.");
        return r;
    }
    job.report(0.02, "Loudness");
    r.loudness = measureLoudness(b);
    r.veryShort = r.durationS < 0.4;

    double sumSq = 0;
    r.dcOffset.assign(size_t(b.numChannels()), 0.0);
    for (int c = 0; c < b.numChannels(); ++c)
    {
        double s = 0, q = 0;
        for (float v : b.vec(c)) { s += v; q += double(v) * v; }
        r.dcOffset[size_t(c)] = s / double(b.numFrames());
        r.dcOffsetMaxDb = std::max(r.dcOffsetMaxDb, gainToDb(std::abs(r.dcOffset[size_t(c)])));
        sumSq += q;
    }
    r.rmsDb = powerToDb(sumSq / double(b.numFrames() * size_t(b.numChannels())));
    r.crestDb = r.loudness.samplePeakDb - r.rmsDb;
    r.plrDb = r.loudness.integratedValid() ? r.loudness.truePeakDb - r.loudness.integrated : 0.0;
    r.silent = r.loudness.samplePeakDb < -90.0;
    if (r.silent)
    {
        r.notes.push_back("Signal is silent (peak below -90 dBFS): processing will pass it through unchanged.");
        return r;
    }
    if (r.veryShort) r.notes.push_back("Very short clip (< 400 ms): integrated loudness is undefined; peak/energy metrics are used.");

    // Clipping: flat runs at the absolute maximum.
    for (int c = 0; c < b.numChannels(); ++c)
    {
        const auto& x = b.vec(c);
        float m = 0;
        for (float v : x) m = std::max(m, std::abs(v));
        if (m < 0.1f) continue;
        const float thr = m * 0.99999f;
        size_t i = 0, clipped = 0;
        while (i < x.size())
        {
            if (std::abs(x[i]) < thr) { ++i; continue; }
            size_t j = i;
            while (j < x.size() && std::abs(x[j]) >= thr && (x[j] > 0) == (x[i] > 0)) ++j;
            if (j - i >= 3) { ++r.clipping.runs; clipped += j - i; }
            i = j;
        }
        r.clipping.level = std::max(r.clipping.level == 1.0 ? 0.0 : r.clipping.level, double(m));
        r.clipping.clippedPercent += 100.0 * double(clipped) / double(x.size() * size_t(b.numChannels()));
    }
    if (r.clipping.level == 0.0) r.clipping.level = 1.0;
    r.clipping.likely = r.clipping.runs >= 3 && r.clipping.runs / std::max(r.durationS / 60.0, 1.0 / 60.0) >= 2.0;
    throwIfCancelled(job);

    job.report(0.1, "Noise profile");
    r.noise = NoiseProfile::estimate(b, StftProcessor::defaultFftSize(b.sampleRate));
    {
        // Active level: mean power of the louder half of 50 ms frames.
        const size_t hop = std::max<size_t>(1, size_t(b.sampleRate * 0.05));
        std::vector<double> fe;
        for (size_t s = 0; s + hop <= b.numFrames(); s += hop)
        {
            double e = 0;
            for (int c = 0; c < b.numChannels(); ++c)
                for (size_t i = s; i < s + hop; ++i) e += double(b.channel(c)[i]) * b.channel(c)[i];
            fe.push_back(e / double(hop * size_t(b.numChannels())));
        }
        if (!fe.empty())
        {
            std::sort(fe.begin(), fe.end());
            double s = 0;
            for (size_t i = fe.size() / 2; i < fe.size(); ++i) s += fe[i];
            r.activeLevelDb = powerToDb(s / double(fe.size() - fe.size() / 2));
        }
        else r.activeLevelDb = r.rmsDb;
    }
    r.snrDb = r.noise.valid ? r.activeLevelDb - r.noise.levelDb : 100.0;
    throwIfCancelled(job);

    const auto mono = b.mixdown();
    job.report(0.25, "Hum detection");
    r.hum = detectHum(mono, b.sampleRate);
    throwIfCancelled(job);

    job.report(0.35, "Click detection");
    if (r.durationS > 0.5)
    {
        r.clickCount = ClickRemover::countClicks(mono, b.sampleRate, ClickSettings{}, 60);
        const double analysed = std::min(r.durationS, 60.0);
        r.clicksPerMinute = r.clickCount / std::max(analysed / 60.0, 1.0 / 60.0);
    }
    throwIfCancelled(job);

    job.report(0.45, "Spectrum");
    r.spectrum = spectrumInfo(b, r.noise);
    r.stereo = stereoInfo(b);
    r.transients = transientInfo(b, r.noise.levelDb, r.loudness);
    throwIfCancelled(job);

    if (cat == Category::Voice && r.durationS > 0.3)
    {
        job.report(0.55, "Speech analysis");
        auto& sp = r.speech;
        sp.analysed = true;
        sp.vad = NeuralSpeechDenoiser::voiceActivity(b, job);
        size_t act = 0;
        for (float v : sp.vad) act += v > 0.5f;
        sp.activeRatio = sp.vad.empty() ? 0.0 : double(act) / double(sp.vad.size());
        job.report(0.7, "Pitch");
        const auto f0 = pitchTrack(mono, b.sampleRate, sp.vad, dbToGain(r.activeLevelDb - 25.0));
        if (f0.size() >= 10)
        {
            sp.f0MedianHz = median(f0);
            sp.f0LowHz = percentile(f0, 10);
        }
        throwIfCancelled(job);
        job.report(0.8, "Sibilance");
        {
            // Run the de-esser's own detector so thresholds are in its units.
            const double sr = b.sampleRate;
            const double q = std::sqrt(0.5);
            std::vector<Biquad> sib(static_cast<size_t>(b.numChannels()), Biquad(BiquadCoeffs::highpass(std::min(4000.0, 0.4 * sr), q, sr)));
            std::vector<Biquad> rh(static_cast<size_t>(b.numChannels()), Biquad(BiquadCoeffs::highpass(300.0, q, sr)));
            std::vector<Biquad> rl(static_cast<size_t>(b.numChannels()), Biquad(BiquadCoeffs::lowpass(std::min(3000.0, 0.45 * sr), q, sr)));
            const double a = timeCoeff(3.0, sr);
            double se = 0, re = 0;
            std::vector<double> ratios;
            const size_t hop = size_t(sr * 0.005);
            const double gate = r.activeLevelDb - 30.0;
            for (size_t i = 0; i < b.numFrames(); ++i)
            {
                double sp2 = 0, rp = 0;
                for (int c = 0; c < b.numChannels(); ++c)
                {
                    const double x = b.channel(c)[i];
                    const double sv = sib[size_t(c)].process(x);
                    const double rv = rl[size_t(c)].process(rh[size_t(c)].process(x));
                    sp2 = std::max(sp2, sv * sv);
                    rp = std::max(rp, rv * rv);
                }
                se = a * se + (1 - a) * sp2;
                re = a * re + (1 - a) * rp;
                if (hop && i % hop == 0)
                {
                    const size_t vi = size_t(double(i) / sr * 100.0);
                    const bool active = sp.vad.empty() || (vi < sp.vad.size() && sp.vad[vi] > 0.5f);
                    if (active && powerToDb(se + re, -200) > gate)
                        ratios.push_back(powerToDb(se, -200) - powerToDb(re + 1e-12, -200));
                }
            }
            if (ratios.size() > 20)
            {
                sp.sibilanceMedianDb = median(ratios);
                sp.sibilanceP98Db = percentile(ratios, 98);
            }
            // Sibilance centre: peak of the fine LTAS between 3.5 and 10 kHz
            // relative to its 1-octave smoothed trend.
            double best = -1e9;
            const auto& fh = r.spectrum.fineHz;
            const auto& fd = r.spectrum.fineDb;
            for (size_t i = 12; i + 12 < fh.size(); ++i)
            {
                if (fh[i] < 3500 || fh[i] > std::min(10000.0, 0.4 * sr)) continue;
                double tr = 0;
                for (size_t j = i - 12; j <= i + 12; ++j) tr += fd[j];
                tr /= 25.0;
                const double ex = fd[i] - tr;
                if (ex > best) { best = ex; sp.sibilanceFreqHz = fh[i]; }
            }
        }
        {
            PlosiveSettings ps;
            ps.detectHz = sp.f0LowHz > 0 ? clampv(0.8 * sp.f0LowHz, 60.0, 150.0) : 100.0;
            PlosiveReducer pr(ps);
            AudioBuffer copy = b;
            RenderOptions ro;
            ro.keepTail = false;
            renderProcessor(pr, copy, ro, {});
            sp.plosiveCandidates = pr.eventCount();
        }
        {
            // Speech-active level (K-weighted short-term, 400 ms).
            std::vector<double> lv;
            const auto lc = loudnessCurve(b, 0.1);
            for (size_t i = 0; i < lc.momentary.size(); ++i)
            {
                const size_t vi = i * 10;
                if (vi < sp.vad.size() && sp.vad[vi] > 0.5f && lc.momentary[i] > -70) lv.push_back(lc.momentary[i]);
            }
            sp.activeLevelDb = lv.empty() ? r.loudness.integrated : median(lv);
        }
    }
    throwIfCancelled(job);
    job.report(0.92, "Reverb estimate");
    r.rt60 = estimateRt60(mono, b.sampleRate, r.noise.levelDb);

    // Notes for the advanced panel.
    char buf[256];
    if (r.clipping.likely)
    {
        std::snprintf(buf, sizeof buf, "Clipping: %d flat runs (%.3f%% of samples) at %.2f dBFS.", r.clipping.runs,
                      r.clipping.clippedPercent, gainToDb(r.clipping.level));
        r.notes.push_back(buf);
    }
    if (r.hum.detected)
    {
        std::snprintf(buf, sizeof buf, "Mains hum at %.2f Hz with %zu harmonics above 6 dB prominence.", r.hum.f0, r.hum.harmonics.size());
        r.notes.push_back(buf);
    }
    if (r.stereo.isStereo && r.stereo.correlation < 0.0)
        r.notes.push_back("Negative stereo correlation: check polarity/mono compatibility.");
    job.report(1.0, "Analysis done");
    return r;
}

std::string describe(const AnalysisReport& r)
{
    std::ostringstream o;
    char buf[512];
    std::snprintf(buf, sizeof buf, "%d ch, %.0f Hz, %.2f s\n", r.channels, r.sampleRate, r.durationS);
    o << buf;
    if (r.silent) { o << "Silent.\n"; return o.str(); }
    std::snprintf(buf, sizeof buf, "Loudness %.1f LUFS, LRA %.1f LU, max M %.1f, max S %.1f, TP %.2f dBTP, peak %.2f dBFS\n",
                  r.loudness.integrated, r.loudness.lra, r.loudness.maxMomentary, r.loudness.maxShortTerm, r.loudness.truePeakDb,
                  r.loudness.samplePeakDb);
    o << buf;
    std::snprintf(buf, sizeof buf, "RMS %.1f dBFS, crest %.1f dB, PLR %.1f dB, DC max %.1f dBFS\n", r.rmsDb, r.crestDb, r.plrDb,
                  r.dcOffsetMaxDb);
    o << buf;
    std::snprintf(buf, sizeof buf, "Noise floor %.1f dBFS (flatness %.2f, stationarity %.2f), active level %.1f dBFS, SNR %.1f dB\n",
                  r.noise.levelDb, r.noise.flatness, r.noise.stationarity, r.activeLevelDb, r.snrDb);
    o << buf;
    std::snprintf(buf, sizeof buf, "Hum: %s", r.hum.detected ? "" : "none\n");
    o << buf;
    if (r.hum.detected)
    {
        std::snprintf(buf, sizeof buf, "%.2f Hz, harmonics:", r.hum.f0);
        o << buf;
        for (auto& h : r.hum.harmonics)
        {
            std::snprintf(buf, sizeof buf, " %.1f(+%.0f dB)", h.freq, h.prominenceDb);
            o << buf;
        }
        o << "\n";
    }
    std::snprintf(buf, sizeof buf, "Clicks: %d (%.1f/min). Clipping runs: %d (%.3f%%)\n", r.clickCount, r.clicksPerMinute,
                  r.clipping.runs, r.clipping.clippedPercent);
    o << buf;
    std::snprintf(buf, sizeof buf, "Spectrum tilt %.2f dB/oct, rumble %.1f dB, subsonic %.1f dB\n", r.spectrum.tiltDbPerOct,
                  r.spectrum.rumbleRelDb, r.spectrum.subsonicRelDb);
    o << buf;
    if (r.stereo.isStereo)
    {
        std::snprintf(buf, sizeof buf, "Stereo: corr %.2f (P5 %.2f, <150 Hz %.2f), S/M %.1f dB, balance %.1f dB%s\n",
                      r.stereo.correlation, r.stereo.correlationP5, r.stereo.lowCorrelation, r.stereo.sideToMidDb,
                      r.stereo.balanceDb, r.stereo.dualMono ? " (dual mono)" : "");
        o << buf;
    }
    std::snprintf(buf, sizeof buf, "Transients: attack %.0f ms, decay %.0f ms, onsets %d, active %.2f s, peak-to-loudness %.1f dB\n",
                  r.transients.attackMs, r.transients.decayMs, r.transients.onsetCount, r.transients.activeDurationS,
                  r.transients.peakToLoudnessDb);
    o << buf;
    if (r.speech.analysed)
    {
        std::snprintf(buf, sizeof buf,
                      "Speech: active %.0f%%, level %.1f LUFS, F0 median %.0f Hz (P10 %.0f), sibilance %.0f Hz median %.1f dB P98 %.1f dB, plosives %d\n",
                      r.speech.activeRatio * 100, r.speech.activeLevelDb, r.speech.f0MedianHz, r.speech.f0LowHz,
                      r.speech.sibilanceFreqHz, r.speech.sibilanceMedianDb, r.speech.sibilanceP98Db, r.speech.plosiveCandidates);
        o << buf;
    }
    std::snprintf(buf, sizeof buf, "RT60 estimate: %s\n", r.rt60 > 0 ? (std::to_string(r.rt60).substr(0, 4) + " s").c_str() : "n/a");
    o << buf;
    for (auto& n : r.notes) o << "Note: " << n << "\n";
    return o.str();
}

} // namespace ac
