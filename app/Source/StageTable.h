#pragma once
// Advanced panel: every stage with its decision, reason, actual DSP settings
// and a per-stage override (Auto / On / Bypass).

#include "Theme.h"

#include <ac/Pipeline.h>

class StageTable : public juce::Component, private juce::TableListBoxModel
{
public:
    StageTable();
    std::function<void(ac::StageId, ac::Override)> onOverride;

    void setPlan(const ac::Plan* plan, const std::map<ac::StageId, ac::Override>& overrides);
    void resized() override { table_.setBounds(getLocalBounds()); }

private:
    int getNumRows() override { return int(rows_.size()); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;
    int getColumnAutoSizeWidth(int col) override;
    juce::String getCellTooltip(int row, int col) override;

    struct Row
    {
        ac::StageId id;
        bool enabled, available, forced;
        juce::String section, name, reason, settings;
    };
    std::vector<Row> rows_;
    std::map<ac::StageId, ac::Override> overrides_;
    juce::TableListBox table_{ "Stages", this };
};
