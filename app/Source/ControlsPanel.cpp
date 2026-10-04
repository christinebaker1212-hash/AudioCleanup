#include "ControlsPanel.h"

namespace {
const double kRates[] = { 0.0, 44100.0, 48000.0, 88200.0, 96000.0 };
}

ControlsPanel::ControlsPanel()
{
    for (auto* b : { &catVoice_, &catSfx_, &catMusic_ })
    {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(101);
        addAndMakeVisible(*b);
    }
    catVoice_.onClick = [this] { if (catVoice_.getToggleState()) setCategory(ac::Category::Voice, true); };
    catSfx_.onClick = [this] { if (catSfx_.getToggleState()) setCategory(ac::Category::SoundEffect, true); };
    catMusic_.onClick = [this] { if (catMusic_.getToggleState()) setCategory(ac::Category::Music, true); };
    catVoice_.setToggleState(true, juce::dontSendNotification);

    addAndMakeVisible(presetBox_);
    presetBox_.onChange = [this] { presetChanged(); };
    presetDesc_.setFont(theme::font(12));
    presetDesc_.setColour(juce::Label::textColourId, theme::dim);
    presetDesc_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(presetDesc_);
    addAndMakeVisible(savePreset_);
    addAndMakeVisible(deletePreset_);
    savePreset_.onClick = [this] {
        nameDialog_ = std::make_unique<juce::AlertWindow>("Save user preset", "Name for the preset (stores the preset constraints and all current controls):",
                                                          juce::MessageBoxIconType::NoIcon);
        nameDialog_->addTextEditor("name", preset() ? juce::String(preset()->name) + " (mine)" : "My preset");
        nameDialog_->addButton("Save", 1);
        nameDialog_->addButton("Cancel", 0);
        nameDialog_->enterModalState(true, juce::ModalCallbackFunction::create([this](int r) {
            if (r == 1 && preset())
            {
                const auto name = nameDialog_->getTextEditorContents("name").trim();
                juce::String err;
                if (name.isNotEmpty() && !userpresets::save(name, *preset(), controls(), err))
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Preset", err);
                rebuildPresetList();
                for (int i = 0; i < int(presetPtrs_.size()); ++i)
                    if (juce::String(presetPtrs_[size_t(i)]->name) == name) presetBox_.setSelectedItemIndex(i, juce::sendNotification);
            }
            nameDialog_.reset();
        }));
    };
    deletePreset_.onClick = [this] {
        const int idx = presetBox_.getSelectedItemIndex();
        const int builtins = int(ac::presetsFor(category_).size());
        if (idx >= builtins && idx - builtins < int(userPresets_.size()))
        {
            int count = 0;
            for (auto& up : userPresets_)
                if (up.def.category == category_ && count++ == idx - builtins) userpresets::remove(up);
            rebuildPresetList();
        }
    };

    process_.setColour(juce::TextButton::buttonColourId, theme::accent.darker(0.2f));
    process_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(process_);
    addAndMakeVisible(cancel_);
    process_.onClick = [this] { if (onProcess) onProcess(); };
    cancel_.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible(progressBar_);
    progressBar_.setColour(juce::ProgressBar::foregroundColourId, theme::accent);
    status_.setFont(theme::font(11));
    status_.setColour(juce::Label::textColourId, theme::dim);
    addAndMakeVisible(status_);
    autoUpdate_.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(autoUpdate_);

    for (auto* t : { &cleanupOn_, &toneOn_, &dynOn_, &outOn_ })
    {
        t->setToggleState(true, juce::dontSendNotification);
        t->onClick = [this] { changed(); };
        addAndMakeVisible(*t);
    }
    addSlider(cleanup_, 0, 200, 1, 100, "%");
    addSlider(tone_, 0, 200, 1, 100, "%");
    addSlider(tilt_, -3, 3, 0.1, 0, " dB");
    addSlider(dynamics_, 0, 200, 1, 100, "%");
    addSlider(target_, -36, -6, 0.5, -16, " LUFS");
    addSlider(ceiling_, -6, 0, 0.1, -1, " dBTP");
    auto lab = [this](juce::Label& l, const juce::String& t) {
        l.setText(t, juce::dontSendNotification);
        l.setFont(theme::font(12));
        l.setColour(juce::Label::textColourId, theme::dim);
        addAndMakeVisible(l);
    };
    lab(cleanupL_, "Repair depth");
    lab(toneL_, "Tonal correction");
    lab(tiltL_, "Tilt");
    lab(dynL_, "Dynamics amount");
    lab(targetL_, "Target");
    lab(ceilingL_, "Ceiling");
    lab(modeL_, "Loudness");
    lab(srL_, "Sample rate");
    mode_.addItemList({ "Unchanged", "Peak normalise", "Integrated (BS.1770)", "Max momentary", "Max short-term" }, 1);
    mode_.onChange = [this] { changed(); };
    addAndMakeVisible(mode_);
    sampleRate_.addItemList({ "Source rate", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, 1);
    sampleRate_.setSelectedId(1, juce::dontSendNotification);
    sampleRate_.onChange = [this] { changed(); };
    addAndMakeVisible(sampleRate_);

    auto opt = [this](juce::ToggleButton& b, ac::StageId id) {
        b.onClick = [this, &b, id] {
            overrides_[id] = b.getToggleState() ? ac::Override::On : ac::Override::Auto;
            changed();
        };
        addAndMakeVisible(b);
    };
    opt(optNeural_, ac::StageId::Neural);
    opt(optDereverb_, ac::StageId::Dereverb);
    opt(optMultiband_, ac::StageId::Multiband);
    opt(optSaturation_, ac::StageId::Saturation);

    format_.addItemList({ "WAV", "FLAC" }, 1);
    format_.setSelectedId(1);
    bits_.addItemList({ "16-bit PCM", "24-bit PCM", "32-bit float" }, 1);
    bits_.setSelectedId(2);
    dither_.addItemList({ "No dither", "TPDF dither", "TPDF + noise shaping" }, 1);
    dither_.setSelectedId(2);
    format_.onChange = [this] {
        if (format_.getSelectedId() == 2 && bits_.getSelectedId() == 3) bits_.setSelectedId(2);
    };
    for (auto* c : { &format_, &bits_, &dither_ }) addAndMakeVisible(*c);
    addAndMakeVisible(export_);
    addAndMakeVisible(processAll_);
    addAndMakeVisible(exportAll_);
    addAndMakeVisible(audio_);
    export_.onClick = [this] { if (onExport) onExport(); };
    processAll_.onClick = [this] { if (onProcessAll) onProcessAll(); };
    exportAll_.onClick = [this] { if (onExportAll) onExportAll(); };
    audio_.onClick = [this] { if (onAudioSettings) onAudioSettings(); };
    addAndMakeVisible(consistency_);
    preserve_.setSliderStyle(juce::Slider::LinearHorizontal);
    preserve_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    preserve_.setRange(0, 100, 1);
    preserve_.setValue(50);
    preserve_.setTextValueSuffix("% kept");
    preserve_.setTooltip("Fraction of each asset's loudness difference from the group median that is kept (intended differences).");
    addAndMakeVisible(preserve_);

    rebuildPresetList();
}

void ControlsPanel::addSlider(juce::Slider& s, double lo, double hi, double step, double def, const juce::String& suffix)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 20);
    s.setRange(lo, hi, step);
    s.setValue(def, juce::dontSendNotification);
    s.setTextValueSuffix(suffix);
    s.setDoubleClickReturnValue(true, def);
    s.onDragEnd = [this] { changed(); };
    s.onValueChange = [this, &s] {
        if (!s.isMouseButtonDown()) changed();
    };
    addAndMakeVisible(s);
}

void ControlsPanel::setCategory(ac::Category c, bool notify)
{
    category_ = c;
    catVoice_.setToggleState(c == ac::Category::Voice, juce::dontSendNotification);
    catSfx_.setToggleState(c == ac::Category::SoundEffect, juce::dontSendNotification);
    catMusic_.setToggleState(c == ac::Category::Music, juce::dontSendNotification);
    optNeural_.setEnabled(c == ac::Category::Voice);
    rebuildPresetList();
    if (notify && onCategoryChanged) onCategoryChanged();
}

void ControlsPanel::rebuildPresetList()
{
    loading_ = true;
    userPresets_ = userpresets::loadAll();
    presetBox_.clear(juce::dontSendNotification);
    presetPtrs_ = ac::presetsFor(category_);
    int id = 1;
    for (auto* p : presetPtrs_) presetBox_.addItem(p->name, id++);
    bool sep = false;
    for (auto& up : userPresets_)
    {
        if (up.def.category != category_) continue;
        if (!sep) { presetBox_.addSeparator(); sep = true; }
        presetPtrs_.push_back(&up.def);
        presetBox_.addItem("* " + juce::String(up.def.name), id++);
    }
    presetBox_.setSelectedItemIndex(0, juce::dontSendNotification);
    loading_ = false;
    presetChanged();
}

bool ControlsPanel::selectPreset(const std::string& id)
{
    for (int i = 0; i < int(presetPtrs_.size()); ++i)
        if (presetPtrs_[size_t(i)]->id == id)
        {
            presetBox_.setSelectedItemIndex(i, juce::sendNotification);
            return true;
        }
    return false;
}

const ac::PresetDef* ControlsPanel::preset() const
{
    const int i = presetBox_.getSelectedItemIndex();
    return i >= 0 && i < int(presetPtrs_.size()) ? presetPtrs_[size_t(i)] : nullptr;
}

void ControlsPanel::presetChanged()
{
    const auto* p = preset();
    if (!p) return;
    loading_ = true;
    presetDesc_.setText(p->description, juce::dontSendNotification);
    // Defaults from the preset; a user preset also restores its controls.
    ac::UserControls uc;
    for (auto& up : userPresets_)
        if (&up.def == p) uc = up.controls;
    mode_.setSelectedItemIndex(int(uc.loudnessMode.value_or(p->loudnessMode)), juce::dontSendNotification);
    target_.setValue(uc.targetLufs.value_or(p->loudnessMode == ac::LoudnessMode::Peak ? p->ceilingDbTP : p->targetLufs), juce::dontSendNotification);
    ceiling_.setValue(uc.ceilingDbTP.value_or(p->ceilingDbTP), juce::dontSendNotification);
    cleanup_.setValue(uc.cleanup * 100, juce::dontSendNotification);
    tone_.setValue(uc.tone * 100, juce::dontSendNotification);
    dynamics_.setValue(uc.dynamics * 100, juce::dontSendNotification);
    tilt_.setValue(uc.tiltDb, juce::dontSendNotification);
    cleanupOn_.setToggleState(uc.sectionOn[0], juce::dontSendNotification);
    toneOn_.setToggleState(uc.sectionOn[1], juce::dontSendNotification);
    dynOn_.setToggleState(uc.sectionOn[2], juce::dontSendNotification);
    outOn_.setToggleState(uc.sectionOn[3], juce::dontSendNotification);
    overrides_ = uc.overrides;
    optNeural_.setToggleState(overrides_[ac::StageId::Neural] == ac::Override::On, juce::dontSendNotification);
    optDereverb_.setToggleState(overrides_[ac::StageId::Dereverb] == ac::Override::On, juce::dontSendNotification);
    optMultiband_.setToggleState(overrides_[ac::StageId::Multiband] == ac::Override::On, juce::dontSendNotification);
    optSaturation_.setToggleState(overrides_[ac::StageId::Saturation] == ac::Override::On, juce::dontSendNotification);
    deletePreset_.setEnabled(p->id.rfind("user.", 0) == 0);
    loading_ = false;
    changed();
}

void ControlsPanel::setOverride(ac::StageId id, ac::Override o)
{
    overrides_[id] = o;
    if (id == ac::StageId::Neural) optNeural_.setToggleState(o == ac::Override::On, juce::dontSendNotification);
    if (id == ac::StageId::Dereverb) optDereverb_.setToggleState(o == ac::Override::On, juce::dontSendNotification);
    if (id == ac::StageId::Multiband) optMultiband_.setToggleState(o == ac::Override::On, juce::dontSendNotification);
    if (id == ac::StageId::Saturation) optSaturation_.setToggleState(o == ac::Override::On, juce::dontSendNotification);
    changed();
}

void ControlsPanel::changed()
{
    const bool peak = mode_.getSelectedItemIndex() == int(ac::LoudnessMode::Peak);
    const bool unchanged = mode_.getSelectedItemIndex() == int(ac::LoudnessMode::Unchanged);
    target_.setEnabled(!peak && !unchanged && outOn_.getToggleState());
    if (!loading_ && onControlsChanged) onControlsChanged();
}

ac::UserControls ControlsPanel::controls() const
{
    ac::UserControls c;
    c.sectionOn[0] = cleanupOn_.getToggleState();
    c.sectionOn[1] = toneOn_.getToggleState();
    c.sectionOn[2] = dynOn_.getToggleState();
    c.sectionOn[3] = outOn_.getToggleState();
    c.cleanup = cleanup_.getValue() / 100.0;
    c.tone = tone_.getValue() / 100.0;
    c.dynamics = dynamics_.getValue() / 100.0;
    c.tiltDb = tilt_.getValue();
    c.loudnessMode = ac::LoudnessMode(juce::jlimit(0, 4, mode_.getSelectedItemIndex()));
    c.targetLufs = target_.getValue();
    c.ceilingDbTP = ceiling_.getValue();
    c.outputSampleRate = kRates[juce::jlimit(0, 4, sampleRate_.getSelectedItemIndex())];
    for (auto& [id, o] : overrides_)
        if (o != ac::Override::Auto) c.overrides[id] = o;
    c.noiseRegion = noiseRegion_;
    return c;
}

af::ExportOptions ControlsPanel::exportOptions() const
{
    af::ExportOptions e;
    e.format = format_.getSelectedId() == 2 ? af::FileFormat::Flac : af::FileFormat::Wav;
    e.bitDepth = bits_.getSelectedId() == 1 ? 16 : bits_.getSelectedId() == 2 ? 24 : 32;
    e.dither = dither_.getSelectedId() == 1 ? ac::DitherType::None : dither_.getSelectedId() == 2 ? ac::DitherType::Tpdf : ac::DitherType::TpdfShaped;
    return e;
}

void ControlsPanel::setBusy(bool busy, double progress, const juce::String& what)
{
    progress_ = busy ? juce::jlimit(0.0, 1.0, progress) : 0.0;
    cancel_.setEnabled(busy);
    status_.setText(busy ? what : juce::String(), juce::dontSendNotification);
}

void ControlsPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    g.setColour(theme::line);
    g.drawVerticalLine(0, 0, float(getHeight()));
}

void ControlsPanel::resized()
{
    auto r = getLocalBounds().reduced(10, 8);
    auto row = [&](int h) { auto x = r.removeFromTop(h); r.removeFromTop(4); return x; };
    {
        auto c = row(28);
        const int w = c.getWidth() / 3;
        catVoice_.setBounds(c.removeFromLeft(w).reduced(1, 0));
        catSfx_.setBounds(c.removeFromLeft(w).reduced(1, 0));
        catMusic_.setBounds(c.reduced(1, 0));
    }
    {
        auto c = row(26);
        deletePreset_.setBounds(c.removeFromRight(56));
        c.removeFromRight(4);
        savePreset_.setBounds(c.removeFromRight(96));
        c.removeFromRight(4);
        presetBox_.setBounds(c);
    }
    presetDesc_.setBounds(row(46));
    {
        auto c = row(40);
        cancel_.setBounds(c.removeFromRight(70));
        c.removeFromRight(6);
        process_.setBounds(c);
    }
    progressBar_.setBounds(row(10));
    status_.setBounds(row(16));
    autoUpdate_.setBounds(row(20));
    r.removeFromTop(4);
    auto section = [&](juce::ToggleButton& t, std::initializer_list<std::pair<juce::Label*, juce::Component*>> items) {
        t.setBounds(row(22));
        for (auto& it : items)
        {
            auto c = row(22);
            it.first->setBounds(c.removeFromLeft(110));
            it.second->setBounds(c);
        }
        r.removeFromTop(4);
    };
    section(cleanupOn_, { { &cleanupL_, &cleanup_ } });
    section(toneOn_, { { &toneL_, &tone_ }, { &tiltL_, &tilt_ } });
    section(dynOn_, { { &dynL_, &dynamics_ } });
    section(outOn_, { { &modeL_, &mode_ }, { &targetL_, &target_ }, { &ceilingL_, &ceiling_ }, { &srL_, &sampleRate_ } });
    {
        auto c = row(22);
        optNeural_.setBounds(c.removeFromLeft(c.getWidth() / 2 + 20));
        optDereverb_.setBounds(c);
        auto d = row(22);
        optMultiband_.setBounds(d.removeFromLeft(d.getWidth() / 2 + 20));
        optSaturation_.setBounds(d);
    }
    r.removeFromTop(6);
    {
        auto c = row(24);
        const int w = c.getWidth() / 3;
        format_.setBounds(c.removeFromLeft(w).reduced(1, 0));
        bits_.setBounds(c.removeFromLeft(w).reduced(1, 0));
        dither_.setBounds(c.reduced(1, 0));
    }
    export_.setBounds(row(30));
    {
        auto c = row(26);
        processAll_.setBounds(c.removeFromLeft(c.getWidth() / 2).reduced(1, 0));
        exportAll_.setBounds(c.reduced(1, 0));
    }
    {
        auto c = row(22);
        consistency_.setBounds(c.removeFromLeft(140));
        preserve_.setBounds(c);
    }
    audio_.setBounds(row(24));
}
