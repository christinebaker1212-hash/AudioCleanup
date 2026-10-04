#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace theme {
inline const juce::Colour bg { 0xff15171c };
inline const juce::Colour panel { 0xff1d2027 };
inline const juce::Colour panel2 { 0xff242832 };
inline const juce::Colour line { 0xff343a46 };
inline const juce::Colour text { 0xffdfe3ea };
inline const juce::Colour dim { 0xff8b93a3 };
inline const juce::Colour accent { 0xff4fb3ff };    // processed
inline const juce::Colour original { 0xffb08cff };  // original
inline const juce::Colour removed { 0xffff9f43 };   // removed noise
inline const juce::Colour good { 0xff47d18c };
inline const juce::Colour warn { 0xffffc048 };
inline const juce::Colour bad { 0xffff5d6c };

inline juce::Font font(float size, bool bold = false)
{
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        auto scheme = getDarkColourScheme();
        scheme.setUIColour(ColourScheme::windowBackground, bg);
        scheme.setUIColour(ColourScheme::widgetBackground, panel2);
        scheme.setUIColour(ColourScheme::menuBackground, panel);
        scheme.setUIColour(ColourScheme::outline, line);
        scheme.setUIColour(ColourScheme::defaultText, text);
        scheme.setUIColour(ColourScheme::defaultFill, accent);
        scheme.setUIColour(ColourScheme::highlightedText, juce::Colours::white);
        scheme.setUIColour(ColourScheme::highlightedFill, accent.withAlpha(0.5f));
        scheme.setUIColour(ColourScheme::menuText, text);
        setColourScheme(scheme);
        setColour(juce::Slider::thumbColourId, accent);
        setColour(juce::Slider::trackColourId, accent.withAlpha(0.6f));
        setColour(juce::TextButton::buttonColourId, panel2);
        setColour(juce::TextButton::buttonOnColourId, accent.darker(0.3f));
        setColour(juce::ComboBox::backgroundColourId, panel2);
        setColour(juce::ListBox::backgroundColourId, panel);
        setColour(juce::TextEditor::backgroundColourId, panel);
        setColour(juce::TableHeaderComponent::backgroundColourId, panel2);
        setColour(juce::TabbedComponent::backgroundColourId, panel);
        setColour(juce::TabbedButtonBar::tabOutlineColourId, line);
    }
};
} // namespace theme
