#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace ID
{
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

    // Clip stages: prefix "d1" / "d2" + suffix
    inline juce::String clip (int stage, const char* suffix) { return "d" + juce::String (stage) + suffix; }
    // EQ bands: "b1Freq" ... "b6Slope"
    inline juce::String band (int bandIndex, const char* suffix) { return "b" + juce::String (bandIndex + 1) + suffix; }
}

namespace Params
{
    constexpr int numBands = 6;
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
