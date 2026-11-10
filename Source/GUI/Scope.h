#pragma once
#include "Widgets.h"
#include "../PluginProcessor.h"

/** Hit-synced oscilloscope: shows the first 500 ms of the most recent kick. */
class KickScope : public juce::Component, private juce::Timer
{
public:
    explicit KickScope (CodexProcessor& p) : proc (p) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        auto plot = r.reduced (8.0f, 6.0f).withTrimmedBottom (12.0f).withTrimmedTop (12.0f);

        // time grid: every 50 ms, labelled every 100 ms
        const float ms = (float) (CodexProcessor::scopeSeconds * 1000.0);
        g.setFont (Fonts::label (10.0f, 0.1f));
        for (int t = 50; t < (int) ms; t += 50)
        {
            const float x = plot.getX() + plot.getWidth() * (float) t / ms;
            g.setColour (t % 100 == 0 ? Colours_::grid.brighter (0.35f) : Colours_::grid);
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            if (t % 100 == 0)
            {
                g.setColour (Colours_::textFaint);
                g.drawText (String (t), Rectangle<float> (x - 15.0f, plot.getBottom() + 1.0f, 30.0f, 11.0f), Justification::centred);
            }
        }
        g.setColour (Colours_::grid.brighter (0.5f));
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());

        g.setColour (Colours_::textDim);
        g.drawText ("SCOPE", r.reduced (9.0f, 4.0f).removeFromTop (11.0f), Justification::centredLeft);
        g.setColour (Colours_::textFaint);
        g.drawText ("MS", r.reduced (9.0f, 4.0f).removeFromBottom (11.0f), Justification::centredRight);

        if (count < 2)
        {
            g.setColour (Colours_::textFaint);
            g.setFont (Fonts::regular (12.5f));
            g.drawText ("Play a note or press KICK", plot, Justification::centred);
            return;
        }

        // auto-scale to the loudest point so quiet kicks are still readable
        float peak = 0.05f;
        for (int i = 0; i < count; ++i) peak = jmax (peak, std::abs (mins[(size_t) i]), std::abs (maxs[(size_t) i]));
        const float scale = 0.95f / peak;

        Path wave;
        const int n = CodexProcessor::scopePoints;
        auto xAt = [&] (int i) { return plot.getX() + plot.getWidth() * (float) i / (float) (n - 1); };
        auto yAt = [&] (float v) { return plot.getCentreY() - v * scale * plot.getHeight() * 0.5f; };
        wave.startNewSubPath (xAt (0), yAt (maxs[0]));
        for (int i = 1; i < count; ++i) wave.lineTo (xAt (i), yAt (maxs[(size_t) i]));
        for (int i = count - 1; i >= 0; --i) wave.lineTo (xAt (i), yAt (mins[(size_t) i]));
        wave.closeSubPath();

        g.setColour (Colours_::accent.withAlpha (0.35f));
        g.fillPath (wave);
        Colours_::strokeGlow (g, wave, Colours_::accent, 1.0f);
    }

private:
    void timerCallback() override
    {
        const int gen = proc.scopeGeneration.load();
        const int written = proc.scopeWritten.load();
        if (gen == lastGeneration && written == count)
            return;
        lastGeneration = gen;
        count = juce::jlimit (0, CodexProcessor::scopePoints, written);
        std::copy (proc.scopeMin.begin(), proc.scopeMin.begin() + count, mins.begin());
        std::copy (proc.scopeMax.begin(), proc.scopeMax.begin() + count, maxs.begin());
        repaint();
    }

    CodexProcessor& proc;
    std::array<float, CodexProcessor::scopePoints> mins {}, maxs {};
    int count = 0, lastGeneration = -1;
};
