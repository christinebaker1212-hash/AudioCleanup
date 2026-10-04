#include "ac/Fft.h"
#include "ac/Common.h"

#include <pffft.h>
#include <stdexcept>

namespace ac {

RealFft::RealFft(int n) : n_(n)
{
    setup_ = pffft_new_setup(n, PFFFT_REAL);
    if (!setup_) throw std::runtime_error("Unsupported FFT size");
    bufIn_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * size_t(n)));
    bufOut_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * size_t(n)));
    work_ = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * size_t(n)));
}

RealFft::~RealFft()
{
    pffft_destroy_setup(static_cast<PFFFT_Setup*>(setup_));
    pffft_aligned_free(bufIn_);
    pffft_aligned_free(bufOut_);
    pffft_aligned_free(work_);
}

void RealFft::forward(const float* in, std::complex<float>* out)
{
    std::copy(in, in + n_, bufIn_);
    pffft_transform_ordered(static_cast<PFFFT_Setup*>(setup_), bufIn_, bufOut_, work_, PFFFT_FORWARD);
    out[0] = { bufOut_[0], 0.0f };
    out[n_ / 2] = { bufOut_[1], 0.0f };
    for (int k = 1; k < n_ / 2; ++k) out[k] = { bufOut_[2 * k], bufOut_[2 * k + 1] };
}

void RealFft::inverse(const std::complex<float>* in, float* out)
{
    bufIn_[0] = in[0].real();
    bufIn_[1] = in[n_ / 2].real();
    for (int k = 1; k < n_ / 2; ++k)
    {
        bufIn_[2 * k] = in[k].real();
        bufIn_[2 * k + 1] = in[k].imag();
    }
    pffft_transform_ordered(static_cast<PFFFT_Setup*>(setup_), bufIn_, bufOut_, work_, PFFFT_BACKWARD);
    const float scale = 1.0f / float(n_);
    for (int i = 0; i < n_; ++i) out[i] = bufOut_[i] * scale;
}

std::vector<float> hannWindow(int n)
{
    std::vector<float> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[size_t(i)] = float(0.5 - 0.5 * std::cos(kTwoPi * i / n));
    return w;
}

} // namespace ac
