#pragma once
#include "ac/Pipeline.h"

namespace ac {
std::string fmt(const char* f, ...);
void initPlan(Plan& plan);
void planCleanupTone(Plan& plan, const AnalysisReport& a, const PresetDef& p, const UserControls& uc, const AudioBuffer& in);
/** Threshold giving ~targetGr dB of reduction at the P95 detector level of x. */
double compressorThreshold(const AudioBuffer& x, const CompressorSettings& c, double targetGr, double gateDb, double* p95Out = nullptr);
void planDynamics(Plan& plan, const AnalysisReport& a, const PresetDef& p, const UserControls& uc, const AudioBuffer& x1);
} // namespace ac
