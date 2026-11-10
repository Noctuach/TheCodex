#pragma once
#include "Widgets.h"
#include "../DSP/PitchDetector.h"
#include "../PluginProcessor.h"

/**
    Measures the real fundamental of the plugin's output (after distortion, EQ and saturation).
    One analyser is shared by every tuner display.
*/
class TunerAnalyser : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    explicit TunerAnalyser (CodexProcessor& p) : proc (p)
    {
        pullBuffer.resize (32768);
        ring.assign (ringSize, 0.0f);
        startTimerHz (30);
    }

    float getTargetNote() const { return proc.getTargetNote(); }
    bool hasReading() const { return holdFrames > 0 && measuredHz > 0.0f; }
    float getMeasuredHz() const { return measuredHz; }

private:
    static constexpr int ringSize = 1536, windowSize = 1024;
    static constexpr double analysisRate = 6000.0;

    void timerCallback() override
    {
        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        const int decim = juce::jmax (1, juce::roundToInt (sr / analysisRate));
        const int got = proc.tunerFeed.pull (pullBuffer.data(), (int) pullBuffer.size());

        for (int i = 0; i < got; ++i)
        {
            acc += pullBuffer[(size_t) i];
            if (++accCount == decim)
            {
                ring[(size_t) writePos] = acc / (float) decim;
                writePos = (writePos + 1) % ringSize;
                acc = 0.0f; accCount = 0;
            }
        }

        if (got > 0)
        {
            std::vector<float> window ((size_t) windowSize);
            double energy = 0.0;
            for (int i = 0; i < windowSize; ++i)
            {
                const float v = ring[(size_t) ((writePos - windowSize + i + ringSize) % ringSize)];
                window[(size_t) i] = v;
                energy += (double) v * v;
            }
            const float rms = (float) std::sqrt (energy / windowSize);
            if (rms > 0.01f)
            {
                const auto res = detectPitch (window.data(), windowSize, sr / decim, 25.0f, 400.0f);
                if (res.frequency > 0.0f && res.clarity > 0.9f)
                {
                    history[(size_t) historyPos] = res.frequency;
                    historyPos = (historyPos + 1) % (int) history.size();
                    historyCount = juce::jmin (historyCount + 1, (int) history.size());
                    std::array<float, 5> sorted {};
                    std::copy (history.begin(), history.begin() + historyCount, sorted.begin());
                    std::sort (sorted.begin(), sorted.begin() + historyCount);
                    measuredHz = sorted[(size_t) historyCount / 2];
                    holdFrames = 60; // keep showing the last reading for 3 s
                }
            }
            else
                historyCount = 0;
        }

        if (holdFrames > 0) --holdFrames;
        sendChangeMessage();
    }

    CodexProcessor& proc;
    std::vector<float> pullBuffer, ring;
    int writePos = 0, accCount = 0;
    float acc = 0.0f;
    std::array<float, 5> history {};
    int historyPos = 0, historyCount = 0, holdFrames = 0;
    float measuredHz = 0.0f;
};

/** Full tuner: note, frequency, cents meter and target. */
class TunerDisplay : public juce::Component, private juce::ChangeListener
{
public:
    explicit TunerDisplay (TunerAnalyser& a) : analyser (a) { analyser.addChangeListener (this); }
    ~TunerDisplay() override { analyser.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
        r = r.reduced (8.0f, 5.0f);

        const float target = analyser.getTargetNote();
        const bool hasReading = analyser.hasReading();
        const float shownNote = hasReading ? frequencyToMidi (analyser.getMeasuredHz()) : target;
        const int nearest = roundToInt (shownNote);
        const float cents = (shownNote - (float) nearest) * 100.0f;
        const bool onTarget = hasReading && std::abs (shownNote - target) < 0.1f;   // within 10 cents
        const auto noteColour = ! hasReading ? Colours_::textDim : (onTarget ? Colour (0xff51e5a0) : Colours_::accent);

        // Bottom line: target
        auto bottom = r.removeFromBottom (14.0f);
        g.setFont (Fonts::label (11.5f, 0.08f));
        g.setColour (Colours_::textDim);
        const int tn = roundToInt (target);
        g.drawText ("TARGET  " + String (noteNameFor (tn)) + String (octaveFor (tn)) + "  "
                    + String (midiToFrequency (target), 2) + " Hz", bottom, Justification::centredLeft);

        // Note name
        auto top = r.removeFromTop (r.getHeight() * 0.62f);
        auto noteArea = top.removeFromLeft (64.0f);
        g.setColour (noteColour);
        g.setFont (Fonts::semibold (30.0f));
        g.drawText (String (noteNameFor (nearest)), noteArea.withTrimmedRight (14.0f), Justification::centredRight);
        g.setFont (Fonts::semibold (13.0f));
        g.drawText (String (octaveFor (nearest)), noteArea.removeFromRight (14.0f).withTrimmedTop (14.0f), Justification::centredLeft);

        // Hz and cents
        g.setFont (Fonts::regular (13.0f));
        g.setColour (hasReading ? Colours_::text : Colours_::textDim);
        g.drawText (hasReading ? String (analyser.getMeasuredHz(), 2) + " Hz" : String ("no signal"), top.withTrimmedLeft (8.0f).removeFromTop (top.getHeight() * 0.5f),
                    Justification::bottomLeft);
        g.setColour (Colours_::textDim);
        g.drawText (hasReading ? (cents >= 0 ? "+" : "") + String (cents, 1) + " cents" : String ("press KICK"),
                    top.withTrimmedLeft (8.0f).withTrimmedTop (top.getHeight() * 0.5f), Justification::topLeft);

        // Cents meter
        auto meter = r.reduced (0.0f, 4.0f);
        const float midX = meter.getCentreX();
        g.setColour (Colours_::grid.brighter (0.4f));
        g.fillRoundedRectangle (meter.withSizeKeepingCentre (meter.getWidth(), 3.0f), 1.5f);
        for (int c = -50; c <= 50; c += 10)
        {
            const float x = jmap ((float) c, -50.0f, 50.0f, meter.getX(), meter.getRight());
            g.setColour (c == 0 ? Colours_::text : Colours_::textDim.withAlpha (0.5f));
            g.drawVerticalLine (roundToInt (x), meter.getY() + (c == 0 ? 0.0f : 3.0f), meter.getBottom() - (c == 0 ? 0.0f : 3.0f));
        }
        if (hasReading)
        {
            const float x = jmap (jlimit (-50.0f, 50.0f, cents), -50.0f, 50.0f, meter.getX(), meter.getRight());
            g.setColour (noteColour.withAlpha (0.25f));
            g.fillRect (Rectangle<float> (jmin (x, midX), meter.getY() + 2, std::abs (x - midX), meter.getHeight() - 4));
            g.setColour (noteColour);
            g.fillRoundedRectangle (Rectangle<float> (4.0f, meter.getHeight()).withCentre ({ x, meter.getCentreY() }), 2.0f);
        }
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { if (isShowing()) repaint(); }
    TunerAnalyser& analyser;
};

/** Compact root-note readout (always visible in the tab bar). */
class TunerChip : public juce::Component, public juce::SettableTooltipClient, private juce::ChangeListener
{
public:
    explicit TunerChip (TunerAnalyser& a) : analyser (a)
    {
        analyser.addChangeListener (this);
        setTooltip ("Tuner: measured root note of the output. Green = within 10 cents of the selected key.");
    }
    ~TunerChip() override { analyser.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Colours_::display);
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        const bool has = analyser.hasReading();
        const float target = analyser.getTargetNote();
        const float note = has ? frequencyToMidi (analyser.getMeasuredHz()) : target;
        const int nearest = roundToInt (note);
        const float cents = (note - (float) nearest) * 100.0f;
        const bool onTarget = has && std::abs (note - target) < 0.1f;
        const auto col = ! has ? Colours_::textDim : (onTarget ? Colours_::good : Colours_::accent);
        g.setColour (has ? col.withAlpha (0.6f) : Colours_::outline);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

        auto t = r.reduced (12.0f, 0.0f);
        g.setFont (Fonts::label (11.0f, 0.16f));
        g.setColour (Colours_::textDim);
        g.drawText ("ROOT", t.removeFromLeft (34.0f), Justification::centredLeft);
        g.setColour (col);
        g.setFont (Fonts::semibold (15.0f));
        g.drawText (String (noteNameFor (nearest)) + String (octaveFor (nearest)), t.removeFromLeft (34.0f), Justification::centredLeft);
        g.setFont (Fonts::regular (12.5f));
        g.setColour (has ? Colours_::text : Colours_::textFaint);
        g.drawText (has ? (cents >= 0 ? "+" : "") + String (cents, 1) + " c" : String ("--"), t, Justification::centredLeft);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { if (isShowing()) repaint(); }
    TunerAnalyser& analyser;
};
