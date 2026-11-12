// CPU benchmark and null-test renderer.
//
//   CodexBench [--seconds S] [--runs R] [--filter text] [--rates 48000,96000] [--blocks 64,128,512]
//       For every scenario (each factory preset plus synthetic worst cases), plays a kick every 1/2 beat at 150 BPM
//       and prints the real-time factor: CPU time of processBlock / audio time, in %, the median of R runs of S seconds.
//       CPU time is the calling thread's CPU time (the linear-phase EQ worker is settled before timing starts).
//   --extra adds the coverage scenarios (parameter automation, mode / slope / placement switches) to the list.
//   --filter text keeps the scenarios whose name contains text (case-insensitive); --filter =Name keeps exactly Name.
//   CodexBench --list
//       Lists the scenario names.
//   CodexBench --null <dir> [--filter text]
//       Renders a fixed set of test notes for every scenario to 32-bit float WAVs in <dir>
//       (48 kHz with 128-sample blocks and 96 kHz with seeded random block sizes), for before / after null tests.
//   CodexBench --compare <dirA> <dirB>
//       Nulls every WAV in dirA against the same file in dirB and prints the peak difference in dBFS
//       (fails if any file is missing, has a different length, or differs by more than -100 dBFS).
//
// Deterministic: the click-noise generator gets a fixed seed, the linear-phase EQ filters are installed early in a
// fixed-length silent pre-roll (see makeProcessor), and random block sizes use a fixed seed.
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include <chrono>
#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <time.h>
#endif

namespace
{
constexpr juce::int64 noiseSeed = 0x5eed1234;

void setParam (CodexProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    jassert (prm != nullptr);
    if (prm != nullptr) prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

double threadCpuSeconds()
{
   #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
    timespec ts {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec + 1.0e-9 * (double) ts.tv_nsec;
   #else
    return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
   #endif
}

//==============================================================================
// Scenarios
struct Scenario
{
    juce::String name;
    std::function<void (CodexProcessor&)> configure;
    bool notes = true;   // false: idle (no MIDI)
    /** Extra (coverage) scenarios only: parameter automation, called before every block with the block's start time. */
    std::function<void (CodexProcessor&, double)> automate {};
    bool extra = false;    // only with --extra (not part of the default benchmark / null set)
    bool waitEq = false;   // after each block, wait until a pending linear-phase EQ build is ready to install (deterministic swaps)
};

/** 24 bands of mixed types, slopes and stereo placements on one EQ, in a processing mode (0 zero latency, 1 natural, 2 linear). */
void eq24 (CodexProcessor& p, int e, int mode, int resolution)
{
    const eq::BandType types[] = { eq::BandType::LowCut, eq::BandType::LowShelf, eq::BandType::Bell, eq::BandType::Notch,
                                   eq::BandType::HighShelf, eq::BandType::HighCut, eq::BandType::BandPass, eq::BandType::TiltShelf,
                                   eq::BandType::FlatTilt, eq::BandType::AllPass };
    for (int b = 0; b < Params::numBands; ++b)
    {
        auto t = types[b % 10];
        // keep the cuts out of the way so the signal survives; pass filters at the extremes
        const float freq = 25.0f * std::pow (2.0f, (float) b * 9.5f / (float) (Params::numBands - 1));
        if (t == eq::BandType::BandPass) t = eq::BandType::Bell;
        setParam (p, ID::eqBand (e, b, "Used"), 1.0f);
        setParam (p, ID::eqBand (e, b, "On"), 1.0f);
        setParam (p, ID::eqBand (e, b, "Type"), (float) (int) t);
        setParam (p, ID::eqBand (e, b, "Freq"), t == eq::BandType::LowCut ? 22.0f : t == eq::BandType::HighCut ? 18000.0f : freq);
        setParam (p, ID::eqBand (e, b, "Gain"), (b % 2 == 0 ? 1.0f : -1.0f) * (1.0f + (float) (b % 5)));
        setParam (p, ID::eqBand (e, b, "Q"), 0.5f + 0.25f * (float) (b % 7));
        setParam (p, ID::eqBand (e, b, "Slope"), (float) (b % eq::numSlopes));
        setParam (p, ID::eqBand (e, b, "Place"), (float) (b % 5 == 4 ? 4 : b % 5 == 3 ? 3 : 0));   // stereo, some mid / side
    }
    setParam (p, ID::eqOn (e), 1.0f);
    setParam (p, ID::eqMode (e), (float) mode);
    setParam (p, ID::eqRes (e), (float) resolution);
}

/** Every module on at its most expensive settings (EQs in Natural Phase = oversampled, 24 bands each). */
void allModulesMax (CodexProcessor& p)
{
    for (int s = 1; s <= 2; ++s)
    {
        setParam (p, ID::clip (s, "On"), 1.0f);
        setParam (p, ID::clip (s, "Mode"), (float) (s == 1 ? ClipDistortion::Tube : ClipDistortion::XShaperAsym));
        setParam (p, ID::clip (s, "Drive"), 18.0f);
        setParam (p, ID::clip (s, "OS"), 4.0f);   // 16x
        setParam (p, ID::clip (s, "Spatial"), 1.0f);
        setParam (p, ID::clip (s, "AutoGain"), 1.0f);
        setParam (p, ID::clip (s, "FltPos"), 1.0f);
        setParam (p, ID::clip (s, "FltType"), 2.0f);
        setParam (p, ID::clip (s, "TailDrv"), 6.0f);
        setParam (p, ID::clip (s, "LoFilt"), 12000.0f);
        setParam (p, ID::clip (s, "HiFilt"), 40.0f);
        setParam (p, ID::clip (s, "ShelfG"), 3.0f);
        setParam (p, ID::clip (s, "Mix"), 0.8f);
    }
    eq24 (p, 0, 1, 1);
    eq24 (p, 1, 1, 1);
    setParam (p, ID::sat ("On"), 1.0f);
    setParam (p, ID::sat ("Bands"), (float) sat::maxBands);
    setParam (p, ID::sat ("OS"), 3.0f);   // 8x
    setParam (p, ID::sat ("Mode"), 1.0f); // mid/side
    setParam (p, ID::sat ("Mix"), 0.8f);
    for (int b = 0; b < sat::maxBands; ++b)
    {
        setParam (p, ID::satBand (b, "Fb"), 0.3f);
        setParam (p, ID::satBand (b, "Dyn"), 0.4f);
        setParam (p, ID::satBand (b, "Bass"), 2.0f);
        setParam (p, ID::satBand (b, "Pres"), 2.0f);
        setParam (p, ID::satBand (b, "DrivePan"), 0.3f);
    }
    setParam (p, ID::cmp ("On"), 1.0f);
    setParam (p, ID::cmp ("Bands"), (float) comp::maxBands);
    setParam (p, ID::cmp ("Look"), 4.0f);  // 10 ms
    setParam (p, ID::cmp ("Hold"), 20.0f);
    setParam (p, ID::cmp ("ScHp"), 80.0f);
    setParam (p, ID::cmp ("Mix"), 0.9f);
    setParam (p, ID::spc ("On"), 1.0f);
    setParam (p, ID::subOn, 1.0f);
    setParam (p, ID::clipOn, 1.0f);
}

//==============================================================================
// Extra coverage scenarios: automation of the parameters the CPU optimisations touch (smoothers, glides, mode,
// slope, band-count and placement changes), rendered before / after with --null --extra.
float wave (double t, double hz, double phase = 0.0) { return (float) std::sin (juce::MathConstants<double>::twoPi * hz * t + phase); }
int stepOf (double t, double len) { return (int) std::floor (t / len); }

void satSetup (CodexProcessor& p, int mode)
{
    setParam (p, ID::clip (1, "On"), 0.0f);
    setParam (p, ID::sat ("On"), 1.0f);
    setParam (p, ID::sat ("Bands"), 6.0f);
    setParam (p, ID::sat ("OS"), 2.0f);
    setParam (p, ID::sat ("Mode"), (float) mode);
    setParam (p, ID::sat ("Mix"), 0.8f);
    for (int b = 0; b < sat::maxBands; ++b)
    {
        setParam (p, ID::satBand (b, "Fb"), 0.25f);
        setParam (p, ID::satBand (b, "Dyn"), b % 3 == 0 ? 0.0f : (b % 2 ? 0.5f : -0.5f));
    }
}

void satAutomate (CodexProcessor& p, double t, const std::array<int, 4>& slopes)
{
    const int k = stepOf (t, 0.13);
    for (int b = 0; b < sat::maxBands; ++b)
    {
        setParam (p, ID::satBand (b, "Style"), (float) ((b * 3 + k) % 20));
        setParam (p, ID::satBand (b, "Drive"), 12.0f + 10.0f * wave (t, 1.3, b));
        setParam (p, ID::satBand (b, "DrivePan"), 0.6f * wave (t, 0.9, 2 * b));
        setParam (p, ID::satBand (b, "LevelPan"), 0.7f * wave (t, 1.1, b + 1));
        setParam (p, ID::satBand (b, "Mix"), 0.6f + 0.4f * wave (t, 0.8, b));
        setParam (p, ID::satBand (b, "Level"), 3.0f * wave (t, 0.5, b));
        setParam (p, ID::satBand (b, "FbFreq"), 300.0f * std::pow (2.0f, 2.0f * wave (t, 0.4, b)));
        setParam (p, ID::satBand (b, "Bass"), b % 3 == 0 ? 0.0f : 6.0f * wave (t, 0.7, b));
        setParam (p, ID::satBand (b, "MidT"), 4.0f * wave (t, 1.7, b));
        setParam (p, ID::satBand (b, "Treble"), b % 2 == 0 ? 0.0f : 5.0f * wave (t, 1.2, b));
        setParam (p, ID::satBand (b, "Pres"), 3.0f * wave (t, 0.6, b + 3));
        setParam (p, ID::satBand (b, "Dyn"), b % 3 == 0 ? 0.0f : 0.6f * wave (t, 0.3, b));
    }
    setParam (p, ID::satBand (1, "Bypass"), t > 0.6 && t < 0.9 ? 1.0f : 0.0f);
    setParam (p, ID::satBand (4, "Mute"), t > 1.0 && t < 1.2 ? 1.0f : 0.0f);
    setParam (p, ID::satBand (2, "Solo"), t > 1.5 && t < 1.65 ? 1.0f : 0.0f);
    const float base[] = { 120.0f, 400.0f, 1200.0f, 3500.0f, 9000.0f };
    for (int j = 0; j < sat::maxBands - 1; ++j)
        setParam (p, ID::sat (("X" + juce::String (j + 1)).toRawUTF8()), base[j] * std::pow (2.0f, 0.9f * wave (t, 0.7, j)));
    setParam (p, ID::sat ("Slope"), (float) slopes[(size_t) (stepOf (t, 0.6) % 4)]);
    const int bands[] = { 6, 4, 2, 1, 5, 3, 6 };
    setParam (p, ID::sat ("Bands"), (float) bands[stepOf (t, 0.4) % 7]);
    setParam (p, ID::sat ("In"), 4.0f * wave (t, 0.9));
    setParam (p, ID::sat ("Out"), 2.0f * wave (t, 1.4));
    setParam (p, ID::sat ("Mix"), 0.7f + 0.3f * wave (t, 1.0));
    setParam (p, ID::sat ("OS"), t < 2.1 ? 2.0f : 1.0f);
}

void distAutomate (CodexProcessor& p, double t)
{
    const int k = stepOf (t, 0.15);
    for (int s = 1; s <= 2; ++s)
    {
        setParam (p, ID::clip (s, "On"), 1.0f);
        setParam (p, ID::clip (s, "Mode"), (float) ((k + (s == 2 ? 8 : 0)) % ClipDistortion::numModes));
        setParam (p, ID::clip (s, "Drive"), 18.0f + 12.0f * wave (t, 1.1, s));
        setParam (p, ID::clip (s, "Bias"), t < 0.3 ? 0.0f : 0.5f * wave (t, 0.8, s));
        setParam (p, ID::clip (s, "Spatial"), juce::jmax (0.0f, wave (t, 0.6, s)));
        setParam (p, ID::clip (s, "Char"), 0.5f + 0.5f * wave (t, 0.9, s));
        setParam (p, ID::clip (s, "Mix"), 0.7f + 0.3f * wave (t, 1.3, s));
        setParam (p, ID::clip (s, "Out"), -6.0f + 3.0f * wave (t, 0.5, s));
        setParam (p, ID::clip (s, "AutoGain"), s == 1 ? 1.0f : 0.0f);
        setParam (p, ID::clip (s, "FltPos"), (float) ((stepOf (t, 0.5) + s) % 3));
        setParam (p, ID::clip (s, "FltType"), (float) (stepOf (t, 0.7) % 3));
        setParam (p, ID::clip (s, "FltFreq"), 1500.0f * std::pow (2.0f, 2.0f * wave (t, 1.0, s)));
        setParam (p, ID::clip (s, "LoFilt"), 9000.0f * std::pow (2.0f, wave (t, 0.7)));
        setParam (p, ID::clip (s, "HiFilt"), 60.0f * std::pow (2.0f, wave (t, 0.9)));
        setParam (p, ID::clip (s, "ShelfF"), 5000.0f * std::pow (2.0f, wave (t, 0.4)));
        setParam (p, ID::clip (s, "ShelfG"), 6.0f * wave (t, 0.8));
        setParam (p, ID::clip (s, "TailDrv"), 8.0f * wave (t, 0.5, s));
        setParam (p, ID::clip (s, "OS"), (float) (t < 1.7 ? 4 - s : 2 + s % 2));
        for (int c = 0; c < ClipDistortion::numCurvePoints; ++c)
        {
            setParam (p, ID::clip (s, ("XP" + juce::String (c + 1)).toRawUTF8()), juce::jlimit (-1.0f, 1.0f, (float) (c + 1) / ClipDistortion::numCurvePoints + 0.3f * wave (t, 0.9, c)));
            setParam (p, ID::clip (s, ("XN" + juce::String (c + 1)).toRawUTF8()), juce::jlimit (-1.0f, 1.0f, (float) (c + 1) / ClipDistortion::numCurvePoints - 0.3f * wave (t, 0.7, c)));
        }
    }
    setParam (p, ID::clipOn, 1.0f);
    setParam (p, ID::clipCeiling, -3.0f + 2.5f * wave (t, 0.9));
}

/** EQ bands with all five placements, automated (placements switch over time). */
void eqPlaceAutomate (CodexProcessor& p, double t, int e, bool movePlacement, bool moveFreq)
{
    for (int b = 0; b < 10; ++b)
    {
        const int place = movePlacement ? (b + stepOf (t, 0.5)) % 5 : b % 5;
        setParam (p, ID::eqBand (e, b, "Place"), (float) place);
        if (moveFreq)
        {
            setParam (p, ID::eqBand (e, b, "Freq"), 80.0f * std::pow (2.0f, (float) b * 0.7f + wave (t, 0.6, b)));
            setParam (p, ID::eqBand (e, b, "Gain"), 6.0f * wave (t, 0.8, b));
        }
    }
}

void eqPlaceSetup (CodexProcessor& p, int e, int mode, int res)
{
    eq24 (p, e, mode, res);
    for (int b = 10; b < Params::numBands; ++b) setParam (p, ID::eqBand (e, b, "On"), 0.0f);
    eqPlaceAutomate (p, 0.0, e, true, true);
}

void compAutomate (CodexProcessor& p, double t)
{
    setParam (p, ID::cmp ("Style"), (float) (stepOf (t, 0.2) % comp::numStyles));
    const int bands[] = { 4, 1, 3, 2, 4 };
    setParam (p, ID::cmp ("Bands"), (float) bands[stepOf (t, 0.55) % 5]);
    setParam (p, ID::cmp ("Look"), (float) (stepOf (t, 0.35) % 5));
    setParam (p, ID::cmp ("Limit"), stepOf (t, 0.9) % 2 ? 1.0f : 0.0f);
    setParam (p, ID::cmp ("Mix"), 0.7f + 0.3f * wave (t, 1.0));
    setParam (p, ID::cmp ("Out"), 2.0f * wave (t, 0.7));
    for (int j = 0; j < comp::maxBands - 1; ++j)
        setParam (p, ID::cmp (("X" + juce::String (j + 1)).toRawUTF8()), 150.0f * std::pow (6.0f, (float) j) * std::pow (2.0f, 0.7f * wave (t, 0.5, j)));
    for (int b = 0; b < comp::maxBands; ++b)
    {
        setParam (p, ID::cmpBand (b, "Thr"), -24.0f + 10.0f * wave (t, 0.8, b));
        setParam (p, ID::cmpBand (b, "Ratio"), 4.0f + 3.0f * wave (t, 0.6, b));
        setParam (p, ID::cmpBand (b, "Dir"), (float) ((b + stepOf (t, 0.8)) % 2));
        setParam (p, ID::cmpBand (b, "Gain"), 3.0f * wave (t, 0.4, b));
        setParam (p, ID::cmpBand (b, "Bypass"), b == 2 && t > 1.2 && t < 1.4 ? 1.0f : 0.0f);
        setParam (p, ID::cmpBand (b, "Solo"), b == 1 && t > 1.8 && t < 1.95 ? 1.0f : 0.0f);
    }
}

void addExtraScenarios (std::vector<Scenario>& list)
{
    auto add = [&list] (juce::String name, std::function<void (CodexProcessor&)> cfg, std::function<void (CodexProcessor&, double)> autom, bool waitEq = false)
    {
        Scenario s { "Extra " + name, std::move (cfg) };
        s.automate = std::move (autom);
        s.extra = true;
        s.waitEq = waitEq;
        list.push_back (std::move (s));
    };
    add ("Sat automation stereo", [] (CodexProcessor& p) { satSetup (p, 0); },
         [] (CodexProcessor& p, double t) { satAutomate (p, t, { 2, 0, 3, 1 }); });
    add ("Sat automation mid side", [] (CodexProcessor& p) { satSetup (p, 1); },
         [] (CodexProcessor& p, double t) { satAutomate (p, t, { 3, 1, 0, 2 }); });
    add ("Sat static 8x stereo", [] (CodexProcessor& p) { satSetup (p, 0); setParam (p, ID::sat ("OS"), 3.0f); satAutomate (p, 0.31, { 1, 1, 1, 1 }); }, {});
    add ("Dist automation", [] (CodexProcessor& p) { distAutomate (p, 0.0); }, distAutomate);
    add ("Dist static Tube offset", [] (CodexProcessor& p) { setParam (p, ID::clip (1, "Mode"), (float) ClipDistortion::Tube); setParam (p, ID::clip (1, "Bias"), 0.3f);
                                                             setParam (p, ID::clip (1, "Spatial"), 0.5f); setParam (p, ID::clip (2, "On"), 1.0f);
                                                             setParam (p, ID::clip (2, "Mode"), (float) ClipDistortion::Diode2); }, {});
    add ("EQ placements Zero Latency", [] (CodexProcessor& p) { eqPlaceSetup (p, 0, 0, 1); },
         [] (CodexProcessor& p, double t) { eqPlaceAutomate (p, t, 0, true, true); });
    add ("EQ placements Natural Phase", [] (CodexProcessor& p) { eqPlaceSetup (p, 1, 1, 1); },
         [] (CodexProcessor& p, double t) { eqPlaceAutomate (p, t, 1, true, true); });
    add ("EQ linear placements switch", [] (CodexProcessor& p) { eqPlaceSetup (p, 0, 2, 1); },
         [] (CodexProcessor& p, double t)
         {
             // Mid/Side + Stereo bands, then Left/Right bands too, then back (M/S <-> 2x2 sets)
             const int phase = stepOf (t, 0.7) % 2;
             for (int b = 0; b < 10; ++b) setParam (p, ID::eqBand (0, b, "Place"), (float) (phase == 0 ? (b % 2 ? 3 : 4) : b % 5));
         }, true);
    add ("EQ linear silence, no shaper", [] (CodexProcessor& p) { setParam (p, ID::clip (1, "On"), 0.0f); setParam (p, ID::clip (2, "On"), 0.0f);
                                                                  eq24 (p, 1, 2, 1); },
         [] (CodexProcessor& p, double t) { setParam (p, ID::eqBand (1, 2, "Gain"), t < 2.3 ? 3.0f : -4.0f); }, true);
    add ("EQ linear M/S, no shaper", [] (CodexProcessor& p) { setParam (p, ID::clip (1, "On"), 0.0f); eq24 (p, 0, 2, 2); }, {});
    add ("Compressor automation", [] (CodexProcessor& p) { setParam (p, ID::cmp ("On"), 1.0f); compAutomate (p, 0.0); }, compAutomate);
    add ("All modules max automated", allModulesMax,
         [] (CodexProcessor& p, double t) { satAutomate (p, t, { 3, 2, 1, 0 }); compAutomate (p, t); eqPlaceAutomate (p, t, 0, false, true);
                                            setParam (p, ID::clip (1, "Bias"), 0.4f * wave (t, 0.7)); setParam (p, ID::clip (2, "Spatial"), 0.5f + 0.5f * wave (t, 0.9)); });
}

std::vector<Scenario> makeScenarios()
{
    std::vector<Scenario> list;
    CodexProcessor probe;
    const auto names = probe.presets.getFactoryNames();
    for (int i = 0; i < names.size(); ++i)
        list.push_back ({ "Preset " + juce::String (i).paddedLeft ('0', 2) + " " + names[i],
                          [i] (CodexProcessor& p) { p.presets.loadPreset (i); } });

    list.push_back ({ "Init (default patch)", [] (CodexProcessor&) {} });
    list.push_back ({ "909 engine", [] (CodexProcessor& p) { setParam (p, ID::engine, (float) KickVoice::Engine909); } });
    list.push_back ({ "Space on", [] (CodexProcessor& p) { setParam (p, ID::spc ("On"), 1.0f); } });
    list.push_back ({ "EQ1+2 24 bands Zero Latency", [] (CodexProcessor& p) { eq24 (p, 0, 0, 1); eq24 (p, 1, 0, 1); } });
    list.push_back ({ "EQ1+2 24 bands Natural Phase", [] (CodexProcessor& p) { eq24 (p, 0, 1, 1); eq24 (p, 1, 1, 1); } });
    list.push_back ({ "EQ1+2 24 bands Linear Medium", [] (CodexProcessor& p) { eq24 (p, 0, 2, 1); eq24 (p, 1, 2, 1); } });
    list.push_back ({ "EQ1+2 24 bands Linear Maximum", [] (CodexProcessor& p) { eq24 (p, 0, 2, 4); eq24 (p, 1, 2, 4); } });
    list.push_back ({ "All modules max", allModulesMax });
    list.push_back ({ "All modules max, EQ linear Max", [] (CodexProcessor& p) { allModulesMax (p); setParam (p, ID::eqMode (0), 2.0f); setParam (p, ID::eqMode (1), 2.0f);
                                                                                 setParam (p, ID::eqRes (0), 4.0f); setParam (p, ID::eqRes (1), 4.0f); } });
    list.push_back ({ "Idle, all modules max (no notes)", allModulesMax, false });
    addExtraScenarios (list);
    return list;
}

//==============================================================================
/** A fresh processor with the scenario applied, prepared, and settled with a fixed-length (4 s) silent pre-roll.
    The pre-roll length is fixed because some state (e.g. the Space reverb's modulation) depends on the number of
    samples processed. During its first second the linear-phase EQ worker gets wall-clock time (the audio thread
    sleeps while filters are pending); the filters must be installed by then and stay so for the remaining 3 s, so
    the crossfade-in (which an asymmetric shaper's DC makes audible even with silent input) has long decayed. */
std::unique_ptr<CodexProcessor> makeProcessor (const Scenario& sc, double rate, int block)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        auto p = std::make_unique<CodexProcessor>();
        p->setPlayConfigDetails (0, 2, rate, block);
        p->setNonRealtime (true);
        p->seedNoiseForTesting (noiseSeed);
        sc.configure (*p);
        p->prepareToPlay (rate, block);

        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer none;
        const int settleBlocks = (int) std::ceil (rate / block), totalBlocks = (int) std::ceil (4.0 * rate / block);
        bool ok = true;
        for (int i = 0; i < totalBlocks && ok; ++i)
        {
            buf.clear();
            p->processBlock (buf, none);
            if (i < settleBlocks)
                for (int w = 0; w < 25 * (attempt + 1) && ! p->isSettled(); ++w)
                    juce::Thread::sleep (2);
            else
                ok = p->isSettled();
        }
        if (ok) return p;
    }
    std::cerr << "warning: " << sc.name << " did not settle (linear-phase EQ not ready)\n";
    return nullptr;
}

struct Note { double onSec, lenSec; int note, velocity; };

/** Plays `notes` from `startSample` on (absolute sample clock), processes `numSamples` in blocks; returns CPU seconds. */
double play (CodexProcessor& p, juce::AudioBuffer<float>& io, juce::int64 startSample, int numSamples, int block,
             const std::vector<Note>& notes, double rate, juce::AudioBuffer<float>* capture = nullptr, juce::Random* randomBlocks = nullptr,
             const Scenario* sc = nullptr)
{
    juce::MidiBuffer midi;
    double cpu = 0.0;
    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min (randomBlocks != nullptr ? 1 + randomBlocks->nextInt (block) : block, numSamples - pos);
        const juce::int64 t0 = startSample + pos, t1 = t0 + n;
        midi.clear();
        for (const auto& nt : notes)
        {
            const auto on = (juce::int64) std::llround (nt.onSec * rate), off = on + (juce::int64) std::llround (nt.lenSec * rate);
            if (on >= t0 && on < t1)   midi.addEvent (juce::MidiMessage::noteOn (1, nt.note, (juce::uint8) nt.velocity), (int) (on - t0));
            if (off >= t0 && off < t1) midi.addEvent (juce::MidiMessage::noteOff (1, nt.note), (int) (off - t0));
        }
        io.setSize (2, n, false, false, true);
        io.clear();
        if (sc != nullptr && sc->automate) sc->automate (p, (double) t0 / rate);
        const double c0 = threadCpuSeconds();
        p.processBlock (io, midi);
        cpu += threadCpuSeconds() - c0;
        if (sc != nullptr && sc->waitEq)
            for (int w = 0; w < 2500 && ! p.isEqBuildReadyForTesting(); ++w)
                juce::Thread::sleep (2);
        if (capture != nullptr)
            for (int ch = 0; ch < 2; ++ch) capture->copyFrom (ch, pos, io, ch, 0, n);
        pos += n;
    }
    return cpu;
}

/** Kicks every 1/2 beat at 150 BPM (0.2 s), each held 0.1 s, covering [fromSec, toSec). */
std::vector<Note> kickPattern (double fromSec, double toSec)
{
    std::vector<Note> v;
    const double step = 60.0 / 150.0 / 2.0;
    for (int k = (int) std::floor (fromSec / step); k * step < toSec; ++k)
        if (k >= 0) v.push_back ({ k * step, 0.1, 60, k % 4 == 0 ? 127 : 100 });
    return v;
}

//==============================================================================
int runBench (const std::vector<Scenario>& scenarios, double seconds, int runs, const std::vector<double>& rates, const std::vector<int>& blocks)
{
    std::cout << "CodexBench: real-time factor = CPU time / audio time (%), median of " << runs << " runs x " << seconds
              << " s, kick every 1/2 beat at 150 BPM\n\n";
    juce::String header = juce::String ("Scenario").paddedRight (' ', 44);
    for (auto r : rates)
        for (auto b : blocks)
            header << juce::String (juce::roundToInt (r / 1000.0)) + "k/" + juce::String (b).paddedLeft (' ', 3) + "  ";
    std::cout << header << "\n" << juce::String::repeatedString ("-", header.length()) << "\n" << std::flush;

    for (const auto& sc : scenarios)
    {
        juce::String line = sc.name.substring (0, 43).paddedRight (' ', 44);
        for (auto rate : rates)
            for (auto block : blocks)
            {
                auto p = makeProcessor (sc, rate, block);
                if (p == nullptr) { line << juce::String ("n/a").paddedLeft (' ', 8) << "  "; continue; }
                juce::AudioBuffer<float> io (2, block);
                const int runSamples = (int) (seconds * rate);
                juce::int64 clock = 0;
                const auto pattern = sc.notes ? kickPattern (0.0, (double) (runs + 1) * seconds + 1.0) : std::vector<Note> {};
                play (*p, io, clock, (int) (0.5 * rate), block, pattern, rate, nullptr, nullptr, &sc);   // warm-up (caches, first hits)
                clock += (int) (0.5 * rate);
                std::vector<double> rtf;
                for (int r = 0; r < runs; ++r)
                {
                    rtf.push_back (100.0 * play (*p, io, clock, runSamples, block, pattern, rate, nullptr, nullptr, &sc) / ((double) runSamples / rate));
                    clock += runSamples;
                }
                std::sort (rtf.begin(), rtf.end());
                const double median = rtf.size() % 2 ? rtf[rtf.size() / 2] : 0.5 * (rtf[rtf.size() / 2 - 1] + rtf[rtf.size() / 2]);
                line << juce::String (median, 2).paddedLeft (' ', 8) << "  ";
            }
        std::cout << line << "\n" << std::flush;
    }
    return 0;
}

//==============================================================================
bool writeFloatWav (const juce::AudioBuffer<float>& b, double rate, const juce::File& f)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream (f.createOutputStream());
    if (stream == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 2, 32, {}, 0));   // 32 bit = IEEE float
    if (writer == nullptr) return false;
    stream.release();
    return writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

juce::String fileNameFor (const juce::String& scenario)
{
    juce::String s;
    for (auto c : scenario)
        s << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c) : juce::String ("_"));
    while (s.contains ("__")) s = s.replace ("__", "_");
    return s.trimCharactersAtEnd ("_");
}

int runNull (const std::vector<Scenario>& scenarios, const juce::File& dir)
{
    dir.createDirectory();
    // velocities, lengths, keys, a fast retrigger, a long hold and a release into the tail
    const std::vector<Note> notes { { 0.05, 0.15, 60, 127 }, { 0.45, 0.05, 60, 64 }, { 0.80, 0.40, 48, 110 },
                                    { 1.30, 0.02, 72, 127 }, { 1.38, 0.10, 60, 90 }, { 1.70, 0.20, 55, 127 } };
    struct Cfg { double rate; int block; bool randomBlocks; const char* tag; double seconds; };
    const Cfg cfgs[] = { { 48000.0, 128, false, "48k", 2.6 }, { 96000.0, 512, true, "96k_rnd", 2.6 } };
    int failures = 0;
    for (const auto& sc : scenarios)
        for (const auto& c : cfgs)
        {
            auto p = makeProcessor (sc, c.rate, c.block);
            if (p == nullptr) { ++failures; continue; }
            const int total = (int) (c.seconds * c.rate) + p->getLatencySamples();   // the whole test, even behind long linear-phase latencies
            juce::AudioBuffer<float> out (2, total), io (2, c.block);
            juce::Random rng (20261112);
            play (*p, io, 0, total, c.block, sc.notes ? notes : std::vector<Note> {}, c.rate, &out, c.randomBlocks ? &rng : nullptr, &sc);
            const auto f = dir.getChildFile (fileNameFor (sc.name) + "_" + c.tag + ".wav");
            if (! writeFloatWav (out, c.rate, f)) { std::cerr << "cannot write " << f.getFullPathName() << "\n"; ++failures; continue; }
            std::cout << f.getFileName() << "  latency " << p->getLatencySamples() << "  peak "
                      << juce::String (juce::Decibels::gainToDecibels (out.getMagnitude (0, total), -200.0f), 1) << " dBFS\n" << std::flush;
        }
    std::cout << (failures == 0 ? "null renders written to " : "FAILED, some renders missing in ") << dir.getFullPathName() << "\n";
    return failures == 0 ? 0 : 1;
}

int runCompare (const juce::File& a, const juce::File& b)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    int bad = 0, count = 0;
    double worst = -400.0;
    for (const auto& fa : a.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        const auto fb = b.getChildFile (fa.getFileName());
        std::unique_ptr<juce::AudioFormatReader> ra (fm.createReaderFor (fa)), rb (fm.createReaderFor (fb));
        if (ra == nullptr || rb == nullptr || ra->lengthInSamples != rb->lengthInSamples || ra->numChannels != rb->numChannels)
        {
            std::cout << fa.getFileName() << "  MISSING OR DIFFERENT LENGTH\n";
            ++bad; continue;
        }
        const int n = (int) ra->lengthInSamples, chs = (int) ra->numChannels;
        juce::AudioBuffer<float> x (chs, n), y (chs, n);
        ra->read (&x, 0, n, 0, true, true);
        rb->read (&y, 0, n, 0, true, true);
        double peak = 0.0;
        bool identical = true;
        for (int ch = 0; ch < chs; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const float d = x.getSample (ch, i) - y.getSample (ch, i);
                identical = identical && std::memcmp (x.getReadPointer (ch) + i, y.getReadPointer (ch) + i, sizeof (float)) == 0;
                peak = std::max (peak, (double) std::abs (d));
            }
        const double db = peak > 0.0 ? 20.0 * std::log10 (peak) : -400.0;
        worst = std::max (worst, db);
        ++count;
        const bool ok = db < -100.0;
        if (! ok) ++bad;
        std::cout << fa.getFileName().paddedRight (' ', 60) << (identical ? "bit-exact" : juce::String (db, 1) + " dBFS") << (ok ? "" : "  FAIL") << "\n";
    }
    std::cout << "\n" << count << " files, worst difference " << (worst <= -400.0 ? juce::String ("none (bit-exact)") : juce::String (worst, 1) + " dBFS")
              << (bad == 0 && count > 0 ? "\nNULL TEST PASSED\n" : "\nNULL TEST FAILED\n");
    return bad == 0 && count > 0 ? 0 : 1;
}

std::vector<double> parseList (const juce::String& s)
{
    std::vector<double> v;
    for (const auto& t : juce::StringArray::fromTokens (s, ",", ""))
        if (t.trim().isNotEmpty()) v.push_back (t.trim().getDoubleValue());
    return v;
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (argv[i]);
    auto opt = [&] (const char* name, const juce::String& def) { const int i = args.indexOf (name); return i >= 0 && i + 1 < args.size() ? args[i + 1] : def; };
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    auto scenarios = makeScenarios();
    if (! args.contains ("--extra"))
        scenarios.erase (std::remove_if (scenarios.begin(), scenarios.end(), [] (const Scenario& s) { return s.extra; }), scenarios.end());
    const auto filter = opt ("--filter", {});
    if (filter.isNotEmpty())
        scenarios.erase (std::remove_if (scenarios.begin(), scenarios.end(), [&] (const Scenario& s) { return filter.startsWithChar ('=') ? s.name != filter.substring (1)
                                                                                                     : ! s.name.containsIgnoreCase (filter); }),
                         scenarios.end());

    if (args.contains ("--list")) { for (const auto& s : scenarios) std::cout << s.name << "\n"; return 0; }
    if (args.contains ("--compare"))
        return runCompare (cwd.getChildFile (opt ("--compare", ".")), cwd.getChildFile (args[args.indexOf ("--compare") + 2]));
    if (args.contains ("--null"))
        return runNull (scenarios, cwd.getChildFile (opt ("--null", "null")));

    std::vector<int> blocks;
    for (auto b : parseList (opt ("--blocks", "64,128,512"))) blocks.push_back ((int) b);
    return runBench (scenarios, opt ("--seconds", "4").getDoubleValue(), opt ("--runs", "5").getIntValue(), parseList (opt ("--rates", "48000,96000")), blocks);
}
