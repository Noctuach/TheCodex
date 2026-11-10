#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace ID
{
    // Source / engine
    inline const juce::String engine     { "engine" };
    inline const juce::String model909   { "model909" };
    inline const juce::String tune909    { "tune909" };
    inline const juce::String attack909  { "attack909" };
    inline const juce::String decay909   { "decay909" };

    // Key
    inline const juce::String key        { "key" };
    inline const juce::String octave     { "octave" };
    inline const juce::String keyMode    { "keyMode" };

    // Oscillator
    inline const juce::String wave       { "wave" };
    inline const juce::String tune       { "tune" };
    inline const juce::String phase      { "phase" };
    inline const juce::String clickLevel { "clickLevel" };
    inline const juce::String clickDecay { "clickDecay" };
    inline const juce::String clickTone  { "clickTone" };

    // Pitch envelope
    inline const juce::String pAmount  { "pAmount" };
    inline const juce::String pAttack  { "pAttack" };
    inline const juce::String pDecay   { "pDecay" };
    inline const juce::String pSustain { "pSustain" };
    inline const juce::String pRelease { "pRelease" };
    inline const juce::String pCurve   { "pCurve" };

    // Amp envelope
    inline const juce::String aAttack  { "aAttack" };
    inline const juce::String aDecay   { "aDecay" };
    inline const juce::String aSustain { "aSustain" };
    inline const juce::String aRelease { "aRelease" };
    inline const juce::String aCurve   { "aCurve" };

    // Output
    inline const juce::String outGain { "outGain" };
    // EQ: scales the gain of all bands (Bell, shelves, tilts)
    inline const juce::String eqScale { "eqScale" };

    // Clip stages: prefix "d1" / "d2" + suffix
    inline juce::String clip (int stage, const char* suffix) { return "d" + juce::String (stage) + suffix; }
    // EQ bands: "b1Freq" ... "b6Slope"
    inline juce::String band (int bandIndex, const char* suffix) { return "b" + juce::String (bandIndex + 1) + suffix; }
    // Saturation: global "satOn", "satBands", ... and per band "s1Style" ... "s6Bypass"
    inline juce::String sat (const char* suffix) { return juce::String ("sat") + suffix; }
    inline juce::String satBand (int bandIndex, const char* suffix) { return "s" + juce::String (bandIndex + 1) + suffix; }
}

namespace Params
{
    constexpr int numBands = 6;
    constexpr int keyBaseNote = 29;  // F1: key index 0 (F) .. 11 (E) -> F1 .. E2
    juce::StringArray keyNames();     // "F", "F#" ... "E"
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
