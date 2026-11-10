#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "Help.h"
#include "../DSP/CurveEnvelope.h"

/** Common behaviour for every parameter slider: tooltip, double-click to reset, Shift-drag for fine control. */
inline void configureParameterSlider (juce::Slider& slider, juce::AudioProcessorValueTreeState& s, const juce::String& id)
{
    slider.setTooltip (Help::forParameter (id));
    if (auto* p = s.getParameter (id))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    slider.setVelocityModeParameters (0.35, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    slider.setScrollWheelEnabled (true);
}

/** Rotary knob with a caption above and value read-out below, bound to a parameter. */
class Knob : public juce::Component
{
public:
    explicit Knob (const juce::String& caption)
    {
        label.setText (caption.toUpperCase(), juce::dontSendNotification);
        label.getProperties().set ("caption", true);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Colours_::textDim);
        label.setInterceptsMouseClicks (false, false);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
        slider.setColour (juce::Slider::textBoxTextColourId, Colours_::text);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        slider.setMouseDragSensitivity (220);
        addAndMakeVisible (label);
        addAndMakeVisible (slider);
    }

    void attach (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        attachment.reset();
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
        configureParameterSlider (slider, s, id);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (16));
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
        label.getProperties().set ("caption", true);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setColour (juce::Label::textColourId, Colours_::textDim);
        label.setInterceptsMouseClicks (false, false);
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
        box.setTooltip (Help::forParameter (id));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (16).withTrimmedLeft (1));
        r.removeFromTop (2);
        box.setBounds (r.removeFromTop (juce::jmin (28, r.getHeight())));
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
        g.fillRoundedRectangle (area, 6.0f);

        g.setColour (Colours_::grid);
        for (int i = 1; i < 8; ++i)
            g.drawVerticalLine ((int) (area.getX() + area.getWidth() * (float) i / 8.0f), area.getY() + 1, area.getBottom() - 1);
        for (int i = 1; i < 4; ++i)
            g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * (float) i / 4.0f), area.getX() + 1, area.getRight() - 1);

        // inner shadow at the top for depth
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.35f), 0, area.getY(),
                                                 juce::Colours::transparentBlack, 0, area.getY() + 10.0f, false));
        g.fillRoundedRectangle (area.withHeight (10.0f), 6.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 1.0f);

        area = area.reduced (9.0f, 9.0f);

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
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.32f), 0, area.getY(),
                                                 colour.withAlpha (0.02f), 0, area.getBottom(), false));
        g.fillPath (fill);
        Colours_::strokeGlow (g, p, colour, 1.8f);

        // Note-off marker
        const float noteOffX = X (wa + wd + ws);
        g.setColour (Colours_::textFaint);
        g.drawVerticalLine ((int) noteOffX, area.getY(), area.getBottom());
        g.setFont (Fonts::label (10.0f, 0.15f));
        g.drawText ("NOTE OFF", juce::Rectangle<float> (noteOffX + 4.0f, area.getY() - 3.0f, 60.0f, 12.0f), juce::Justification::centredLeft);
    }

private:
    void timerCallback() override
    {
        bool changed = false;
        for (size_t i = 0; i < params.size(); ++i)
        {
            const float v = params[i]->load();
            if (! juce::exactlyEqual (v, cached[i])) { cached[i] = v; changed = true; }
        }
        if (changed) repaint();
    }

    juce::Colour colour;
    std::array<std::atomic<float>*, 5> params {};
    std::array<float, 5> cached { -1, -1, -1, -1, -1 };
};

/** Vertical peak meter with hold marker (red above 0 dBFS). */
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
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        r = r.reduced (2.5f);
        auto toY = [&] (float db) { return juce::jmap (db, -60.0f, 6.0f, r.getBottom(), r.getY()); };

        // tick marks every 12 dB
        g.setColour (Colours_::grid.brighter (0.3f));
        for (float db = -48.0f; db <= 0.0f; db += 12.0f)
            g.drawHorizontalLine ((int) toY (db), r.getX(), r.getRight());

        const float y = toY (juce::jmin (level, 6.0f));
        g.setGradientFill (juce::ColourGradient (Colours_::accentHi, 0, toY (0.0f), Colours_::accent.darker (0.5f), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r.withTop (y), 1.5f);
        if (level > 0.0f)
        {
            g.setColour (Colours_::warn);
            g.fillRect (r.withTop (y).withBottom (toY (0.0f)));
        }
        g.setColour (hold > 0.0f ? Colours_::warn : Colours_::text);
        g.fillRect (r.getX(), toY (juce::jmin (hold, 6.0f)) - 1.0f, r.getWidth(), 2.0f);
        g.setColour (Colours_::textDim.withAlpha (0.7f));
        g.drawHorizontalLine ((int) toY (0.0f), r.getX() - 2.0f, r.getRight() + 2.0f);
    }

private:
    float level = -60.0f, hold = -60.0f;
    int holdTimer = 0;
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
        const auto col = on ? Colours_::accent : Colours_::textDim.withAlpha (over ? 0.95f : 0.6f);

        if (on)
        {
            g.setColour (col.withAlpha (0.16f));
            g.fillEllipse (c.expanded (1.5f));
        }
        g.setColour (Colours_::display);
        g.fillEllipse (c.reduced (1.5f));
        g.setColour (on ? col : (over ? Colours_::outlineHi : Colours_::outline));
        g.drawEllipse (c.reduced (1.5f), 1.2f);
        Icons::draw (g, Icons::power(), c.reduced (d * 0.27f), col, 1.5f);
    }
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

/** Rounded section panel with a header strip and soft drop shadow (shared by all views). */
inline void drawSectionPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour dot)
{
    using namespace juce;
    const auto rf = r.toFloat();
    for (int i = 3; i > 0; --i)
    {
        g.setColour (Colours::black.withAlpha (0.09f));
        g.fillRoundedRectangle (rf.expanded ((float) i * 1.5f).translated (0.0f, (float) i), 10.0f);
    }
    g.setGradientFill (ColourGradient (Colours_::panelTop, 0, rf.getY(), Colours_::panel, 0, rf.getBottom(), false));
    g.fillRoundedRectangle (rf, 8.0f);

    auto strip = rf.withHeight (28.0f);
    Path headerPath;
    headerPath.addRoundedRectangle (strip.getX(), strip.getY(), strip.getWidth(), strip.getHeight(), 8.0f, 8.0f, true, true, false, false);
    g.setGradientFill (ColourGradient (Colours_::header.brighter (0.06f), 0, strip.getY(), Colours_::header, 0, strip.getBottom(), false));
    g.fillPath (headerPath);
    g.setColour (Colours_::outline);
    g.drawHorizontalLine ((int) strip.getBottom(), strip.getX(), strip.getRight());
    g.setColour (Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine ((int) strip.getY() + 1, strip.getX() + 8.0f, strip.getRight() - 8.0f);
    g.setColour (Colours_::outline);
    g.drawRoundedRectangle (rf.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (dot.withAlpha (0.25f));
    g.fillRoundedRectangle (rf.getX() + 10.0f, strip.getCentreY() - 6.0f, 3.0f, 12.0f, 1.5f);
    g.setColour (dot);
    g.fillRoundedRectangle (rf.getX() + 10.0f, strip.getCentreY() - 4.0f, 3.0f, 8.0f, 1.5f);
    g.setColour (Colour (0xffc3d0ee));
    g.setFont (Fonts::label (13.5f, 0.16f));
    g.drawText (title, strip.withTrimmedLeft (22.0f), Justification::centredLeft);
}

/** Small caption with a rule, drawn above a group of controls. */
inline void drawGroupCaption (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    using namespace juce;
    g.setColour (Colours_::textDim);
    const auto font = Font (Fonts::label (11.5f, 0.18f));
    g.setFont (font);
    g.drawText (text, r, Justification::centredLeft);
    GlyphArrangement ga;
    ga.addLineOfText (font, text, 0, 0);
    const float x = (float) r.getX() + ga.getBoundingBox (0, -1, true).getWidth() + 10.0f;
    g.setColour (Colours_::outline);
    g.drawHorizontalLine (r.getCentreY(), x, (float) r.getRight());
}
