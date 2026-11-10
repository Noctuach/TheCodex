#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "Widgets.h"
#include "EQDisplay.h"
#include "Tuner.h"
#include "Scope.h"
#include "PresetBar.h"
#include "DistortionView.h"
#include "Overlays.h"

class CodexProcessor;

/** The complete UI at its design size. The editor scales this view to the window size. */
class MainView : public juce::Component, private juce::Timer
{
public:
    static constexpr int baseWidth = 1200, baseHeight = 850;

    MainView (CodexProcessor&, juce::PropertiesFile* settings);

    void paint (juce::Graphics&) override;
    void resized() override;

    void showTab (int tab);   // 0 = KICK, 1 = DISTORTION
    void openAbout() { aboutOverlay.open(); }
    void openSaveDialog() { presetBar.onSaveRequested(); }
    std::function<void (float)> onScaleRequested;

private:
    void timerCallback() override;
    void updateEngineView();
    void showMainMenu();
    void setTooltipsEnabled (bool);
    std::vector<juce::Component*> kickTabComponents();

    struct QuickDistortion
    {
        PowerButton on;
        juce::TextButton edit { "EDIT" };
        ChoiceBox mode { "Type" };
        Knob drive { "Drive" }, character { "Character" }, mix { "Mix" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    };

    CodexProcessor& proc;
    juce::PropertiesFile* settings;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    int currentTab = 0;
    juce::Rectangle<int> header, logoArea, presetArea, tabBar, sourceArea, pitchArea, ampArea, eqArea,
                         quickAreas[2], outArea;
    std::array<juce::Rectangle<int>, 6> sourceCells;

    // Header
    IconButton undoButton { "undo", Icons::undo() }, redoButton { "redo", Icons::redo() }, menuButton { "menu", Icons::menu() };
    PresetBar presetBar;
    ChoiceBox keyBox { "Key" }, octaveBox { "Oct" }, keyModeBox { "Key Mode" };
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

    // Distortion
    QuickDistortion quick[2];
    DistortionUnit unit1, unit2;

    // Output
    KickScope scope;
    TunerDisplay tuner;
    Knob outGain { "Output" };
    LevelMeter meter;

    /** Shown over the pitch-envelope panel while the 909 engine is active. */
    struct EngineNotice : public juce::Component
    {
        EngineNotice() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override;
    } engineNotice;

    // Overlays
    SavePresetOverlay saveOverlay;
    AboutOverlay aboutOverlay;
};

class CodexEditor : public juce::AudioProcessorEditor
{
public:
    explicit CodexEditor (CodexProcessor&);
    ~CodexEditor() override;

    void resized() override;
    void showTab (int tab) { view.showTab (tab); }
    MainView& getView() { return view; }

private:
    void applyScale (float scale);

    CodexLookAndFeel lnf;
    juce::ApplicationProperties appProperties;
    MainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CodexEditor)
};
