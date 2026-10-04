#include "StageTable.h"

namespace {
enum Col { ColOverride = 1, ColState, ColSection, ColStage, ColReason, ColSettings };

class OverrideBox : public juce::ComboBox
{
public:
    OverrideBox()
    {
        addItem("Auto", 1);
        addItem("On", 2);
        addItem("Bypass", 3);
    }
    ac::StageId id{};
};
} // namespace

StageTable::StageTable()
{
    auto& h = table_.getHeader();
    h.addColumn("Mode", ColOverride, 78, 70, 90);
    h.addColumn("", ColState, 26, 26, 26);
    h.addColumn("Section", ColSection, 70, 50, 90);
    h.addColumn("Stage", ColStage, 150, 100, 220);
    h.addColumn("Decision / reason", ColReason, 430, 200, 2000);
    h.addColumn("Actual DSP settings", ColSettings, 520, 200, 3000);
    table_.setRowHeight(40);
    table_.setColour(juce::ListBox::backgroundColourId, theme::panel);
    addAndMakeVisible(table_);
}

void StageTable::setPlan(const ac::Plan* plan, const std::map<ac::StageId, ac::Override>& overrides)
{
    overrides_ = overrides;
    rows_.clear();
    for (int i = 0; i < int(ac::StageId::Count); ++i)
    {
        const auto id = ac::StageId(i);
        Row r{ id, false, true, false, ac::sectionName(ac::stageSection(id)), ac::stageName(id), "not processed yet", "" };
        if (plan && size_t(i) < plan->stages.size())
        {
            const auto& sp = plan->stages[size_t(i)];
            r.enabled = sp.enabled;
            r.available = sp.available;
            r.forced = sp.forced;
            r.reason = sp.reason;
            juce::StringArray kv;
            for (auto& p : sp.params) kv.add(juce::String(p.first) + ": " + juce::String(p.second));
            r.settings = kv.joinIntoString("  |  ");
        }
        rows_.push_back(r);
    }
    table_.updateContent();
    table_.repaint();
}

void StageTable::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? theme::accent.withAlpha(0.15f) : (row % 2 ? theme::panel : theme::panel2.withAlpha(0.6f)));
}

void StageTable::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool)
{
    if (row < 0 || row >= int(rows_.size())) return;
    const auto& r = rows_[size_t(row)];
    g.setFont(theme::font(12));
    switch (col)
    {
        case ColState:
        {
            const juce::Colour c = !r.available ? theme::line : r.enabled ? theme::good : theme::dim.withAlpha(0.5f);
            g.setColour(c);
            g.fillEllipse(juce::Rectangle<float>(7, float(h) / 2 - 6, 12, 12));
            break;
        }
        case ColSection:
            g.setColour(theme::dim);
            g.drawText(r.section, 4, 0, w - 4, h, juce::Justification::centredLeft);
            break;
        case ColStage:
            g.setColour(r.enabled ? theme::text : theme::dim);
            g.setFont(theme::font(13, r.enabled));
            g.drawText(r.name, 4, 0, w - 4, h, juce::Justification::centredLeft);
            break;
        case ColReason:
            g.setColour(r.forced ? theme::warn : theme::text.withAlpha(r.enabled ? 1.0f : 0.6f));
            g.drawFittedText(r.reason, 4, 2, w - 6, h - 4, juce::Justification::centredLeft, 3, 0.85f);
            break;
        case ColSettings:
            g.setColour(r.enabled ? theme::accent.brighter(0.4f) : theme::dim.withAlpha(0.6f));
            g.drawFittedText(r.settings, 4, 2, w - 6, h - 4, juce::Justification::centredLeft, 3, 0.85f);
            break;
        default: break;
    }
}

juce::Component* StageTable::refreshComponentForCell(int row, int col, bool, juce::Component* existing)
{
    if (col != ColOverride)
    {
        delete existing;
        return nullptr;
    }
    auto* box = dynamic_cast<OverrideBox*>(existing);
    if (!box) box = new OverrideBox();
    if (row < 0 || row >= int(rows_.size())) return box;
    box->id = rows_[size_t(row)].id;
    auto it = overrides_.find(box->id);
    const auto ov = it == overrides_.end() ? ac::Override::Auto : it->second;
    box->setSelectedId(ov == ac::Override::Auto ? 1 : ov == ac::Override::On ? 2 : 3, juce::dontSendNotification);
    box->setEnabled(rows_[size_t(row)].available);
    box->onChange = [this, box] {
        const int s = box->getSelectedId();
        const auto ov = s == 2 ? ac::Override::On : s == 3 ? ac::Override::Off : ac::Override::Auto;
        overrides_[box->id] = ov;
        if (onOverride) onOverride(box->id, ov);
    };
    return box;
}

int StageTable::getColumnAutoSizeWidth(int col) { return col == ColReason ? 500 : 120; }

juce::String StageTable::getCellTooltip(int row, int col)
{
    if (row < 0 || row >= int(rows_.size())) return {};
    if (col == ColReason) return rows_[size_t(row)].reason;
    if (col == ColSettings) return rows_[size_t(row)].settings.replace("  |  ", "\n");
    return {};
}
