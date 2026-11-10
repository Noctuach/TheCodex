#include "PresetManager.h"
#include "Parameters.h"
#include "DSP/ClipDistortion.h"
#include "DSP/ParametricEQ.h"

using namespace juce;

const Identifier PresetManager::presetNameProperty { "presetName" };

namespace
{
    using Values = std::vector<std::pair<String, float>>;
    using CD = ClipDistortion;
    enum Band { LowCut = 0, LowShelf, Bell, Notch, HighShelf, HighCut };

    int slopeIndex (int dbPerOct)
    {
        for (int i = 0; i < eq::numSlopes; ++i)
            if (eq::slopeDb[i] == dbPerOct) return i;
        return eq::defaultSlope;
    }

    struct Builder
    {
        Values v;
        Builder& set (const String& id, float value) { v.push_back ({ id, value }); return *this; }

        Builder& synth (int wave, float pitchAmt, float pitchDecay, float pitchCurve)
        {
            return set (ID::engine, 0).set (ID::wave, (float) wave).set (ID::pAmount, pitchAmt)
                  .set (ID::pDecay, pitchDecay).set (ID::pCurve, pitchCurve);
        }
        Builder& kick909 (int model, float tune = 0.5f, float attack = 0.5f, float decay = 0.5f)
        {
            // In 909 mode the amp ADSR is a gate: full sustain so the 909 decay shapes the tail
            return set (ID::engine, 1).set (ID::model909, (float) model).set (ID::tune909, tune)
                  .set (ID::attack909, attack).set (ID::decay909, decay).set (ID::aSustain, 1.0f);
        }
        Builder& amp (float decay, float sustain, float release, float curve = 2.0f)
        {
            return set (ID::aDecay, decay).set (ID::aSustain, sustain).set (ID::aRelease, release).set (ID::aCurve, curve);
        }
        Builder& click (float level, float decayMs, float toneHz)
        {
            return set (ID::clickLevel, level).set (ID::clickDecay, decayMs).set (ID::clickTone, toneHz);
        }
        Builder& clip (int stage, int mode, float drive, float out, float bias = 0.0f, float mix = 1.0f)
        {
            return set (ID::clip (stage, "On"), 1).set (ID::clip (stage, "Mode"), (float) mode)
                  .set (ID::clip (stage, "Drive"), drive).set (ID::clip (stage, "Out"), out)
                  .set (ID::clip (stage, "Bias"), bias).set (ID::clip (stage, "Mix"), mix);
        }
        /** Distroyer-style extras: characteristic, boost, wet lo/hi filters and shelf. */
        Builder& character (int stage, float chr, float boostDb = 0.0f, float loHz = 20000.0f, float hiHz = 20.0f,
                            float shelfHz = 6000.0f, float shelfDb = 0.0f)
        {
            return set (ID::clip (stage, "Char"), chr).set (ID::clip (stage, "Boost"), boostDb)
                  .set (ID::clip (stage, "LoFilt"), loHz).set (ID::clip (stage, "HiFilt"), hiHz)
                  .set (ID::clip (stage, "ShelfF"), shelfHz).set (ID::clip (stage, "ShelfG"), shelfDb);
        }
        Builder& clipOff (int stage) { return set (ID::clip (stage, "On"), 0); }
        /** slopeDb: 6, 12, 18, 24, 36, 48, 72 or 96 dB/oct */
        Builder& band (int n, int type, float freq, float gain = 0.0f, float q = 0.707f, int slopeDbPerOct = 24)
        {
            const int b = n - 1;
            return set (ID::band (b, "Used"), 1).set (ID::band (b, "On"), 1).set (ID::band (b, "Type"), (float) type).set (ID::band (b, "Freq"), freq)
                  .set (ID::band (b, "Gain"), gain).set (ID::band (b, "Q"), q).set (ID::band (b, "Slope"), (float) slopeIndex (slopeDbPerOct));
        }
        Builder& out (float db) { return set (ID::outGain, db); }
    };
}

PresetManager::PresetManager (AudioProcessorValueTreeState& s) : apvts (s)
{
    auto add = [this] (const String& name, Builder b) { factory.push_back ({ name, std::move (b.v) }); };

    add ("Init", Builder());

    // ---- Synth engine ----
    add ("Oldskool Raw", Builder().synth (0, 54.0f, 90.0f, 5.0f).amp (800.0f, 0.8f, 120.0f)
         .clip (1, CD::HardClip, 26.0f, -8.0f).band (1, LowCut, 28.0f).band (2, Bell, 90.0f, -6.0f, 2.0f)
         .band (3, Bell, 220.0f, 5.0f, 1.2f).band (4, Bell, 1200.0f, -4.0f, 1.5f)
         .clip (2, CD::Tanh, 10.0f, -4.0f).character (2, 0.0f, 0.0f, 20000.0f, 20.0f, 8000.0f, -4.0f).out (-2.0f));

    add ("Euphoric Tail", Builder().synth (2, 40.0f, 150.0f, 3.5f).amp (1500.0f, 0.85f, 300.0f)
         .click (0.1f, 4.0f, 3500.0f).clip (1, CD::SoftClip, 14.0f, -4.0f).band (3, Bell, 160.0f, 3.0f, 1.0f)
         .band (5, HighShelf, 6000.0f, -6.0f, 0.7f).clip (2, CD::Tube, 6.0f, -3.0f));

    add ("Early Hardstyle", Builder().synth (0, 50.0f, 110.0f, 4.0f).amp (700.0f, 0.6f, 140.0f)
         .clip (1, CD::Overdrive, 22.0f, -8.0f).band (1, LowCut, 30.0f).band (2, Notch, 120.0f, 0.0f, 4.0f)
         .band (3, Bell, 300.0f, 6.0f, 1.0f).clip (2, CD::HardClip, 8.0f, -5.0f));

    add ("Punchy Tok", Builder().synth (0, 60.0f, 45.0f, 6.0f).amp (250.0f, 0.3f, 60.0f, 3.0f)
         .click (0.45f, 2.0f, 6000.0f).clip (1, CD::HardClip, 20.0f, -7.0f).band (4, Bell, 2500.0f, 4.0f, 1.5f)
         .clip (2, CD::SoftClip, 6.0f, -4.0f));

    add ("Zaag Kick", Builder().synth (1, 48.0f, 100.0f, 4.0f).amp (900.0f, 0.8f, 150.0f)
         .clip (1, CD::TriFold, 16.0f, -6.0f, 0.15f).band (3, Bell, 400.0f, 6.0f, 0.8f)
         .clip (2, CD::HardClip, 14.0f, -8.0f));

    add ("Rotterdam Gabber", Builder().synth (0, 60.0f, 70.0f, 4.0f).amp (400.0f, 0.5f, 80.0f)
         .clip (1, CD::Fuzz, 30.0f, -10.0f).band (2, Bell, 150.0f, 8.0f, 1.0f).band (3, Bell, 900.0f, -9.0f, 0.8f)
         .clip (2, CD::HardClip, 12.0f, -6.0f));

    // ---- 909 engine ----
    add ("909 Clean", Builder().kick909 (0).set (ID::aRelease, 120.0f).clipOff (1).clipOff (2).out (-3.0f));

    add ("909 Oldskool Hardstyle", Builder().kick909 (6, 0.5f, 0.5f, 0.6f).set (ID::aRelease, 120.0f)
         .clip (1, CD::Overdrive, 20.0f, -8.0f).band (1, LowCut, 30.0f).band (2, Bell, 170.0f, -4.0f, 2.0f)
         .band (3, Bell, 383.0f, 6.0f, 1.0f).clip (2, CD::HardClip, 10.0f, -6.0f));

    add ("909 Euphoric", Builder().kick909 (1, 0.45f, 0.4f, 0.5f).set (ID::aRelease, 400.0f)
         .clip (1, CD::SoftClip, 12.0f, -5.0f).band (3, Bell, 200.0f, 3.0f, 1.0f)
         .band (5, HighShelf, 5000.0f, -6.0f, 0.7f).clip (2, CD::Tanh, 6.0f, -3.0f));

    add ("909 Rawstyle", Builder().kick909 (2, 0.55f, 0.7f, 0.55f).set (ID::aRelease, 100.0f)
         .clip (1, CD::HardClip, 28.0f, -10.0f).band (2, Bell, 50.0f, -4.0f, 1.5f).band (3, Bell, 383.0f, 6.0f, 1.0f)
         .band (4, Bell, 1800.0f, -5.0f, 2.0f).clip (2, CD::Diode, 12.0f, -6.0f)
         .character (1, 0.35f, 0.0f, 9000.0f).character (2, 0.0f, 0.0f, 20000.0f, 40.0f));

    add ("909 Gabber Overdrive", Builder().kick909 (5, 0.7f, 0.5f, 0.5f).set (ID::aRelease, 80.0f)
         .clip (1, CD::Overdrive, 34.0f, -10.0f).band (2, LowShelf, 80.0f, 6.0f, 0.7f).band (3, Bell, 800.0f, -9.0f, 0.7f)
         .clip (2, CD::HardClip, 14.0f, -7.0f).character (1, 0.5f, 6.0f, 12000.0f));

    add ("909 Tok", Builder().kick909 (9, 0.5f, 0.8f, 0.5f).set (ID::aRelease, 60.0f)
         .clip (1, CD::HardClip, 18.0f, -7.0f).band (4, Bell, 3000.0f, 5.0f, 1.2f).clip (2, CD::SoftClip, 4.0f, -3.0f));

    add ("909 Boom Tail", Builder().kick909 (7, 0.5f, 0.4f, 0.7f).set (ID::aRelease, 500.0f)
         .clip (1, CD::Tube, 16.0f, -6.0f).band (1, LowCut, 25.0f, 0.0f, 0.707f, 48).clip (2, CD::Tanh, 8.0f, -4.0f));

    add ("909 Fuzz Box", Builder().kick909 (8).set (ID::aRelease, 150.0f)
         .clip (1, CD::Fuzz, 22.0f, -10.0f).band (3, Bell, 250.0f, 5.0f, 1.0f).clip (2, CD::AsymClip, 10.0f, -6.0f));

    add ("909 Bitcrushed", Builder().kick909 (0, 0.6f).set (ID::aRelease, 150.0f)
         .clip (1, CD::Bitcrush, 12.0f, -6.0f, 0.0f, 0.6f).band (5, HighCut, 9000.0f, 0.0f, 0.707f, 24)
         .clip (2, CD::Tanh, 10.0f, -5.0f));

    add ("909 Hard Attack Raw", Builder().kick909 (4, 0.5f, 0.9f, 0.45f).set (ID::aRelease, 90.0f)
         .clip (1, CD::Diode, 24.0f, -9.0f, 0.1f).band (1, LowCut, 32.0f).band (3, Bell, 450.0f, 5.0f, 1.2f)
         .band (4, Notch, 1500.0f, 0.0f, 3.0f).clip (2, CD::HardClip, 12.0f, -6.0f).character (1, 0.25f));

    // ---- Stereo / saturation showcases ----
    // Spatial distortion for width, with a Side low cut so everything below 150 Hz stays mono
    add ("909 Wide Rawstyle", Builder().kick909 (2, 0.55f, 0.7f, 0.55f).set (ID::aRelease, 100.0f)
         .clip (1, CD::HardClip, 26.0f, -10.0f).set (ID::clip (1, "Spatial"), 0.5f)
         .band (1, LowCut, 150.0f, 0.0f, 0.707f, 48).set (ID::band (0, "Place"), 4.0f)   // band 1: Side low cut
         .band (3, Bell, 383.0f, 6.0f, 1.0f).band (4, Bell, 1800.0f, -5.0f, 2.0f)
         .clip (2, CD::Diode, 12.0f, -6.0f).set (ID::clip (2, "Spatial"), 0.3f));

    // Multiband saturation: warm lows, taped mids, driven top
    add ("Saturated Euphoric", Builder().synth (2, 40.0f, 150.0f, 3.5f).amp (1500.0f, 0.85f, 300.0f)
         .click (0.1f, 4.0f, 3500.0f).clip (1, CD::SoftClip, 12.0f, -4.0f).band (3, Bell, 160.0f, 3.0f, 1.0f)
         .clip (2, CD::Tube, 4.0f, -3.0f)
         .set (ID::sat ("On"), 1.0f).set (ID::sat ("Bands"), 3.0f).set (ID::sat ("X1"), 150.0f).set (ID::sat ("X2"), 2500.0f)
         .set (ID::satBand (0, "Style"), 1.0f).set (ID::satBand (0, "Drive"), 8.0f)
         .set (ID::satBand (1, "Style"), 4.0f).set (ID::satBand (1, "Drive"), 12.0f).set (ID::satBand (1, "Dyn"), 0.3f)
         .set (ID::satBand (2, "Style"), 9.0f).set (ID::satBand (2, "Drive"), 6.0f).set (ID::satBand (2, "Treble"), -4.0f)
         .set (ID::satBand (2, "Level"), -6.0f));

    // ---- Showcases for the newer features (appended: program numbers of older presets stay the same) ----
    auto drawEnv = [] (Builder& b, char env, std::initializer_list<std::array<float, 3>> pts)
    {
        b.set (ID::envMode (env), 1.0f).set (ID::envPoints (env), (float) pts.size());
        int i = 0;
        for (auto& p : pts)
        {
            b.set (ID::envPoint (env, 'T', i), p[0]).set (ID::envPoint (env, 'L', i), p[1]).set (ID::envPoint (env, 'B', i), p[2]);
            ++i;
        }
    };

    {   // resonant band-pass before a hard clip: the distortion screams around 220 Hz
        Builder b; b.synth (0, 52.0f, 70.0f, 5.0f).amp (650.0f, 0.75f, 110.0f);
        drawEnv (b, 'p', { { 0, 1.0f, 0.85f }, { 12, 0.3f, 0.6f }, { 70, 0.05f, 0.2f }, { 200, 0.0f, 0.0f } });
        b.clip (1, CD::HardClip, 30.0f, -10.0f).set (ID::clip (1, "FltPos"), 1.0f).set (ID::clip (1, "FltType"), 2.0f)
         .set (ID::clip (1, "FltFreq"), 220.0f).set (ID::clip (1, "FltRes"), 0.55f)
         .band (1, LowCut, 30.0f).band (3, Bell, 400.0f, 4.0f, 1.0f).band (4, Bell, 1600.0f, -4.0f, 1.5f)
         .clip (2, CD::Diode2, 12.0f, -6.0f).set (ID::clip (2, "FltPos"), 2.0f).set (ID::clip (2, "FltType"), 1.0f)
         .set (ID::clip (2, "FltFreq"), 9000.0f).set (ID::clip (2, "FltRes"), 0.1f)
         .set (ID::cmp ("On"), 1.0f).set (ID::cmp ("Style"), 1.0f).set (ID::cmpBand (0, "Thr"), -14.0f).set (ID::cmpBand (0, "Ratio"), 3.0f)
         .set (ID::cmpBand (0, "Att"), 8.0f).set (ID::cmpBand (0, "Rel"), 90.0f);
        add ("Screech Rawstyle", b);
    }
    {   // drawn transfer curve with a fold: hollow, zaag-like body
        Builder b; b.synth (1, 46.0f, 100.0f, 4.0f).amp (900.0f, 0.8f, 150.0f)
         .clip (1, CD::XShaper, 14.0f, -6.0f).band (3, Bell, 350.0f, 5.0f, 0.9f).clip (2, CD::HardClip, 12.0f, -7.0f);
        const float ys[] = { 0.55f, 0.95f, 0.75f, 0.35f, 0.6f, 0.95f, 1.0f, 1.0f };
        for (int k = 0; k < 8; ++k) b.set (ID::clip (1, ("XP" + String (k + 1)).toRawUTF8()), ys[k]);
        add ("X-Shaped Zaag", b);
    }
    {   // long euphoric tail with a hall around it; the low end stays dry and mono
        Builder b; b.synth (2, 38.0f, 160.0f, 3.5f).amp (1600.0f, 0.85f, 320.0f).click (0.1f, 4.0f, 3500.0f)
         .clip (1, CD::SoftClip, 12.0f, -4.0f).band (3, Bell, 160.0f, 3.0f, 1.0f).clip (2, CD::Tube, 6.0f, -3.0f)
         .set (ID::spc ("On"), 1.0f).set (ID::spc ("Mode"), 3.0f).set (ID::spc ("Mix"), 0.16f).set (ID::spc ("Decay"), 2.2f)
         .set (ID::spc ("Width"), 1.5f).set (ID::spc ("LowCut"), 280.0f).set (ID::spc ("PreDelay"), 18.0f);
        add ("Euphoric Wide Hall", b);
    }
    {   // upward compression on the mids brings the grit of the tail forward
        Builder b; b.kick909 (2, 0.55f, 0.6f, 0.6f).set (ID::aRelease, 120.0f)
         .clip (1, CD::Overdrive, 22.0f, -8.0f).band (3, Bell, 380.0f, 5.0f, 1.0f).clip (2, CD::HardClip, 10.0f, -6.0f)
         .set (ID::cmp ("On"), 1.0f).set (ID::cmp ("Bands"), 3.0f).set (ID::cmp ("X1"), 150.0f).set (ID::cmp ("X2"), 2500.0f)
         .set (ID::cmpBand (1, "Dir"), 1.0f).set (ID::cmpBand (1, "Thr"), -24.0f).set (ID::cmpBand (1, "Ratio"), 3.0f).set (ID::cmpBand (1, "Range"), 12.0f)
         .set (ID::cmpBand (0, "Thr"), -12.0f).set (ID::cmpBand (0, "Ratio"), 2.5f);
        add ("Upward Tail Crunch", b);
    }
    {   // drawn pitch and amp envelopes, post low-pass on the second distortion
        Builder b; b.synth (0, 48.0f, 100.0f, 4.0f);
        drawEnv (b, 'p', { { 0, 1.0f, 0.7f }, { 18, 0.5f, -0.2f }, { 60, 0.42f, 0.6f }, { 190, 0.0f, 0.0f } });
        drawEnv (b, 'a', { { 0, 0.0f, 0.0f }, { 1, 1.0f, 0.6f }, { 60, 0.7f, 0.0f }, { 700, 0.65f, 0.4f }, { 1300, 0.0f, 0.0f } });
        b.clip (1, CD::Tanh, 24.0f, -8.0f).band (1, LowCut, 28.0f).band (2, Bell, 110.0f, -4.0f, 2.0f).band (3, Bell, 300.0f, 6.0f, 1.0f)
         .clip (2, CD::HardClip, 12.0f, -6.0f).set (ID::clip (2, "FltPos"), 2.0f).set (ID::clip (2, "FltType"), 1.0f)
         .set (ID::clip (2, "FltFreq"), 6000.0f).set (ID::clip (2, "FltRes"), 0.3f);
        add ("Drawn Modern Hardstyle", b);
    }
    {   // sample-rate reduction blended in: gritty hardcore crunch
        Builder b; b.synth (0, 60.0f, 60.0f, 5.0f).amp (450.0f, 0.5f, 90.0f)
         .clip (1, CD::Downsample, 20.0f, -4.0f, 0.0f, 0.5f).band (2, Bell, 150.0f, 6.0f, 1.0f)
         .clip (2, CD::HardClip, 16.0f, -8.0f);
        add ("Downsampled Hardcore", b);
    }

    // Output levels matched to about -3 dBFS peak in the key of F (measured with tools/CodexRender),
    // so switching presets does not jump in volume.
    const std::map<String, float> outputLevels {
        { "Oldskool Raw", 0.3f }, { "Euphoric Tail", -1.3f }, { "Early Hardstyle", 0.4f }, { "Punchy Tok", -1.8f },
        { "Zaag Kick", 1.4f }, { "Rotterdam Gabber", -0.2f }, { "909 Clean", -3.7f }, { "909 Oldskool Hardstyle", 1.4f },
        { "909 Euphoric", -0.9f }, { "909 Rawstyle", -0.8f }, { "909 Gabber Overdrive", 1.6f }, { "909 Tok", -3.3f },
        { "909 Boom Tail", -0.2f }, { "909 Fuzz Box", 1.7f }, { "909 Bitcrushed", 1.9f }, { "909 Hard Attack Raw", -0.1f },
        { "909 Wide Rawstyle", 3.0f }, { "Saturated Euphoric", -4.0f },
        { "Screech Rawstyle", 0.6f }, { "X-Shaped Zaag", 0.7f }, { "Euphoric Wide Hall", -1.0f }, { "Upward Tail Crunch", -8.4f },
        { "Drawn Modern Hardstyle", -0.6f }, { "Downsampled Hardcore", 2.3f } };
    for (auto& preset : factory)
        if (auto it = outputLevels.find (preset.name); it != outputLevels.end())
        {
            auto& v = preset.values;
            v.erase (std::remove_if (v.begin(), v.end(), [] (const auto& kv) { return kv.first == ID::outGain; }), v.end());
            v.push_back ({ ID::outGain, it->second });
        }

    refreshUserPresets();
    if (apvts.state.getProperty (presetNameProperty).toString().isEmpty())
        setCurrentName ("Init");
    markClean();
}

StringArray PresetManager::getCategories()
{
    return { "Init", "Hardstyle", "Rawstyle", "Euphoric", "Early Hardstyle", "Hardcore", "Tok", "FX" };
}

String PresetManager::getFactoryCategory (int index) const
{
    if (index < 0 || index >= (int) factory.size()) return {};
    static const std::map<String, String> categories {
        { "Init", "Init" }, { "Oldskool Raw", "Rawstyle" }, { "Euphoric Tail", "Euphoric" }, { "Early Hardstyle", "Early Hardstyle" },
        { "Punchy Tok", "Tok" }, { "Zaag Kick", "Hardstyle" }, { "Rotterdam Gabber", "Hardcore" }, { "909 Clean", "Init" },
        { "909 Oldskool Hardstyle", "Early Hardstyle" }, { "909 Euphoric", "Euphoric" }, { "909 Rawstyle", "Rawstyle" },
        { "909 Gabber Overdrive", "Hardcore" }, { "909 Tok", "Tok" }, { "909 Boom Tail", "Hardstyle" }, { "909 Fuzz Box", "FX" },
        { "909 Bitcrushed", "FX" }, { "909 Hard Attack Raw", "Rawstyle" }, { "909 Wide Rawstyle", "Rawstyle" },
        { "Saturated Euphoric", "Euphoric" }, { "Screech Rawstyle", "Rawstyle" }, { "X-Shaped Zaag", "Hardstyle" },
        { "Euphoric Wide Hall", "Euphoric" }, { "Upward Tail Crunch", "Hardstyle" }, { "Drawn Modern Hardstyle", "Hardstyle" },
        { "Downsampled Hardcore", "Hardcore" } };
    const auto& name = factory[(size_t) index].name;
    if (auto it = categories.find (name); it != categories.end()) return it->second;
    return "Hardstyle";
}

StringArray PresetManager::getFactoryNames() const
{
    StringArray names;
    for (const auto& p : factory) names.add (p.name);
    return names;
}

File PresetManager::getUserDirectory() const
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Codex").getChildFile ("Presets");
    dir.createDirectory();
    return dir;
}

StringArray PresetManager::getUserNames() const
{
    const ScopedLock sl (userNamesLock);
    return userNames;
}

void PresetManager::refreshUserPresets()
{
    StringArray names;
    for (const auto& f : getUserDirectory().findChildFiles (File::findFiles, false, "*.cdxpreset"))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    const ScopedLock sl (userNamesLock);
    userNames.swapWith (names);
}

bool PresetManager::deleteUserPreset (const String& name)
{
    const bool ok = getUserDirectory().getChildFile (name + ".cdxpreset").deleteFile();
    refreshUserPresets();
    if (ok && getCurrentName() == name)
        setCurrentName ("Init");
    return ok;
}

bool PresetManager::isKeyParameter (const String& id) const
{
    return id == ID::key || id == ID::octave || id == ID::keyMode;
}

void PresetManager::setValue (const String& id, float value)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

void PresetManager::resetToDefaults()
{
    for (auto* p : apvts.processor.getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            if (! isKeyParameter (rp->paramID))
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (rp->getDefaultValue());
                rp->endChangeGesture();
            }
}

void PresetManager::setCurrentName (const String& name)
{
    apvts.state.setProperty (presetNameProperty, name, apvts.undoManager);
}

String PresetManager::getCurrentName() const
{
    return apvts.state.getProperty (presetNameProperty, "Init").toString();
}

void PresetManager::markClean()
{
    cleanValues.clear();
    for (auto* p : apvts.processor.getParameters())
        cleanValues.push_back (p->getValue());
}

bool PresetManager::isModified() const
{
    const auto& params = apvts.processor.getParameters();
    if (cleanValues.size() != (size_t) params.size()) return false;
    for (int i = 0; i < params.size(); ++i)
    {
        auto* rp = dynamic_cast<RangedAudioParameter*> (params[i]);
        if (rp != nullptr && isKeyParameter (rp->paramID)) continue;
        if (std::abs (params[i]->getValue() - cleanValues[(size_t) i]) > 1.0e-5f) return true;
    }
    return false;
}

void PresetManager::loadFactory (int index)
{
    if (! isPositiveAndBelow (index, (int) factory.size())) return;
    resetToDefaults();
    for (const auto& [id, value] : factory[(size_t) index].values)
        setValue (id, value);
    setCurrentName (factory[(size_t) index].name);
    markClean();
}

void PresetManager::loadUser (const String& name)
{
    auto xml = parseXML (getUserDirectory().getChildFile (name + ".cdxpreset"));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // Apply every parameter except the key settings
    const auto tree = ValueTree::fromXml (*xml);
    for (auto* p : apvts.processor.getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
        {
            if (isKeyParameter (rp->paramID)) continue;
            const auto child = tree.getChildWithProperty ("id", rp->paramID);
            if (child.isValid())
                setValue (rp->paramID, (float) child.getProperty ("value"));
        }
    setCurrentName (name);
    markClean();
}

bool PresetManager::saveUserPreset (const String& rawName)
{
    const auto name = File::createLegalFileName (rawName.trim());
    if (name.isEmpty()) return false;
    setCurrentName (name);
    markClean();
    auto state = apvts.copyState();
    bool ok = false;
    if (auto xml = state.createXml())
        ok = xml->writeTo (getUserDirectory().getChildFile (name + ".cdxpreset"));
    refreshUserPresets();
    return ok;
}

void PresetManager::loadPreset (int combinedIndex)
{
    if (auto* um = apvts.undoManager)
        um->beginNewTransaction ("Load preset");
    if (combinedIndex < getNumFactory())
        loadFactory (combinedIndex);
    else
    {
        const auto users = getUserNames();
        const int u = combinedIndex - getNumFactory();
        if (isPositiveAndBelow (u, users.size()))
            loadUser (users[u]);
    }
}

int PresetManager::getCurrentIndex() const
{
    const auto name = getCurrentName();
    const auto users = getUserNames();
    const int u = users.indexOf (name);
    if (u >= 0) return getNumFactory() + u;
    return getFactoryNames().indexOf (name);
}

void PresetManager::step (int delta)
{
    const int n = getNumPresets();
    if (n == 0) return;
    const int current = getCurrentIndex();
    loadPreset (((current < 0 ? 0 : current + delta) % n + n) % n);
}
