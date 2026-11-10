#include "EQDisplay.h"
#include "../PluginProcessor.h"

using namespace juce;

//==============================================================================
EQDisplay::EQDisplay (HardKickProcessor& p) : proc (p), apvts (p.apvts)
{
    ring.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    spectrumDb.assign ((size_t) fftSize / 2, -120.0f);
    pullBuffer.assign (32768, 0.0f);
    startTimerHz (30);
}

void EQDisplay::resized()
{
    plot = getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (16.0f).withTrimmedRight (30.0f);
}

float EQDisplay::freqToX (float f) const
{
    return plot.getX() + plot.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
}

float EQDisplay::xToFreq (float x) const
{
    return minFreq * std::pow (maxFreq / minFreq, (x - plot.getX()) / plot.getWidth());
}

float EQDisplay::gainToY (float db) const { return jmap (db, -maxDb, maxDb, plot.getBottom(), plot.getY()); }
float EQDisplay::yToGain (float y) const  { return jmap (y, plot.getBottom(), plot.getY(), -maxDb, maxDb); }
float EQDisplay::analyzerDbToY (float db) const { return jmap (db, -90.0f, 6.0f, plot.getBottom(), plot.getY()); }

bool EQDisplay::typeUsesGain (eq::BandType t) const
{
    return t == eq::BandType::Bell || t == eq::BandType::LowShelf || t == eq::BandType::HighShelf;
}

RangedAudioParameter* EQDisplay::param (int b, const char* suffix) const
{
    return apvts.getParameter (ID::band (b, suffix));
}

void EQDisplay::setParam (int b, const char* suffix, float value)
{
    if (auto* prm = param (b, suffix))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

eq::BandSettings EQDisplay::readBand (int b) const
{
    eq::BandSettings s;
    s.on     = apvts.getRawParameterValue (ID::band (b, "On"))->load() > 0.5f;
    s.type   = (eq::BandType) (int) apvts.getRawParameterValue (ID::band (b, "Type"))->load();
    s.freq   = apvts.getRawParameterValue (ID::band (b, "Freq"))->load();
    s.gainDb = apvts.getRawParameterValue (ID::band (b, "Gain"))->load();
    s.q      = apvts.getRawParameterValue (ID::band (b, "Q"))->load();
    s.slope  = (int) apvts.getRawParameterValue (ID::band (b, "Slope"))->load();
    return s;
}

Point<float> EQDisplay::nodePosition (int b) const
{
    const auto s = readBand (b);
    return { freqToX (s.freq), typeUsesGain (s.type) ? gainToY (s.gainDb) : gainToY (0.0f) };
}

int EQDisplay::hitTestNode (Point<float> pt) const
{
    int best = -1;
    float bestDist = 12.0f;
    for (int b = 0; b < Params::numBands; ++b)
    {
        const float d = nodePosition (b).getDistanceFrom (pt);
        // Prefer enabled bands when nodes overlap
        const float bias = readBand (b).on ? 0.0f : 3.0f;
        if (d + bias < bestDist) { bestDist = d + bias; best = b; }
    }
    return best;
}

//==============================================================================
void EQDisplay::timerCallback()
{
    // Keep the most recent fftSize samples in the ring buffer
    const int got = proc.analyzer.pull (pullBuffer.data(), (int) pullBuffer.size());
    if (got >= fftSize)
    {
        std::copy (pullBuffer.begin() + (got - fftSize), pullBuffer.begin() + got, ring.begin());
    }
    else if (got > 0)
    {
        std::move (ring.begin() + got, ring.end(), ring.begin());
        std::copy (pullBuffer.begin(), pullBuffer.begin() + got, ring.end() - got);
    }

    std::fill (fftData.begin(), fftData.end(), 0.0f);
    std::copy (ring.begin(), ring.end(), fftData.begin());
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float norm = 4.0f / (float) fftSize; // Hann window coherent gain compensation
    for (size_t i = 0; i < spectrumDb.size(); ++i)
    {
        const float db = Decibels::gainToDecibels (fftData[i] * norm, -120.0f);
        // fast attack, slow release
        spectrumDb[i] = db > spectrumDb[i] ? db : jmax (db, spectrumDb[i] - 2.0f);
    }

    repaint();
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
}

void EQDisplay::drawGrid (Graphics& g)
{
    g.setFont (FontOptions (10.0f));

    const float freqs[] = { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 800, 1000, 2000, 3000, 4000, 5000, 8000, 10000, 20000 };
    for (float f : freqs)
    {
        const float x = freqToX (f);
        const bool major = f == 50 || f == 100 || f == 500 || f == 1000 || f == 5000 || f == 10000;
        g.setColour (major ? Colours_::grid.brighter (0.35f) : Colours_::grid);
        g.drawVerticalLine (roundToInt (x), plot.getY(), plot.getBottom());

        if (major || f == 20 || f == 20000)
        {
            const String label = f >= 1000 ? String ((int) (f / 1000)) + "k" : String ((int) f);
            g.setColour (Colours_::textDim);
            const float lx = jlimit (plot.getX(), plot.getRight() - 40.0f, x - 20.0f);
            g.drawText (label, Rectangle<float> (lx, plot.getBottom() + 1, 40, 14),
                        f <= 20 ? Justification::centredLeft : Justification::centred);
        }
    }

    for (float db = -18.0f; db <= 18.0f; db += 6.0f)
    {
        const float y = gainToY (db);
        g.setColour (juce::exactlyEqual (db, 0.0f) ? Colours_::grid.brighter (0.7f) : Colours_::grid);
        g.drawHorizontalLine (roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (Colours_::textDim);
        g.drawText ((db > 0 ? "+" : "") + String ((int) db), Rectangle<float> (plot.getRight() + 2, y - 7, 28, 14),
                    Justification::centredLeft);
    }
}

void EQDisplay::drawAnalyzer (Graphics& g)
{
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
    const float binHz = (float) (sr / fftSize);
    const int numBins = (int) spectrumDb.size();

    Path path;
    bool started = false;
    const int x0 = (int) plot.getX(), x1 = (int) plot.getRight();

    for (int x = x0; x <= x1; x += 2)
    {
        const float fLo = xToFreq ((float) x - 1.0f), fHi = xToFreq ((float) x + 1.0f);
        const float binLo = fLo / binHz, binHi = fHi / binHz;
        float db;

        if (binHi - binLo < 1.0f)
        {
            // Sparse bins at low frequencies: interpolate
            const float pos = jlimit (0.0f, (float) numBins - 2.0f, xToFreq ((float) x) / binHz);
            const int i = (int) pos;
            db = jmap (pos - (float) i, spectrumDb[(size_t) i], spectrumDb[(size_t) i + 1]);
        }
        else
        {
            db = -120.0f;
            for (int i = jmax (0, (int) binLo); i <= jmin (numBins - 1, (int) binHi); ++i)
                db = jmax (db, spectrumDb[(size_t) i]);
        }

        const float y = jlimit (plot.getY(), plot.getBottom(), analyzerDbToY (db));
        if (! started) { path.startNewSubPath ((float) x, plot.getBottom()); path.lineTo ((float) x, y); started = true; }
        else path.lineTo ((float) x, y);
    }

    path.lineTo ((float) x1, plot.getBottom());
    path.closeSubPath();

    g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (0.30f), 0, plot.getY(),
                                       Colours_::accent2.withAlpha (0.04f), 0, plot.getBottom(), false));
    g.fillPath (path);
    g.setColour (Colours_::accent.withAlpha (0.45f));
    g.strokePath (path, PathStrokeType (1.0f));
}

void EQDisplay::drawCurves (Graphics& g)
{
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;

    std::array<eq::BandDesign, Params::numBands> designs;
    for (int b = 0; b < Params::numBands; ++b)
        designs[(size_t) b] = eq::design (readBand (b), sr);

    const int x0 = (int) plot.getX(), x1 = (int) plot.getRight();
    Path total, sel;
    const bool selOn = readBand (selected).on;

    for (int x = x0; x <= x1; ++x)
    {
        const double f = xToFreq ((float) x);
        double sumDb = 0.0;
        for (int b = 0; b < Params::numBands; ++b)
        {
            if (designs[(size_t) b].numStages == 0) continue;
            const double db = 20.0 * std::log10 (jmax (1.0e-6, eq::magnitude (designs[(size_t) b], f, sr)));
            sumDb += db;
            if (b == selected)
            {
                const float y = jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, gainToY ((float) db));
                if (x == x0) sel.startNewSubPath ((float) x, y); else sel.lineTo ((float) x, y);
            }
        }
        const float y = jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, gainToY ((float) sumDb));
        if (x == x0) total.startNewSubPath ((float) x, y); else total.lineTo ((float) x, y);
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

    Colours_::strokeGlow (g, total, Colour (0xffd6f0ff), 2.0f);
}

void EQDisplay::drawNodes (Graphics& g)
{
    g.setFont (FontOptions (10.0f, Font::bold));

    for (int b = 0; b < Params::numBands; ++b)
    {
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

        if (b == hover || b == dragging)
        {
            const String typeNames[] = { "Low Cut", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut" };
            String text = typeNames[(int) s.type] + "  "
                        + (s.freq < 1000 ? String (s.freq, 1) + " Hz" : String (s.freq / 1000.0f, 2) + " kHz");
            if (typeUsesGain (s.type)) text << "  " << (s.gainDb > 0 ? "+" : "") << String (s.gainDb, 1) << " dB";
            text << "  Q " << String (s.q, 2);
            if (! s.on) text << "  (off)";

            const float w = 230.0f;
            auto box = Rectangle<float> (w, 18.0f).withCentre (pos.translated (0, -22.0f));
            box = box.constrainedWithin (plot);
            g.setColour (Colours_::panelTop.withAlpha (0.95f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (col.withAlpha (0.5f));
            g.drawRoundedRectangle (box, 4.0f, 1.0f);
            g.setColour (col);
            g.drawText (text, box, Justification::centred);
        }
    }
}

//==============================================================================
void EQDisplay::beginDrag (int b)
{
    dragging = b;
    for (auto* sfx : { "Freq", "Gain" })
        if (auto* prm = param (b, sfx)) prm->beginChangeGesture();
}

void EQDisplay::endDrag()
{
    if (dragging >= 0)
        for (auto* sfx : { "Freq", "Gain" })
            if (auto* prm = param (dragging, sfx)) prm->endChangeGesture();
    dragging = -1;
}

void EQDisplay::mouseDown (const MouseEvent& e)
{
    const int b = hitTestNode (e.position);
    if (b < 0)
        return;

    selected = b;
    if (onBandSelected) onBandSelected (b);

    if (e.mods.isPopupMenu())
    {
        showBandMenu (b);
        return;
    }

    if (! readBand (b).on)
    {
        // Grabbing a disabled node switches it on
        if (auto* prm = param (b, "On")) { prm->beginChangeGesture(); prm->setValueNotifyingHost (1.0f); prm->endChangeGesture(); }
    }

    beginDrag (b);
    lastDragPos = e.position;
    repaint();
}

void EQDisplay::mouseDrag (const MouseEvent& e)
{
    if (dragging < 0)
        return;

    const auto s = readBand (dragging);
    Point<float> target = e.position;

    if (e.mods.isShiftDown())
    {
        // Fine mode: move at 1/10 speed relative to the node's current position
        const auto delta = (e.position - lastDragPos) * 0.1f;
        target = nodePosition (dragging) + delta;
    }
    lastDragPos = e.position;

    target.x = jlimit (plot.getX(), plot.getRight(), target.x);
    target.y = jlimit (plot.getY(), plot.getBottom(), target.y);

    setParam (dragging, "Freq", jlimit (minFreq, maxFreq, xToFreq (target.x)));
    if (typeUsesGain (s.type))
        setParam (dragging, "Gain", jlimit (-maxDb, maxDb, yToGain (target.y)));
}

void EQDisplay::mouseUp (const MouseEvent&) { endDrag(); }

void EQDisplay::mouseMove (const MouseEvent& e)
{
    const int h = hitTestNode (e.position);
    if (h != hover) { hover = h; repaint(); }
    setMouseCursor (h >= 0 ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
}

void EQDisplay::mouseExit (const MouseEvent&) { hover = -1; repaint(); }

void EQDisplay::mouseDoubleClick (const MouseEvent& e)
{
    endDrag();
    const int b = hitTestNode (e.position);

    if (b >= 0)
    {
        if (auto* prm = param (b, "On"))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (readBand (b).on ? 0.0f : 1.0f);
            prm->endChangeGesture();
        }
        return;
    }

    if (! plot.contains (e.position))
        return;

    // Activate the first free band as a bell at the clicked position
    for (int i = 0; i < Params::numBands; ++i)
    {
        if (readBand (i).on) continue;

        for (auto* sfx : { "On", "Type", "Freq", "Gain", "Q" })
            if (auto* prm = param (i, sfx)) prm->beginChangeGesture();

        setParam (i, "Type", (float) eq::BandType::Bell);
        setParam (i, "Freq", jlimit (minFreq, maxFreq, xToFreq (e.position.x)));
        setParam (i, "Gain", jlimit (-maxDb, maxDb, yToGain (e.position.y)));
        setParam (i, "Q", 1.0f);
        setParam (i, "On", 1.0f);

        for (auto* sfx : { "On", "Type", "Freq", "Gain", "Q" })
            if (auto* prm = param (i, sfx)) prm->endChangeGesture();

        selected = i;
        if (onBandSelected) onBandSelected (i);
        break;
    }
}

void EQDisplay::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    int b = hitTestNode (e.position);
    if (b < 0) b = dragging >= 0 ? dragging : -1;
    if (b < 0) return;

    if (auto* prm = param (b, "Q"))
    {
        const float q = readBand (b).q;
        const float newQ = jlimit (0.1f, 18.0f, q * std::exp (w.deltaY * 1.5f));
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (newQ));
        prm->endChangeGesture();
    }
}

void EQDisplay::showBandMenu (int b)
{
    const auto s = readBand (b);
    PopupMenu menu, slopes;
    const String typeNames[] = { "Low Cut", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut" };
    for (int t = 0; t < 6; ++t)
        menu.addItem (1 + t, typeNames[t], true, (int) s.type == t);

    const String slopeNames[] = { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    for (int sl = 0; sl < 3; ++sl)
        slopes.addItem (20 + sl, slopeNames[sl], true, s.slope == sl);
    menu.addSubMenu ("Slope", slopes, s.type == eq::BandType::LowCut || s.type == eq::BandType::HighCut);

    menu.addSeparator();
    menu.addItem (30, s.on ? "Disable band" : "Enable band");
    menu.addItem (31, "Reset gain");

    SafePointer<EQDisplay> safe (this);
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition(),
        [safe, b] (int result)
        {
            if (safe == nullptr || result == 0) return;
            auto set = [&] (const char* sfx, float v)
            {
                if (auto* prm = safe->param (b, sfx))
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
                    prm->endChangeGesture();
                }
            };
            if (result >= 1 && result <= 6) { set ("Type", (float) (result - 1)); set ("On", 1.0f); }
            else if (result >= 20 && result <= 22) set ("Slope", (float) (result - 20));
            else if (result == 30) set ("On", safe->readBand (b).on ? 0.0f : 1.0f);
            else if (result == 31) set ("Gain", 0.0f);
        });
}

//==============================================================================
BandControls::BandControls (AudioProcessorValueTreeState& s) : apvts (s)
{
    for (int b = 0; b < 6; ++b)
    {
        auto& btn = bandButtons[b];
        btn.setButtonText (String (b + 1));
        btn.setClickingTogglesState (false);
        btn.setColour (TextButton::buttonOnColourId, Colours_::band (b));
        btn.onClick = [this, b]
        {
            setBand (b);
            if (onBandSelected) onBandSelected (b);
        };
        addAndMakeVisible (btn);
    }

    addAndMakeVisible (onButton);
    for (Component* c : { (Component*) &type, (Component*) &slope, (Component*) &freq, (Component*) &gain, (Component*) &q })
        addAndMakeVisible (c);

    setBand (0);
}

void BandControls::setBand (int b)
{
    if (b == band) return;
    band = b;

    for (int i = 0; i < 6; ++i)
        bandButtons[i].setToggleState (i == b, dontSendNotification);

    onAttachment.reset();
    onAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::band (b, "On"), onButton);
    type.attach (apvts, ID::band (b, "Type"));
    slope.attach (apvts, ID::band (b, "Slope"));
    freq.attach (apvts, ID::band (b, "Freq"));
    gain.attach (apvts, ID::band (b, "Gain"));
    q.attach (apvts, ID::band (b, "Q"));

    const auto col = Colours_::band (b);
    for (auto* k : { &freq, &gain, &q })
        k->label.setColour (Label::textColourId, col);
    repaint();
}

void BandControls::paint (Graphics& g)
{
    g.setColour (Colours_::textDim);
    g.setFont (FontOptions (11.0f));
    g.setFont (FontOptions (10.0f, Font::bold).withKerningFactor (0.08f));
    g.drawText ("BAND", Rectangle<int> (0, 0, 200, 14), Justification::centredLeft);
    g.drawText ("ACTIVE", Rectangle<int> (30, 48, 100, 24), Justification::centredLeft);
}

void BandControls::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (200);
    left.removeFromTop (16);
    auto buttons = left.removeFromTop (26);
    for (auto& btn : bandButtons)
        btn.setBounds (buttons.removeFromLeft (30).reduced (2, 0));
    left.removeFromTop (6);
    onButton.setBounds (left.removeFromTop (24).removeFromLeft (24));

    r.removeFromLeft (10);
    type.setBounds (r.removeFromLeft (130).withTrimmedTop (10).withHeight (44));
    r.removeFromLeft (10);
    slope.setBounds (r.removeFromLeft (130).withTrimmedTop (10).withHeight (44));
    r.removeFromLeft (20);
    for (auto* k : { &freq, &gain, &q })
        k->setBounds (r.removeFromLeft (90));
}
