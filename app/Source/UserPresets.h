#pragma once
// User presets: a full preset definition (constraints) plus the user's
// macro/section/override settings, stored as JSON in the user's app-data folder.

#include <ac/Pipeline.h>

#include <juce_core/juce_core.h>

struct UserPreset
{
    ac::PresetDef def;
    ac::UserControls controls;
    juce::File file;
};

namespace userpresets {
juce::File folder();
std::vector<UserPreset> loadAll();
bool save(const juce::String& name, const ac::PresetDef& base, const ac::UserControls& controls, juce::String& error);
bool remove(const UserPreset& p);
juce::var controlsToVar(const ac::UserControls& c);
ac::UserControls controlsFromVar(const juce::var& v);
} // namespace userpresets
