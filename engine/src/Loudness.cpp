#include "ac/Loudness.h"

#include "ac/Oversampling.h"

#include <ebur128.h>

namespace ac {

namespace {
struct StateDeleter
{
    void operator()(ebur128_state* s) const
    {
        if (s) ebur128_destroy(&s);
    }
};
using StatePtr = std::unique_ptr<ebur128_state, StateDeleter>;

StatePtr makeState(int channels, double sr, int mode)
{
    ebur128_state* s = ebur128_init(unsigned(std::max(1, channels)), (unsigned long)std::lround(sr), mode);
    if (s && channels == 6)
    {
        // 5.1 in SMPTE/WAV order: L R C LFE Ls Rs
        ebur128_set_channel(s, 0, EBUR128_LEFT);
        ebur128_set_channel(s, 1, EBUR128_RIGHT);
        ebur128_set_channel(s, 2, EBUR128_CENTER);
        ebur128_set_channel(s, 3, EBUR128_UNUSED);
        ebur128_set_channel(s, 4, EBUR128_LEFT_SURROUND);
        ebur128_set_channel(s, 5, EBUR128_RIGHT_SURROUND);
    }
    else if (s && channels > 2)
    {
        for (int c = 0; c < channels; ++c) ebur128_set_channel(s, unsigned(c), EBUR128_LEFT); // weight 1.0
    }
    return StatePtr(s);
}

void feed(ebur128_state* s, const AudioBuffer& b, size_t start, size_t n, std::vector<float>& inter)
{
    const int nch = b.numChannels();
    inter.resize(n * size_t(nch));
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < nch; ++c) inter[i * size_t(nch) + size_t(c)] = b.channel(c)[start + i];
    ebur128_add_frames_float(s, inter.data(), n);
}

double fin(double v) { return std::isfinite(v) ? v : -std::numeric_limits<double>::infinity(); }
} // namespace

LoudnessStats measureLoudness(const AudioBuffer& b, bool withTruePeak, bool withLra)
{
    LoudnessStats st;
    if (b.empty()) return st;
    int mode = EBUR128_MODE_I | EBUR128_MODE_S | EBUR128_MODE_SAMPLE_PEAK;
    if (withLra) mode |= EBUR128_MODE_LRA;
    if (withTruePeak) mode |= EBUR128_MODE_TRUE_PEAK;
    auto s = makeState(b.numChannels(), b.sampleRate, mode);
    if (!s) return st;

    std::vector<float> inter;
    const size_t hop = size_t(std::max(1.0, b.sampleRate * 0.1));
    for (size_t pos = 0; pos < b.numFrames(); pos += hop)
    {
        const size_t n = std::min(hop, b.numFrames() - pos);
        feed(s.get(), b, pos, n, inter);
        double m = 0, sterm = 0;
        if (ebur128_loudness_momentary(s.get(), &m) == EBUR128_SUCCESS && std::isfinite(m) && m > -200)
            st.maxMomentary = std::max(st.maxMomentary, m);
        if (pos + n >= size_t(3.0 * b.sampleRate) || pos + n == b.numFrames())
            if (ebur128_loudness_shortterm(s.get(), &sterm) == EBUR128_SUCCESS && std::isfinite(sterm) && sterm > -200)
                st.maxShortTerm = std::max(st.maxShortTerm, sterm);
    }
    double gi = 0;
    if (ebur128_loudness_global(s.get(), &gi) == EBUR128_SUCCESS) st.integrated = fin(gi);
    if (withLra)
    {
        double lra = 0;
        if (ebur128_loudness_range(s.get(), &lra) == EBUR128_SUCCESS && std::isfinite(lra)) st.lra = lra;
    }
    double tp = 0, sp = 0;
    for (int c = 0; c < b.numChannels(); ++c)
    {
        if (withTruePeak && ebur128_true_peak(s.get(), unsigned(c), &tp) == EBUR128_SUCCESS)
            st.truePeakDb = std::max(st.truePeakDb, gainToDb(tp));
        if (ebur128_sample_peak(s.get(), unsigned(c), &sp) == EBUR128_SUCCESS)
            st.samplePeakDb = std::max(st.samplePeakDb, gainToDb(sp));
    }
    if (!withTruePeak) st.truePeakDb = st.samplePeakDb;
    if (st.integrated < -70.0) st.integrated = -std::numeric_limits<double>::infinity();
    return st;
}

double integratedLoudness(const AudioBuffer& b)
{
    return measureLoudness(b, false, false).integrated;
}

LoudnessCurve loudnessCurve(const AudioBuffer& b, double hopSeconds)
{
    LoudnessCurve lc;
    lc.hopSeconds = hopSeconds;
    if (b.empty()) return lc;
    auto s = makeState(b.numChannels(), b.sampleRate, EBUR128_MODE_M | EBUR128_MODE_S);
    if (!s) return lc;
    std::vector<float> inter;
    const size_t hop = size_t(std::max(1.0, b.sampleRate * hopSeconds));
    for (size_t pos = 0; pos < b.numFrames(); pos += hop)
    {
        const size_t n = std::min(hop, b.numFrames() - pos);
        feed(s.get(), b, pos, n, inter);
        double m = -200, st = -200;
        ebur128_loudness_momentary(s.get(), &m);
        ebur128_loudness_shortterm(s.get(), &st);
        lc.momentary.push_back(float(std::isfinite(m) ? std::max(-200.0, m) : -200.0));
        lc.shortTerm.push_back(float(std::isfinite(st) ? std::max(-200.0, st) : -200.0));
    }
    return lc;
}

std::vector<float> truePeakCurve(const AudioBuffer& b, double hopSeconds)
{
    std::vector<float> out;
    if (b.empty()) return out;
    const size_t hop = std::max<size_t>(1, size_t(b.sampleRate * hopSeconds));
    std::vector<TruePeakDetector> det(size_t(b.numChannels()));
    for (auto& d : det) d.setup(24);
    const int lat = det[0].latency();
    const size_t n = b.numFrames();
    out.assign((n + hop - 1) / hop, float(kSilenceDb));
    for (int c = 0; c < b.numChannels(); ++c)
    {
        auto& d = det[size_t(c)];
        const float* x = b.channel(c);
        for (size_t i = 0; i < n + size_t(lat); ++i)
        {
            const double v = d.process(i < n ? x[i] : 0.0);
            if (i < size_t(lat)) continue;
            const size_t k = (i - size_t(lat)) / hop;
            if (k < out.size()) out[k] = std::max(out[k], float(gainToDb(v)));
        }
    }
    return out;
}

struct LiveLoudnessMeter::Impl
{
    StatePtr st;
    int channels = 0;
    double sr = 48000;
    std::vector<float> inter;
};

LiveLoudnessMeter::LiveLoudnessMeter() : impl_(std::make_unique<Impl>()) {}
LiveLoudnessMeter::~LiveLoudnessMeter() = default;

void LiveLoudnessMeter::prepare(double sr, int channels)
{
    impl_->sr = sr;
    impl_->channels = channels;
    reset();
}

void LiveLoudnessMeter::reset()
{
    impl_->st = makeState(impl_->channels, impl_->sr,
                          EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
}

void LiveLoudnessMeter::addFrames(const float* const* ch, int numCh, int n)
{
    if (!impl_->st || n <= 0) return;
    const int nc = std::min(numCh, impl_->channels);
    impl_->inter.assign(size_t(n) * size_t(impl_->channels), 0.0f);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < nc; ++c) impl_->inter[size_t(i) * size_t(impl_->channels) + size_t(c)] = ch[c][i];
    ebur128_add_frames_float(impl_->st.get(), impl_->inter.data(), size_t(n));
}

double LiveLoudnessMeter::momentary() const
{
    double v = -200;
    if (impl_->st) ebur128_loudness_momentary(impl_->st.get(), &v);
    return std::isfinite(v) ? v : -200.0;
}
double LiveLoudnessMeter::shortTerm() const
{
    double v = -200;
    if (impl_->st) ebur128_loudness_shortterm(impl_->st.get(), &v);
    return std::isfinite(v) ? v : -200.0;
}
double LiveLoudnessMeter::integrated() const
{
    double v = -200;
    if (impl_->st) ebur128_loudness_global(impl_->st.get(), &v);
    return std::isfinite(v) ? v : -200.0;
}
double LiveLoudnessMeter::truePeakDb() const
{
    double best = 0;
    if (!impl_->st) return kSilenceDb;
    for (int c = 0; c < impl_->channels; ++c)
    {
        double tp = 0;
        if (ebur128_true_peak(impl_->st.get(), unsigned(c), &tp) == EBUR128_SUCCESS) best = std::max(best, tp);
    }
    return gainToDb(best);
}

} // namespace ac
