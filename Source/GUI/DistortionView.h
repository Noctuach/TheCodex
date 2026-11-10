#pragma once
#include "Widgets.h"
#include "../PluginProcessor.h"

/** Draws a rounded section panel with a header strip (shared by the editor and the distortion units). */
inline void drawSectionPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour dot)
{
    using namespace juce;
    const auto rf = r.toFloat();
    g.setGradientFill (ColourGradient (Colours_::panelTop, 0, rf.getY(), Colours_::panel, 0, rf.getBottom(), false));
    g.fillRoundedRectangle (rf, 8.0f);

    auto strip = rf.withHeight (26.0f);
    Path headerPath;
    headerPath.addRoundedRectangle (strip.getX(), strip.getY(), strip.getWidth(), strip.getHeight(), 8.0f, 8.0f, true, true, false, false);
    g.setColour (Colours_::header);
    g.fillPath (headerPath);
    g.setColour (Colours_::outline);
    g.drawHorizontalLine ((int) strip.getBottom(), strip.getX(), strip.getRight());
    g.drawRoundedRectangle (rf.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (dot.withAlpha (0.3f));
    g.fillEllipse (rf.getX() + 9.0f, strip.getCentreY() - 5.0f, 10.0f, 10.0f);
    g.setColour (dot);
    g.fillEllipse (rf.getX() + 11.0f, strip.getCentreY() - 3.0f, 6.0f, 6.0f);
    g.setColour (Colour (0xffb7c6e6));
    g.setFont (FontOptions (11.0f, Font::bold).withKerningFactor (0.12f));
    g.drawText (title, strip.withTrimmedLeft (26.0f), Justification::centredLeft);
}

/** Small caption drawn above a group of controls. */
inline void drawGroupCaption (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    using namespace juce;
    g.setColour (Colours_::textDim.withAlpha (0.8f));
    g.setFont (FontOptions (9.5f, Font::bold).withKerningFactor (0.15f));
    g.drawText (text, r, Justification::centredLeft);
    GlyphArrangement ga;
    ga.addLineOfText (g.getCurrentFont(), text, 0, 0);
    const float x = (float) r.getX() + ga.getBoundingBox (0, -1, true).getWidth() + 8.0f;
    g.setColour (Colours_::outline);
    g.drawHorizontalLine (r.getCentreY(), x, (float) r.getRight());
}

/** Live transfer curve (input -> output) of one distortion stage, with the current input level marked. */
class TransferDisplay : public juce::Component
{
public:
    TransferDisplay (juce::AudioProcessorValueTreeState& s, int stage)
    {
        auto p = [&] (const char* sfx) { return s.getRawParameterValue (ID::clip (stage, sfx)); };
        mode = p ("Mode"); drive = p ("Drive"); boost = p ("Boost"); offset = p ("Bias"); chr = p ("Char"); on = p ("On");
    }

    void setInputLevel (float linear) { level = linear > level ? linear : level * 0.88f; repaint(); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);

        auto plot = r.reduced (12.0f);
        const float size = jmin (plot.getWidth(), plot.getHeight());
        plot = plot.withSizeKeepingCentre (size, size);

        // grid
        g.setColour (Colours_::grid);
        for (int i = 1; i < 8; ++i)
        {
            const float t = (float) i / 8.0f;
            g.drawVerticalLine ((int) (plot.getX() + t * size), plot.getY(), plot.getBottom());
            g.drawHorizontalLine ((int) (plot.getY() + t * size), plot.getX(), plot.getRight());
        }
        g.setColour (Colours_::grid.brighter (0.6f));
        g.drawVerticalLine ((int) plot.getCentreX(), plot.getY(), plot.getBottom());
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());

        // unity reference
        Path diag;
        diag.startNewSubPath (plot.getX(), plot.getBottom());
        diag.lineTo (plot.getRight(), plot.getY());
        float dashes[] = { 4.0f, 4.0f };
        PathStrokeType (1.0f).createDashedStroke (diag, diag, dashes, 2);
        g.setColour (Colours_::textDim.withAlpha (0.35f));
        g.fillPath (diag);

        const bool enabled = on->load() > 0.5f;
        const int m = (int) mode->load();
        const float gain = Decibels::decibelsToGain (drive->load() + boost->load());
        const float off = offset->load(), c = chr->load();
        auto toPoint = [&] (float x, float y)
        {
            return Point<float> (jmap (x, -1.0f, 1.0f, plot.getX(), plot.getRight()),
                                 jmap (jlimit (-1.05f, 1.05f, y), -1.0f, 1.0f, plot.getBottom(), plot.getY()));
        };

        Path curve;
        const int steps = (int) size * 2;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = -1.0f + 2.0f * (float) i / (float) steps;
            const auto pt = toPoint (x, enabled ? ClipDistortion::transfer (m, x, gain, off, c) : x);
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        const auto col = enabled ? Colours_::accent : Colours_::textDim;
        Colours_::strokeGlow (g, curve, col, 2.0f);

        // current input level (pre-drive, with boost) marked on both sides of the curve
        const float lv = jlimit (0.0f, 1.0f, level);
        if (lv > 0.001f && enabled)
        {
            const float rawIn = lv / jmax (1.0e-6f, Decibels::decibelsToGain (boost->load()));
            for (float sgn : { -1.0f, 1.0f })
            {
                const float x = sgn * jlimit (0.0f, 1.0f, rawIn);
                const auto pt = toPoint (x, ClipDistortion::transfer (m, x, gain, off, c));
                g.setColour (Colours::white.withAlpha (0.25f));
                g.fillEllipse (Rectangle<float> (14.0f, 14.0f).withCentre (pt));
                g.setColour (Colours::white);
                g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (pt));
            }
        }

        g.setFont (FontOptions (9.5f, Font::bold).withKerningFactor (0.15f));
        g.setColour (Colours_::textDim);
        g.drawText ("IN", plot.removeFromBottom (12.0f).removeFromRight (20.0f), Justification::centredRight);
        g.drawText ("OUT", Rectangle<float> (plot.getX() + 4.0f, plot.getY(), 30.0f, 12.0f), Justification::centredLeft);
    }

private:
    std::atomic<float> *mode, *drive, *boost, *offset, *chr, *on;
    float level = 0.0f;
};

/** Full Distroyer-style distortion unit for one stage. */
class DistortionUnit : public juce::Component, private juce::Timer
{
public:
    DistortionUnit (HardKickProcessor& p, int stageNumber, const juce::String& panelTitle)
        : proc (p), stage (stageNumber), title (panelTitle), transfer (p.apvts, stageNumber)
    {
        auto& s = p.apvts;
        onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clip (stage, "On"), power);
        dcAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clip (stage, "DC"), dcButton);
        dcButton.setClickingTogglesState (true);
        dcButton.setColour (juce::TextButton::textColourOffId, Colours_::textDim);

        type.attach (s, ID::clip (stage, "Mode"));
        oversampling.attach (s, ID::clip (stage, "OS"));
        drive.attach (s, ID::clip (stage, "Drive"));
        boost.attach (s, ID::clip (stage, "Boost"));
        character.attach (s, ID::clip (stage, "Char"));
        offset.attach (s, ID::clip (stage, "Bias"));
        loFilter.attach (s, ID::clip (stage, "LoFilt"));
        hiFilter.attach (s, ID::clip (stage, "HiFilt"));
        shelfFreq.attach (s, ID::clip (stage, "ShelfF"));
        shelfGain.attach (s, ID::clip (stage, "ShelfG"));
        mix.attach (s, ID::clip (stage, "Mix"));
        output.attach (s, ID::clip (stage, "Out"));

        drive.label.setFont (juce::FontOptions (12.0f, juce::Font::bold).withKerningFactor (0.15f));
        drive.label.setColour (juce::Label::textColourId, Colours_::accent);

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &power, &dcButton, &type, &oversampling, &transfer, &drive, &boost, &character, &offset,
                 &loFilter, &hiFilter, &shelfFreq, &shelfGain, &mix, &output, &inMeter, &outMeter })
            addAndMakeVisible (c);

        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionPanel (g, getLocalBounds(), title, Colours_::accent);
        drawGroupCaption (g, shapeCaption, "SHAPE");
        drawGroupCaption (g, filterCaption, "WET FILTERS  /  TONE");
        drawGroupCaption (g, outputCaption, "OUTPUT");

        g.setColour (Colours_::textDim);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold).withKerningFactor (0.15f));
        auto labelUnder = [] (juce::Rectangle<int> m) { return m.withSizeKeepingCentre (40, m.getHeight()).translated (0, m.getHeight() + 2).withHeight (12); };
        g.drawText ("IN", labelUnder (inMeter.getBounds()), juce::Justification::centred);
        g.drawText ("OUT", labelUnder (outMeter.getBounds()), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        power.setBounds (r.withHeight (26).removeFromRight (30).reduced (4));

        auto c = r.reduced (12).withTrimmedTop (24);

        // Top row: type, oversampling, DC filter
        auto top = c.removeFromTop (46);
        type.setBounds (top.removeFromLeft (170));
        top.removeFromLeft (10);
        oversampling.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (10);
        dcButton.setBounds (top.removeFromLeft (64).withTrimmedTop (16).withHeight (26));
        c.removeFromTop (8);

        // Middle: transfer display | drive + shape knobs
        auto middle = c.removeFromTop (juce::jmin (250, c.getHeight() - 220));
        transfer.setBounds (middle.removeFromLeft (middle.getHeight() + 20));
        middle.removeFromLeft (12);
        auto driveArea = middle.removeFromTop (middle.getHeight() - 96);
        drive.setBounds (driveArea.withSizeKeepingCentre (juce::jmin (driveArea.getWidth(), driveArea.getHeight() + 10), driveArea.getHeight()));
        shapeCaption = middle.removeFromTop (14);
        const int w3 = middle.getWidth() / 3;
        boost.setBounds (middle.removeFromLeft (w3));
        character.setBounds (middle.removeFromLeft (w3));
        offset.setBounds (middle);

        c.removeFromTop (12);
        filterCaption = c.removeFromTop (14);
        auto filters = c.removeFromTop (92);
        const int w4 = filters.getWidth() / 4;
        loFilter.setBounds (filters.removeFromLeft (w4));
        hiFilter.setBounds (filters.removeFromLeft (w4));
        shelfFreq.setBounds (filters.removeFromLeft (w4));
        shelfGain.setBounds (filters);

        c.removeFromTop (12);
        outputCaption = c.removeFromTop (14);
        auto out = c.removeFromTop (92);
        inMeter.setBounds (out.removeFromLeft (14).withTrimmedBottom (16));
        outMeter.setBounds (out.removeFromRight (14).withTrimmedBottom (16));
        out.reduce (10, 0);
        mix.setBounds (out.removeFromLeft (out.getWidth() / 2));
        output.setBounds (out);
    }

private:
    void timerCallback() override
    {
        auto& clip = proc.getClip (stage);
        const float in = clip.getAndResetInputPeak();
        inMeter.setLevel (in);
        outMeter.setLevel (clip.getAndResetOutputPeak());
        transfer.setInputLevel (in);
    }

    HardKickProcessor& proc;
    int stage;
    juce::String title;

    PowerButton power;
    juce::TextButton dcButton { "DC FILTER" };
    ChoiceBox type { "Type" }, oversampling { "Oversampling" };
    TransferDisplay transfer;
    Knob drive { "Drive" }, boost { "Boost" }, character { "Characteristic" }, offset { "Offset" };
    Knob loFilter { "Lo Filter" }, hiFilter { "Hi Filter" }, shelfFreq { "Shelf Freq" }, shelfGain { "Shelf Gain" };
    Knob mix { "Mix" }, output { "Output" };
    LevelMeter inMeter, outMeter;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment, dcAttachment;
    juce::Rectangle<int> shapeCaption, filterCaption, outputCaption;
};
