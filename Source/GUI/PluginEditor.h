#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LookAndFeel.h"
#include "Widgets.h"
#include "EQDisplay.h"
#include "SpaceView.h"
#include "CompressorView.h"
#include "Eq2View.h"
#include "DrawEnvelopeEditor.h"
#include "PresetBrowser.h"
#include "ExportButton.h"
#include "Tuner.h"
#include "PresetBar.h"
#include "DistortionView.h"
#include "ScopeView.h"
#include "SaturationView.h"
#include "Overlays.h"
#include "ChainStrip.h"
#include "Sweep909Display.h"
#include "Dancer.h"
#include "KeyboardFocus.h"

class CodexProcessor;

/**
    The user settings file (UI size, tooltips, preset favourites), shared by every editor in the process so that one
    instance cannot write back a stale copy of another's values. No InterProcessLock: on Linux its lock file is shared by
    every user (/var/tmp), so a second user, or a sandbox that cannot create it, would never get the lock and PropertiesFile
    would silently skip every load and save. PropertiesFile already saves atomically (temporary file + rename), so instances
    in other processes end up last-writer-wins either way.
*/
struct CodexSettings
{
    CodexSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Codex";
        o.filenameSuffix = ".settings";
       #if JUCE_LINUX
        o.folderName = ".config/Codex";   // keep the user's home folder tidy
       #else
        o.folderName = "Codex";
       #endif
        o.osxLibrarySubFolder = "Application Support";
        o.millisecondsBeforeSaving = 500;
        props.setStorageParameters (o);
    }
    juce::PropertiesFile* user() { return props.getUserSettings(); }

    juce::ApplicationProperties props;
};

/** The complete UI at its design size. The editor scales this view to the window size. */
class MainView : public juce::Component, private juce::Timer
{
public:
    static constexpr int baseWidth = 1200, baseHeight = 850;

    MainView (CodexProcessor&, juce::PropertiesFile* settings);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    void showTab (int tab);   // 0 = KICK, 1 = DISTORTION, 2 = SATURATION, 3 = COMPRESSOR, 4 = EQ 2, 5 = SPACE, 6 = SCOPE
    void openAbout() { aboutOverlay.open(); }
    void openBrowser() { browserOverlay.open(); }
    void openGuide() { guideOverlay.open(); }
    void toggleDancer() { dancer.toggle(); }
    void mouseUp (const juce::MouseEvent&) override;
    void openSaveDialog() { presetBar.onSaveRequested(); }
    /** Keyboard focus policy (KeyboardFocus.h): off = the host keeps the computer keyboard (default). */
    void setKeyboardShortcutsEnabled (bool);
    KeyboardFocusPolicy& getKeyboardFocusPolicy() { return focusPolicy; }
    std::function<void (float)> onScaleRequested;
    /** The window's actual scale (window width / baseWidth) and the largest scale that fits the editor's screen. */
    std::function<float()> getWindowScale, getFitScale;

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
    bool keyDimmed = false;   // Key / Oct dimmed while Key Mode is Follow MIDI
    PressButton previewButton { "preview", Icons::play() };
    HeaderKnob outGain { "Output" };
    LevelMeter meterL, meterR;
    std::array<juce::TextButton, 7> tabButtons { juce::TextButton ("KICK"), juce::TextButton ("DISTORTION"), juce::TextButton ("SATURATION"),
                                                 juce::TextButton ("COMPRESSOR"), juce::TextButton ("EQ 2"), juce::TextButton ("SPACE"), juce::TextButton ("SCOPE") };
    TunerAnalyser tunerAnalyser;
    TunerChip tunerChip;
    ChainStrip chainStrip;

    // Small animations: the tab underline slides, a page fades in when shown, each hit pulses the logo
    struct Veil : public juce::Component
    {
        Veil() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { g.fillAll (Colours_::background.withAlpha (amount)); }
        float amount = 0.0f;
    } pageVeil;
    Dancer dancer { proc.hitCounter };   // easter egg: click the book
    juce::Rectangle<float> underline, underlineTarget;
    float logoPulse = 0.0f;
    unsigned int lastHits = 0;
    void setUnderlineTarget (bool snap);
    void animate();

    // Source
    SegmentedChoice engineSwitch;
    ChoiceBox wave { "Wave" }, model { "909 Model" };
    Knob tune { "Fine Tune" }, phase { "Phase" }, clickLevel { "Click" }, clickDecay { "Click Decay" }, clickTone { "Click Tone" };
    Knob tune909 { "Tune" }, attack909 { "Attack" }, decay909 { "Decay" };
    // 909 circuit (shown in the pitch panel with the 909 engine): Tune Depth, Accent, Overload, Decay Hold
    Knob depth909 { "Depth" }, accent909 { "Accent" }, overload909 { "Overload" };
    juce::TextButton hold909 { "DECAY HOLD" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> hold909Attachment;
    Sweep909Display sweep909;

    // Envelopes
    EnvelopeDisplay pitchEnvDisplay, ampEnvDisplay;
    DrawEnvelopeEditor pitchDrawEditor, ampDrawEditor;
    SegmentedChoice pitchModeSwitch, ampModeSwitch;
    void updateEnvelopeModes();

    // Footer: macros, A/B compare, CPU
    std::array<HeaderKnob, ID::numMacros> macroKnobs { HeaderKnob ("Punch"), HeaderKnob ("Dirt"), HeaderKnob ("Tail"), HeaderKnob ("Width") };
    juce::TextButton abA { "A" }, abB { "B" }, abCopy { "COPY" };
    juce::TextButton clipButton { "CLIP" };
    ExportButton exportButton { proc };
    HeaderKnob clipCeiling { "Ceiling" }, clipSoftness { "Knee" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> clipAttachment;
    float clipShown = 0.0f;
    std::array<juce::ValueTree, 2> abSlots;
    int abCurrent = 0;
    void switchAB (int slot);
    juce::Rectangle<int> footer;
    float cpuShown = 0.0f;
    int footerTick = 0;
    Knob pAmount { "Amount" }, pAttack { "Attack" }, pDecay { "Decay" }, pSustain { "Sustain" }, pRelease { "Release" }, pCurve { "Curve" };
    Knob aAttack { "Attack" }, aDecay { "Decay" }, aSustain { "Sustain" }, aRelease { "Release" }, aCurve { "Curve" };

    // Sub layer + velocity
    PowerButton subPower;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> subPowerAttachment;
    Knob subLevel { "Level" }, subLowPass { "Low-Pass" }, subFade { "Fade-In" }, subPhase { "Phase" }, velocity { "Velocity" };
    juce::Rectangle<int> subArea, velocityCaption;
    juce::TextButton subAlign { "ALIGN" };

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
    PresetBrowserOverlay browserOverlay;
    QuickGuideOverlay guideOverlay;
    AboutOverlay aboutOverlay;

    KeyboardFocusPolicy focusPolicy;   // last: its sweep covers every component above
};

class CodexEditor : public juce::AudioProcessorEditor
{
public:
    explicit CodexEditor (CodexProcessor&);
    ~CodexEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void showTab (int tab) { view.showTab (tab); }
    MainView& getView() { return view; }

    static constexpr float minScale = 0.75f;    // smallest size offered by the menu and the resize corner (screens permitting)
    static constexpr float maxScale = 2.0f;

private:
    void applyScale (float scale);
    /** The largest scale whose window fits the work area of the editor's display (or, before the editor is on screen, the
        display under the mouse). Never below 0.6, so a very small screen still gets a window. */
    float maxScaleForScreen() const;
    /** The smallest scale allowed on this screen: minScale, or the screen fit when that is smaller (never below 0.6).
        applyScale and the resize limits (the host-facing constrainer and the resize corner) both use it. */
    float minScaleForScreen() const;
    void updateResizeLimits();

    bool initialised = false;   // only save the window size once the user can actually change it
    bool screenChecked = false; // the one-shot fit check once the editor has a peer
    CodexLookAndFeel lnf;
    juce::SharedResourcePointer<CodexSettings> settings;
    MainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CodexEditor)
};
