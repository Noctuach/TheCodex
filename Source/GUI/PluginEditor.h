#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "Widgets.h"
#include "EQDisplay.h"

class HardKickProcessor;

class HardKickEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HardKickEditor (HardKickProcessor&);
    ~HardKickEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawPanel (juce::Graphics&, juce::Rectangle<int>, const juce::String& title, juce::Colour dot);

    struct ClipSection
    {
        PowerButton on;
        ChoiceBox mode { "Mode" };
        Knob drive { "Drive" }, bias { "Bias" }, mix { "Mix" }, out { "Output" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    };

    HKLookAndFeel lnf;
    HardKickProcessor& proc;

    juce::Rectangle<int> header, screen, oscArea, pitchArea, ampArea, eqArea, clip1Area, clip2Area, outArea;

    TriggerButton kickButton { "KICK" };

    // Oscillator
    ChoiceBox wave { "Wave" };
    Knob tune { "Tune" }, phase { "Phase" }, clickLevel { "Click" }, clickDecay { "Click Decay" }, clickTone { "Click Tone" };

    // Envelopes
    EnvelopeDisplay pitchEnvDisplay, ampEnvDisplay;
    Knob pAmount { "Amount" }, pAttack { "Attack" }, pDecay { "Decay" }, pSustain { "Sustain" }, pRelease { "Release" }, pCurve { "Curve" };
    Knob aAttack { "Attack" }, aDecay { "Decay" }, aSustain { "Sustain" }, aRelease { "Release" }, aCurve { "Curve" };

    // EQ
    EQDisplay eqDisplay;
    BandControls bandControls;

    // Clip stages
    ClipSection clips[2];

    // Output
    Knob outGain { "Output" };
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardKickEditor)
};
