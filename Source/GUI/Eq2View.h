#pragma once
#include "EQDisplay.h"
#include "Help.h"

/** The EQ 2 tab: a second 24-band equaliser after the Compressor (before Space), for final shaping. */
class Eq2Page : public juce::Component, private juce::Timer
{
public:
    explicit Eq2Page (CodexProcessor& p) : apvts (p.apvts), display (p, 1), controls (p.apvts, 1)
    {
        power = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ID::eqOn (1), powerButton);
        powerButton.setTooltip (Help::forParameter (ID::eqOn (1)));
        display.onBandSelected = [this] (int b) { controls.setBand (b); };
        controls.onBandSelected = [this] (int b) { display.setSelectedBand (b); };
        for (juce::Component* c : std::initializer_list<juce::Component*> { &powerButton, &display, &controls })
            addAndMakeVisible (c);
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionPanel (g, getLocalBounds(), juce::String::fromUTF8 ("EQ 2  \xc2\xb7  POST"), off ? Colours_::textFaint : Colours_::accent);
        if (EQDisplay::numPresent (apvts, 1) == 0)
        {
            g.setColour (Colours_::textDim);
            g.setFont (Fonts::regular (13.0f));
            g.drawText ("Double-click the display to add a band. EQ 2 sits after the Compressor, before Space.",
                        display.getBounds().withTrimmedTop (display.getHeight() / 2 - 10).withHeight (20), juce::Justification::centred);
        }
    }

    void resized() override
    {
        powerButton.setBounds (getLocalBounds().withHeight (28).removeFromRight (32).reduced (4));
        auto c = getLocalBounds().reduced (14).withTrimmedTop (28);
        controls.setBounds (c.removeFromBottom (84));
        c.removeFromBottom (10);
        display.setBounds (c);
    }

private:
    void timerCallback() override
    {
        const bool o = apvts.getRawParameterValue (ID::eqOn (1))->load() < 0.5f;
        const int n = EQDisplay::numPresent (apvts, 1);
        if (o != off || n != lastCount) { off = o; lastCount = n; display.setAlpha (off ? 0.45f : 1.0f); repaint(); }
    }

    juce::AudioProcessorValueTreeState& apvts;
    EQDisplay display;
    BandControls controls;
    PowerButton powerButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> power;
    bool off = false;
    int lastCount = -1;
};
