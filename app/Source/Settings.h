#pragma once
// Persistent application settings (interface size) and window fitting.

#include <juce_gui_basics/juce_gui_basics.h>

namespace settings {

/** Persistent per-user properties file (AudioFinisher/AudioFinisher.settings). */
juce::PropertiesFile& props();

/** Interface size multiplier on top of the OS display scaling (0.7 .. 1.2). */
double uiScale();
void setUiScale(double s);
inline const double kUiScales[] = { 0.7, 0.8, 0.9, 1.0, 1.1, 1.2 };

/** Shrink/move a top-level window so it lies fully inside the usable area of
    the display it is on (taskbar excluded), at the current scaling. */
void fitToScreen(juce::Component& window, int preferredW, int preferredH, bool centre);

} // namespace settings
