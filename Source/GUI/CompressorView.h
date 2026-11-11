#pragma once
#include "Widgets.h"
#include "Help.h"
#include "PresetBar.h"
#include "../PluginProcessor.h"

namespace compUi
{
    inline juce::RangedAudioParameter* prm (juce::AudioProcessorValueTreeState& s, const juce::String& id) { return s.getParameter (id); }
    inline float val (juce::AudioProcessorValueTreeState& s, const juce::String& id) { return s.getRawParameterValue (id)->load(); }
    inline void setGesture (juce::AudioProcessorValueTreeState& s, const juce::String& id, float v)
    {
        if (auto* p = prm (s, id)) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (v)); p->endChangeGesture(); }
    }
    inline comp::BandSettings band (juce::AudioProcessorValueTreeState& s, int b)
    {
        comp::BandSettings bs;
        bs.direction = (int) val (s, ID::cmpBand (b, "Dir"));
        bs.thresholdDb = val (s, ID::cmpBand (b, "Thr"));
        bs.ratio = val (s, ID::cmpBand (b, "Ratio"));
        bs.kneeDb = val (s, ID::cmpBand (b, "Knee"));
        bs.rangeDb = val (s, ID::cmpBand (b, "Range"));
        return bs;
    }
}

/**
    Pro-C style level display: scrolling input (grey) and output (blue) levels with the gain change
    of every band drawn from the 0 dB line, plus a knee display (input -> output transfer curve) for the
    selected band on the right. Drag the threshold line (or the knee handle) to set the threshold;
    drag inside the knee display above the threshold to set the ratio.
*/
class CompressorDisplay : public juce::Component, private juce::Timer
{
public:
    CompressorDisplay (CodexProcessor& p) : proc (p), apvts (p.apvts)
    {
        history.assign (historySize, comp::MeterFrame { -100.0f, -100.0f, {} });
        startTimerHz (60);
    }

    void setSelectedBand (int b) { selected = b; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);

        const bool on = compUi::val (apvts, ID::cmp ("On")) > 0.5f;
        const int nb = (int) compUi::val (apvts, ID::cmp ("Bands"));
        const auto bs = compUi::band (apvts, selected);
        const int style = (int) compUi::val (apvts, ID::cmp ("Style"));
        const bool limit = compUi::val (apvts, ID::cmp ("Limit")) > 0.5f;

        // dB grid shared by both displays
        g.setFont (Fonts::regular (10.5f));
        for (int db = -6; db >= -54; db -= 6)
        {
            const float y = dbToY ((float) db);
            g.setColour (Colours_::grid);
            g.drawHorizontalLine (juce::roundToInt (y), levelArea.getX(), kneeArea.getRight());
            g.setColour (Colours_::textFaint);
            g.drawText (juce::String (db), juce::Rectangle<float> (levelArea.getRight() + 2.0f, y - 6.0f, 26.0f, 12.0f), juce::Justification::centred);
        }

        // ---- scrolling levels ----
        {
            juce::Path in, out;
            const float w = levelArea.getWidth();
            const int n = historySize;
            in.startNewSubPath (levelArea.getX(), levelArea.getBottom());
            out.startNewSubPath (levelArea.getX(), levelArea.getBottom());
            for (int i = 0; i < n; ++i)
            {
                const auto& f = history[(size_t) ((writePos + i) % n)];
                const float x = levelArea.getX() + w * (float) i / (float) (n - 1);
                in.lineTo (x, dbToY (f.inDb));
                out.lineTo (x, dbToY (f.outDb));
            }
            in.lineTo (levelArea.getRight(), levelArea.getBottom()); in.closeSubPath();
            out.lineTo (levelArea.getRight(), levelArea.getBottom()); out.closeSubPath();
            g.setColour (Colours_::textDim.withAlpha (0.22f));
            g.fillPath (in);
            g.setGradientFill (juce::ColourGradient (Colours_::accent.withAlpha (0.55f), 0, levelArea.getY(), Colours_::accent.withAlpha (0.12f), 0, levelArea.getBottom(), false));
            g.fillPath (out);

            // gain change per band: downward hangs from the top, upward rises from the bottom
            for (int band = 0; band < nb; ++band)
            {
                juce::Path gr;
                for (int i = 0; i < n; ++i)
                {
                    const auto& f = history[(size_t) ((writePos + i) % n)];
                    const float x = levelArea.getX() + w * (float) i / (float) (n - 1);
                    const float y = levelArea.getY() + juce::jlimit (-levelArea.getHeight() * 0.5f, levelArea.getHeight() * 0.5f, -f.grDb[(size_t) band] * grScale());
                    if (i == 0) gr.startNewSubPath (x, y); else gr.lineTo (x, y);
                }
                const auto col = nb > 1 ? Colours_::band (band) : Colours_::warn;
                g.setColour (col.withAlpha (band == selected || nb == 1 ? 0.95f : 0.45f));
                g.strokePath (gr, juce::PathStrokeType (band == selected ? 1.8f : 1.2f));
            }
            g.setColour (Colours_::textFaint);
            g.setFont (Fonts::label (10.5f, 0.12f));
            g.drawText ("GAIN CHANGE", levelArea.reduced (8, 4).removeFromTop (14), juce::Justification::topLeft);
        }

        // threshold line across the level display
        {
            const float y = dbToY (bs.thresholdDb);
            const auto col = nb > 1 ? Colours_::band (selected) : Colours_::accentHi;
            g.setColour (col.withAlpha (dragMode == 1 || hoverThreshold ? 0.95f : 0.6f));
            const float dashes[] = { 5.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> (levelArea.getX(), y, kneeArea.getRight(), y), dashes, 2, 1.2f);
            g.setFont (Fonts::medium (11.5f));
            g.drawText ("THRESHOLD " + juce::String (bs.thresholdDb, 1) + " dB", juce::Rectangle<float> (levelArea.getX() + 8, y - 16, 200, 14), juce::Justification::centredLeft);
        }

        // ---- knee display ----
        {
            g.setColour (Colours_::outline);
            g.drawVerticalLine (juce::roundToInt (kneeArea.getX()) - 1, kneeArea.getY(), kneeArea.getBottom());
            const float dashes[] = { 3.0f, 3.0f };
            g.setColour (Colours_::grid.brighter (0.4f));
            g.drawDashedLine (juce::Line<float> (kneeArea.getBottomLeft(), kneeArea.getTopRight()), dashes, 2, 1.0f);
            juce::Path curve;
            for (float x = kneeArea.getX(); x <= kneeArea.getRight(); x += 1.0f)
            {
                const float inDb = juce::jmap (x, kneeArea.getX(), kneeArea.getRight(), minDb, 0.0f);
                const float outDb = inDb + comp::gainComputer (inDb, bs, style, limit);
                const float y = juce::jlimit (kneeArea.getY(), kneeArea.getBottom(), dbToY (outDb));
                if (x <= kneeArea.getX()) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            }
            const auto col = on ? (nb > 1 ? Colours_::band (selected) : Colours_::accent) : Colours_::textFaint;
            Colours_::strokeGlow (g, curve, col, 2.0f);
            const auto h = kneeHandle();
            g.setColour (col.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (18, 18).withCentre (h));
            g.setColour (col);
            g.fillEllipse (juce::Rectangle<float> (9, 9).withCentre (h));

            // live input dot on the curve
            const float inNow = history[(size_t) ((writePos + historySize - 1) % historySize)].inDb;
            if (inNow > minDb)
            {
                const float outNow = inNow + comp::gainComputer (inNow, bs, style, limit);
                g.setColour (Colours_::text);
                g.fillEllipse (juce::Rectangle<float> (6, 6).withCentre ({ juce::jmap (inNow, minDb, 0.0f, kneeArea.getX(), kneeArea.getRight()), dbToY (outNow) }));
            }
            g.setColour (Colours_::textFaint);
            g.setFont (Fonts::label (10.5f, 0.12f));
            g.drawText ("KNEE", kneeArea.reduced (8, 4).removeFromTop (14), juce::Justification::topLeft);
            g.setColour (Colours_::text);
            g.setFont (Fonts::medium (12.0f));
            const juce::String ratio = bs.ratio >= 99.5f ? juce::String::fromUTF8 ("\xe2\x88\x9e") : juce::String (bs.ratio, bs.ratio < 10 ? 1 : 0);
            g.drawText (ratio + " : 1", kneeArea.reduced (8, 4).removeFromBottom (16), juce::Justification::bottomRight);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().toFloat().reduced (8.0f, 8.0f);
        kneeArea = r.removeFromRight (juce::jmin (r.getHeight(), r.getWidth() * 0.32f));
        r.removeFromRight (30.0f);
        levelArea = r;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool h = std::abs (e.position.y - dbToY (compUi::band (apvts, selected).thresholdDb)) < 6.0f || kneeHandle().getDistanceFrom (e.position) < 10.0f;
        if (h != hoverThreshold) { hoverThreshold = h; repaint(); }
        setMouseCursor (h ? juce::MouseCursor::UpDownResizeCursor : kneeArea.contains (e.position) ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragMode = 0;
        if (hoverThreshold) dragMode = 1;
        else if (kneeArea.contains (e.position) && e.position.y < dbToY (compUi::band (apvts, selected).thresholdDb)) dragMode = 2;
        if (dragMode == 0) return;
        startValue = compUi::val (apvts, ID::cmpBand (selected, dragMode == 1 ? "Thr" : "Ratio"));
        if (auto* p = compUi::prm (apvts, ID::cmpBand (selected, dragMode == 1 ? "Thr" : "Ratio"))) p->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragMode == 0) return;
        auto* p = compUi::prm (apvts, ID::cmpBand (selected, dragMode == 1 ? "Thr" : "Ratio"));
        const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
        if (dragMode == 1)
            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (-60.0f, 0.0f, startValue - (float) e.getDistanceFromDragStartY() * fine * (-minDb / levelArea.getHeight()))));
        else
            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->convertTo0to1 (startValue) + (float) e.getDistanceFromDragStartY() * fine * 0.004f));
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragMode != 0)
            if (auto* p = compUi::prm (apvts, ID::cmpBand (selected, dragMode == 1 ? "Thr" : "Ratio"))) p->endChangeGesture();
        dragMode = 0;
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (hoverThreshold) compUi::setGesture (apvts, ID::cmpBand (selected, "Thr"), -18.0f);
    }

private:
    void timerCallback() override
    {
        comp::MeterFrame frames[256];
        int got;
        bool any = false;
        while ((got = proc.getCompressor().meters.pull (frames, 256)) > 0)
        {
            for (int i = 0; i < got; ++i)
            {
                history[(size_t) writePos] = frames[i];
                writePos = (writePos + 1) % historySize;
            }
            any = true;
            if (got < 256) break;
        }
        if (! any && ! isMouseOverOrDragging())
        {
            // keep scrolling (silence) so the display does not freeze when the host stops
            if (++idle % 2 == 0) { history[(size_t) writePos] = { -100.0f, -100.0f, {} }; writePos = (writePos + 1) % historySize; }
        }
        repaint();
    }

    float dbToY (float db) const { return juce::jmap (juce::jlimit (minDb, 0.0f, db), minDb, 0.0f, levelArea.getBottom(), levelArea.getY()); }
    float grScale() const { return levelArea.getHeight() / 48.0f; }   // gain change drawn at 48 dB over the height
    juce::Point<float> kneeHandle() const
    {
        const auto bs = compUi::band (apvts, selected);
        return { juce::jmap (bs.thresholdDb, minDb, 0.0f, kneeArea.getX(), kneeArea.getRight()), dbToY (bs.thresholdDb) };
    }

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    static constexpr int historySize = 1600;   // ~4 s at 400 frames per second
    static constexpr float minDb = -60.0f;
    std::vector<comp::MeterFrame> history;
    int writePos = 0, selected = 0, dragMode = 0, idle = 0;
    float startValue = 0.0f;
    bool hoverThreshold = false;
    juce::Rectangle<float> levelArea, kneeArea;
};

/** Frequency strip: one segment per band (click to select), draggable crossovers and live gain-change meters. */
class CompressorBandBar : public juce::Component, private juce::Timer
{
public:
    CompressorBandBar (CodexProcessor& p) : proc (p), apvts (p.apvts) { startTimerHz (60); }
    std::function<void (int)> onBandSelected;
    int selected = 0;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 5.0f);
        const int nb = numBands();
        for (int b = 0; b < nb; ++b)
        {
            auto seg = segment (b);
            const auto col = Colours_::band (b);
            g.setColour (col.withAlpha (b == selected ? 0.22f : 0.08f));
            g.fillRoundedRectangle (seg.reduced (1.0f), 4.0f);
            if (b == selected) { g.setColour (col.withAlpha (0.8f)); g.drawRoundedRectangle (seg.reduced (1.0f), 4.0f, 1.2f); }

            // gain change meter (downward = red-ish bar from the top, upward = amber from the bottom)
            const float gr = proc.getCompressor().currentGr[(size_t) b].load (std::memory_order_relaxed);
            smoothed[(size_t) b] += 0.35f * (gr - smoothed[(size_t) b]);
            const float h = juce::jlimit (0.0f, seg.getHeight() - 4.0f, std::abs (smoothed[(size_t) b]) / 24.0f * (seg.getHeight() - 4.0f));
            auto meter = seg.reduced (4.0f, 2.0f).removeFromRight (5.0f);
            g.setColour (smoothed[(size_t) b] < 0 ? Colours_::warn : Colours_::good);
            g.fillRect (smoothed[(size_t) b] < 0 ? meter.withHeight (h) : meter.withTop (meter.getBottom() - h));

            g.setColour (b == selected ? Colours_::text : Colours_::textDim);
            g.setFont (Fonts::label (11.0f, 0.14f));
            const juce::String name = nb == 1 ? "FULL BAND" : "BAND " + juce::String (b + 1);
            const float shown = std::abs (smoothed[(size_t) b]) < 0.05f ? 0.0f : smoothed[(size_t) b];   // no "-0.0"
            g.drawText (name + "   " + juce::String (shown, 1) + " dB", seg.reduced (10.0f, 0.0f), juce::Justification::centredLeft);
        }
        for (int j = 0; j < nb - 1; ++j)
        {
            const float x = fx (xover (j));
            g.setColour (j == hoverX || j == dragX ? Colours_::text : Colours_::textDim);
            g.fillRect (x - 1.0f, r.getY() + 3.0f, 2.0f, r.getHeight() - 6.0f);
            g.setFont (Fonts::regular (10.5f));
            const float f = xover (j);
            // frequency right-aligned against its divider (inside the lower band), clear of the band names
            g.drawText (f < 1000 ? juce::String (f, 0) + " Hz" : juce::String (f / 1000.0f, 2) + " kHz",
                        juce::Rectangle<float> (x - 72.0f, r.getY(), 66.0f, r.getHeight()), juce::Justification::centredRight);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hitX (e.position.x);
        if (h != hoverX) { hoverX = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hoverX = -1; }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragX = hitX (e.position.x);
        if (dragX >= 0) { if (auto* p = compUi::prm (apvts, xId (dragX))) p->beginChangeGesture(); return; }
        for (int b = 0; b < numBands(); ++b)
            if (segment (b).contains (e.position)) { selected = b; if (onBandSelected) onBandSelected (b); repaint(); }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragX < 0) return;
        const float lo = dragX > 0 ? xover (dragX - 1) * 1.1f : 25.0f, hi = dragX < numBands() - 2 ? xover (dragX + 1) / 1.1f : 18000.0f;
        if (auto* p = compUi::prm (apvts, xId (dragX))) p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (lo, hi, xf (e.position.x))));
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragX >= 0) if (auto* p = compUi::prm (apvts, xId (dragX))) p->endChangeGesture();
        dragX = -1;
    }

private:
    void timerCallback() override { repaint(); }
    int numBands() const { return juce::jlimit (1, comp::maxBands, (int) compUi::val (apvts, ID::cmp ("Bands"))); }
    static juce::String xId (int j) { return ID::cmp (("X" + juce::String (j + 1)).toRawUTF8()); }
    float xover (int j) const { return compUi::val (apvts, xId (j)); }
    float fx (float f) const { auto r = getLocalBounds().toFloat(); return r.getX() + r.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float xf (float x) const { auto r = getLocalBounds().toFloat(); return 20.0f * std::pow (1000.0f, (x - r.getX()) / r.getWidth()); }
    juce::Rectangle<float> segment (int b) const
    {
        auto r = getLocalBounds().toFloat();
        const int nb = numBands();
        const float x0 = b == 0 ? r.getX() : fx (xover (b - 1)), x1 = b == nb - 1 ? r.getRight() : fx (xover (b));
        return { x0, r.getY(), x1 - x0, r.getHeight() };
    }
    int hitX (float x) const
    {
        for (int j = 0; j < numBands() - 1; ++j) if (std::abs (fx (xover (j)) - x) < 6.0f) return j;
        return -1;
    }

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    std::array<float, comp::maxBands> smoothed {};
    int hoverX = -1, dragX = -1;
};

/** The COMPRESSOR tab. */
class CompressorPage : public juce::Component, private juce::Timer
{
public:
    explicit CompressorPage (CodexProcessor& p)
        : apvts (p.apvts), display (p), bandBar (p),
          bandCount (*p.apvts.getParameter (ID::cmp ("Bands")), { "1", "2", "3", "4" }, Help::forParameter (ID::cmp ("Bands")), 1),
          t4Mode (*p.apvts.getParameter (ID::cmp ("Limit")), { "COMPRESS", "LIMIT" }, Help::forParameter (ID::cmp ("Limit")))
    {
        power = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::cmp ("On"), powerButton);
        powerButton.setTooltip (Help::forParameter (ID::cmp ("On")));
        style.attach (apvts, ID::cmp ("Style"));
        lookahead.attach (apvts, ID::cmp ("Look"));
        hold.attach (apvts, ID::cmp ("Hold"));
        scHp.attach (apvts, ID::cmp ("ScHp"));
        mix.attach (apvts, ID::cmp ("Mix"));
        output.attach (apvts, ID::cmp ("Out"));
        autoGainAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::cmp ("Auto"), autoGain);
        autoGain.setTooltip (Help::forParameter (ID::cmp ("Auto")));
        for (auto* b : { &autoGain, &mute, &solo, &bypass })
        {
            b->setComponentID ("pill");
            b->setClickingTogglesState (true);
        }

        display.setInterceptsMouseClicks (true, false);
        bandBar.onBandSelected = [this] (int b) { selectBand (b); };
        bandCount.onChange = [this] (int) { if (band >= numBands()) selectBand (numBands() - 1); };

        notice.title = "COMPRESSOR IS OFF";
        notice.subtitle = "Multiband downward / upward compression, after Saturation.";
        notice.enable.onClick = [this] { compUi::setGesture (apvts, ID::cmp ("On"), 1.0f); };

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &powerButton, &display, &bandBar, &style, &bandCount, &lookahead, &t4Mode, &hold, &scHp, &mix, &output, &autoGain,
                 &threshold, &ratio, &knee, &range, &attack, &release, &gain, &mute, &solo, &bypass })
            addAndMakeVisible (c);
        addChildComponent (notice);
        onParam = apvts.getRawParameterValue (ID::cmp ("On"));
        selectBand (0);
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        const auto acc = off ? Colours_::textFaint : Colours_::accent;
        drawSectionPanel (g, displayArea, juce::String::fromUTF8 ("COMPRESSOR  \xc2\xb7  DOWNWARD / UPWARD"), acc);
        drawSectionPanel (g, bandArea, numBands() > 1 ? "BAND " + juce::String (band + 1) : juce::String ("FULL BAND"), numBands() > 1 ? Colours_::band (band) : acc);
        drawSectionPanel (g, globalArea, "GLOBAL", acc);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (11.5f, 0.14f));
        g.drawText ("DIRECTION", direction.translated (0, -18).withHeight (16), juce::Justification::centredLeft);
        g.drawText ("BANDS", bandCount.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);
        g.drawText ("T4 OPTO MODE", t4Mode.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        displayArea = r.removeFromTop (r.getHeight() * 47 / 100);
        powerButton.setBounds (displayArea.withHeight (28).removeFromRight (32).reduced (4));
        {
            auto c = displayArea.reduced (10).withTrimmedTop (30);
            bandBar.setBounds (c.removeFromBottom (30));
            c.removeFromBottom (6);
            display.setBounds (c);
        }
        r.removeFromTop (12);
        globalArea = r.removeFromRight (330);
        r.removeFromRight (12);
        bandArea = r;

        {
            auto c = bandArea.reduced (14).withTrimmedTop (30);
            auto top = c.removeFromTop (46);
            top.removeFromTop (18);
            direction = top.removeFromLeft (300);
            if (dirButtons == nullptr) makeDirectionButtons();
            {
                auto d = direction;
                const int w = d.getWidth() / 3;
                for (int i = 0; i < 3; ++i) dirButtons[i].setBounds (i < 2 ? d.removeFromLeft (w) : d);
            }
            top.removeFromLeft (20);
            const int pw = 64;
            bypass.setBounds (top.removeFromRight (pw + 10).withTrimmedLeft (10));
            solo.setBounds (top.removeFromRight (pw + 6).withTrimmedLeft (6));
            mute.setBounds (top.removeFromRight (pw));
            c.removeFromTop (10);
            const int w = c.getWidth() / 7;
            for (auto* k : { &threshold, &ratio, &knee, &range, &attack, &release, &gain })
                k->setBounds (c.removeFromLeft (w).withSizeKeepingCentre (w, juce::jmin (c.getHeight(), 120)));
        }
        {
            auto c = globalArea.reduced (14).withTrimmedTop (30);
            auto row = c.removeFromTop (48);
            style.setBounds (row.removeFromLeft (row.getWidth() * 3 / 5));
            row.removeFromLeft (8);
            lookahead.setBounds (row);
            c.removeFromTop (24);
            auto row2 = c.removeFromTop (28);
            bandCount.setBounds (row2.removeFromLeft (row2.getWidth() / 2 - 6));
            row2.removeFromLeft (12);
            t4Mode.setBounds (row2);
            c.removeFromTop (8);
            const int kw = c.getWidth() / 4;
            auto knobs = c.removeFromTop (juce::jmin (c.getHeight() - 30, 104));
            for (auto* k : { &hold, &scHp, &mix, &output })
                k->setBounds (knobs.removeFromLeft (kw));
            c.removeFromTop (2);
            autoGain.setBounds (c.removeFromTop (26).withSizeKeepingCentre (130, 24));
        }
        notice.setBounds (getLocalBounds().withTrimmedTop (29).reduced (1));
    }

private:
    int numBands() const { return juce::jlimit (1, comp::maxBands, (int) compUi::val (apvts, ID::cmp ("Bands"))); }

    void makeDirectionButtons()
    {
        dirButtons = std::make_unique<juce::TextButton[]> (3);
        for (int i = 0; i < 3; ++i)
        {
            auto& b = dirButtons[i];
            b.setButtonText (comp::directionNames()[i].toUpperCase());
            b.setComponentID ("seg");
            b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 2 ? juce::Button::ConnectedOnRight : 0));
            b.setTooltip (Help::forParameter ("c1Dir"));
            b.onClick = [this, i] { compUi::setGesture (apvts, ID::cmpBand (band, "Dir"), (float) i); };
            addAndMakeVisible (b);
        }
    }

    void selectBand (int b)
    {
        band = juce::jlimit (0, comp::maxBands - 1, b);
        display.setSelectedBand (band);
        bandBar.selected = band;
        threshold.attach (apvts, ID::cmpBand (band, "Thr"));
        ratio.attach (apvts, ID::cmpBand (band, "Ratio"));
        knee.attach (apvts, ID::cmpBand (band, "Knee"));
        range.attach (apvts, ID::cmpBand (band, "Range"));
        attack.attach (apvts, ID::cmpBand (band, "Att"));
        release.attach (apvts, ID::cmpBand (band, "Rel"));
        gain.attach (apvts, ID::cmpBand (band, "Gain"));
        muteAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::cmpBand (band, "Mute"), mute);
        soloAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::cmpBand (band, "Solo"), solo);
        bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::cmpBand (band, "Bypass"), bypass);
        const auto col = numBands() > 1 ? Colours_::band (band) : Colours_::accent;
        threshold.label.setColour (juce::Label::textColourId, col);
        timerCallback();
        repaint();
    }

    void timerCallback() override
    {
        const bool o = compUi::val (apvts, ID::cmp ("On")) < 0.5f;
        if (o != off) { off = o; notice.setVisible (off); repaint(); }

        // direction buttons follow the parameter; styles that set their own behaviour grey out unused controls
        if (dirButtons != nullptr)
        {
            const int d = (int) compUi::val (apvts, ID::cmpBand (band, "Dir"));
            for (int i = 0; i < 3; ++i) dirButtons[i].setToggleState (i == d, juce::dontSendNotification);
        }
        const int st = (int) compUi::val (apvts, ID::cmp ("Style"));
        auto enable = [] (juce::Component& c, bool on) { if (c.isEnabled() != on) { c.setEnabled (on); c.setAlpha (on ? 1.0f : 0.35f); } };
        const bool t4 = st == comp::T4Opto;
        enable (attack, ! t4);
        enable (release, ! t4);
        enable (ratio, ! t4);
        enable (t4Mode, t4);
        for (int i = 0; dirButtons != nullptr && i < 3; ++i) enable (dirButtons[i], st != comp::TTM);
        if (numBands() != lastBands) { lastBands = numBands(); if (band >= lastBands) selectBand (lastBands - 1); repaint(); }
    }

    juce::AudioProcessorValueTreeState& apvts;
    CompressorDisplay display;
    CompressorBandBar bandBar;
    PowerButton powerButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> power;
    ChoiceBox style { "Style" }, lookahead { "Lookahead" };
    SegmentedChoice bandCount, t4Mode;
    Knob hold { "Hold" }, scHp { "SC High-Pass" }, mix { "Mix" }, output { "Output" };
    juce::TextButton autoGain { "AUTO GAIN" }, mute { "MUTE" }, solo { "SOLO" }, bypass { "BYPASS" };
    // declared after the buttons: members are destroyed in reverse order, so the attachments go before their buttons
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoGainAtt, muteAtt, soloAtt, bypassAtt;
    Knob threshold { "Threshold" }, ratio { "Ratio" }, knee { "Knee" }, range { "Range" }, attack { "Attack" }, release { "Release" }, gain { "Gain" };
    std::unique_ptr<juce::TextButton[]> dirButtons;
    juce::Rectangle<int> direction;
    BypassNotice notice;
    std::atomic<float>* onParam = nullptr;
    int band = 0, lastBands = -1;
    bool off = false;
    juce::Rectangle<int> displayArea, bandArea, globalArea;
};
