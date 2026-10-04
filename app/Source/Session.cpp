#include "Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <ac/Resampler.h>

Worker::Worker() : thread_([this] { run(); }) {}

Worker::~Worker()
{
    quit_ = true;
    cancel_ = true;
    cv_.notify_all();
    thread_.join();
}

void Worker::post(juce::String label, Task t)
{
    {
        std::lock_guard<std::mutex> l(m_);
        q_.emplace_back(std::move(label), std::move(t));
    }
    cv_.notify_all();
}

void Worker::cancelAll()
{
    {
        std::lock_guard<std::mutex> l(m_);
        q_.clear();
    }
    cancel_ = true;
}

juce::String Worker::what() const
{
    std::lock_guard<std::mutex> l(whatM_);
    return what_;
}

void Worker::run()
{
    while (!quit_)
    {
        std::pair<juce::String, Task> job;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [this] { return quit_ || !q_.empty(); });
            if (quit_) return;
            job = std::move(q_.front());
            q_.pop_front();
            cancel_ = false;
            busy_ = true;
        }
        {
            std::lock_guard<std::mutex> l(whatM_);
            what_ = job.first;
        }
        progress_ = 0.0;
        ac::Job j;
        j.cancel = &cancel_;
        j.progress = [this, label = job.first](double f, const std::string& w) {
            progress_ = f;
            std::lock_guard<std::mutex> l(whatM_);
            what_ = w.empty() ? label : label + ": " + juce::String(w);
        };
        try
        {
            job.second(j);
        }
        catch (const ac::CancelledException&)
        {
        }
        catch (const std::exception& e)
        {
            const juce::String msg = e.what();
            juce::MessageManager::callAsync([msg] {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Processing error", msg);
            });
        }
        busy_ = !q_.empty();
        progress_ = 0.0;
        std::lock_guard<std::mutex> l(whatM_);
        what_ = {};
    }
}

namespace {
void buckets(const ac::AudioBuffer& b, size_t n, std::vector<float>& mn, std::vector<float>& mx, size_t totalFrames)
{
    mn.assign(n, 0.0f);
    mx.assign(n, 0.0f);
    if (b.empty() || totalFrames == 0) return;
    for (size_t k = 0; k < n; ++k)
    {
        const size_t a = k * totalFrames / n, e = std::min(b.numFrames(), (k + 1) * totalFrames / n);
        float lo = 0, hi = 0;
        for (int c = 0; c < b.numChannels(); ++c)
            for (size_t i = a; i < e; ++i)
            {
                lo = std::min(lo, b.channel(c)[i]);
                hi = std::max(hi, b.channel(c)[i]);
            }
        mn[k] = lo;
        mx[k] = hi;
    }
}

std::vector<std::vector<float>> toDevice(const ac::AudioBuffer& b, double rate, const ac::Job& job)
{
    ac::AudioBuffer r = ac::resample(b, rate, job);
    std::vector<std::vector<float>> out;
    for (int c = 0; c < r.numChannels(); ++c) out.push_back(r.vec(c));
    return out;
}
} // namespace

std::shared_ptr<DisplayData> buildDisplay(const ac::ProcessResult& r, const ac::AudioBuffer&)
{
    auto d = std::make_shared<DisplayData>();
    const size_t n = r.output.numFrames();
    d->sampleRate = r.output.sampleRate;
    d->durationS = double(n) / r.output.sampleRate;
    const size_t B = 4096;
    buckets(r.reference, B, d->origMin, d->origMax, n);
    buckets(r.output, B, d->procMin, d->procMax, n);
    d->grTrace = r.grTrace;
    d->outCurve = ac::loudnessCurve(r.output, 0.1);
    d->refCurve = ac::loudnessCurve(r.reference, 0.1);
    d->outTp = ac::truePeakCurve(r.output, 0.1);
    d->refTp = ac::truePeakCurve(r.reference, 0.1);
    d->outLtas = ac::longTermSpectrum(r.output);
    d->refLtas = ac::longTermSpectrum(r.reference);
    return d;
}

std::shared_ptr<DisplayData> buildDisplayOriginal(const ac::AudioBuffer& a)
{
    auto d = std::make_shared<DisplayData>();
    const size_t n = a.numFrames();
    d->sampleRate = a.sampleRate;
    d->durationS = a.durationSeconds();
    buckets(a, 4096, d->origMin, d->origMax, n);
    d->refCurve = ac::loudnessCurve(a, 0.1);
    d->refTp = ac::truePeakCurve(a, 0.1);
    d->refLtas = ac::longTermSpectrum(a);
    return d;
}

std::shared_ptr<PlaybackSet> buildPlayback(const ac::ProcessResult& r, double deviceRate, const ac::Job& job)
{
    auto p = std::make_shared<PlaybackSet>();
    p->sampleRate = deviceRate;
    p->processed = toDevice(r.output, deviceRate, job);
    p->original = toDevice(r.reference, deviceRate, job);
    p->removed = toDevice(r.removed, deviceRate, job);
    p->matchDb = r.matchGainDb;
    // Mono sources are monitored on both speakers.
    for (auto* v : { &p->processed, &p->original, &p->removed })
        if (v->size() == 1) v->push_back((*v)[0]);
    return p;
}

std::shared_ptr<PlaybackSet> buildPlaybackOriginal(const ac::AudioBuffer& a, double deviceRate, const ac::Job& job)
{
    auto p = std::make_shared<PlaybackSet>();
    p->sampleRate = deviceRate;
    p->original = toDevice(a, deviceRate, job);
    p->processed = p->original;
    p->removed.assign(p->original.size(), std::vector<float>(p->original.empty() ? 0 : p->original[0].size(), 0.0f));
    if (p->original.size() == 1)
    {
        p->original.push_back(p->original[0]);
        p->processed.push_back(p->processed[0]);
        p->removed.push_back(p->removed[0]);
    }
    return p;
}
