#pragma once
#include <complex>
#include <memory>
#include <vector>

namespace ac {

/** Real FFT (PFFFT backend). Sizes must be a multiple of 32 with factors 2,3,5.
    forward(): n real samples -> n/2+1 complex bins (unnormalised).
    inverse(): n/2+1 bins -> n real samples, scaled by 1/n (exact round trip). */
class RealFft
{
public:
    explicit RealFft(int n);
    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    int size() const { return n_; }
    int numBins() const { return n_ / 2 + 1; }

    void forward(const float* in, std::complex<float>* out);
    void inverse(const std::complex<float>* in, float* out);

private:
    int n_;
    void* setup_ = nullptr;
    float* bufIn_ = nullptr;
    float* bufOut_ = nullptr;
    float* work_ = nullptr;
};

/** Periodic Hann window. */
std::vector<float> hannWindow(int n);

} // namespace ac
