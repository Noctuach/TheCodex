#pragma once
#include <juce_core/juce_core.h>
#include "../Parameters.h"

/** Tooltip text for every parameter: "Title: explanation". */
namespace Help
{
    inline juce::String forParameter (const juce::String& id)
    {
        static const std::map<juce::String, juce::String> tips {
            { ID::engine,     "Engine: SYNTH builds the kick from an oscillator and two ADSR envelopes. 909 is a synthesized 909-style bass drum with its own pitch sweep." },
            { ID::model909,   "909 Model: Starting character for the 909 engine, from a stock 909 to gabber and early-hardstyle variations." },
            { ID::tune909,    "909 Tune: How far the pitch sweeps down at the start of the hit, like the Tune knob on the original machine. Does not change the root key." },
            { ID::attack909,  "909 Attack: Level of the click at the start of the hit (pulse + noise burst)." },
            { ID::decay909,   "909 Decay: Length of the 909's own volume decay." },
            { ID::key,        "Key: Root note of the kick, F to E (F1 = 43.65 Hz up to E2 = 82.41 Hz). Set it to the key of your track." },
            { ID::octave,     "Octave: Shifts the key down or up one octave." },
            { ID::keyMode,    "Key Mode: Fixed Key plays the selected key from any MIDI note. Follow MIDI plays the note you press instead." },
            { ID::wave,       "Wave: Oscillator shape. Sine is the clean classic; Triangle and Warm Sine add harmonics before distortion." },
            { ID::tune,       "Fine Tune: Shifts the root by semitones and cents. Keep at 0 to stay exactly on the selected key." },
            { ID::phase,      "Start Phase: Where the waveform starts on each hit. 0 degrees is the softest start; 90 degrees gives a harder, clicky start." },
            { ID::clickLevel, "Click: Level of a short noise click layered on the start of the kick." },
            { ID::clickDecay, "Click Decay: How long the click lasts." },
            { ID::clickTone,  "Click Tone: Brightness of the click (high-pass frequency of the noise)." },
            { ID::pAmount,    "Pitch Amount: How many semitones above the root the kick starts. Higher values give a harder, punchier sweep." },
            { ID::pAttack,    "Pitch Attack: Time for the pitch to rise to its peak. Usually 0 for kicks." },
            { ID::pDecay,     "Pitch Decay: How fast the pitch falls back to the root. Short = tight punch, long = boomy sweep." },
            { ID::pSustain,   "Pitch Sustain: Pitch offset held while the note is down. Keep at 0 % so the tail sits on the root key." },
            { ID::pRelease,   "Pitch Release: Pitch fall time after the note is released." },
            { ID::pCurve,     "Pitch Curve: Shape of the sweep. 1 is linear; higher values snap down faster (more 'kick', less 'zap')." },
            { ID::aAttack,    "Amp Attack: Fade-in time. Keep very short (under 1 ms) for a hard transient." },
            { ID::aDecay,     "Amp Decay: Time to fall from full level to the sustain level." },
            { ID::aSustain,   "Amp Sustain: Level held while the note is down. With the 909 engine this is fixed at 100 %." },
            { ID::aRelease,   "Amp Release: Fade-out after the note is released. The note length controls the length of the tail." },
            { ID::aCurve,     "Amp Curve: Shape of decay and release. Higher values drop faster at first." },
            { ID::outGain,    "Output: Final output level. Watch the meter: the peak marker turns red above 0 dB." },
        };
        if (auto it = tips.find (id); it != tips.end())
            return it->second;

        // Distortion stages: d1Drive, d2Mix, ...
        if (id.length() > 2 && id[0] == 'd' && (id[1] == '1' || id[1] == '2'))
        {
            const auto sfx = id.substring (2);
            static const std::map<juce::String, juce::String> dist {
                { "On",     "Distortion On/Off: Bypass this distortion stage." },
                { "Mode",   "Type: Base distortion curve. Hard Clip and Fuzz are aggressive, Soft Clip / Tanh / Overdrive are smoother, Tube and Diode are asymmetric, Folds and Rectify add extra harmonics." },
                { "Drive",  "Drive: How hard the signal hits the distortion curve." },
                { "Boost",  "Boost: Extra input gain on both the dry and the distorted signal." },
                { "Char",   "Characteristic: Morphs the curve from its normal shape (0 %) towards a square wave (100 %) for extreme distortion." },
                { "Bias",   "Offset: Shifts the operating point of the curve for asymmetric distortion (even harmonics)." },
                { "LoFilt", "Lo Filter: Low-pass on the distorted path, before the distortion. Lower it to distort only the body and keep the top end clean." },
                { "HiFilt", "Hi Filter: High-pass on the distorted signal after the distortion. Raise it to keep the sub clean while distorting the mids." },
                { "ShelfF", "Shelf Freq: Corner frequency of the high shelf on the distorted signal." },
                { "ShelfG", "Shelf Gain: Brightens or darkens the distorted signal above the shelf frequency." },
                { "Mix",    "Mix: Blend between the clean and the distorted signal." },
                { "Out",    "Output: Level after this distortion stage. Use it to control how hard the next stage is driven." },
                { "DC",     "DC Filter: Removes DC offset caused by asymmetric curves and Offset." },
                { "OS",     "Oversampling: Reduces aliasing (harsh, non-musical overtones) at the cost of CPU. 4x is a good default." },
            };
            if (auto it = dist.find (sfx); it != dist.end())
                return it->second;
        }

        // EQ bands: b1Freq, ...
        if (id.length() > 2 && id[0] == 'b')
        {
            const auto sfx = id.substring (2);
            static const std::map<juce::String, juce::String> band {
                { "On",    "Band On/Off: Enable this EQ band. Tip: double-click on the EQ display to add a band." },
                { "Type",  "Band Type: Low Cut, Low Shelf, Bell, Notch, High Shelf or High Cut." },
                { "Freq",  "Frequency: Centre / corner frequency of the band. Drag the node on the display horizontally." },
                { "Gain",  "Gain: Boost or cut. Drag the node on the display vertically." },
                { "Q",     "Q: Width of the band (higher = narrower). Use the mouse wheel over a node." },
                { "Slope", "Slope: Steepness of Low Cut / High Cut filters." },
            };
            if (auto it = band.find (sfx); it != band.end())
                return it->second;
        }
        return {};
    }
}
