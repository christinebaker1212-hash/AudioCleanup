#pragma once
// Session model and background worker. All analysis, rendering and export
// run on the worker thread; results are handed to the message thread.

#include "AudioIO.h"

#include <ac/Pipeline.h>

#include <juce_core/juce_core.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

struct DisplayData
{
    double durationS = 0;
    double sampleRate = 48000;
    std::vector<float> origMin, origMax, procMin, procMax; ///< waveform buckets
    std::vector<float> grTrace;                           ///< 10 ms hops (dB)
    ac::LoudnessCurve outCurve, refCurve;                  ///< 100 ms hops
    std::vector<float> outTp, refTp;                       ///< 100 ms hops (dBTP)
    ac::SpectrumInfo outLtas, refLtas;
};

/** Device-rate buffers for A/B monitoring. */
struct PlaybackSet
{
    std::vector<std::vector<float>> processed, original, removed;
    double sampleRate = 48000;
    double matchDb = 0.0; ///< processed loudness minus original loudness
    size_t length() const { return processed.empty() ? 0 : processed[0].size(); }
};

struct SessionItem
{
    juce::File file;
    std::shared_ptr<const ac::AudioBuffer> audio;
    af::SourceInfo info;
    std::shared_ptr<const ac::AnalysisReport> analysis;
    ac::Category analysisCategory = ac::Category::Voice;
    std::shared_ptr<const ac::ProcessResult> result;
    std::shared_ptr<const DisplayData> display;
    std::shared_ptr<const PlaybackSet> playback;
    juce::String status = "loaded";
    juce::String presetUsed;
    double playbackDeviceRate = 0;
};

/** Single background thread executing tasks in order. A task may be
    cancelled through the shared flag; progress is published atomically. */
class Worker
{
public:
    using Task = std::function<void(const ac::Job&)>;
    Worker();
    ~Worker();
    void post(juce::String label, Task t);
    void cancelAll();
    bool busy() const { return busy_.load(); }
    double progress() const { return progress_.load(); }
    juce::String what() const;

private:
    void run();
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<juce::String, Task>> q_;
    std::atomic<bool> quit_{ false }, busy_{ false }, cancel_{ false };
    std::atomic<double> progress_{ 0.0 };
    mutable std::mutex whatM_;
    juce::String what_;
};

/** Builds the display + playback data for a result (worker thread). */
std::shared_ptr<DisplayData> buildDisplay(const ac::ProcessResult& r, const ac::AudioBuffer& source);
std::shared_ptr<PlaybackSet> buildPlayback(const ac::ProcessResult& r, double deviceRate, const ac::Job& job);
/** Display/playback for an unprocessed file (original only). */
std::shared_ptr<DisplayData> buildDisplayOriginal(const ac::AudioBuffer& a);
std::shared_ptr<PlaybackSet> buildPlaybackOriginal(const ac::AudioBuffer& a, double deviceRate, const ac::Job& job);
