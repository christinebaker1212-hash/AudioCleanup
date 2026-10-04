#pragma once

#include "ControlsPanel.h"
#include "Playback.h"
#include "Session.h"
#include "StageTable.h"
#include "StemMixerPanel.h"
#include "Theme.h"
#include "Views.h"

#include <juce_audio_utils/juce_audio_utils.h>

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ListBoxModel
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& k) override;

    /** files... [--category voice|sfx|music] [--preset id] [--process] [--ab original|removed]
        [--screenshot out.png] [--quit]  (also used for "Open with" and automated UI checks) */
    void handleCommandLine(const juce::StringArray& args);

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    // ListBoxModel (file list)
    int getNumRows() override { return int(items_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void timerCallback() override;

    void addFiles(const juce::Array<juce::File>& files, bool asStems);
    void selectItem(std::shared_ptr<SessionItem> item);
    std::shared_ptr<SessionItem> selected() const { return current_; }
    void processItem(std::shared_ptr<SessionItem> item, std::optional<double> targetOverride = std::nullopt,
                     std::function<void(std::shared_ptr<SessionItem>)> then = {});
    void processSelected();
    void exportSelected();
    void processAll(bool exportToo, juce::File folder);
    void runStemMix();
    void ensurePlayback(std::shared_ptr<SessionItem> item);
    void refreshDetails();
    void updateMeters();
    void setAB(ABPlayer::Source s);
    void scheduleAutoRender();
    void showAudioSettings();
    juce::File defaultExportFile(const SessionItem& it, const af::ExportOptions& ex) const;

    theme::LookAndFeel laf_;
    juce::AudioDeviceManager deviceManager_;
    juce::AudioSourcePlayer sourcePlayer_;
    ABPlayer player_;
    Worker worker_;

    std::vector<std::shared_ptr<SessionItem>> items_;
    std::shared_ptr<SessionItem> current_;

    juce::Label title_;
    juce::TextButton addFiles_{ "Add files..." }, removeFile_{ "Remove" }, play_{ "Play" }, stop_{ "Stop" };
    juce::TextButton abProc_{ "Processed" }, abOrig_{ "Original" }, abRemoved_{ "Removed noise" };
    juce::ToggleButton matched_{ "Loudness-matched A/B" }, loop_{ "Loop" };
    juce::TextButton clearRegion_{ "Clear noise region" };
    juce::ListBox fileList_{ "Files", this };
    WaveformView waveform_;
    SpectrumView spectrum_;
    MeterView meters_;
    juce::TabbedComponent tabs_{ juce::TabbedButtonBar::TabsAtTop };
    StageTable stageTable_;
    juce::TextEditor analysisText_, logText_;
    StemMixerPanel stemPanel_;
    ControlsPanel controls_;
    juce::Label hint_;

    std::unique_ptr<juce::FileChooser> chooser_;
    int autoRenderCountdown_ = -1;
    std::vector<float> scopeBuf_ = std::vector<float>(16384);
    std::shared_ptr<SessionItem> stemItem_;
    struct Script
    {
        bool active = false, process = false, quit = false, processPosted = false, mix = false;
        int stemsExpected = 0;
        juce::String preset, ab;
        juce::File screenshot;
        int ticks = 0, settle = 0;
    } script_;
    void runScript();
};
