#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Widgets.h"
#include "PresetBar.h"
#include "ScopeView.h"
#include "../DSP/MultibandSaturator.h"

/** Band add / remove / copy operations on the saturation parameters (each is one undo step). */
namespace satBands
{
    inline const std::array<const char*, 16>& suffixes()
    {
        static const std::array<const char*, 16> s { "Style", "Drive", "Mix", "Fb", "FbFreq", "Dyn", "Bass", "MidT", "Treble", "Pres",
                                                      "DrivePan", "LevelPan", "Level", "Mute", "Solo", "Bypass" };
        return s;
    }

    inline void set (juce::AudioProcessorValueTreeState& s, const juce::String& id, float normalised)
    {
        if (auto* p = s.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (normalised);
            p->endChangeGesture();
        }
    }
    inline float get (juce::AudioProcessorValueTreeState& s, const juce::String& id) { return s.getParameter (id)->getValue(); }
    inline juce::String xId (int j) { return ID::sat (("X" + juce::String (j + 1)).toRawUTF8()); }
    inline int count (juce::AudioProcessorValueTreeState& s) { return juce::jlimit (1, sat::maxBands, (int) s.getRawParameterValue (ID::sat ("Bands"))->load()); }

    inline void copyBand (juce::AudioProcessorValueTreeState& s, int from, int to)
    {
        for (auto* sfx : suffixes()) set (s, ID::satBand (to, sfx), get (s, ID::satBand (from, sfx)));
    }

    inline void setCount (juce::AudioProcessorValueTreeState& s, int n)
    {
        auto* p = s.getParameter (ID::sat ("Bands"));
        set (s, ID::sat ("Bands"), p->convertTo0to1 ((float) n));
    }

    /** Splits band b at freq: the new upper half copies the band's settings. */
    inline void splitBand (juce::AudioProcessorValueTreeState& s, int b, float freq)
    {
        const int nb = count (s);
        if (nb >= sat::maxBands) return;
        if (auto* um = s.undoManager) um->beginNewTransaction ("Add saturation band");
        for (int k = nb - 1; k > b; --k) copyBand (s, k, k + 1);   // shift bands up
        copyBand (s, b, b + 1);
        for (int j = nb - 2; j >= b; --j) set (s, xId (j + 1), get (s, xId (j)));   // shift crossovers up
        auto* x = s.getParameter (xId (b));
        set (s, xId (b), x->convertTo0to1 (freq));
        setCount (s, nb + 1);
    }

    /** Removes band b; its frequency range goes to the neighbouring band. */
    inline void removeBand (juce::AudioProcessorValueTreeState& s, int b)
    {
        const int nb = count (s);
        if (nb <= 1) return;
        if (auto* um = s.undoManager) um->beginNewTransaction ("Remove saturation band");
        for (int k = b; k < nb - 1; ++k) copyBand (s, k + 1, k);   // shift bands down
        const int removedX = b == nb - 1 ? b - 1 : b;
        for (int j = removedX; j < nb - 2; ++j) set (s, xId (j), get (s, xId (j + 1)));
        setCount (s, nb - 1);
    }
}

//==============================================================================
/** Spectrum with draggable crossovers, level handles and mute / solo / bypass badges per band. */
class SaturationBandDisplay : public juce::Component, private juce::Timer
{
public:
    bool isVisibleOnPage() const { return isVisible() && getParentComponent() != nullptr && getParentComponent()->isVisible(); }

    SaturationBandDisplay (CodexProcessor& p, const StereoHistory& h) : proc (p), apvts (p.apvts), history (h)
    {
        fftData.assign (fftSize * 2, 0.0f);
        spectrum.assign (fftSize / 2, -150.0f);
        startTimerHz (30);
    }

    std::function<void (int)> onBandSelected;
    void setSelectedBand (int b) { selected = b; repaint(); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        const bool enabled = value (ID::sat ("On")) > 0.5f;
        const int nb = satBands::count (apvts);

        g.setFont (Fonts::label (10.5f, 0.1f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            g.setColour (Colours_::grid.brighter (0.25f));
            g.drawVerticalLine ((int) freqToX (f), plot.getY(), plot.getBottom());
            g.setColour (Colours_::textFaint);
            g.drawText (f >= 1000 ? String ((int) (f / 1000)) + "k" : String ((int) f), Rectangle<float> (freqToX (f) - 15, plot.getBottom() + 1, 30, 12), Justification::centred);
        }

        // Band regions
        for (int b = 0; b < nb; ++b)
        {
            auto region = bandRegion (b);
            const bool sel = b == selected;
            const bool bypass = value (ID::satBand (b, "Bypass")) > 0.5f, mute = value (ID::satBand (b, "Mute")) > 0.5f;
            g.setColour (Colours_::band (b).withAlpha (sel ? 0.10f : 0.03f));
            g.fillRect (region);
            if (sel)
            {
                g.setColour (Colours_::band (b).withAlpha (0.8f));
                g.fillRect (region.withHeight (2.0f));
            }

            auto label = region.reduced (8.0f, 0.0f).removeFromBottom (44.0f);
            const int style = (int) value (ID::satBand (b, "Style"));
            g.setColour (mute || bypass || ! enabled ? Colours_::textFaint : Colours_::text);
            g.setFont (Fonts::semibold (13.5f));
            g.drawFittedText (sat::styles()[(size_t) jlimit (0, (int) sat::styles().size() - 1, style)].name,
                              label.removeFromTop (18.0f).toNearestInt(), Justification::centredLeft, 1, 0.8f);
            g.setFont (Fonts::regular (12.0f));
            g.setColour (Colours_::textDim);
            g.drawText (mute ? String ("MUTED") : bypass ? String ("BYPASSED") : "Drive " + String (value (ID::satBand (b, "Drive")), 1) + " dB",
                        label.removeFromTop (16.0f), Justification::centredLeft);

            // band output meter
            auto meter = region.reduced (8.0f, 0.0f).withTrimmedTop (56.0f).withTrimmedBottom (50.0f).removeFromRight (4.0f);
            g.setColour (Colours_::grid.brighter (0.2f));
            g.fillRoundedRectangle (meter, 2.0f);
            const float lv = jlimit (0.0f, 1.0f, (Decibels::gainToDecibels (bandLevels[(size_t) b], -60.0f) + 60.0f) / 60.0f);
            g.setColour (Colours_::band (b));
            g.fillRoundedRectangle (meter.withTop (meter.getBottom() - lv * meter.getHeight()), 2.0f);

            // M / S / B badges
            for (int k = 0; k < 3; ++k)
            {
                const auto badge = badgeRect (b, k);
                const char* sfx = k == 0 ? "Mute" : k == 1 ? "Solo" : "Bypass";
                const bool on = value (ID::satBand (b, sfx)) > 0.5f;
                const Colour onCol = k == 0 ? Colours_::warn : k == 1 ? Colours_::band (2) : Colours_::accent;
                g.setColour (on ? onCol.withAlpha (0.85f) : Colours_::panelTop);
                g.fillRoundedRectangle (badge, 3.0f);
                g.setColour (on ? onCol : Colours_::outline);
                g.drawRoundedRectangle (badge, 3.0f, 1.0f);
                g.setColour (on ? Colour (0xff04101e) : Colours_::textDim);
                g.setFont (Fonts::label (11.0f, 0.05f));
                g.drawText (k == 0 ? "M" : k == 1 ? "S" : "B", badge, Justification::centred);
            }
        }

        // Spectrum of the output (mid), 3 dB/oct tilt
        {
            const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            Path p;
            bool started = false;
            for (int x = (int) plot.getX(); x <= (int) plot.getRight(); x += 2)
            {
                const float f = xToFreq ((float) x);
                const float bin = f / (float) (sr / fftSize);
                const int i = jlimit (1, fftSize / 2 - 2, (int) bin);
                const float v = jmap (jlimit (0.0f, 1.0f, bin - (float) i), spectrum[(size_t) i], spectrum[(size_t) i + 1]) + 3.0f * std::log2 (f / 1000.0f);
                const float y = jlimit (plot.getY(), plot.getBottom(), jmap (v, -90.0f, 0.0f, plot.getBottom(), plot.getY() + 30.0f));
                if (! started) { p.startNewSubPath ((float) x, plot.getBottom()); p.lineTo ((float) x, y); started = true; }
                else p.lineTo ((float) x, y);
            }
            p.lineTo (plot.getRight(), plot.getBottom());
            p.closeSubPath();
            g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (0.20f), 0, plot.getY(), Colours_::accent.withAlpha (0.02f), 0, plot.getBottom(), false));
            g.fillPath (p);
            g.setColour (Colours_::accent.withAlpha (0.35f));
            g.strokePath (p, PathStrokeType (1.0f));
        }

        // Level handles (drag: level, Alt-drag: drive)
        for (int b = 0; b < nb; ++b)
        {
            const auto h = levelHandle (b);
            const bool hot = b == hoverHandle || b == draggingHandle;
            g.setColour (Colours_::band (b).withAlpha (0.25f));
            g.drawHorizontalLine ((int) h.getCentreY(), bandRegion (b).getX() + 2.0f, bandRegion (b).getRight() - 2.0f);
            g.setColour (hot ? Colours_::band (b) : Colours_::panelTop);
            g.fillRoundedRectangle (h, 5.0f);
            g.setColour (Colours_::band (b));
            g.drawRoundedRectangle (h, 5.0f, 1.2f);
            g.setColour (hot ? Colour (0xff04101e) : Colours_::text);
            g.setFont (Fonts::medium (11.5f));
            const float lvDb = value (ID::satBand (b, "Level"));
            g.drawText ((altHeld && hot ? "DRV " + String (value (ID::satBand (b, "Drive")), 1)
                                        : (lvDb > 0 ? "+" : "") + String (lvDb, 1)) + " dB", h, Justification::centred);
        }

        // Crossover lines with handles
        for (int j = 0; j < nb - 1; ++j)
        {
            const float x = freqToX (crossover (j));
            const bool hot = j == hoverCrossover || j == dragging;
            g.setColour ((hot ? Colours_::accentHi : Colours_::text).withAlpha (hot ? 0.9f : 0.5f));
            g.drawLine (x, plot.getY(), x, plot.getBottom(), hot ? 2.0f : 1.2f);
            auto handle = crossoverHandle (j);
            g.setColour (hot ? Colours_::accent : Colours_::panelTop);
            g.fillRoundedRectangle (handle, 9.0f);
            g.setColour (hot ? Colours_::accent : Colours_::outlineHi);
            g.drawRoundedRectangle (handle, 9.0f, 1.0f);
            g.setColour (hot ? Colour (0xff04101e) : Colours_::text);
            g.setFont (Fonts::medium (11.5f));
            const float f = crossover (j);
            g.drawText (f < 1000 ? String (roundToInt (f)) : String (f / 1000.0f, 1) + "k", handle, Justification::centred);
        }

        // "+" to add a band where the mouse is (top strip)
        if (addButtonVisible())
        {
            auto plus = addButtonRect();
            g.setColour (Colours_::accent);
            g.fillEllipse (plus);
            Icons::draw (g, [] { Path p; p.startNewSubPath (0.5f, 0.22f); p.lineTo (0.5f, 0.78f); p.startNewSubPath (0.22f, 0.5f); p.lineTo (0.78f, 0.5f); return p; }(),
                         plus.reduced (3.0f), Colour (0xff04101e), 2.0f);
        }

        if (! enabled)
        {
            g.setColour (Colours_::panel.withAlpha (0.55f));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (Colours_::text);
            g.setFont (Fonts::label (14.0f, 0.18f));
            g.drawText ("SATURATION IS OFF", r.withTrimmedBottom (r.getHeight() * 0.5f), Justification::centredBottom);
            g.setColour (Colours_::textDim);
            g.setFont (Fonts::regular (13.0f));
            g.drawText ("Switch it on with the power button in the panel header.", r.withTrimmedTop (r.getHeight() * 0.5f + 4.0f), Justification::centredTop);
        }
    }

    void resized() override { plot = getLocalBounds().toFloat().reduced (2.0f).withTrimmedBottom (14.0f); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        mousePos = e.position;
        mouseInside = true;
        altHeld = e.mods.isAltDown();
        hoverCrossover = hitCrossover (e.position);
        hoverHandle = hitHandle (e.position);
        setMouseCursor (hoverCrossover >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                        : hoverHandle >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
    void mouseExit (const juce::MouseEvent&) override { hoverCrossover = hoverHandle = -1; mouseInside = false; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int nb = satBands::count (apvts);
        moved = false;

        if (addButtonVisible() && addButtonRect().contains (e.position))
        {
            const float f = xToFreq (e.position.x);
            for (int b = 0; b < nb; ++b)
                if (bandRegion (b).contains (juce::Point<float> (e.position.x, plot.getCentreY())))
                {
                    satBands::splitBand (apvts, b, f);
                    selectBand (b + 1);
                    break;
                }
            return;
        }

        for (int b = 0; b < nb; ++b)
            for (int k = 0; k < 3; ++k)
                if (badgeRect (b, k).contains (e.position))
                {
                    const char* sfx = k == 0 ? "Mute" : k == 1 ? "Solo" : "Bypass";
                    const bool on = value (ID::satBand (b, sfx)) > 0.5f;
                    if (auto* um = apvts.undoManager) um->beginNewTransaction ("Band toggle");
                    if (e.mods.isCommandDown() && k < 2)   // Ctrl/Cmd-click: exclusive solo / mute
                        for (int o = 0; o < nb; ++o)
                            satBands::set (apvts, ID::satBand (o, sfx), o == b ? 1.0f : 0.0f);
                    else
                        satBands::set (apvts, ID::satBand (b, sfx), on ? 0.0f : 1.0f);
                    return;
                }

        dragging = hitCrossover (e.position);
        if (dragging >= 0)
        {
            if (auto* prm = crossoverParam (dragging)) prm->beginChangeGesture();
            return;
        }

        draggingHandle = hitHandle (e.position);
        if (draggingHandle >= 0)
        {
            selectBand (draggingHandle);
            if (e.mods.isCommandDown())   // Ctrl/Cmd-click: reset the level
            {
                satBands::set (apvts, ID::satBand (draggingHandle, "Level"), apvts.getParameter (ID::satBand (draggingHandle, "Level"))->getDefaultValue());
                draggingHandle = -1;
                return;
            }
            driveDrag = e.mods.isAltDown();
            dragStartValue = value (ID::satBand (draggingHandle, driveDrag ? "Drive" : "Level"));
            dragStartY = e.position.y;
            if (auto* prm = apvts.getParameter (ID::satBand (draggingHandle, driveDrag ? "Drive" : "Level"))) prm->beginChangeGesture();
            return;
        }

        for (int b = 0; b < nb; ++b)
            if (bandRegion (b).contains (e.position))
                selectBand (b);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        moved = moved || e.getDistanceFromDragStart() > 2;
        const int nb = satBands::count (apvts);
        if (dragging >= 0)
        {
            const float lo = dragging > 0 ? crossover (dragging - 1) * 1.15f : 30.0f;
            const float hi = dragging < nb - 2 ? crossover (dragging + 1) / 1.15f : 16000.0f;
            if (auto* prm = crossoverParam (dragging))
                prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (lo, hi, xToFreq (e.position.x))));
            return;
        }
        if (draggingHandle >= 0)
        {
            const float fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
            const float dy = (dragStartY - e.position.y) * fine;
            if (auto* prm = apvts.getParameter (ID::satBand (draggingHandle, driveDrag ? "Drive" : "Level")))
            {
                const auto range = prm->getNormalisableRange();
                const float perPixel = (range.end - range.start) / juce::jmax (60.0f, plot.getHeight() * 0.8f);
                prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (range.start, range.end, dragStartValue + dy * perPixel)));
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging >= 0)
        {
            if (auto* prm = crossoverParam (dragging)) prm->endChangeGesture();
            if (! moved) showSlopeMenu (e.position);   // click on a crossover: choose the slope
        }
        if (draggingHandle >= 0)
            if (auto* prm = apvts.getParameter (ID::satBand (draggingHandle, driveDrag ? "Drive" : "Level"))) prm->endChangeGesture();
        dragging = draggingHandle = -1;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int nb = satBands::count (apvts);
        for (int b = 0; b < nb; ++b)
            if (bandRegion (b).contains (e.position))
            {
                const char* sfx = e.mods.isAltDown() ? "Drive" : "Level";
                auto* prm = apvts.getParameter (ID::satBand (b, sfx));
                const float step = (e.mods.isShiftDown() ? 0.2f : 1.0f) * (w.deltaY > 0 ? 1.0f : -1.0f);
                satBands::set (apvts, ID::satBand (b, sfx), prm->convertTo0to1 (prm->getNormalisableRange().snapToLegalValue (value (ID::satBand (b, sfx)) + step)));
                return;
            }
    }

private:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;

    void selectBand (int b) { selected = b; if (onBandSelected) onBandSelected (b); repaint(); }

    void showSlopeMenu (juce::Point<float>)
    {
        juce::PopupMenu menu;
        const int current = (int) value (ID::sat ("Slope"));
        menu.addSectionHeader ("Crossover slope");
        const auto names = sat::slopeNames();
        for (int i = 0; i < names.size(); ++i) menu.addItem (i + 1, names[i], true, i == current);
        juce::Component::SafePointer<SaturationBandDisplay> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto* p = safe->apvts.getParameter (ID::sat ("Slope"));
            satBands::set (safe->apvts, ID::sat ("Slope"), p->convertTo0to1 ((float) (r - 1)));
        });
    }

    void timerCallback() override
    {
        if (! isVisibleOnPage()) return;
        std::fill (fftData.begin(), fftData.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = 0.5f * (history.left (fftSize - 1 - i) + history.right (fftSize - 1 - i));
        window.multiplyWithWindowingTable (fftData.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());
        for (size_t i = 0; i < spectrum.size(); ++i)
        {
            const float db = juce::Decibels::gainToDecibels (fftData[i] * 4.0f / fftSize, -150.0f);
            spectrum[i] = db > spectrum[i] ? db : juce::jmax (db, spectrum[i] - 1.5f);
        }
        auto& satProc = proc.getSaturator();
        for (int b = 0; b < sat::maxBands; ++b)
        {
            const float pk = satProc.getAndResetBandPeak (b);
            bandLevels[(size_t) b] = pk > bandLevels[(size_t) b] ? pk : bandLevels[(size_t) b] * 0.9f;
        }
        repaint();
    }

    float value (const juce::String& id) const { return apvts.getRawParameterValue (id)->load(); }
    float crossover (int j) const { return value (satBands::xId (j)); }
    juce::RangedAudioParameter* crossoverParam (int j) const { return apvts.getParameter (satBands::xId (j)); }

    float freqToX (float f) const { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float xToFreq (float x) const { return 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth()); }

    juce::Rectangle<float> bandRegion (int b) const
    {
        const int nb = satBands::count (apvts);
        const float x0 = b == 0 ? plot.getX() : freqToX (crossover (b - 1));
        const float x1 = b == nb - 1 ? plot.getRight() : freqToX (crossover (b));
        return { x0, plot.getY(), juce::jmax (1.0f, x1 - x0), plot.getHeight() };
    }

    juce::Rectangle<float> badgeRect (int b, int k) const
    {
        const auto region = bandRegion (b);
        return { region.getX() + 8.0f + (float) k * 22.0f, region.getY() + 30.0f, 19.0f, 18.0f };
    }

    juce::Rectangle<float> levelHandle (int b) const
    {
        // -24 dB at the bottom of the usable area, +12 dB at the top
        const float lv = value (ID::satBand (b, "Level"));
        const float top = plot.getY() + 60.0f, bottom = plot.getBottom() - 56.0f;
        const float y = juce::jmap (juce::jlimit (-24.0f, 12.0f, lv), -24.0f, 12.0f, bottom, top);
        const auto region = bandRegion (b);
        return juce::Rectangle<float> (juce::jmin (78.0f, region.getWidth() - 12.0f), 20.0f).withCentre ({ region.getCentreX(), y });
    }

    juce::Rectangle<float> crossoverHandle (int j) const
    {
        return juce::Rectangle<float> (54.0f, 18.0f).withCentre ({ freqToX (crossover (j)), plot.getY() + 12.0f });
    }

    bool addButtonVisible() const
    {
        return mouseInside && satBands::count (apvts) < sat::maxBands && hoverCrossover < 0 && hoverHandle < 0
            && mousePos.y < plot.getY() + 26.0f && plot.contains (mousePos);
    }
    juce::Rectangle<float> addButtonRect() const { return juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ mousePos.x, plot.getY() + 12.0f }); }

    int hitCrossover (juce::Point<float> p) const
    {
        for (int j = 0; j < satBands::count (apvts) - 1; ++j)
            if (std::abs (freqToX (crossover (j)) - p.x) < 7.0f || crossoverHandle (j).contains (p)) return j;
        return -1;
    }
    int hitHandle (juce::Point<float> p) const
    {
        for (int b = 0; b < satBands::count (apvts); ++b)
            if (levelHandle (b).expanded (2.0f).contains (p)) return b;
        return -1;
    }

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    const StereoHistory& history;
    juce::Rectangle<float> plot;
    int selected = 0, dragging = -1, hoverCrossover = -1, hoverHandle = -1, draggingHandle = -1;
    bool driveDrag = false, moved = false, mouseInside = false, altHeld = false;
    float dragStartValue = 0.0f, dragStartY = 0.0f;
    juce::Point<float> mousePos;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftData, spectrum;
    std::array<float, sat::maxBands> bandLevels {};
};

//==============================================================================
/** Vertical slider with caption and value (tone controls). */
class ToneSlider : public juce::Component
{
public:
    explicit ToneSlider (const juce::String& caption)
    {
        label.setText (caption.toUpperCase(), juce::dontSendNotification);
        label.getProperties().set ("caption", true);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Colours_::textDim);
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
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

//==============================================================================
/** The SATURATION tab. */
class SaturationPage : public juce::Component
{
public:
    SaturationPage (CodexProcessor& p, const StereoHistory& h)
        : apvts (p.apvts), display (p, h),
          bandCount (*p.apvts.getParameter (ID::sat ("Bands")), { "1", "2", "3", "4", "5", "6" },
                     "Bands: number of frequency bands. Tip: hover the top of the display and click + to split a band where you want.", 1),
          channelMode (*p.apvts.getParameter (ID::sat ("Mode")), { "STEREO", "MID / SIDE" }, Help::forParameter (ID::sat ("Mode")))
    {
        power = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::sat ("On"), powerButton);
        powerButton.setTooltip ("Saturation On/Off: multiband saturation after Distortion 2");
        slope.attach (apvts, ID::sat ("Slope"));
        oversampling.attach (apvts, ID::sat ("OS"));
        input.attach (apvts, ID::sat ("In"));
        globalMix.attach (apvts, ID::sat ("Mix"));
        globalOut.attach (apvts, ID::sat ("Out"));

        // Style list with category headings
        juce::String lastCategory;
        for (int i = 0; i < (int) sat::styles().size(); ++i)
        {
            const auto& st = sat::styles()[(size_t) i];
            if (lastCategory != st.category) { style.box.addSectionHeading (st.category); lastCategory = st.category; }
            style.box.addItem (st.name, i + 1);
        }

        for (auto* b : { &mute, &solo, &bypass })
        {
            b->setComponentID ("pill");
            b->setClickingTogglesState (true);
        }
        removeButton.setTooltip ("Remove this band (its frequency range goes to the neighbouring band)");
        removeButton.onClick = [this]
        {
            satBands::removeBand (apvts, band);
            selectBand (juce::jmin (band, satBands::count (apvts) - 1));
        };

        display.onBandSelected = [this] (int b) { selectBand (b); };
        bandCount.onChange = [this] (int) { if (band >= satBands::count (apvts)) selectBand (satBands::count (apvts) - 1); };

        for (auto* c : std::initializer_list<juce::Component*> { &display, &powerButton, &bandCount, &channelMode, &slope, &oversampling,
                                                                &input, &globalMix, &globalOut, &style, &drive, &drivePan, &mix, &level, &levelPan,
                                                                &feedback, &feedbackFreq, &dynamics, &bass, &mid, &treble, &presence,
                                                                &mute, &solo, &bypass, &removeButton })
            addAndMakeVisible (c);
        selectBand (0);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionPanel (g, displayArea, "MULTIBAND SATURATION", Colours_::accent);
        drawSectionPanel (g, bandArea, "BAND " + juce::String (band + 1), Colours_::band (band));
        drawSectionPanel (g, globalArea, "GLOBAL", Colours_::accent);
        drawGroupCaption (g, levelCaption, "MIX  /  LEVEL");
        drawGroupCaption (g, colourCaption, "FEEDBACK  /  DYNAMICS");
        drawGroupCaption (g, toneCaption, "TONE");

        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (11.5f, 0.14f));
        g.drawText ("BANDS", bandCount.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);
        g.drawText ("CHANNEL MODE", channelMode.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        displayArea = r.removeFromTop (r.getHeight() * 44 / 100);
        powerButton.setBounds (displayArea.withHeight (28).removeFromRight (32).reduced (4));
        display.setBounds (displayArea.reduced (10).withTrimmedTop (30));
        r.removeFromTop (12);

        globalArea = r.removeFromRight (300);
        r.removeFromRight (12);
        bandArea = r;
        removeButton.setBounds (bandArea.withHeight (28).removeFromRight (32).reduced (5));

        // Band panel: [style, M/S/B, drive + drive pan] [mix/level row, feedback/dynamics row] [tone sliders]
        {
            auto c = bandArea.reduced (14).withTrimmedTop (30);
            auto left = c.removeFromLeft (200);
            style.setBounds (left.removeFromTop (48));
            left.removeFromTop (10);
            auto pills = left.removeFromTop (26);
            const int pw = (pills.getWidth() - 12) / 3;
            mute.setBounds (pills.removeFromLeft (pw)); pills.removeFromLeft (6);
            solo.setBounds (pills.removeFromLeft (pw)); pills.removeFromLeft (6);
            bypass.setBounds (pills);
            left.removeFromTop (6);
            auto panRow = left.removeFromBottom (76);
            drivePan.setBounds (panRow.withSizeKeepingCentre (90, panRow.getHeight()));
            const int driveH = juce::jmin (150, left.getHeight());
            drive.setBounds (left.withSizeKeepingCentre (juce::jmin (left.getWidth(), 130), driveH));
            c.removeFromLeft (16);

            auto toneArea = c.removeFromRight (240);
            c.removeFromRight (16);
            toneCaption = toneArea.removeFromTop (16);
            const int tw = toneArea.getWidth() / 4;
            bass.setBounds (toneArea.removeFromLeft (tw));
            mid.setBounds (toneArea.removeFromLeft (tw));
            treble.setBounds (toneArea.removeFromLeft (tw));
            presence.setBounds (toneArea);

            levelCaption = c.removeFromTop (16);
            auto row1 = c.removeFromTop (c.getHeight() / 2 - 8);
            c.removeFromTop (4);
            colourCaption = c.removeFromTop (16);
            const int w1 = row1.getWidth() / 3;
            mix.setBounds (row1.removeFromLeft (w1));
            level.setBounds (row1.removeFromLeft (w1));
            levelPan.setBounds (row1);
            const int w2 = c.getWidth() / 3;
            feedback.setBounds (c.removeFromLeft (w2));
            feedbackFreq.setBounds (c.removeFromLeft (w2));
            dynamics.setBounds (c);
        }

        // Global panel
        {
            auto c = globalArea.reduced (14).withTrimmedTop (30);
            c.removeFromTop (18);
            bandCount.setBounds (c.removeFromTop (28));
            c.removeFromTop (24);
            channelMode.setBounds (c.removeFromTop (28));
            c.removeFromTop (8);
            auto combos = c.removeFromTop (48);
            slope.setBounds (combos.removeFromLeft (combos.getWidth() / 2 - 4));
            combos.removeFromLeft (8);
            oversampling.setBounds (combos);
            c.removeFromTop (8);
            const int w = c.getWidth() / 3;
            input.setBounds (c.removeFromLeft (w));
            globalMix.setBounds (c.removeFromLeft (w));
            globalOut.setBounds (c);
        }
    }

private:
    void selectBand (int b)
    {
        band = juce::jlimit (0, sat::maxBands - 1, b);
        display.setSelectedBand (band);
        styleAttachment.reset();
        styleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, ID::satBand (band, "Style"), style.box);
        style.box.setTooltip (Help::forParameter (ID::satBand (band, "Style")));
        drive.attach (apvts, ID::satBand (band, "Drive"));
        drivePan.attach (apvts, ID::satBand (band, "DrivePan"));
        mix.attach (apvts, ID::satBand (band, "Mix"));
        level.attach (apvts, ID::satBand (band, "Level"));
        levelPan.attach (apvts, ID::satBand (band, "LevelPan"));
        feedback.attach (apvts, ID::satBand (band, "Fb"));
        feedbackFreq.attach (apvts, ID::satBand (band, "FbFreq"));
        dynamics.attach (apvts, ID::satBand (band, "Dyn"));
        bass.attach (apvts, ID::satBand (band, "Bass"));
        mid.attach (apvts, ID::satBand (band, "MidT"));
        treble.attach (apvts, ID::satBand (band, "Treble"));
        presence.attach (apvts, ID::satBand (band, "Pres"));
        muteAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::satBand (band, "Mute"), mute);
        soloAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::satBand (band, "Solo"), solo);
        bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::satBand (band, "Bypass"), bypass);
        mute.setTooltip (Help::forParameter (ID::satBand (band, "Mute")));
        solo.setTooltip (Help::forParameter (ID::satBand (band, "Solo")));
        bypass.setTooltip (Help::forParameter (ID::satBand (band, "Bypass")));
        drive.label.getProperties().set ("captionLarge", true);
        drive.label.setColour (juce::Label::textColourId, Colours_::band (band));
        repaint();
    }

    juce::AudioProcessorValueTreeState& apvts;
    SaturationBandDisplay display;
    PowerButton powerButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> power;
    SegmentedChoice bandCount, channelMode;
    ChoiceBox slope { "Crossover Slope" }, oversampling { "Oversampling" };
    Knob input { "Input" }, globalMix { "Mix" }, globalOut { "Output" };

    int band = 0;
    ChoiceBox style { "Style" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> styleAttachment;
    Knob drive { "Drive" }, drivePan { "Drive Pan" }, mix { "Mix" }, level { "Level" }, levelPan { "Level Pan" },
         feedback { "Feedback" }, feedbackFreq { "Feedback Freq" }, dynamics { "Dynamics" };
    ToneSlider bass { "Bass" }, mid { "Mid" }, treble { "Treble" }, presence { "Presence" };
    juce::TextButton mute { "MUTE" }, solo { "SOLO" }, bypass { "BYPASS" };
    IconButton removeButton { "remove", Icons::close(), false };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAtt, soloAtt, bypassAtt;

    juce::Rectangle<int> displayArea, bandArea, globalArea, levelCaption, colourCaption, toneCaption;
};
