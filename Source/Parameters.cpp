#include "Parameters.h"
#include "DSP/ClipDistortion.h"
#include "DSP/Kick909.h"

using namespace juce;

namespace
{
    using Layout = AudioProcessorValueTreeState::ParameterLayout;

    NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    AudioParameterFloatAttributes withText (std::function<String (float)> fn)
    {
        return AudioParameterFloatAttributes().withStringFromValueFunction ([fn] (float v, int) { return fn (v); });
    }

    String msText (float v)    { return v < 10.0f ? String (v, 2) + " ms" : String (v, 1) + " ms"; }
    String hzText (float v)    { return v < 1000.0f ? String (v, 1) + " Hz" : String (v / 1000.0f, 2) + " kHz"; }
    String dbText (float v)    { return String (v, 1) + " dB"; }
    String pctText (float v)   { return String (roundToInt (v * 100.0f)) + " %"; }
    String stText (float v)    { return String (v, 2) + " st"; }

    void addFloat (Layout& l, const String& id, const String& name, NormalisableRange<float> range, float def,
                   std::function<String (float)> text)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def, withText (std::move (text))));
    }

    void addChoice (Layout& l, const String& id, const String& name, StringArray choices, int def)
    {
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
    }

    void addBool (Layout& l, const String& id, const String& name, bool def)
    {
        l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    }
}

StringArray Params::keyNames()
{
    return { "F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E" };
}

AudioProcessorValueTreeState::ParameterLayout Params::createLayout()
{
    Layout l;

    // ---------------- Key ----------------
    addChoice (l, ID::key, "Key", keyNames(), 0);
    addChoice (l, ID::octave, "Octave", { "-1", "0", "+1" }, 1);
    addChoice (l, ID::keyMode, "Key Mode", { "Fixed Key", "Follow MIDI" }, 0);

    // ---------------- Engine ----------------
    addChoice (l, ID::engine, "Engine", { "Synth", "909" }, 0);
    StringArray models;
    for (const auto& m : kick909Models()) models.add (m.name);
    addChoice (l, ID::model909, "909 Model", models, 0);
    addFloat (l, ID::tune909, "909 Tune", { 0.0f, 1.0f }, 0.5f, pctText);
    addFloat (l, ID::attack909, "909 Attack", { 0.0f, 1.0f }, 0.5f, pctText);
    addFloat (l, ID::decay909, "909 Decay", { 0.0f, 1.0f }, 0.5f, pctText);

    // ---------------- Oscillator ----------------
    addChoice (l, ID::wave, "Wave", { "Sine", "Triangle", "Warm Sine" }, 0);
    addFloat (l, ID::tune, "Tune", { -24.0f, 24.0f, 0.01f }, 0.0f, stText);
    addFloat (l, ID::phase, "Start Phase", { 0.0f, 360.0f, 1.0f }, 0.0f,
              [] (float v) { return String (roundToInt (v)) + juce::String (CharPointer_UTF8 ("\xc2\xb0")); });
    addFloat (l, ID::clickLevel, "Click Level", { 0.0f, 1.0f }, 0.15f, pctText);
    addFloat (l, ID::clickDecay, "Click Decay", skewed (0.5f, 50.0f, 6.0f), 4.0f, msText);
    addFloat (l, ID::clickTone, "Click Tone", skewed (500.0f, 16000.0f, 3000.0f), 3500.0f, hzText);

    // ---------------- Pitch envelope ----------------
    addFloat (l, ID::pAmount, "Pitch Amount", { 0.0f, 96.0f, 0.01f }, 48.0f, stText);
    addFloat (l, ID::pAttack, "Pitch Attack", skewed (0.0f, 200.0f, 10.0f), 0.0f, msText);
    addFloat (l, ID::pDecay, "Pitch Decay", skewed (1.0f, 2000.0f, 150.0f), 120.0f, msText);
    addFloat (l, ID::pSustain, "Pitch Sustain", { 0.0f, 1.0f }, 0.0f, pctText);
    addFloat (l, ID::pRelease, "Pitch Release", skewed (1.0f, 2000.0f, 150.0f), 80.0f, msText);
    addFloat (l, ID::pCurve, "Pitch Curve", skewed (1.0f, 10.0f, 3.0f), 4.0f, [] (float v) { return String (v, 2); });

    // ---------------- Amp envelope ----------------
    addFloat (l, ID::aAttack, "Amp Attack", skewed (0.0f, 200.0f, 10.0f), 0.5f, msText);
    addFloat (l, ID::aDecay, "Amp Decay", skewed (1.0f, 5000.0f, 400.0f), 600.0f, msText);
    addFloat (l, ID::aSustain, "Amp Sustain", { 0.0f, 1.0f }, 0.7f, pctText);
    addFloat (l, ID::aRelease, "Amp Release", skewed (1.0f, 5000.0f, 400.0f), 150.0f, msText);
    addFloat (l, ID::aCurve, "Amp Curve", skewed (1.0f, 10.0f, 3.0f), 2.0f, [] (float v) { return String (v, 2); });

    // ---------------- Clip distortion stages ----------------
    const StringArray clipModes = ClipDistortion::modeNames();
    for (int stage = 1; stage <= 2; ++stage)
    {
        const String n = "Dist " + String (stage) + " ";
        addBool (l, ID::clip (stage, "On"), n + "On", true);
        addChoice (l, ID::clip (stage, "Mode"), n + "Type", clipModes, stage == 1 ? 2 : 0);
        addFloat (l, ID::clip (stage, "Drive"), n + "Drive", { 0.0f, 48.0f, 0.01f }, stage == 1 ? 18.0f : 6.0f, dbText);
        addFloat (l, ID::clip (stage, "Boost"), n + "Boost", { 0.0f, 18.0f, 0.01f }, 0.0f, dbText);
        addFloat (l, ID::clip (stage, "Char"), n + "Characteristic", { 0.0f, 1.0f }, 0.0f, pctText);
        addFloat (l, ID::clip (stage, "Bias"), n + "Offset", { -1.0f, 1.0f, 0.001f }, 0.0f, [] (float v) { return String (v, 2); });
        addFloat (l, ID::clip (stage, "LoFilt"), n + "Lo Filter", skewed (200.0f, 20000.0f, 3000.0f), 20000.0f,
                  [] (float v) { return v >= 19900.0f ? String ("Off") : hzText (v); });
        addFloat (l, ID::clip (stage, "HiFilt"), n + "Hi Filter", skewed (20.0f, 2000.0f, 150.0f), 20.0f,
                  [] (float v) { return v <= 20.5f ? String ("Off") : hzText (v); });
        addFloat (l, ID::clip (stage, "ShelfF"), n + "Shelf Freq", skewed (1000.0f, 16000.0f, 5000.0f), 6000.0f, hzText);
        addFloat (l, ID::clip (stage, "ShelfG"), n + "Shelf Gain", { -18.0f, 18.0f, 0.01f }, 0.0f, dbText);
        addFloat (l, ID::clip (stage, "Mix"), n + "Mix", { 0.0f, 1.0f }, 1.0f, pctText);
        addFloat (l, ID::clip (stage, "Out"), n + "Output", { -24.0f, 12.0f, 0.01f }, -6.0f, dbText);
        addBool (l, ID::clip (stage, "DC"), n + "DC Filter", true);
        addChoice (l, ID::clip (stage, "OS"), n + "Oversampling", ClipDistortion::oversamplingNames(), 2);
    }

    // ---------------- EQ ----------------
    const StringArray bandTypes { "Low Cut", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut" };
    const StringArray slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    const int   defTypes[Params::numBands] = { 0, 1, 2, 2, 2, 5 };
    const float defFreqs[Params::numBands] = { 30.0f, 60.0f, 200.0f, 1000.0f, 4000.0f, 16000.0f };

    for (int b = 0; b < Params::numBands; ++b)
    {
        const String n = "Band " + String (b + 1) + " ";
        addBool (l, ID::band (b, "On"), n + "On", false);
        addChoice (l, ID::band (b, "Type"), n + "Type", bandTypes, defTypes[b]);
        addFloat (l, ID::band (b, "Freq"), n + "Freq", skewed (20.0f, 20000.0f, 640.0f), defFreqs[b], hzText);
        addFloat (l, ID::band (b, "Gain"), n + "Gain", { -24.0f, 24.0f, 0.01f }, 0.0f, dbText);
        addFloat (l, ID::band (b, "Q"), n + "Q", skewed (0.1f, 18.0f, 1.0f), 0.707f, [] (float v) { return String (v, 2); });
        addChoice (l, ID::band (b, "Slope"), n + "Slope", slopes, 1);
    }

    // ---------------- Output ----------------
    addFloat (l, ID::outGain, "Output", { -24.0f, 12.0f, 0.01f }, -3.0f, dbText);

    return l;
}
