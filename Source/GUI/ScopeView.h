#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Widgets.h"
#include "Tuner.h"
#include "../PluginProcessor.h"

/** Row of radio buttons (not bound to a parameter): view options. */
class OptionGroup : public juce::Component
{
public:
    OptionGroup (const juce::StringArray& labels, int initial, const juce::String& tooltip)
    {
        for (int i = 0; i < labels.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (labels[i]));
            b->setComponentID ("seg");
            b->setTooltip (tooltip);
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < labels.size() - 1) edges |= juce::Button::ConnectedOnRight;
            b->setConnectedEdges (edges);
            b->onClick = [this, i] { setIndex (i); if (onChange) onChange (i); };
            addAndMakeVisible (b);
        }
        setIndex (initial);
    }

    std::function<void (int)> onChange;
    int getIndex() const { return index; }
    void setIndex (int i)
    {
        index = i;
        for (int b = 0; b < buttons.size(); ++b) buttons[b]->setToggleState (b == i, juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int w = r.getWidth() / juce::jmax (1, buttons.size());
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setBounds (i == buttons.size() - 1 ? r : r.removeFromLeft (w));
    }

private:
    juce::OwnedArray<juce::TextButton> buttons;
    int index = 0;
};

/** Stereo ring buffer of the plugin output, shared by the scope page panels. */
struct StereoHistory
{
    static constexpr int size = 1 << 17;   // ~2.7 s at 48 kHz
    std::vector<float> l = std::vector<float> (size, 0.0f), r = std::vector<float> (size, 0.0f);
    int writePos = 0;
    int newSamples = 0;                     // samples added during the last update

    void push (const float* lp, const float* rp, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            l[(size_t) writePos] = lp[i];
            r[(size_t) writePos] = rp[i];
            writePos = (writePos + 1) & (size - 1);
        }
    }
    /** Sample `ago` samples before the newest one. */
    float left (int ago) const  { return l[(size_t) ((writePos - 1 - ago) & (size - 1))]; }
    float right (int ago) const { return r[(size_t) ((writePos - 1 - ago) & (size - 1))]; }
};

//==============================================================================
/** Panel frame with a title and space for option groups in the header strip. */
class ScopePanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ScopePanel (const juce::String& t) : title (t) {}
    void paint (juce::Graphics& g) override { drawSectionPanel (g, *this, getLocalBounds(), title, Colours_::accent); paintContent (g, content()); }

protected:
    virtual void paintContent (juce::Graphics&, juce::Rectangle<int>) {}
    juce::Rectangle<int> content() const { return getLocalBounds().reduced (10).withTrimmedTop (30); }
    juce::Rectangle<int> headerSlot (int fromRight, int width) const
    {
        return getLocalBounds().withHeight (28).withTrimmedRight (fromRight).removeFromRight (width).reduced (0, 4);
    }
    void plotBackground (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Colours_::outline);
        Px::Snap (g, *this).border (r, 6.0f);
    }
    juce::String title;
};

//==============================================================================
class OscilloscopePanel : public ScopePanel
{
public:
    OscilloscopePanel (CodexProcessor& p, const StereoHistory& h)
        : ScopePanel ("OSCILLOSCOPE"), proc (p), history (h),
          mode ({ "HIT", "FREE" }, 0, "Sync: HIT starts the display at every note (captures the full kick); FREE shows the signal continuously"),
          window ({ "100 MS", "250 MS", "500 MS", "1 S", "2 S" }, 2, "Time window shown"),
          channels ({ "L / R", "MID", "SIDE", "LEFT", "RIGHT" }, 0, "Channels: left and right overlaid, mid (L+R), side (L-R) or a single side")
    {
        for (auto* c : { &mode, &window, &channels }) addAndMakeVisible (c);
        window.onChange = [this] (int) { proc.scopeWindowSeconds.store (windowSeconds()); };
        proc.scopeWindowSeconds.store (windowSeconds());
    }

    void update()
    {
        if (mode.getIndex() == 0)
        {
            const int gen = proc.scopeGeneration.load(), written = proc.scopeWritten.load();
            if (gen == lastGen && written == hitCount) return;
            lastGen = gen;
            hitCount = juce::jlimit (0, CodexProcessor::scopePoints, written);
            for (size_t c = 0; c < CodexProcessor::numScopeChannels; ++c)
            {
                std::copy (proc.scopeMin[c].begin(), proc.scopeMin[c].begin() + hitCount, hitMin[c].begin());
                std::copy (proc.scopeMax[c].begin(), proc.scopeMax[c].begin() + hitCount, hitMax[c].begin());
            }
            hitSeconds = proc.scopeCapturedSeconds.load();
        }
        repaint();
    }

    void resized() override
    {
        channels.setBounds (headerSlot (10, 300));
        window.setBounds (headerSlot (322, 270));
        mode.setBounds (headerSlot (604, 110));
    }

private:
    float windowSeconds() const { const float w[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f }; return w[window.getIndex()]; }

    void paintContent (juce::Graphics& g, juce::Rectangle<int> area) override
    {
        using namespace juce;
        auto r = area.toFloat();
        plotBackground (g, r);
        auto plot = r.reduced (10.0f, 8.0f).withTrimmedBottom (14.0f);

        const bool hit = mode.getIndex() == 0;
        const float seconds = hit ? hitSeconds : windowSeconds();
        const float ms = seconds * 1000.0f;
        const float step = ms <= 120 ? 10.0f : ms <= 300 ? 25.0f : ms <= 600 ? 50.0f : ms <= 1100 ? 100.0f : 200.0f;

        const Px::Snap px (g, *this);
        for (float t = step; t < ms; t += step)
        {
            const float x = plot.getX() + plot.getWidth() * (hit ? t / ms : 1.0f - t / ms);
            const bool major = std::fmod (t, step * 2.0f) < 0.01f;
            g.setColour (major ? Colours_::grid.brighter (0.4f) : Colours_::grid);
            px.vline (x, plot.getY(), plot.getBottom());
            if (major)
                drawAxisText (g, (hit ? "" : "-") + String ((int) t) + " ms", Rectangle<float> (x - 30.0f, plot.getBottom() + 1.0f, 60.0f, 14.0f), Justification::centred);
        }
        for (float lv : { -0.5f, 0.5f })
        {
            g.setColour (Colours_::grid);
            px.hline (plot.getCentreY() - lv * plot.getHeight() * 0.5f, plot.getX(), plot.getRight());
        }
        g.setColour (Colours_::grid.brighter (0.6f));
        px.hline (plot.getCentreY(), plot.getX(), plot.getRight());

        // Which traces to draw: {scope channel, colour}
        std::vector<std::pair<int, Colour>> traces;
        switch (channels.getIndex())
        {
            case 0:  traces = { { 0, Colours_::accent }, { 1, Colours_::accent2 } }; break;
            case 1:  traces = { { 2, Colours_::accent } }; break;
            case 2:  traces = { { 3, Colours_::accent2 } }; break;
            case 3:  traces = { { 0, Colours_::accent } }; break;
            default: traces = { { 1, Colours_::accent2 } }; break;
        }

        // one column per physical pixel (HiDPI / scaled UI), so the trace stays sharp
        const float pixelScale = jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const int columns = jmax (2, (int) (plot.getWidth() * pixelScale));
        std::vector<float> mn ((size_t) jmax (columns, CodexProcessor::scopePoints)), mx (mn.size());
        bool any = false;

        for (const auto& [ch, colour] : traces)
        {
            int count = 0;
            if (hit)
            {
                if (hitCount < 2) continue;
                count = hitCount;   // one display point per captured point, spread over the full width
                for (int i = 0; i < count; ++i)
                {
                    mn[(size_t) i] = hitMin[(size_t) ch][(size_t) i];
                    mx[(size_t) i] = hitMax[(size_t) ch][(size_t) i];
                }
            }
            else
            {
                const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
                const int total = jmin (StereoHistory::size - 1, (int) (seconds * sr));
                count = columns;
                for (int x = 0; x < columns; ++x)
                {
                    // x = 0 is the oldest sample, the right edge is "now"
                    const int a0 = total - (int) ((long long) x * total / columns);
                    const int a1 = jmax (0, total - (int) ((long long) (x + 1) * total / columns));
                    float a = 1.0e9f, b = -1.0e9f;
                    for (int ago = a1; ago < a0; ++ago)
                    {
                        const float L = history.left (ago), R = history.right (ago);
                        const float v = ch == 0 ? L : ch == 1 ? R : ch == 2 ? 0.5f * (L + R) : 0.5f * (L - R);
                        a = jmin (a, v); b = jmax (b, v);
                    }
                    mn[(size_t) x] = a; mx[(size_t) x] = b;
                }
            }
            if (count < 2) continue;
            any = true;

            Path wave;
            auto xAt = [&] (int i) { return plot.getX() + plot.getWidth() * (float) i / (float) ((hit ? CodexProcessor::scopePoints : count) - 1); };
            auto yAt = [&] (float v) { return plot.getCentreY() - jlimit (-1.1f, 1.1f, v) * plot.getHeight() * 0.5f; };

            // Zoomed in far enough that a column holds about one sample: draw the actual waveform as a crisp line
            const double srNow = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            if (! hit && seconds * srNow < (double) columns * 1.5)
            {
                const int total = jmin (StereoHistory::size - 1, (int) (seconds * srNow));
                Path line;
                for (int i = 0; i <= total; ++i)
                {
                    const int ago = total - i;
                    const float L = history.left (ago), R = history.right (ago);
                    const float v = ch == 0 ? L : ch == 1 ? R : ch == 2 ? 0.5f * (L + R) : 0.5f * (L - R);
                    const float x = plot.getX() + plot.getWidth() * (float) i / (float) total;
                    if (i == 0) line.startNewSubPath (x, yAt (v)); else line.lineTo (x, yAt (v));
                }
                Colours_::strokeGlow (g, line, colour, 1.3f);
                continue;
            }
            wave.startNewSubPath (xAt (0), yAt (mx[0]));
            for (int i = 1; i < count; ++i) wave.lineTo (xAt (i), yAt (mx[(size_t) i]));
            for (int i = count - 1; i >= 0; --i) wave.lineTo (xAt (i), yAt (mn[(size_t) i]));
            wave.closeSubPath();
            g.setColour (colour.withAlpha (traces.size() > 1 ? 0.22f : 0.3f));
            g.fillPath (wave);
            g.setColour (colour);
            g.strokePath (wave, PathStrokeType (1.0f / pixelScale * 1.2f));
        }

        if (! any)
        {
            g.setColour (Colours_::textMuted);
            g.setFont (Fonts::regular (Fonts::Size::body));
            g.drawText (hit ? "Play a note to capture the kick" : "No signal", plot, Justification::centred);
        }

        // 0 dBFS reference label
        drawAxisText (g, "0 dB", plot.withHeight (14.0f).translated (4.0f, 0.0f), Justification::centredLeft, true);
    }

    CodexProcessor& proc;
    const StereoHistory& history;
    OptionGroup mode, window, channels;
    std::array<std::array<float, CodexProcessor::scopePoints>, CodexProcessor::numScopeChannels> hitMin {}, hitMax {};
    int hitCount = 0, lastGen = -1;
    float hitSeconds = 0.5f;
};

//==============================================================================
class StereoFieldPanel : public ScopePanel
{
public:
    explicit StereoFieldPanel (const StereoHistory& h)
        : ScopePanel ("STEREO FIELD"), history (h),
          orientation ({ "M/S", "L/R" }, 0, "Orientation: M/S shows mono content as a vertical line (vectorscope); L/R shows a Lissajous plot")
    {
        addAndMakeVisible (orientation);
    }

    void update (int newSamples)
    {
        // correlation and width over the newest samples
        const int n = juce::jmin (8192, juce::jmax (newSamples, 1024));
        double lr = 0.0, ll = 0.0, rr = 0.0, mm = 0.0, ss = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double L = history.left (i), R = history.right (i);
            lr += L * R; ll += L * L; rr += R * R;
            mm += 0.25 * (L + R) * (L + R); ss += 0.25 * (L - R) * (L - R);
        }
        if (ll + rr > 1.0e-7)
        {
            const float corr = (float) (lr / std::sqrt (std::max (1.0e-12, ll * rr)));
            correlation += 0.25f * (juce::jlimit (-1.0f, 1.0f, corr) - correlation);
            width += 0.25f * ((float) (ss / std::max (1.0e-12, mm + ss)) - width);
            active = true;
        }
        else
            active = false;
        repaint();
    }

    void resized() override { orientation.setBounds (headerSlot (10, 110)); }

private:
    void paintContent (juce::Graphics& g, juce::Rectangle<int> area) override
    {
        using namespace juce;
        auto r = area.toFloat();
        auto meterArea = r.removeFromBottom (42.0f);
        plotBackground (g, r);

        const float size = jmin (r.getWidth(), r.getHeight()) - 20.0f;
        auto plot = r.withSizeKeepingCentre (size, size);
        const auto c = plot.getCentre();
        const bool ms = orientation.getIndex() == 0;

        // guides: circle, axes and the L / R diagonals
        g.setColour (Colours_::grid.brighter (0.3f));
        g.drawEllipse (plot, 1.0f);
        g.drawEllipse (plot.reduced (size * 0.25f), 1.0f);
        g.setColour (Colours_::grid.brighter (0.5f));
        g.drawLine (c.x, plot.getY(), c.x, plot.getBottom(), 1.0f);
        g.drawLine (plot.getX(), c.y, plot.getRight(), c.y, 1.0f);
        const float d = size * 0.3535f;
        g.setColour (Colours_::grid.brighter (0.2f));
        g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 1.0f);
        g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d, 1.0f);

        auto axis = [&] (const char* t, Rectangle<float> box) { drawAxisText (g, t, box, Justification::centred); };
        if (ms)
        {
            axis ("M", Rectangle<float> (c.x - 10.0f, plot.getY() - 3.0f, 20.0f, 14.0f));
            axis ("L", Rectangle<float> (c.x - d - 15.0f, c.y - d - 15.0f, 14.0f, 14.0f));
            axis ("R", Rectangle<float> (c.x + d + 1.0f, c.y - d - 15.0f, 14.0f, 14.0f));
            axis ("+S", Rectangle<float> (plot.getRight() - 20.0f, c.y + 2.0f, 20.0f, 14.0f));
        }
        else
        {
            axis ("L", Rectangle<float> (c.x - 10.0f, plot.getY() - 3.0f, 20.0f, 14.0f));
            axis ("R", Rectangle<float> (plot.getRight() - 15.0f, c.y + 2.0f, 14.0f, 14.0f));
        }

        // points: the newest ~40 ms, older points fainter
        const int n = 6144;
        const float scale = size * 0.5f;
        for (int i = n - 1; i >= 0; --i)
        {
            const float L = history.left (i), R = history.right (i);
            float x, y;
            if (ms) { x = (L - R) * 0.7071f; y = (L + R) * 0.7071f; }
            else    { x = R; y = L; }
            const float px = c.x + jlimit (-1.0f, 1.0f, x) * scale, py = c.y - jlimit (-1.0f, 1.0f, y) * scale;
            g.setColour (Colours_::accent.withAlpha (0.08f + 0.55f * (1.0f - (float) i / (float) n)));
            g.fillRect (px - 0.6f, py - 0.6f, 1.2f, 1.2f);
        }

        // correlation and width meters
        meterArea.removeFromTop (8.0f);
        auto corrRow = meterArea.removeFromTop (16.0f);
        auto widthRow = meterArea.withTrimmedTop (2.0f).removeFromTop (16.0f);
        auto bar = [&] (Rectangle<float> row, const String& label, float value, float minV, float maxV, const String& text, Colour col)
        {
            g.setFont (Fonts::label (Fonts::Size::micro, 0.1f));
            g.setColour (Colours_::textDim);
            g.drawText (label, row.removeFromLeft (80.0f), Justification::centredLeft);
            g.setColour (Colours_::text);
            g.setFont (Fonts::medium (12.0f));
            g.drawText (text, row.removeFromRight (44.0f), Justification::centredRight);
            row = row.reduced (6.0f, 4.0f);
            g.setColour (Colours_::display);
            g.fillRoundedRectangle (row, 3.0f);
            const float zero = jmap (0.0f, minV, maxV, row.getX(), row.getRight());
            const float pos = jmap (jlimit (minV, maxV, value), minV, maxV, row.getX(), row.getRight());
            g.setColour (col);
            g.fillRoundedRectangle (Rectangle<float> (jmin (zero, pos), row.getY(), std::abs (pos - zero) + 1.0f, row.getHeight()), 2.0f);
            g.setColour (Colours_::textFaint);
            Px::Snap (g, *this).vline (zero, row.getY() - 2.0f, row.getBottom() + 2.0f);
        };
        bar (corrRow, "CORRELATION", active ? correlation : 0.0f, -1.0f, 1.0f, active ? String (correlation, 2) : String ("--"),
             correlation < 0.0f ? Colours_::warn : Colours_::good);
        bar (widthRow, "WIDTH", active ? width : 0.0f, 0.0f, 1.0f, active ? String (roundToInt (width * 100.0f)) + " %" : String ("--"), Colours_::accent);
    }

    const StereoHistory& history;
    OptionGroup orientation;
    float correlation = 1.0f, width = 0.0f;
    bool active = false;
};

//==============================================================================
class SpectrumPanel : public ScopePanel
{
public:
    explicit SpectrumPanel (CodexProcessor& p, const StereoHistory& h)
        : ScopePanel ("SPECTRUM"), proc (p), history (h),
          channel ({ "MID", "SIDE", "L / R" }, 0, "Channel analysed"),
          slope ({ "0", "3", "4.5" }, 1, "Slope (dB/oct): tilts the display so a balanced mix looks flat; 3 dB is common for mixing, 4.5 dB for mastering"),
          hold ({ "HOLD" }, 0, "Peak hold: keep the highest level per frequency (click again to reset)")
    {
        for (auto* c : { &channel, &slope, &hold }) addAndMakeVisible (c);
        hold.onChange = [this] (int) { holdOn = ! holdOn; hold.setIndex (holdOn ? 0 : -1); std::fill (peak[0].begin(), peak[0].end(), -150.0f); std::fill (peak[1].begin(), peak[1].end(), -150.0f); };
        holdOn = false;
        hold.setIndex (-1);
        for (int c = 0; c < 2; ++c) { smooth[(size_t) c].assign (fftSize / 2, -150.0f); peak[(size_t) c].assign (fftSize / 2, -150.0f); }
        fftData.assign (fftSize * 2, 0.0f);
    }

    void update()
    {
        const int traces = channel.getIndex() == 2 ? 2 : 1;
        for (int t = 0; t < traces; ++t)
        {
            std::fill (fftData.begin(), fftData.end(), 0.0f);
            for (int i = 0; i < fftSize; ++i)
            {
                const float L = history.left (fftSize - 1 - i), R = history.right (fftSize - 1 - i);
                fftData[(size_t) i] = channel.getIndex() == 0 ? 0.5f * (L + R) : channel.getIndex() == 1 ? 0.5f * (L - R) : (t == 0 ? L : R);
            }
            window.multiplyWithWindowingTable (fftData.data(), fftSize);
            fft.performFrequencyOnlyForwardTransform (fftData.data());
            for (size_t i = 0; i < (size_t) fftSize / 2; ++i)
            {
                const float db = juce::Decibels::gainToDecibels (fftData[i] * 4.0f / fftSize, -150.0f);
                auto& s = smooth[(size_t) t][i];
                s = db > s ? db : juce::jmax (db, s - 1.5f);
                if (holdOn) peak[(size_t) t][i] = juce::jmax (peak[(size_t) t][i], s);
            }
        }
        repaint();
    }

    void resized() override
    {
        hold.setBounds (headerSlot (10, 62));
        slope.setBounds (headerSlot (80, 120));
        channel.setBounds (headerSlot (208, 170));
    }

private:
    static constexpr int fftOrder = 14, fftSize = 1 << fftOrder;

    void paintContent (juce::Graphics& g, juce::Rectangle<int> area) override
    {
        using namespace juce;
        auto r = area.toFloat();
        plotBackground (g, r);
        auto plot = r.reduced (10.0f, 8.0f).withTrimmedBottom (14.0f).withTrimmedRight (26.0f);
        auto fx = [&] (float f) { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
        auto dy = [&] (float db) { return jmap (db, -96.0f, 0.0f, plot.getBottom(), plot.getY()); };

        const Px::Snap px (g, *this);
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            g.setColour (Colours_::grid.brighter (0.3f));
            px.vline (fx (f), plot.getY(), plot.getBottom());
            const bool decade = juce::approximatelyEqual (f, 100.0f) || juce::approximatelyEqual (f, 1000.0f) || juce::approximatelyEqual (f, 10000.0f);
            drawAxisText (g, f >= 1000 ? String ((int) (f / 1000)) + "k" : String ((int) f), Rectangle<float> (fx (f) - 15, plot.getBottom() + 1, 30, 14),
                          Justification::centred, decade);
        }
        for (float db = -84.0f; db <= -12.0f; db += 12.0f)
        {
            g.setColour (Colours_::grid);
            px.hline (dy (db), plot.getX(), plot.getRight());
            drawAxisText (g, String ((int) db), Rectangle<float> (plot.getRight() + 3, dy (db) - 7, 26, 14), Justification::centredLeft);
        }

        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        const float slopeDb = slope.getIndex() == 0 ? 0.0f : slope.getIndex() == 1 ? 3.0f : 4.5f;
        const int traces = channel.getIndex() == 2 ? 2 : 1;
        const Colour cols[] = { channel.getIndex() == 1 ? Colours_::accent2 : Colours_::accent, Colours_::accent2 };

        auto buildPath = [&] (const std::vector<float>& db, bool closed)
        {
            Path p;
            bool started = false;
            const float step = 1.0f / jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
            for (float xf = plot.getX(); xf <= plot.getRight(); xf += step)
            {
                const float x = xf;
                const float f = 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth());
                const float fNext = 20.0f * std::pow (1000.0f, (x + step - plot.getX()) / plot.getWidth());
                const float bin = f / (float) (sr / fftSize), binNext = fNext / (float) (sr / fftSize);
                const int i = jlimit (1, fftSize / 2 - 2, (int) bin);
                const float frac = jlimit (0.0f, 1.0f, bin - (float) i);
                float v = jmap (frac, db[(size_t) i], db[(size_t) i + 1]);
                for (int k = i + 1; k < jmin (fftSize / 2 - 1, (int) binNext); ++k) v = jmax (v, db[(size_t) k]);   // many bins per pixel: peak
                v += slopeDb * std::log2 (f / 1000.0f);   // tilt around 1 kHz
                const float y = jlimit (plot.getY(), plot.getBottom(), dy (v));
                if (! started) { p.startNewSubPath (x, closed ? plot.getBottom() : y); if (closed) p.lineTo (x, y); started = true; }
                else p.lineTo (x, y);
            }
            if (closed) { p.lineTo (plot.getRight(), plot.getBottom()); p.closeSubPath(); }
            return p;
        };

        for (int t = 0; t < traces; ++t)
        {
            auto fill = buildPath (smooth[(size_t) t], true);
            g.setGradientFill (ColourGradient (cols[t].withAlpha (traces > 1 ? 0.18f : 0.3f), 0, plot.getY(), cols[t].withAlpha (0.02f), 0, plot.getBottom(), false));
            g.fillPath (fill);
            Colours_::strokeGlow (g, buildPath (smooth[(size_t) t], false), cols[t], 1.2f);
            if (holdOn)
            {
                g.setColour (Colours_::text.withAlpha (0.6f));
                g.strokePath (buildPath (peak[(size_t) t], false), PathStrokeType (1.0f));
            }
        }
    }

    CodexProcessor& proc;
    const StereoHistory& history;
    OptionGroup channel, slope, hold;
    bool holdOn = false;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftData;
    std::array<std::vector<float>, 2> smooth, peak;
};

//==============================================================================
/** Left/right peak and RMS meters with numeric read-outs. */
class StereoMetersPanel : public ScopePanel
{
public:
    explicit StereoMetersPanel (const StereoHistory& h) : ScopePanel ("LEVELS"), history (h)
    {
        setTooltip ("Peak (bright) and RMS (solid) level per channel. Click to reset the peak values.");
    }

    void update (int newSamples)
    {
        const int n = juce::jmax (1, juce::jmin (newSamples, StereoHistory::size - 1));
        for (int c = 0; c < 2; ++c)
        {
            float pk = 0.0f; double sum = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const float v = c == 0 ? history.left (i) : history.right (i);
                pk = juce::jmax (pk, std::abs (v));
                sum += (double) v * v;
            }
            const float pkDb = juce::Decibels::gainToDecibels (pk, -70.0f);
            const float rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (sum / n), -70.0f);
            peakDb[(size_t) c] = pkDb > peakDb[(size_t) c] ? pkDb : juce::jmax (-70.0f, peakDb[(size_t) c] - 1.2f);
            rms[(size_t) c] += 0.3f * (rmsDb - rms[(size_t) c]);
            maxDb[(size_t) c] = juce::jmax (maxDb[(size_t) c], pkDb);
        }
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override { maxDb = { -70.0f, -70.0f }; }

private:
    void paintContent (juce::Graphics& g, juce::Rectangle<int> area) override
    {
        using namespace juce;
        auto r = area.toFloat();
        plotBackground (g, r);
        r = r.reduced (12.0f, 10.0f);
        auto readouts = r.removeFromBottom (34.0f);
        auto scaleArea = r.removeFromRight (26.0f);
        const float barW = (r.getWidth() - 12.0f) / 2.0f;
        auto toY = [&] (float db) { return jmap (db, -60.0f, 6.0f, r.getBottom(), r.getY()); };

        const Px::Snap px (g, *this);
        for (float db = -54.0f; db <= 0.0f; db += 6.0f)
        {
            g.setColour (Colours_::grid.brighter (0.3f));
            px.hline (toY (db), r.getX(), r.getRight());
            if ((int) db % 12 == 0)
                drawAxisText (g, String ((int) db), Rectangle<float> (scaleArea.getX() + 3, toY (db) - 7, 26, 14), Justification::centredLeft, db > -0.5f);
        }

        for (int c = 0; c < 2; ++c)
        {
            auto bar = Rectangle<float> (r.getX() + (float) c * (barW + 12.0f), r.getY(), barW, r.getHeight());
            g.setColour (Colours_::grid);
            g.fillRoundedRectangle (bar, 3.0f);
            const float yp = toY (jmin (6.0f, peakDb[(size_t) c])), yr = toY (jmin (6.0f, rms[(size_t) c]));
            g.setColour (Colours_::accent.withAlpha (0.35f));
            g.fillRoundedRectangle (bar.withTop (yp), 3.0f);
            g.setGradientFill (ColourGradient (Colours_::accentHi, 0, toY (0.0f), Colours_::accent.darker (0.4f), 0, r.getBottom(), false));
            g.fillRoundedRectangle (bar.withTop (yr), 3.0f);
            if (peakDb[(size_t) c] > 0.0f)
            {
                g.setColour (Colours_::warn);
                g.fillRect (bar.withHeight (4.0f));
            }

            auto txt = readouts.withX (bar.getX() - 6.0f).withWidth (barW + 12.0f);
            g.setFont (Fonts::label (Fonts::Size::micro, 0.12f));
            g.setColour (Colours_::textDim);
            g.drawText (c == 0 ? "L" : "R", txt.removeFromTop (14.0f), Justification::centred);
            g.setFont (Fonts::medium (12.5f));
            g.setColour (maxDb[(size_t) c] > 0.0f ? Colours_::warn : Colours_::text);
            g.drawText (maxDb[(size_t) c] <= -69.9f ? String ("-inf") : String (maxDb[(size_t) c], 1), txt, Justification::centred);
        }
    }

    const StereoHistory& history;
    std::array<float, 2> peakDb { -70.0f, -70.0f }, rms { -70.0f, -70.0f }, maxDb { -70.0f, -70.0f };
};

//==============================================================================
/** The SCOPE tab. */
class ScopePage : public juce::Component, private juce::Timer
{
public:
    /** Visible in the editor (works for off-screen snapshots too, unlike isShowing()). */
    bool isVisibleOnPage() const { return isVisible() && getParentComponent() != nullptr; }
    ScopePage (CodexProcessor& p, TunerAnalyser& tunerAnalyser)
        : proc (p), oscilloscope (p, history), stereo (history), spectrum (p, history), meters (history), tuner (tunerAnalyser)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &oscilloscope, &stereo, &spectrum, &meters, &tuner })
            addAndMakeVisible (c);
        pullL.resize (32768);
        pullR.resize (32768);
        startTimerHz (60);
    }

    const StereoHistory& getHistory() const { return history; }

    void paint (juce::Graphics& g) override
    {
        drawSectionPanel (g, *this, tunerArea, "TUNER", Colours_::good);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        oscilloscope.setBounds (r.removeFromTop (r.getHeight() * 46 / 100));
        r.removeFromTop (12);
        stereo.setBounds (r.removeFromLeft (r.getHeight() - 10));
        r.removeFromLeft (12);
        auto right = r.removeFromRight (220);
        r.removeFromRight (12);
        spectrum.setBounds (r);
        tunerArea = right.removeFromTop (138);
        tuner.setBounds (tunerArea.reduced (10).withTrimmedTop (30));
        right.removeFromTop (12);
        meters.setBounds (right);
    }

private:
    void timerCallback() override
    {
        // Always drain the FIFOs (even when hidden) so the data is fresh when the page is shown
        const int nl = proc.scopeFeedL.pull (pullL.data(), (int) pullL.size());
        const int nr = proc.scopeFeedR.pull (pullR.data(), (int) pullR.size());
        const int n = juce::jmin (nl, nr);
        history.push (pullL.data(), pullR.data(), n);
        if (! isVisibleOnPage()) return;
        oscilloscope.update();
        stereo.update (n);
        spectrum.update();
        meters.update (n);
    }

    CodexProcessor& proc;
    StereoHistory history;
    std::vector<float> pullL, pullR;
    OscilloscopePanel oscilloscope;
    StereoFieldPanel stereo;
    SpectrumPanel spectrum;
    StereoMetersPanel meters;
    TunerDisplay tuner;
    juce::Rectangle<int> tunerArea;
};
