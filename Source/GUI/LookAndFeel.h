#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/** Dark navy / blue-glow palette in the spirit of modern wavetable synths. */
namespace Colours_
{
    const juce::Colour background { 0xff0a0f1c };
    const juce::Colour panel      { 0xff101727 };
    const juce::Colour panelTop   { 0xff15203a };
    const juce::Colour header     { 0xff17223d };
    const juce::Colour display    { 0xff060a14 };
    const juce::Colour outline    { 0xff1f2b47 };
    const juce::Colour knobTrack  { 0xff1a2440 };
    const juce::Colour accent     { 0xff38b6ff }; // primary blue
    const juce::Colour accent2    { 0xff7d6bff }; // violet (pitch envelope / analyzer)
    const juce::Colour text       { 0xffe3ebfa };
    const juce::Colour textDim    { 0xff7f8db0 };
    const juce::Colour grid       { 0xff131c31 };

    inline juce::Colour band (int i)
    {
        static const juce::Colour c[] = { juce::Colour (0xffff5c8a), juce::Colour (0xffffa94d), juce::Colour (0xffffe066),
                                          juce::Colour (0xff51e5a0), juce::Colour (0xff4dd2ff), juce::Colour (0xffa98bff) };
        return c[i % 6];
    }

    /** Draws a path twice: a wide translucent "glow" and a crisp core line. */
    inline void strokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width)
    {
        g.setColour (c.withAlpha (0.12f));
        g.strokePath (p, juce::PathStrokeType (width * 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c.withAlpha (0.25f));
        g.strokePath (p, juce::PathStrokeType (width * 2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

class HKLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HKLookAndFeel()
    {
        using namespace Colours_;
        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
        setColour (juce::Label::textColourId, text);
        setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::backgroundColourId, display);
        setColour (juce::TextEditor::outlineColourId, accent);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ComboBox::backgroundColourId, display);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, accent);
        setColour (juce::PopupMenu::backgroundColourId, panelTop);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::headerTextColourId, textDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.3f));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextButton::buttonColourId, display);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, textDim);
        setColour (juce::TextButton::textColourOnId, juce::Colour (0xff04101e));
    }

    //==========================================================================
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        using namespace Colours_;
        const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lineW = juce::jlimit (2.5f, 4.5f, radius * 0.11f);
        const float arcR = radius - lineW;
        const float angle = startAngle + pos * (endAngle - startAngle);

        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0
                             && std::abs (s.getMinimum() + s.getMaximum()) < 1.0e-6;
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        const auto arcColour = s.isEnabled() ? accent : textDim;

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (knobTrack);
        g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (std::abs (angle - from) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
            strokeGlow (g, value, arcColour, lineW);
        }

        // Body
        const float bodyR = arcR - lineW * 1.9f;
        const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff222e4f), centre.x, centre.y - bodyR,
                                                 juce::Colour (0xff0d1424), centre.x, centre.y + bodyR, false));
        g.fillEllipse (body);
        g.setColour (juce::Colour (0xff2c3a60));
        g.drawEllipse (body, 1.0f);

        // Indicator line
        const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
        g.setColour (text);
        g.drawLine (juce::Line<float> (centre + dir * (bodyR * 0.35f), centre + dir * (bodyR * 0.9f)), 2.0f);

        // Indicator dot on the ring
        g.setColour (juce::Colours::white);
        const auto dot = centre + dir * arcR;
        g.fillEllipse (juce::Rectangle<float> (lineW * 1.1f, lineW * 1.1f).withCentre (dot));
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::textColourId, Colours_::text);
        l->setColour (juce::Label::outlineWhenEditingColourId, Colours_::accent);
        l->setColour (juce::Label::backgroundWhenEditingColourId, Colours_::display);
        return l;
    }

    juce::Font getLabelFont (juce::Label& l) override
    {
        if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
            return juce::Font (juce::FontOptions (11.5f));
        return juce::Font (juce::FontOptions (10.0f, juce::Font::bold).withKerningFactor (0.08f));
    }

    //==========================================================================
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (box.hasKeyboardFocus (true) || box.isMouseOver (true) ? Colours_::accent.withAlpha (0.6f) : Colours_::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        juce::Path arrow;
        const float ax = (float) w - 14.0f, ay = (float) h * 0.5f;
        arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
        arrow.lineTo (ax, ay + 2.0f);
        arrow.lineTo (ax + 4.0f, ay - 2.0f);
        g.setColour (Colours_::accent);
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (12.5f)); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (6, 1, box.getWidth() - 26, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    //==========================================================================
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);

        if (b.getComponentID() == "kick")
        {
            // Big glowing trigger pad
            auto c = Colours_::accent;
            if (down) c = c.brighter (0.3f); else if (over) c = c.brighter (0.12f);
            for (int i = 3; i > 0; --i)
            {
                g.setColour (c.withAlpha (0.07f * (float) i));
                g.fillRoundedRectangle (r.reduced ((float) i * 1.5f - 4.5f), 6.0f);
            }
            g.setGradientFill (juce::ColourGradient (c.brighter (0.25f), 0, r.getY(), c.darker (0.35f), 0, r.getBottom(), false));
            g.fillRoundedRectangle (r.reduced (3.0f), 5.0f);
            return;
        }

        auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
        if (down) c = c.brighter (0.2f); else if (over) c = c.brighter (0.1f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (b.getToggleState() ? c.brighter (0.3f) : Colours_::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton& b, int h) override
    {
        if (b.getComponentID() == "kick")
            return juce::Font (juce::FontOptions (15.0f, juce::Font::bold).withKerningFactor (0.15f));
        return juce::Font (juce::FontOptions (juce::jmin (13.0f, (float) h * 0.6f), juce::Font::bold));
    }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (Colours_::panelTop);
        g.setColour (Colours_::outline);
        g.drawRect (0, 0, w, h);
    }
};
