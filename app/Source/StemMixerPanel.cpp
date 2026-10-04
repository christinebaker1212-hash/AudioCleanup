#include "StemMixerPanel.h"

class StemMixerPanel::Row : public juce::Component
{
public:
    Row(StemMixerPanel& owner, size_t index) : owner_(owner), idx_(index)
    {
        auto& e = owner_.stems_[idx_];
        name_.setText(e.file.getFileNameWithoutExtension() + "  (" + juce::String(e.audio->numChannels()) + " ch, " +
                          juce::String(e.audio->durationSeconds(), 1) + " s)",
                      juce::dontSendNotification);
        name_.setFont(theme::font(12));
        addAndMakeVisible(name_);
        for (int r = 0; r < int(ac::StemRole::Count); ++r) role_.addItem(ac::stemRoleName(ac::StemRole(r)), r + 1);
        role_.setSelectedId(int(e.role) + 1, juce::dontSendNotification);
        role_.onChange = [this] {
            auto& s = owner_.stems_[idx_];
            s.role = ac::StemRole(role_.getSelectedId() - 1);
            s.bus = ac::defaultBusFor(s.role);
            bus_.setSelectedId(s.bus + 1, juce::dontSendNotification);
        };
        addAndMakeVisible(role_);
        gain_.setSliderStyle(juce::Slider::LinearHorizontal);
        gain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
        gain_.setRange(-24, 12, 0.1);
        gain_.setValue(e.gainDb, juce::dontSendNotification);
        gain_.setTextValueSuffix(" dB");
        gain_.onValueChange = [this] { owner_.stems_[idx_].gainDb = gain_.getValue(); };
        addAndMakeVisible(gain_);
        pan_.setSliderStyle(juce::Slider::LinearHorizontal);
        pan_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
        pan_.setRange(-100, 100, 1);
        pan_.setValue(e.pan * 100, juce::dontSendNotification);
        pan_.onValueChange = [this] { owner_.stems_[idx_].pan = pan_.getValue() / 100.0; };
        addAndMakeVisible(pan_);
        bus_.addItemList({ "Vocals bus", "Music bus", "FX/Amb bus" }, 1);
        bus_.setSelectedId(e.bus + 1, juce::dontSendNotification);
        bus_.onChange = [this] { owner_.stems_[idx_].bus = bus_.getSelectedId() - 1; };
        addAndMakeVisible(bus_);
        mute_.setToggleState(e.mute, juce::dontSendNotification);
        mute_.onClick = [this] { owner_.stems_[idx_].mute = mute_.getToggleState(); };
        addAndMakeVisible(mute_);
        proc_.setToggleState(e.process, juce::dontSendNotification);
        proc_.onClick = [this] { owner_.stems_[idx_].process = proc_.getToggleState(); };
        addAndMakeVisible(proc_);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(4, 3);
        name_.setBounds(r.removeFromLeft(240));
        role_.setBounds(r.removeFromLeft(130).reduced(2, 0));
        gain_.setBounds(r.removeFromLeft(190).reduced(2, 0));
        pan_.setBounds(r.removeFromLeft(150).reduced(2, 0));
        bus_.setBounds(r.removeFromLeft(120).reduced(2, 0));
        mute_.setBounds(r.removeFromLeft(64));
        proc_.setBounds(r.removeFromLeft(90));
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(idx_ % 2 ? theme::panel : theme::panel2);
        g.fillRect(getLocalBounds());
    }

private:
    StemMixerPanel& owner_;
    size_t idx_;
    juce::Label name_;
    juce::ComboBox role_, bus_;
    juce::Slider gain_, pan_;
    juce::ToggleButton mute_{ "Mute" }, proc_{ "Treat" };
};

StemMixerPanel::StemMixerPanel()
{
    addAndMakeVisible(add_);
    addAndMakeVisible(clear_);
    addAndMakeVisible(mix_);
    mix_.setColour(juce::TextButton::buttonColourId, theme::accent.darker(0.2f));
    add_.onClick = [this] { if (onAddStems) onAddStems(); };
    clear_.onClick = [this] { stems_.clear(); rebuild(); };
    mix_.onClick = [this] { if (onMix) onMix(); };
    masterL_.setText("Master preset", juce::dontSendNotification);
    addAndMakeVisible(masterL_);
    int id = 1;
    for (auto* p : ac::presetsFor(ac::Category::Music)) master_.addItem(p->name, id++);
    master_.setSelectedId(1);
    addAndMakeVisible(master_);
    const char* names[3] = { "Vocals bus", "Music bus", "FX/Amb bus" };
    for (int b = 0; b < 3; ++b)
    {
        busL_[b].setText(names[b], juce::dontSendNotification);
        addAndMakeVisible(busL_[b]);
        busGain_[b].setSliderStyle(juce::Slider::LinearHorizontal);
        busGain_[b].setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
        busGain_[b].setRange(-24, 12, 0.1);
        busGain_[b].setValue(0);
        busGain_[b].setTextValueSuffix(" dB");
        addAndMakeVisible(busGain_[b]);
        busGlue_[b].setButtonText("Glue comp");
        busGlue_[b].setToggleState(b < 2, juce::dontSendNotification);
        addAndMakeVisible(busGlue_[b]);
    }
    info_.setText("Stems must be time-aligned (common start). Each stem gets role-aware cleanup/tone/dynamics; buses get measured "
                  "glue compression; the master chain uses the selected Music preset and the Output controls on the right. "
                  "A/B compares against the static (unprocessed) mix at matched loudness.",
                  juce::dontSendNotification);
    info_.setFont(theme::font(12));
    info_.setColour(juce::Label::textColourId, theme::dim);
    addAndMakeVisible(info_);
    viewport_.setViewedComponent(&rowHolder_, false);
    addAndMakeVisible(viewport_);
}

StemMixerPanel::~StemMixerPanel() { rows_.clear(); }

void StemMixerPanel::addStem(const juce::File& f, std::shared_ptr<const ac::AudioBuffer> audio)
{
    StemEntry e;
    e.file = f;
    e.audio = std::move(audio);
    e.role = ac::guessRole(f.getFileNameWithoutExtension().toStdString());
    e.bus = ac::defaultBusFor(e.role);
    stems_.push_back(e);
    rebuild();
}

void StemMixerPanel::rebuild()
{
    rows_.clear();
    for (size_t i = 0; i < stems_.size(); ++i)
    {
        auto* r = rows_.add(new Row(*this, i));
        rowHolder_.addAndMakeVisible(r);
    }
    resized();
}

std::vector<ac::BusSpec> StemMixerPanel::buses() const
{
    auto b = ac::defaultBuses();
    for (int i = 0; i < 3; ++i)
    {
        b[size_t(i)].gainDb = busGain_[i].getValue();
        b[size_t(i)].glue = busGlue_[i].getToggleState();
    }
    return b;
}

const ac::PresetDef* StemMixerPanel::masterPreset() const
{
    const auto list = ac::presetsFor(ac::Category::Music);
    const int i = master_.getSelectedItemIndex();
    return i >= 0 && i < int(list.size()) ? list[size_t(i)] : list.front();
}

void StemMixerPanel::paint(juce::Graphics& g) { g.fillAll(theme::panel); }

void StemMixerPanel::resized()
{
    auto r = getLocalBounds().reduced(8);
    auto top = r.removeFromTop(28);
    add_.setBounds(top.removeFromLeft(170));
    top.removeFromLeft(6);
    clear_.setBounds(top.removeFromLeft(70));
    top.removeFromLeft(16);
    masterL_.setBounds(top.removeFromLeft(100));
    master_.setBounds(top.removeFromLeft(200));
    top.removeFromLeft(16);
    mix_.setBounds(top.removeFromLeft(160));
    r.removeFromTop(6);
    auto busRow = r.removeFromTop(24);
    for (int b = 0; b < 3; ++b)
    {
        auto c = busRow.removeFromLeft(busRow.getWidth() / (3 - b));
        busL_[b].setBounds(c.removeFromLeft(80));
        busGlue_[b].setBounds(c.removeFromRight(90));
        busGain_[b].setBounds(c.reduced(2, 0));
    }
    r.removeFromTop(4);
    info_.setBounds(r.removeFromTop(34));
    viewport_.setBounds(r);
    const int rowH = 30;
    rowHolder_.setBounds(0, 0, std::max(1000, r.getWidth() - 12), std::max(rowH, int(rows_.size()) * rowH));
    for (int i = 0; i < rows_.size(); ++i) rows_[i]->setBounds(0, i * rowH, rowHolder_.getWidth(), rowH);
}
