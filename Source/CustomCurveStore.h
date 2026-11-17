#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "DSP/ClipDistortion.h"
#include "DSP/CustomCurve.h"

/**
    Owner of the custom distortion curves in the plug-in state.

    The drawn points are not host parameters (a curve of up to 64 free points cannot be automated sensibly); they are
    stored as one string per unit in a "CURVES" child of the APVTS state (properties "c1" / "c2", FreeCurve text
    form). So they travel with the project, user presets and A/B, and every edit is an undoable property change
    (a whole drag coalesces into one undo step). Only "Dist N Custom Curve" (on/off) is a parameter.

    A missing or empty property means "no custom curve": the unit then plays its Type exactly as before, even with
    Custom Curve switched on.

    Whenever the stored text changes (an edit, undo / redo, A/B, a preset, a restored session) the curve is parsed,
    compiled and published to the DSP synchronously, on the thread that changed the tree (never the audio thread).
    That keeps offline renders right after setStateInformation() correct without a message loop. Each actual change
    also calls onStoredCurveChanged, which the processor turns into one (coalesced) host notice that the project changed.
*/
class CustomCurveStore : private juce::ValueTree::Listener
{
public:
    CustomCurveStore (juce::AudioProcessorValueTreeState& state, std::array<ClipDistortion*, 2> units);
    ~CustomCurveStore() override;

    /** True when a (parsable) curve is stored for the unit (stage 1 or 2). */
    bool has (int stage) const;
    /** The stored curve (a straight line when there is none). */
    FreeCurve get (int stage) const;
    /** The stored text ("" when there is none). */
    juce::String text (int stage) const;
    /** Stores a curve (nullptr removes it). Undoable through um. */
    void set (int stage, const FreeCurve* c, juce::UndoManager* um);
    /** Removes every stored curve. */
    void clearAll (juce::UndoManager* um);
    /** Both stored texts, for the preset "modified" marker. */
    juce::String fingerprint() const;

    void rebuild (int stage);
    void rebuildAll();

    /** Called (on the thread that changed the tree) whenever a unit's stored curve text actually changes: an edit,
        undo / redo, A/B, a preset or a restored session. The processor uses it to tell the host that the project
        changed, since the curves are not host parameters. */
    std::function<void()> onStoredCurveChanged;

private:
    juce::ValueTree node() const { return apvts.state.getChildWithName (ID::curvesNode); }
    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    juce::AudioProcessorValueTreeState& apvts;
    std::array<ClipDistortion*, 2> clips;
    std::array<juce::String, 2> published;   // the text each unit's DSP curve was last built from

    JUCE_DECLARE_NON_COPYABLE (CustomCurveStore)
};
