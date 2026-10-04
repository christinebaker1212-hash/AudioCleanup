#pragma once
#include "AudioBuffer.h"

#include <memory>
#include <string>

namespace ac {

/** Records the maximum gain reduction (positive dB) per hop for metering. */
class GrTrace
{
public:
    void reset(double sampleRate, double hopSeconds = 0.01)
    {
        hop_ = std::max(1, int(std::lround(sampleRate * hopSeconds)));
        count_ = 0;
        cur_ = 0.0f;
        values_.clear();
    }
    inline void push(float grDb)
    {
        cur_ = std::max(cur_, grDb);
        if (++count_ >= hop_)
        {
            values_.push_back(cur_);
            cur_ = 0.0f;
            count_ = 0;
        }
    }
    int hop() const { return hop_; }
    const std::vector<float>& values() const { return values_; }
    float maxValue() const
    {
        float m = 0;
        for (float v : values_) m = std::max(m, v);
        return m;
    }

private:
    int hop_ = 480, count_ = 0;
    float cur_ = 0.0f;
    std::vector<float> values_;
};

/** Streaming processor interface. Every processor is fully deterministic and
    block-size independent: rendering with any block partitioning produces the
    same output (verified by tests). Latency is reported in samples and is
    removed by the renderer so every stage output is time-aligned with its input. */
class Processor
{
public:
    virtual ~Processor() = default;

    /** Allocate and reset state. */
    virtual void prepare(double sampleRate, int numChannels) = 0;
    /** Samples of delay this processor introduces. */
    virtual int latency() const { return 0; }
    /** Samples of output that continue after the input ends (filter ring, release). */
    virtual int tail() const { return 0; }
    /** In-place processing of n frames. */
    virtual void process(float* const* channels, int numChannels, int n) = 0;

    /** Processors that need the whole signal (two-pass / non-causal smoothing)
        override these; the renderer then calls processOffline() instead. */
    virtual bool isOffline() const { return false; }
    virtual void processOffline(AudioBuffer& /*buffer*/, const Job& /*job*/) {}

    /** Gain-reduction history (positive dB per hop), if the processor has one.
        Values are in processing time; the renderer aligns them for latency. */
    virtual const GrTrace* grTrace() const { return nullptr; }
};

struct RenderOptions
{
    int blockSize = 512;
    bool keepTail = true;          ///< append tail() samples after the input end
    double tailFloorDb = -120.0;   ///< trailing tail samples below this are trimmed
};

/** Run a processor over a whole buffer: compensates latency (output is aligned
    with the input sample-for-sample), renders the tail, and trims silent tail. */
AudioBuffer renderProcessor(Processor& p, const AudioBuffer& input, const RenderOptions& opt, const Job& job);

/** Trim trailing samples beyond minLength whose magnitude is below floorDb. */
void trimSilentTail(AudioBuffer& b, size_t minLength, double floorDb);

} // namespace ac
