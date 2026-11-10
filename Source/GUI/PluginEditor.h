#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "Widgets.h"
#include "EQDisplay.h"
#include "Tuner.h"
#include "PresetBar.h"
#include "DistortionView.h"

class HardKickProcessor;

class HardKickEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HardKickEditor (HardKickProcessor&);
    ~HardKickEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void showTab (int tab);   // 0 = KICK, 1 = DISTORTION

private:
    void timerCallback() override;
    void updateEngineView();
    std::vector<juce::Component*> kickTabComponents();

    struct ClipSection
    {
        PowerButton on;
        juce::TextButton edit { "EDIT" };
        ChoiceBox mode { "Type" };
        Knob drive { "Drive" }, character { "Character" }, mix { "Mix" }, out { "Output" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    };

    HKLookAndFeel lnf;
    HardKickProcessor& proc;

    int currentTab = 0;
    juce::Rectangle<int> header, tabBar, upperArea, sourceArea, pitchArea, ampArea, eqArea, clip1Area, clip2Area, outArea;
    std::array<juce::Rectangle<int>, 6> sourceCells;

    // Header
    PresetBar presetBar;
    ChoiceBox keyBox { "Key" }, octaveBox { "Oct" }, keyModeBox { "Mode" };
    TriggerButton kickButton { "KICK" };
    juce::TextButton kickTabButton { "KICK" }, distTabButton { "DISTORTION" };

    // Source
    SegmentedChoice engineSwitch;
    ChoiceBox wave { "Wave" }, model { "909 Model" };
    Knob tune { "Fine Tune" }, phase { "Phase" }, clickLevel { "Click" }, clickDecay { "Click Decay" }, clickTone { "Click Tone" };
    Knob tune909 { "Tune" }, attack909 { "Attack" }, decay909 { "Decay" };

    // Envelopes
    EnvelopeDisplay pitchEnvDisplay, ampEnvDisplay;
    Knob pAmount { "Amount" }, pAttack { "Attack" }, pDecay { "Decay" }, pSustain { "Sustain" }, pRelease { "Release" }, pCurve { "Curve" };
    Knob aAttack { "Attack" }, aDecay { "Decay" }, aSustain { "Sustain" }, aRelease { "Release" }, aCurve { "Curve" };

    // EQ
    EQDisplay eqDisplay;
    BandControls bandControls;

    // Clip stages: compact quick controls (bottom row) + full units (distortion tab)
    ClipSection clips[2];
    DistortionUnit unit1, unit2;

    // Master
    TunerDisplay tuner;
    Knob outGain { "Output" };
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardKickEditor)
};
