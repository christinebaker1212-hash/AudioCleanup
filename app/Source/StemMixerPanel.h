#pragma once
// Aligned-stem mode: track roles, gain, pan, bus routing, mute, per-stem
// treatment toggle, bus gains; renders through ac::mixStems.

#include "Theme.h"

#include <ac/StemMixer.h>

#include <juce_core/juce_core.h>

struct StemEntry
{
    juce::File file;
    std::shared_ptr<const ac::AudioBuffer> audio;
    ac::StemRole role = ac::StemRole::Other;
    double gainDb = 0, pan = 0;
    int bus = 1;
    bool mute = false, process = true;
};

class StemMixerPanel : public juce::Component
{
public:
    StemMixerPanel();
    ~StemMixerPanel() override;
    std::function<void()> onAddStems, onMix;

    void addStem(const juce::File& f, std::shared_ptr<const ac::AudioBuffer> audio);
    std::vector<StemEntry>& stems() { return stems_; }
    std::vector<ac::BusSpec> buses() const;
    const ac::PresetDef* masterPreset() const;
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class Row;
    void rebuild();
    std::vector<StemEntry> stems_;
    juce::OwnedArray<Row> rows_;
    juce::Component rowHolder_;
    juce::Viewport viewport_;
    juce::TextButton add_{ "Add aligned stems..." }, clear_{ "Clear" }, mix_{ "MIX + MASTER" };
    juce::ComboBox master_;
    juce::Label masterL_, info_;
    juce::Slider busGain_[3];
    juce::ToggleButton busGlue_[3];
    juce::Label busL_[3];
};
