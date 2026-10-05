#pragma once
#include "ac/Pipeline.h"

namespace ac {
std::string fmt(const char* f, ...);
void initPlan(Plan& plan);
void planCleanupTone(Plan& plan, const AnalysisReport& a, const PresetDef& p, const UserControls& uc, const AudioBuffer& in);
/** Threshold giving ~targetGr dB of reduction at the P95 detector level of x. */
double compressorThreshold(const AudioBuffer& x, const CompressorSettings& c, double targetGr, double gateDb, double* p95Out = nullptr);
/** Level-neutral deviation (reference - source, dB) per source 1/3-octave band,
    smoothed over neighbours; bands with no reference data are 0. Returns the
    rms deviation (100 Hz-12 kHz) through rmsOut. */
std::vector<double> referenceDeviation(const SpectrumInfo& src, const SpectrumInfo& ref, double* rmsOut = nullptr);
/** Fit broad EQ bands to a deviation curve (bounded). */
std::vector<EqBand> fitEqBands(const std::vector<double>& hz, const std::vector<double>& dev, double maxBoost, double maxCut, double sr,
                               Category cat, double minBellHz, int maxBells);
/** Snap a release time to the nearest note length (1/32..1/2 note) at bpm when
    within a factor of 1.6; returns ms unchanged otherwise. label gets e.g. "1/16 note at 124 BPM". */
double snapToNote(double ms, double bpm, std::string* label = nullptr);
void planDynamics(Plan& plan, const AnalysisReport& a, const PresetDef& p, const UserControls& uc, const AudioBuffer& x1);
} // namespace ac
