#pragma once
#include "AudioBuffer.h"

namespace ac {

/** High-quality linear-phase sample-rate conversion (r8brain-free-src,
    24-bit profile: 180 dB stop-band attenuation, 2% transition band).
    Output is time-aligned with the input (latency consumed) and has length
    round(inFrames * dstRate / srcRate). */
AudioBuffer resample(const AudioBuffer& in, double dstRate, const Job& job = {});

} // namespace ac
