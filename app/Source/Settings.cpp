#include "Settings.h"

namespace settings {

juce::PropertiesFile& props()
{
    static std::unique_ptr<juce::PropertiesFile> p = [] {
        juce::PropertiesFile::Options o;
        o.applicationName = "AudioFinisher";
        o.folderName = "AudioFinisher";
        o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        return std::make_unique<juce::PropertiesFile>(o);
    }();
    return *p;
}

double uiScale() { return juce::jlimit(0.5, 2.0, props().getDoubleValue("uiScale", 1.0)); }

void setUiScale(double s)
{
    props().setValue("uiScale", s);
    props().saveIfNeeded();
}

void fitToScreen(juce::Component& window, int preferredW, int preferredH, bool centre)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* d = displays.getDisplayForRect(window.getScreenBounds());
    if (!d) d = displays.getPrimaryDisplay();
    if (!d) return;
    // Leave room for the native title bar and a small margin. All values are
    // logical pixels, so this is correct at any OS scaling (125 %, 150 %...).
    const auto area = d->userArea.reduced(8).withTrimmedTop(32);
    const int w = std::min(preferredW, area.getWidth());
    const int h = std::min(preferredH, area.getHeight());
    juce::Rectangle<int> b(window.getX(), window.getY(), w, h);
    if (centre) b = b.withCentre(area.getCentre());
    b = b.constrainedWithin(area);
    window.setBounds(b);
}

} // namespace settings
