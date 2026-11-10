#pragma once
#include "Widgets.h"
#include "Help.h"
#include "../PluginProcessor.h"

/**
    Decay display for the Space tab: reverb time (RT60) across the spectrum, derived from
    Decay, the Decay EQ multipliers/crossovers and the mode, with the Low/High Cut regions shaded.
    Drag the bass / high handles: horizontally = crossover, vertically = multiplier.
*/
class SpaceDecayDisplay : public juce::Component, private juce::Timer
{
public:
    explicit SpaceDecayDisplay (juce::AudioProcessorValueTreeState& s) : apvts (s) { startTimerHz (60); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);
        plot = b.reduced (12.0f, 10.0f).withTrimmedBottom (14.0f).withTrimmedLeft (26.0f);

        const bool on = v ("On") > 0.5f;
        const float maxT = maxTime();

        // grid
        g.setFont (Fonts::regular (10.5f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float x = fx (f);
            g.setColour (Colours_::grid);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (Colours_::textFaint);
            g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                        juce::Rectangle<float> (x - 20, plot.getBottom() + 1, 40, 12), juce::Justification::centred);
        }
        for (int i = 1; i <= 4; ++i)
        {
            const float t = maxT * (float) i / 4.0f, y = ty (t);
            g.setColour (Colours_::grid);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (Colours_::textFaint);
            g.drawText (juce::String (t, t < 10 ? 1 : 0) + "s", juce::Rectangle<float> (plot.getX() - 28, y - 6, 25, 12), juce::Justification::centredRight);
        }

        // filtered-out regions (Low Cut / High Cut)
        g.setColour (Colours_::background.withAlpha (0.55f));
        const float lx = fx (v ("LowCut")), hx = fx (v ("HighCut"));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plot.getX(), plot.getY(), lx, plot.getBottom()));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (hx, plot.getY(), plot.getRight(), plot.getBottom()));

        // decay curve
        juce::Path curve, fill;
        for (float x = plot.getX(); x <= plot.getRight(); x += 1.0f)
        {
            const float y = ty (rt60 (xf (x)));
            if (x <= plot.getX()) { curve.startNewSubPath (x, y); fill.startNewSubPath (x, plot.getBottom()); fill.lineTo (x, y); }
            else { curve.lineTo (x, y); fill.lineTo (x, y); }
        }
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.closeSubPath();
        const auto col = on ? Colours_::accent : Colours_::textFaint;
        g.setGradientFill (juce::ColourGradient (col.withAlpha (0.28f), 0, plot.getY(), col.withAlpha (0.02f), 0, plot.getBottom(), false));
        g.fillPath (fill);
        g.setColour (col);
        g.strokePath (curve, juce::PathStrokeType (1.8f));

        // handles
        for (int h = 0; h < 2; ++h)
        {
            const auto p = handle (h);
            const bool hi = h == hover || h == dragging;
            g.setColour (col.withAlpha (hi ? 0.35f : 0.18f));
            g.fillEllipse (juce::Rectangle<float> (22, 22).withCentre (p));
            g.setColour (col);
            g.fillEllipse (juce::Rectangle<float> (10, 10).withCentre (p));
            g.setColour (Colours_::textDim);
            g.setFont (Fonts::label (10.5f, 0.12f));
            g.drawText (h == 0 ? "BASS" : "HIGH", juce::Rectangle<float> (60, 12).withCentre (p.translated (0, -16)), juce::Justification::centred);
        }

        // readout
        g.setColour (Colours_::text);
        g.setFont (Fonts::medium (13.0f));
        g.drawText ("RT60  " + juce::String (v ("Decay"), 2) + " s", plot.withHeight (18).reduced (8, 0), juce::Justification::centredRight);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h != hover) { hover = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = hit (e.position);
        if (dragging < 0) return;
        for (auto* p : params (dragging)) p->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        auto ps = params (dragging);
        const float f = juce::jlimit (20.0f, 20000.0f, xf (e.position.x));
        ps[0]->setValueNotifyingHost (ps[0]->convertTo0to1 (f));
        // vertical: multiplier relative to the mid decay
        const float t = juce::jmax (0.01f, yt (e.position.y));
        ps[1]->setValueNotifyingHost (ps[1]->convertTo0to1 (t / juce::jmax (0.05f, v ("Decay") * (dragging == 1 ? damping() : 1.0f))));
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) for (auto* p : params (dragging)) p->endChangeGesture();
        dragging = -1;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h < 0) return;
        for (auto* p : params (h)) { p->beginChangeGesture(); p->setValueNotifyingHost (p->getDefaultValue()); p->endChangeGesture(); }
    }

private:
    void timerCallback() override
    {
        float h = 0.0f;
        for (auto* id : { "On", "Mode", "Decay", "LowCut", "HighCut", "BassMult", "BassX", "HighMult", "HighX" })
            h = h * 1.37f + v (id);
        if (! juce::exactlyEqual (h, lastHash)) { lastHash = h; repaint(); }
    }

    float v (const char* sfx) const { return apvts.getRawParameterValue (ID::spc (sfx))->load(); }
    float damping() const { return space::modeShape ((int) v ("Mode")).damping; }

    /** Approximate RT60 at frequency f, matching the 3-band decay filter in the reverb. */
    float rt60 (float f) const
    {
        const float bx = v ("BassX"), hx = v ("HighX");
        const float wLow = 1.0f / std::sqrt (1.0f + (f / bx) * (f / bx));
        const float r = f / hx, wHigh = r / std::sqrt (1.0f + r * r);
        const float wMid = juce::jmax (0.0f, 1.0f - wLow - wHigh);
        const float sum = wLow + wMid + wHigh;
        const float mult = (wLow * v ("BassMult") + wMid + wHigh * v ("HighMult") * damping()) / sum;
        return v ("Decay") * mult;
    }
    float maxTime() const
    {
        const float d = v ("Decay");
        const float m = juce::jmax (1.0f, v ("BassMult"), v ("HighMult") * damping());
        const float t = d * m * 1.15f;
        for (float step : { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 80.0f })
            if (t <= step) return step;
        return 80.0f;
    }
    float fx (float f) const { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float xf (float x) const { return 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth()); }
    float ty (float t) const { return juce::jmap (juce::jlimit (0.0f, maxTime(), t), 0.0f, maxTime(), plot.getBottom(), plot.getY() + 6.0f); }
    float yt (float y) const { return juce::jmap (y, plot.getBottom(), plot.getY() + 6.0f, 0.0f, maxTime()); }

    juce::Point<float> handle (int h) const
    {
        const float fh = h == 0 ? v ("BassX") : v ("HighX");
        return { fx (fh), ty (rt60 (h == 0 ? fh * 0.35f : fh * 2.8f)) };
    }
    int hit (juce::Point<float> p) const
    {
        for (int h = 0; h < 2; ++h)
            if (handle (h).getDistanceFrom (p) < 14.0f) return h;
        return -1;
    }
    std::array<juce::RangedAudioParameter*, 2> params (int h)
    {
        return { apvts.getParameter (ID::spc (h == 0 ? "BassX" : "HighX")), apvts.getParameter (ID::spc (h == 0 ? "BassMult" : "HighMult")) };
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::Rectangle<float> plot;
    int hover = -1, dragging = -1;
    float lastHash = -1.0f;
};

/** The SPACE tab: a stereo room reverb modelled on ValhallaRoom's layout. */
class SpacePage : public juce::Component, private juce::Timer
{
public:
    explicit SpacePage (CodexProcessor& p) : apvts (p.apvts), decay (p.apvts)
    {
        power = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::spc ("On"), powerButton);
        powerButton.setTooltip (Help::forParameter (ID::spc ("On")));
        mode.attach (apvts, ID::spc ("Mode"));

        const std::pair<Knob*, const char*> all[] = {
            { &mix, "Mix" }, { &predelay, "PreDelay" }, { &decayKnob, "Decay" }, { &depth, "Depth" },
            { &highCut, "HighCut" }, { &lowCut, "LowCut" }, { &width, "Width" }, { &duck, "Duck" },
            { &eSize, "ESize" }, { &eCross, "ECross" }, { &eSend, "ESend" }, { &eDiff, "EDiff" }, { &eRate, "EModRate" }, { &eDepth, "EModDepth" },
            { &lSize, "LSize" }, { &lCross, "LCross" }, { &lDiff, "LDiff" }, { &lRate, "LModRate" }, { &lDepth, "LModDepth" },
            { &bassMult, "BassMult" }, { &bassX, "BassX" }, { &highMult, "HighMult" }, { &highX, "HighX" } };
        for (auto& [k, id] : all)
        {
            k->attach (apvts, ID::spc (id));
            addAndMakeVisible (*k);
        }
        decayKnob.label.setColour (juce::Label::textColourId, Colours_::accent);
        mix.label.setColour (juce::Label::textColourId, Colours_::accent);

        notice.title = "SPACE IS OFF";
        notice.subtitle = "Turn on the room reverb to add width and ambience.";
        notice.enable.onClick = [this]
        {
            if (auto* prm = apvts.getParameter (ID::spc ("On")))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (1.0f);
                prm->endChangeGesture();
            }
        };
        for (juce::Component* c : std::initializer_list<juce::Component*> { &powerButton, &mode, &decay })
            addAndMakeVisible (c);
        addChildComponent (notice);
        onParam = apvts.getRawParameterValue (ID::spc ("On"));
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        const auto acc = off ? Colours_::textFaint : Colours_::accent;
        drawSectionPanel (g, mainArea, juce::String::fromUTF8 ("SPACE  \xc2\xb7  ROOM REVERB"), acc);
        drawSectionPanel (g, earlyArea, "EARLY", acc);
        drawSectionPanel (g, lateArea, "LATE", acc);
        drawSectionPanel (g, eqArea, "DECAY EQ", acc);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        mainArea = r.removeFromTop (r.getHeight() * 53 / 100);
        r.removeFromTop (12);
        const int unit = (r.getWidth() - 24) / 8;
        earlyArea = r.removeFromLeft (unit * 3);
        r.removeFromLeft (12);
        lateArea = r.removeFromLeft (unit * 3);
        r.removeFromLeft (12);
        eqArea = r;

        powerButton.setBounds (mainArea.withHeight (28).removeFromRight (32).reduced (4));
        {
            auto c = mainArea.reduced (14).withTrimmedTop (30);
            auto knobs = c.removeFromRight (juce::jmin (470, c.getWidth() / 2));
            c.removeFromRight (16);
            decay.setBounds (c);
            mode.setBounds (knobs.removeFromTop (48).withTrimmedRight (knobs.getWidth() / 2));
            knobs.removeFromTop (6);
            grid ({ &mix, &predelay, &decayKnob, &depth, &highCut, &lowCut, &width, &duck }, knobs, 4);
        }
        grid ({ &eSize, &eCross, &eSend, &eDiff, &eRate, &eDepth }, earlyArea.reduced (10).withTrimmedTop (30), 3);
        grid ({ &lSize, &lCross, &lDiff, &lRate, &lDepth }, lateArea.reduced (10).withTrimmedTop (30), 3);
        grid ({ &bassMult, &bassX, &highMult, &highX }, eqArea.reduced (10).withTrimmedTop (30), 2);

        notice.setBounds (getLocalBounds().withTrimmedTop (29).reduced (1));
    }

private:
    static void grid (std::initializer_list<juce::Component*> items, juce::Rectangle<int> area, int cols)
    {
        const int rows = ((int) items.size() + cols - 1) / cols;
        const int w = area.getWidth() / cols, h = juce::jmin (110, area.getHeight() / rows);
        int i = 0;
        for (auto* c : items)
        {
            c->setBounds (area.getX() + (i % cols) * w, area.getY() + (i / cols) * h, w, h);
            ++i;
        }
    }

    void timerCallback() override
    {
        const bool o = onParam->load() < 0.5f;
        if (o != off)
        {
            off = o;
            notice.setVisible (off);
            repaint();
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    PowerButton powerButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> power;
    ChoiceBox mode { "Mode" };
    SpaceDecayDisplay decay;
    Knob mix { "Mix" }, predelay { "Predelay" }, decayKnob { "Decay" }, depth { "Depth" },
         highCut { "High Cut" }, lowCut { "Low Cut" }, width { "Width" }, duck { "Ducking" },
         eSize { "Size" }, eCross { "Cross" }, eSend { "Send" }, eDiff { "Diffusion" }, eRate { "Mod Rate" }, eDepth { "Mod Depth" },
         lSize { "Size" }, lCross { "Cross" }, lDiff { "Diffusion" }, lRate { "Mod Rate" }, lDepth { "Mod Depth" },
         bassMult { "Bass Mult" }, bassX { "Bass Xover" }, highMult { "High Mult" }, highX { "High Xover" };
    BypassNotice notice;
    std::atomic<float>* onParam = nullptr;
    bool off = false;
    juce::Rectangle<int> mainArea, earlyArea, lateArea, eqArea;
};
