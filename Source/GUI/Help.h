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
                { "Spatial", "Spatial: Distorts the left and right channels slightly differently (drive and operating point), creating stereo width from the mono kick. Use the EQ's Side placement to shape it." },
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
                { "Type",  "Shape: Bell, Low/High Shelf, Low/High Cut, Notch, Band Pass (isolate a range), Tilt Shelf (tilt around the frequency), Flat Tilt (tilt the whole spectrum) or All Pass (phase only, no level change)." },
                { "Freq",  "Frequency: Centre / corner frequency of the band. Drag the node on the display horizontally." },
                { "Gain",  "Gain: boost or cut (+/-30 dB). Drag the node vertically, or Ctrl/Cmd + mouse wheel." },
                { "Q",     "Q: width of the band (higher = narrower). Mouse wheel over a node, or Ctrl/Cmd + drag vertically." },
                { "Slope", "Slope: steepness of Low Cut, High Cut and Band Pass filters, 6 to 96 dB/oct. Tip: mouse wheel over a cut filter's node steps the slope." },
                { "Place", "Stereo Placement: which part of the stereo signal this band affects: Stereo (both channels), Left, Right, Mid (L+R, the centre) or Side (L-R, the width). Tip: a Side low cut keeps the low end mono." },
            };
            if (auto it = band.find (sfx); it != band.end())
                return it->second;
        }
        // Saturation: global "satOn" ... and per band "s1Drive" ...
        static const std::map<juce::String, juce::String> satTips {
            { "satOn",    "Saturation On/Off: multiband saturation after Distortion 2." },
            { "satBands", "Bands: number of frequency bands, each with its own saturation." },
            { "satSlope", "Crossover Slope: how steeply the bands are separated. Steeper = more independent bands; 6 dB/oct blends most smoothly." },
            { "satOS",    "Oversampling: reduces aliasing from the saturation at the cost of CPU." },
            { "satMix",   "Mix: blend between the signal before and after the saturation stage." },
            { "satOut",   "Output: level after the saturation stage." },
        };
        if (auto it = satTips.find (id); it != satTips.end())
            return it->second;
        if (id.startsWith ("satX"))
            return "Crossover: split frequency between two bands. Drag the line on the display.";
        if (id.length() > 2 && id[0] == 's' && juce::CharacterFunctions::isDigit (id[1]))
        {
            static const std::map<juce::String, juce::String> satBand {
                { "Style",  "Style: saturation character of this band: tube, tape, transformer, amp, clipping or effects." },
                { "Drive",  "Drive: how hard this band is pushed into the saturation." },
                { "Mix",    "Mix: blend between the clean and the saturated band." },
                { "Fb",     "Feedback: feeds the saturated output back into the input around the Feedback Freq, adding resonance and grit." },
                { "FbFreq", "Feedback Freq: centre frequency of the feedback path." },
                { "Dyn",    "Dynamics: positive values saturate loud parts more (punchier); negative values saturate them less (smoother)." },
                { "Tone",   "Tone: tilts this band darker (negative) or brighter (positive) after the saturation." },
                { "Level",  "Level: output level of this band." },
                { "Mute",   "Mute: silences this band." },
                { "Solo",   "Solo: hear only the soloed bands." },
                { "Bypass", "Bypass: passes this band through clean." },
            };
            if (auto it = satBand.find (id.substring (2)); it != satBand.end())
                return it->second;
        }
        return {};
    }
}
