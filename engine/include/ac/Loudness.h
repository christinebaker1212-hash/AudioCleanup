#pragma once
// ITU-R BS.1770-4 / EBU R128 measurement via libebur128.

#include "AudioBuffer.h"

#include <memory>
#include <vector>

namespace ac {

struct LoudnessStats
{
    double integrated = -std::numeric_limits<double>::infinity(); ///< LUFS (gated)
    double lra = 0.0;              ///< LU
    double maxMomentary = -std::numeric_limits<double>::infinity();
    double maxShortTerm = -std::numeric_limits<double>::infinity();
    double truePeakDb = kSilenceDb; ///< dBTP, max over channels (4x oversampled)
    double samplePeakDb = kSilenceDb;
    bool integratedValid() const { return std::isfinite(integrated); }
};

LoudnessStats measureLoudness(const AudioBuffer& b, bool withTruePeak = true, bool withLra = true);

/** Integrated loudness of a buffer, or -inf if undefined (silence / too short). */
double integratedLoudness(const AudioBuffer& b);

/** Momentary (400 ms) and short-term (3 s) loudness sampled every hopSeconds. */
struct LoudnessCurve
{
    double hopSeconds = 0.1;
    std::vector<float> momentary;
    std::vector<float> shortTerm;
};
LoudnessCurve loudnessCurve(const AudioBuffer& b, double hopSeconds = 0.1);

/** True peak (dBTP, 4x oversampled, max over channels) per hop. */
std::vector<float> truePeakCurve(const AudioBuffer& b, double hopSeconds = 0.1);

/** Incremental meter for live playback metering. */
class LiveLoudnessMeter
{
public:
    LiveLoudnessMeter();
    ~LiveLoudnessMeter();
    void prepare(double sr, int channels);
    void reset();
    void addFrames(const float* const* ch, int numCh, int n);
    double momentary() const;
    double shortTerm() const;
    double integrated() const;
    double truePeakDb() const; ///< since reset

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ac
