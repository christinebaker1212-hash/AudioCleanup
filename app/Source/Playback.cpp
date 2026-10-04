#include "Playback.h"

void ABPlayer::setSet(std::shared_ptr<const PlaybackSet> s)
{
    std::shared_ptr<const PlaybackSet> old;
    {
        const juce::SpinLock::ScopedLockType l(lock_);
        old = std::move(set_);
        set_ = std::move(s);
    }
    // `old` is released here, on the message thread, outside the lock.
}

std::shared_ptr<const PlaybackSet> ABPlayer::currentSet() const
{
    const juce::SpinLock::ScopedLockType l(lock_);
    return set_;
}

void ABPlayer::setPositionSeconds(double s) { pos_ = int64_t(std::max(0.0, s) * deviceRate_.load()); }

double ABPlayer::positionSeconds() const { return double(pos_.load()) / deviceRate_.load(); }

void ABPlayer::matchGains(double matchDb, bool matched, float& gProc, float& gOrig)
{
    gProc = gOrig = 1.0f;
    if (!matched) return;
    if (matchDb > 0) gProc = float(std::pow(10.0, -matchDb / 20.0));
    else gOrig = float(std::pow(10.0, matchDb / 20.0));
}

void ABPlayer::prepareToPlay(int, double sampleRate)
{
    const double old = deviceRate_.load();
    if (sampleRate > 0 && old > 0) pos_ = int64_t(double(pos_.load()) * sampleRate / old);
    deviceRate_ = sampleRate;
    fadeLen_ = std::max(1, int(0.02 * sampleRate));
}

void ABPlayer::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    // Hold the set by raw pointer under a try-lock; the message thread only
    // swaps it while holding the same lock, and releases old sets itself.
    const PlaybackSet* s = nullptr;
    const juce::SpinLock::ScopedTryLockType l(lock_);
    if (l.isLocked()) s = set_.get();
    if (!s || !playing_ || s->length() == 0 || std::abs(s->sampleRate - deviceRate_.load()) > 0.5) return;

    float gp, go;
    matchGains(s->matchDb, matched_, gp, go);
    const float gains[3] = { gp, go, 1.0f };
    const std::vector<std::vector<float>>* srcs[3] = { &s->processed, &s->original, &s->removed };

    const int tgt = target_.load();
    if (tgt != current_ && fadePos_ == 0)
    {
        fadeFrom_ = current_;
        current_ = tgt;
        fadePos_ = fadeLen_;
    }
    const int outCh = info.buffer->getNumChannels();
    const int64_t len = int64_t(s->length());
    int64_t p = pos_.load();
    for (int i = 0; i < info.numSamples; ++i)
    {
        if (p >= len)
        {
            if (loop_) p = 0;
            else
            {
                playing_ = false;
                break;
            }
        }
        float mono = 0.0f;
        float fadeIn = 1.0f;
        if (fadePos_ > 0)
        {
            fadeIn = 1.0f - float(fadePos_) / float(fadeLen_);
            --fadePos_;
        }
        for (int c = 0; c < outCh; ++c)
        {
            const auto& cur = *srcs[current_];
            const size_t sc = size_t(std::min<int>(c, int(cur.size()) - 1));
            float v = cur[sc][size_t(p)] * gains[current_];
            if (fadeIn < 1.0f)
            {
                const auto& prev = *srcs[fadeFrom_];
                const size_t pc = size_t(std::min<int>(c, int(prev.size()) - 1));
                v = v * fadeIn + prev[pc][size_t(p)] * gains[fadeFrom_] * (1.0f - fadeIn);
            }
            info.buffer->setSample(c, info.startSample + i, v);
            mono += v / float(std::max(1, outCh));
        }
        // scope FIFO (drop when full)
        int s1, n1, s2, n2;
        fifo_.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 > 0) fifoBuf_[size_t(s1)] = mono;
        else if (n2 > 0) fifoBuf_[size_t(s2)] = mono;
        fifo_.finishedWrite(n1 + n2);
        ++p;
    }
    pos_ = p;
}

int ABPlayer::readScope(float* dest, int maxSamples)
{
    int s1, n1, s2, n2;
    fifo_.prepareToRead(maxSamples, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) dest[i] = fifoBuf_[size_t(s1 + i)];
    for (int i = 0; i < n2; ++i) dest[n1 + i] = fifoBuf_[size_t(s2 + i)];
    fifo_.finishedRead(n1 + n2);
    return n1 + n2;
}
