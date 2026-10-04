#include "Views.h"

// ---------------------------------------------------------------- Waveform
double WaveformView::xToSeconds(float x) const
{
    if (!data_ || getWidth() <= 0) return 0;
    return juce::jlimit(0.0, data_->durationS, double(x) / getWidth() * data_->durationS);
}

void WaveformView::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    auto r = getLocalBounds().toFloat();
    const float grH = 22.0f;
    auto wave = r.withTrimmedBottom(grH);
    auto grLane = r.removeFromBottom(grH);
    g.setColour(theme::line);
    g.drawHorizontalLine(int(wave.getCentreY()), 0, r.getWidth());
    if (!data_)
    {
        g.setColour(theme::dim);
        g.setFont(theme::font(16));
        g.drawText("Drop WAV / FLAC / AIFF / OGG / MP3 files here", getLocalBounds(), juce::Justification::centred);
        return;
    }
    auto drawWave = [&](const std::vector<float>& mn, const std::vector<float>& mx, juce::Colour c) {
        if (mn.empty()) return;
        juce::Path p;
        const int W = getWidth();
        const float mid = wave.getCentreY(), h = wave.getHeight() * 0.48f;
        for (int x = 0; x < W; ++x)
        {
            const size_t k0 = size_t(double(x) / W * mn.size()), k1 = std::max(k0 + 1, size_t(double(x + 1) / W * mn.size()));
            float lo = 0, hi = 0;
            for (size_t k = k0; k < std::min(k1, mn.size()); ++k) { lo = std::min(lo, mn[k]); hi = std::max(hi, mx[k]); }
            const float y0 = mid - juce::jlimit(-1.0f, 1.0f, hi) * h, y1 = mid - juce::jlimit(-1.0f, 1.0f, lo) * h;
            g.setColour(c);
            g.drawVerticalLine(x, y0, std::max(y0 + 1.0f, y1));
        }
    };
    drawWave(data_->origMin, data_->origMax, theme::original.withAlpha(showProc_ ? 0.45f : 0.9f));
    if (showProc_) drawWave(data_->procMin, data_->procMax, theme::accent.withAlpha(0.85f));

    // Gain reduction lane (0..12 dB).
    g.setColour(theme::panel2);
    g.fillRect(grLane);
    if (!data_->grTrace.empty())
    {
        const double hopS = 0.01;
        g.setColour(theme::bad.withAlpha(0.8f));
        for (int x = 0; x < getWidth(); ++x)
        {
            const double t0 = double(x) / getWidth() * data_->durationS, t1 = double(x + 1) / getWidth() * data_->durationS;
            float m = 0;
            for (size_t k = size_t(t0 / hopS); k < std::min(data_->grTrace.size(), size_t(t1 / hopS) + 1); ++k) m = std::max(m, data_->grTrace[k]);
            const float hh = juce::jlimit(0.0f, 1.0f, m / 12.0f) * grH;
            g.drawVerticalLine(x, grLane.getY(), grLane.getY() + hh);
        }
    }
    g.setColour(theme::dim);
    g.setFont(theme::font(11));
    g.drawText("GR", grLane.toNearestInt().withTrimmedLeft(4), juce::Justification::centredLeft);

    // Noise region selection.
    if (regA_ >= 0 && regB_ > regA_)
    {
        const float xa = float(regA_ / data_->durationS * getWidth()), xb = float(regB_ / data_->durationS * getWidth());
        g.setColour(theme::removed.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(xa, 0, xb - xa, wave.getHeight()));
        g.setColour(theme::removed);
        g.drawText("noise profile", juce::Rectangle<float>(xa + 3, 2, 120, 14).toNearestInt(), juce::Justification::left);
    }
    // Playhead.
    const float px = float(play_ / std::max(1e-9, data_->durationS) * getWidth());
    g.setColour(juce::Colours::white);
    g.drawVerticalLine(int(px), 0, float(getHeight()));
    // Time labels.
    g.setColour(theme::dim);
    g.drawText(juce::String(play_, 2) + " / " + juce::String(data_->durationS, 2) + " s", getLocalBounds().removeFromTop(16).withTrimmedRight(6),
               juce::Justification::right);
}

void WaveformView::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isShiftDown()) dragA_ = xToSeconds(float(e.x));
    else if (onSeek) onSeek(xToSeconds(float(e.x)));
}

void WaveformView::mouseDrag(const juce::MouseEvent& e)
{
    if (dragA_ >= 0)
    {
        const double b = xToSeconds(float(e.x));
        setRegion(std::min(dragA_, b), std::max(dragA_, b));
    }
    else if (onSeek) onSeek(xToSeconds(float(e.x)));
}

void WaveformView::mouseUp(const juce::MouseEvent&)
{
    if (dragA_ >= 0 && regB_ - regA_ > 0.05 && onRegion) onRegion(regA_, regB_);
    dragA_ = -1;
}

// ---------------------------------------------------------------- Spectrum
SpectrumView::SpectrumView()
{
    win_ = ac::hannWindow(N);
}

void SpectrumView::push(const float* x, int n, double sampleRate)
{
    sr_ = sampleRate;
    for (int i = 0; i < n; ++i)
    {
        ring_[size_t(ringPos_)] = x[i];
        ringPos_ = (ringPos_ + 1) % N;
    }
    std::vector<float> fr(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) fr[size_t(i)] = ring_[size_t((ringPos_ + i) % N)] * win_[size_t(i)];
    std::vector<std::complex<float>> sp(static_cast<size_t>(N / 2 + 1));
    fft_.forward(fr.data(), sp.data());
    for (int k = 0; k <= N / 2; ++k)
    {
        // dB re full-scale sine (Hann coherent gain 0.5)
        const float db = 10.0f * std::log10(std::norm(sp[size_t(k)]) / (float(N) * float(N) * 0.0625f) + 1e-20f);
        float& s = smoothDb_[size_t(k)];
        s = db > s ? db : s * 0.85f + db * 0.15f;
    }
    repaint();
}

void SpectrumView::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    auto r = getLocalBounds().toFloat().reduced(4);
    const float fLo = 20.0f, fHi = 20000.0f, dbLo = -100.0f, dbHi = 0.0f;
    auto xOf = [&](double f) { return r.getX() + float(std::log(f / fLo) / std::log(fHi / fLo)) * r.getWidth(); };
    auto yOf = [&](double db) { return r.getBottom() - float((db - dbLo) / (dbHi - dbLo)) * r.getHeight(); };
    g.setFont(theme::font(10));
    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        g.setColour(theme::line);
        g.drawVerticalLine(int(xOf(f)), r.getY(), r.getBottom());
        g.setColour(theme::dim);
        g.drawText(f >= 1000 ? juce::String(f / 1000.0, 0) + "k" : juce::String(f, 0), juce::Rectangle<float>(xOf(f) + 2, r.getBottom() - 12, 30, 12).toNearestInt(),
                   juce::Justification::left);
    }
    for (double db : { -80.0, -60.0, -40.0, -20.0 })
    {
        g.setColour(theme::line.withAlpha(0.5f));
        g.drawHorizontalLine(int(yOf(db)), r.getX(), r.getRight());
    }
    // Long-term spectra (relative, aligned at their 1 kHz region for shape comparison).
    auto drawLtas = [&](const ac::SpectrumInfo& si, juce::Colour c) {
        if (si.bandHz.empty()) return;
        juce::Path p;
        bool started = false;
        for (size_t i = 0; i < si.bandHz.size(); ++i)
        {
            if (si.bandAbsDb[i] < -150.0) { started = false; continue; } // no content in this band
            const float x = xOf(si.bandHz[i]), y = yOf(juce::jlimit(dbLo, dbHi, float(si.bandAbsDb[i] - 10.0)));
            if (!started) { p.startNewSubPath(x, y); started = true; }
            else p.lineTo(x, y);
        }
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType(2.0f));
    };
    if (data_)
    {
        drawLtas(data_->refLtas, theme::original.withAlpha(0.8f));
        drawLtas(data_->outLtas, theme::accent.withAlpha(0.9f));
    }
    // Realtime analyser.
    juce::Path rt;
    bool started = false;
    for (int k = 1; k <= N / 2; ++k)
    {
        const double f = k * sr_ / N;
        if (f < fLo || f > fHi) continue;
        const float x = xOf(f), y = yOf(juce::jlimit(-120.0f, 6.0f, smoothDb_[size_t(k)]));
        if (!started) { rt.startNewSubPath(x, y); started = true; }
        else rt.lineTo(x, y);
    }
    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.strokePath(rt, juce::PathStrokeType(1.0f));
    g.setColour(theme::dim);
    g.setFont(theme::font(11));
    g.drawText("LTAS: original (violet) / processed (blue); white = live", getLocalBounds().reduced(6, 4), juce::Justification::topLeft);
}

// ------------------------------------------------------------------- Meters
void MeterView::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    auto r = getLocalBounds().reduced(6);
    g.setColour(theme::text);
    g.setFont(theme::font(12, true));
    g.drawText(v_.label, r.removeFromTop(16), juce::Justification::left);
    const int barW = (r.getWidth() - 12) / 5;
    struct Bar { const char* name; float value; float lo, hi; juce::Colour c; bool inverted; float marker; };
    const Bar bars[] = {
        { "M", v_.momentary, -50, 0, theme::accent, false, v_.target },
        { "S", v_.shortTerm, -50, 0, theme::accent, false, v_.target },
        { "I", v_.integrated, -50, 0, theme::good, false, v_.target },
        { "TP max", v_.truePeak, -30, 3, theme::warn, false, v_.ceiling },
        { "GR", v_.gr, 0, 15, theme::bad, true, -1000 },
    };
    for (int i = 0; i < 5; ++i)
    {
        auto col = juce::Rectangle<int>(r.getX() + i * (barW + 3), r.getY(), barW, r.getHeight());
        auto label = col.removeFromBottom(30);
        g.setColour(theme::panel2);
        g.fillRect(col);
        const auto& b = bars[i];
        const float t = juce::jlimit(0.0f, 1.0f, (b.value - b.lo) / (b.hi - b.lo));
        g.setColour(b.c);
        if (b.inverted) g.fillRect(col.withHeight(int(t * col.getHeight())));
        else g.fillRect(col.withTop(col.getBottom() - int(t * col.getHeight())));
        if (b.marker > b.lo && b.marker < b.hi)
        {
            const int y = col.getBottom() - int((b.marker - b.lo) / (b.hi - b.lo) * col.getHeight());
            g.setColour(juce::Colours::white);
            g.drawHorizontalLine(y, float(col.getX()), float(col.getRight()));
        }
        g.setColour(theme::dim);
        g.setFont(theme::font(10));
        g.drawText(b.name, label.removeFromTop(14), juce::Justification::centred);
        g.setColour(theme::text);
        g.drawText(b.value <= -69.9f ? juce::String("-inf") : juce::String(b.value, 1), label, juce::Justification::centred);
    }
}
