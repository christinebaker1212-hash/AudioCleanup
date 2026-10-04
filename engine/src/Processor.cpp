#include "ac/Processor.h"

namespace ac {

void trimSilentTail(AudioBuffer& b, size_t minLength, double floorDb)
{
    const float thr = float(dbToGain(floorDb));
    size_t end = b.numFrames();
    while (end > minLength)
    {
        bool loud = false;
        for (int c = 0; c < b.numChannels(); ++c)
            if (std::abs(b.channel(c)[end - 1]) > thr) { loud = true; break; }
        if (loud) break;
        --end;
    }
    b.resizeFrames(end);
}

AudioBuffer renderProcessor(Processor& p, const AudioBuffer& input, const RenderOptions& opt, const Job& job)
{
    const int nch = input.numChannels();
    p.prepare(input.sampleRate, nch);

    if (p.isOffline())
    {
        AudioBuffer out = input;
        p.processOffline(out, job);
        if (!opt.keepTail) out.resizeFrames(input.numFrames());
        return out;
    }

    const size_t inLen = input.numFrames();
    const size_t lat = size_t(std::max(0, p.latency()));
    const size_t tail = opt.keepTail ? size_t(std::max(0, p.tail())) : 0;
    const size_t outLen = inLen + tail;
    const size_t total = outLen + lat; // frames to push through

    AudioBuffer out(nch, outLen, input.sampleRate);
    const int bs = std::max(1, opt.blockSize);
    std::vector<std::vector<float>> blk(static_cast<size_t>(nch), std::vector<float>(size_t(bs)));
    std::vector<float*> ptrs(static_cast<size_t>(nch));
    for (int c = 0; c < nch; ++c) ptrs[size_t(c)] = blk[size_t(c)].data();

    size_t pos = 0;
    size_t blockCounter = 0;
    while (pos < total)
    {
        const int n = int(std::min<size_t>(size_t(bs), total - pos));
        for (int c = 0; c < nch; ++c)
        {
            float* d = blk[size_t(c)].data();
            const float* src = input.channel(c);
            for (int i = 0; i < n; ++i)
            {
                const size_t idx = pos + size_t(i);
                d[i] = idx < inLen ? src[idx] : 0.0f;
            }
        }
        p.process(ptrs.data(), nch, n);
        for (int c = 0; c < nch; ++c)
        {
            const float* d = blk[size_t(c)].data();
            float* dst = out.channel(c);
            for (int i = 0; i < n; ++i)
            {
                const size_t idx = pos + size_t(i);
                if (idx >= lat && idx - lat < outLen) dst[idx - lat] = d[i];
            }
        }
        pos += size_t(n);
        if ((++blockCounter & 63) == 0)
        {
            throwIfCancelled(job);
            job.report(double(pos) / double(total), "");
        }
    }
    if (tail > 0) trimSilentTail(out, inLen, opt.tailFloorDb);
    return out;
}

} // namespace ac
