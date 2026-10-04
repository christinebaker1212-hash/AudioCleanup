#pragma once
#include "Session.h"
#include "Theme.h"

#include <ac/Fft.h>

/** Overview waveform: original (behind) and processed, GR lane, playhead,
    click-to-seek, shift-drag to select a noise-only region. */
class WaveformView : public juce::Component
{
public:
    std::function<void(double seconds)> onSeek;
    std::function<void(double startS, double endS)> onRegion;

    void setData(std::shared_ptr<const DisplayData> d) { data_ = std::move(d); repaint(); }
    void setPlayhead(double s) { if (std::abs(s - play_) > 1e-3) { play_ = s; repaint(); } }
    void setRegion(double a, double b) { regA_ = a; regB_ = b; repaint(); }
    void setShowProcessed(bool b) { showProc_ = b; repaint(); }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    double xToSeconds(float x) const;
    std::shared_ptr<const DisplayData> data_;
    double play_ = 0, regA_ = -1, regB_ = -1, dragA_ = -1;
    bool showProc_ = true;
};

/** Realtime spectrum of the monitored signal over the long-term spectra of
    original and processed (1/3-octave LTAS). */
class SpectrumView : public juce::Component
{
public:
    SpectrumView();
    void setData(std::shared_ptr<const DisplayData> d) { data_ = std::move(d); repaint(); }
    void push(const float* x, int n, double sampleRate);
    void paint(juce::Graphics& g) override;

private:
    std::shared_ptr<const DisplayData> data_;
    static constexpr int N = 4096;
    ac::RealFft fft_{ N };
    std::vector<float> ring_ = std::vector<float>(N, 0.0f), win_;
    std::vector<float> smoothDb_ = std::vector<float>(N / 2 + 1, -120.0f);
    int ringPos_ = 0;
    double sr_ = 48000;
};

/** Loudness / true-peak / gain-reduction meters at the playhead. */
class MeterView : public juce::Component
{
public:
    struct Values
    {
        float momentary = -70, shortTerm = -70, integrated = -70, truePeak = -70, gr = 0;
        float target = -16, ceiling = -1;
        bool processed = true;
        juce::String label;
    };
    void setValues(const Values& v) { v_ = v; repaint(); }
    void paint(juce::Graphics& g) override;

private:
    Values v_;
};
