#pragma once
#include <juce_dsp/juce_dsp.h>
#include "ParametricEQ.h"
#include <cmath>

/**
    Distortion stage.

    Signal path (inside the oversampled domain):
        Boost (dry + wet)
          ├── dry ─────────────────────────────────────────────────────────┐
          └── wet: Lo Filter (low-pass) -> Drive -> Type curve (+Offset)    │
                   -> Characteristic (morph towards a rectangle)           Mix -> Output
                   -> Hi Filter (high-pass) -> High shelf (tone) ──────────┘
        -> optional DC filter
*/
class ClipDistortion
{
public:
    enum Mode { HardClip = 0, SoftClip, Tanh, Overdrive, Tube, Diode, Fuzz, SineFold, TriFold, Rectify, Bitcrush, AsymClip };

    static juce::StringArray modeNames()
    {
        return { "Hard Clip", "Soft Clip", "Tanh", "Overdrive", "Tube", "Diode",
                 "Fuzz", "Sine Fold", "Tri Fold", "Rectify", "Bitcrush", "Asym Clip" };
    }

    static juce::StringArray oversamplingNames() { return { "Off", "2x", "4x", "8x" }; }

    struct Settings
    {
        bool  enabled = true;
        int   mode = HardClip;
        float driveDb = 0.0f, boostDb = 0.0f, offset = 0.0f, character = 0.0f;
        float mix = 1.0f, outDb = 0.0f;
        float loFilterHz = 20000.0f, hiFilterHz = 20.0f, shelfHz = 6000.0f, shelfDb = 0.0f;
        bool  dcFilter = true;
        int   oversampling = 2; // index into oversamplingNames()
        float spatial = 0.0f;   // 0 = identical L/R, 1 = different distortion per channel (stereo)
    };

    void prepare (double sr, int maxBlockSize)
    {
        baseRate = sr;
        for (int i = 0; i < 3; ++i)
        {
            oversamplers[(size_t) i] = std::make_unique<juce::dsp::Oversampling<float>> (
                2, i + 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
            oversamplers[(size_t) i]->initProcessing ((size_t) maxBlockSize);
        }
        dcR = (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sr);
        activeOs = -1;           // force (re)configuration
        applySettings (true);
        reset();
    }

    void reset()
    {
        for (auto& os : oversamplers)
            if (os != nullptr) os->reset();
        for (int ch = 0; ch < 2; ++ch)
            for (auto* f : { &loFilter[ch], &hiFilter[ch], &shelf[ch] }) f->reset();
        dcX = { 0.0f, 0.0f }; dcY = { 0.0f, 0.0f };
    }

    /** Latency in base-rate samples for the current oversampling setting. */
    float getLatencyInSamples() const
    {
        const int i = settings.oversampling - 1;
        return i >= 0 && oversamplers[(size_t) i] != nullptr ? oversamplers[(size_t) i]->getLatencyInSamples() : 0.0f;
    }

    void setSettings (const Settings& s)
    {
        settings = s;
        applySettings (false);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        float inPk = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = boost.getNextValue();
            left[i] *= g;
            right[i] *= g;
            inPk = std::max (inPk, std::max (std::abs (left[i]), std::abs (right[i])));
        }

        if (activeOs >= 0)
        {
            float* channels[] = { left, right };
            juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
            auto up = oversamplers[(size_t) activeOs]->processSamplesUp (block);
            processSamples (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
            oversamplers[(size_t) activeOs]->processSamplesDown (block);
        }
        else
        {
            processSamples (left, right, numSamples);
        }

        float outPk = 0.0f;
        float* chans[] = { left, right };
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
            {
                float& v = chans[ch][i];
                if (settings.dcFilter)
                {
                    const float x = v;
                    dcY[(size_t) ch] = x - dcX[(size_t) ch] + dcR * dcY[(size_t) ch];
                    dcX[(size_t) ch] = x;
                    v = dcY[(size_t) ch];
                }
                outPk = std::max (outPk, std::abs (v));
            }

        if (inPk > inPeak.load())   inPeak.store (inPk);
        if (outPk > outPeak.load()) outPeak.store (outPk);
    }

    /** Wet transfer curve for one input sample (excluding filters, mix and output); also used by the GUI display. */
    static float transfer (int mode, float x, float driveGain, float offset, float character) noexcept
    {
        const float shaped = shapeFor (mode, x * driveGain + offset) - shapeFor (mode, offset);
        return harden (shaped, character);
    }

    /** Characteristic: 0 = curve unchanged ... 1 = pushed towards a rectangle wave. */
    static float harden (float y, float character) noexcept
    {
        if (character <= 0.0f) return y;
        const float g = 1.0f + 40.0f * character * character;
        return juce::jlimit (-1.0f, 1.0f, y * g);
    }

    static float shapeFor (int m, float x) noexcept
    {
        using juce::MathConstants;
        switch (m)
        {
            case HardClip:  return juce::jlimit (-1.0f, 1.0f, x);
            case SoftClip:
                if (x >= 1.0f)  return 1.0f;
                if (x <= -1.0f) return -1.0f;
                return 1.5f * (x - x * x * x / 3.0f);
            case Tanh:      return std::tanh (x);
            case Overdrive: return std::atan (x * 1.5f) * (2.0f / MathConstants<float>::pi);
            case Tube:      // asymmetric: negative half saturates earlier -> even harmonics
                return x >= 0.0f ? std::tanh (x) : std::tanh (x * 1.6f) * 0.75f;
            case Diode:     // forward-biased diode curve, strongly asymmetric
                return x >= 0.0f ? 1.0f - std::exp (-1.5f * x) : -0.6f * (1.0f - std::exp (1.2f * x));
            case Fuzz:      // very fast saturation towards a square wave
                return (x >= 0.0f ? 1.0f : -1.0f) * (1.0f - std::exp (-4.0f * std::abs (x)));
            case SineFold:  return std::sin (x * MathConstants<float>::halfPi);
            case TriFold:
            {
                float t = (x + 1.0f) * 0.25f;
                t -= std::floor (t);
                return 1.0f - 4.0f * std::abs (t - 0.5f);
            }
            case Rectify:   return std::tanh (std::abs (x)); // full-wave: adds an octave-up buzz
            case Bitcrush:
            {
                const float c = juce::jlimit (-1.0f, 1.0f, x);
                return std::round (c * 8.0f) / 8.0f;
            }
            case AsymClip:  return juce::jlimit (-0.55f, 1.0f, x);
            default:        return x;
        }
    }

    /** Peak levels since the last call (for the GUI meters). */
    float getAndResetInputPeak()  { return inPeak.exchange (0.0f); }
    float getAndResetOutputPeak() { return outPeak.exchange (0.0f); }

private:
    void applySettings (bool force)
    {
        const int os = settings.oversampling - 1; // -1 = off
        const bool rateChanged = os != activeOs || force;
        activeOs = os;
        const double rate = baseRate * (os >= 0 ? (1 << (os + 1)) : 1);

        if (rateChanged)
        {
            if (os >= 0 && oversamplers[(size_t) os] != nullptr)
                oversamplers[(size_t) os]->reset();
            for (auto* s : { &drive, &mix, &outGain, &offset, &character, &spatial })
                s->reset (rate, 0.02);
            boost.reset (baseRate, 0.02);
            for (int ch = 0; ch < 2; ++ch)
                for (auto* f : { &loFilter[ch], &hiFilter[ch], &shelf[ch] }) f->reset();
            lastFilterRate = 0.0;
        }

        // Always run the full path when bypassed so latency stays constant; just mix in 0 % wet at 0 dB.
        const auto& s = settings;
        setTarget (drive, juce::Decibels::decibelsToGain (s.driveDb), force || rateChanged);
        setTarget (boost, s.enabled ? juce::Decibels::decibelsToGain (s.boostDb) : 1.0f, force || rateChanged);
        setTarget (mix, s.enabled ? s.mix : 0.0f, force || rateChanged);
        setTarget (outGain, s.enabled ? juce::Decibels::decibelsToGain (s.outDb) : 1.0f, force || rateChanged);
        setTarget (offset, s.offset, force || rateChanged);
        setTarget (character, s.character, force || rateChanged);
        setTarget (spatial, s.spatial, force || rateChanged);

        // Wet filters (redesigned only when something changed)
        if (rate != lastFilterRate || s.loFilterHz != lastLo || s.hiFilterHz != lastHi || s.shelfHz != lastShelfHz || s.shelfDb != lastShelfDb)
        {
            eq::BandSettings lp; lp.on = true; lp.type = eq::BandType::HighCut; lp.freq = s.loFilterHz; lp.slope = 0;
            eq::BandSettings hp; hp.on = true; hp.type = eq::BandType::LowCut;  hp.freq = s.hiFilterHz; hp.slope = 0;
            eq::BandSettings sh; sh.on = true; sh.type = eq::BandType::HighShelf; sh.freq = s.shelfHz; sh.gainDb = s.shelfDb;
            for (int ch = 0; ch < 2; ++ch)
            {
                loFilter[ch].c = eq::design (lp, rate).stages[0];
                hiFilter[ch].c = eq::design (hp, rate).stages[0];
                shelf[ch].c    = eq::design (sh, rate).stages[0];
            }
            loActive = s.loFilterHz < 19900.0f;
            hiActive = s.hiFilterHz > 20.5f;
            shelfActive = std::abs (s.shelfDb) > 0.01f;
            lastFilterRate = rate; lastLo = s.loFilterHz; lastHi = s.hiFilterHz; lastShelfHz = s.shelfHz; lastShelfDb = s.shelfDb;
        }
        mode = s.mode;
    }

    static void setTarget (juce::SmoothedValue<float>& v, float target, bool snap)
    {
        if (snap) v.setCurrentAndTargetValue (target);
        else      v.setTargetValue (target);
    }

    void processSamples (float* l, float* r, int n) noexcept
    {
        float* chans[] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            const float d = drive.getNextValue(), off = offset.getNextValue(), chr = character.getNextValue();
            const float sp = spatial.getNextValue(), m = mix.getNextValue(), og = outGain.getNextValue();

            // Spatial: the two channels hit slightly different curves (drive and operating point),
            // which creates stereo harmonics from a mono input.
            const float driveSpread = 1.0f + 0.6f * sp, offsetSpread = 0.3f * sp;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float dry = chans[ch][i];
                const float chDrive  = ch == 0 ? d * driveSpread : d / driveSpread;
                const float chOffset = ch == 0 ? off + offsetSpread : off - offsetSpread;
                float w = loActive ? loFilter[ch].process (dry) : dry;
                w = transfer (mode, w, chDrive, chOffset, chr);
                if (hiActive)    w = hiFilter[ch].process (w);
                if (shelfActive) w = shelf[ch].process (w);
                chans[ch][i] = (dry + m * (w - dry)) * og;
            }
        }
    }

    double baseRate = 44100.0;
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers;
    int activeOs = -1;
    int mode = HardClip;
    Settings settings;

    juce::SmoothedValue<float> drive, boost, mix, outGain, offset, character, spatial;
    eq::Biquad loFilter[2], hiFilter[2], shelf[2];
    bool loActive = false, hiActive = false, shelfActive = false;
    double lastFilterRate = 0.0;
    float lastLo = -1.0f, lastHi = -1.0f, lastShelfHz = -1.0f, lastShelfDb = -100.0f;

    float dcR = 0.999f;
    std::array<float, 2> dcX { 0.0f, 0.0f }, dcY { 0.0f, 0.0f };
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
};
