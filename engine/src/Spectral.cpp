#include "ac/Restoration.h"

#include <cstring>

namespace ac {

// ---------------------------------------------------------------------- STFT
int StftProcessor::defaultFftSize(double sr)
{
    const double target = 0.0427 * sr;
    int n = 256;
    while (n < target * 0.75) n *= 2;
    return clampv(n, 256, 16384);
}

void StftProcessor::prepare(double sr, int ch)
{
    sr_ = sr;
    nch_ = ch;
    fft_ = std::make_unique<RealFft>(N_);
    win_ = hannWindow(N_);
    for (auto& w : win_) w = std::sqrt(w);
    in_.assign(size_t(ch), std::vector<float>(size_t(N_), 0.0f));
    acc_.assign(size_t(ch), std::vector<float>(size_t(N_), 0.0f));
    outq_.assign(size_t(ch), std::vector<float>(size_t(H_), 0.0f));
    spec_.assign(size_t(ch), std::vector<std::complex<float>>(size_t(N_ / 2 + 1)));
    frame_.assign(size_t(N_), 0.0f);
    inCount_ = 0;
    outPos_ = 0;
    onPrepare();
}

void StftProcessor::process(float* const* ch, int nch, int n)
{
    const float scale = 0.5f; // sum of sqrtHann^2 at 75 % overlap = 2
    std::vector<std::complex<float>*> ptrs(static_cast<size_t>(nch_));
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < nch; ++c)
        {
            in_[size_t(c)][size_t(N_ - H_ + inCount_)] = ch[c][i];
            ch[c][i] = outq_[size_t(c)][size_t(outPos_)];
        }
        ++inCount_;
        ++outPos_;
        if (inCount_ == H_)
        {
            for (int c = 0; c < nch_; ++c)
            {
                const auto& in = in_[size_t(c)];
                for (int k = 0; k < N_; ++k) frame_[size_t(k)] = in[size_t(k)] * win_[size_t(k)];
                fft_->forward(frame_.data(), spec_[size_t(c)].data());
                ptrs[size_t(c)] = spec_[size_t(c)].data();
            }
            processFrame(ptrs);
            for (int c = 0; c < nch_; ++c)
            {
                fft_->inverse(spec_[size_t(c)].data(), frame_.data());
                auto& acc = acc_[size_t(c)];
                for (int k = 0; k < N_; ++k) acc[size_t(k)] += frame_[size_t(k)] * win_[size_t(k)] * scale;
                std::memcpy(outq_[size_t(c)].data(), acc.data(), sizeof(float) * size_t(H_));
                std::memmove(acc.data(), acc.data() + H_, sizeof(float) * size_t(N_ - H_));
                std::fill(acc.begin() + (N_ - H_), acc.end(), 0.0f);
                auto& in = in_[size_t(c)];
                std::memmove(in.data(), in.data() + H_, sizeof(float) * size_t(N_ - H_));
            }
            inCount_ = 0;
            outPos_ = 0;
        }
    }
}

// --------------------------------------------------------------- LSA gain
namespace {
double expint1(double v)
{
    if (v <= 0) return 50.0;
    if (v < 1.0)
    {
        double sum = 0, term = 1;
        for (int k = 1; k <= 25; ++k)
        {
            term *= -v / k;
            sum -= term / k;
        }
        return -0.5772156649015329 - std::log(v) + sum;
    }
    return std::exp(-v) / v * (v * v + 2.334733 * v + 0.250621) / (v * v + 3.330657 * v + 1.681534);
}
} // namespace

double lsaGain(double xi, double gamma)
{
    const double v = xi * gamma / (1.0 + xi);
    const double g = xi / (1.0 + xi) * std::exp(0.5 * expint1(std::max(v, 1e-12)));
    return clampv(g, 0.0, 1.0);
}

// ------------------------------------------------------------ Noise profile
namespace {
struct FramePowers
{
    std::vector<std::vector<float>> psd; // per frame, channel-averaged |X|^2
    std::vector<double> energy;          // per frame total
    std::vector<double> logMean;         // per frame mean log-power (each bin weighted equally)
    std::vector<size_t> start;
};

FramePowers framePowers(const AudioBuffer& b, int N, size_t from, size_t to)
{
    FramePowers fp;
    RealFft fft(N);
    auto win = hannWindow(N);
    for (auto& w : win) w = std::sqrt(w);
    const int bins = N / 2 + 1;
    std::vector<float> frame(static_cast<size_t>(N));
    std::vector<std::complex<float>> spec(static_cast<size_t>(bins));
    const size_t hop = size_t(N / 2);
    to = std::min(to, b.numFrames());
    if (to < from + size_t(N)) return fp;
    for (size_t s = from; s + size_t(N) <= to; s += hop)
    {
        std::vector<float> p(static_cast<size_t>(bins), 0.0f);
        for (int c = 0; c < b.numChannels(); ++c)
        {
            const float* x = b.channel(c) + s;
            for (int k = 0; k < N; ++k) frame[size_t(k)] = x[k] * win[size_t(k)];
            fft.forward(frame.data(), spec.data());
            for (int k = 0; k < bins; ++k) p[size_t(k)] += std::norm(spec[size_t(k)]) / float(b.numChannels());
        }
        double e = 0, lm = 0;
        for (float v : p) { e += v; lm += std::log(double(v) + 1e-30); }
        fp.psd.push_back(std::move(p));
        fp.energy.push_back(e);
        fp.logMean.push_back(lm / bins);
        fp.start.push_back(s);
    }
    return fp;
}

void finishProfile(NoiseProfile& np, const AudioBuffer& b)
{
    const int N = np.fftSize;
    const int bins = N / 2 + 1;
    double sum = 0;
    for (int k = 0; k < bins; ++k) sum += (k == 0 || k == bins - 1 ? 1.0 : 2.0) * np.psd[size_t(k)];
    const double var = sum / double(N) / (double(N) * 0.5);
    np.levelDb = powerToDb(var);
    const int k0 = std::max(1, int(100.0 * N / b.sampleRate));
    const int k1 = std::min(bins - 1, int(std::min(16000.0, 0.45 * b.sampleRate) * N / b.sampleRate));
    double lg = 0, ar = 0;
    int cnt = 0;
    for (int k = k0; k < k1; ++k)
    {
        const double v = std::max(1e-30, double(np.psd[size_t(k)]));
        lg += std::log(v);
        ar += v;
        ++cnt;
    }
    np.flatness = cnt > 0 && ar > 0 ? std::exp(lg / cnt) / (ar / cnt) : 0.0;
    np.valid = np.levelDb > -140.0;
}
} // namespace

NoiseProfile NoiseProfile::estimate(const AudioBuffer& b, int N, double quietPercent)
{
    NoiseProfile np;
    np.fftSize = N;
    np.sampleRate = b.sampleRate;
    const int bins = N / 2 + 1;
    np.psd.assign(size_t(bins), 0.0f);
    auto fp = framePowers(b, N, 0, b.numFrames());
    // Drop digital silence (exact zeros / dither-free gaps) - not noise.
    std::vector<size_t> idx;
    for (size_t i = 0; i < fp.energy.size(); ++i)
        if (fp.energy[i] > 1e-12) idx.push_back(i);
    if (idx.size() < 3) return np;
    // Rank frames by mean log-power so that every bin has equal say; ranking
    // by total energy would let the strongest (usually low) bins pick the
    // frames and bias exactly those bins low.
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t c) { return fp.logMean[a] < fp.logMean[c]; });
    const size_t take = std::max<size_t>(std::min<size_t>(idx.size(), 5), size_t(double(idx.size()) * quietPercent / 100.0));
    // Per-bin median over the quiet frames / ln2 = mean of an exponential
    // distribution: robust to occasional events inside "quiet" frames.
    std::vector<double> col(take);
    for (int k = 0; k < bins; ++k)
    {
        for (size_t j = 0; j < take; ++j) col[j] = fp.psd[idx[j]][size_t(k)];
        np.psd[size_t(k)] = float(median(col) / std::log(2.0));
    }
    // Stationarity: spread of per-second noise floors.
    const double framesPerSec = b.sampleRate / (N / 2);
    std::vector<double> floors;
    for (size_t s = 0; s < fp.energy.size(); s += size_t(std::max(1.0, framesPerSec)))
    {
        double m = 1e300;
        for (size_t j = s; j < std::min(fp.energy.size(), s + size_t(std::max(1.0, framesPerSec))); ++j)
            if (fp.energy[j] > 1e-12) m = std::min(m, fp.energy[j]);
        if (m < 1e299) floors.push_back(powerToDb(m));
    }
    if (floors.size() >= 3)
    {
        // Use the lower half of per-second floors (upper ones are program-dominated).
        std::sort(floors.begin(), floors.end());
        floors.resize((floors.size() + 1) / 2);
        double mean = 0;
        for (double f : floors) mean += f;
        mean /= double(floors.size());
        double var = 0;
        for (double f : floors) var += (f - mean) * (f - mean);
        const double sd = std::sqrt(var / double(floors.size()));
        np.stationarity = clampv(1.0 - (sd - 1.0) / 5.0, 0.0, 1.0);
    }
    finishProfile(np, b);
    return np;
}

NoiseProfile NoiseProfile::estimateFromRegion(const AudioBuffer& b, int N, size_t start, size_t end)
{
    NoiseProfile np;
    np.fftSize = N;
    np.sampleRate = b.sampleRate;
    const int bins = N / 2 + 1;
    np.psd.assign(size_t(bins), 0.0f);
    auto fp = framePowers(b, N, start, end);
    if (fp.psd.empty()) return np;
    for (auto& f : fp.psd)
        for (int k = 0; k < bins; ++k) np.psd[size_t(k)] += f[size_t(k)] / float(fp.psd.size());
    finishProfile(np, b);
    return np;
}

// --------------------------------------------------------- SpectralDenoiser
void SpectralDenoiser::onPrepare()
{
    bins_ = fftSize() / 2 + 1;
    const int g = s_.linkChannels ? 1 : numChannels();
    const size_t n = size_t(g) * size_t(bins_);
    noise_.assign(size_t(bins_), 0.0);
    for (int k = 0; k < bins_; ++k)
        noise_[size_t(k)] = k < int(s_.profile.psd.size()) ? double(s_.profile.psd[size_t(k)]) : 1e-12;
    prevGainSq_.assign(n, 1.0);
    prevPost_.assign(n, 1.0);
    gain_.assign(size_t(bins_), 1.0);
    smoothG_.assign(n, 1.0);
    tmp_.assign(size_t(bins_) + 1, 0.0);
    pw_.assign(size_t(bins_), 0.0);
    aRel_ = std::exp(-double(hop()) / (std::max(1.0, s_.releaseMs) * 0.001 * sampleRate()));
    // Minimum statistics: 8 sub-windows covering ~1.5 s.
    subFrames_ = std::max(2, int(0.19 * sampleRate() / hop()));
    const double B = 1.6;
    psm_.assign(n, 0.0);
    curMin_.assign(n, 1e300);
    subMins_.assign(8, std::vector<double>(n, 0.0));
    for (auto& sm : subMins_)
        for (size_t i = 0; i < n; ++i) sm[i] = noise_[i % size_t(bins_)] / B;
    subCount_ = 0;
    subIdx_ = 0;
    attnNum_ = attnDen_ = 0;
    frameAttn_.clear();
    first_ = true;
}

double SpectralDenoiser::medianFrameAttenuationDb() const
{
    return frameAttn_.empty() ? 0.0 : median(frameAttn_);
}

double SpectralDenoiser::p95FrameAttenuationDb() const
{
    return frameAttn_.empty() ? 0.0 : percentile(frameAttn_, 95.0);
}

void SpectralDenoiser::processFrame(std::vector<std::complex<float>*>& X)
{
    const int nch = numChannels();
    const int groups = s_.linkChannels ? 1 : nch;
    const double gmin = dbToGain(-std::abs(s_.reductionDb));
    const double alpha = clampv(s_.ddAlpha, 0.5, 0.999);
    const double xiMin = 0.003;
    const int kLow = int(s_.lowCutoffHz * fftSize() / sampleRate());
    const double B = 1.6;

    attnNum_ = attnDen_ = 0;
    for (int gi = 0; gi < groups; ++gi)
    {
        const size_t off = size_t(gi) * size_t(bins_);
        for (int k = 0; k < bins_; ++k)
        {
            double y = 0;
            if (s_.linkChannels)
            {
                for (int c = 0; c < nch; ++c) y += std::norm(X[size_t(c)][k]);
                y /= nch;
            }
            else y = std::norm(X[size_t(gi)][k]);
            pw_[size_t(k)] = y;

            double lambda = noise_[size_t(k)];
            if (s_.adaptive)
            {
                double& p = psm_[off + size_t(k)];
                p = first_ ? y : 0.85 * p + 0.15 * y;
                double& cm = curMin_[off + size_t(k)];
                cm = std::min(cm, p);
                double mn = cm;
                for (auto& sm : subMins_) mn = std::min(mn, sm[off + size_t(k)]);
                const double tracked = B * mn;
                lambda = s_.profile.valid ? clampv(tracked, 0.1 * lambda, 30.0 * lambda) : tracked;
            }
            lambda = std::max(1e-20, lambda * s_.oversubtraction);
            const double gamma = std::min(1e4, y / lambda);
            double xi = first_ ? std::max(gamma - 1.0, 0.0)
                               : alpha * prevGainSq_[off + size_t(k)] * prevPost_[off + size_t(k)] + (1.0 - alpha) * std::max(gamma - 1.0, 0.0);
            xi = std::max(xi, xiMin);
            double g = lsaGain(xi, gamma);
            prevGainSq_[off + size_t(k)] = g * g;
            prevPost_[off + size_t(k)] = gamma;
            gain_[size_t(k)] = std::max(g, gmin);
        }
        // Frame speech/programme presence: mean a-posteriori SNR in 300 Hz-4 kHz.
        // The release hold (which protects decays) is only used while programme
        // is present; on noise-only frames it would latch onto random peaks
        // and leave "chirpy" residual noise.
        double gsum = 0;
        int gcnt = 0;
        const int kp0 = std::max(1, int(300.0 * fftSize() / sampleRate()));
        const int kp1 = std::min(bins_ - 1, int(4000.0 * fftSize() / sampleRate()));
        for (int k = kp0; k <= kp1; ++k)
        {
            gsum += pw_[size_t(k)] / std::max(1e-20, noise_[size_t(k)] * s_.oversubtraction);
            ++gcnt;
        }
        const bool programme = gcnt > 0 && gsum / gcnt > 4.0;
        // Constant-relative-bandwidth smoothing across frequency.
        tmp_[0] = 0;
        for (int k = 0; k < bins_; ++k) tmp_[size_t(k) + 1] = tmp_[size_t(k)] + gain_[size_t(k)];
        for (int k = 0; k < bins_; ++k)
        {
            const int h = int(k * s_.freqSmoothing * 0.5);
            const int a = std::max(0, k - h), e = std::min(bins_ - 1, k + h);
            double g = (tmp_[size_t(e) + 1] - tmp_[size_t(a)]) / double(e - a + 1);
            if (k < kLow) g = 1.0;
            // Release smoothing only while programme is present (decay tails).
            double& sg = smoothG_[off + size_t(k)];
            sg = (g >= sg || !programme) ? g : aRel_ * sg + (1.0 - aRel_) * g;
            const double ga = std::max(sg, gmin);
            attnNum_ += pw_[size_t(k)] * ga * ga;
            attnDen_ += pw_[size_t(k)];
            if (s_.linkChannels)
                for (int c = 0; c < nch; ++c) X[size_t(c)][k] *= float(ga);
            else X[size_t(gi)][k] *= float(ga);
        }
    }
    if (attnDen_ > 1e-12) frameAttn_.push_back(-powerToDb(attnNum_ / attnDen_));
    if (s_.adaptive && ++subCount_ >= subFrames_)
    {
        subMins_[size_t(subIdx_)] = curMin_;
        subIdx_ = (subIdx_ + 1) % int(subMins_.size());
        std::fill(curMin_.begin(), curMin_.end(), 1e300);
        subCount_ = 0;
    }
    first_ = false;
}

// ------------------------------------------------------------- Dereverberator
void Dereverberator::onPrepare()
{
    bins_ = fftSize() / 2 + 1;
    delayFrames_ = std::max(1, int(std::lround(s_.delayMs * 0.001 * sampleRate() / hop())));
    const double td = double(delayFrames_ * hop()) / sampleRate();
    const double delta = 3.0 * std::log(10.0) / std::max(0.05, s_.rt60);
    decay_ = std::exp(-2.0 * delta * td);
    hist_.assign(size_t(delayFrames_ + 1), std::vector<double>(size_t(bins_), 0.0));
    histPos_ = 0;
    psd_.assign(size_t(bins_), 0.0);
    prevGainSq_.assign(size_t(bins_), 1.0);
    prevPost_.assign(size_t(bins_), 1.0);
    gain_.assign(size_t(bins_), 1.0);
}

void Dereverberator::processFrame(std::vector<std::complex<float>*>& X)
{
    const int nch = numChannels();
    const double gmin = dbToGain(-std::abs(s_.reductionDb));
    const int L = int(hist_.size());
    const auto& old = hist_[size_t((histPos_ + 1) % L)]; // delayFrames_ ago
    auto& cur = hist_[size_t(histPos_)];
    for (int k = 0; k < bins_; ++k)
    {
        double y = 0;
        for (int c = 0; c < nch; ++c) y += std::norm(X[size_t(c)][k]);
        y /= nch;
        psd_[size_t(k)] = 0.6 * psd_[size_t(k)] + 0.4 * y;
        cur[size_t(k)] = psd_[size_t(k)];
        const double lr = std::max(1e-20, s_.strength * decay_ * old[size_t(k)]);
        const double gamma = std::min(1e4, y / lr);
        double xi = 0.9 * prevGainSq_[size_t(k)] * prevPost_[size_t(k)] + 0.1 * std::max(gamma - 1.0, 0.0);
        xi = std::max(xi, 0.003);
        double g = lsaGain(xi, gamma);
        prevGainSq_[size_t(k)] = g * g;
        prevPost_[size_t(k)] = gamma;
        g = std::max(g, gmin);
        // Gentle temporal smoothing against flutter.
        gain_[size_t(k)] = g >= gain_[size_t(k)] ? g : 0.5 * gain_[size_t(k)] + 0.5 * g;
        for (int c = 0; c < nch; ++c) X[size_t(c)][k] *= float(gain_[size_t(k)]);
    }
    histPos_ = (histPos_ + 1) % L;
}

} // namespace ac
