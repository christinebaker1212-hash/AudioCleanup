#pragma once
// Latency-aligned, loudness-matched A/B monitor. All three signals
// (processed, original, removed noise) share one timeline; switching is a
// 20 ms equal-gain crossfade at the same playhead, so comparisons are instant.

#include "Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

class ABPlayer : public juce::AudioSource
{
public:
    enum Source { Processed = 0, Original = 1, Removed = 2 };

    void setSet(std::shared_ptr<const PlaybackSet> s);
    std::shared_ptr<const PlaybackSet> currentSet() const;
    void setSource(Source s) { target_ = int(s); }
    Source source() const { return Source(target_.load()); }
    void setMatched(bool m) { matched_ = m; }
    bool matched() const { return matched_; }
    void setLooping(bool l) { loop_ = l; }
    void play() { playing_ = true; }
    void stop() { playing_ = false; }
    bool isPlaying() const { return playing_; }
    void setPositionSeconds(double s);
    double positionSeconds() const;
    double deviceRate() const { return deviceRate_.load(); }

    /** Gains applied to processed / original in matched mode (attenuate the louder one). */
    static void matchGains(double matchDb, bool matched, float& gProc, float& gOrig);

    void prepareToPlay(int, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

    /** Monitored signal (mono sum) for the realtime spectrum. */
    int readScope(float* dest, int maxSamples);

private:
    mutable juce::SpinLock lock_;
    std::shared_ptr<const PlaybackSet> set_;
    std::atomic<int> target_{ 0 };
    int current_ = 0;
    int fadePos_ = 0, fadeLen_ = 960;
    int fadeFrom_ = 0;
    std::atomic<bool> playing_{ false }, matched_{ true }, loop_{ false };
    std::atomic<int64_t> pos_{ 0 };
    std::atomic<double> deviceRate_{ 48000.0 };
    juce::AbstractFifo fifo_{ 16384 };
    std::vector<float> fifoBuf_ = std::vector<float>(16384, 0.0f);
};
