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
class EnvelopeDisplay : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    EnvelopeDisplay (juce::AudioProcessorValueTreeState& s, const juce::String& a, const juce::String& d,
                     const juce::String& sus, const juce::String& r, const juce::String& curve, juce::Colour c)
        : colour (c)
    {
        params = { s.getRawParameterValue (a), s.getRawParameterValue (d), s.getRawParameterValue (sus),
                   s.getRawParameterValue (r), s.getRawParameterValue (curve) };
        rangedParams = { s.getParameter (a), s.getParameter (d), s.getParameter (sus), s.getParameter (r), s.getParameter (curve) };
        setTooltip ("Drag the points: attack (left peak), decay + sustain (corner), release (end), curve (middle of the decay). "
                    "Shift: fine. Double-click a point: reset.");
        startTimerHz (60);
    }

    // ---- direct editing: drag the envelope's points ----
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hitNode (e.position);
        if (h != hover) { hover = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = hitNode (e.position);
        if (dragging < 0) return;
        lastPos = e.position;
        for (int i : nodeParams (dragging)) rangedParams[(size_t) i]->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
        const auto d = (e.position - lastPos) * fine;
        lastPos = e.position;
        auto nudge = [this] (int i, float delta)
        {
            auto* p = rangedParams[(size_t) i];
            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->getValue() + delta));
        };
        const float w = juce::jmax (40.0f, inner().getWidth()), h = juce::jmax (20.0f, inner().getHeight());
        switch (dragging)
        {
            case 0: nudge (0, d.x / w * 1.2f); break;                         // attack time
            case 1: nudge (1, d.x / w * 1.2f); nudge (2, -d.y / h); break;     // decay time + sustain level
            case 2: nudge (3, d.x / w * 1.2f); break;                         // release time
            case 3: nudge (4, -d.y / h * 1.5f); break;                         // curve
            default: break;
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            for (int i : nodeParams (dragging)) rangedParams[(size_t) i]->endChangeGesture();
        dragging = -1;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int n = hitNode (e.position);
        if (n < 0) return;
        for (int i : nodeParams (n))
        {
            auto* p = rangedParams[(size_t) i];
            p->beginChangeGesture(); p->setValueNotifyingHost (p->getDefaultValue()); p->endChangeGesture();
        }
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

        area = inner();

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

        // draggable points
        for (int n = 0; n < 4; ++n)
        {
            const auto pt = nodes[(size_t) n];
            const bool hi = n == hover || n == dragging;
            const float r = n == 3 ? 3.5f : 4.5f;
            if (hi) { g.setColour (colour.withAlpha (0.25f)); g.fillEllipse (juce::Rectangle<float> (r * 4, r * 4).withCentre (pt)); }
            g.setColour (n == 3 ? Colours_::display : colour);
            g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (pt));
            g.setColour (n == 3 ? colour : Colours_::display);
            g.drawEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (pt), 1.2f);
        }
        if (dragging >= 0 || hover >= 0)
        {
            const int n = dragging >= 0 ? dragging : hover;
            const juce::String names[] = { "Attack", "Decay / Sustain", "Release", "Curve" };
            juce::String txt = names[n] + "  ";
            for (int i : nodeParams (n)) txt << rangedParams[(size_t) i]->getCurrentValueAsText() << "  ";
            g.setFont (Fonts::medium (11.5f));
            auto box = juce::Rectangle<float> (180.0f, 18.0f).withCentre (nodes[(size_t) n].translated (0, -18.0f)).constrainedWithin (area);
            g.setColour (Colours_::panelTop.withAlpha (0.92f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (Colours_::text);
            g.drawText (txt.trim(), box, juce::Justification::centred);
        }
    }

    void resized() override { updateNodes(); }

private:
    void timerCallback() override
    {
        bool changed = false;
        for (size_t i = 0; i < params.size(); ++i)
        {
            const float v = params[i]->load();
            if (! juce::exactlyEqual (v, cached[i])) { cached[i] = v; changed = true; }
        }
        if (changed) { updateNodes(); repaint(); }
    }

    juce::Rectangle<float> inner() const { return getLocalBounds().toFloat().reduced (9.0f, 9.0f); }
    void updateNodes()
    {
        const auto area = inner();
        const float A = juce::jmax (0.0f, cached[0]), D = juce::jmax (0.0f, cached[1]), S = juce::jlimit (0.0f, 1.0f, cached[2]), R = juce::jmax (0.0f, cached[3]), C = cached[4];
        const float wa = std::sqrt (A), wd = std::sqrt (D), wr = std::sqrt (R);
        const float ws = (wa + wd + wr) * 0.25f + 1.0f, total = wa + wd + ws + wr;
        auto X = [&] (float u) { return area.getX() + u / total * area.getWidth(); };
        auto Y = [&] (float lvl) { return area.getBottom() - lvl * area.getHeight(); };
        nodes[0] = { X (wa), Y (1.0f) };
        nodes[1] = { X (wa + wd), Y (S) };
        nodes[2] = { X (total), Y (0.0f) };
        nodes[3] = { X (wa + 0.5f * wd), Y (S + (1.0f - S) * CurveEnvelope::shapeFor (0.5f, C)) };
    }
    int hitNode (juce::Point<float> p) const
    {
        int best = -1;
        float bestD = 11.0f;
        for (int n = 0; n < 4; ++n)
            if (const float d = nodes[(size_t) n].getDistanceFrom (p); d < bestD) { bestD = d; best = n; }
        return best;
    }
    static std::vector<int> nodeParams (int node)
    {
        switch (node) { case 0: return { 0 }; case 1: return { 1, 2 }; case 2: return { 3 }; default: return { 4 }; }
    }

    juce::Colour colour;
    std::array<std::atomic<float>*, 5> params {};
    std::array<juce::RangedAudioParameter*, 5> rangedParams {};
    std::array<float, 5> cached { -1, -1, -1, -1, -1 };
    std::array<juce::Point<float>, 4> nodes {};
    int hover = -1, dragging = -1;
    juce::Point<float> lastPos;
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

/** Icon button that reports press and release (used for the preview / play button). */
class PressButton : public IconButton
{
public:
    using IconButton::IconButton;
    std::function<void (bool)> onPress;
    void buttonStateChanged() override
    {
        const bool down = isDown();
        if (down != wasDown) { wasDown = down; if (onPress) onPress (down); }
    }
private:
    bool wasDown = false;
};

/** Small knob with caption and value beside it, for the header. */
class HeaderKnob : public juce::Component
{
public:
    explicit HeaderKnob (const juce::String& caption) : title (caption.toUpperCase())
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setMouseDragSensitivity (220);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
    }

    void attach (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
        configureParameterSlider (slider, s, id);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().withTrimmedLeft ((float) getHeight() + 4.0f);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (11.5f, 0.14f));
        g.drawText (title, r.removeFromTop (r.getHeight() * 0.5f), juce::Justification::bottomLeft);
        g.setColour (Colours_::text);
        g.setFont (Fonts::medium (13.5f));
        g.drawText (slider.getTextFromValue (slider.getValue()), r, juce::Justification::topLeft);
    }

    void resized() override { slider.setBounds (getLocalBounds().removeFromLeft (getHeight())); }

    juce::Slider slider;

private:
    juce::String title;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** Card shown over a bypassed section, with a button that switches it back on. */
struct BypassNotice : public juce::Component
{
    BypassNotice() { enable.setComponentID ("pill"); addAndMakeVisible (enable); }
    juce::String title, subtitle { "The signal passes through unchanged." };
    juce::TextButton enable { "ENABLE" };
    juce::Rectangle<float> card() const { return getLocalBounds().toFloat().withSizeKeepingCentre (jmin (420.0f, (float) getWidth() - 40.0f), 112.0f); }
    void paint (juce::Graphics& g) override
    {
        g.setColour (Colours_::panel.withAlpha (0.72f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 7.0f);
        auto c = card();
        g.setColour (Colours_::display.withAlpha (0.95f));
        g.fillRoundedRectangle (c, 8.0f);
        g.setColour (Colours_::outlineHi);
        g.drawRoundedRectangle (c, 8.0f, 1.0f);
        g.setColour (Colours_::text);
        g.setFont (Fonts::label (14.0f, 0.18f));
        g.drawText (title, c.removeFromTop (38.0f), juce::Justification::centredBottom);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::regular (13.0f));
        g.drawText (subtitle, c.removeFromTop (22.0f), juce::Justification::centred);
    }
    void resized() override { enable.setBounds (card().toNearestInt().removeFromBottom (44).withSizeKeepingCentre (120, 28)); }
    static float jmin (float a, float b) { return a < b ? a : b; }
};
