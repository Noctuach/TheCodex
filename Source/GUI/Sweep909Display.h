#pragma once
#include "Widgets.h"
#include "../DSP/Kick909.h"

/**
    The 909 engine's pitch sweep, drawn from the model, Tune (speed) and Depth: semitones above the root over the
    first part of the hit, with the moment the sweep lands on the root marked.
*/
class Sweep909Display : public juce::Component, private juce::Timer
{
public:
    explicit Sweep909Display (juce::AudioProcessorValueTreeState& s)
        : model (s.getRawParameterValue (ID::model909)), time (s.getRawParameterValue (ID::tuneTime909)),
          depth (s.getRawParameterValue (ID::tune909))
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }

    /** Semitones above the root at t ms (the same law as KickVoice::render909). */
    static float semisAt (const Kick909Model& m, float timeParam, float depthParam, float tMs)
    {
        const float timeScale = std::exp2 ((timeParam - 0.5f) * 2.5f);
        const float depthScale = std::exp2 ((depthParam - 0.5f) * 2.0f);
        constexpr float floorLevel = 0.06f, floorNorm = 1.0f / (1.0f - floorLevel);
        const float fast = std::max (0.0f, (std::exp (-tMs / (m.fastMs * std::sqrt (timeScale))) - floorLevel) * floorNorm);
        const float slow = std::max (0.0f, (std::exp (-tMs / (m.slowMs * timeScale)) - floorLevel) * floorNorm);
        return depthScale * (m.sweepSemis * fast + m.slowSemis * slow);
    }

    static float landingMs (const Kick909Model& m, float timeParam)
    {
        const float timeScale = std::exp2 ((timeParam - 0.5f) * 2.5f);
        return -std::log (0.06f) * std::max (m.fastMs * std::sqrt (timeScale), m.slowMs * timeScale);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        const auto& m = kick909Models()[(size_t) jlimit (0, (int) kick909Models().size() - 1, (int) shown[0])];
        const float land = landingMs (m, shown[1]);
        const float spanMs = jlimit (60.0f, 400.0f, land * 1.25f);
        const float peak = semisAt (m, shown[1], shown[2], 0.0f);
        const float top = std::max (24.0f, std::ceil (peak / 12.0f) * 12.0f);

        auto plot = r.reduced (10.0f, 8.0f).withTrimmedLeft (34.0f).withTrimmedBottom (12.0f).withTrimmedTop (12.0f);
        auto X = [&] (float ms) { return plot.getX() + ms / spanMs * plot.getWidth(); };
        auto Y = [&] (float st) { return plot.getBottom() - st / top * plot.getHeight(); };

        // grid: octaves and time
        g.setFont (Fonts::label (10.0f, 0.06f));
        for (float st = 0.0f; st <= top + 0.1f; st += 12.0f)
        {
            g.setColour (Colours_::grid.brighter (st == 0.0f ? 0.6f : 0.25f));
            g.drawHorizontalLine (roundToInt (Y (st)), plot.getX(), plot.getRight());
            if (plot.getHeight() < 90.0f && st > 0.0f && st < top - 0.1f) continue;   // small display: root and top only
            g.setColour (Colours_::textFaint);
            g.drawText (st == 0.0f ? String ("ROOT") : "+" + String (roundToInt (st)), Rectangle<float> (r.getX() + 4.0f, Y (st) - 6.0f, 38.0f, 12.0f),
                        Justification::centredRight);
        }
        const float step = spanMs > 200.0f ? 50.0f : 25.0f;
        for (float t = step; t < spanMs; t += step)
        {
            g.setColour (Colours_::grid.brighter (0.25f));
            g.drawVerticalLine (roundToInt (X (t)), plot.getY(), plot.getBottom());
            g.setColour (Colours_::textFaint);
            g.drawText (String (roundToInt (t)) + " ms", Rectangle<float> (X (t) - 24.0f, plot.getBottom() + 1.0f, 48.0f, 12.0f), Justification::centred);
        }

        // the sweep
        Path p;
        for (int i = 0; i <= 160; ++i)
        {
            const float t = spanMs * (float) i / 160.0f;
            const Point<float> pt { X (t), Y (semisAt (m, shown[1], shown[2], t)) };
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        Path fill (p);
        fill.lineTo (X (spanMs), Y (0.0f));
        fill.lineTo (X (0.0f), Y (0.0f));
        fill.closeSubPath();
        g.setGradientFill (ColourGradient (Colours_::accent2.withAlpha (0.28f), 0, plot.getY(), Colours_::accent2.withAlpha (0.02f), 0, plot.getBottom(), false));
        g.fillPath (fill);
        Colours_::strokeGlow (g, p, Colours_::accent2, 1.8f);

        // landing marker
        if (land < spanMs)
        {
            g.setColour (Colours_::accent.withAlpha (0.7f));
            const float lx = X (land);
            for (float y = plot.getY(); y < plot.getBottom(); y += 6.0f)
                g.drawVerticalLine (roundToInt (lx), y, std::min (y + 3.0f, plot.getBottom()));
            g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre ({ lx, Y (0.0f) }));
        }
        g.setFont (Fonts::label (10.5f, 0.1f));
        g.setColour (Colours_::textDim);
        g.drawText ("+" + String (peak, 1) + " ST AT THE HIT   /   ON THE ROOT AFTER " + String (roundToInt (land)) + " MS",
                    plot.withHeight (14.0f).translated (0.0f, -12.0f), Justification::centredRight);
    }

private:
    void timerCallback() override
    {
        const std::array<float, 3> now { model->load(), time->load(), depth->load() };
        if (now != shown) { shown = now; repaint(); }
    }

    std::atomic<float> *model, *time, *depth;
    std::array<float, 3> shown { -1.0f, -1.0f, -1.0f };
};
