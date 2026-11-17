#pragma once
#include "Widgets.h"
#include "../PluginProcessor.h"
#include "../DSP/CurveSeed.h"

/**
    Free-point curve editing on a distortion unit's transfer display (the display owns it and forwards the mouse).
    Not a component, so it never takes the keyboard; every action is a mouse gesture or a menu item.

      - Drag a point: x and y (Shift: fine). Endpoints move in y only; the symmetric origin is fixed.
      - Drag a diamond (or Alt-drag anywhere on a segment), or the mouse wheel over a diamond: bend the segment.
      - Double-click empty space: add a point. Double-click a point: delete it. Double-click a diamond: straight bend.
      - Ctrl/Cmd-click a point: sharp corner on/off. Right-click a point or a diamond: its menu. Right-click elsewhere: curve menu.
      - Pencil mode: draw a stroke; it replaces the points under it when the mouse is released.
    While the unit plays its Type (Custom off), drawing on the curve, double-clicking or a pencil stroke first converts
    the Type into a custom curve seeded from it, in the same undo step.

    Every edit is written to the processor's CustomCurveStore (undoable); the display re-reads the stored text on its
    timer, so undo / redo, A/B, presets and restored sessions always show what the DSP plays.
*/
class CurveEditLogic
{
public:
    CurveEditLogic (CodexProcessor& p, int stageNumber, juce::Component& ownerComponent)
        : proc (p), stage (stageNumber), owner (ownerComponent)
    {
        sync();
    }

    struct Handle
    {
        enum Kind { None = 0, Point, Diamond };
        Kind kind = None;
        int index = -1;          // source point / segment
        bool mirrored = false;   // the odd mirror (symmetric curves)
        bool operator== (const Handle& o) const { return kind == o.kind && index == o.index && mirrored == o.mirrored; }
        bool operator!= (const Handle& o) const { return ! (*this == o); }
    };

    // Legacy display mapping (Custom off): the display shows transfer (x) for x in -1..1 with this gain and offset.
    std::function<float (float)> legacyCurveY;     // y of the shown legacy curve at display x
    std::function<float()> legacyGain, legacyOffset;

    // ---- state ----
    /** Re-reads the stored curve; true when it changed. */
    bool sync()
    {
        const auto t = proc.getCustomCurves().text (stage);
        if (t == cachedText && synced) return false;
        synced = true;
        cachedText = t;
        FreeCurve c;
        hasCurve = t.isNotEmpty() && FreeCurve::fromString (t, c);
        curve = hasCurve ? c : FreeCurve::linear();
        shape.compile (curve);
        return true;
    }
    bool isActive() const { return hasCurve && proc.isCustomEnabled (stage); }
    bool hasStoredCurve() const { return hasCurve; }
    const FreeCurve& getCurve() const { return curve; }
    const FreeCurveShape& getShape() const { return shape; }
    bool pencil = false;

    // ---- geometry ----
    juce::Rectangle<float> plot;
    juce::Point<float> toScreen (float u, float y) const
    {
        return { juce::jmap (u, -1.0f, 1.0f, plot.getX(), plot.getRight()),
                 juce::jmap (juce::jlimit (-1.05f, 1.05f, y), -1.0f, 1.0f, plot.getBottom(), plot.getY()) };
    }
    float screenToU (float x) const { return juce::jlimit (-1.0f, 1.0f, juce::jmap (x, plot.getX(), plot.getRight(), -1.0f, 1.0f)); }
    float screenToY (float y) const { return juce::jlimit (-1.0f, 1.0f, juce::jmap (y, plot.getBottom(), plot.getY(), -1.0f, 1.0f)); }
    float pixelsPerUnit() const { return plot.getWidth() * 0.5f; }

    /** Visual position of a point handle. */
    juce::Point<float> pointPos (int i, bool mirrored) const
    {
        const auto& q = curve.p[(size_t) i];
        return mirrored ? toScreen (-q.x, -q.y) : toScreen (q.x, q.y);
    }
    juce::Point<float> diamondPos (int i, bool mirrored) const
    {
        const float u = 0.5f * (curve.p[(size_t) i].x + curve.p[(size_t) i + 1].x);
        const float y = shape.evalSegment (i, u);
        return mirrored ? toScreen (-u, -y) : toScreen (u, y);
    }
    bool diamondVisible (int i) const
    {
        return (curve.p[(size_t) i + 1].x - curve.p[(size_t) i].x) * pixelsPerUnit() >= 18.0f;
    }

    Handle hitTest (juce::Point<float> pos) const
    {
        Handle best;
        float bestD = 10.0f;
        for (int i = 0; i < curve.count; ++i)
            for (bool m : { false, true })
            {
                if (m && (! curve.symmetric || i == 0)) continue;
                if (const float d = pointPos (i, m).getDistanceFrom (pos); d < bestD) { bestD = d; best = { Handle::Point, i, m }; }
            }
        if (best.kind != Handle::None) return best;
        bestD = 8.0f;
        for (int i = 0; i < curve.count - 1; ++i)
            for (bool m : { false, true })
            {
                if (m && ! curve.symmetric) continue;
                if (! diamondVisible (i)) continue;
                if (const float d = diamondPos (i, m).getDistanceFrom (pos); d < bestD) { bestD = d; best = { Handle::Diamond, i, m }; }
            }
        return best;
    }

    /** Distance (px) from a screen position to the drawn custom curve. */
    float distanceToCurve (juce::Point<float> pos) const
    {
        float best = 1.0e9f;
        for (float dx = -10.0f; dx <= 10.0f; dx += 1.0f)
        {
            const float u = screenToU (pos.x + dx);
            best = std::min (best, toScreen (u, shape.evalU (u)).getDistanceFrom (pos));
        }
        return best;
    }
    float distanceToLegacyCurve (juce::Point<float> pos) const
    {
        if (! legacyCurveY) return 1.0e9f;
        float best = 1.0e9f;
        for (float dx = -10.0f; dx <= 10.0f; dx += 1.0f)
        {
            const float x = screenToU (pos.x + dx);
            best = std::min (best, toScreen (x, legacyCurveY (x)).getDistanceFrom (pos));
        }
        return best;
    }

    /** Segment under a screen x (source index, mirrored when it lies in the symmetric curve's left half). */
    Handle segmentAt (juce::Point<float> pos) const
    {
        float u = screenToU (pos.x);
        const bool m = curve.symmetric && u < 0.0f;
        if (m) u = -u;
        const int i = juce::jlimit (0, curve.count - 2, shape.segmentFor (u));
        return { Handle::Diamond, i, m };
    }

    // ---- commands (menus and tests) ----
    juce::UndoManager& undo() { return proc.undoManager; }

    /** Opens an undo step for an edit, converting the Type into a seeded custom curve first when needed. */
    void beginEdit (const juce::String& name)
    {
        if (! isActive()) proc.beginCustomEdit (stage, name, true);
        else Params::beginTransaction (proc.apvts, name);
        sync();
    }
    void commit (FreeCurve c)
    {
        c.sanitise();
        proc.getCustomCurves().set (stage, &c, &undo());
        sync();
        owner.repaint();
    }
    template <typename Fn>
    void edit (const juce::String& name, Fn&& fn)
    {
        beginEdit (name);
        FreeCurve c = curve;
        fn (c);
        commit (c);
    }

    float seedReach() const
    {
        const auto s = proc.buildClipSettings (stage);
        return CurveSeed::reachFor (s.mode, s.curve, juce::Decibels::decibelsToGain (s.driveDb), juce::Decibels::decibelsToGain (s.boostDb),
                                    s.tailDriveDb, s.spatial, s.offset);
    }
    juce::String modeName() const
    {
        return ClipDistortion::modeNames()[juce::jlimit (0, ClipDistortion::numModes - 1, proc.buildClipSettings (stage).mode)];
    }

    void applyStarter (int which)
    {
        // keeps the current range (a curve seeded just now: the seed's range)
        edit ("Curve shape: " + CurveSeed::starterNames()[which], [which] (FreeCurve& c) { c = CurveSeed::starter (which, c.reach); });
    }
    void reseed()            { edit ("Re-seed curve", [this] (FreeCurve& c) { c = proc.seedCustomCurve (stage); }); }
    void fitRangeToDrive()   { const float r = seedReach(); edit ("Fit curve range to Drive", [r] (FreeCurve& c) { c.reach = r; }); }
    void setSymmetric (bool sym)
    {
        if (isActive() && curve.symmetric == sym) return;
        edit (sym ? "Symmetric curve" : "Asymmetric curve", [sym] (FreeCurve& c) { if (c.symmetric != sym) c = CurveSeed::toggledSymmetry (c); });
    }
    void flip()              { edit ("Flip curve", [] (FreeCurve& c) { c = CurveSeed::flipped (c); }); }
    void mirrorPosToNeg()    { edit ("Mirror curve", [] (FreeCurve& c) { c = CurveSeed::mirroredPosToNeg (c); }); }
    void straight()          { edit ("Straight curve", [] (FreeCurve& c) { c = FreeCurve::linear (c.symmetric, c.reach); }); }
    void setAllSharp (bool s){ edit (s ? "Sharpen curve corners" : "Smooth curve corners", [s] (FreeCurve& c) { c = CurveSeed::withAllSharp (c, s); }); }
    void deletePoint (int i)
    {
        if (! isActive() || curve.isLocked (i)) return;
        edit ("Delete curve point", [i] (FreeCurve& c) { c.remove (i); });
    }
    void toggleSharp (int i)
    {
        if (! isActive() || i < 0 || i >= curve.count) return;
        edit ("Curve corner", [i] (FreeCurve& c) { c.setSharp (i, ! c.p[(size_t) i].sharp); });
    }
    void resetBend (int i)
    {
        if (! isActive() || i < 0 || i >= curve.count - 1) return;
        edit ("Reset curve bend", [i] (FreeCurve& c) { c.setBend (i, 0.0f); });
    }
    void straightenSegment (int i)
    {
        if (! isActive() || i < 0 || i >= curve.count - 1) return;
        edit ("Straighten curve segment", [i] (FreeCurve& c) { c.setBend (i, 0.0f); c.setSharp (i, true); c.setSharp (i + 1, true); });
    }
    void turnOff()
    {
        if (! proc.isCustomEnabled (stage)) return;
        Params::beginTransaction (proc.apvts, "Custom curve off");
        proc.setCustomEnabled (stage, false);
        owner.repaint();
    }
    /** While the Type plays (Custom off) the display's x is the input before Drive: maps such an x to the input of a
        custom curve with this range (the same input the Type sees), so a conversion keeps every gesture where it was drawn. */
    float legacyToU (float x, float reach) const
    {
        const float g = legacyGain ? legacyGain() : 1.0f, off = legacyOffset ? legacyOffset() : 0.0f;
        return juce::jlimit (-1.0f, 1.0f, (x * g + off) / juce::jmax (1.0e-6f, reach));
    }

    /** Adds a point at a screen position (converting first when needed); returns its index or -1. */
    int addPointAt (juce::Point<float> pos)
    {
        const bool converting = ! isActive();
        // a double-click on (or right next to) the Type's curve adds a point on the seeded curve: only a point, no dent
        const bool onTypeCurve = converting && distanceToLegacyCurve (pos) < 8.0f;
        beginEdit ("Add curve point");
        FreeCurve c = curve;
        float u = screenToU (pos.x), y = screenToY (pos.y);
        if (converting) u = legacyToU (u, c.reach);
        if (onTypeCurve) y = shape.evalU (u);
        if (c.symmetric && u < 0.0f) { u = -u; y = -y; }
        const int at = c.insert (u, y, false);
        if (at < 0) { if (c.count >= FreeCurve::maxPoints) flashFull(); return -1; }
        commit (c);
        return at;
    }
    void flashFull() { fullFlashUntil = juce::Time::getMillisecondCounter() + 1000; owner.repaint(); }

    // ---- menus ----
    enum MenuIds { starterBase = 100, idReseed = 120, idFit, idSym, idAsym, idFlip, idMirror, idSmoothAll, idSharpAll, idStraight, idOff,
                   idDeletePoint = 200, idSharpPoint, idResetBend = 210, idStraighten };

    void fillCurveMenu (juce::PopupMenu& m)
    {
        const bool active = isActive();
        m.addSectionHeader ("Starter shapes");
        const auto names = CurveSeed::starterNames();
        for (int i = 0; i < names.size(); ++i) m.addItem (starterBase + i, names[i]);
        m.addSectionHeader ("Custom curve");
        m.addItem (idReseed, "Re-seed from " + modeName());
        m.addItem (idFit, "Fit range to Drive", active);
        m.addItem (idSym, "Symmetric", true, active && curve.symmetric);
        m.addItem (idAsym, "Asymmetric", true, active && ! curve.symmetric);
        m.addItem (idFlip, "Flip vertically", active);
        m.addItem (idMirror, juce::String (juce::CharPointer_UTF8 ("Mirror + half to \xe2\x88\x92 half")), active && ! curve.symmetric);
        m.addItem (idSmoothAll, "Smooth all corners", active);
        m.addItem (idSharpAll, "Sharpen all corners", active);
        m.addItem (idStraight, "Straight line (reset)");
        m.addItem (idOff, "Back to " + modeName() + " (curve off)", proc.isCustomEnabled (stage));
    }
    void fillPointMenu (juce::PopupMenu& m, int i)
    {
        m.addSectionHeader ("Curve point");
        m.addItem (idDeletePoint, "Delete point", ! curve.isLocked (i));
        m.addItem (idSharpPoint, "Sharp corner", true, curve.p[(size_t) i].sharp);
    }
    void fillDiamondMenu (juce::PopupMenu& m)
    {
        m.addSectionHeader ("Curve segment");
        m.addItem (idResetBend, "Reset bend");
        m.addItem (idStraighten, "Straighten segment");
    }
    void handleMenu (int id, Handle h)
    {
        if (id >= starterBase && id < starterBase + CurveSeed::numStarters) { applyStarter (id - starterBase); return; }
        switch (id)
        {
            case idReseed:      reseed(); break;
            case idFit:         fitRangeToDrive(); break;
            case idSym:         setSymmetric (true); break;
            case idAsym:        setSymmetric (false); break;
            case idFlip:        flip(); break;
            case idMirror:      mirrorPosToNeg(); break;
            case idSmoothAll:   setAllSharp (false); break;
            case idSharpAll:    setAllSharp (true); break;
            case idStraight:    straight(); break;
            case idOff:         turnOff(); break;
            case idDeletePoint: deletePoint (h.index); break;
            case idSharpPoint:  toggleSharp (h.index); break;
            case idResetBend:   resetBend (h.index); break;
            case idStraighten:  straightenSegment (h.index); break;
            default: break;
        }
    }

    // ---- mouse (left button; the display handles right-click menus and the legacy X-Shaper handles) ----
    void mouseMove (juce::Point<float> pos)
    {
        hoverPos = pos;
        const Handle h = isActive() && ! pencil ? hitTest (pos) : Handle {};
        if (h != hover) { hover = h; }
        owner.repaint();
    }
    void mouseExit() { hover = {}; hoverPos = { -1.0f, -1.0f }; owner.repaint(); }
    juce::MouseCursor cursor() const
    {
        if (pencil) return juce::MouseCursor::CrosshairCursor;
        if (hover.kind == Handle::Point) return juce::MouseCursor::DraggingHandCursor;
        if (hover.kind == Handle::Diamond) return juce::MouseCursor::UpDownResizeCursor;
        return juce::MouseCursor::NormalCursor;
    }

    void mouseDown (const juce::MouseEvent& e)
    {
        gesture = Gesture::None;
        pressPos = lastPos = e.position;
        if (pencil)
        {
            stroke.clear();
            stroke.push_back (e.position);
            gesture = Gesture::Pencil;
            return;
        }
        if (isActive())
        {
            const Handle h = hitTest (e.position);
            if (h.kind == Handle::Point && e.mods.isCommandDown()) { toggleSharp (h.index); return; }
            if (e.mods.isAltDown() && plot.contains (e.position))
            {
                active = h.kind == Handle::Diamond ? h : segmentAt (e.position);
                Params::beginTransaction (proc.apvts, "Bend curve segment");
                bendAcc = curve.p[(size_t) active.index].bend;
                gesture = Gesture::Bend;
                return;
            }
            if (h.kind == Handle::Point)
            {
                Params::beginTransaction (proc.apvts, "Move curve point");
                startPointDrag (h);
                return;
            }
            if (h.kind == Handle::Diamond)
            {
                Params::beginTransaction (proc.apvts, "Bend curve segment");
                active = h;
                bendAcc = curve.p[(size_t) h.index].bend;
                gesture = Gesture::Bend;
                return;
            }
            if (distanceToCurve (e.position) < 8.0f) gesture = Gesture::PendingInsert;
            return;
        }
        // Custom off: a drag that starts on the curve converts it (at the drag threshold, so a click changes nothing)
        if (plot.contains (e.position) && distanceToLegacyCurve (e.position) < 8.0f)
            gesture = e.mods.isAltDown() ? Gesture::PendingBend : Gesture::PendingInsert;
    }

    void mouseDrag (const juce::MouseEvent& e)
    {
        if (gesture == Gesture::Pencil)
        {
            if (stroke.empty() || stroke.back().getDistanceFrom (e.position) >= 1.0f) stroke.push_back (e.position);
            owner.repaint();
            return;
        }
        if (gesture == Gesture::PendingInsert || gesture == Gesture::PendingBend)
        {
            if (e.position.getDistanceFrom (pressPos) <= 3.0f) return;
            const bool bend = gesture == Gesture::PendingBend;
            gesture = Gesture::None;
            float u;
            if (! isActive())
            {
                proc.beginCustomEdit (stage, bend ? "Bend curve segment" : "Draw distortion curve", true);
                sync();
                // the press on the Type's curve, in the custom curve's own input range
                u = legacyToU (screenToU (pressPos.x), curve.reach);
            }
            else
            {
                Params::beginTransaction (proc.apvts, "Add curve point");
                u = screenToU (pressPos.x);
            }
            if (bend)
            {
                active = segmentAt (toScreen (u, 0.0f));
                bendAcc = curve.p[(size_t) active.index].bend;
                gesture = Gesture::Bend;
            }
            else
            {
                FreeCurve c = curve;
                const bool m = c.symmetric && u < 0.0f;
                const float su = m ? -u : u;
                const int at = c.insert (su, shape.evalU (su), false);
                if (at < 0)
                {
                    if (c.count >= FreeCurve::maxPoints) flashFull();
                    owner.repaint();
                    return;
                }
                commit (c);
                startPointDrag ({ Handle::Point, at, m });
            }
            lastPos = pressPos;
        }
        const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
        const auto d = (e.position - lastPos) * fine;
        lastPos = e.position;
        if (gesture == Gesture::MovePoint)
        {
            dragU += d.x / pixelsPerUnit();
            dragY -= d.y / pixelsPerUnit() * (plot.getWidth() / plot.getHeight());
            FreeCurve c = curve;
            const float su = active.mirrored ? -dragU : dragU, sy = active.mirrored ? -dragY : dragY;
            c.move (active.index, su, sy);
            commitDrag (c);
        }
        else if (gesture == Gesture::Bend)
        {
            const auto& a = curve.p[(size_t) active.index];
            const auto& b = curve.p[(size_t) active.index + 1];
            // on screen, left to right: the mirror of a segment runs from -b to -a
            const float ya = active.mirrored ? -b.y : a.y, yb = active.mirrored ? -a.y : b.y;
            const float dir = yb < ya ? 1.0f : -1.0f;   // dragging down always bends towards the lower end (as the envelope editor)
            const float visualDelta = dir * d.y / plot.getHeight() * 2.0f;
            bendAcc = juce::jlimit (-1.0f, 1.0f, bendAcc + (active.mirrored ? -visualDelta : visualDelta));
            FreeCurve c = curve;
            c.setBend (active.index, bendAcc);
            commitDrag (c);
        }
    }

    void mouseUp (const juce::MouseEvent&)
    {
        if (gesture == Gesture::Pencil) applyPencil();
        gesture = Gesture::None;
        active = {};
        stroke.clear();
        owner.repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e)
    {
        gesture = Gesture::None;
        if (! plot.expanded (6.0f).contains (e.position)) return;
        if (! isActive())
        {
            addPointAt (e.position);
            return;
        }
        const Handle h = hitTest (e.position);
        if (h.kind == Handle::Point)        deletePoint (h.index);
        else if (h.kind == Handle::Diamond) resetBend (h.index);
        else if (plot.contains (e.position)) addPointAt (e.position);
    }

    bool mouseWheel (juce::Point<float> pos, float deltaY)
    {
        if (! isActive()) return false;
        const Handle h = hitTest (pos);
        if (h.kind != Handle::Diamond) return false;
        const float b = curve.p[(size_t) h.index].bend + (h.mirrored ? -1.0f : 1.0f) * deltaY * 0.5f;
        edit ("Bend curve segment", [&] (FreeCurve& c) { c.setBend (h.index, b); });
        return true;
    }

    /** Pencil stroke in screen coordinates -> points (one undo step; converts first when needed). */
    void applyPencil()
    {
        if (stroke.size() < 2) return;
        // while the Type plays, the stroke was drawn over the Type's curve: map it into the seed's input range
        const bool converting = ! isActive();
        const FreeCurve target = converting ? proc.seedCustomCurve (stage) : curve;
        std::vector<juce::Point<float>> uy;
        for (auto s : stroke)
        {
            const float x = screenToU (s.x);
            uy.push_back ({ converting ? legacyToU (x, target.reach) : x, screenToY (s.y) });
        }
        // ignore strokes that cover less than two cells of the curve's grid
        float lo = 2.0f, hi = -2.0f;
        for (auto q : uy) { const float u = target.symmetric ? std::abs (q.x) : q.x; lo = std::min (lo, u); hi = std::max (hi, u); }
        if (hi - lo < 2.0f / 256.0f) return;
        beginEdit ("Pencil stroke");
        FreeCurve c = curve;
        c.applyStroke (uy, 3.0f / plot.getHeight());
        commit (c);
    }

    // ---- painting ----
    struct PaintInfo { juce::Colour colour; bool enabled = true; float driveGain = 1.0f, offset = 0.0f, inputLevel = 0.0f; };

    void paint (juce::Graphics& g, const PaintInfo& info)
    {
        using namespace juce;
        const auto col = info.colour;
        // curve (2 samples per pixel) with a faint fill to the centre line
        Path p;
        const int steps = (int) plot.getWidth() * 2;
        for (int i = 0; i <= steps; ++i)
        {
            const float u = -1.0f + 2.0f * (float) i / (float) steps;
            const auto pt = toScreen (u, shape.evalU (u));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        Path fill (p);
        fill.lineTo (plot.getRight(), plot.getCentreY());
        fill.lineTo (plot.getX(), plot.getCentreY());
        fill.closeSubPath();
        g.setColour (col.withAlpha (0.08f));
        g.fillPath (fill);

        // drive markers: the largest input the Drive reaches, in the curve's range
        const float R = curve.reach;
        for (float sgn : { -1.0f, 1.0f })
        {
            const float v = sgn * info.driveGain + info.offset;
            const float u = v / R;
            float dash[] = { 3.0f, 3.0f };
            if (std::abs (u) <= 1.0f)
            {
                Path line;
                const float x = toScreen (u, 0.0f).x;
                line.startNewSubPath (x, plot.getY());
                line.lineTo (x, plot.getBottom());
                PathStrokeType (1.0f).createDashedStroke (line, line, dash, 2);
                g.setColour (Colours_::accent2.withAlpha (0.5f));
                g.fillPath (line);
                if (sgn > 0.0f)
                {
                    g.setFont (Fonts::label (Fonts::Size::micro, 0.06f));
                    g.drawText ("DRIVE", Rectangle<float> (x - 44.0f, plot.getBottom() - 30.0f, 40.0f, 13.0f), Justification::centredRight);
                }
            }
            else
            {
                const float x = sgn > 0.0f ? plot.getRight() - 6.0f : plot.getX() + 6.0f;
                const float y = plot.getCentreY() + (sgn > 0.0f ? 14.0f : -14.0f);
                Path arrow;
                arrow.addTriangle (x + sgn * 4.0f, y, x - sgn * 3.0f, y - 4.0f, x - sgn * 3.0f, y + 4.0f);
                // the drive reaches past the drawn range: the curve is held flat there (normal for clipping Types)
                g.setColour (Colours_::accent2.withAlpha (0.8f));
                g.fillPath (arrow);
                if (sgn > 0.0f)
                {
                    g.setFont (Fonts::label (Fonts::Size::micro, 0.06f));
                    g.drawText ("DRIVE PAST RANGE", Rectangle<float> (plot.getRight() - 130.0f, y + 5.0f, 124.0f, 13.0f), Justification::centredRight);
                }
            }
        }

        Colours_::strokeGlow (g, p, col, 2.0f);

        // bend diamonds
        for (int i = 0; i < curve.count - 1; ++i)
            for (bool m : { false, true })
            {
                if ((m && ! curve.symmetric) || ! diamondVisible (i)) continue;
                const Handle h { Handle::Diamond, i, m };
                const bool hi = h == hover || h == active;
                Path d;
                d.addRectangle (-3.5f, -3.5f, 7.0f, 7.0f);
                d.applyTransform (AffineTransform::rotation (MathConstants<float>::pi * 0.25f).translated (diamondPos (i, m)));
                g.setColour (hi ? col : col.withAlpha (m ? 0.25f : 0.45f));
                g.fillPath (d);
            }
        // points
        for (int i = 0; i < curve.count; ++i)
            for (bool m : { false, true })
            {
                if (m && (! curve.symmetric || i == 0)) continue;
                const auto pt = pointPos (i, m);
                const Handle h { Handle::Point, i, m };
                const bool hi = h == hover || h == active;
                const bool origin = curve.symmetric && i == 0;
                const float size = origin ? 6.0f : curve.p[(size_t) i].sharp ? 8.0f : 9.0f;
                if (hi) { g.setColour (col.withAlpha (0.25f)); g.fillEllipse (Rectangle<float> (18.0f, 18.0f).withCentre (pt)); }
                const auto r = Rectangle<float> (size, size).withCentre (pt);
                g.setColour (m ? col.withAlpha (0.35f) : (origin ? col.withAlpha (0.6f) : col));
                if (curve.p[(size_t) i].sharp && ! origin) g.fillRect (r); else g.fillEllipse (r);
                g.setColour (Colours_::display);
                if (curve.p[(size_t) i].sharp && ! origin) g.drawRect (r, 1.2f); else g.drawEllipse (r, 1.2f);
            }

        // input level (pre-drive, with boost) on both sides of the curve
        const float lv = jlimit (0.0f, 1.0f, info.inputLevel);
        if (lv > 0.001f && info.enabled)
            for (float sgn : { -1.0f, 1.0f })
            {
                const float u = jlimit (-1.0f, 1.0f, (sgn * lv * info.driveGain + info.offset) / R);
                const auto pt = toScreen (u, shape.evalU (u));
                g.setColour (Colours::white.withAlpha (0.25f));
                g.fillEllipse (Rectangle<float> (14.0f, 14.0f).withCentre (pt));
                g.setColour (Colours::white);
                g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (pt));
            }

        // pencil preview
        if (gesture == Gesture::Pencil && stroke.size() > 1)
        {
            Path s;
            s.startNewSubPath (stroke.front());
            for (size_t k = 1; k < stroke.size(); ++k) s.lineTo (stroke[k]);
            g.setColour (Colours_::accentHi);
            g.strokePath (s, PathStrokeType (1.5f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        // readout
        const Handle shown = active.kind != Handle::None ? active : hover;
        if (shown.kind != Handle::None && shown.index >= 0 && shown.index < curve.count)
        {
            String txt;
            Point<float> at;
            if (shown.kind == Handle::Point)
            {
                const auto& q = curve.p[(size_t) shown.index];
                const float s = shown.mirrored ? -1.0f : 1.0f;
                txt = "IN " + String (s * q.x * R, 2) + String (CharPointer_UTF8 ("  \xe2\x86\x92  OUT ")) + String (s * q.y, 2);
                at = pointPos (shown.index, shown.mirrored);
            }
            else if (shown.index < curve.count - 1)
            {
                const float b = curve.p[(size_t) shown.index].bend * (shown.mirrored ? -1.0f : 1.0f);
                txt = "BEND " + String (b >= 0.0f ? "+" : "") + String (b, 2);
                at = diamondPos (shown.index, shown.mirrored);
            }
            if (txt.isNotEmpty())
            {
                g.setFont (Fonts::medium (Fonts::Size::micro));
                auto box = Rectangle<float> (120.0f, 18.0f).withCentre (at.translated (0, -16.0f)).constrainedWithin (plot);
                g.setColour (Colours_::panelTop.withAlpha (0.92f));
                g.fillRoundedRectangle (box, 4.0f);
                g.setColour (Colours_::text);
                g.drawText (txt, box, Justification::centred);
            }
        }

        // hint and range caption
        g.setFont (Fonts::label (Fonts::Size::micro, 0.06f));
        g.setColour (Colours_::textMuted);
        const bool full = Time::getMillisecondCounter() < fullFlashUntil;
        g.setColour (full ? Colours_::warn : Colours_::textMuted);
        g.drawText (full ? String ("MAX POINTS") : String (CharPointer_UTF8 (curve.symmetric ? "CUSTOM  \xc2\xb7  MIRRORED" : "CUSTOM  \xc2\xb7  BOTH HALVES")),
                    Rectangle<float> (plot.getX() + 4.0f, plot.getY() + 16.0f, plot.getWidth() * 0.6f, 14.0f), Justification::centredLeft);
        g.setColour (Colours_::textMuted);
        g.drawText ("IN " + String (CharPointer_UTF8 ("\xc2\xb1")) + String (R, R < 10.0f ? 1 : 0),
                    Rectangle<float> (plot.getRight() - 64.0f, plot.getBottom() - 14.0f, 38.0f, 14.0f), Justification::centredRight);
    }

    /** Hint for the display while the Type plays (Custom off). */
    static juce::String drawHint() { return "DOUBLE-CLICK OR DRAG TO DRAW"; }

    // tests
    Handle getHover() const { return hover; }
    void setHoverForTesting (Handle h) { hover = h; owner.repaint(); }

private:
    void startPointDrag (Handle h)
    {
        active = h;
        const auto& q = curve.p[(size_t) h.index];
        dragU = h.mirrored ? -q.x : q.x;
        dragY = h.mirrored ? -q.y : q.y;
        gesture = Gesture::MovePoint;
    }
    void commitDrag (const FreeCurve& c)
    {
        proc.getCustomCurves().set (stage, &c, &undo());
        sync();
        owner.repaint();
    }

    enum class Gesture { None, MovePoint, Bend, PendingInsert, PendingBend, Pencil };

    CodexProcessor& proc;
    int stage;
    juce::Component& owner;
    juce::String cachedText;
    bool synced = false, hasCurve = false;
    FreeCurve curve;
    FreeCurveShape shape;
    Handle hover, active;
    Gesture gesture = Gesture::None;
    juce::Point<float> pressPos, lastPos, hoverPos { -1.0f, -1.0f };
    float dragU = 0.0f, dragY = 0.0f, bendAcc = 0.0f;
    std::vector<juce::Point<float>> stroke;
    juce::uint32 fullFlashUntil = 0;
};
