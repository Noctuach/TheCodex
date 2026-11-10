#include "EQDisplay.h"
#include "../PluginProcessor.h"
#include "../DSP/PitchDetector.h"

using namespace juce;

namespace
{
    const float displayRanges[] = { 3.0f, 6.0f, 12.0f, 30.0f };

    /** Colours of the per-channel result curves and node badges. */
    Colour placementColour (eq::Placement p)
    {
        switch (p)
        {
            case eq::Placement::Left:  return Colour (0xffffb84d);
            case eq::Placement::Right: return Colour (0xffff6b6b);
            case eq::Placement::Mid:   return Colours_::good;
            case eq::Placement::Side:  return Colours_::accent2;
            default:                   return Colour (0xffd6f0ff);
        }
    }
    const char* placementName (eq::Placement p)
    {
        const char* n[] = { "Stereo", "Left", "Right", "Mid", "Side" };
        return n[(int) p];
    }
    const char* placementTag (eq::Placement p)
    {
        const char* n[] = { "", "L", "R", "M", "S" };
        return n[(int) p];
    }
}

//==============================================================================
EQDisplay::EQDisplay (CodexProcessor& p, int eqIndex) : eqIdx (eqIndex), proc (p), apvts (p.apvts)
{
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    pullBuffer.assign (32768, 0.0f);
    post.init (fftSize);
    pre.init (fftSize);

    for (int i = 0; i < 4; ++i)
        rangeBox.addItem (String (CharPointer_UTF8 ("\xc2\xb1")) + String ((int) displayRanges[i]) + " dB", i + 1);
    rangeBox.setSelectedId (3, dontSendNotification);
    rangeBox.setTooltip ("Display range: zoom the gain scale (3, 6, 12 or 30 dB). It expands automatically when a band needs more room.");
    rangeBox.onChange = [this] { repaint(); };
    addAndMakeVisible (rangeBox);

    for (auto* b : { &preButton, &postButton, &freezeButton })
    {
        b->setComponentID ("pill");
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    preButton.setTooltip (eqIdx == 0 ? "Pre: spectrum going into the EQ (after Distortion 1)" : "Pre: spectrum going into EQ 2 (after the Compressor)");

    // Processing mode (Pro-Q 4): Zero Latency / Natural Phase / Linear Phase, plus the linear-phase resolution
    modeBox.addItemList (eq::processingModeNames(), 1);
    resBox.addItemList (eq::resolutionNames(), 1);
    modeAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, ID::eqMode (eqIdx), modeBox);
    resAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, ID::eqRes (eqIdx), resBox);
    modeBox.setTooltip (Help::forParameter (ID::eqMode (eqIdx)));
    resBox.setTooltip (Help::forParameter (ID::eqRes (eqIdx)));
    modeBox.onChange = [this] { resBox.setVisible (modeBox.getSelectedItemIndex() == 2); };
    addAndMakeVisible (modeBox);
    addChildComponent (resBox);
    resBox.setVisible (modeBox.getSelectedItemIndex() == 2);
    postButton.setTooltip ("Post: spectrum after the whole chain");
    freezeButton.setTooltip ("Freeze: the spectrum stops falling and builds up a maximum over time");
    postButton.setToggleState (true, dontSendNotification);
    preButton.onClick = [this] { showPre = preButton.getToggleState(); };
    postButton.onClick = [this] { showPost = postButton.getToggleState(); };
    freezeButton.onClick = [this]
    {
        freeze = freezeButton.getToggleState();
        if (! freeze) { std::fill (post.db.begin(), post.db.end(), -150.0f); std::fill (pre.db.begin(), pre.db.end(), -150.0f); }
    };
    analyzerMenuButton.setTooltip ("Analyzer settings: range, speed and tilt");
    analyzerMenuButton.onClick = [this] { showAnalyzerMenu(); };
    addAndMakeVisible (analyzerMenuButton);
    setWantsKeyboardFocus (true);

    startTimerHz (60);
}

void EQDisplay::resized()
{
    plot = getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (16.0f).withTrimmedRight (30.0f);
    rangeBox.setBounds (getWidth() - 96, 6, 88, 24);
    modeBox.setBounds (rangeBox.getX() - 146, 6, 138, 24);
    resBox.setBounds (modeBox.getX() - 112, 6, 104, 24);
    auto row = Rectangle<int> (8, 6, 260, 22);
    preButton.setBounds (row.removeFromLeft (52)); row.removeFromLeft (4);
    postButton.setBounds (row.removeFromLeft (56)); row.removeFromLeft (4);
    freezeButton.setBounds (row.removeFromLeft (70)); row.removeFromLeft (4);
    analyzerMenuButton.setBounds (row.removeFromLeft (26));
}

float EQDisplay::freqToX (float f) const { return plot.getX() + plot.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq); }
float EQDisplay::xToFreq (float x) const { return minFreq * std::pow (maxFreq / minFreq, (x - plot.getX()) / plot.getWidth()); }
float EQDisplay::gainToY (float db) const { return jmap (db, -displayRange, displayRange, plot.getBottom(), plot.getY()); }
float EQDisplay::yToGain (float y) const  { return jmap (y, plot.getBottom(), plot.getY(), -displayRange, displayRange); }
float EQDisplay::analyzerDbToY (float db) const { return jmap (db, -analyzerRange, 6.0f, plot.getBottom(), plot.getY()); }

RangedAudioParameter* EQDisplay::param (int b, const char* suffix) const { return apvts.getParameter (ID::eqBand (eqIdx, b, suffix)); }

void EQDisplay::setParam (int b, const char* suffix, float value)
{
    if (auto* prm = param (b, suffix))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

void EQDisplay::setParamGesture (int b, const char* suffix, float value)
{
    if (auto* prm = param (b, suffix))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        prm->endChangeGesture();
    }
}

float EQDisplay::gainScale() const { return apvts.getRawParameterValue (ID::eqScaleFor (eqIdx))->load(); }

eq::BandSettings EQDisplay::readBand (int b, bool scaled) const
{
    auto v = [this, b] (const char* sfx) { return apvts.getRawParameterValue (ID::eqBand (eqIdx, b, sfx))->load(); };
    eq::BandSettings s;
    s.on        = v ("On") > 0.5f;
    s.type      = (eq::BandType) (int) v ("Type");
    s.freq      = v ("Freq");
    s.gainDb    = v ("Gain") * (scaled ? gainScale() : 1.0f);
    s.q         = v ("Q");
    s.slope     = (int) v ("Slope");
    s.placement = (eq::Placement) (int) v ("Place");
    return s;
}

Point<float> EQDisplay::nodePosition (int b) const
{
    const auto s = readBand (b);
    return { freqToX (s.freq), eq::typeUsesGain (s.type) ? gainToY (s.gainDb) : gainToY (0.0f) };
}

int EQDisplay::hitTestNode (Point<float> pt) const
{
    int best = -1;
    float bestDist = 12.0f;
    for (int b = 0; b < Params::numBands; ++b)
    {
        if (! present (b)) continue;
        const float d = nodePosition (b).getDistanceFrom (pt);
        const float bias = readBand (b).on ? 0.0f : 3.0f;   // prefer enabled bands when nodes overlap
        if (d + bias < bestDist) { bestDist = d + bias; best = b; }
    }
    return best;
}

//==============================================================================
void EQDisplay::updateSpectrum (bool preEq, Spectrum& sp)
{
    auto& fifo = preEq ? (eqIdx == 0 ? proc.analyzerPre : proc.analyzerPre2) : proc.analyzer;
    const int got = fifo.pull (pullBuffer.data(), (int) pullBuffer.size());
    if (got >= fftSize)
        std::copy (pullBuffer.begin() + (got - fftSize), pullBuffer.begin() + got, sp.ring.begin());
    else if (got > 0)
    {
        std::move (sp.ring.begin() + got, sp.ring.end(), sp.ring.begin());
        std::copy (pullBuffer.begin(), pullBuffer.begin() + got, sp.ring.end() - got);
    }
    if (! (preEq ? showPre : showPost)) return;

    std::fill (fftData.begin(), fftData.end(), 0.0f);
    std::copy (sp.ring.begin(), sp.ring.end(), fftData.begin());
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float norm = 4.0f / (float) fftSize;   // Hann window coherent gain compensation
    for (size_t i = 0; i < sp.db.size(); ++i)
    {
        const float db = Decibels::gainToDecibels (fftData[i] * norm, -150.0f);
        auto& v = sp.db[i];
        v = db > v ? db : (freeze ? v : jmax (db, v - analyzerDecay));
    }
}

void EQDisplay::timerCallback()
{
    updateSpectrum (false, post);
    updateSpectrum (true, pre);

    // Display range: the chosen range, expanded when a band needs more room (Pro-Q's auto-adjust)
    float needed = 0.0f;
    for (int b = 0; b < Params::numBands; ++b)
    {
        const auto s = readBand (b);
        if (s.on && eq::typeUsesGain (s.type)) needed = jmax (needed, std::abs (s.gainDb));
    }
    float range = displayRanges[jlimit (0, 3, rangeBox.getSelectedId() - 1)];
    for (float r : displayRanges)
        if (range < needed && r > range) range = r;
    displayRange = range;

    // Keep the selection on a band that exists (e.g. after loading a preset)
    if (! present (selected) && dragging < 0)
        for (int b = 0; b < Params::numBands; ++b)
            if (present (b)) { selected = b; if (onBandSelected) onBandSelected (b); break; }

    repaint();
}

void EQDisplay::showAnalyzerMenu()
{
    PopupMenu menu, range, speed, tilt;
    for (float r : { 60.0f, 90.0f, 120.0f }) range.addItem (10 + (int) r, String ((int) r) + " dB", true, exactlyEqual (analyzerRange, r));
    const char* speeds[] = { "Slow", "Medium", "Fast" };
    const float decays[] = { 0.6f, 1.5f, 3.5f };
    for (int i = 0; i < 3; ++i) speed.addItem (200 + i, speeds[i], true, exactlyEqual (analyzerDecay, decays[i]));
    const float tilts[] = { 0.0f, 3.0f, 4.5f, 6.0f };
    for (int i = 0; i < 4; ++i) tilt.addItem (300 + i, String (tilts[i], 1) + " dB/oct", true, exactlyEqual (analyzerTilt, tilts[i]));
    menu.addSectionHeader ("Analyzer");
    menu.addSubMenu ("Range", range);
    menu.addSubMenu ("Speed", speed);
    menu.addSubMenu ("Tilt", tilt);

    SafePointer<EQDisplay> safe (this);
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&analyzerMenuButton), [safe, decays, tilts] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r >= 70 && r <= 130) safe->analyzerRange = (float) (r - 10);
        else if (r >= 200 && r < 203) safe->analyzerDecay = decays[r - 200];
        else if (r >= 300 && r < 304) safe->analyzerTilt = tilts[r - 300];
        safe->repaint();
    });
}

//==============================================================================
void EQDisplay::paint (Graphics& g)
{
    g.setColour (Colours_::display);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
    g.setColour (Colours_::outline);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);

    drawGrid (g);
    drawAnalyzer (g);
    drawCurves (g);
    drawNodes (g);
    drawHoverReadout (g);
}

void EQDisplay::drawGrid (Graphics& g)
{
    g.setFont (Fonts::regular (11.0f));
    const float freqs[] = { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 800, 1000, 2000, 3000, 4000, 5000, 8000, 10000, 20000 };
    for (float f : freqs)
    {
        const float x = freqToX (f);
        const bool major = exactlyEqual (f, 50.0f) || exactlyEqual (f, 100.0f) || exactlyEqual (f, 500.0f)
                        || exactlyEqual (f, 1000.0f) || exactlyEqual (f, 5000.0f) || exactlyEqual (f, 10000.0f);
        g.setColour (major ? Colours_::grid.brighter (0.35f) : Colours_::grid);
        g.drawVerticalLine (roundToInt (x), plot.getY(), plot.getBottom());
        if (major || exactlyEqual (f, 20.0f) || exactlyEqual (f, 20000.0f))
        {
            const String label = f >= 1000 ? String ((int) (f / 1000)) + "k" : String ((int) f);
            g.setColour (Colours_::textDim);
            const float lx = jlimit (plot.getX(), plot.getRight() - 40.0f, x - 20.0f);
            g.drawText (label, Rectangle<float> (lx, plot.getBottom() + 1, 40, 14), f <= 20 ? Justification::centredLeft : Justification::centred);
        }
    }

    const float step = displayRange <= 3.0f ? 1.0f : displayRange <= 6.0f ? 2.0f : displayRange <= 12.0f ? 3.0f : 6.0f;
    for (float db = -displayRange + step; db <= displayRange - step + 0.01f; db += step)
    {
        const float y = gainToY (db);
        g.setColour (std::abs (db) < 0.01f ? Colours_::grid.brighter (0.7f) : Colours_::grid);
        g.drawHorizontalLine (roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (Colours_::textDim);
        g.drawText ((db > 0 ? "+" : "") + String ((int) db), Rectangle<float> (plot.getRight() + 2, y - 7, 28, 14), Justification::centredLeft);
    }
}

void EQDisplay::drawAnalyzer (Graphics& g)
{
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
    const float binHz = (float) (sr / fftSize);

    auto build = [&] (const std::vector<float>& spectrum)
    {
        const int numBins = (int) spectrum.size();
        Path path;
        const int x0 = (int) plot.getX(), x1 = (int) plot.getRight();
        for (int x = x0; x <= x1; x += 2)
        {
            const float f = xToFreq ((float) x);
            const float fLo = xToFreq ((float) x - 1.0f), fHi = xToFreq ((float) x + 1.0f);
            const float binLo = fLo / binHz, binHi = fHi / binHz;
            float db;
            if (binHi - binLo < 1.0f)
            {
                const float pos = jlimit (0.0f, (float) numBins - 2.0f, f / binHz);
                const int i = (int) pos;
                db = jmap (pos - (float) i, spectrum[(size_t) i], spectrum[(size_t) i + 1]);
            }
            else
            {
                db = -150.0f;
                for (int i = jmax (0, (int) binLo); i <= jmin (numBins - 1, (int) binHi); ++i)
                    db = jmax (db, spectrum[(size_t) i]);
            }
            db += analyzerTilt * std::log2 (f / 1000.0f);   // tilt around 1 kHz
            const float y = jlimit (plot.getY(), plot.getBottom(), analyzerDbToY (db));
            if (x == x0) { path.startNewSubPath ((float) x, plot.getBottom()); path.lineTo ((float) x, y); }
            else path.lineTo ((float) x, y);
        }
        path.lineTo ((float) x1, plot.getBottom());
        path.closeSubPath();
        return path;
    };

    if (showPre)
    {
        auto p = build (pre.db);
        g.setColour (Colours_::textDim.withAlpha (0.12f));
        g.fillPath (p);
        g.setColour (Colours_::textDim.withAlpha (0.45f));
        g.strokePath (p, PathStrokeType (1.0f));
    }
    if (showPost)
    {
        auto p = build (post.db);
        g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (0.30f), 0, plot.getY(), Colours_::accent2.withAlpha (0.04f), 0, plot.getBottom(), false));
        g.fillPath (p);
        g.setColour ((freeze ? Colours_::accentHi : Colours_::accent).withAlpha (0.5f));
        g.strokePath (p, PathStrokeType (1.0f));
    }
}

void EQDisplay::drawCurves (Graphics& g)
{
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;

    std::array<eq::BandDesign, Params::numBands> designs;
    std::array<eq::Placement, Params::numBands> places {};
    std::array<bool, 5> used {};   // which placements are in use
    for (int b = 0; b < Params::numBands; ++b)
    {
        auto bs = readBand (b);
        bs.on = bs.on && present (b);
        designs[(size_t) b] = eq::design (bs, sr);
        places[(size_t) b] = bs.placement;
        if (bs.on) used[(size_t) bs.placement] = true;
    }
    const bool anyGroup = used[1] || used[2] || used[3] || used[4];

    const int x0 = (int) plot.getX(), x1 = (int) plot.getRight();
    std::array<Path, 5> curves;   // [0] = all channels (stereo bands), [1..4] = L, R, M, S results
    Path sel;
    const bool selOn = readBand (selected).on && present (selected);

    for (int x = x0; x <= x1; ++x)
    {
        const double f = xToFreq ((float) x);
        std::array<double, 5> sum {};
        for (int b = 0; b < Params::numBands; ++b)
        {
            if (designs[(size_t) b].numStages == 0) continue;
            const double db = 20.0 * std::log10 (jmax (1.0e-6, eq::magnitude (designs[(size_t) b], f, sr)));
            const int p = (int) places[(size_t) b];
            if (p == 0) for (auto& s : sum) s += db;   // stereo bands affect every result
            else sum[(size_t) p] += db;
            if (b == selected)
            {
                const float y = jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, gainToY ((float) db));
                if (x == x0) sel.startNewSubPath ((float) x, y); else sel.lineTo ((float) x, y);
            }
        }
        for (int p = 0; p < 5; ++p)
        {
            if (p > 0 && ! used[(size_t) p]) continue;
            const float y = jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, gainToY ((float) sum[(size_t) p]));
            if (x == x0) curves[(size_t) p].startNewSubPath ((float) x, y); else curves[(size_t) p].lineTo ((float) x, y);
        }
    }

    // Selected band's own curve, filled to the 0 dB line
    if (selOn && ! sel.isEmpty())
    {
        Path fill (sel);
        fill.lineTo ((float) x1, gainToY (0.0f));
        fill.lineTo ((float) x0, gainToY (0.0f));
        fill.closeSubPath();
        g.setColour (Colours_::band (selected).withAlpha (0.18f));
        g.fillPath (fill);
        g.setColour (Colours_::band (selected).withAlpha (0.7f));
        g.strokePath (sel, PathStrokeType (1.2f));
    }

    for (int p = 1; p < 5; ++p)
        if (used[(size_t) p])
            Colours_::strokeGlow (g, curves[(size_t) p], placementColour ((eq::Placement) p), 1.6f);
    Colours_::strokeGlow (g, curves[0], placementColour (eq::Placement::Stereo), anyGroup ? 1.6f : 2.0f);

    // Legend when channel-specific bands exist
    if (anyGroup)
    {
        g.setFont (Fonts::label (11.0f, 0.14f));
        float x = plot.getX() + 10.0f;
        const float y = plot.getY() + 34.0f;
        for (int p = 0; p < 5; ++p)
        {
            if (p > 0 && ! used[(size_t) p]) continue;
            const auto c = placementColour ((eq::Placement) p);
            g.setColour (c);
            g.fillRect (x, y + 6.0f, 14.0f, 2.0f);
            const String name = p == 0 ? "ALL" : String (placementName ((eq::Placement) p)).toUpperCase();
            g.drawText (name, Rectangle<float> (x + 18.0f, y, 60.0f, 14.0f), Justification::centredLeft);
            x += 30.0f + 9.0f * (float) name.length();
        }
    }
}

void EQDisplay::drawNodes (Graphics& g)
{
    for (int b = 0; b < Params::numBands; ++b)
    {
        if (! present (b)) continue;
        const auto s = readBand (b);
        const auto pos = nodePosition (b);
        const auto col = Colours_::band (b);
        const float r = 7.5f;
        const auto circle = Rectangle<float> (r * 2, r * 2).withCentre (pos);

        if (s.on)
        {
            g.setColour (col.withAlpha (0.25f));
            g.fillEllipse (circle.expanded (4.0f));
            g.setColour (col);
            g.fillEllipse (circle);
            g.setColour (Colours::black);
            g.setFont (Fonts::semibold (10.0f));
            g.drawText (String (b + 1), circle, Justification::centred);
        }
        else
        {
            g.setColour (col.withAlpha (0.35f));
            g.drawEllipse (circle.reduced (1.0f), 1.5f);
        }

        if (b == selected)
        {
            g.setColour (Colours::white);
            g.drawEllipse (circle.expanded (3.0f), 1.5f);
        }

        if (s.placement != eq::Placement::Stereo)
        {
            auto badge = Rectangle<float> (13.0f, 13.0f).withCentre (pos.translated (10.0f, -10.0f));
            g.setColour (placementColour (s.placement));
            g.fillEllipse (badge);
            g.setColour (Colours::black);
            g.setFont (Fonts::label (10.0f, 0.0f));
            g.drawText (placementTag (s.placement), badge, Justification::centred);
        }

        if (b == hover || b == dragging)
        {
            String text = String (eq::bandTypeNames()[(int) s.type]) + "  "
                        + (s.freq < 1000 ? String (s.freq, 1) + " Hz" : String (s.freq / 1000.0f, 2) + " kHz");
            if (eq::typeUsesGain (s.type)) text << "  " << (s.gainDb > 0 ? "+" : "") << String (s.gainDb, 1) << " dB";
            if (eq::typeUsesSlope (s.type)) text << "  " << eq::slopeDb[s.slope] << " dB/oct";
            else text << "  Q " << String (s.q, 2);
            if (s.placement != eq::Placement::Stereo) text << "  " << placementName (s.placement);
            if (! s.on) text << "  (off)";

            g.setFont (Fonts::medium (12.0f));
            GlyphArrangement ga;
            ga.addLineOfText (g.getCurrentFont(), text, 0, 0);
            const float w = ga.getBoundingBox (0, -1, true).getWidth() + 20.0f;
            auto box = Rectangle<float> (w, 20.0f).withCentre (pos.translated (0, -24.0f)).constrainedWithin (plot);
            g.setColour (Colours_::panelTop.withAlpha (0.95f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (col.withAlpha (0.5f));
            g.drawRoundedRectangle (box, 4.0f, 1.0f);
            g.setColour (col);
            g.drawText (text, box, Justification::centred);
        }
    }
}

void EQDisplay::drawHoverReadout (Graphics& g)
{
    if (! mouseInside || dragging >= 0 || hover >= 0 || ! plot.contains (mousePos)) return;
    const float f = xToFreq (mousePos.x);
    const float note = frequencyToMidi (f);
    const int n = roundToInt (note);
    const int cents = roundToInt ((note - (float) n) * 100.0f);
    const String text = (f < 1000 ? String (f, 1) + " Hz" : String (f / 1000.0f, 2) + " kHz") + "   "
                      + noteNameFor (n) + String (octaveFor (n)) + (cents == 0 ? String() : String (cents > 0 ? " +" : " ") + String (cents) + "c");

    g.setColour (Colours_::textDim.withAlpha (0.35f));
    g.drawVerticalLine ((int) mousePos.x, plot.getY(), plot.getBottom());
    g.setFont (Fonts::medium (12.0f));
    auto box = Rectangle<float> (130.0f, 18.0f).withCentre ({ mousePos.x, plot.getBottom() - 14.0f }).constrainedWithin (plot);
    g.setColour (Colours_::panelTop.withAlpha (0.92f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (Colours_::text);
    g.drawText (text, box, Justification::centred);
}

//==============================================================================
void EQDisplay::beginDrag (int b)
{
    dragging = b;
    for (auto* sfx : { "Freq", "Gain", "Q" })
        if (auto* prm = param (b, sfx)) prm->beginChangeGesture();
}

void EQDisplay::endDrag()
{
    if (dragging >= 0)
        for (auto* sfx : { "Freq", "Gain", "Q" })
            if (auto* prm = param (dragging, sfx)) prm->endChangeGesture();
    dragging = -1;
    qDrag = false;
}

void EQDisplay::mouseDown (const MouseEvent& e)
{
    const int b = hitTestNode (e.position);
    if (b < 0)
        return;

    selected = b;
    if (onBandSelected) onBandSelected (b);

    if (e.mods.isPopupMenu()) { showBandMenu (b); return; }

    // Alt-click: bypass / enable the band (Pro-Q behaviour)
    if (e.mods.isAltDown())
    {
        setBandOn (apvts, eqIdx, b, ! readBand (b).on);
        repaint();
        return;
    }

    if (! readBand (b).on)
        setBandOn (apvts, eqIdx, b, true);   // grabbing a bypassed node switches it on

    beginDrag (b);
    qDrag = e.mods.isCommandDown();
    lastDragPos = e.position;
    repaint();
}

void EQDisplay::mouseDrag (const MouseEvent& e)
{
    if (dragging < 0)
        return;

    const auto s = readBand (dragging, false);
    const float speed = e.mods.isShiftDown() ? 0.1f : 1.0f;

    if (qDrag)   // Ctrl/Cmd + vertical drag: Q
    {
        const float dy = (e.position.y - lastDragPos.y) * speed;
        setParam (dragging, "Q", jlimit (0.1f, 18.0f, s.q * std::exp (-dy * 0.02f)));
        lastDragPos = e.position;
        return;
    }

    Point<float> target = e.position;
    if (e.mods.isShiftDown())
        target = nodePosition (dragging) + (e.position - lastDragPos) * 0.1f;   // fine mode
    lastDragPos = e.position;

    target.x = jlimit (plot.getX(), plot.getRight(), target.x);
    target.y = jlimit (plot.getY(), plot.getBottom(), target.y);

    setParam (dragging, "Freq", jlimit (minFreq, maxFreq, xToFreq (target.x)));
    if (eq::typeUsesGain (s.type))
    {
        const float scale = gainScale();
        const float shown = yToGain (target.y);
        setParam (dragging, "Gain", jlimit (-maxDb, maxDb, scale > 0.05f ? shown / scale : shown));
    }
}

void EQDisplay::mouseUp (const MouseEvent&) { endDrag(); }

void EQDisplay::mouseMove (const MouseEvent& e)
{
    mouseInside = true;
    mousePos = e.position;
    if (! hasKeyboardFocus (false) && isShowing()) grabKeyboardFocus();
    const int h = hitTestNode (e.position);
    if (h != hover) hover = h;
    setMouseCursor (h >= 0 ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
    repaint();
}

void EQDisplay::mouseExit (const MouseEvent&) { hover = -1; mouseInside = false; repaint(); }

void EQDisplay::mouseDoubleClick (const MouseEvent& e)
{
    endDrag();
    const int b = hitTestNode (e.position);

    if (b >= 0)
    {
        setBandOn (apvts, eqIdx, b, ! readBand (b).on);
        return;
    }
    if (! plot.contains (e.position))
        return;

    // New band in the first free slot; the shape depends on where you click
    const float relX = (e.position.x - plot.getX()) / plot.getWidth();
    const float relY = (e.position.y - plot.getY()) / plot.getHeight();
    eq::BandType t = eq::BandType::Bell;
    if (relX < 0.06f)      t = eq::BandType::LowCut;
    else if (relX > 0.94f) t = eq::BandType::HighCut;
    else if (relY > 0.88f) t = eq::BandType::Notch;

    const float gain = t == eq::BandType::Bell ? jlimit (-maxDb, maxDb, yToGain (e.position.y) / jmax (0.05f, gainScale())) : 0.0f;
    const int i = addBand (apvts, eqIdx, t, jlimit (minFreq, maxFreq, xToFreq (e.position.x)), gain, 1.0f);
    if (i >= 0)
    {
        selected = i;
        if (onBandSelected) onBandSelected (i);
    }
}

bool EQDisplay::keyPressed (const KeyPress& k)
{
    if ((k == KeyPress::deleteKey || k == KeyPress::backspaceKey) && present (selected))
    {
        if (auto* um = apvts.undoManager) um->beginNewTransaction ("Delete EQ band");
        deleteBand (apvts, eqIdx, selected);
        return true;
    }
    return false;
}

void EQDisplay::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    int b = hitTestNode (e.position);
    if (b < 0) b = dragging;
    if (b < 0) return;

    const auto s = readBand (b, false);
    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;

    if (e.mods.isCommandDown())   // Ctrl/Cmd + wheel: gain
    {
        if (eq::typeUsesGain (s.type))
            setParamGesture (b, "Gain", jlimit (-maxDb, maxDb, s.gainDb + w.deltaY * 6.0f * fine));
        return;
    }
    if (eq::typeUsesSlope (s.type))   // cut / band-pass filters: step the slope
    {
        const int step = w.deltaY > 0 ? 1 : (w.deltaY < 0 ? -1 : 0);
        setParamGesture (b, "Slope", (float) jlimit (0, eq::numSlopes - 1, s.slope + step));
        return;
    }
    setParamGesture (b, "Q", jlimit (0.1f, 18.0f, s.q * std::exp (w.deltaY * 1.5f * fine)));
}

//==============================================================================
bool EQDisplay::isPresent (AudioProcessorValueTreeState& s, int eqIdx, int b)
{
    if (b < 0 || b >= Params::numBands) return false;
    return s.getRawParameterValue (ID::eqBand (eqIdx, b, "Used"))->load() > 0.5f
        || s.getRawParameterValue (ID::eqBand (eqIdx, b, "On"))->load() > 0.5f;
}

int EQDisplay::numPresent (AudioProcessorValueTreeState& s, int eqIdx)
{
    int n = 0;
    for (int b = 0; b < Params::numBands; ++b) n += isPresent (s, eqIdx, b) ? 1 : 0;
    return n;
}

int EQDisplay::firstFreeBand (AudioProcessorValueTreeState& s, int eqIdx, int exclude)
{
    for (int b = 0; b < Params::numBands; ++b)
        if (b != exclude && ! isPresent (s, eqIdx, b)) return b;
    return -1;
}

static void setGesture (AudioProcessorValueTreeState& s, int eqIdx, int b, const char* sfx, float value)
{
    if (auto* p = s.getParameter (ID::eqBand (eqIdx, b, sfx)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

void EQDisplay::setBandOn (AudioProcessorValueTreeState& s, int eqIdx, int b, bool on)
{
    setGesture (s, eqIdx, b, "Used", 1.0f);
    setGesture (s, eqIdx, b, "On", on ? 1.0f : 0.0f);
}

int EQDisplay::addBand (AudioProcessorValueTreeState& s, int eqIdx, eq::BandType t, float freq, float gain, float q)
{
    const int i = firstFreeBand (s, eqIdx);
    if (i < 0) return -1;
    if (auto* um = s.undoManager) um->beginNewTransaction ("Add EQ band");
    resetBand (s, eqIdx, i);
    setGesture (s, eqIdx, i, "Type", (float) t);
    setGesture (s, eqIdx, i, "Freq", freq);
    if (eq::typeUsesGain (t)) setGesture (s, eqIdx, i, "Gain", gain);
    if (! eq::typeUsesSlope (t)) setGesture (s, eqIdx, i, "Q", q);
    setBandOn (s, eqIdx, i, true);
    return i;
}

void EQDisplay::resetBand (AudioProcessorValueTreeState& s, int eqIdx, int b)
{
    for (auto* sfx : { "Used", "On", "Type", "Freq", "Gain", "Q", "Slope", "Place" })
        if (auto* p = s.getParameter (ID::eqBand (eqIdx, b, sfx)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
}

void EQDisplay::splitBand (AudioProcessorValueTreeState& s, int eqIdx, int from, bool midSide)
{
    const int target = firstFreeBand (s, eqIdx, from);
    if (target < 0) return;

    if (auto* um = s.undoManager) um->beginNewTransaction ("Split EQ band");
    for (auto* sfx : { "Type", "Freq", "Gain", "Q", "Slope", "Used", "On" })
        if (auto* p = s.getParameter (ID::eqBand (eqIdx, target, sfx)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (s.getParameter (ID::eqBand (eqIdx, from, sfx))->getValue());
            p->endChangeGesture();
        }
    auto place = [&s, eqIdx] (int b, eq::Placement pl)
    {
        if (auto* p = s.getParameter (ID::eqBand (eqIdx, b, "Place")))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 ((float) pl));
            p->endChangeGesture();
        }
    };
    place (from, midSide ? eq::Placement::Mid : eq::Placement::Left);
    place (target, midSide ? eq::Placement::Side : eq::Placement::Right);
}

void EQDisplay::showBandMenu (int b)
{
    const auto s = readBand (b, false);
    PopupMenu menu, shapes, slopes, placement;
    for (int t = 0; t < eq::numBandTypes; ++t)
        shapes.addItem (1 + t, eq::bandTypeNames()[t], true, (int) s.type == t);
    for (int sl = 0; sl < eq::numSlopes; ++sl)
        slopes.addItem (20 + sl, String (eq::slopeDb[sl]) + " dB/oct", true, s.slope == sl);
    for (int p = 0; p < 5; ++p)
        placement.addItem (40 + p, placementName ((eq::Placement) p), true, (int) s.placement == p);

    const bool freeBand = firstFreeBand (apvts, eqIdx, b) >= 0;

    menu.addSectionHeader ("Band " + String (b + 1));
    menu.addSubMenu ("Shape", shapes);
    menu.addSubMenu ("Slope", slopes, eq::typeUsesSlope (s.type));
    menu.addSubMenu ("Stereo placement", placement);
    menu.addItem (50, "Split into Left + Right", freeBand);
    menu.addItem (51, "Split into Mid + Side", freeBand);
    menu.addItem (52, "Reset placement", s.placement != eq::Placement::Stereo);
    menu.addSeparator();
    menu.addItem (30, s.on ? "Bypass band" : "Enable band");
    menu.addItem (31, "Reset gain", eq::typeUsesGain (s.type));
    menu.addItem (32, "Reset band settings");
    menu.addItem (33, "Delete band");

    SafePointer<EQDisplay> safe (this);
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe, b] (int result)
    {
        if (safe == nullptr || result == 0) return;
        auto& d = *safe;
        if (result >= 1 && result <= eq::numBandTypes) { d.setParamGesture (b, "Type", (float) (result - 1)); setBandOn (d.apvts, d.eqIdx, b, true); }
        else if (result >= 20 && result < 20 + eq::numSlopes) d.setParamGesture (b, "Slope", (float) (result - 20));
        else if (result >= 40 && result <= 44) d.setParamGesture (b, "Place", (float) (result - 40));
        else if (result == 50 || result == 51) splitBand (d.apvts, d.eqIdx, b, result == 51);
        else if (result == 52) d.setParamGesture (b, "Place", 0.0f);
        else if (result == 30) setBandOn (d.apvts, d.eqIdx, b, ! d.readBand (b).on);
        else if (result == 31) d.setParamGesture (b, "Gain", 0.0f);
        else if (result == 32)
        {
            // keep the band where it is, just reset its settings
            const float f = d.readBand (b, false).freq;
            resetBand (d.apvts, d.eqIdx, b);
            d.setParamGesture (b, "Type", (float) eq::BandType::Bell);
            d.setParamGesture (b, "Freq", f);
            setBandOn (d.apvts, d.eqIdx, b, true);
        }
        else if (result == 33)
        {
            if (auto* um = d.apvts.undoManager) um->beginNewTransaction ("Delete EQ band");
            deleteBand (d.apvts, d.eqIdx, b);
        }
    });
}

//==============================================================================
BandControls::BandControls (AudioProcessorValueTreeState& s, int eqIndex) : eqIdx (eqIndex), apvts (s)
{
    prevButton.setTooltip ("Previous band");
    nextButton.setTooltip ("Next band");
    prevButton.onClick = [this] { step (-1); };
    nextButton.onClick = [this] { step (1); };
    for (auto* b : { &addButton, &deleteButton, &resetButton })
        b->setComponentID ("pill");
    addButton.setTooltip ("Add band: creates a new bell band (up to 24). Tip: double-click the display to add a band at the mouse position.");
    addButton.onClick = [this]
    {
        // Place the new band an octave above the selected one, or at 1 kHz
        float f = 1000.0f;
        if (EQDisplay::isPresent (apvts, eqIdx, band))
            f = jlimit (20.0f, 20000.0f, apvts.getRawParameterValue (ID::eqBand (eqIdx, band, "Freq"))->load() * 2.0f);
        const int b = EQDisplay::addBand (apvts, eqIdx, eq::BandType::Bell, f);
        if (b >= 0) select (b);
    };
    deleteButton.setTooltip ("Delete band: removes the selected band (Delete / Backspace on the display)");
    deleteButton.onClick = [this]
    {
        if (auto* um = apvts.undoManager) um->beginNewTransaction ("Delete EQ band");
        EQDisplay::deleteBand (apvts, eqIdx, band);
        step (1);
    };

    onButton.setTooltip ("Band bypass: switch the selected band off without deleting it (Alt-click a node on the display)");
    onButton.onClick = [this] { EQDisplay::setBandOn (apvts, eqIdx, band, onButton.getToggleState()); };
    resetButton.setTooltip ("Reset band: return the selected band to a flat bell, keeping its frequency");
    resetButton.onClick = [this]
    {
        const float f = apvts.getRawParameterValue (ID::eqBand (eqIdx, band, "Freq"))->load();
        if (auto* um = apvts.undoManager) um->beginNewTransaction ("Reset EQ band");
        EQDisplay::resetBand (apvts, eqIdx, band);
        if (auto* p = apvts.getParameter (ID::eqBand (eqIdx, band, "Freq"))) p->setValueNotifyingHost (p->convertTo0to1 (f));
        if (auto* p = apvts.getParameter (ID::eqBand (eqIdx, band, "Type"))) p->setValueNotifyingHost (p->convertTo0to1 ((float) eq::BandType::Bell));
        EQDisplay::setBandOn (apvts, eqIdx, band, true);
    };
    gainScale.attach (apvts, ID::eqScaleFor (eqIdx));
    gainScale.slider.setTooltip ("Gain Scale: scales the gain of all Bell, Shelf and Tilt bands at once (100 % = as set, 0 % = flat, 200 % = double)");

    for (Component* c : std::initializer_list<Component*> { &prevButton, &nextButton, &addButton, &deleteButton, &onButton, &resetButton,
                                                            &type, &slope, &placement, &freq, &gain, &q, &gainScale })
        addAndMakeVisible (c);

    setBand (0);
    startTimerHz (10);
}

void BandControls::step (int direction)
{
    for (int i = 1; i <= Params::numBands; ++i)
    {
        const int b = ((band + direction * i) % Params::numBands + Params::numBands) % Params::numBands;
        if (EQDisplay::isPresent (apvts, eqIdx, b)) { select (b); return; }
    }
}

void BandControls::setBand (int b)
{
    if (b == band) return;
    band = b;

    // The power button is driven manually (see timerCallback) so bypassing keeps the band present
    onButton.setClickingTogglesState (true);
    type.attach (apvts, ID::eqBand (eqIdx, b, "Type"));
    slope.attach (apvts, ID::eqBand (eqIdx, b, "Slope"));
    placement.attach (apvts, ID::eqBand (eqIdx, b, "Place"));
    freq.attach (apvts, ID::eqBand (eqIdx, b, "Freq"));
    gain.attach (apvts, ID::eqBand (eqIdx, b, "Gain"));
    q.attach (apvts, ID::eqBand (eqIdx, b, "Q"));

    const auto col = Colours_::band (b);
    for (auto* k : { &freq, &gain, &q })
        k->label.setColour (Label::textColourId, col);
    timerCallback();
    repaint();
}

void BandControls::timerCallback()
{
    const bool isOn = apvts.getRawParameterValue (ID::eqBand (eqIdx, band, "On"))->load() > 0.5f;
    if (onButton.getToggleState() != isOn) onButton.setToggleState (isOn, dontSendNotification);

    const int count = EQDisplay::numPresent (apvts, eqIdx);
    const bool has = EQDisplay::isPresent (apvts, eqIdx, band);
    addButton.setEnabled (count < Params::numBands);
    prevButton.setEnabled (count > 1 || (count == 1 && ! has));
    nextButton.setEnabled (prevButton.isEnabled());
    if (! has && count > 0) step (1);   // the selected band was deleted elsewhere

    // Grey out the controls that do not apply to the selected shape (or everything without a band)
    const auto t = (eq::BandType) (int) apvts.getRawParameterValue (ID::eqBand (eqIdx, band, "Type"))->load();
    const int sl = (int) apvts.getRawParameterValue (ID::eqBand (eqIdx, band, "Slope"))->load();
    const bool usesSlope = eq::typeUsesSlope (t), usesGain = eq::typeUsesGain (t);
    const bool usesQ = ! (usesSlope && sl == 0) && t != eq::BandType::FlatTilt;
    const bool present = EQDisplay::isPresent (apvts, eqIdx, band);
    auto enable = [] (Component& c, bool on) { if (c.isEnabled() != on) { c.setEnabled (on); c.setAlpha (on ? 1.0f : 0.35f); } };
    enable (onButton, present);
    enable (deleteButton, present);
    enable (resetButton, present);
    enable (type, present);
    enable (placement, present);
    enable (freq, present);
    enable (slope, present && usesSlope);
    enable (gain, present && usesGain);
    enable (q, present && usesQ);
    if (present != hasBand) { hasBand = present; repaint(); }
    if (count != lastCount) { lastCount = count; repaint(); }
}

void BandControls::paint (Graphics& g)
{
    g.setColour (Colours_::textDim);
    g.setFont (Fonts::label (11.5f, 0.14f));
    g.drawText ("BAND", Rectangle<int> (1, 0, 200, 16), Justification::centredLeft);
    g.drawText (String (EQDisplay::numPresent (apvts, eqIdx)) + " / " + String (Params::numBands),
                Rectangle<int> (1, 0, 200, 16), Justification::centredRight);
    g.drawText ("GLOBAL", toolsArea.withHeight (16), Justification::centredLeft);

    // Current band: colour chip + number, or a hint when the EQ is empty
    auto r = bandLabelArea.toFloat();
    if (hasBand)
    {
        const auto col = Colours_::band (band);
        auto chip = r.withSizeKeepingCentre (r.getWidth(), 24.0f);
        g.setColour (col.withAlpha (0.16f));
        g.fillRoundedRectangle (chip, 5.0f);
        g.setColour (col);
        g.drawRoundedRectangle (chip, 5.0f, 1.0f);
        g.setFont (Fonts::semibold (14.0f));
        g.drawText (String (band + 1), chip, Justification::centred);
    }
    else
    {
        g.setColour (Colours_::textFaint);
        g.setFont (Fonts::regular (11.5f));
        g.drawFittedText ("Double-click the\ndisplay to add", bandLabelArea, Justification::centred, 2);
    }

    g.setColour (Colours_::outline);
    g.drawVerticalLine (toolsArea.getX() - 18, 6.0f, (float) getHeight() - 6.0f);
}

void BandControls::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (200);
    left.removeFromTop (18);
    auto nav = left.removeFromTop (28);
    prevButton.setBounds (nav.removeFromLeft (26));
    addButton.setBounds (nav.removeFromRight (64).reduced (0, 1));
    nav.removeFromRight (6);
    nextButton.setBounds (nav.removeFromRight (26));
    bandLabelArea = nav.reduced (4, 0);
    left.removeFromTop (8);
    auto row = left.removeFromTop (26);
    onButton.setBounds (row.removeFromLeft (26));
    row.removeFromLeft (6);
    resetButton.setBounds (row.removeFromLeft (78).reduced (0, 1));
    deleteButton.setBounds (row.removeFromRight (78).reduced (0, 1));

    toolsArea = r.removeFromRight (170);
    gainScale.setBounds (toolsArea.withTrimmedTop (22).withHeight (44));
    r.removeFromRight (30);

    r.removeFromLeft (10);
    type.setBounds (r.removeFromLeft (130).withTrimmedTop (8).withHeight (48));
    r.removeFromLeft (10);
    slope.setBounds (r.removeFromLeft (116).withTrimmedTop (8).withHeight (48));
    r.removeFromLeft (10);
    placement.setBounds (r.removeFromLeft (108).withTrimmedTop (8).withHeight (48));
    r.removeFromLeft (10);
    const int kw = jmin (100, r.getWidth() / 3);
    for (auto* k : { &freq, &gain, &q })
        k->setBounds (r.removeFromLeft (kw));
}
