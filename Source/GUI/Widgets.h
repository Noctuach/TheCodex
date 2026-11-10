#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "../DSP/CurveEnvelope.h"

/** Rotary knob with a caption above and value read-out below, bound to a parameter. */
class Knob : public juce::Component
{
public:
    explicit Knob (const juce::String& caption)
    {
        label.setText (caption.toUpperCase(), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Colours_::textDim);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 16);
        slider.setColour (juce::Slider::textBoxTextColourId, Colours_::text);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (label);
        addAndMakeVisible (slider);
    }

    void attach (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        attachment.reset();
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (14));
        slider.setBounds (r);
    }

    juce::Label label;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** ComboBox with a caption, populated from an AudioParameterChoice. */
class ChoiceBox : public juce::Component
{
public:
    explicit ChoiceBox (const juce::String& caption)
    {
        label.setText (caption.toUpperCase(), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Colours_::textDim);
        addAndMakeVisible (label);
        addAndMakeVisible (box);
    }

    void attach (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        attachment.reset();
        if (box.getNumItems() == 0)
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (id)))
                box.addItemList (c->choices, 1);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (s, id, box);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (14));
        box.setBounds (r.withSizeKeepingCentre (r.getWidth(), juce::jmin (24, r.getHeight())));
    }

    juce::Label label;
    juce::ComboBox box;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

/** Draws the actual ADSR shape (with curve) from live parameter values. */
class EnvelopeDisplay : public juce::Component, private juce::Timer
{
public:
    EnvelopeDisplay (juce::AudioProcessorValueTreeState& s, const juce::String& a, const juce::String& d,
                     const juce::String& sus, const juce::String& r, const juce::String& curve, juce::Colour c)
        : colour (c)
    {
        params = { s.getRawParameterValue (a), s.getRawParameterValue (d), s.getRawParameterValue (sus),
                   s.getRawParameterValue (r), s.getRawParameterValue (curve) };
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);

        // faint grid
        g.setColour (Colours_::grid);
        for (int i = 1; i < 8; ++i)
            g.drawVerticalLine ((int) (area.getX() + area.getWidth() * (float) i / 8.0f), area.getY() + 1, area.getBottom() - 1);
        for (int i = 1; i < 4; ++i)
            g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * (float) i / 4.0f), area.getX() + 1, area.getRight() - 1);

        area = area.reduced (8.0f, 8.0f);

        const float A = cached[0], D = cached[1], S = cached[2], R = cached[3], C = cached[4];
        // sqrt time scaling keeps short and long stages readable at once
        const float wa = std::sqrt (A), wd = std::sqrt (D), wr = std::sqrt (R);
        const float ws = (wa + wd + wr) * 0.25f + 1.0f;
        const float total = wa + wd + ws + wr;
        const float W = area.getWidth(), H = area.getHeight();
        auto X = [&] (float u) { return area.getX() + u / total * W; };
        auto Y = [&] (float lvl) { return area.getBottom() - lvl * H; };

        juce::Path p;
        p.startNewSubPath (X (0), Y (0));
        p.lineTo (X (wa), Y (1.0f));
        constexpr int steps = 48;
        for (int i = 1; i <= steps; ++i)
        {
            const float t = (float) i / steps;
            p.lineTo (X (wa + t * wd), Y (S + (1.0f - S) * CurveEnvelope::shapeFor (1.0f - t, C)));
        }
        p.lineTo (X (wa + wd + ws), Y (S));
        for (int i = 1; i <= steps; ++i)
        {
            const float t = (float) i / steps;
            p.lineTo (X (wa + wd + ws + t * wr), Y (S * CurveEnvelope::shapeFor (1.0f - t, C)));
        }

        juce::Path fill (p);
        fill.lineTo (X (total), Y (0));
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.35f), 0, area.getY(),
                                                 colour.withAlpha (0.02f), 0, area.getBottom(), false));
        g.fillPath (fill);
        Colours_::strokeGlow (g, p, colour, 1.8f);

        // Note-off marker
        g.setColour (Colours_::textDim.withAlpha (0.35f));
        const float noteOffX = X (wa + wd + ws);
        g.drawVerticalLine ((int) noteOffX, area.getY(), area.getBottom());
    }

private:
    void timerCallback() override
    {
        bool changed = false;
        for (size_t i = 0; i < params.size(); ++i)
        {
            const float v = params[i]->load();
            if (v != cached[i]) { cached[i] = v; changed = true; }
        }
        if (changed) repaint();
    }

    juce::Colour colour;
    std::array<std::atomic<float>*, 5> params {};
    std::array<float, 5> cached { -1, -1, -1, -1, -1 };
};

/** Simple vertical peak meter. */
class LevelMeter : public juce::Component
{
public:
    void setLevel (float linearPeak)
    {
        const float db = juce::Decibels::gainToDecibels (linearPeak, -60.0f);
        level = db > level ? db : juce::jmax (-60.0f, level - 1.5f);
        if (db > hold) { hold = db; holdTimer = 45; }
        else if (--holdTimer <= 0) hold = juce::jmax (-60.0f, hold - 1.0f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 3.0f);
        auto toY = [&] (float db) { return juce::jmap (db, -60.0f, 6.0f, r.getBottom(), r.getY()); };
        const float y = toY (juce::jmin (level, 6.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffbfe9ff), 0, toY (0.0f), Colours_::accent.darker (0.4f), 0, r.getBottom(), false));
        g.fillRect (r.withTop (y).reduced (2.0f, 0.0f));
        g.setColour (hold > 0.0f ? juce::Colour (0xffff4d6d) : Colours_::text);
        g.fillRect (r.getX() + 2.0f, toY (juce::jmin (hold, 6.0f)), r.getWidth() - 4.0f, 2.0f);
        g.setColour (Colours_::textDim.withAlpha (0.6f));
        g.drawHorizontalLine ((int) toY (0.0f), r.getX(), r.getRight());
    }

private:
    float level = -60.0f, hold = -60.0f;
    int holdTimer = 0;
};

/** Momentary button: fires onPress(true) on mouse down and onPress(false) on mouse up. */
class TriggerButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    std::function<void (bool)> onPress;
    void mouseDown (const juce::MouseEvent& e) override { TextButton::mouseDown (e); if (onPress) onPress (true); }
    void mouseUp (const juce::MouseEvent& e) override   { TextButton::mouseUp (e);   if (onPress) onPress (false); }
};

/** Round power toggle (section on/off), glowing when enabled. */
class PowerButton : public juce::ToggleButton
{
public:
    PowerButton() { setClickingTogglesState (true); }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat();
        const float d = juce::jmin (r.getWidth(), r.getHeight()) - 2.0f;
        auto c = r.withSizeKeepingCentre (d, d);
        const bool on = getToggleState();
        const auto col = on ? Colours_::accent : Colours_::textDim.withAlpha (over ? 0.9f : 0.6f);

        if (on)
        {
            g.setColour (col.withAlpha (0.18f));
            g.fillEllipse (c.expanded (1.0f));
        }
        g.setColour (Colours_::display);
        g.fillEllipse (c.reduced (1.5f));
        g.setColour (col);
        g.drawEllipse (c.reduced (1.5f), 1.2f);

        const auto ctr = c.getCentre();
        const float ir = d * 0.24f;
        juce::Path icon;
        icon.addCentredArc (ctr.x, ctr.y, ir, ir, 0.0f, 0.7f, juce::MathConstants<float>::twoPi - 0.7f, true);
        icon.startNewSubPath (ctr.x, ctr.y - ir * 1.35f);
        icon.lineTo (ctr.x, ctr.y - ir * 0.2f);
        g.strokePath (icon, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};
