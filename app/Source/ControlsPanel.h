#pragma once
// Right-hand panel: category, preset, Process, the four simple sections
// (Cleanup / Tone / Dynamics / Output), optional processors, export and batch.

#include "AudioIO.h"
#include "Theme.h"
#include "UserPresets.h"

#include <ac/Pipeline.h>

class ControlsPanel : public juce::Component
{
public:
    ControlsPanel();

    // ---- state
    ac::Category category() const { return category_; }
    const ac::PresetDef* preset() const;
    ac::UserControls controls() const;
    af::ExportOptions exportOptions() const;
    bool autoUpdate() const { return autoUpdate_.getToggleState(); }
    bool batchConsistency() const { return consistency_.getToggleState(); }
    double batchPreserve() const { return preserve_.getValue() / 100.0; }
    void setNoiseRegion(std::optional<std::pair<size_t, size_t>> r) { noiseRegion_ = r; }
    void setOverride(ac::StageId id, ac::Override o);
    const std::map<ac::StageId, ac::Override>& overrides() const { return overrides_; }
    void setBusy(bool busy, double progress, const juce::String& what);
    void setCategory(ac::Category c, bool notify);
    bool selectPreset(const std::string& id);

    // ---- callbacks
    std::function<void()> onProcess, onCancel, onExport, onProcessAll, onExportAll, onControlsChanged, onCategoryChanged,
        onAudioSettings;
    std::function<void(double)> onUiScale;
    std::function<void()> onLoadReference;

    /** Reference track for matching (nullptr = generic preset targets). */
    void setReference(std::shared_ptr<const ac::ReferenceProfile> r);

    /** Height the content needs at the current width (the panel scrolls when taller than the view). */
    int contentHeight() const { return contentHeight_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void rebuildPresetList();
    void presetChanged();
    void changed();
    void addSlider(juce::Slider& s, double lo, double hi, double step, double def, const juce::String& suffix);

    ac::Category category_ = ac::Category::Voice;
    std::vector<UserPreset> userPresets_;
    std::vector<const ac::PresetDef*> presetPtrs_;
    std::map<ac::StageId, ac::Override> overrides_;
    std::optional<std::pair<size_t, size_t>> noiseRegion_;
    bool loading_ = false;

    juce::TextButton catVoice_{ "Voice Clip" }, catSfx_{ "Sound Effect" }, catMusic_{ "Music" };
    juce::ComboBox presetBox_;
    juce::Label presetDesc_;
    juce::TextButton savePreset_{ "Save preset..." }, deletePreset_{ "Delete" };
    juce::TextButton process_{ "PROCESS" }, cancel_{ "Cancel" };
    double progress_ = 0.0;
    juce::ProgressBar progressBar_{ progress_ };
    juce::Label status_;
    juce::ToggleButton autoUpdate_{ "Auto re-render on change" };

    juce::ToggleButton cleanupOn_{ "Cleanup" }, toneOn_{ "Tone" }, dynOn_{ "Dynamics" }, outOn_{ "Output" };
    juce::Slider cleanup_, tone_, tilt_, dynamics_, target_, ceiling_;
    juce::Label cleanupL_, toneL_, tiltL_, dynL_, targetL_, ceilingL_, modeL_, srL_;
    juce::ComboBox mode_, sampleRate_;
    juce::ToggleButton optNeural_{ "Force neural enhancer" }, optDereverb_{ "Force de-reverb" }, optMultiband_{ "Force multiband" },
        optSaturation_{ "Force saturation" };

    juce::ComboBox format_, bits_, dither_;
    juce::TextButton export_{ "Export..." }, processAll_{ "Process all" }, exportAll_{ "Export all..." }, audio_{ "Audio device..." };
    juce::ToggleButton consistency_{ "Batch consistency" };
    std::shared_ptr<const ac::ReferenceProfile> reference_;
    juce::TextButton refLoad_{ "Match a reference..." }, refClear_{ "Clear" };
    juce::Label refName_, refAmountL_;
    juce::Slider refAmount_;
    juce::ToggleButton refLoudness_{ "Match its loudness" };
    juce::ToggleButton refDynamics_{ "Match its dynamics" };
    juce::Label uiSizeL_;
    juce::ComboBox uiSize_;
    int contentHeight_ = 800;
    juce::Slider preserve_;
    std::unique_ptr<juce::AlertWindow> nameDialog_;
};
