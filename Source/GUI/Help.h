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

        // Compressor: cmpOn ... and per band c1Thr ...
        if (id.startsWith ("cmp") || (id.length() > 2 && id[0] == 'c' && juce::CharacterFunctions::isDigit (id[1])))
        {
            static const std::map<juce::String, juce::String> cmp {
                { "On",     "Compressor On/Off: multiband downward / upward compression after the Saturation stage." },
                { "Bands",  "Bands: compress the full signal (1) or split it into 2-4 bands, each with its own settings. Drag the dividers in the band strip to move the crossovers." },
                { "Style",  "Style: detection and timing character. Clean = transparent, Punch = analog-like, lets the transient through, Smooth / Bus = glue, Mastering = transparent with fast catch, Classic / Vari-Mu = vintage feedback designs, Opto = slow and soft, T4 Opto = LA-3A-style program-dependent release, Pumping = deep audible swell, TTM = upward + downward at once (threshold becomes a target level)." },
                { "Look",   "Lookahead: delays the audio slightly so the compressor can react before the peak. Adds latency (reported to the host)." },
                { "Hold",   "Hold: keeps the gain change for this long before releasing. Longer hold = pumping." },
                { "ScHp",   "Side-Chain High-Pass: stops the lowest band from reacting to the sub. Off = full-range detection." },
                { "Mix",    "Mix: parallel compression. Blend the uncompressed signal back in." },
                { "Out",    "Output: level after the compressor." },
                { "Auto",   "Auto Gain: automatic make-up gain estimated from threshold, ratio and knee, so the level stays similar while you adjust." },
                { "Limit",  "T4 Opto Mode: Compress (about 3:1, very soft knee) or Limit (about 12:1), like the switch on an LA-3A. Only used by the T4 Opto style." },
                { "Dir",    "Direction: Downward turns loud parts down (classic compression). Upward brings quiet parts up (the tail of the kick, room, texture). Both does both: the threshold becomes a target level." },
                { "Thr",    "Threshold: level where compression starts. Drag the dashed line in the display." },
                { "Ratio",  "Ratio: amount of compression. 4:1 = 4 dB in gives 1 dB out above the threshold. Infinity = limiting. Drag up/down in the knee display above the threshold." },
                { "Knee",   "Knee: softness of the transition around the threshold (0 = hard, up to 48 dB = very soft)." },
                { "Range",  "Range: maximum gain change in dB, in either direction. Limits how far upward compression can push quiet parts up." },
                { "Att",    "Attack: how fast the compressor reacts. Slower attack lets more of the kick's transient through (more punch)." },
                { "Rel",    "Release: how fast the gain recovers. Program dependent in the Clean, Smooth, Bus and Opto styles." },
                { "Gain",   "Gain: make-up gain for this band (added to Auto Gain)." },
                { "Mute",   "Mute: silence this band." },
                { "Solo",   "Solo: hear only the soloed bands." },
                { "Bypass", "Bypass: pass this band through without compression." },
            };
            const auto sfx = id.startsWith ("cmp") ? id.substring (3) : id.substring (2);
            if (auto it = cmp.find (sfx); it != cmp.end())
                return it->second;
        }

        // Space (reverb): spcOn, spcDecay, ...
        if (id.startsWith ("spc"))
        {
            static const std::map<juce::String, juce::String> spc {
                { "On",        "Space On/Off: stereo room reverb at the end of the chain, for width and ambience around the kick." },
                { "Mode",      "Mode: character of the room. Room / Large Room are natural, Chamber is dense, Hall is long and smooth, Plate is bright with almost no early reflections, Warehouse is big, dark and wobbly." },
                { "Mix",       "Mix: balance between the dry kick and the reverb (equal power). Keep it low for kicks; 10-25 % is usually plenty." },
                { "PreDelay",  "Predelay: gap between the kick and the start of the reverb. 10-30 ms keeps the transient clean." },
                { "Decay",     "Decay: reverb time (RT60) of the mid frequencies. The display shows the decay across the spectrum." },
                { "HighCut",   "High Cut: low-pass on the reverb output. Lower it for a darker, less noisy tail." },
                { "LowCut",    "Low Cut: high-pass before the reverb, so the sub stays mono and clean. 150-300 Hz works well for kicks." },
                { "Depth",     "Depth: balance between early reflections (0 %, close) and the late reverb (100 %, far away)." },
                { "Width",     "Width: stereo width of the reverb (0 % = mono, 100 % = normal, 200 % = extra wide). The dry kick is not affected." },
                { "Duck",      "Ducking: turns the reverb down while the kick hits and lets it bloom afterwards, keeping the punch." },
                { "ESize",     "Early Size: spread of the early reflections (small = tight room, large = big space)." },
                { "ECross",    "Early Cross: how much left and right feed into each other in the early reflections." },
                { "ESend",     "Early Send: how much of the early reflections feed the late reverb." },
                { "EDiff",     "Early Diffusion: smears the early reflections from distinct echoes into a smooth wash." },
                { "EModRate",  "Early Mod Rate: speed of the movement in the early reflections." },
                { "EModDepth", "Early Mod Depth: amount of movement in the early reflections (chorus-like)." },
                { "LSize",     "Late Size: size of the late reverb (delay lengths). Larger sounds bigger and less metallic." },
                { "LCross",    "Late Cross: how much left and right feed into each other in the late reverb." },
                { "LDiff",     "Late Diffusion: density of the late reverb. High = smooth, low = grainier, more echo-like." },
                { "LModRate",  "Late Mod Rate: speed of the delay modulation in the late reverb." },
                { "LModDepth", "Late Mod Depth: amount of modulation in the late reverb; reduces metallic ringing." },
                { "BassMult",  "Bass Mult: decay of the low frequencies relative to Decay (below 1x = shorter, tighter low end). Drag the BASS handle on the display." },
                { "BassX",     "Bass Xover: frequency below which Bass Mult applies." },
                { "HighMult",  "High Mult: decay of the high frequencies relative to Decay (below 1x = darker tail, like a real room). Drag the HIGH handle on the display." },
                { "HighX",     "High Xover: frequency above which High Mult applies." },
            };
            if (auto it = spc.find (id.substring (3)); it != spc.end())
                return it->second;
        }

        // Distortion stages: d1Drive, d2Mix, ...
        if (id.length() > 2 && id[0] == 'd' && (id[1] == '1' || id[1] == '2'))
        {
            const auto sfx = id.substring (2);
            static const std::map<juce::String, juce::String> dist {
                { "On",     "Distortion On/Off: Bypass this distortion stage." },
                { "Mode",   "Type: base distortion curve. Hard Clip, Fuzz and Diode 2 are aggressive, Soft Clip / Tanh / Overdrive are smoother, Tube and Diode are asymmetric, Folds and Rectify add extra harmonics, Zero-Square is hollow and gritty, Downsample reduces the sample rate (Drive sets the amount), X-Shaper lets you draw the curve (Asym: both halves separately)." },
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
                { "OS",     "Oversampling: reduces aliasing (harsh, non-musical overtones) at the cost of CPU. 4x is a good default; 8x or 16x for very high drive on bright sounds." },
                { "FltPos",  "Filter Position: a resonant filter on the distorted path, either PRE (before the curve: only the part of the kick that passes the filter gets distorted) or POST (after the curve: shapes the distortion's harmonics). Off by default." },
                { "FltType", "Filter Type: Low Pass 12/24 dB, Band Pass, High Pass 12/24 dB or Notch. Tip: a resonant Band Pass PRE the curve is a classic way to focus the distortion on the punch." },
                { "FltFreq", "Filter Freq: cutoff / centre frequency of the distortion filter. Drag the handle on the transfer display's filter strip, or use the knob." },
                { "FltRes",  "Filter Resonance: emphasis around the cutoff. High resonance PRE the curve gives a ringing, tonal distortion." },
                { "Spatial", "Spatial: Distorts the left and right channels slightly differently (drive and operating point), creating stereo width from the mono kick. Use the EQ's Side placement to shape it." },
            };
            if (auto it = dist.find (sfx); it != dist.end())
                return it->second;
            if (sfx.startsWith ("XP") || sfx.startsWith ("XN"))
                return "X-Shaper point: one point of the drawable shaper curve (used by the X-Shaper modes). Drag the points on the transfer display.";
        }

        if (id == "eqMode" || id == "eq2Mode")
            return "Processing Mode (as in Pro-Q 4): Zero Latency = minimum-phase filters, no delay. Natural Phase = filters matched to their analog response up to the top of the spectrum, small latency. Linear Phase = no phase shift at all (the kick's waveform keeps its shape), adds latency; the resolution sets how accurate the low end is.";
        if (id == "eqRes" || id == "eq2Res")
            return "Linear Phase Resolution: length of the linear-phase filter. Higher = more accurate low-frequency filters (important for kicks) but more latency (Medium is about 85 ms at 48 kHz).";
        if (id == "eqOn" || id == "eq2On")
            return id == "eqOn" ? "EQ On/Off: bypass the whole equaliser." : "EQ 2 On/Off: the second equaliser after the Compressor, before Space. Use it for final shaping.";
        if (id == "eq2Scale")
            return "Gain Scale: scales the gain of all Bell, Shelf and Tilt bands of EQ 2 at once.";

        // EQ bands: b1Freq (EQ 1), q1Freq (EQ 2), ...
        if (id.length() > 2 && (id[0] == 'b' || id[0] == 'q') && juce::CharacterFunctions::isDigit (id[1]))
        {
            const auto sfx = id.substring (2);
            static const std::map<juce::String, juce::String> band {
                { "Used",  "Band Used: whether this band exists on the EQ display (up to 24 bands). Add bands by double-clicking the display, delete them with the band menu or the Delete key." },
                { "On",    "Band On/Off: bypass this EQ band without deleting it (Alt-click its node). Tip: double-click on the EQ display to add a band." },
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
            { "satIn",    "Input: level going into the saturation stage. Higher input = more saturation in every band." },
            { "satMode",  "Channel Mode: Stereo processes left and right; Mid/Side processes the centre and the width separately (use with Drive Pan / Level Pan)." },
        };
        if (auto it = satTips.find (id); it != satTips.end())
            return it->second;
        if (id.startsWith ("satX"))
            return "Crossover: split frequency between two bands. Drag the line on the display.";
        if (id.length() > 2 && id[0] == 's' && juce::CharacterFunctions::isDigit (id[1]))
        {
            static const std::map<juce::String, juce::String> satBand {
                { "Style",  "Style: saturation character of this band: tube, tape, transformer, amp, clipping or effects." },
                { "Drive",  "Drive: how hard this band is pushed into the saturation. The output level is compensated automatically." },
                { "Mix",    "Mix: blend between the clean and the saturated band." },
                { "Fb",     "Feedback: feeds the saturated output back into the input around the Feedback Freq, adding resonance and grit." },
                { "FbFreq", "Feedback Freq: centre frequency of the feedback path." },
                { "Dyn",    "Dynamics: turn right for compression (loud parts pulled down, pumping), left for gating / expansion (quiet parts pulled down)." },
                { "Bass",   "Bass: low shelf at 120 Hz on the saturated band." },
                { "MidT",   "Mid: bell at 800 Hz on the saturated band." },
                { "Treble", "Treble: high shelf at 5 kHz on the saturated band." },
                { "Pres",   "Presence: bell at 3 kHz on the saturated band." },
                { "DrivePan", "Drive Pan: shifts the drive towards the left or right channel (mid or side in Mid/Side mode)." },
                { "LevelPan", "Level Pan: balance of this band between left and right (mid and side in Mid/Side mode)." },
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
