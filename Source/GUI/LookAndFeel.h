#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "CodexAssets.h"

//==============================================================================
/** Colour palette: clean graphite with a cool blue accent (flat, studio-tool look). */
namespace Colours_
{
    const juce::Colour background { 0xff0c0e12 };
    const juce::Colour panel      { 0xff14171d };
    const juce::Colour panelTop   { 0xff181b22 };
    const juce::Colour header     { 0xff181b22 };
    const juce::Colour display    { 0xff0a0b0e };
    const juce::Colour outline    { 0xff242831 };
    const juce::Colour outlineHi  { 0xff343a46 };
    const juce::Colour knobTrack  { 0xff252a33 };
    const juce::Colour accent     { 0xff45a6ff }; // primary blue
    const juce::Colour accentHi   { 0xffa3d2ff };
    const juce::Colour accent2    { 0xff9487ff }; // violet (pitch envelope)
    const juce::Colour good       { 0xff4fd49a };
    const juce::Colour warn       { 0xffff5a6a };
    const juce::Colour text       { 0xffe8ebf0 };
    const juce::Colour textDim    { 0xff8b929e };
    const juce::Colour textFaint  { 0xff545a65 };
    const juce::Colour grid       { 0xff171a20 };

    inline juce::Colour band (int i)
    {
        static const juce::Colour c[] = { juce::Colour (0xffff5c8a), juce::Colour (0xffffa94d), juce::Colour (0xffffe066),
                                          juce::Colour (0xff51e5a0), juce::Colour (0xff4dd2ff), juce::Colour (0xffa98bff) };
        return c[i % 6];
    }

    /** Draws a path twice: a wide translucent "glow" and a crisp core line. */
    inline void strokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width)
    {
        // a restrained halo: just enough to lift curves off the display
        g.setColour (c.withAlpha (0.12f));
        g.strokePath (p, juce::PathStrokeType (width * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

//==============================================================================
/** Embedded typefaces (Barlow / Barlow Condensed, SIL OFL). */
namespace Fonts
{
    struct Faces
    {
        juce::Typeface::Ptr regular, medium, semibold, condensed, condensedBold;
        Faces()
        {
            using namespace CodexAssets;
            auto make = [] (const char* data, int size) { return juce::Typeface::createSystemTypefaceFor (data, (size_t) size); };
            regular       = make (BarlowRegular_ttf, BarlowRegular_ttfSize);
            medium        = make (BarlowMedium_ttf, BarlowMedium_ttfSize);
            semibold      = make (BarlowSemiBold_ttf, BarlowSemiBold_ttfSize);
            condensed     = make (BarlowCondensedSemiBold_ttf, BarlowCondensedSemiBold_ttfSize);
            condensedBold = make (BarlowCondensedBold_ttf, BarlowCondensedBold_ttfSize);
        }
    };

    inline const Faces& faces() { static Faces f; return f; }

    inline juce::FontOptions make (juce::Typeface::Ptr t, float h) { return juce::FontOptions (t).withHeight (h); }

    /** Body text and values. */
    inline juce::FontOptions regular (float h)  { return make (faces().regular, h); }
    inline juce::FontOptions medium (float h)   { return make (faces().medium, h); }
    inline juce::FontOptions semibold (float h) { return make (faces().semibold, h); }
    /** Upper-case captions: condensed with tracking. */
    inline juce::FontOptions label (float h, float tracking = 0.08f) { return make (faces().condensed, h).withKerningFactor (tracking); }
    /** Headlines / logo. */
    inline juce::FontOptions display (float h, float tracking = 0.18f) { return make (faces().condensedBold, h).withKerningFactor (tracking); }
}

//==============================================================================
/** Vector icons, drawn in a unit box (0..1) and scaled to fit. */
namespace Icons
{
    inline juce::Path power()
    {
        juce::Path p;
        p.addCentredArc (0.5f, 0.55f, 0.36f, 0.36f, 0.0f, 0.65f, juce::MathConstants<float>::twoPi - 0.65f, true);
        p.startNewSubPath (0.5f, 0.08f);
        p.lineTo (0.5f, 0.48f);
        return p;
    }
    inline juce::Path chevronLeft()  { juce::Path p; p.startNewSubPath (0.62f, 0.2f); p.lineTo (0.32f, 0.5f); p.lineTo (0.62f, 0.8f); return p; }
    inline juce::Path chevronRight() { juce::Path p; p.startNewSubPath (0.38f, 0.2f); p.lineTo (0.68f, 0.5f); p.lineTo (0.38f, 0.8f); return p; }
    inline juce::Path chevronDown()  { juce::Path p; p.startNewSubPath (0.2f, 0.38f); p.lineTo (0.5f, 0.66f); p.lineTo (0.8f, 0.38f); return p; }
    inline juce::Path save()
    {
        juce::Path p;
        p.startNewSubPath (0.15f, 0.12f); p.lineTo (0.72f, 0.12f); p.lineTo (0.88f, 0.28f); p.lineTo (0.88f, 0.88f);
        p.lineTo (0.15f, 0.88f); p.closeSubPath();
        p.startNewSubPath (0.3f, 0.12f); p.lineTo (0.3f, 0.36f); p.lineTo (0.66f, 0.36f); p.lineTo (0.66f, 0.12f);
        p.addRectangle (0.3f, 0.56f, 0.43f, 0.32f);
        return p;
    }
    inline juce::Path undo()
    {
        juce::Path p;
        p.addCentredArc (0.52f, 0.56f, 0.3f, 0.28f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi * 0.85f, true);
        p.startNewSubPath (0.38f, 0.14f); p.lineTo (0.22f, 0.28f); p.lineTo (0.4f, 0.4f);
        return p;
    }
    inline juce::Path redo()
    {
        auto p = undo();
        p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f).translated (1.0f, 0.0f));
        return p;
    }
    inline juce::Path play()
    {
        juce::Path p;
        p.startNewSubPath (0.32f, 0.2f); p.lineTo (0.78f, 0.5f); p.lineTo (0.32f, 0.8f); p.closeSubPath();
        return p;
    }
    inline juce::Path menu()
    {
        juce::Path p;
        for (float y : { 0.25f, 0.5f, 0.75f }) { p.startNewSubPath (0.18f, y); p.lineTo (0.82f, y); }
        return p;
    }
    /** Preset browser: a 2x2 grid of tiles. */
    inline juce::Path browse()
    {
        juce::Path p;
        for (float x : { 0.18f, 0.56f })
            for (float y : { 0.18f, 0.56f })
                p.addRoundedRectangle (x, y, 0.26f, 0.26f, 0.05f);
        return p;
    }
    inline juce::Path star()
    {
        juce::Path p;
        p.addStar ({ 0.5f, 0.52f }, 5, 0.18f, 0.42f, -juce::MathConstants<float>::pi * 0.5f + juce::MathConstants<float>::pi * 0.5f);
        return p;
    }
    inline juce::Path close()
    {
        juce::Path p;
        p.startNewSubPath (0.25f, 0.25f); p.lineTo (0.75f, 0.75f);
        p.startNewSubPath (0.75f, 0.25f); p.lineTo (0.25f, 0.75f);
        return p;
    }
    /** Logo mark: a decaying kick waveform inside a ring. */
    inline juce::Path kickMark()
    {
        juce::Path p;
        p.addEllipse (0.04f, 0.04f, 0.92f, 0.92f);
        p.startNewSubPath (0.16f, 0.5f);
        constexpr int steps = 60;
        for (int i = 1; i <= steps; ++i)
        {
            const float t = (float) i / steps;
            const float phase = 2.4f * std::pow (t, 0.55f) * juce::MathConstants<float>::twoPi;
            const float amp = 0.3f * std::exp (-2.2f * t);
            p.lineTo (0.16f + 0.68f * t, 0.5f - amp * std::sin (phase));
        }
        return p;
    }

    inline void draw (juce::Graphics& g, const juce::Path& unitPath, juce::Rectangle<float> area, juce::Colour c, float stroke = 1.6f)
    {
        const float s = juce::jmin (area.getWidth(), area.getHeight());
        const auto box = area.withSizeKeepingCentre (s, s);
        g.setColour (c);
        g.strokePath (unitPath, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::scale (s, s).translated (box.getX(), box.getY()));
    }
}

//==============================================================================
/** Flat icon button with hover and toggle states. */
class IconButton : public juce::Button
{
public:
    IconButton (const juce::String& name, juce::Path icon, bool drawFrame = true)
        : juce::Button (name), path (std::move (icon)), frame (drawFrame) {}

    void setIcon (juce::Path p) { path = std::move (p); repaint(); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        if (frame)
        {
            g.setColour (down ? Colours_::outlineHi : (over ? Colours_::panelTop.brighter (0.1f) : Colours_::display));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (over ? Colours_::outlineHi : Colours_::outline);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }
        auto c = getToggleState() ? Colours_::accent : (over ? Colours_::text : Colours_::textDim);
        if (! isEnabled()) c = Colours_::textFaint;
        Icons::draw (g, path, r.reduced (r.getHeight() * 0.26f), c, 1.6f);
    }

private:
    juce::Path path;
    bool frame;
};

//==============================================================================
/** A subtle film-grain tile used to give the background some texture. */
inline juce::Image noiseTile()
{
    static juce::Image img = []
    {
        juce::Image im (juce::Image::ARGB, 128, 128, true);
        juce::Random rng (1234);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                im.setPixelAt (x, y, juce::Colours::white.withAlpha (rng.nextFloat() * 0.035f));
        return im;
    }();
    return img;
}

//==============================================================================
class CodexLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CodexLookAndFeel()
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
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
        setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextEditor::outlineColourId, outline);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ComboBox::backgroundColourId, display);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, accent);
        setColour (juce::PopupMenu::backgroundColourId, panelTop);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::headerTextColourId, textDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.18f));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextButton::buttonColourId, display);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, textDim);
        setColour (juce::TextButton::textColourOnId, juce::Colour (0xff04101e));
        setColour (juce::TooltipWindow::backgroundColourId, panelTop);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, outlineHi);
    }

    // Every default font in the UI uses the embedded Barlow faces
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& f) override
    {
        const auto& faces = Fonts::faces();
        if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName() || f.getTypefaceName().isEmpty())
            return f.isBold() ? faces.semibold : faces.regular;
        return LookAndFeel_V4::getTypefaceForFont (f);
    }

    //==========================================================================
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        using namespace Colours_;
        const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lineW = juce::jlimit (2.0f, 3.5f, radius * 0.075f);
        const float arcR = radius - lineW;
        const float angle = startAngle + pos * (endAngle - startAngle);
        const bool hot = s.isMouseOverOrDragging() && s.isEnabled();

        const bool bipolar = (s.getMinimum() < 0.0 && s.getMaximum() > 0.0 && std::abs (s.getMinimum() + s.getMaximum()) < 1.0e-6)
                             || s.getProperties().contains ("bipolar");
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        const auto arcColour = s.isEnabled() ? (hot ? accentHi : accent) : textFaint;

        // Track
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (knobTrack);
        g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Value arc
        if (std::abs (angle - from) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (arcColour);
            g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Flat body with a hairline rim
        const float bodyR = arcR - lineW * 2.2f;
        const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff272c36), centre.x, body.getY(),
                                                 juce::Colour (0xff1b1f27), centre.x, body.getBottom(), false));
        g.fillEllipse (body);
        g.setColour (hot ? outlineHi.brighter (0.2f) : outlineHi);
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        // Pointer
        const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
        juce::Path pointer;
        pointer.startNewSubPath (centre + dir * (bodyR * 0.35f));
        pointer.lineTo (centre + dir * (bodyR * 0.85f));
        g.setColour (s.isEnabled() ? (hot ? juce::Colours::white : text) : textFaint);
        g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.6f, bodyR * 0.08f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }
        const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float cx = r.getCentreX();
        const auto track = juce::Rectangle<float> (4.0f, r.getHeight()).withCentre ({ cx, r.getCentreY() });
        g.setColour (Colours_::knobTrack);
        g.fillRoundedRectangle (track, 2.0f);

        // bipolar fill from the centre (0 dB) to the thumb
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float zeroY = bipolar ? (float) s.getPositionOfValue (0.0) : r.getBottom();
        const bool hot = s.isMouseOverOrDragging() && s.isEnabled();
        const auto col = s.isEnabled() ? (hot ? Colours_::accentHi : Colours_::accent) : Colours_::textFaint;
        g.setColour (col);
        g.fillRoundedRectangle (juce::Rectangle<float> (cx - 2.0f, juce::jmin (zeroY, pos), 4.0f, std::abs (zeroY - pos)), 2.0f);
        if (bipolar)
        {
            g.setColour (Colours_::textFaint);
            g.drawHorizontalLine ((int) zeroY, cx - 8.0f, cx + 8.0f);
        }

        // thumb
        const auto thumb = juce::Rectangle<float> (26.0f, 12.0f).withCentre ({ cx, pos });
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (thumb.translated (0.0f, 1.5f), 4.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2c3a63), cx, thumb.getY(), juce::Colour (0xff141c33), cx, thumb.getBottom(), false));
        g.fillRoundedRectangle (thumb, 4.0f);
        g.setColour (col);
        g.drawRoundedRectangle (thumb, 4.0f, 1.2f);
        g.fillRect (thumb.withSizeKeepingCentre (14.0f, 2.0f));
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::textColourId, Colours_::text);
        l->setColour (juce::Label::outlineWhenEditingColourId, Colours_::accent);
        l->setColour (juce::Label::backgroundWhenEditingColourId, Colours_::display);
        l->setColour (juce::TextEditor::highlightColourId, Colours_::accent.withAlpha (0.35f));
        return l;
    }

    juce::Font getLabelFont (juce::Label& l) override
    {
        if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
            return juce::Font (Fonts::medium (13.0f));
        if (l.getProperties().contains ("captionLarge"))
            return juce::Font (Fonts::label (14.0f, 0.12f));
        if (l.getProperties().contains ("caption"))
            return juce::Font (Fonts::label (11.5f, 0.09f));
        return juce::Font (Fonts::regular (13.0f));
    }

    //==========================================================================
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        const bool hot = box.isMouseOver (true) || box.isPopupActive();
        g.setColour (hot ? Colours_::panelTop.brighter (0.05f) : Colours_::panelTop);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (hot ? Colours_::outlineHi.brighter (0.2f) : Colours_::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        Icons::draw (g, Icons::chevronDown(), juce::Rectangle<float> ((float) w - 22.0f, 0.0f, 14.0f, (float) h).withSizeKeepingCentre (10.0f, 10.0f),
                     hot ? Colours_::text : Colours_::textDim, 1.4f);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (Fonts::medium (14.0f)); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu (juce::ComboBox& box, juce::Label& label) override
    {
        return LookAndFeel_V4::getOptionsForComboBoxPopupMenu (box, label).withStandardItemHeight (26);
    }

    //==========================================================================
    juce::Font getPopupMenuFont() override { return juce::Font (Fonts::medium (14.5f)); }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (Colours_::panelTop);
        g.setColour (Colours_::outlineHi);
        g.drawRect (0, 0, w, h);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable*, const juce::Colour*) override
    {
        if (isSeparator)
        {
            g.setColour (Colours_::outline);
            g.fillRect (area.reduced (10, 0).withSizeKeepingCentre (area.getWidth() - 20, 1));
            return;
        }
        auto r = area.reduced (4, 1).toFloat();
        if (isHighlighted && isActive)
        {
            g.setColour (Colours_::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (r, 4.0f);
        }
        if (isTicked)
        {
            g.setColour (Colours_::accent);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getX() + 11.0f, r.getCentreY() }));
        }
        g.setColour (! isActive ? Colours_::textFaint : (isHighlighted ? juce::Colours::white : (isTicked ? Colours_::accentHi : Colours_::text)));
        g.setFont (getPopupMenuFont());
        g.drawText (text, r.withTrimmedLeft (22.0f).withTrimmedRight (24.0f), juce::Justification::centredLeft);
        if (shortcutKeyText.isNotEmpty())
        {
            g.setColour (Colours_::textDim);
            g.setFont (Fonts::regular (12.5f));
            g.drawText (shortcutKeyText, r.withTrimmedRight (10.0f), juce::Justification::centredRight);
        }
        if (hasSubMenu)
            Icons::draw (g, Icons::chevronRight(), r.removeFromRight (20.0f).withSizeKeepingCentre (10.0f, 10.0f), Colours_::textDim, 1.4f);
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name) override
    {
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (11.0f, 0.18f));
        g.drawText (name.toUpperCase(), area.reduced (14, 0).withTrimmedTop (4), juce::Justification::centredLeft);
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardHeight, int& w, int& h) override
    {
        if (isSeparator) { w = 50; h = 9; return; }
        h = standardHeight > 0 ? standardHeight : 28;
        juce::GlyphArrangement ga;
        ga.addLineOfText (getPopupMenuFont(), text, 0, 0);
        w = (int) ga.getBoundingBox (0, -1, true).getWidth() + 60;
    }

    //==========================================================================
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const auto id = b.getComponentID();

        if (id == "kick")
        {
            // Big glowing trigger pad
            auto c = Colours_::accent;
            if (down) c = c.brighter (0.35f); else if (over) c = c.brighter (0.12f);
            for (int i = 3; i > 0; --i)
            {
                g.setColour (c.withAlpha ((down ? 0.12f : 0.06f) * (float) i));
                g.fillRoundedRectangle (r.reduced ((float) i * 1.5f - 4.5f), 7.0f);
            }
            g.setGradientFill (juce::ColourGradient (c.brighter (0.3f), 0, r.getY(), c.darker (0.35f), 0, r.getBottom(), false));
            g.fillRoundedRectangle (r.reduced (3.0f), 6.0f);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawHorizontalLine ((int) r.getY() + 4, r.getX() + 9.0f, r.getRight() - 9.0f);
            return;
        }

        if (id == "tab")
        {
            if (b.getToggleState())
            {
                g.setColour (Colours_::accent);
                g.fillRect (r.removeFromBottom (2.0f).reduced (6.0f, 0.0f));
            }
            return;
        }

        if (id == "pill")
        {
            const bool on = b.getToggleState();
            g.setColour (on ? Colours_::accent.withAlpha (0.16f) : (over ? Colours_::panelTop.brighter (0.04f) : Colours_::panelTop));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (on ? Colours_::accent.withAlpha (0.8f) : (over ? Colours_::outlineHi : Colours_::outline));
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
            return;
        }

        if (id == "seg")
        {
            const bool on = b.getToggleState();
            const bool first = b.getConnectedEdgeFlags() & juce::Button::ConnectedOnRight;
            const bool last  = b.getConnectedEdgeFlags() & juce::Button::ConnectedOnLeft;
            juce::Path p;
            p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), 4.0f, 4.0f,
                                   ! last, ! first, ! last, ! first);
            g.setColour (on ? Colours_::accent.withAlpha (0.2f) : (over ? Colours_::panelTop.brighter (0.04f) : Colours_::panelTop));
            g.fillPath (p);
            g.setColour (on ? Colours_::accent.withAlpha (0.8f) : Colours_::outline);
            g.strokePath (p, juce::PathStrokeType (1.0f));
            return;
        }

        auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
        if (down) c = c.brighter (0.2f); else if (over) c = c.brighter (0.08f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (b.getToggleState() ? c.brighter (0.3f) : (over ? Colours_::outlineHi : Colours_::outline));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool) override
    {
        const auto id = b.getComponentID();
        juce::Colour c;
        if (id == "kick")      c = juce::Colour (0xff04101e);
        else if (id == "tab")  c = b.getToggleState() ? Colours_::text : (over ? Colours_::text.withAlpha (0.8f) : Colours_::textDim);
        else if (id == "pill") c = b.getToggleState() ? Colours_::accentHi : Colours_::textDim;
        else if (id == "seg")  c = b.getToggleState() ? Colours_::accentHi : (over ? Colours_::text : Colours_::textDim);
        else                   c = b.getToggleState() ? b.findColour (juce::TextButton::textColourOnId)
                                                      : (over ? Colours_::text : b.findColour (juce::TextButton::textColourOffId));
        if (! b.isEnabled()) c = Colours_::textFaint;
        g.setColour (c);
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.drawText (b.getButtonText(), b.getLocalBounds().withTrimmedBottom (id == "tab" ? 2 : 0), juce::Justification::centred);
    }

    juce::Font getTextButtonFont (juce::TextButton& b, int h) override
    {
        const auto id = b.getComponentID();
        if (id == "kick") return juce::Font (Fonts::display (19.0f, 0.3f));
        if (id == "tab")  return juce::Font (Fonts::label (14.5f, 0.1f));
        return juce::Font (Fonts::label (juce::jmin (13.0f, (float) h * 0.58f), 0.08f));
    }

    //==========================================================================
    void drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging) override
    {
        const auto c = (isMouseOver || isMouseDragging) ? Colours_::accent : Colours_::textFaint;
        g.setColour (c);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j <= i; ++j)
                g.fillEllipse ((float) w - 4.0f - (float) j * 4.5f, (float) h - 4.0f - (float) (i - j) * 4.5f, 2.2f, 2.2f);
    }

    //==========================================================================
    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&) override
    {
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 5.0f);
    }

    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        g.setColour (te.hasKeyboardFocus (true) ? Colours_::accent : Colours_::outline);
        g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 5.0f, 1.0f);
    }

    //==========================================================================
    juce::Rectangle<int> getTooltipBounds (const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parentArea) override
    {
        const auto layout = tooltipLayout (tip);
        const int w = (int) std::ceil (layout.getWidth()) + 22, h = (int) std::ceil (layout.getHeight()) + 14;
        return juce::Rectangle<int> (pos.x > parentArea.getCentreX() ? pos.x - (w + 12) : pos.x + 18,
                                     pos.y > parentArea.getCentreY() ? pos.y - (h + 8) : pos.y + 18, w, h)
                   .constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        g.setColour (Colours_::panelTop);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Colours_::outlineHi);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        tooltipLayout (text).draw (g, r.reduced (11.0f, 7.0f));
    }

private:
    static juce::TextLayout tooltipLayout (const juce::String& text)
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::topLeft);
        // "Title: description" -> bold title line
        const int colon = text.indexOfChar (':');
        if (colon > 0 && colon < 28)
        {
            s.append (text.substring (0, colon) + "\n", juce::Font (Fonts::semibold (14.0f)), Colours_::accentHi);
            s.append (text.substring (colon + 1).trim(), juce::Font (Fonts::regular (13.5f)), Colours_::text);
        }
        else
            s.append (text, juce::Font (Fonts::regular (13.5f)), Colours_::text);
        juce::TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (s, 300.0f);
        return tl;
    }
};
