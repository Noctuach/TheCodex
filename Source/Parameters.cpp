#include "Parameters.h"
#include "DSP/ClipDistortion.h"
#include "DSP/ParametricEQ.h"
#include "DSP/Kick909.h"
#include "DSP/MultibandSaturator.h"
#include "DSP/SpaceReverb.h"
#include "DSP/Compressor.h"
#include "DSP/EqEngine.h"
#include "DSP/DrawEnvelope.h"
#include "DSP/Chain.h"

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
    addChoice (l, ID::keyMode, "Key Mode", { "Fixed Key", "Follow MIDI" }, 1);   // Follow MIDI: the DAW's typing keyboard / piano roll pick the note

    // ---------------- Engine ----------------
    addChoice (l, ID::engine, "Engine", { "Synth", "909" }, 0);
    StringArray models;
    for (const auto& m : kick909Models()) models.add (m.name);
    addChoice (l, ID::model909, "909 Model", models, 0);
    addFloat (l, ID::tune909, "909 Tune Depth", { 0.0f, 1.0f }, 0.5f, pctText);
    addFloat (l, ID::tuneTime909, "909 Tune", { 0.0f, 1.0f }, 0.5f, pctText);
    addFloat (l, ID::accent909, "909 Accent", { 0.0f, 1.0f }, 0.0f, pctText);
    addFloat (l, ID::overload909, "909 Overload", { 0.0f, 1.0f }, 0.0f, pctText);
    addBool (l, ID::hold909, "909 Decay Hold", false);
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

    // ---------------- Macros ----------------
    for (int m = 0; m < ID::numMacros; ++m)
        addFloat (l, ID::macro (m), "Macro " + ID::macroName (m), { 0.0f, 1.0f }, 0.5f,
                  [] (float v) { const int p = roundToInt ((v - 0.5f) * 200.0f); return (p > 0 ? "+" : "") + String (p) + " %"; });

    // ---------------- Drawn envelopes (breakpoints) ----------------
    {
        // defaults: a classic hardstyle pitch drop, and a full-body amp curve
        const float pT[] = { 0, 25, 110, 320, 600, 900, 1200, 1500 }, pL[] = { 1.0f, 0.42f, 0.1f, 0.0f, 0, 0, 0, 0 }, pB[] = { 0.55f, 0.45f, 0.3f, 0, 0, 0, 0, 0 };
        const float aT[] = { 0, 1, 450, 1100, 1400, 1700, 2000, 2300 }, aL[] = { 0.0f, 1.0f, 0.82f, 0.0f, 0, 0, 0, 0 }, aB[] = { 0, 0.15f, 0.35f, 0, 0, 0, 0, 0 };
        for (char env : { 'p', 'a' })
        {
            const String n = env == 'p' ? "Pitch Env " : "Amp Env ";
            addChoice (l, ID::envMode (env), n + "Mode", { "ADSR", "Draw" }, 0);
            l.add (std::make_unique<AudioParameterInt> (ParameterID { ID::envPoints (env), 1 }, n + "Points", 2, DrawEnvelope::maxPoints, env == 'p' ? 4 : 4));
            for (int i = 0; i < DrawEnvelope::maxPoints; ++i)
            {
                const String pn = n + "Point " + String (i + 1) + " ";
                addFloat (l, ID::envPoint (env, 'T', i), pn + "Time", skewed (0.0f, 4000.0f, 300.0f), env == 'p' ? pT[i] : aT[i], msText);
                addFloat (l, ID::envPoint (env, 'L', i), pn + "Level", { 0.0f, 1.0f }, env == 'p' ? pL[i] : aL[i], pctText);
                addFloat (l, ID::envPoint (env, 'B', i), pn + "Bend", { -1.0f, 1.0f }, env == 'p' ? pB[i] : aB[i], [] (float v) { return String (v, 2); });
            }
        }
    }

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
        addBool (l, ID::clip (stage, "AutoGain"), n + "Auto Gain", false);
        addChoice (l, ID::clip (stage, "OS"), n + "Oversampling", ClipDistortion::oversamplingNames(), 2);
        // Resonant filter before or after the curve
        addChoice (l, ID::clip (stage, "FltPos"), n + "Filter Position", ClipDistortion::filterPositionNames(), 0);
        addChoice (l, ID::clip (stage, "FltType"), n + "Filter Type", ClipDistortion::filterTypeNames(), 0);
        addFloat (l, ID::clip (stage, "FltFreq"), n + "Filter Freq", skewed (20.0f, 20000.0f, 640.0f), 1000.0f, hzText);
        addFloat (l, ID::clip (stage, "FltRes"), n + "Filter Resonance", { 0.0f, 1.0f }, 0.2f, pctText);
        // Punch / tail split
        addFloat (l, ID::clip (stage, "TailDrv"), n + "Tail Drive", { -24.0f, 24.0f, 0.01f }, 0.0f, dbText);
        addFloat (l, ID::clip (stage, "Split"), n + "Punch / Tail Split", skewed (5.0f, 400.0f, 60.0f), 60.0f, msText);
        // X-Shaper curve points (positive half; negative half used by X-Shaper Asym)
        for (int k = 0; k < ClipDistortion::numCurvePoints; ++k)
        {
            const float def = (float) (k + 1) / (float) ClipDistortion::numCurvePoints;
            addFloat (l, ID::clip (stage, ("XP" + String (k + 1)).toRawUTF8()), n + "X-Shaper +" + String (k + 1), { -1.0f, 1.0f, 0.001f }, def,
                      [] (float v) { return String (v, 3); });
            addFloat (l, ID::clip (stage, ("XN" + String (k + 1)).toRawUTF8()), n + "X-Shaper -" + String (k + 1), { -1.0f, 1.0f, 0.001f }, def,
                      [] (float v) { return String (v, 3); });
        }
        addFloat (l, ID::clip (stage, "Spatial"), n + "Spatial", { 0.0f, 1.0f }, 0.0f, pctText);
    }

    // ---------------- EQ ----------------
    StringArray bandTypes, slopes;
    for (int t = 0; t < eq::numBandTypes; ++t) bandTypes.add (eq::bandTypeNames()[t]);
    for (int i = 0; i < eq::numSlopes; ++i) slopes.add (String (eq::slopeDb[i]) + " dB/oct");
    // Bands 1-6 keep their classic defaults; bands 7-24 start as bells
    const int   defTypes[] = { 0, 1, 2, 2, 2, 5 };
    const float defFreqs[] = { 30.0f, 60.0f, 200.0f, 1000.0f, 4000.0f, 16000.0f };

    for (int e = 0; e < ID::numEqs; ++e)
    {
        const String eqName = e == 0 ? "" : "EQ 2 ";
        for (int b = 0; b < Params::numBands; ++b)
        {
            const String n = eqName + "Band " + String (b + 1) + " ";
            const bool classic = e == 0 && b < 6;   // EQ 1 bands 1-6 keep their original defaults
            // "Used": the band exists on the display (it can still be bypassed with "On").
            // A band counts as present when Used or On is set, so older states without Used still load.
            addBool (l, ID::eqBand (e, b, "Used"), n + "Used", false);
            addBool (l, ID::eqBand (e, b, "On"), n + "On", false);
            addChoice (l, ID::eqBand (e, b, "Type"), n + "Type", bandTypes, classic ? defTypes[b] : 2);
            addFloat (l, ID::eqBand (e, b, "Freq"), n + "Freq", skewed (20.0f, 20000.0f, 640.0f), classic ? defFreqs[b] : 1000.0f, hzText);
            addFloat (l, ID::eqBand (e, b, "Gain"), n + "Gain", { -30.0f, 30.0f, 0.01f }, 0.0f, dbText);
            addFloat (l, ID::eqBand (e, b, "Q"), n + "Q", skewed (0.1f, 18.0f, 1.0f), 0.707f, [] (float v) { return String (v, 2); });
            addChoice (l, ID::eqBand (e, b, "Slope"), n + "Slope", slopes, eq::defaultSlope);
            addChoice (l, ID::eqBand (e, b, "Place"), n + "Stereo Placement", { "Stereo", "Left", "Right", "Mid", "Side" }, 0);
        }
        const String g = e == 0 ? "EQ " : "EQ 2 ";
        addFloat (l, ID::eqScaleFor (e), g + "Gain Scale", { 0.0f, 2.0f }, 1.0f, pctText);
        addBool (l, ID::eqOn (e), g + "On", true);
        addChoice (l, ID::eqMode (e), g + "Processing Mode", eq::processingModeNames(), 0);
        addChoice (l, ID::eqRes (e), g + "Linear Phase Resolution", eq::resolutionNames(), 1);
    }

    // ---------------- Saturation (multiband) ----------------
    addBool (l, ID::sat ("On"), "Saturation On", false);
    l.add (std::make_unique<AudioParameterInt> (ParameterID { ID::sat ("Bands"), 1 }, "Saturation Bands", 1, sat::maxBands, 3));
    addChoice (l, ID::sat ("Slope"), "Saturation Crossover Slope", sat::slopeNames(), 2);
    addChoice (l, ID::sat ("OS"), "Saturation Oversampling", sat::oversamplingNames(), 1);
    addFloat (l, ID::sat ("Mix"), "Saturation Mix", { 0.0f, 1.0f }, 1.0f, pctText);
    addFloat (l, ID::sat ("Out"), "Saturation Output", { -24.0f, 12.0f, 0.01f }, 0.0f, dbText);
    addFloat (l, ID::sat ("In"), "Saturation Input", { -24.0f, 24.0f, 0.01f }, 0.0f, dbText);
    addChoice (l, ID::sat ("Mode"), "Saturation Channel Mode", sat::channelModeNames(), 0);
    {
        const float defX[] = { 120.0f, 400.0f, 1200.0f, 3500.0f, 9000.0f };
        for (int j = 0; j < sat::maxBands - 1; ++j)
            addFloat (l, ID::sat (("X" + String (j + 1)).toRawUTF8()), "Saturation Crossover " + String (j + 1),
                      skewed (30.0f, 16000.0f, 700.0f), defX[j], hzText);
    }
    StringArray styleNames;
    for (const auto& st : sat::styles()) styleNames.add (st.name);
    const int defStyles[] = { 1, 1, 4, 3, 8, 0 };
    for (int b = 0; b < sat::maxBands; ++b)
    {
        const String n = "Sat Band " + String (b + 1) + " ";
        addChoice (l, ID::satBand (b, "Style"), n + "Style", styleNames, defStyles[b]);
        addFloat (l, ID::satBand (b, "Drive"), n + "Drive", { 0.0f, 36.0f, 0.01f }, 6.0f, dbText);
        addFloat (l, ID::satBand (b, "Mix"), n + "Mix", { 0.0f, 1.0f }, 1.0f, pctText);
        addFloat (l, ID::satBand (b, "Fb"), n + "Feedback", { 0.0f, 1.0f }, 0.0f, pctText);
        addFloat (l, ID::satBand (b, "FbFreq"), n + "Feedback Freq", skewed (20.0f, 20000.0f, 640.0f), 1000.0f, hzText);
        addFloat (l, ID::satBand (b, "Dyn"), n + "Dynamics", { -1.0f, 1.0f, 0.001f }, 0.0f, [] (float v) { return String (roundToInt (v * 100.0f)) + " %"; });
        for (auto [sfx, name] : { std::pair<const char*, const char*> { "Bass", "Bass" }, { "MidT", "Mid" }, { "Treble", "Treble" }, { "Pres", "Presence" } })
            addFloat (l, ID::satBand (b, sfx), n + name, { -12.0f, 12.0f, 0.01f }, 0.0f, dbText);
        auto panText = [] (float v) { return std::abs (v) < 0.005f ? String ("C") : (v < 0 ? "L " : "R ") + String (roundToInt (std::abs (v) * 100.0f)); };
        addFloat (l, ID::satBand (b, "DrivePan"), n + "Drive Pan", { -1.0f, 1.0f, 0.001f }, 0.0f, panText);
        addFloat (l, ID::satBand (b, "LevelPan"), n + "Level Pan", { -1.0f, 1.0f, 0.001f }, 0.0f, panText);
        addFloat (l, ID::satBand (b, "Level"), n + "Level", { -24.0f, 12.0f, 0.01f }, 0.0f, dbText);
        addBool (l, ID::satBand (b, "Mute"), n + "Mute", false);
        addBool (l, ID::satBand (b, "Solo"), n + "Solo", false);
        addBool (l, ID::satBand (b, "Bypass"), n + "Bypass", false);
    }

    // ---------------- Compressor (Pro-C 3 / LA-3A inspired, multiband, up/down) ----------------
    {
        addBool (l, ID::cmp ("On"), "Compressor On", false);
        l.add (std::make_unique<AudioParameterInt> (ParameterID { ID::cmp ("Bands"), 1 }, "Compressor Bands", 1, comp::maxBands, 1));
        addChoice (l, ID::cmp ("Style"), "Compressor Style", comp::styleNames(), comp::Punch);
        addChoice (l, ID::cmp ("Look"), "Compressor Lookahead", comp::lookaheadNames(), 0);
        addFloat (l, ID::cmp ("Hold"), "Compressor Hold", skewed (0.0f, 500.0f, 50.0f), 0.0f, msText);
        addFloat (l, ID::cmp ("ScHp"), "Compressor Side Chain High-Pass", skewed (20.0f, 500.0f, 100.0f), 20.0f,
                  [] (float v) { return v <= 20.5f ? String ("Off") : String (v, 0) + " Hz"; });
        addFloat (l, ID::cmp ("Mix"), "Compressor Mix", { 0.0f, 1.0f }, 1.0f, pctText);
        addFloat (l, ID::cmp ("Out"), "Compressor Output", { -24.0f, 24.0f, 0.01f }, 0.0f, dbText);
        addBool (l, ID::cmp ("Auto"), "Compressor Auto Gain", true);
        addChoice (l, ID::cmp ("Limit"), "Compressor T4 Mode", { "Compress", "Limit" }, 0);
        const float defX[] = { 120.0f, 1000.0f, 6000.0f };
        for (int j = 0; j < comp::maxBands - 1; ++j)
            addFloat (l, ID::cmp (("X" + String (j + 1)).toRawUTF8()), "Compressor Crossover " + String (j + 1),
                      skewed (20.0f, 20000.0f, 640.0f), defX[j], hzText);
        auto ratioText = [] (float v) { return v >= 99.5f ? String (CharPointer_UTF8 ("\xe2\x88\x9e : 1")) : String (v, v < 10.0f ? 1 : 0) + " : 1"; };
        for (int b = 0; b < comp::maxBands; ++b)
        {
            const String n = "Compressor Band " + String (b + 1) + " ";
            addChoice (l, ID::cmpBand (b, "Dir"), n + "Direction", comp::directionNames(), 0);
            addFloat (l, ID::cmpBand (b, "Thr"), n + "Threshold", { -60.0f, 0.0f, 0.01f }, -18.0f, dbText);
            addFloat (l, ID::cmpBand (b, "Ratio"), n + "Ratio", skewed (1.0f, 100.0f, 4.0f), 4.0f, ratioText);
            addFloat (l, ID::cmpBand (b, "Knee"), n + "Knee", { 0.0f, 48.0f, 0.01f }, 6.0f, dbText);
            addFloat (l, ID::cmpBand (b, "Range"), n + "Range", { 0.0f, 60.0f, 0.01f }, 30.0f, dbText);
            addFloat (l, ID::cmpBand (b, "Att"), n + "Attack", skewed (0.01f, 250.0f, 10.0f), 5.0f, msText);
            addFloat (l, ID::cmpBand (b, "Rel"), n + "Release", skewed (5.0f, 2500.0f, 150.0f), 120.0f, msText);
            addFloat (l, ID::cmpBand (b, "Gain"), n + "Gain", { -24.0f, 24.0f, 0.01f }, 0.0f, dbText);
            addBool (l, ID::cmpBand (b, "Mute"), n + "Mute", false);
            addBool (l, ID::cmpBand (b, "Solo"), n + "Solo", false);
            addBool (l, ID::cmpBand (b, "Bypass"), n + "Bypass", false);
        }
    }

    // ---------------- Space (reverb, ValhallaRoom-style controls) ----------------
    {
        auto secText = [] (float v) { return v < 10.0f ? String (v, 2) + " s" : String (v, 1) + " s"; };
        auto multText = [] (float v) { return String (v, 2) + "x"; };
        auto hzText2 = [] (float v) { return v < 1000.0f ? String (v, 0) + " Hz" : String (v / 1000.0f, 2) + " kHz"; };
        auto rateText = [] (float v) { return String (0.05f + 4.95f * v * v, 2) + " Hz"; };
        auto addPct = [&] (const char* sfx, const String& name, float def)
        { addFloat (l, ID::spc (sfx), "Space " + name, { 0.0f, 1.0f }, def, pctText); };

        addBool (l, ID::spc ("On"), "Space On", false);
        addChoice (l, ID::spc ("Mode"), "Space Mode", space::modeNames(), 1);
        addPct ("Mix", "Mix", 0.2f);
        addFloat (l, ID::spc ("PreDelay"), "Space Predelay", skewed (0.0f, 500.0f, 60.0f), 10.0f, msText);
        addFloat (l, ID::spc ("Decay"), "Space Decay", skewed (0.1f, 20.0f, 2.0f), 1.2f, secText);
        addFloat (l, ID::spc ("HighCut"), "Space High Cut", skewed (1000.0f, 20000.0f, 6000.0f), 8000.0f, hzText2);
        addFloat (l, ID::spc ("LowCut"), "Space Low Cut", skewed (20.0f, 1000.0f, 200.0f), 200.0f, hzText2);
        addPct ("Depth", "Depth", 0.6f);
        addFloat (l, ID::spc ("Width"), "Space Width", { 0.0f, 2.0f }, 1.0f, pctText);
        addPct ("Duck", "Ducking", 0.3f);

        addPct ("ESize", "Early Size", 0.5f);
        addPct ("ECross", "Early Cross", 0.3f);
        addPct ("ESend", "Early Send", 0.5f);
        addPct ("EDiff", "Early Diffusion", 0.6f);
        addFloat (l, ID::spc ("EModRate"), "Space Early Mod Rate", { 0.0f, 1.0f }, 0.4f, rateText);
        addPct ("EModDepth", "Early Mod Depth", 0.3f);

        addPct ("LSize", "Late Size", 0.5f);
        addPct ("LCross", "Late Cross", 0.4f);
        addPct ("LDiff", "Late Diffusion", 0.7f);
        addFloat (l, ID::spc ("LModRate"), "Space Late Mod Rate", { 0.0f, 1.0f }, 0.35f, rateText);
        addPct ("LModDepth", "Late Mod Depth", 0.3f);

        addFloat (l, ID::spc ("BassMult"), "Space Bass Mult", skewed (0.25f, 4.0f, 1.0f), 0.75f, multText);
        addFloat (l, ID::spc ("BassX"), "Space Bass Xover", skewed (50.0f, 2000.0f, 300.0f), 250.0f, hzText2);
        addFloat (l, ID::spc ("HighMult"), "Space High Mult", skewed (0.1f, 2.0f, 0.75f), 0.6f, multText);
        addFloat (l, ID::spc ("HighX"), "Space High Xover", skewed (1000.0f, 16000.0f, 4500.0f), 4000.0f, hzText2);
    }

    // ---------------- Output ----------------
    addFloat (l, ID::outGain, "Output", { -24.0f, 12.0f, 0.01f }, -3.0f, dbText);
    l.add (std::make_unique<AudioParameterInt> (ParameterID { ID::chainOrder, 1 }, "Chain Order", 0, chain::numOrders - 1, chain::modernIndex(),
                                                AudioParameterIntAttributes().withAutomatable (false)
                                                    .withStringFromValueFunction ([] (int v, int) { return chain::describe (v); })));
    addBool (l, ID::subOn, "Sub Layer On", false);
    addFloat (l, ID::subLevel, "Sub Layer Level", { -36.0f, 6.0f, 0.01f }, -6.0f, dbText);
    addFloat (l, ID::subLowPass, "Sub Layer Low-Pass", skewed (30.0f, 300.0f, 100.0f), 110.0f, hzText);
    addFloat (l, ID::subFade, "Sub Layer Fade-In", skewed (0.0f, 400.0f, 60.0f), 60.0f, msText);
    addFloat (l, ID::subPhase, "Sub Layer Phase", { 0.0f, 360.0f, 1.0f }, 0.0f, [] (float v) { return String (roundToInt (v)) + String (CharPointer_UTF8 ("\xc2\xb0")); });
    addFloat (l, ID::velocity, "Velocity Sensitivity", { 0.0f, 1.0f }, 0.0f, pctText);
    addBool (l, ID::clipOn, "Output Clipper On", false);
    addFloat (l, ID::clipCeiling, "Output Clipper Ceiling", { -12.0f, 0.0f, 0.01f }, -0.3f, dbText);
    addFloat (l, ID::clipSoft, "Output Clipper Softness", { 0.0f, 1.0f }, 0.3f, pctText);

    return l;
}
