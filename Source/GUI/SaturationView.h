#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Widgets.h"
#include "PresetBar.h"
#include "ScopeView.h"
#include "../DSP/MultibandSaturator.h"

/** Spectrum with draggable crossovers and per-band mute / solo / bypass badges. */
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
        const int nb = numBands();

        // Frequency grid
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
            g.setColour (Colours_::band (b).withAlpha (sel ? 0.12f : 0.04f));
            g.fillRect (region);
            if (sel)
            {
                g.setColour (Colours_::band (b).withAlpha (0.8f));
                g.fillRect (region.withHeight (2.0f));
            }

            // label: style + drive
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
            auto meter = region.reduced (8.0f, 0.0f).withTrimmedTop (34.0f).withTrimmedBottom (50.0f).removeFromRight (4.0f);
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

        // Spectrum of the output (mid)
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
            g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (0.22f), 0, plot.getY(), Colours_::accent.withAlpha (0.02f), 0, plot.getBottom(), false));
            g.fillPath (p);
            g.setColour (Colours_::accent.withAlpha (0.4f));
            g.strokePath (p, PathStrokeType (1.0f));
        }

        // Crossover lines with handles
        for (int j = 0; j < nb - 1; ++j)
        {
            const float x = freqToX (crossover (j));
            const bool hot = j == hoverCrossover || j == dragging;
            g.setColour ((hot ? Colours_::accentHi : Colours_::text).withAlpha (hot ? 0.9f : 0.5f));
            g.drawLine (x, plot.getY(), x, plot.getBottom(), hot ? 2.0f : 1.2f);
            auto handle = Rectangle<float> (54.0f, 18.0f).withCentre ({ x, plot.getY() + 12.0f });
            g.setColour (hot ? Colours_::accent : Colours_::panelTop);
            g.fillRoundedRectangle (handle, 9.0f);
            g.setColour (hot ? Colours_::accent : Colours_::outlineHi);
            g.drawRoundedRectangle (handle, 9.0f, 1.0f);
            g.setColour (hot ? Colour (0xff04101e) : Colours_::text);
            g.setFont (Fonts::medium (11.5f));
            const float f = crossover (j);
            g.drawText (f < 1000 ? String (roundToInt (f)) : String (f / 1000.0f, 1) + "k", handle, Justification::centred);
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
        const int h = hitCrossover (e.position.x);
        if (h != hoverCrossover) { hoverCrossover = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hoverCrossover = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int nb = numBands();
        for (int b = 0; b < nb; ++b)
            for (int k = 0; k < 3; ++k)
                if (badgeRect (b, k).contains (e.position))
                {
                    const char* sfx = k == 0 ? "Mute" : k == 1 ? "Solo" : "Bypass";
                    if (auto* prm = apvts.getParameter (ID::satBand (b, sfx)))
                    {
                        prm->beginChangeGesture();
                        prm->setValueNotifyingHost (value (ID::satBand (b, sfx)) > 0.5f ? 0.0f : 1.0f);
                        prm->endChangeGesture();
                    }
                    return;
                }

        dragging = hitCrossover (e.position.x);
        if (dragging >= 0)
        {
            if (auto* prm = crossoverParam (dragging)) prm->beginChangeGesture();
            return;
        }
        for (int b = 0; b < nb; ++b)
            if (bandRegion (b).contains (e.position))
            {
                selected = b;
                if (onBandSelected) onBandSelected (b);
                repaint();
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const int nb = numBands();
        const float lo = dragging > 0 ? crossover (dragging - 1) * 1.15f : 30.0f;
        const float hi = dragging < nb - 2 ? crossover (dragging + 1) / 1.15f : 16000.0f;
        if (auto* prm = crossoverParam (dragging))
            prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (lo, hi, xToFreq (e.position.x))));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            if (auto* prm = crossoverParam (dragging)) prm->endChangeGesture();
        dragging = -1;
    }

private:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;

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
    int numBands() const { return juce::jlimit (1, sat::maxBands, (int) value (ID::sat ("Bands"))); }
    float crossover (int j) const { return value (ID::sat (("X" + juce::String (j + 1)).toRawUTF8())); }
    juce::RangedAudioParameter* crossoverParam (int j) const { return apvts.getParameter (ID::sat (("X" + juce::String (j + 1)).toRawUTF8())); }

    float freqToX (float f) const { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    float xToFreq (float x) const { return 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth()); }

    juce::Rectangle<float> bandRegion (int b) const
    {
        const int nb = numBands();
        const float x0 = b == 0 ? plot.getX() : freqToX (crossover (b - 1));
        const float x1 = b == nb - 1 ? plot.getRight() : freqToX (crossover (b));
        return { x0, plot.getY(), juce::jmax (1.0f, x1 - x0), plot.getHeight() };
    }

    juce::Rectangle<float> badgeRect (int b, int k) const
    {
        const auto region = bandRegion (b);
        return { region.getX() + 8.0f + (float) k * 22.0f, region.getY() + 30.0f, 19.0f, 18.0f };
    }

    int hitCrossover (float x) const
    {
        for (int j = 0; j < numBands() - 1; ++j)
            if (std::abs (freqToX (crossover (j)) - x) < 7.0f) return j;
        return -1;
    }

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    const StereoHistory& history;
    juce::Rectangle<float> plot;
    int selected = 0, dragging = -1, hoverCrossover = -1;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftData, spectrum;
    std::array<float, sat::maxBands> bandLevels {};
};

//==============================================================================
/** The SATURATION tab. */
class SaturationPage : public juce::Component
{
public:
    SaturationPage (CodexProcessor& p, const StereoHistory& h)
        : apvts (p.apvts), display (p, h),
          bandCount (*p.apvts.getParameter (ID::sat ("Bands")), { "1", "2", "3", "4", "5", "6" },
                     "Bands: number of frequency bands, each with its own saturation", 1)
    {
        power = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::sat ("On"), powerButton);
        powerButton.setTooltip ("Saturation On/Off: multiband saturation after Distortion 2");
        slope.attach (apvts, ID::sat ("Slope"));
        oversampling.attach (apvts, ID::sat ("OS"));
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

        display.onBandSelected = [this] (int b) { selectBand (b); };
        bandCount.onChange = [this] (int) { if (band >= numBands()) selectBand (numBands() - 1); };

        for (auto* c : std::initializer_list<juce::Component*> { &display, &powerButton, &bandCount, &slope, &oversampling, &globalMix, &globalOut,
                                                                &style, &drive, &mix, &feedback, &feedbackFreq, &dynamics, &tone, &level,
                                                                &mute, &solo, &bypass })
            addAndMakeVisible (c);
        selectBand (0);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionPanel (g, displayArea, "MULTIBAND SATURATION", Colours_::accent);
        drawSectionPanel (g, bandArea, "BAND " + juce::String (band + 1), Colours_::band (band));
        drawSectionPanel (g, globalArea, "GLOBAL", Colours_::accent);
        drawGroupCaption (g, shapeCaption, "SATURATION");
        drawGroupCaption (g, colourCaption, "FEEDBACK  /  DYNAMICS  /  TONE");

        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (11.5f, 0.14f));
        g.drawText ("BANDS", bandCount.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        displayArea = r.removeFromTop (r.getHeight() * 47 / 100);
        powerButton.setBounds (displayArea.withHeight (28).removeFromRight (32).reduced (4));
        display.setBounds (displayArea.reduced (10).withTrimmedTop (30));
        r.removeFromTop (12);

        globalArea = r.removeFromRight (300);
        r.removeFromRight (12);
        bandArea = r;

        // Band panel
        {
            auto c = bandArea.reduced (14).withTrimmedTop (30);
            auto left = c.removeFromLeft (210);
            style.setBounds (left.removeFromTop (48));
            left.removeFromTop (14);
            auto pills = left.removeFromTop (28);
            const int pw = (pills.getWidth() - 12) / 3;
            mute.setBounds (pills.removeFromLeft (pw)); pills.removeFromLeft (6);
            solo.setBounds (pills.removeFromLeft (pw)); pills.removeFromLeft (6);
            bypass.setBounds (pills);
            left.removeFromTop (8);
            drive.setBounds (left);
            c.removeFromLeft (20);

            shapeCaption = c.removeFromTop (16);
            auto row1 = c.removeFromTop (c.getHeight() / 2 - 10);
            c.removeFromTop (6);
            colourCaption = c.removeFromTop (16);
            auto row2 = c;
            const int w = row1.getWidth() / 3;
            mix.setBounds (row1.removeFromLeft (w));
            level.setBounds (row1.removeFromLeft (w));
            const int w4 = row2.getWidth() / 4;
            feedback.setBounds (row2.removeFromLeft (w4));
            feedbackFreq.setBounds (row2.removeFromLeft (w4));
            dynamics.setBounds (row2.removeFromLeft (w4));
            tone.setBounds (row2);
        }

        // Global panel
        {
            auto c = globalArea.reduced (14).withTrimmedTop (30);
            c.removeFromTop (18);
            bandCount.setBounds (c.removeFromTop (28));
            c.removeFromTop (12);
            auto combos = c.removeFromTop (48);
            slope.setBounds (combos.removeFromLeft (combos.getWidth() / 2 - 4));
            combos.removeFromLeft (8);
            oversampling.setBounds (combos);
            c.removeFromTop (10);
            globalMix.setBounds (c.removeFromLeft (c.getWidth() / 2));
            globalOut.setBounds (c);
        }
    }

private:
    int numBands() const { return juce::jlimit (1, sat::maxBands, (int) apvts.getRawParameterValue (ID::sat ("Bands"))->load()); }

    void selectBand (int b)
    {
        band = juce::jlimit (0, sat::maxBands - 1, b);
        display.setSelectedBand (band);
        styleAttachment.reset();
        styleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, ID::satBand (band, "Style"), style.box);
        style.box.setTooltip (Help::forParameter (ID::satBand (band, "Style")));
        drive.attach (apvts, ID::satBand (band, "Drive"));
        mix.attach (apvts, ID::satBand (band, "Mix"));
        feedback.attach (apvts, ID::satBand (band, "Fb"));
        feedbackFreq.attach (apvts, ID::satBand (band, "FbFreq"));
        dynamics.attach (apvts, ID::satBand (band, "Dyn"));
        tone.attach (apvts, ID::satBand (band, "Tone"));
        level.attach (apvts, ID::satBand (band, "Level"));
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
    SegmentedChoice bandCount;
    ChoiceBox slope { "Crossover Slope" }, oversampling { "Oversampling" };
    Knob globalMix { "Mix" }, globalOut { "Output" };

    int band = 0;
    ChoiceBox style { "Style" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> styleAttachment;
    Knob drive { "Drive" }, mix { "Mix" }, level { "Level" }, feedback { "Feedback" }, feedbackFreq { "Feedback Freq" },
         dynamics { "Dynamics" }, tone { "Tone" };
    juce::TextButton mute { "MUTE" }, solo { "SOLO" }, bypass { "BYPASS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAtt, soloAtt, bypassAtt;

    juce::Rectangle<int> displayArea, bandArea, globalArea, shapeCaption, colourCaption;
};
