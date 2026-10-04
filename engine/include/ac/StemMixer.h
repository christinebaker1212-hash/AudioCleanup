#pragma once
// Aligned-stem mixing: per-stem role-aware treatment (cleanup / tone /
// dynamics, no delivery stage), gain + constant-power pan, routing into
// buses with measured glue compression, then the master chain (a Music
// preset) on the stereo sum.

#include "Pipeline.h"

namespace ac {

enum class StemRole { LeadVocal, BackingVocal, Dialogue, Drums, Bass, Keys, Guitar, Synth, Strings, Fx, Ambience, Other, Count };
const char* stemRoleName(StemRole r);

struct StemSpec
{
    const AudioBuffer* audio = nullptr;
    std::string name;
    StemRole role = StemRole::Other;
    double gainDb = 0.0;
    double pan = 0.0;        ///< -1 (L) .. +1 (R); stereo stems: balance
    int bus = 0;
    bool mute = false;
    bool process = true;     ///< role-aware stem treatment
};

struct BusSpec
{
    std::string name;
    double gainDb = 0.0;
    bool glue = true;        ///< measured 2:1 glue compression (~1.5 dB GR)
};

struct StemMixRequest
{
    std::vector<StemSpec> stems;
    std::vector<BusSpec> buses;
    const PresetDef* masterPreset = nullptr;
    UserControls masterControls;
};

struct StemMixResult
{
    ProcessResult master;       ///< master render; reference = static (unprocessed) mix
    std::vector<std::string> log;
};

/** Default bus for a role (0 = Vocals, 1 = Music, 2 = FX/Ambience). */
int defaultBusFor(StemRole r);
std::vector<BusSpec> defaultBuses();
/** Guess a role from a file name (e.g. "Kick", "Vox", "Bass"). */
StemRole guessRole(const std::string& name);
/** Stem treatment preset for a role (constraints only; delivery is off). */
PresetDef stemPreset(StemRole r);

StemMixResult mixStems(const StemMixRequest& req, const Job& job = {});

} // namespace ac
