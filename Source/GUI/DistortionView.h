#pragma once
#include "Widgets.h"
#include "PresetBar.h"
#include "../PluginProcessor.h"
#include "CurveEditor.h"

/**
    Live transfer curve (input -> output) of one distortion stage, with the current input level marked.
    In the X-Shaper modes it shows the drawable curve itself: drag the points (Shift: fine), double-click a point
    to reset it, right-click for curve presets.
    In every Type the curve can also be drawn freely (custom curve, see CurveEditLogic): drag on the curve,
    double-click, or use the pencil. The CURVE pill switches the unit between its Type and the drawn curve.
*/
class TransferDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    TransferDisplay (CodexProcessor& processor, int stageNumber)
        : proc (processor), apvts (processor.apvts), stage (stageNumber), editor (processor, stageNumber, *this)
    {
        auto& s = apvts;
        auto p = [&] (const char* sfx) { return s.getRawParameterValue (ID::clip (stage, sfx)); };
        mode = p ("Mode"); drive = p ("Drive"); boost = p ("Boost"); offset = p ("Bias"); chr = p ("Char"); on = p ("On");
        tailDrv = p ("TailDrv");
        for (int k = 0; k < ClipDistortion::numCurvePoints; ++k)
        {
            xp[(size_t) k] = s.getParameter (ID::clip (stage, ("XP" + juce::String (k + 1)).toRawUTF8()));
            xn[(size_t) k] = s.getParameter (ID::clip (stage, ("XN" + juce::String (k + 1)).toRawUTF8()));
        }
        editor.legacyCurveY = [this] (float x) { return legacyY (x); };
        editor.legacyGain = [this] { return isShaper() ? 1.0f : juce::Decibels::decibelsToGain (drive->load() + boost->load()); };
        editor.legacyOffset = [this] { return isShaper() ? 0.0f : offset->load(); };

        // toolbar: CURVE (custom on/off) | SYM ASYM, pencil, shapes menu. Plain buttons: they never take the keyboard.
        curveButton.setComponentID ("pill");
        curveButton.setClickingTogglesState (false);
        curveButton.setTooltip (Help::forParameter (ID::clip (stage, "Cust")));
        curveButton.onClick = [this]
        {
            if (editor.isActive()) editor.turnOff();
            else proc.beginCustomEdit (stage, "Custom curve on");
            editor.sync();
            refreshButtons();
            repaint();
        };
        pencilButton.setClickingTogglesState (true);
        pencilButton.setTooltip ("Pencil: draw the curve freehand. The stroke replaces the points under it (one undo step).");
        pencilButton.onClick = [this] { editor.pencil = pencilButton.getToggleState(); repaint(); };
        shapesButton.setTooltip ("Curve menu: starter shapes, re-seed from the Type, symmetry, flip, reset.");
        shapesButton.onClick = [this] { showCurveMenu ({}, true); };
        symButton.setComponentID ("seg");
        asymButton.setComponentID ("seg");
        symButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        asymButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        symButton.setTooltip ("Symmetric: draw the positive half, the negative half mirrors it.");
        asymButton.setTooltip ("Asymmetric: draw both halves independently (even harmonics).");
        symButton.onClick = [this] { editor.setSymmetric (true); refreshButtons(); };
        asymButton.onClick = [this] { editor.setSymmetric (false); refreshButtons(); };
        for (auto* b : std::initializer_list<juce::Component*> { &curveButton, &pencilButton, &shapesButton })
            addAndMakeVisible (b);
        addChildComponent (symButton);
        addChildComponent (asymButton);
        refreshButtons();
    }

    void setInputLevel (float linear) { level = linear > level ? linear : level * 0.88f; repaint(); }

    /** Called by the unit's timer: follows undo / redo, presets and A/B. */
    void tick()
    {
        if (editor.sync()) repaint();
        refreshButtons();
    }

    bool isShaper() const { const int m = (int) mode->load(); return m == ClipDistortion::XShaper || m == ClipDistortion::XShaperAsym; }
    bool isAsym() const { return (int) mode->load() == ClipDistortion::XShaperAsym; }
    bool isCustom() const { return editor.isActive(); }

    ClipDistortion::Curve curve() const
    {
        ClipDistortion::Curve c;
        for (int k = 0; k < ClipDistortion::numCurvePoints; ++k)
        {
            c.pos[(size_t) k] = xp[(size_t) k]->convertFrom0to1 (xp[(size_t) k]->getValue());
            c.neg[(size_t) k] = xn[(size_t) k]->convertFrom0to1 (xn[(size_t) k]->getValue());
        }
        return c;
    }

    // tests
    CurveEditLogic& logic() { return editor; }
    int getStage() const { return stage; }
    juce::Point<float> plotToScreen (float u, float y) const { return editor.toScreen (u, y); }
    juce::Button& getPencilButton() { return pencilButton; }
    juce::Button& getCurveButton() { return curveButton; }

    void resized() override
    {
        auto band = getLocalBounds().reduced (10, 0).withTrimmedTop (5).withHeight (18);
        curveButton.setBounds (band.removeFromLeft (54));
        shapesButton.setBounds (band.removeFromRight (20).withSizeKeepingCentre (20, 20));
        band.removeFromRight (4);
        pencilButton.setBounds (band.removeFromRight (20).withSizeKeepingCentre (20, 20));
        band.removeFromRight (6);
        asymButton.setBounds (band.removeFromRight (40));
        symButton.setBounds (band.removeFromRight (36));
        updatePlot();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        const Px::Snap px (g, *this);
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Colours_::outline);
        px.border (r, 5.0f);

        updatePlot();
        const float size = plot.getWidth();

        // grid
        g.setColour (Colours_::grid);
        for (int i = 1; i < 8; ++i)
        {
            const float t = (float) i / 8.0f;
            px.vline (plot.getX() + t * size, plot.getY(), plot.getBottom());
            px.hline (plot.getY() + t * size, plot.getX(), plot.getRight());
        }
        g.setColour (Colours_::grid.brighter (0.6f));
        px.vline (plot.getCentreX(), plot.getY(), plot.getBottom());
        px.hline (plot.getCentreY(), plot.getX(), plot.getRight());

        // unity reference
        Path diag;
        diag.startNewSubPath (plot.getX(), plot.getBottom());
        diag.lineTo (plot.getRight(), plot.getY());
        float dashes[] = { 4.0f, 4.0f };
        PathStrokeType (1.0f).createDashedStroke (diag, diag, dashes, 2);
        g.setColour (Colours_::textDim.withAlpha (0.35f));
        g.fillPath (diag);

        const bool enabled = on->load() > 0.5f;
        const auto col = enabled ? Colours_::accent : Colours_::textDim;
        if (editor.isActive())
        {
            CurveEditLogic::PaintInfo info;
            info.colour = col;
            info.enabled = enabled;
            info.driveGain = (int) mode->load() == ClipDistortion::Downsample ? 1.0f : Decibels::decibelsToGain (drive->load() + boost->load());
            info.offset = offset->load();
            info.inputLevel = level / jmax (1.0e-6f, Decibels::decibelsToGain (boost->load()));
            editor.paint (g, info);
        }
        else
            paintLegacy (g, col, enabled);

        g.setFont (Fonts::label (Fonts::Size::micro, 0.1f));
        g.setColour (Colours_::textDim);
        auto pl = plot;
        g.drawText ("IN", pl.removeFromBottom (14.0f).removeFromRight (22.0f), Justification::centredRight);
        g.drawText ("OUT", Rectangle<float> (plot.getX() + 4.0f, plot.getY(), 32.0f, 14.0f), Justification::centredLeft);
    }

    // ---- mouse: legacy X-Shaper handles first (Custom off), then the free curve editor ----
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = ! editor.isActive() && isShaper() ? hitPoint (e.position) : -1;
        if (h != hover) { hover = h; repaint(); }
        hovering = true;
        editor.mouseMove (e.position);
        setMouseCursor (h >= 0 ? juce::MouseCursor::UpDownResizeCursor : editor.cursor());
    }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; hovering = false; editor.mouseExit(); repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        if (e.mods.isPopupMenu())
        {
            const auto h = editor.isActive() ? editor.hitTest (e.position) : CurveEditLogic::Handle {};
            showCurveMenu (h, false);
            return;
        }
        if (! editor.isActive() && isShaper() && ! editor.pencil)
        {
            dragging = hitPoint (e.position);
            if (dragging >= 0) { param (dragging)->beginChangeGesture(); lastY = e.position.y; return; }
        }
        editor.mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) { editor.mouseDrag (e); return; }
        auto* p = param (dragging);
        const bool negHalf = dragging >= ClipDistortion::numCurvePoints;
        const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
        const float dy = -(e.position.y - lastY) / (plot.getHeight() * 0.5f) * fine * (negHalf ? -1.0f : 1.0f);
        lastY = e.position.y;
        const float v = juce::jlimit (-1.0f, 1.0f, p->convertFrom0to1 (p->getValue()) + dy);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
        repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging >= 0) param (dragging)->endChangeGesture();
        else editor.mouseUp (e);
        dragging = -1;
        refreshButtons();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! editor.isActive() && isShaper())
        {
            if (const int h = hitPoint (e.position); h >= 0)
            {
                auto* p = param (h);
                p->beginChangeGesture(); p->setValueNotifyingHost (p->getDefaultValue()); p->endChangeGesture();
                return;
            }
        }
        editor.mouseDoubleClick (e);
        refreshButtons();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! editor.mouseWheel (e.position, w.deltaY))
            juce::Component::mouseWheelMove (e, w);
    }

private:
    void updatePlot()
    {
        plot = getLocalBounds().toFloat().reduced (12.0f).withTrimmedTop (16.0f);
        const float size = juce::jmin (plot.getWidth(), plot.getHeight());
        plot = plot.withSizeKeepingCentre (size, size);
        editor.plot = plot;
    }

    void refreshButtons()
    {
        const bool active = editor.isActive();
        curveButton.setToggleState (proc.isCustomEnabled (stage) && editor.hasStoredCurve(), juce::dontSendNotification);
        symButton.setVisible (active);
        asymButton.setVisible (active);
        symButton.setToggleState (active && editor.getCurve().symmetric, juce::dontSendNotification);
        asymButton.setToggleState (active && ! editor.getCurve().symmetric, juce::dontSendNotification);
        if (pencilButton.getToggleState() != editor.pencil) pencilButton.setToggleState (editor.pencil, juce::dontSendNotification);
    }

    /** The shown legacy curve (Custom off) at display x. */
    float legacyY (float x) const
    {
        const bool enabled = on->load() > 0.5f, shaper = isShaper();
        const float gain = shaper ? 1.0f : juce::Decibels::decibelsToGain (drive->load() + boost->load());
        const float off = shaper ? 0.0f : offset->load(), c = shaper ? 0.0f : chr->load();
        const auto cv = curve();
        return enabled || shaper ? ClipDistortion::transfer ((int) mode->load(), x, gain, off, c, &cv) : x;
    }

    void paintLegacy (juce::Graphics& g, juce::Colour col, bool enabled)
    {
        using namespace juce;
        const int m = (int) mode->load();
        const bool shaper = isShaper();
        // X-Shaper: show the drawn curve over its own input range; otherwise the full curve including drive
        const float gain = shaper ? 1.0f : Decibels::decibelsToGain (drive->load() + boost->load());
        const float off = shaper ? 0.0f : offset->load(), c = shaper ? 0.0f : chr->load();
        const auto cv = curve();

        Path curvePath;
        const int steps = (int) plot.getWidth() * 2;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = -1.0f + 2.0f * (float) i / (float) steps;
            const auto pt = toPoint (x, enabled || shaper ? ClipDistortion::transfer (m, x, gain, off, c, &cv) : x);
            if (i == 0) curvePath.startNewSubPath (pt); else curvePath.lineTo (pt);
        }
        // the tail's curve (Tail Drive), dashed, behind the punch curve
        if (enabled && ! shaper && std::abs (tailDrv->load()) > 0.05f)
        {
            const float tailGain = Decibels::decibelsToGain (drive->load() + boost->load() + tailDrv->load());
            Path tail;
            for (int i = 0; i <= steps; ++i)
            {
                const float x = -1.0f + 2.0f * (float) i / (float) steps;
                const auto pt = toPoint (x, ClipDistortion::transfer (m, x, tailGain, off, c, &cv));
                if (i == 0) tail.startNewSubPath (pt); else tail.lineTo (pt);
            }
            float dash[] = { 5.0f, 4.0f };
            PathStrokeType (1.4f).createDashedStroke (tail, tail, dash, 2);
            g.setColour (Colours_::accent2.withAlpha (0.85f));
            g.fillPath (tail);
            g.setFont (Fonts::label (Fonts::Size::micro, 0.08f));
            g.drawText ("TAIL", Rectangle<float> (plot.getRight() - 44.0f, plot.getY() + 15.0f, 40.0f, 14.0f), Justification::centredRight);
        }
        Colours_::strokeGlow (g, curvePath, col, 2.0f);

        g.setFont (Fonts::label (Fonts::Size::micro, 0.06f));
        g.setColour (Colours_::textMuted);
        const auto hintArea = Rectangle<float> (plot.getX() + 4.0f, plot.getY() + 16.0f, plot.getWidth() * 0.6f, 14.0f);
        if (shaper)
        {
            for (int k = 0; k < ClipDistortion::numCurvePoints * 2; ++k)
            {
                const auto pt = pointPosition (k);
                const bool mirrored = k >= ClipDistortion::numCurvePoints && ! isAsym();
                const bool hi = k == hover || k == dragging;
                if (hi) { g.setColour (col.withAlpha (0.25f)); g.fillEllipse (Rectangle<float> (18.0f, 18.0f).withCentre (pt)); }
                g.setColour (mirrored ? col.withAlpha (0.35f) : col);
                g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (pt));
                g.setColour (Colours_::display);
                g.drawEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (pt), 1.2f);
            }
            g.setFont (Fonts::label (Fonts::Size::micro, 0.06f));
            g.setColour (Colours_::textMuted);
            // upper-left quadrant: the one area a drawn curve never reaches
            g.drawText (isAsym() ? "DRAW BOTH HALVES" : "DRAW CURVE  (MIRRORED)", hintArea, Justification::centredLeft);
        }
        else if (hovering)
            g.drawText (CurveEditLogic::drawHint(), hintArea, Justification::centredLeft);

        // current input level (pre-drive, with boost) marked on both sides of the curve
        const float lv = jlimit (0.0f, 1.0f, level);
        if (lv > 0.001f && enabled && ! shaper)
        {
            const float rawIn = lv / jmax (1.0e-6f, Decibels::decibelsToGain (boost->load()));
            for (float sgn : { -1.0f, 1.0f })
            {
                const float x = sgn * jlimit (0.0f, 1.0f, rawIn);
                const auto pt = toPoint (x, ClipDistortion::transfer (m, x, gain, off, c, &cv));
                g.setColour (Colours::white.withAlpha (0.25f));
                g.fillEllipse (Rectangle<float> (14.0f, 14.0f).withCentre (pt));
                g.setColour (Colours::white);
                g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (pt));
            }
        }
    }

    juce::Point<float> toPoint (float x, float y) const
    {
        return { juce::jmap (x, -1.0f, 1.0f, plot.getX(), plot.getRight()),
                 juce::jmap (juce::jlimit (-1.05f, 1.05f, y), -1.0f, 1.0f, plot.getBottom(), plot.getY()) };
    }
    /** Points 0-7: positive half; 8-15: negative half (mirrors of 0-7 unless Asym). */
    juce::Point<float> pointPosition (int k) const
    {
        const auto cv = curve();
        const int i = k % ClipDistortion::numCurvePoints;
        const float x = (float) (i + 1) / (float) ClipDistortion::numCurvePoints;
        if (k < ClipDistortion::numCurvePoints) return toPoint (x, cv.pos[(size_t) i]);
        return toPoint (-x, -(isAsym() ? cv.neg[(size_t) i] : cv.pos[(size_t) i]));
    }
    juce::RangedAudioParameter* param (int k) const
    {
        const int i = k % ClipDistortion::numCurvePoints;
        return k >= ClipDistortion::numCurvePoints && isAsym() ? xn[(size_t) i] : xp[(size_t) i];
    }
    int hitPoint (juce::Point<float> p) const
    {
        int best = -1;
        float bestD = 10.0f;
        for (int k = 0; k < ClipDistortion::numCurvePoints * 2; ++k)
            if (const float d = pointPosition (k).getDistanceFrom (p); d < bestD) { bestD = d; best = k; }
        return best;
    }

    /** Right-click / shapes button: point or segment menu over a handle, otherwise the curve menu (in the X-Shaper
        Types with the X-Shaper presets first while the custom curve is off). */
    void showCurveMenu (CurveEditLogic::Handle h, bool fromButton)
    {
        juce::PopupMenu m;
        const bool legacySection = ! editor.isActive() && isShaper() && h.kind == CurveEditLogic::Handle::None;
        if (h.kind == CurveEditLogic::Handle::Point) editor.fillPointMenu (m, h.index);
        else if (h.kind == CurveEditLogic::Handle::Diamond) editor.fillDiamondMenu (m);
        else
        {
            if (legacySection)
            {
                m.addSectionHeader ("X-Shaper curve");
                const char* names[] = { "Linear", "Soft Saturation", "Hard Knee", "Expander (Punch)", "Fold", "Steps", "Mirror to Negative Half" };
                for (int i = 0; i < 7; ++i) m.addItem (i + 1, names[i], i < 6 || isAsym());
            }
            editor.fillCurveMenu (m);
        }
        juce::Component::SafePointer<TransferDisplay> safe (this);
        auto opts = juce::PopupMenu::Options().withTargetComponent (fromButton ? static_cast<juce::Component*> (&shapesButton) : this);
        if (! fromButton) opts = opts.withMousePosition();
        m.showMenuAsync (opts, [safe, h] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r < 100) safe->applyPreset (r - 1);
            else safe->editor.handleMenu (r, h);
            safe->refreshButtons();
            safe->repaint();
        });
    }

    void applyPreset (int preset)
    {
        const int n = ClipDistortion::numCurvePoints;
        auto set = [] (juce::RangedAudioParameter* p, float v)
        {
            p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (-1.0f, 1.0f, v))); p->endChangeGesture();
        };
        Params::beginTransaction (apvts, "X-Shaper curve");
        for (int k = 0; k < n; ++k)
        {
            const float x = (float) (k + 1) / (float) n;
            float y = x;
            switch (preset)
            {
                case 1: y = std::tanh (2.5f * x) / std::tanh (2.5f); break;
                case 2: y = juce::jmin (1.0f, x * 1.6f); break;
                case 3: y = x * x * (1.5f - 0.5f * x) + 0.0f; break;
                case 4: y = std::sin (x * juce::MathConstants<float>::pi * 1.5f); break;
                case 5: y = std::ceil (x * 3.0f) / 3.0f; break;
                case 6: set (xn[(size_t) k], xp[(size_t) k]->convertFrom0to1 (xp[(size_t) k]->getValue())); continue;
                default: break;
            }
            set (xp[(size_t) k], y);
            if (isAsym() && preset != 6) set (xn[(size_t) k], y);
        }
    }

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    int stage;
    CurveEditLogic editor;
    std::atomic<float> *mode, *drive, *boost, *offset, *chr, *on, *tailDrv;
    std::array<juce::RangedAudioParameter*, ClipDistortion::numCurvePoints> xp {}, xn {};
    juce::Rectangle<float> plot;
    float level = 0.0f, lastY = 0.0f;
    int hover = -1, dragging = -1;
    bool hovering = false;
    juce::TextButton curveButton { "CURVE" }, symButton { "SYM" }, asymButton { "ASYM" };
    IconButton pencilButton { "Pencil", Icons::pencil() }, shapesButton { "Curve menu", Icons::menu() };
};

/** Type menu of a distortion unit. Remembers that the user opened it (or turned the wheel over it), so a Type the user
    picks can switch the custom curve off, while preset loads, undo and automation never do. */
class ArmedComboBox : public juce::ComboBox
{
public:
    void showPopup() override { arm(); juce::ComboBox::showPopup(); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        arm();   // expires shortly after if the wheel changed nothing
        juce::ComboBox::mouseWheelMove (e, w);
    }
    void arm() { armed = true; armedAt = juce::Time::getMillisecondCounter(); }
    /** Called by the owner's timer: the arm expires shortly after the popup closes. */
    void expire()
    {
        if (armed && ! isPopupActive() && juce::Time::getMillisecondCounter() - armedAt > 300) armed = false;
    }
    bool armed = false;
    juce::uint32 armedAt = 0;
};

/** ChoiceBox with an ArmedComboBox (the distortion Type). */
class TypeChoiceBox : public juce::Component
{
public:
    explicit TypeChoiceBox (const juce::String& caption)
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
    ArmedComboBox box;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

/**
    Response of the distortion's resonant filter. Drag horizontally for the frequency and vertically for
    the resonance; mouse wheel: resonance; double-click: reset.
*/
class DistortionFilterStrip : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    DistortionFilterStrip (juce::AudioProcessorValueTreeState& s, int stage) : apvts (s)
    {
        freq = s.getParameter (ID::clip (stage, "FltFreq"));
        res = s.getParameter (ID::clip (stage, "FltRes"));
        pos = s.getRawParameterValue (ID::clip (stage, "FltPos"));
        type = s.getRawParameterValue (ID::clip (stage, "FltType"));
        setTooltip ("Distortion filter: drag left/right for the frequency, up/down for the resonance (mouse wheel: resonance, double-click: reset). Choose PRE or POST above.");
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Colours_::outline);
        Px::Snap (g, *this).border (r, 5.0f);
        plot = r.reduced (8.0f, 6.0f);

        const bool active = pos->load() > 0.5f;
        const int t = (int) type->load();
        const float fc = value (freq), rv = value (res);
        const float k = 1.0f / (0.5f + 17.5f * rv * rv);

        juce::Path p;
        for (float x = plot.getX(); x <= plot.getRight(); x += 1.0f)
        {
            const float f = 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth());
            const float w = f / fc, re = 1.0f - w * w, den = std::sqrt (re * re + k * k * w * w);
            float mag = 1.0f;
            switch (t)
            {
                case ClipDistortion::LowPass12:  mag = 1.0f / den; break;
                case ClipDistortion::LowPass24:  mag = 1.0f / (den * den); break;
                case ClipDistortion::BandPass:   mag = k * w / den; break;
                case ClipDistortion::HighPass12: mag = w * w / den; break;
                case ClipDistortion::HighPass24: mag = (w * w / den) * (w * w / den); break;
                case ClipDistortion::Notch:      mag = std::abs (re) / den; break;
                default: break;
            }
            const float db = juce::Decibels::gainToDecibels (mag, -60.0f);
            const float y = juce::jmap (juce::jlimit (-36.0f, 24.0f, db), -36.0f, 24.0f, plot.getBottom(), plot.getY());
            if (x <= plot.getX()) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        const auto col = active ? Colours_::accent2 : Colours_::textFaint.withMultipliedAlpha (0.55f);
        juce::Path fill (p);
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.lineTo (plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setColour (col.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (1.5f));

        const float hx = juce::jmap (std::log (fc / 20.0f) / std::log (1000.0f), plot.getX(), plot.getRight());
        g.setColour (col);
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ hx, juce::jmap (rv, plot.getBottom() - 4.0f, plot.getY() + 4.0f) }));

        g.setFont (Fonts::label (Fonts::Size::micro, 0.06f));
        g.setColour (active ? Colours_::textDim : Colours_::textMuted);
        const juce::String where = active ? (pos->load() > 1.5f ? "POST" : "PRE") : "OFF";
        g.drawText ("FILTER " + where + "   " + freq->getCurrentValueAsText() + "   RES " + res->getCurrentValueAsText(),
                    plot.reduced (4.0f, 0.0f), juce::Justification::topLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        freq->beginChangeGesture(); res->beginChangeGesture();
        mouseDrag (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float f = 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (e.position.x - plot.getX()) / plot.getWidth()));
        const float rv = juce::jlimit (0.0f, 1.0f, (plot.getBottom() - e.position.y) / plot.getHeight());
        freq->setValueNotifyingHost (freq->convertTo0to1 (f));
        res->setValueNotifyingHost (res->convertTo0to1 (rv));
    }
    void mouseUp (const juce::MouseEvent&) override { freq->endChangeGesture(); res->endChangeGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        for (auto* p : { freq, res }) { p->beginChangeGesture(); p->setValueNotifyingHost (p->getDefaultValue()); p->endChangeGesture(); }
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        res->beginChangeGesture();
        res->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, res->getValue() + w.deltaY * 0.25f));
        res->endChangeGesture();
    }

private:
    static float value (juce::RangedAudioParameter* p) { return p->convertFrom0to1 (p->getValue()); }
    void timerCallback() override
    {
        const float h = value (freq) * 3.1f + value (res) * 7.3f + pos->load() * 11.0f + type->load() * 13.0f;
        if (! juce::exactlyEqual (h, lastHash)) { lastHash = h; repaint(); }
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::RangedAudioParameter *freq, *res;
    std::atomic<float> *pos, *type;
    juce::Rectangle<float> plot;
    float lastHash = -1.0f;
};

/** Full Distroyer-style distortion unit for one stage. */
class DistortionUnit : public juce::Component, private juce::Timer
{
public:
    DistortionUnit (CodexProcessor& p, int stageNumber, const juce::String& panelTitle)
        : proc (p), stage (stageNumber), title (panelTitle), transfer (p, stageNumber), filterStrip (p.apvts, stageNumber),
          filterPos (*p.apvts.getParameter (ID::clip (stageNumber, "FltPos")), { "OFF", "PRE", "POST" }, Help::forParameter (ID::clip (stageNumber, "FltPos")))
    {
        auto& s = p.apvts;
        onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clip (stage, "On"), power);
        dcAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clip (stage, "DC"), dcButton);
        autoGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clip (stage, "AutoGain"), autoGainButton);
        autoGainButton.setClickingTogglesState (true);
        autoGainButton.setComponentID ("pill");
        autoGainButton.setTooltip (Help::forParameter (ID::clip (stage, "AutoGain")));
        dcButton.setClickingTogglesState (true);
        dcButton.setComponentID ("pill");
        dcButton.setTooltip (Help::forParameter (ID::clip (stage, "DC")));
        power.setTooltip (Help::forParameter (ID::clip (stage, "On")));

        type.attach (s, ID::clip (stage, "Mode"));
        // a Type the user picks plays that Type: the custom curve switches off (kept, the CURVE pill brings it back)
        type.box.onChange = [this]
        {
            if (type.box.armed && proc.isCustomEnabled (stage))
            {
                Params::beginTransaction (proc.apvts, "Change distortion type");
                proc.setCustomEnabled (stage, false);
            }
            type.box.armed = false;
        };
        oversampling.attach (s, ID::clip (stage, "OS"));
        filterType.attach (s, ID::clip (stage, "FltType"));
        drive.attach (s, ID::clip (stage, "Drive"));
        boost.attach (s, ID::clip (stage, "Boost"));
        character.attach (s, ID::clip (stage, "Char"));
        offset.attach (s, ID::clip (stage, "Bias"));
        spatial.attach (s, ID::clip (stage, "Spatial"));
        tailDrive.attach (s, ID::clip (stage, "TailDrv"));
        split.attach (s, ID::clip (stage, "Split"));
        loFilter.attach (s, ID::clip (stage, "LoFilt"));
        hiFilter.attach (s, ID::clip (stage, "HiFilt"));
        shelfFreq.attach (s, ID::clip (stage, "ShelfF"));
        shelfGain.attach (s, ID::clip (stage, "ShelfG"));
        mix.attach (s, ID::clip (stage, "Mix"));
        output.attach (s, ID::clip (stage, "Out"));

        drive.label.getProperties().set ("captionLarge", true);
        drive.label.setColour (juce::Label::textColourId, Colours_::accent);

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &power, &dcButton, &autoGainButton, &type, &oversampling, &filterPos, &filterType, &filterStrip, &transfer, &drive, &boost, &character, &offset, &spatial, &tailDrive, &split,
                 &loFilter, &hiFilter, &shelfFreq, &shelfGain, &mix, &output, &inMeter, &outMeter })
            addAndMakeVisible (c);

        // Shown while the unit is bypassed, so a switched-off stage is impossible to miss
        bypassNotice.enable.onClick = [this, &s]
        {
            if (auto* prm = s.getParameter (ID::clip (stage, "On")))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (1.0f);
                prm->endChangeGesture();
            }
        };
        bypassNotice.title = "DISTORTION " + juce::String (stage) + " IS BYPASSED";
        addChildComponent (bypassNotice);
        onParam = s.getRawParameterValue (ID::clip (stage, "On"));

        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionPanel (g, *this, getLocalBounds(), title, bypassed ? Colours_::textFaint : Colours_::accent);
        if (bypassed)
        {
            auto tag = getLocalBounds().withHeight (28).withTrimmedRight (140).removeFromRight (86).reduced (0, 6).toFloat();
            g.setColour (Colours_::warn.withAlpha (0.18f));
            g.fillRoundedRectangle (tag, tag.getHeight() * 0.5f);
            g.setColour (Colours_::warn);
            g.drawRoundedRectangle (tag, tag.getHeight() * 0.5f, 1.0f);
            g.setFont (Fonts::label (Fonts::Size::micro, 0.12f));
            g.drawText ("BYPASSED", tag, juce::Justification::centred);
        }
        drawGroupCaption (g, *this, shapeCaption, "SHAPE");
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (Fonts::Size::caption, 0.08f));
        g.drawText ("FILTER", filterPos.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);
        drawGroupCaption (g, *this, filterCaption, "WET FILTERS  /  TONE");
        drawGroupCaption (g, *this, outputCaption, "OUTPUT");

        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (Fonts::Size::micro, 0.12f));
        auto labelUnder = [] (juce::Rectangle<int> m) { return m.withSizeKeepingCentre (40, m.getHeight()).translated (0, m.getHeight() + 2).withHeight (14); };
        g.drawText ("IN", labelUnder (inMeter.getBounds()), juce::Justification::centred);
        g.drawText ("OUT", labelUnder (outMeter.getBounds()), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        power.setBounds (r.withHeight (28).removeFromRight (32).reduced (2));

        auto c = r.reduced (14).withTrimmedTop (28);

        // DC filter as a compact pill in the header
        dcButton.setBounds (r.withHeight (28).withTrimmedRight (38).removeFromRight (40).reduced (0, 5));
        autoGainButton.setBounds (dcButton.getBounds().translated (-52, 0).withWidth (48));

        // Top row: type, oversampling | filter position, filter type
        auto top = c.removeFromTop (48);
        const int tw = top.getWidth();
        type.setBounds (top.removeFromLeft (tw * 30 / 100));
        top.removeFromLeft (8);
        oversampling.setBounds (top.removeFromLeft (tw * 15 / 100));
        top.removeFromLeft (14);
        filterPos.setBounds (top.removeFromLeft (tw * 27 / 100).withTrimmedTop (18).withHeight (28));
        top.removeFromLeft (8);
        filterType.setBounds (top);
        c.removeFromTop (12);

        // Bottom: one row of six knobs (filters / tone | output)
        auto bottomRow = c.removeFromBottom (96);
        auto captions = c.removeFromBottom (16);
        c.removeFromBottom (8);
        const int w6 = bottomRow.getWidth() / 6;
        filterCaption = captions.removeFromLeft (w6 * 4).withTrimmedRight (16);
        outputCaption = captions;
        for (auto* k : { &loFilter, &hiFilter, &shelfFreq, &shelfGain, &mix })
            k->setBounds (bottomRow.removeFromLeft (w6));
        output.setBounds (bottomRow);

        // Middle: meters around the transfer display | drive (moderate size) + 2x2 shape knobs
        auto middle = c;
        const int displaySize = juce::jmin (middle.getHeight() - 16, middle.getWidth() / 2 + 40);
        auto left = middle.removeFromLeft (displaySize + 44);
        inMeter.setBounds (left.removeFromLeft (12).withTrimmedBottom (16));
        outMeter.setBounds (left.removeFromRight (12).withTrimmedBottom (16));
        {
            auto t = left.reduced (8, 0).withTrimmedBottom (16);
            filterStrip.setBounds (t.removeFromBottom (58));
            t.removeFromBottom (6);
            transfer.setBounds (t);
        }
        middle.removeFromLeft (16);
        const int driveH = juce::jmin (150, middle.getHeight() / 2);
        drive.setBounds (middle.removeFromTop (driveH).withSizeKeepingCentre (juce::jmin (middle.getWidth(), 130), driveH));
        middle.removeFromTop (6);
        shapeCaption = middle.removeFromTop (16);
        auto grid = middle;
        const int rowH = juce::jmin (100, grid.getHeight() / 2);
        auto row1 = grid.removeFromTop (rowH), row2 = grid.removeFromTop (rowH);
        const int cw = row1.getWidth() / 3;
        boost.setBounds (row1.removeFromLeft (cw)); character.setBounds (row1.removeFromLeft (cw)); offset.setBounds (row1);
        spatial.setBounds (row2.removeFromLeft (cw)); tailDrive.setBounds (row2.removeFromLeft (cw)); split.setBounds (row2);

        bypassNotice.setBounds (getLocalBounds().withTrimmedTop (29).reduced (1));
    }

private:
    void timerCallback() override
    {
        auto& clip = proc.getClip (stage);
        const float in = clip.getAndResetInputPeak();
        inMeter.setLevel (in);
        outMeter.setLevel (clip.getAndResetOutputPeak());
        transfer.setInputLevel (in);
        transfer.tick();
        type.box.expire();
        const int tipState = transfer.isCustom() ? 2 : transfer.isShaper() ? 1 : 0;
        if (tipState != lastTipState)
        {
            lastTipState = tipState;
            transfer.setTooltip (tipState == 2 ? Help::customCurveEditor()
                                 : tipState == 1 ? juce::String ("X-Shaper: drag the points to draw the curve (Shift: fine), double-click a point to reset it, right-click for preset curves. Drag elsewhere on the curve, double-click empty space or use the pencil to draw a free custom curve.")
                                                 : Help::customCurveStart());
        }

        const bool off = onParam->load() < 0.5f;
        if (off != bypassed)
        {
            bypassed = off;
            bypassNotice.setVisible (off);
            resized();
            repaint();
        }
    }


    CodexProcessor& proc;
    int stage;
    juce::String title;

    PowerButton power;
    juce::TextButton dcButton { "DC" }, autoGainButton { "AUTO" };
    TypeChoiceBox type { "Type" };
    ChoiceBox oversampling { "Oversampling" }, filterType { "Filter Type" };
    TransferDisplay transfer;
    DistortionFilterStrip filterStrip;
    SegmentedChoice filterPos;
    Knob drive { "Drive" }, boost { "Boost" }, character { "Character" }, offset { "Offset" }, spatial { "Spatial" },
         tailDrive { "Tail Drive" }, split { "Split" };
    Knob loFilter { "Lo Filter" }, hiFilter { "Hi Filter" }, shelfFreq { "Shelf Freq" }, shelfGain { "Shelf Gain" };
    Knob mix { "Mix" }, output { "Output" };
    LevelMeter inMeter, outMeter;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment, dcAttachment, autoGainAttachment;
    juce::Rectangle<int> shapeCaption, filterCaption, outputCaption;
    BypassNotice bypassNotice;
    std::atomic<float>* onParam = nullptr;
    bool bypassed = false;
    int lastTipState = -1;
};
