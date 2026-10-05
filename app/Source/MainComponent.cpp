#include "MainComponent.h"
#include "Settings.h"

#include <ac/StemMixer.h>

namespace {
juce::String fmtDb(double v, const char* unit)
{
    return std::isfinite(v) && v > -150 ? juce::String(v, 2) + " " + unit : juce::String("-inf ") + unit;
}
} // namespace

MainComponent::MainComponent()
{
    setLookAndFeel(&laf_);
    juce::LookAndFeel::setDefaultLookAndFeel(&laf_);

    title_.setText("AudioFinisher", juce::dontSendNotification);
    title_.setFont(theme::font(20, true));
    title_.setColour(juce::Label::textColourId, theme::text);
    addAndMakeVisible(title_);
    for (auto* b : { &addFiles_, &removeFile_, &play_, &stop_, &abProc_, &abOrig_, &abRemoved_, &clearRegion_ }) addAndMakeVisible(*b);
    for (auto* b : { &abProc_, &abOrig_, &abRemoved_ })
    {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(201);
    }
    abProc_.setToggleState(true, juce::dontSendNotification);
    abProc_.setColour(juce::TextButton::buttonOnColourId, theme::accent.darker(0.3f));
    abOrig_.setColour(juce::TextButton::buttonOnColourId, theme::original.darker(0.3f));
    abRemoved_.setColour(juce::TextButton::buttonOnColourId, theme::removed.darker(0.3f));
    abProc_.onClick = [this] { if (abProc_.getToggleState()) setAB(ABPlayer::Processed); };
    abOrig_.onClick = [this] { if (abOrig_.getToggleState()) setAB(ABPlayer::Original); };
    abRemoved_.onClick = [this] { if (abRemoved_.getToggleState()) setAB(ABPlayer::Removed); };
    abProc_.setTooltip("Hear the processed result (key 1)");
    abOrig_.setTooltip("Hear the original, latency-aligned and loudness-matched (key 2)");
    abRemoved_.setTooltip("Hear only what the Cleanup section removed (key 3)");
    matched_.setToggleState(true, juce::dontSendNotification);
    matched_.onClick = [this] { player_.setMatched(matched_.getToggleState()); };
    loop_.onClick = [this] { player_.setLooping(loop_.getToggleState()); };
    addAndMakeVisible(matched_);
    addAndMakeVisible(loop_);
    play_.onClick = [this] {
        if (player_.isPlaying()) player_.stop();
        else player_.play();
    };
    stop_.onClick = [this] {
        player_.stop();
        player_.setPositionSeconds(0);
    };
    addFiles_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Add audio files", juce::File(), af::supportedWildcard());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::canSelectMultipleItems,
                              [this](const juce::FileChooser& fc) { addFiles(fc.getResults(), false); });
    };
    removeFile_.onClick = [this] {
        const int row = fileList_.getSelectedRow();
        if (row < 0 || row >= int(items_.size())) return;
        if (items_[size_t(row)] == current_)
        {
            player_.stop();
            player_.setSet(nullptr);
            current_.reset();
        }
        items_.erase(items_.begin() + row);
        fileList_.updateContent();
        refreshDetails();
    };
    clearRegion_.onClick = [this] {
        waveform_.setRegion(-1, -1);
        controls_.setNoiseRegion(std::nullopt);
        scheduleAutoRender();
    };

    fileList_.setRowHeight(38);
    fileList_.setColour(juce::ListBox::backgroundColourId, theme::panel);
    addAndMakeVisible(fileList_);
    addAndMakeVisible(waveform_);
    addAndMakeVisible(spectrum_);
    addAndMakeVisible(meters_);
    waveform_.onSeek = [this](double s) { player_.setPositionSeconds(s); };
    waveform_.onRegion = [this](double a, double b) {
        if (!current_ || !current_->audio) return;
        // Waveform time is the output timeline, aligned with the source.
        const double sr = current_->audio->sampleRate;
        controls_.setNoiseRegion(std::make_pair(size_t(a * sr), size_t(b * sr)));
        hint_.setText("Noise profile will be learned from the selected region (" + juce::String(a, 2) + "-" + juce::String(b, 2) + " s).",
                      juce::dontSendNotification);
        scheduleAutoRender();
    };

    for (auto* t : { &analysisText_, &logText_ })
    {
        t->setMultiLine(true);
        t->setReadOnly(true);
        t->setScrollbarsShown(true);
        t->setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        t->setColour(juce::TextEditor::textColourId, theme::text);
    }
    tabs_.addTab("Stages (advanced)", theme::panel, &stageTable_, false);
    tabs_.addTab("Analysis", theme::panel, &analysisText_, false);
    tabs_.addTab("Decision log", theme::panel, &logText_, false);
    tabs_.addTab("Stem mixer", theme::panel, &stemPanel_, false);
    tabs_.setTabBarDepth(28);
    addAndMakeVisible(tabs_);
    stageTable_.onOverride = [this](ac::StageId id, ac::Override o) { controls_.setOverride(id, o); };

    stemPanel_.onAddStems = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Add aligned stems", juce::File(), af::supportedWildcard());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::canSelectMultipleItems,
                              [this](const juce::FileChooser& fc) { addFiles(fc.getResults(), true); });
    };
    stemPanel_.onMix = [this] { runStemMix(); };

    // The controls panel scrolls vertically when the window is shorter than
    // its content (small screens, high display scaling).
    controlsView_.setViewedComponent(&controls_, false);
    controlsView_.setScrollBarsShown(true, false);
    controlsView_.setScrollBarThickness(10);
    addAndMakeVisible(controlsView_);
    controls_.onUiScale = [this](double s) {
        juce::Desktop::getInstance().setGlobalScaleFactor(float(s));
        if (auto* w = getTopLevelComponent()) settings::fitToScreen(*w, w->getWidth(), w->getHeight(), false);
    };
    controls_.onProcess = [this] { processSelected(); };
    controls_.onCancel = [this] { worker_.cancelAll(); };
    controls_.onExport = [this] { exportSelected(); };
    controls_.onProcessAll = [this] { processAll(false, {}); };
    controls_.onExportAll = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Export all processed files to folder");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File()) processAll(true, fc.getResult());
                              });
    };
    controls_.onControlsChanged = [this] { scheduleAutoRender(); };
    controls_.onCategoryChanged = [this] { scheduleAutoRender(); };
    controls_.onAudioSettings = [this] { showAudioSettings(); };
    controls_.onLoadReference = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Reference track to match", juce::File(), af::supportedWildcard());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  const auto f = fc.getResult();
                                  if (f.existsAsFile()) loadReference(f);
                              });
    };

    hint_.setFont(theme::font(12));
    hint_.setColour(juce::Label::textColourId, theme::dim);
    hint_.setText("Drop files, choose a category and preset, press PROCESS, audition A/B, export. Shift-drag on the waveform to "
                  "mark a noise-only region. Space = play/pause, 1/2/3 = processed/original/removed.",
                  juce::dontSendNotification);
    addAndMakeVisible(hint_);

    const auto err = deviceManager_.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) hint_.setText("Audio device unavailable (" + err + "): processing and export still work.", juce::dontSendNotification);
    sourcePlayer_.setSource(&player_);
    deviceManager_.addAudioCallback(&sourcePlayer_);

    setWantsKeyboardFocus(true);
    setSize(1500, 940);
    startTimerHz(30);
    refreshDetails();
}

MainComponent::~MainComponent()
{
    stopTimer();
    worker_.cancelAll();
    deviceManager_.removeAudioCallback(&sourcePlayer_);
    sourcePlayer_.setSource(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(theme::bg); }

void MainComponent::onMessageThread(std::function<void()> fn)
{
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this), fn = std::move(fn)] {
        if (safe) fn();
    });
}

void MainComponent::resized()
{
    auto r = getLocalBounds();

    // ---- Toolbar: one row when it fits, otherwise two rows (narrow windows /
    // high display scaling). Widths shrink proportionally if still too wide.
    struct Item { juce::Component* c; int w; int gapBefore; };
    const std::vector<Item> row1 = { { &title_, 160, 0 }, { &addFiles_, 100, 0 }, { &removeFile_, 70, 4 }, { &play_, 64, 16 }, { &stop_, 56, 4 } };
    const std::vector<Item> row2 = { { &abProc_, 96, 14 }, { &abOrig_, 86, 0 }, { &abRemoved_, 116, 0 }, { &matched_, 178, 10 },
                                     { &loop_, 62, 0 }, { &clearRegion_, 140, 4 } };
    auto widthOf = [](const std::vector<Item>& v) { int t = 0; for (auto& i : v) t += i.w + i.gapBefore; return t; };
    auto layoutRow = [](juce::Rectangle<int> area, const std::vector<Item>& v, int total) {
        const double k = std::min(1.0, double(area.getWidth()) / std::max(1, total));
        for (auto& i : v)
        {
            area.removeFromLeft(int(i.gapBefore * k));
            i.c->setBounds(area.removeFromLeft(int(i.w * k)));
        }
    };
    const int full = widthOf(row1) + widthOf(row2);
    if (full <= getWidth() - 16)
    {
        auto top = r.removeFromTop(44).reduced(8, 7);
        std::vector<Item> all(row1);
        all.insert(all.end(), row2.begin(), row2.end());
        layoutRow(top, all, full);
    }
    else
    {
        auto a = r.removeFromTop(38).reduced(8, 5);
        auto b = r.removeFromTop(36).reduced(8, 4);
        layoutRow(a, row1, widthOf(row1));
        auto r2 = row2;
        r2.front().gapBefore = 0;
        layoutRow(b, r2, widthOf(r2));
    }

    // ---- Side panels scale with the window; the controls panel scrolls.
    const int rightW = juce::jlimit(330, 380, int(getWidth() * 0.26));
    auto right = r.removeFromRight(rightW);
    controlsView_.setBounds(right);
    controls_.setBounds(0, 0, right.getWidth(), 2000); // lay out once to measure
    const int needH = controls_.contentHeight();
    const bool scroll = needH > right.getHeight();
    controls_.setSize(right.getWidth() - (scroll ? controlsView_.getScrollBarThickness() : 0), std::max(needH, right.getHeight()));

    const int leftW = juce::jlimit(150, 250, int(getWidth() * 0.16));
    auto left = r.removeFromLeft(leftW);
    fileList_.setBounds(left.reduced(6));
    hint_.setBounds(r.removeFromBottom(22).reduced(6, 0));
    auto centre = r.reduced(6);
    auto upper = centre.removeFromTop(int(centre.getHeight() * 0.52));
    auto meterArea = upper.removeFromRight(juce::jlimit(170, 250, int(upper.getWidth() * 0.28)));
    waveform_.setBounds(upper.removeFromTop(int(upper.getHeight() * 0.58)));
    upper.removeFromTop(4);
    spectrum_.setBounds(upper);
    meters_.setBounds(meterArea.withTrimmedLeft(6));
    centre.removeFromTop(6);
    tabs_.setBounds(centre);
}

bool MainComponent::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)
    {
        play_.triggerClick();
        return true;
    }
    if (k.getTextCharacter() == '1') { abProc_.setToggleState(true, juce::sendNotification); return true; }
    if (k.getTextCharacter() == '2') { abOrig_.setToggleState(true, juce::sendNotification); return true; }
    if (k.getTextCharacter() == '3') { abRemoved_.setToggleState(true, juce::sendNotification); return true; }
    return false;
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files)
        if (af::isSupportedExtension(f.toStdString())) return true;
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> fs;
    for (auto& f : files) fs.add(juce::File(f));
    const bool stems = tabs_.getCurrentContentComponent() == &stemPanel_;
    addFiles(fs, stems);
}

void MainComponent::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= int(items_.size())) return;
    const auto& it = *items_[size_t(row)];
    g.fillAll(selected ? theme::accent.withAlpha(0.25f) : theme::panel);
    g.setColour(theme::text);
    g.setFont(theme::font(13, selected));
    g.drawText(it.file.getFileName(), 8, 2, w - 10, h / 2, juce::Justification::centredLeft, true);
    g.setColour(it.result ? theme::good : theme::dim);
    g.setFont(theme::font(11));
    juce::String sub = it.status;
    if (it.audio) sub = juce::String(it.audio->numChannels()) + " ch " + juce::String(it.audio->sampleRate / 1000.0, 1) + " kHz  " + sub;
    g.drawText(sub, 8, h / 2, w - 10, h / 2 - 2, juce::Justification::centredLeft, true);
}

void MainComponent::selectedRowsChanged(int row)
{
    if (row >= 0 && row < int(items_.size())) selectItem(items_[size_t(row)]);
}

void MainComponent::addFiles(const juce::Array<juce::File>& files, bool asStems)
{
    for (const auto& f : files)
    {
        if (!f.existsAsFile()) continue;
        worker_.post("Loading " + f.getFileName(), [this, f, asStems](const ac::Job& job) {
            auto buf = std::make_shared<ac::AudioBuffer>();
            std::string err;
            af::SourceInfo info;
            if (!af::loadAudio(f.getFullPathName().toStdString(), *buf, err, &info))
            {
                juce::MessageManager::callAsync([err] {
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot load", err);
                });
                return;
            }
            if (asStems)
            {
                onMessageThread([this, f, buf] { stemPanel_.addStem(f, buf); });
                return;
            }
            auto item = std::make_shared<SessionItem>();
            item->file = f;
            item->audio = buf;
            item->info = info;
            item->display = buildDisplayOriginal(*buf);
            const double rate = player_.deviceRate();
            item->playback = buildPlaybackOriginal(*buf, rate, job);
            item->playbackDeviceRate = rate;
            item->status = juce::String(info.bitsPerSample) + (info.floatingPoint ? "-bit float" : "-bit") + ", not processed";
            onMessageThread([this, item] {
                items_.push_back(item);
                fileList_.updateContent();
                if (!current_)
                {
                    fileList_.selectRow(int(items_.size()) - 1);
                    selectItem(item);
                }
                fileList_.repaint();
            });
        });
    }
}

void MainComponent::loadReference(const juce::File& f)
{
    worker_.post("Analysing reference " + f.getFileName(), [this, f](const ac::Job&) {
        ac::AudioBuffer buf;
        std::string err;
        if (!af::loadAudio(f.getFullPathName().toStdString(), buf, err))
        {
            onMessageThread([err] { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Reference", err); });
            return;
        }
        auto prof = std::make_shared<const ac::ReferenceProfile>(ac::analyzeReference(buf, f.getFileName().toStdString()));
        onMessageThread([this, prof] {
            controls_.setReference(prof);
            resized();
            scheduleAutoRender();
            hint_.setText("Reference loaded: tonal balance, loudness, width and loudness range will be matched (bounded). "
                          "Press PROCESS if auto re-render is off.",
                          juce::dontSendNotification);
        });
    });
}

void MainComponent::selectItem(std::shared_ptr<SessionItem> item)
{
    current_ = std::move(item);
    player_.stop();
    if (current_)
    {
        ensurePlayback(current_);
        player_.setSet(current_->playback);
        waveform_.setData(current_->display);
        waveform_.setShowProcessed(current_->result != nullptr);
        spectrum_.setData(current_->display);
        if (current_->analysis) controls_.setCategory(current_->analysisCategory, false);
    }
    else
    {
        player_.setSet(nullptr);
        waveform_.setData(nullptr);
        spectrum_.setData(nullptr);
    }
    waveform_.setRegion(-1, -1);
    controls_.setNoiseRegion(std::nullopt);
    refreshDetails();
}

void MainComponent::ensurePlayback(std::shared_ptr<SessionItem> item)
{
    const double rate = player_.deviceRate();
    if (!item || std::abs(item->playbackDeviceRate - rate) < 0.5) return;
    item->playbackDeviceRate = rate; // claim: avoid queuing duplicates
    auto result = item->result;
    auto audio = item->audio;
    worker_.post("Preparing monitor", [this, item, rate, result, audio](const ac::Job& job) {
        std::shared_ptr<PlaybackSet> p = result ? buildPlayback(*result, rate, job) : buildPlaybackOriginal(*audio, rate, job);
        onMessageThread([this, item, p, rate] {
            item->playback = p;
            item->playbackDeviceRate = rate;
            if (item == current_) player_.setSet(p);
        });
    });
}

void MainComponent::processItem(std::shared_ptr<SessionItem> item, std::optional<double> targetOverride,
                                std::function<void(std::shared_ptr<SessionItem>)> then)
{
    if (!item || !item->audio || !controls_.preset()) return;
    const ac::PresetDef presetCopy = *controls_.preset();
    ac::UserControls uc = controls_.controls();
    if (targetOverride) uc.targetLufs = *targetOverride;
    if (item != current_) uc.noiseRegion.reset();
    const ac::Category cat = controls_.category();
    item->status = "processing...";
    fileList_.repaint();
    // Snapshot on the message thread: the task never reads mutable item state.
    auto cachedAnalysis = item->analysisCategory == cat ? item->analysis : nullptr;
    auto audio = item->audio;
    worker_.post("Processing " + item->file.getFileName(), [this, item, audio, cachedAnalysis, presetCopy, uc, cat, then](const ac::Job& job) {
        auto analysis = cachedAnalysis;
        if (!analysis)
        {
            ac::Job aj = job;
            aj.progress = [&job](double f, const std::string& w) { job.report(0.25 * f, "Analysing: " + w); };
            analysis = std::make_shared<const ac::AnalysisReport>(ac::analyze(*audio, cat, aj));
        }
        ac::ProcessRequest rq;
        rq.input = audio.get();
        rq.preset = &presetCopy;
        rq.category = cat;
        rq.controls = uc;
        rq.analysis = analysis.get();
        ac::Job pj = job;
        pj.progress = [&job](double f, const std::string& w) { job.report(0.25 + 0.65 * f, w); };
        auto result = std::make_shared<ac::ProcessResult>(ac::process(rq, pj));
        result->plan.preset = nullptr; // presetCopy dies with this task
        job.report(0.92, "Preparing display");
        auto display = buildDisplay(*result, *audio);
        const double rate = player_.deviceRate();
        auto pb = buildPlayback(*result, rate, job);
        onMessageThread([this, item, analysis, result, display, pb, rate, cat, name = presetCopy.name, then] {
            item->analysis = analysis;
            item->analysisCategory = cat;
            item->result = result;
            item->display = display;
            item->playback = pb;
            item->playbackDeviceRate = rate;
            item->presetUsed = name;
            item->status = juce::String(name) + ": " + fmtDb(result->outputStats.integrated, "LUFS") + ", " + fmtDb(result->outputStats.truePeakDb, "dBTP");
            fileList_.repaint();
            if (item == current_)
            {
                const double pos = player_.positionSeconds();
                const bool wasPlaying = player_.isPlaying();
                player_.setSet(pb);
                player_.setPositionSeconds(pos);
                if (wasPlaying) player_.play();
                waveform_.setData(display);
                waveform_.setShowProcessed(true);
                spectrum_.setData(display);
                refreshDetails();
            }
            if (then) then(item);
        });
    });
}

void MainComponent::processSelected()
{
    if (!current_)
    {
        hint_.setText("Add a file first (drag & drop or Add files...).", juce::dontSendNotification);
        return;
    }
    if (current_ == stemItem_)
    {
        runStemMix();
        return;
    }
    worker_.cancelAll();
    processItem(current_);
}

void MainComponent::scheduleAutoRender()
{
    if (controls_.autoUpdate() && current_ && current_->result) autoRenderCountdown_ = 12; // ~400 ms debounce
}

juce::File MainComponent::defaultExportFile(const SessionItem& it, const af::ExportOptions& ex) const
{
    const auto ext = af::extensionFor(ex.format);
    const auto tag = juce::File::createLegalFileName(it.presetUsed).replaceCharacter(' ', '_').replaceCharacter('/', '-');
    return it.file.getSiblingFile(it.file.getFileNameWithoutExtension() + "_" + tag + ext);
}

void MainComponent::exportSelected()
{
    if (!current_ || !current_->result)
    {
        hint_.setText("Process the file before exporting.", juce::dontSendNotification);
        return;
    }
    const auto ex = controls_.exportOptions();
    chooser_ = std::make_unique<juce::FileChooser>("Export processed audio", defaultExportFile(*current_, ex),
                                                   juce::String("*") + af::extensionFor(ex.format));
    auto item = current_;
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this, item, ex](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (f == item->file)
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export",
                                                                         "Originals are never overwritten. Choose another file name.");
                                  return;
                              }
                              auto result = item->result;
                              worker_.post("Exporting " + f.getFileName(), [this, f, result, ex](const ac::Job&) {
                                  std::string err;
                                  const bool ok = af::saveAudio(f.getFullPathName().toStdString(), result->output, ex, err);
                                  onMessageThread([this, ok, err, f] {
                                      hint_.setText(ok ? "Exported " + f.getFullPathName() : "Export failed: " + juce::String(err),
                                                    juce::dontSendNotification);
                                  });
                              });
                          });
}

void MainComponent::processAll(bool exportToo, juce::File folder)
{
    if (items_.empty()) return;
    const auto ex = controls_.exportOptions();
    std::vector<std::optional<double>> targets(items_.size());
    if (controls_.batchConsistency() && controls_.preset())
    {
        // Measure each asset with the delivery metric, then pull toward the
        // group target while keeping a fraction of their differences.
        const auto uc = controls_.controls();
        const auto mode = uc.loudnessMode.value_or(controls_.preset()->loudnessMode);
        std::vector<double> metrics;
        for (auto& it : items_) metrics.push_back(ac::loudnessMetric(*it->audio, mode));
        const auto t = ac::batchConsistencyTargets(metrics, uc.targetLufs.value_or(controls_.preset()->targetLufs), controls_.batchPreserve());
        for (size_t i = 0; i < t.size(); ++i) targets[i] = t[i];
    }
    for (size_t i = 0; i < items_.size(); ++i)
    {
        auto item = items_[i];
        processItem(item, targets[i], [this, exportToo, folder, ex](std::shared_ptr<SessionItem> it) {
            if (!exportToo || !it->result) return;
            const auto dst = folder.getChildFile(defaultExportFile(*it, ex).getFileName());
            auto result = it->result;
            worker_.post("Exporting " + dst.getFileName(), [this, dst, result, ex](const ac::Job&) {
                std::string err;
                const bool ok = af::saveAudio(dst.getFullPathName().toStdString(), result->output, ex, err);
                onMessageThread([this, ok, err, dst] {
                    hint_.setText(ok ? "Exported " + dst.getFullPathName() : "Export failed: " + juce::String(err), juce::dontSendNotification);
                });
            });
        });
    }
}

void MainComponent::runStemMix()
{
    auto& stems = stemPanel_.stems();
    if (stems.empty())
    {
        hint_.setText("Add aligned stems in the Stem mixer tab first.", juce::dontSendNotification);
        return;
    }
    ac::StemMixRequest rq;
    for (auto& s : stems) rq.stems.push_back({ s.audio.get(), s.file.getFileNameWithoutExtension().toStdString(), s.role, s.gainDb, s.pan, s.bus, s.mute, s.process });
    rq.buses = stemPanel_.buses();
    const ac::PresetDef master = *stemPanel_.masterPreset();
    rq.masterControls = controls_.controls();
    rq.masterControls.noiseRegion.reset();
    if (controls_.category() != ac::Category::Music)
    {
        // The right-hand Output controls belong to a non-music preset: deliver
        // with the master preset's own loudness target and ceiling instead.
        rq.masterControls.loudnessMode.reset();
        rq.masterControls.targetLufs.reset();
        rq.masterControls.ceilingDbTP.reset();
        rq.masterControls.overrides.clear();
    }
    // Keep the stem buffers alive for the task.
    std::vector<std::shared_ptr<const ac::AudioBuffer>> keep;
    for (auto& s : stems) keep.push_back(s.audio);
    worker_.cancelAll();
    worker_.post("Stem mix", [this, rq, master, keep](const ac::Job& job) mutable {
        rq.masterPreset = &master;
        auto r = ac::mixStems(rq, job);
        auto result = std::make_shared<ac::ProcessResult>(std::move(r.master));
        result->plan.preset = nullptr;
        auto display = buildDisplay(*result, result->reference);
        const double rate = player_.deviceRate();
        auto pb = buildPlayback(*result, rate, job);
        auto mixAudio = std::make_shared<ac::AudioBuffer>(result->reference);
        onMessageThread([this, result, display, pb, rate, mixAudio, name = master.name] {
            if (!stemItem_)
            {
                stemItem_ = std::make_shared<SessionItem>();
                stemItem_->file = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Stem mix.wav");
                items_.push_back(stemItem_);
            }
            stemItem_->audio = mixAudio;
            stemItem_->result = result;
            stemItem_->display = display;
            stemItem_->playback = pb;
            stemItem_->playbackDeviceRate = rate;
            stemItem_->presetUsed = "Stem mix - " + name;
            stemItem_->status = juce::String("mixed + mastered: ") + fmtDb(result->outputStats.integrated, "LUFS");
            fileList_.updateContent();
            fileList_.selectRow(int(std::find(items_.begin(), items_.end(), stemItem_) - items_.begin()));
            selectItem(stemItem_);
        });
    });
}

void MainComponent::setAB(ABPlayer::Source s)
{
    player_.setSource(s);
    waveform_.setShowProcessed(s != ABPlayer::Original);
}

void MainComponent::showAudioSettings()
{
    auto* sel = new juce::AudioDeviceSelectorComponent(deviceManager_, 0, 0, 2, 2, false, false, true, false);
    sel->setSize(520, 380);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(sel);
    o.dialogTitle = "Audio device";
    o.dialogBackgroundColour = theme::panel;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void MainComponent::refreshDetails()
{
    const ac::Plan* plan = current_ && current_->result ? &current_->result->plan : nullptr;
    stageTable_.setPlan(plan, controls_.overrides());
    juce::String a, l;
    if (current_ && current_->analysis)
    {
        a << "File: " << current_->file.getFullPathName() << "\n";
        a << "Category: " << ac::categoryName(current_->analysisCategory) << "\n\n";
        a << ac::describe(*current_->analysis);
    }
    else if (current_ && current_->audio)
        a << current_->file.getFullPathName() << "\nNot analysed yet: press PROCESS.";
    if (current_ && current_->result)
    {
        const auto& r = *current_->result;
        a << "\n--- Before / after (loudness-matched A/B gain " << juce::String(r.matchGainDb, 2) << " dB) ---\n";
        a << "Original : " << fmtDb(r.referenceStats.integrated, "LUFS") << ", LRA " << juce::String(r.referenceStats.lra, 1) << " LU, TP "
          << fmtDb(r.referenceStats.truePeakDb, "dBTP") << ", max M " << fmtDb(r.referenceStats.maxMomentary, "LUFS") << "\n";
        a << "Processed: " << fmtDb(r.outputStats.integrated, "LUFS") << ", LRA " << juce::String(r.outputStats.lra, 1) << " LU, TP "
          << fmtDb(r.outputStats.truePeakDb, "dBTP") << ", max M " << fmtDb(r.outputStats.maxMomentary, "LUFS") << "\n";
        a << "Output: " << juce::String(r.output.sampleRate, 0) << " Hz, " << r.output.numChannels() << " ch, "
          << juce::String(r.output.durationSeconds(), 3) << " s\n";
        if (!r.targetReached) a << "NOTE: delivery target not reached within the preset's limiting bound (see Loudness stage).\n";
        for (auto& line : r.plan.log) l << line << "\n";
        l << "\n--- Stage timings ---\n";
        for (auto& m : r.metrics) l << ac::stageName(m.id) << ": " << juce::String(m.milliseconds, 0) << " ms, max GR " << juce::String(m.maxGrDb, 1) << " dB\n";
    }
    analysisText_.setText(a, false);
    logText_.setText(l, false);
}

void MainComponent::updateMeters()
{
    MeterView::Values v;
    const auto* p = controls_.preset();
    const auto uc = controls_.controls();
    v.target = float(uc.targetLufs.value_or(p ? p->targetLufs : -16.0));
    v.ceiling = float(uc.ceilingDbTP.value_or(p ? p->ceilingDbTP : -1.0));
    if (current_ && current_->result)
    {
        // Markers show what this render was actually delivered against.
        v.target = float(current_->result->plan.targetLufs);
        v.ceiling = float(current_->result->plan.ceilingDbTP);
    }
    const bool proc = player_.source() != ABPlayer::Original && current_ && current_->result;
    v.label = proc ? "Meters: processed" : "Meters: original (file level)";
    if (current_ && current_->display)
    {
        const auto& d = *current_->display;
        const double t = player_.positionSeconds();
        const auto& curve = proc ? d.outCurve : d.refCurve;
        const auto& tp = proc ? d.outTp : d.refTp;
        const size_t k = size_t(t / 0.1);
        if (!curve.momentary.empty())
        {
            const size_t kk = std::min(k, curve.momentary.size() - 1);
            v.momentary = std::max(-70.0f, curve.momentary[kk]);
            v.shortTerm = std::max(-70.0f, curve.shortTerm[kk]);
        }
        float mx = -70;
        for (size_t i = 0; i <= std::min(k, tp.size() ? tp.size() - 1 : 0) && i < tp.size(); ++i) mx = std::max(mx, tp[i]);
        v.truePeak = mx;
        if (current_->result)
        {
            const auto& st = proc ? current_->result->outputStats : current_->result->referenceStats;
            v.integrated = std::isfinite(st.integrated) ? float(st.integrated) : -70.0f;
            if (proc && !d.grTrace.empty()) v.gr = d.grTrace[std::min(d.grTrace.size() - 1, size_t(t / 0.01))];
        }
        else if (current_->analysis && current_->analysis->loudness.integratedValid())
            v.integrated = float(current_->analysis->loudness.integrated);
    }
    meters_.setValues(v);
}

void MainComponent::handleCommandLine(const juce::StringArray& args)
{
    juce::Array<juce::File> files;
    for (int i = 0; i < args.size(); ++i)
    {
        const auto a = args[i].unquoted();
        if (a == "--category" && i + 1 < args.size())
        {
            const auto c = args[++i];
            controls_.setCategory(c == "music" ? ac::Category::Music : c == "sfx" ? ac::Category::SoundEffect : ac::Category::Voice, false);
        }
        else if (a == "--preset" && i + 1 < args.size()) script_.preset = args[++i];
        else if (a == "--process") script_.process = true;
        else if (a == "--ab" && i + 1 < args.size()) script_.ab = args[++i];
        else if (a == "--screenshot" && i + 1 < args.size()) script_.screenshot = juce::File::getCurrentWorkingDirectory().getChildFile(args[++i]);
        else if (a == "--quit") script_.quit = true;
        else if (a == "--stem" && i + 1 < args.size())
        {
            juce::Array<juce::File> st;
            st.add(juce::File::getCurrentWorkingDirectory().getChildFile(args[++i]));
            addFiles(st, true);
            ++script_.stemsExpected;
            tabs_.setCurrentTabIndex(3);
        }
        else if (a == "--mix") script_.mix = true;
        else if (a == "--match" && i + 1 < args.size()) loadReference(juce::File::getCurrentWorkingDirectory().getChildFile(args[++i]));
        else if (a == "--tab" && i + 1 < args.size()) tabs_.setCurrentTabIndex(args[++i].getIntValue());
        else if (a == "--scale" && i + 1 < args.size()) ++i; // applied at startup (Main.cpp)
        else if (a == "--window" && i + 1 < args.size())
        {
            const auto wh = juce::StringArray::fromTokens(args[++i], "x", "");
            if (wh.size() == 2)
                if (auto* w = getTopLevelComponent()) w->setSize(wh[0].getIntValue(), wh[1].getIntValue());
        }
        else if (!a.startsWith("--") && juce::File::getCurrentWorkingDirectory().getChildFile(a).existsAsFile())
            files.add(juce::File::getCurrentWorkingDirectory().getChildFile(a));
    }
    if (script_.preset.isNotEmpty()) controls_.selectPreset(script_.preset.toStdString());
    if (!files.isEmpty()) addFiles(files, false);
    script_.active = script_.process || script_.mix || script_.screenshot != juce::File() || script_.quit;
}

void MainComponent::runScript()
{
    if (!script_.active) return;
    ++script_.ticks;
    if (script_.mix && !script_.processPosted && int(stemPanel_.stems().size()) >= script_.stemsExpected && !worker_.busy())
    {
        script_.processPosted = true;
        script_.process = true; // wait for the mix result
        runStemMix();
        return;
    }
    if (script_.mix && !script_.processPosted) return;
    if (script_.process && !script_.processPosted && current_ && !worker_.busy())
    {
        script_.processPosted = true;
        processSelected();
        return;
    }
    const bool done = !worker_.busy() && current_ && (!script_.process || current_->result) && current_->playbackDeviceRate > 0;
    if (!done && script_.ticks < 30 * 300) return;
    if (script_.ab == "original") abOrig_.setToggleState(true, juce::sendNotification);
    if (script_.ab == "removed") abRemoved_.setToggleState(true, juce::sendNotification);
    if (++script_.settle < 15) return; // let views repaint
    if (script_.screenshot != juce::File())
    {
        auto img = getTopLevelComponent()->createComponentSnapshot(getTopLevelComponent()->getLocalBounds(), true, 1.0f);
        script_.screenshot.deleteFile();
        juce::FileOutputStream os(script_.screenshot);
        juce::PNGImageFormat png;
        png.writeImageToStream(img, os);
    }
    script_.active = false;
    if (script_.quit) juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainComponent::timerCallback()
{
    runScript();
    controls_.setBusy(worker_.busy(), worker_.progress(), worker_.what());
    play_.setButtonText(player_.isPlaying() ? "Pause" : "Play");
    waveform_.setPlayhead(player_.positionSeconds());
    updateMeters();
    const int n = player_.readScope(scopeBuf_.data(), int(scopeBuf_.size()));
    if (n > 0) spectrum_.push(scopeBuf_.data(), n, player_.deviceRate());
    if (current_) ensurePlayback(current_);
    if (autoRenderCountdown_ > 0 && --autoRenderCountdown_ == 0)
    {
        autoRenderCountdown_ = -1;
        processSelected();
    }
}
