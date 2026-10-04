#pragma once
#include "AudioBuffer.h"

#include <cstdint>

namespace ac {

enum class DitherType
{
    None,          ///< plain rounding (use only for 32-bit float or test material)
    Tpdf,          ///< flat triangular-PDF dither, 2 LSB p-p
    TpdfShaped     ///< TPDF + 3-tap F-weighted error feedback (Wannamaker), 44.1/48 kHz only
};

/** Quantise float audio to integer PCM with dither. Returns samples as int32
    holding values in [-2^(bits-1), 2^(bits-1)-1]. Deterministic (seeded). */
std::vector<std::vector<int32_t>> quantise(const AudioBuffer& b, int bits, DitherType type, uint32_t seed = 0x1234567u);

/** The same quantisation written back as float (for preview / measurement). */
AudioBuffer quantiseToFloat(const AudioBuffer& b, int bits, DitherType type, uint32_t seed = 0x1234567u);

} // namespace ac
