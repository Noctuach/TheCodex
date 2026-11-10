#pragma once
#include "Widgets.h"
#include "../DSP/DrawEnvelope.h"

/**
    Breakpoint envelope editor (draw mode).
      - Drag a point: time and level (Shift: fine). The first point stays at 0 ms.
      - Drag the small diamond in the middle of a segment up/down (or use the mouse wheel over it): bend.
      - Double-click empty space: add a point (up to 8). Double-click a point: delete it.
      - Right-click: shape presets.
*/
class DrawEnvelopeEditor : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    DrawEnvelopeEditor (juce::AudioProcessorValueTreeState& s, char envId, juce::Colour c, const juce::String& valueUnit)
        : apvts (s), env (envId), colour (c), unit (valueUnit)
    {
        setTooltip ("Draw envelope: drag points (Shift: fine), drag the diamonds to bend a segment, double-click to add or delete a point, right-click for shapes.");
        startTimerHz (60);
    }

    std::function<float (float)> levelToText;   // optional: converts a level to the displayed value (e.g. semitones)

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 1.0f);
        plot = area.reduced (10.0f, 9.0f);

        const auto s = shape();
        if (dragging < 0 && bendDrag < 0) viewMs = niceRange (s.points[(size_t) s.count - 1].timeMs);

        // time grid
        g.setFont (Fonts::regular (10.0f));
        const float step = viewMs <= 300 ? 50.0f : viewMs <= 800 ? 100.0f : viewMs <= 2000 ? 250.0f : 500.0f;
        for (float t = step; t < viewMs; t += step)
        {
            const float x = tx (t);
            g.setColour (Colours_::grid);
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (Colours_::textFaint);
            g.drawText (juce::String ((int) t), juce::Rectangle<float> (x + 2, plot.getBottom() - 11, 40, 10), juce::Justification::centredLeft);
        }
        for (int i = 1; i < 4; ++i)
        {
            g.setColour (Colours_::grid);
            g.drawHorizontalLine ((int) (plot.getY() + plot.getHeight() * (float) i / 4.0f), plot.getX(), plot.getRight());
        }

        // curve
        juce::Path p;
        for (float x = plot.getX(); x <= plot.getRight(); x += 1.0f)
        {
            const float y = ly (DrawEnvelope::valueAt (s, xt (x)));
            if (x <= plot.getX()) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        juce::Path fill (p);
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.lineTo (plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.28f), 0, plot.getY(), colour.withAlpha (0.02f), 0, plot.getBottom(), false));
        g.fillPath (fill);
        Colours_::strokeGlow (g, p, colour, 1.6f);

        // bend handles
        for (int i = 0; i < s.count - 1; ++i)
        {
            const auto h = bendHandle (s, i);
            const bool hi = i == hoverBend || i == bendDrag;
            juce::Path d;
            d.addRectangle (-3.5f, -3.5f, 7.0f, 7.0f);
            d.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f).translated (h));
            g.setColour (hi ? colour : colour.withAlpha (0.45f));
            g.fillPath (d);
        }
        // points
        for (int i = 0; i < s.count; ++i)
        {
            const auto pt = point (s, i);
            const bool hi = i == hover || i == dragging;
            if (hi) { g.setColour (colour.withAlpha (0.25f)); g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (pt)); }
            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (pt));
            g.setColour (Colours_::display);
            g.drawEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (pt), 1.2f);
        }

        // readout
        const int shown = dragging >= 0 ? dragging : hover;
        if (shown >= 0 && shown < s.count)
        {
            const auto& pt = s.points[(size_t) shown];
            const float v = levelToText ? levelToText (pt.level) : pt.level * 100.0f;
            const juce::String txt = juce::String (pt.timeMs, pt.timeMs < 10 ? 1 : 0) + " ms   " + juce::String (v, 1) + " " + unit;
            g.setFont (Fonts::medium (11.5f));
            auto box = juce::Rectangle<float> (130.0f, 18.0f).withCentre (point (s, shown).translated (0, -16.0f)).constrainedWithin (plot);
            g.setColour (Colours_::panelTop.withAlpha (0.92f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (Colours_::text);
            g.drawText (txt, box, juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto s = shape();
        const int h = hitPoint (s, e.position), hb = h < 0 ? hitBend (s, e.position) : -1;
        if (h != hover || hb != hoverBend) { hover = h; hoverBend = hb; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : hb >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hover = hoverBend = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto s = shape();
        if (e.mods.isPopupMenu()) { showMenu(); return; }
        dragging = hitPoint (s, e.position);
        bendDrag = dragging < 0 ? hitBend (s, e.position) : -1;
        lastPos = e.position;
        if (dragging >= 0) { gesture (prm ('T', dragging), true); gesture (prm ('L', dragging), true); }
        if (bendDrag >= 0) gesture (prm ('B', bendDrag), true);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto s = shape();
        const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
        const auto d = (e.position - lastPos) * fine;
        lastPos = e.position;
        if (dragging >= 0)
        {
            const auto& pt = s.points[(size_t) dragging];
            if (dragging > 0)
            {
                const float lo = s.points[(size_t) dragging - 1].timeMs;
                const float hi = dragging < s.count - 1 ? s.points[(size_t) dragging + 1].timeMs : 4000.0f;
                set (prm ('T', dragging), juce::jlimit (lo, hi, pt.timeMs + d.x / plot.getWidth() * viewMs));
            }
            set (prm ('L', dragging), juce::jlimit (0.0f, 1.0f, pt.level - d.y / plot.getHeight()));
        }
        else if (bendDrag >= 0)
        {
            const auto& a = s.points[(size_t) bendDrag];
            const auto& b = s.points[(size_t) bendDrag + 1];
            const float dir = b.level < a.level ? 1.0f : -1.0f;   // dragging down always bends towards "fast drop"
            set (prm ('B', bendDrag), juce::jlimit (-1.0f, 1.0f, a.bend + dir * d.y / plot.getHeight() * 2.0f));
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) { gesture (prm ('T', dragging), false); gesture (prm ('L', dragging), false); }
        if (bendDrag >= 0) gesture (prm ('B', bendDrag), false);
        dragging = bendDrag = -1;
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto s = shape();
        const int b = hitBend (s, e.position);
        if (b < 0) return;
        auto* p = prm ('B', b);
        gesture (p, true);
        set (p, juce::jlimit (-1.0f, 1.0f, s.points[(size_t) b].bend + w.deltaY * 0.5f));
        gesture (p, false);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto s = shape();
        const int h = hitPoint (s, e.position);
        if (auto* um = apvts.undoManager) um->beginNewTransaction (h >= 0 ? "Delete envelope point" : "Add envelope point");
        if (h > 0 && s.count > 2)
        {
            for (int i = h; i < s.count - 1; ++i) s.points[(size_t) i] = s.points[(size_t) i + 1];
            s.count--;
            write (s);
        }
        else if (h < 0 && s.count < DrawEnvelope::maxPoints && plot.contains (e.position))
        {
            const float t = xt (e.position.x);
            int at = s.count;
            for (int i = 1; i < s.count; ++i) if (s.points[(size_t) i].timeMs > t) { at = i; break; }
            for (int i = s.count; i > at; --i) s.points[(size_t) i] = s.points[(size_t) i - 1];
            s.points[(size_t) at] = { t, juce::jlimit (0.0f, 1.0f, (plot.getBottom() - e.position.y) / plot.getHeight()), 0.0f };
            s.count++;
            write (s);
        }
    }

private:
    void timerCallback() override
    {
        const auto s = shape();
        if (! (s == last)) { last = s; repaint(); }
    }

    juce::RangedAudioParameter* prm (char what, int i) const { return apvts.getParameter (ID::envPoint (env, what, i)); }
    static void gesture (juce::RangedAudioParameter* p, bool begin) { if (begin) p->beginChangeGesture(); else p->endChangeGesture(); }
    static void set (juce::RangedAudioParameter* p, float v) { p->setValueNotifyingHost (p->convertTo0to1 (v)); }
    static float get (juce::RangedAudioParameter* p) { return p->convertFrom0to1 (p->getValue()); }

    DrawEnvelope::Shape shape() const
    {
        DrawEnvelope::Shape s;
        s.count = (int) apvts.getRawParameterValue (ID::envPoints (env))->load();
        for (int i = 0; i < DrawEnvelope::maxPoints; ++i)
            s.points[(size_t) i] = { get (prm ('T', i)), get (prm ('L', i)), get (prm ('B', i)) };
        return DrawEnvelope::sanitise (s);
    }

    void write (const DrawEnvelope::Shape& s)
    {
        auto* countParam = apvts.getParameter (ID::envPoints (env));
        gesture (countParam, true); set (countParam, (float) s.count); gesture (countParam, false);
        for (int i = 0; i < s.count; ++i)
            for (char w : { 'T', 'L', 'B' })
            {
                auto* p = prm (w, i);
                const auto& pt = s.points[(size_t) i];
                gesture (p, true); set (p, w == 'T' ? pt.timeMs : w == 'L' ? pt.level : pt.bend); gesture (p, false);
            }
    }

    void showMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Envelope shape");
        const bool pitch = env == 'p';
        const char* names[] = { pitch ? "Hardstyle punch" : "Full body", pitch ? "Long zap" : "Short and tight", pitch ? "Rawstyle snap" : "Long tail",
                                pitch ? "Two-stage drop" : "Punch then sustain", "Straight line" };
        for (int i = 0; i < 5; ++i) m.addItem (i + 1, names[i]);
        juce::Component::SafePointer<DrawEnvelopeEditor> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe] (int r)
        {
            if (safe == nullptr || r == 0) return;
            safe->applyPreset (r - 1);
        });
    }

    void applyPreset (int which)
    {
        DrawEnvelope::Shape s;
        auto pts = [&s] (std::initializer_list<DrawEnvelope::Point> list) { s.count = 0; for (auto& p : list) s.points[(size_t) s.count++] = p; };
        if (env == 'p')
        {
            switch (which)
            {
                case 0:  pts ({ { 0, 1.0f, 0.55f }, { 25, 0.42f, 0.45f }, { 110, 0.1f, 0.3f }, { 320, 0.0f, 0 } }); break;
                case 1:  pts ({ { 0, 1.0f, 0.2f }, { 180, 0.25f, 0.4f }, { 600, 0.0f, 0 } }); break;
                case 2:  pts ({ { 0, 1.0f, 0.85f }, { 12, 0.3f, 0.6f }, { 70, 0.05f, 0.2f }, { 200, 0.0f, 0 } }); break;
                case 3:  pts ({ { 0, 1.0f, 0.7f }, { 18, 0.5f, -0.2f }, { 60, 0.42f, 0.6f }, { 220, 0.05f, 0.2f }, { 400, 0.0f, 0 } }); break;
                default: pts ({ { 0, 1.0f, 0.0f }, { 200, 0.0f, 0 } }); break;
            }
        }
        else
        {
            switch (which)
            {
                case 0:  pts ({ { 0, 0.0f, 0 }, { 1, 1.0f, 0.15f }, { 450, 0.82f, 0.35f }, { 1100, 0.0f, 0 } }); break;
                case 1:  pts ({ { 0, 0.0f, 0 }, { 1, 1.0f, 0.4f }, { 250, 0.4f, 0.5f }, { 450, 0.0f, 0 } }); break;
                case 2:  pts ({ { 0, 0.0f, 0 }, { 1, 1.0f, 0.1f }, { 900, 0.75f, 0.3f }, { 2400, 0.0f, 0 } }); break;
                case 3:  pts ({ { 0, 0.0f, 0 }, { 1, 1.0f, 0.6f }, { 60, 0.7f, 0.0f }, { 700, 0.65f, 0.4f }, { 1300, 0.0f, 0 } }); break;
                default: pts ({ { 0, 1.0f, 0.0f }, { 1000, 0.0f, 0 } }); break;
            }
        }
        if (auto* um = apvts.undoManager) um->beginNewTransaction ("Envelope shape");
        write (DrawEnvelope::sanitise (s));
    }

    static float niceRange (float lastMs)
    {
        for (float r : { 200.0f, 300.0f, 500.0f, 800.0f, 1200.0f, 1600.0f, 2500.0f, 4000.0f })
            if (lastMs * 1.15f <= r) return r;
        return 4500.0f;
    }
    float tx (float t) const { return plot.getX() + plot.getWidth() * t / viewMs; }
    float xt (float x) const { return juce::jlimit (0.0f, 4000.0f, (x - plot.getX()) / plot.getWidth() * viewMs); }
    float ly (float l) const { return plot.getBottom() - l * plot.getHeight(); }
    juce::Point<float> point (const DrawEnvelope::Shape& s, int i) const { return { tx (s.points[(size_t) i].timeMs), ly (s.points[(size_t) i].level) }; }
    juce::Point<float> bendHandle (const DrawEnvelope::Shape& s, int i) const
    {
        const auto& a = s.points[(size_t) i];
        const auto& b = s.points[(size_t) i + 1];
        const float t = 0.5f * (a.timeMs + b.timeMs);
        return { tx (t), ly (DrawEnvelope::valueAt (s, t)) };
    }
    int hitPoint (const DrawEnvelope::Shape& s, juce::Point<float> p) const
    {
        int best = -1;
        float bestD = 10.0f;
        for (int i = 0; i < s.count; ++i)
            if (const float d = point (s, i).getDistanceFrom (p); d < bestD) { bestD = d; best = i; }
        return best;
    }
    int hitBend (const DrawEnvelope::Shape& s, juce::Point<float> p) const
    {
        for (int i = 0; i < s.count - 1; ++i)
            if (bendHandle (s, i).getDistanceFrom (p) < 8.0f) return i;
        return -1;
    }

    juce::AudioProcessorValueTreeState& apvts;
    char env;
    juce::Colour colour;
    juce::String unit;
    juce::Rectangle<float> plot;
    float viewMs = 500.0f;
    int hover = -1, hoverBend = -1, dragging = -1, bendDrag = -1;
    juce::Point<float> lastPos;
    DrawEnvelope::Shape last;
};
