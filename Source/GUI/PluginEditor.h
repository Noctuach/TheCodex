#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "Widgets.h"
#include "EQDisplay.h"
#include "SpaceView.h"
#include "CompressorView.h"
#include "Eq2View.h"
#include "Tuner.h"
#include "PresetBar.h"
#include "DistortionView.h"
#include "ScopeView.h"
#include "SaturationView.h"
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

    void showTab (int tab);   // 0 = KICK, 1 = DISTORTION, 2 = SATURATION, 3 = COMPRESSOR, 4 = EQ 2, 5 = SPACE, 6 = SCOPE
    void openAbout() { aboutOverlay.open(); }
    void openSaveDialog() { presetBar.onSaveRequested(); }
    std::function<void (float)> onScaleRequested;

private:
    void timerCallback() override;
    void updateEngineView();
    void showMainMenu();
    void setTooltipsEnabled (bool);
    std::vector<juce::Component*> kickTabComponents();

    CodexProcessor& proc;
    juce::PropertiesFile* settings;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    int currentTab = 0;
    juce::Rectangle<int> header, logoArea, presetArea, tabBar, breadcrumbArea, sourceArea, pitchArea, ampArea, eqArea;
    std::array<juce::Rectangle<int>, 6> sourceCells;

    // Header
    IconButton undoButton { "undo", Icons::undo() }, redoButton { "redo", Icons::redo() }, menuButton { "menu", Icons::menu() };
    PresetBar presetBar;
    ChoiceBox keyBox { "Key" }, octaveBox { "Oct" }, keyModeBox { "Key Mode" };
    PressButton previewButton { "preview", Icons::play() };
    HeaderKnob outGain { "Output" };
    LevelMeter meterL, meterR;
    std::array<juce::TextButton, 7> tabButtons { juce::TextButton ("KICK"), juce::TextButton ("DISTORTION"), juce::TextButton ("SATURATION"),
                                                 juce::TextButton ("COMPRESSOR"), juce::TextButton ("EQ 2"), juce::TextButton ("SPACE"), juce::TextButton ("SCOPE") };
    TunerAnalyser tunerAnalyser;
    TunerChip tunerChip;

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

    // Distortion, saturation, scope
    DistortionUnit unit1, unit2;
    ScopePage scopePage;
    SaturationPage saturationPage;
    SpacePage spacePage;
    CompressorPage compressorPage;
    Eq2Page eq2Page;

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

    bool initialised = false;   // only save the window size once the user can actually change it
    CodexLookAndFeel lnf;
    juce::ApplicationProperties appProperties;
    MainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CodexEditor)
};
