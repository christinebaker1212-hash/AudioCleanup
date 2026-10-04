#include "ac/Resampler.h"

#include <CDSPResampler.h>
#include <memory>

namespace ac {

AudioBuffer resample(const AudioBuffer& in, double dstRate, const Job& job)
{
    if (std::abs(in.sampleRate - dstRate) < 1e-9 || in.empty())
    {
        AudioBuffer out = in;
        out.sampleRate = dstRate;
        return out;
    }
    const size_t inLen = in.numFrames();
    const size_t outLen = size_t(std::llround(double(inLen) * dstRate / in.sampleRate));
    AudioBuffer out(in.numChannels(), outLen, dstRate);

    const int blk = 4096;
    std::vector<double> ib(static_cast<size_t>(blk));
    for (int c = 0; c < in.numChannels(); ++c)
    {
        // 24-bit profile (ReqAtten 180.15 dB) with 2% transition band.
        auto rs = std::make_unique<r8b::CDSPResampler24>(in.sampleRate, dstRate, blk, 2.0);
        const float* src = in.channel(c);
        float* dst = out.channel(c);
        size_t written = 0, pos = 0;
        while (written < outLen)
        {
            for (int i = 0; i < blk; ++i) ib[size_t(i)] = pos + size_t(i) < inLen ? double(src[pos + size_t(i)]) : 0.0;
            pos += size_t(blk);
            double* op = nullptr;
            const int n = rs->process(ib.data(), blk, op);
            for (int i = 0; i < n && written < outLen; ++i) dst[written++] = float(op[i]);
            throwIfCancelled(job);
        }
        job.report(double(c + 1) / double(in.numChannels()), "Resampling");
    }
    return out;
}

} // namespace ac
