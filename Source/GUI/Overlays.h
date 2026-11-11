#pragma once
#include "Widgets.h"

/** Full-window modal card that dims the UI behind it. Click outside or press Escape to close. */
class Overlay : public juce::Component
{
public:
    Overlay() { setWantsKeyboardFocus (true); setAlwaysOnTop (true); }

    void open()  { setVisible (true); toFront (true); grabKeyboardFocus(); opened(); }
    void close() { setVisible (false); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));
        const auto c = card.toFloat();
        for (int i = 4; i > 0; --i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.08f));
            g.fillRoundedRectangle (c.expanded ((float) i * 3.0f).translated (0.0f, 4.0f), 12.0f);
        }
        g.setGradientFill (juce::ColourGradient (Colours_::panelTop, 0, c.getY(), Colours_::panel, 0, c.getBottom(), false));
        g.fillRoundedRectangle (c, 10.0f);
        g.setColour (Colours_::outlineHi);
        g.drawRoundedRectangle (c.reduced (0.5f), 10.0f, 1.0f);
        paintCard (g, card);
    }

    void resized() override { card = getLocalBounds().withSizeKeepingCentre (cardSize.x, cardSize.y); layoutCard (card); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! card.contains (e.getPosition())) close();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { close(); return true; }
        return false;
    }

protected:
    virtual void paintCard (juce::Graphics&, juce::Rectangle<int>) {}
    virtual void layoutCard (juce::Rectangle<int>) {}
    virtual void opened() {}

    juce::Point<int> cardSize { 400, 220 };
    juce::Rectangle<int> card;
};

/** "Save preset" dialog. */
class SavePresetOverlay : public Overlay
{
public:
    std::function<void (const juce::String&)> onSave;
    std::function<bool (const juce::String&)> exists;

    SavePresetOverlay()
    {
        cardSize = { 400, 196 };
        name.setFont (Fonts::medium (16.0f));
        name.setIndents (10, 0);
        name.setJustification (juce::Justification::centredLeft);
        name.setInputRestrictions (64, {});
        name.onReturnKey = [this] { commit(); };
        name.onEscapeKey = [this] { close(); };
        name.onTextChange = [this] { repaint(); };
        save.onClick = [this] { commit(); };
        cancel.onClick = [this] { close(); };
        save.setColour (juce::TextButton::buttonColourId, Colours_::accent);
        save.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff140900));
        for (auto* c : std::initializer_list<juce::Component*> { &name, &save, &cancel }) addAndMakeVisible (c);
    }

    void setInitialName (const juce::String& n) { name.setText (n, false); name.selectAll(); }

private:
    void opened() override { name.grabKeyboardFocus(); name.selectAll(); }

    void commit()
    {
        const auto n = name.getText().trim();
        if (n.isEmpty()) return;
        close();
        if (onSave) onSave (n);
    }

    void paintCard (juce::Graphics& g, juce::Rectangle<int> c) override
    {
        auto r = c.reduced (22, 18);
        g.setColour (Colours_::text);
        g.setFont (Fonts::display (20.0f, 0.15f));
        g.drawText ("SAVE PRESET", r.removeFromTop (26), juce::Justification::centredLeft);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::regular (13.0f));
        g.drawText ("Saved to Documents / Codex / Presets. Key settings are not stored.", r.removeFromTop (20), juce::Justification::centredLeft);
        if (exists != nullptr && exists (name.getText().trim()))
        {
            g.setColour (Colours_::band (1));
            g.drawText ("A preset with this name exists and will be overwritten.",
                        name.getBounds().translated (0, name.getHeight() + 4).withHeight (16), juce::Justification::centredLeft);
        }
    }

    void layoutCard (juce::Rectangle<int> c) override
    {
        auto r = c.reduced (22, 18);
        r.removeFromTop (54);
        name.setBounds (r.removeFromTop (36));
        auto buttons = r.removeFromBottom (32);
        save.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (10);
        cancel.setBounds (buttons.removeFromRight (100));
    }

    juce::TextEditor name;
    juce::TextButton save { "SAVE" }, cancel { "CANCEL" };
};

/** About / help card. */
class AboutOverlay : public Overlay
{
public:
    explicit AboutOverlay (const juce::String& versionString) : version (versionString)
    {
        cardSize = { 540, 336 };
        closeButton.onClick = [this] { close(); };
        addAndMakeVisible (closeButton);
    }

private:
    void paintCard (juce::Graphics& g, juce::Rectangle<int> c) override
    {
        using namespace juce;
        auto r = c.reduced (28, 24).toFloat();

        auto head = r.removeFromTop (54.0f);
        Icons::draw (g, Icons::kickMark(), head.removeFromLeft (54.0f), Colours_::accent, 2.4f);
        head.removeFromLeft (14.0f);
        g.setColour (Colours_::text);
        g.setFont (Fonts::display (34.0f, 0.25f));
        g.drawText ("CODEX", head.removeFromTop (34.0f), Justification::bottomLeft);
        g.setColour (Colours_::accent);
        g.setFont (Fonts::label (12.5f, 0.2f));
        g.drawText ("HARDSTYLE KICK SYNTHESIZER  " + String (CharPointer_UTF8 ("\xc2\xb7")) + "  VERSION " + version, head, Justification::topLeft);

        r.removeFromTop (18.0f);
        auto para = [&] (const String& title, const String& body, float h)
        {
            g.setColour (Colours_::textDim);
            g.setFont (Fonts::label (11.5f, 0.16f));
            g.drawText (title, r.removeFromTop (16.0f), Justification::centredLeft);
            g.setColour (Colours_::text);
            g.setFont (Fonts::regular (14.0f));
            g.drawFittedText (body, r.removeFromTop (h).toNearestInt(), Justification::topLeft, 4, 1.0f);
            r.removeFromTop (10.0f);
        };
        para ("ABOUT", "A free kick synthesizer for hardstyle, built the old-fashioned way: a swept oscillator or a synthesized 909, "
                       "two distortion stages around a parametric EQ. No samples.", 42.0f);
        para ("TIPS", String (CharPointer_UTF8 ("Hover any control for help  \xc2\xb7  Double-click a knob to reset  \xc2\xb7  Shift-drag for fine control  \xc2\xb7  "
                                                "Double-click the EQ to add a band  \xc2\xb7  Set KEY to the key of your track; the tuner confirms the root.")), 42.0f);
        para ("CREDITS", "Built with JUCE. Fonts: Barlow by The Barlow Project Authors, SIL Open Font License 1.1. "
                         "VST is a registered trademark of Steinberg Media Technologies GmbH.", 42.0f);
    }

    void layoutCard (juce::Rectangle<int> c) override
    {
        closeButton.setBounds (c.getRight() - 40, c.getY() + 12, 28, 28);
    }

    juce::String version;
    IconButton closeButton { "close", Icons::close(), false };
};

/** Quick guide: getting started, mouse gestures and keyboard shortcuts on one page. */
class QuickGuideOverlay : public Overlay
{
public:
    QuickGuideOverlay()
    {
        cardSize = { 860, 470 };
        closeButton.onClick = [this] { close(); };
        addAndMakeVisible (closeButton);
    }

private:
    void paintCard (juce::Graphics& g, juce::Rectangle<int> c) override
    {
        using namespace juce;
        auto r = c.reduced (30, 24).toFloat();
        g.setColour (Colours_::text);
        g.setFont (Fonts::display (22.0f, 0.18f));
        g.drawText ("QUICK GUIDE", r.removeFromTop (30.0f), Justification::centredLeft);
        r.removeFromTop (12.0f);

        const auto dot = String::fromUTF8 ("  \xc2\xb7  ");
        // "Ctrl+" or the command symbol, as the platform writes it
        const auto cmd = KeyPress ('z', ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons()
                             .upToLastOccurrenceOf ("+", true, false).removeCharacters (" ");
        auto col = [&] (Rectangle<float> area, std::initializer_list<std::pair<const char*, String>> rows)
        {
            for (const auto& [title, body] : rows)
            {
                g.setColour (Colours_::accent);
                g.setFont (Fonts::label (11.5f, 0.12f));
                g.drawText (title, area.removeFromTop (16.0f), Justification::centredLeft);
                g.setColour (Colours_::text);
                g.setFont (Fonts::regular (13.0f));
                const int lines = jmax (1, (int) std::ceil (GlyphArrangement::getStringWidth (Font (Fonts::regular (13.0f)), body) / (area.getWidth() - 10.0f)) + body.containsChar ('\n'));
                const float h = 17.0f * (float) lines + 2.0f;
                g.drawFittedText (body, area.removeFromTop (h).toNearestInt(), Justification::topLeft, lines + 1, 1.0f);
                area.removeFromTop (10.0f);
            }
        };
        auto left = r.removeFromLeft (r.getWidth() * 0.5f - 12.0f);
        r.removeFromLeft (24.0f);
        col (left, {
            { "1  KEY", String::fromUTF8 ("Set KEY (F to E) to the key of your track. Every MIDI note plays the kick in that key; the ROOT readout confirms it.") },
            { "2  PRESET", String::fromUTF8 ("Browse with the arrows or the grid button (search, categories, favourites). '*' after the name = edited.") },
            { "3  SHAPE THE HIT", String::fromUTF8 ("KICK page: pitch and amp envelopes (ADSR or DRAW). DISTORTION: two units with Pre/Post filter and drawable X-Shaper. EQ sits between them.") },
            { "4  POLISH", String::fromUTF8 ("SATURATION, COMPRESSOR, EQ 2 and SPACE are off until you switch them on. SUB LAYER adds a clean fundamental: press ALIGN.") },
            { "5  FINISH", String::fromUTF8 ("Macros (Punch, Dirt, Tail, Width) for quick moves, CLIP for loudness, then DRAG KICK straight onto a track in your DAW.") },
        });
        col (r, {
            { "KNOBS", String::fromUTF8 ("Drag up/down  \xc2\xb7  Shift: fine  \xc2\xb7  double-click: default  \xc2\xb7  click the value to type it.") },
            { "EQ DISPLAY", String::fromUTF8 ("Double-click: add band  \xc2\xb7  drag: frequency/gain  \xc2\xb7  wheel: Q / slope  \xc2\xb7  Alt-click: bypass  \xc2\xb7  right-click: shape, placement, delete  \xc2\xb7  Delete key: delete band.") },
            { "DRAW ENVELOPES / X-SHAPER", String::fromUTF8 ("Drag points; drag the diamonds to bend a segment  \xc2\xb7  double-click: add or delete a point  \xc2\xb7  right-click: preset shapes.") },
            { "SHORTCUTS", cmd + "Z undo" + dot + cmd + "Shift+Z redo" + dot + cmd + "B browse presets" + dot + cmd + "S save preset" + dot + cmd + "Left/Right previous/next preset." },
            { "HELP", String::fromUTF8 ("Hover over any control for an explanation (menu: Show tooltips).") },
        });
    }

    void layoutCard (juce::Rectangle<int> c) override { closeButton.setBounds (c.getRight() - 46, c.getY() + 18, 28, 28); }

    IconButton closeButton { "close", Icons::close(), false };
};
