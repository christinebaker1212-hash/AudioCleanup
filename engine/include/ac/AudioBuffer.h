#pragma once
#include "Common.h"

#include <cstring>
#include <vector>

namespace ac {

/** Planar 32-bit float multichannel audio with its sample rate.
    Floating point throughout: values above 0 dBFS are preserved. */
class AudioBuffer
{
public:
    AudioBuffer() = default;
    AudioBuffer(int channels, size_t frames, double sr) { setSize(channels, frames); sampleRate = sr; }

    void setSize(int channels, size_t frames)
    {
        data_.resize(size_t(std::max(0, channels)));
        for (auto& c : data_) c.assign(frames, 0.0f);
    }
    void resizeFrames(size_t frames)
    {
        for (auto& c : data_) c.resize(frames, 0.0f);
    }

    int numChannels() const { return int(data_.size()); }
    size_t numFrames() const { return data_.empty() ? 0 : data_[0].size(); }
    double durationSeconds() const { return sampleRate > 0 ? double(numFrames()) / sampleRate : 0.0; }
    bool empty() const { return numFrames() == 0 || data_.empty(); }

    float* channel(int c) { return data_[size_t(c)].data(); }
    const float* channel(int c) const { return data_[size_t(c)].data(); }
    std::vector<float>& vec(int c) { return data_[size_t(c)]; }
    const std::vector<float>& vec(int c) const { return data_[size_t(c)]; }

    void clear()
    {
        for (auto& c : data_) std::fill(c.begin(), c.end(), 0.0f);
    }
    void applyGain(double g)
    {
        for (auto& c : data_)
            for (auto& s : c) s = float(s * g);
    }
    float peak() const
    {
        float p = 0.0f;
        for (auto& c : data_)
            for (auto s : c) p = std::max(p, std::abs(s));
        return p;
    }

    /** Copy a frame range into a new buffer. */
    AudioBuffer slice(size_t start, size_t len) const
    {
        AudioBuffer out(numChannels(), len, sampleRate);
        for (int c = 0; c < numChannels(); ++c)
        {
            const size_t avail = start < numFrames() ? std::min(len, numFrames() - start) : 0;
            if (avail) std::memcpy(out.channel(c), channel(c) + start, avail * sizeof(float));
        }
        return out;
    }

    /** Mono mixdown (average of channels). */
    std::vector<float> mixdown() const
    {
        std::vector<float> m(numFrames(), 0.0f);
        if (data_.empty()) return m;
        const float k = 1.0f / float(numChannels());
        for (auto& c : data_)
            for (size_t i = 0; i < m.size(); ++i) m[i] += c[i] * k;
        return m;
    }

    double sampleRate = 48000.0;

private:
    std::vector<std::vector<float>> data_;
};

} // namespace ac
