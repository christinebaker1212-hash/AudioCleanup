#include "ac/Resampler.h"
#include "ac/Restoration.h"

#include <rnnoise.h>

namespace ac {

namespace {
constexpr int kFrame = 480; // RNNoise frame at 48 kHz (10 ms)

struct RnState
{
    DenoiseState* st = rnnoise_create(nullptr);
    ~RnState() { rnnoise_destroy(st); }
};

/** Run RNNoise on a 48 kHz mono signal; output is latency-compensated. */
std::vector<float> runModel(const std::vector<float>& x, std::vector<float>* vad, const Job& job)
{
    const int lat = NeuralSpeechDenoiser::modelLatency48k();
    const size_t n = x.size();
    const size_t total = n + size_t(lat);
    const size_t frames = (total + kFrame - 1) / kFrame;
    std::vector<float> out(frames * kFrame, 0.0f);
    RnState rs;
    float in[kFrame], o[kFrame];
    for (size_t f = 0; f < frames; ++f)
    {
        for (int i = 0; i < kFrame; ++i)
        {
            const size_t idx = f * kFrame + size_t(i);
            in[i] = idx < n ? x[idx] * 32768.0f : 0.0f;
        }
        const float p = rnnoise_process_frame(rs.st, o, in);
        if (vad) vad->push_back(p);
        for (int i = 0; i < kFrame; ++i) out[f * kFrame + size_t(i)] = o[i] / 32768.0f;
        if ((f & 255) == 0) throwIfCancelled(job);
    }
    return std::vector<float>(out.begin() + lat, out.begin() + lat + long(n));
}
} // namespace

int NeuralSpeechDenoiser::modelLatency48k()
{
    // RNNoise 0.2: one frame of input buffering plus one frame of
    // overlap-add synthesis = 20 ms. Verified by tests (cross-correlation).
    return 2 * kFrame;
}

void NeuralSpeechDenoiser::processOffline(AudioBuffer& b, const Job& job)
{
    const double sr = b.sampleRate;
    const double floorG = dbToGain(-std::abs(s_.maxAttenuationDb));
    const double mix = clampv(s_.mix, 0.0, 1.0);
    for (int c = 0; c < b.numChannels(); ++c)
    {
        AudioBuffer mono(1, b.numFrames(), sr);
        mono.vec(0) = b.vec(c);
        AudioBuffer m48 = resample(mono, 48000.0, job);
        auto y = runModel(m48.vec(0), nullptr, job);
        m48.vec(0) = std::move(y);
        AudioBuffer back = resample(m48, sr, job);
        back.resizeFrames(b.numFrames());
        float* d = b.channel(c);
        const float* w = back.channel(0);
        for (size_t i = 0; i < b.numFrames(); ++i)
        {
            const double dry = d[i];
            const double wet = w[i] + floorG * (dry - w[i]);
            d[i] = float(dry * (1.0 - mix) + wet * mix);
        }
        job.report(double(c + 1) / b.numChannels(), "Neural speech enhancement");
    }
}

std::vector<float> NeuralSpeechDenoiser::voiceActivity(const AudioBuffer& b, const Job& job)
{
    std::vector<float> acc;
    for (int c = 0; c < b.numChannels(); ++c)
    {
        AudioBuffer mono(1, b.numFrames(), b.sampleRate);
        mono.vec(0) = b.vec(c);
        AudioBuffer m48 = resample(mono, 48000.0, job);
        std::vector<float> vad;
        runModel(m48.vec(0), &vad, job);
        // Frame f's probability describes input ending one frame earlier.
        if (!vad.empty()) vad.erase(vad.begin());
        if (acc.empty()) acc.assign(vad.size(), 0.0f);
        for (size_t i = 0; i < std::min(acc.size(), vad.size()); ++i) acc[i] += vad[i] / float(b.numChannels());
    }
    return acc;
}

} // namespace ac
