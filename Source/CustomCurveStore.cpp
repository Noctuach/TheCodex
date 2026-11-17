#include "CustomCurveStore.h"
#include "Parameters.h"

CustomCurveStore::CustomCurveStore (juce::AudioProcessorValueTreeState& state, std::array<ClipDistortion*, 2> units)
    : apvts (state), clips (units)
{
    apvts.state.addListener (this);
    rebuildAll();
}

CustomCurveStore::~CustomCurveStore()
{
    apvts.state.removeListener (this);
}

juce::String CustomCurveStore::text (int stage) const
{
    const auto n = node();
    return n.isValid() ? n.getProperty (ID::curveProp (stage)).toString() : juce::String();
}

bool CustomCurveStore::has (int stage) const
{
    FreeCurve c;
    const auto t = text (stage);
    return t.isNotEmpty() && FreeCurve::fromString (t, c);
}

FreeCurve CustomCurveStore::get (int stage) const
{
    FreeCurve c;
    if (! FreeCurve::fromString (text (stage), c)) c = FreeCurve::linear();
    return c;
}

void CustomCurveStore::set (int stage, const FreeCurve* c, juce::UndoManager* um)
{
    const auto prop = ID::curveProp (stage);
    if (c != nullptr)
    {
        auto n = apvts.state.getOrCreateChildWithName (ID::curvesNode, um);
        const auto t = c->toString();
        if (n.getProperty (prop).toString() != t) n.setProperty (prop, t, um);
    }
    else if (auto n = node(); n.isValid() && n.hasProperty (prop))
        n.removeProperty (prop, um);
}

void CustomCurveStore::clearAll (juce::UndoManager* um)
{
    if (auto n = node(); n.isValid())
        apvts.state.removeChild (n, um);
}

juce::String CustomCurveStore::fingerprint() const
{
    return text (1) + "#" + text (2);
}

void CustomCurveStore::rebuild (int stage)
{
    FreeCurve c;
    const auto t = text (stage);
    const bool ok = t.isNotEmpty() && FreeCurve::fromString (t, c);
    const auto idx = (size_t) juce::jlimit (0, 1, stage - 1);
    if (auto* clip = clips[idx])
        clip->publishCustomCurve (ok ? &c : nullptr);
    if (t != published[idx])
    {
        published[idx] = t;
        if (onStoredCurveChanged) onStoredCurveChanged();
    }
}

void CustomCurveStore::rebuildAll()
{
    rebuild (1);
    rebuild (2);
}

void CustomCurveStore::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (! t.hasType (ID::curvesNode)) return;   // parameter values: one comparison
    if (p == ID::curveProp (1)) rebuild (1);
    else if (p == ID::curveProp (2)) rebuild (2);
}

void CustomCurveStore::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c)
{
    if (c.hasType (ID::curvesNode)) rebuildAll();
}

void CustomCurveStore::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int)
{
    if (c.hasType (ID::curvesNode)) rebuildAll();
}

void CustomCurveStore::valueTreeRedirected (juce::ValueTree&)
{
    rebuildAll();
}
