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
    // Envelope mode (ADSR / Draw) and breakpoints: env = "p" (pitch) or "a" (amp); "pEnvMode", "pPts", "pT3", "pL3", "pB3"
    inline juce::String envMode (char env) { return juce::String::charToString ((juce::juce_wchar) env) + "EnvMode"; }
    inline juce::String envPoints (char env) { return juce::String::charToString ((juce::juce_wchar) env) + "Pts"; }
    inline juce::String envPoint (char env, char what, int index) { return juce::String::charToString ((juce::juce_wchar) env) + juce::String::charToString ((juce::juce_wchar) what) + juce::String (index + 1); }

    // Output
    inline const juce::String outGain { "outGain" };
    // EQ: scales the gain of all bands (Bell, shelves, tilts)
    inline const juce::String eqScale { "eqScale" };

    // Clip stages: prefix "d1" / "d2" + suffix
    inline juce::String clip (int stage, const char* suffix) { return "d" + juce::String (stage) + suffix; }
    // EQ bands: "b1Freq" ... "b6Slope"
    inline juce::String band (int bandIndex, const char* suffix) { return "b" + juce::String (bandIndex + 1) + suffix; }
    // Two equalisers: EQ 1 (between the distortions) uses "b1Freq" ..., EQ 2 (post, before Space) uses "q1Freq" ...
    constexpr int numEqs = 2;
    inline juce::String eqBand (int eq, int bandIndex, const char* suffix) { return (eq == 0 ? "b" : "q") + juce::String (bandIndex + 1) + suffix; }
    inline juce::String eqScaleFor (int eq) { return eq == 0 ? eqScale : juce::String ("eq2Scale"); }
    inline juce::String eqMode (int eq) { return eq == 0 ? juce::String ("eqMode") : juce::String ("eq2Mode"); }
    inline juce::String eqRes (int eq)  { return eq == 0 ? juce::String ("eqRes") : juce::String ("eq2Res"); }
    inline juce::String eqOn (int eq)   { return eq == 0 ? juce::String ("eqOn") : juce::String ("eq2On"); }
    // Saturation: global "satOn", "satBands", ... and per band "s1Style" ... "s6Bypass"
    inline juce::String sat (const char* suffix) { return juce::String ("sat") + suffix; }
    inline juce::String satBand (int bandIndex, const char* suffix) { return "s" + juce::String (bandIndex + 1) + suffix; }
    /** Compressor: global "cmpOn" ... and per band "c1Thr" ... */
    inline juce::String cmp (const char* suffix) { return juce::String ("cmp") + suffix; }
    inline juce::String cmpBand (int bandIndex, const char* suffix) { return "c" + juce::String (bandIndex + 1) + suffix; }
    /** Space (reverb) parameters: "spcOn", "spcDecay", ... */
    inline juce::String spc (const char* suffix) { return juce::String ("spc") + suffix; }
}

namespace Params
{
    constexpr int numBands = 24;   // same limit as Pro-Q 4
    constexpr int keyBaseNote = 29;  // F1: key index 0 (F) .. 11 (E) -> F1 .. E2
    juce::StringArray keyNames();     // "F", "F#" ... "E"
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
