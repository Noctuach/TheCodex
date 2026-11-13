#pragma once
#include <juce_dsp/juce_dsp.h>
#include "ParametricEQ.h"
#include "StereoOversampling.h"
#include <cmath>
#include <cstdint>
#include <cstring>

/**
    Distortion stage.

    Signal path (inside the oversampled domain):
        Boost (dry + wet)
          ├── dry ──────────────────────────────────────────────────────────────┐
          └── wet: Lo Filter (low-pass) -> [PRE filter] -> Drive -> Type curve   │
                   (+Offset) -> Characteristic -> [POST filter]                 Mix -> Output
                   -> Hi Filter (high-pass) -> High shelf (tone) ───────────────┘
        -> optional DC filter

    The PRE / POST filter is a resonant zero-delay state-variable filter (LP 12/24, BP, HP 12/24, Notch),
    the classic synth-distortion workflow: a resonant band-pass before the curve decides which part of the
    kick gets distorted, a low-pass after it tames the fizz.
    X-Shaper modes use a drawable curve (9 points per half, Catmull-Rom interpolated).
*/
class ClipDistortion
{
public:
    // New modes are appended so saved projects and presets keep their mode.
    enum Mode { HardClip = 0, SoftClip, Tanh, Overdrive, Tube, Diode, Fuzz, SineFold, TriFold, Rectify, Bitcrush, AsymClip,
                Diode2, ZeroSquare, Downsample, XShaper, XShaperAsym, numModes };

    static juce::StringArray modeNames()
    {
        return { "Hard Clip", "Soft Clip", "Tanh", "Overdrive", "Tube", "Diode",
                 "Fuzz", "Sine Fold", "Tri Fold", "Rectify", "Bitcrush", "Asym Clip",
                 "Diode 2", "Zero-Square", "Downsample", "X-Shaper", "X-Shaper (Asym)" };
    }

    /** "Raw" = no oversampling and no anti-aliasing at all: the deliberately gritty, aliasing sound of classic
        digital drum machines (Microtonic's distortion aliases on purpose for the same reason). */
    static juce::StringArray oversamplingNames() { return { "Off", "2x", "4x", "8x", "16x", "Raw" }; }
    static constexpr int rawOversampling = 5;

    enum FilterType { LowPass12 = 0, LowPass24, BandPass, HighPass12, HighPass24, Notch };
    static juce::StringArray filterTypeNames() { return { "Low Pass 12", "Low Pass 24", "Band Pass", "High Pass 12", "High Pass 24", "Notch" }; }
    static juce::StringArray filterPositionNames() { return { "Off", "Pre", "Post" }; }

    /** Drawable shaper curve: y at x = 0, 1/8 ... 1 for the positive half (and the negative half for the asym mode). */
    static constexpr int numCurvePoints = 8;
    struct Curve
    {
        std::array<float, numCurvePoints> pos { 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f };
        std::array<float, numCurvePoints> neg { 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f };
        bool operator== (const Curve& o) const { return pos == o.pos && neg == o.neg; }

        /** Catmull-Rom through (0,0), (1/8, p0) ... (1, p7); held flat beyond x = 1. */
        static float evalHalf (const std::array<float, numCurvePoints>& p, float ax) noexcept
        {
            if (ax >= 1.0f) return p[numCurvePoints - 1];
            const float t = ax * (float) numCurvePoints;
            const int i = juce::jlimit (0, numCurvePoints - 1, (int) t);
            const float f = t - (float) i;
            auto pt = [&p] (int k) { return k <= 0 ? (k == 0 ? 0.0f : -p[0]) : p[(size_t) juce::jmin (k, numCurvePoints) - 1]; };
            const float y0 = pt (i - 1), y1 = pt (i), y2 = pt (i + 1), y3 = i + 2 <= numCurvePoints ? pt (i + 2) : 2.0f * pt (i + 1) - pt (i);
            return y1 + 0.5f * f * (y2 - y0 + f * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + f * (3.0f * (y1 - y2) + y3 - y0)));
        }
        float eval (float x, bool asym) const noexcept
        {
            return x >= 0.0f ? evalHalf (pos, x) : -evalHalf (asym ? neg : pos, -x);
        }
    };

    struct Settings
    {
        bool  enabled = true;
        int   mode = HardClip;
        float driveDb = 0.0f, boostDb = 0.0f, offset = 0.0f, character = 0.0f;
        float mix = 1.0f, outDb = 0.0f;
        float loFilterHz = 20000.0f, hiFilterHz = 20.0f, shelfHz = 6000.0f, shelfDb = 0.0f;
        bool  dcFilter = true;
        bool  autoGain = false;   // keep the unit's output level matched to its input (Drive changes colour, not loudness)
        int   oversampling = 2; // index into oversamplingNames()
        float spatial = 0.0f;   // 0 = identical L/R, 1 = different distortion per channel (stereo)
        int   filterPos = 0, filterType = LowPass12;   // filterPos: 0 = off, 1 = pre, 2 = post
        float filterHz = 1000.0f, filterRes = 0.2f;
        Curve curve;
        // Punch / tail split: the drive moves by tailDriveDb from the hit to the tail over splitMs after each note-on
        float tailDriveDb = 0.0f, splitMs = 60.0f;
    };

    /** Drive change (dB) at a time after the hit: 0 at the hit, tailDriveDb after splitMs (smoothstep). */
    static float tailDriveAt (float tailDriveDb, float splitMs, float msSinceHit) noexcept
    {
        const float u = juce::jlimit (0.0f, 1.0f, msSinceHit / juce::jmax (1.0f, splitMs));
        return tailDriveDb * u * u * (3.0f - 2.0f * u);
    }

    void prepare (double sr, int maxBlockSize)
    {
        baseRate = sr;
        tailMul.assign ((size_t) juce::jmax (1, maxBlockSize), 1.0f);
        autoIn.assign ((size_t) juce::jmax (1, maxBlockSize), 0.0f);
        autoCoef = (float) (1.0 - std::exp (-1.0 / (1.0 * sr)));
        msIn = msOut = 0.0f;
        autoGainNow = 1.0f;
        fixedTable (0);                                   // build the shared ADAA tables here, never on the audio thread
        curveTable.build ([] (float x) { return juce::jlimit (-1.0f, 1.0f, x); }, false);   // allocate the X-Shaper table up front
        for (int i = 0; i < 4; ++i)
        {
            oversamplers[(size_t) i] = std::make_unique<StereoOversampling> (i + 1);
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
        for (auto& f : svf) f.reset();
        dcX = { 0.0f, 0.0f }; dcY = { 0.0f, 0.0f };
        holdCount = { 0, 0 }; holdValue = { 0.0f, 0.0f };
        for (auto& pm : phase) pm.reset();
        adaaPrev = { 0.0, 0.0 }; adaaPrevF = { 0.0, 0.0 };
    }

    /** Latency in base-rate samples for the current oversampling setting. */
    float getLatencyInSamples() const
    {
        const int i = settings.oversampling == rawOversampling ? -1 : settings.oversampling - 1;
        return i >= 0 && oversamplers[(size_t) i] != nullptr ? oversamplers[(size_t) i]->getLatencyInSamples() : 0.0f;
    }

    void setSettings (const Settings& s)
    {
        settings = s;
        applySettings (false);
    }

    /** hitMs (optional): milliseconds since the last note-on for every sample (punch / tail split). */
    void process (float* left, float* right, int numSamples, const float* hitMs = nullptr) noexcept
    {
        // per-sample drive multiplier for the punch / tail split (computed at the base rate)
        tailActive = hitMs != nullptr && (std::abs (settings.tailDriveDb) > 0.01f || tailDb.isSmoothing()) && numSamples <= (int) tailMul.size();
        if (tailActive)
            for (int i = 0; i < numSamples; ++i)   // tail drive glides too (no step when the knob moves mid-note)
                tailMul[(size_t) i] = juce::Decibels::decibelsToGain (tailDriveAt (tailDb.getNextValue(), settings.splitMs, hitMs[i]));
        else
            tailDb.skip (numSamples);

        float inPk = 0.0f;
        if (settings.autoGain && numSamples <= (int) autoIn.size())
            for (int i = 0; i < numSamples; ++i) autoIn[(size_t) i] = 0.5f * (left[i] + right[i]);   // the unit's input (before Boost)
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

        // Auto Gain: slow (about 1 s) mean-square trackers of the unit's input and output; the output is scaled so its
        // long-term level matches the input (times the Output knob). Slow enough not to squash the punch within a hit.
        if (settings.autoGain && settings.enabled)
        {
            const float target = juce::Decibels::decibelsToGain (settings.outDb);
            for (int i = 0; i < numSamples; ++i)
            {
                const float inMs = 0.5f * (autoIn[(size_t) i] * autoIn[(size_t) i]);
                const float outMs = 0.5f * (left[i] * left[i] + right[i] * right[i]) * 0.5f;
                msIn += autoCoef * (inMs - msIn);
                msOut += autoCoef * (outMs - msOut);
                if (msIn > 1.0e-7f && msOut > 1.0e-9f)
                {
                    const float want = juce::jlimit (0.06f, 16.0f, std::sqrt (msIn / msOut) * target);
                    autoGainNow += 0.0005f * (want - autoGainNow);
                }
                left[i] *= autoGainNow;
                right[i] *= autoGainNow;
            }
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
    static float transfer (int mode, float x, float driveGain, float offset, float character, const Curve* curve = nullptr) noexcept
    {
        if (mode == Downsample) driveGain = 1.0f;   // in Downsample mode, Drive sets the rate reduction
        const float shaped = shapeFor (mode, x * driveGain + offset, curve) - shapeFor (mode, offset, curve);
        return harden (shaped, character);
    }

    //==========================================================================
    /**
        Antiderivative anti-aliasing (first order, Parker et al. 2016): instead of f(v[n]) the stage outputs
        (F(v[n]) - F(v[n-1])) / (v[n] - v[n-1]), where F is the integral of the curve. This removes most of the
        aliasing that clipping corners create, on top of oversampling. F is tabulated once per curve
        (cubic Hermite between nodes), extended linearly beyond the table for saturating curves and wrapped
        for the periodic folds.
    */
    struct AdaaTable
    {
        static constexpr double range = 16.0;
        static constexpr int size = 8192;
        std::vector<double> F, f;
        bool periodic = false;
        double period = 4.0, h = 0.0;

        /** Allocates once; rebuilding later (e.g. while an X-Shaper point is dragged) does not allocate. */
        template <typename Fn>
        void build (Fn&& fn, bool isPeriodic)
        {
            periodic = isPeriodic;
            const double lo = periodic ? -0.5 * period : -range, hi = periodic ? 0.5 * period : range;
            h = (hi - lo) / (size - 1);
            if ((int) F.size() != size) { F.resize (size); f.resize (size); }
            std::fill (F.begin(), F.end(), 0.0);
            for (int i = 0; i < size; ++i) f[(size_t) i] = fn ((float) (lo + h * i));
            // trapezoid integration from the centre so F(0) = 0
            const int mid = (size - 1) / 2;
            for (int i = mid + 1; i < size; ++i) F[(size_t) i] = F[(size_t) i - 1] + 0.5 * h * (f[(size_t) i - 1] + f[(size_t) i]);
            for (int i = mid - 1; i >= 0; --i)    F[(size_t) i] = F[(size_t) i + 1] - 0.5 * h * (f[(size_t) i + 1] + f[(size_t) i]);
            const double offsetAtZero = F[(size_t) mid] + (0.0 - (lo + h * mid)) * f[(size_t) mid];
            for (auto& v : F) v -= offsetAtZero;
        }

        double eval (double v) const noexcept
        {
            const double lo = periodic ? -0.5 * period : -range, hi = periodic ? 0.5 * period : range;
            if (periodic)
            {
                v = std::fmod (v - lo, period);
                if (v < 0.0) v += period;
                v += lo;
            }
            else if (v >= hi) return F.back() + f.back() * (v - hi);
            else if (v <= lo) return F.front() + f.front() * (v - lo);
            const double t = (v - lo) / h;
            const int i = juce::jlimit (0, size - 2, (int) t);
            const double u = t - i;
            // cubic Hermite with the curve itself as the derivative
            const double u2 = u * u, u3 = u2 * u;
            return (2 * u3 - 3 * u2 + 1) * F[(size_t) i] + (u3 - 2 * u2 + u) * h * f[(size_t) i]
                 + (-2 * u3 + 3 * u2) * F[(size_t) i + 1] + (u3 - u2) * h * f[(size_t) i + 1];
        }
    };

    static bool isPeriodicMode (int m) noexcept { return m == SineFold || m == TriFold; }

    /** Shared tables for the fixed curves (built once, on first use). */
    static const AdaaTable& fixedTable (int m)
    {
        static const std::vector<AdaaTable> tables = []
        {
            std::vector<AdaaTable> t ((size_t) numModes);
            for (int i = 0; i < numModes; ++i)
                if (i != XShaper && i != XShaperAsym)
                    t[(size_t) i].build ([i] (float x) { return shapeFor (i, x); }, isPeriodicMode (i));
            return t;
        }();
        return tables[(size_t) juce::jlimit (0, (int) numModes - 1, m)];
    }

    /** Downsample mode: hold factor (at the base sample rate) for a drive setting in dB (0-48). */
    static float downsampleFactor (float driveDb) noexcept { return 1.0f + std::pow (juce::jlimit (0.0f, 1.0f, driveDb / 48.0f), 1.5f) * 47.0f; }

    /** Characteristic: 0 = curve unchanged ... 1 = pushed towards a rectangle wave. */
    static float harden (float y, float character) noexcept
    {
        if (character <= 0.0f) return y;
        const float g = 1.0f + 40.0f * character * character;
        return juce::jlimit (-1.0f, 1.0f, y * g);
    }

    static float shapeFor (int m, float x, const Curve* curve = nullptr) noexcept
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
            case Diode2:    // symmetric diode pair: soft knee, then a firm ceiling (odd harmonics, "buzz")
                return x / std::pow (1.0f + std::pow (std::abs (x), 2.5f), 0.4f);
            case ZeroSquare:   // square law around zero (crossover gap) with a clipped top: hollow, gritty
            {
                const float c = juce::jlimit (-1.0f, 1.0f, x);
                return c * std::abs (c);
            }
            case Downsample: return juce::jlimit (-1.0f, 1.0f, x);
            case XShaper:     return curve != nullptr ? curve->eval (x, false) : juce::jlimit (-1.0f, 1.0f, x);
            case XShaperAsym: return curve != nullptr ? curve->eval (x, true) : juce::jlimit (-1.0f, 1.0f, x);
            default:        return x;
        }
    }

    /** Peak levels since the last call (for the GUI meters). */
    float getAndResetInputPeak()  { return inPeak.exchange (0.0f); }
    float getAndResetOutputPeak() { return outPeak.exchange (0.0f); }

private:
    void applySettings (bool force)
    {
        ++shapeVersion;   // mode / curve may change below: every shapeFor memo misses once
        const int os = settings.oversampling == rawOversampling ? -1 : settings.oversampling - 1; // -1 = off
        rawMode = settings.oversampling == rawOversampling;
        const bool rateChanged = os != activeOs || force;
        activeOs = os;
        const double rate = baseRate * (os >= 0 ? (1 << (os + 1)) : 1);
        osFactor = os >= 0 ? (1 << (os + 1)) : 1;
        curRate = rate;

        if (rateChanged)
        {
            if (os >= 0 && oversamplers[(size_t) os] != nullptr)
                oversamplers[(size_t) os]->reset();
            for (auto* s : { &drive, &mix, &outGain, &offset, &character, &spatial })
                s->reset (rate, 0.02);
            filterCutoff.reset (rate, 0.03);
            for (auto& f : svf) f.reset();
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
        setTarget (filterCutoff, juce::jlimit (20.0f, (float) (rate * 0.45), s.filterHz), force || rateChanged);
        filterPos = s.enabled ? s.filterPos : 0;
        filterType = s.filterType;
        // resonance 0..1 -> Q 0.5 .. 18 (quadratic, so most of the knob is musical)
        filterK = 1.0f / (0.5f + 17.5f * s.filterRes * s.filterRes);
        // the resonant peak is Q (= 1/k); scale it so full resonance peaks around +12 dB
        resComp = 1.0f / std::sqrt (juce::jmax (1.0f, 1.0f / filterK) / 4.0f + 0.75f);
        if (! (s.curve == curve) || ! curveTableBuilt || s.mode != mode)
        {
            curve = s.curve;
            if (s.mode == XShaper || s.mode == XShaperAsym)
            {
                const bool asym = s.mode == XShaperAsym;
                const Curve c = curve;
                curveTable.build ([c, asym] (float x) { return c.eval (x, asym); }, false);
                curveTableBuilt = true;
                tableChanged = true;
            }
        }
        downsampleHold = downsampleFactor (s.driveDb) * (float) osFactor;

        // Wet filters (redesigned only when something changed)
        // Wet filters glide to new settings (see glideFilters) so moving them never clicks; a rate change snaps them.
        tgtLo = s.loFilterHz; tgtHi = s.hiFilterHz; tgtShelfHz = s.shelfHz; tgtShelfDb = s.shelfDb;
        tailDb.setTargetValue (s.tailDriveDb);
        if (rate != lastFilterRate || force)
        {
            curLo = tgtLo; curHi = tgtHi; curShelfHz = tgtShelfHz; curShelfDb = tgtShelfDb;
            tailDb.reset (baseRate, 0.03);
            tailDb.setCurrentAndTargetValue (s.tailDriveDb);
            glideStep = (float) (1.0 - std::exp (-16.0 / (0.012 * rate)));   // ~12 ms glide, updated every 16 samples
            designFilters (rate);
            lastFilterRate = rate;
        }
        if (s.mode != mode || tableChanged)
        {
            // keep the ADAA history consistent with the new curve (no click when switching modes)
            const AdaaTable& tbl = (s.mode == XShaper || s.mode == XShaperAsym) ? curveTable : fixedTable (s.mode);
            for (int ch = 0; ch < 2; ++ch) adaaPrevF[(size_t) ch] = tbl.eval (adaaPrev[(size_t) ch]);
            tableChanged = false;
        }
        mode = s.mode;
    }

    /** Moves the wet filters a step towards their targets (every 16 samples while gliding) and redesigns them. */
    void glideFilters() noexcept
    {
        if (curLo == tgtLo && curHi == tgtHi && curShelfHz == tgtShelfHz && curShelfDb == tgtShelfDb) return;
        auto glideLog = [this] (float& cur, float tgt)
        {
            cur = std::exp (std::log (cur) + glideStep * (std::log (tgt) - std::log (cur)));
            if (std::abs (cur / tgt - 1.0f) < 0.002f) cur = tgt;
        };
        glideLog (curLo, tgtLo);
        glideLog (curHi, tgtHi);
        glideLog (curShelfHz, tgtShelfHz);
        curShelfDb += glideStep * (tgtShelfDb - curShelfDb);
        if (std::abs (curShelfDb - tgtShelfDb) < 0.01f) curShelfDb = tgtShelfDb;
        designFilters (curRate);
    }

    void designFilters (double rate) noexcept
    {
        eq::BandSettings sh; sh.on = true; sh.type = eq::BandType::HighShelf; sh.freq = curShelfHz; sh.gainDb = curShelfDb;
        const auto shelfC = eq::design (sh, rate).stages[0];
        for (int ch = 0; ch < 2; ++ch)
        {
            phase[ch].setCutoffs (rate, curLo, curHi);
            shelf[ch].c = shelfC;
        }
        loActive = curLo < 19900.0f;
        hiActive = curHi > 20.5f;
        shelfActive = std::abs (curShelfDb) > 0.01f;
    }

    template <typename Smoothed>
    static void setTarget (Smoothed& v, float target, bool snap)
    {
        if (snap) v.setCurrentAndTargetValue (target);
        else      v.setTargetValue (target);
    }

    /** Zero-delay-feedback state-variable filter (Cytomic / Simper), stable under fast modulation. */
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        void reset() { ic1 = ic2 = 0.0f; }
        /** Returns low, band (unity peak), high and notch in one pass. */
        inline void run (float x, float a1, float a2, float a3, float k, float& low, float& band, float& high) noexcept
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            low = v2; band = v1; high = x - k * v1 - v2;
        }
    };

    inline float runFilter (int ch, float x) noexcept
    {
        float lo, bp, hi;
        const bool steep = filterType == LowPass24 || filterType == HighPass24;
        // 24 dB: a Butterworth stage followed by the resonant stage (one resonant peak, like an analog cascade)
        svf[(size_t) ch * 2].run (x, steep ? fb1 : fa1, steep ? fb2 : fa2, steep ? fb3 : fa3, steep ? 1.41421356f : filterK, lo, bp, hi);
        float y = x;
        switch (filterType)
        {
            case LowPass12:  y = lo; break;
            case LowPass24:  { float l2, b2, h2; svf[(size_t) ch * 2 + 1].run (lo, fa1, fa2, fa3, filterK, l2, b2, h2); y = l2; break; }
            case BandPass:   return bp * filterK;
            case HighPass12: y = hi; break;
            case HighPass24: { float l2, b2, h2; svf[(size_t) ch * 2 + 1].run (hi, fa1, fa2, fa3, filterK, l2, b2, h2); y = h2; break; }
            case Notch:      return lo + hi;
            default:         return x;
        }
        return y * resComp;   // resonance gain compensation keeps the peak musical instead of ear-splitting
    }

    void updateFilterCoefficients (float hz) noexcept
    {
        const float g = std::tan (juce::MathConstants<float>::pi * juce::jmin (hz, (float) (curRate * 0.45)) / (float) curRate);
        fa1 = 1.0f / (1.0f + g * (g + filterK));
        fa2 = g * fa1;
        fa3 = g * fa2;
        fb1 = 1.0f / (1.0f + g * (g + 1.41421356f));
        fb2 = g * fb1;
        fb3 = g * fb2;
    }

    void processSamples (float* l, float* r, int n) noexcept
    {
        float* chans[] = { l, r };
        // mode and curve only change in applySettings (between calls)
        const AdaaTable* adaaTable = (mode == XShaper || mode == XShaperAsym) ? &curveTable : &fixedTable (mode);
        const bool adaa = mode != Downsample && mode != Bitcrush && ! rawMode;
        if (filterPos != 0) updateFilterCoefficients (filterCutoff.getCurrentValue());
        for (int i = 0; i < n; ++i)
        {
            if ((i & 15) == 0) glideFilters();
            const float d = drive.getNextValue() * (tailActive ? tailMul[(size_t) (i / osFactor)] : 1.0f);
            const float off = offset.getNextValue(), chr = character.getNextValue();
            const float sp = spatial.getNextValue(), m = mix.getNextValue(), og = outGain.getNextValue();
            if (filterPos != 0 && filterCutoff.isSmoothing()) updateFilterCoefficients (filterCutoff.getNextValue());

            // Spatial: the two channels hit slightly different curves (drive and operating point),
            // which creates stereo harmonics from a mono input.
            const float driveSpread = 1.0f + 0.6f * sp, offsetSpread = 0.3f * sp;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float in = chans[ch][i];
                const float chDrive  = ch == 0 ? d * driveSpread : d / driveSpread;
                const float chOffset = ch == 0 ? off + offsetSpread : off - offsetSpread;
                auto& pm = phase[ch];
                // Dry path, phase-matched to the wet path so the Mix knob never comb-filters: the same all-pass phase as
                // the Linkwitz-Riley Lo / Hi filters, plus the half-sample delay of the anti-aliasing. A bypassed unit stays untouched.
                float dry = in;
                if (settings.enabled)
                {
                    if (loActive) dry = pm.allpassLo (dry);
                    if (hiActive) dry = pm.allpassHi (dry);
                    if (adaa)     dry = pm.halfSample (dry);
                }
                float w = loActive ? pm.lowPassLR2 (in) : in;
                if (filterPos == 1) w = runFilter (ch, w);
                if (mode == Downsample)
                {
                    // sample-and-hold rate reduction (Drive sets the amount), then a gentle clip
                    if (--holdCount[(size_t) ch] <= 0)
                    {
                        holdValue[(size_t) ch] = w;
                        holdCount[(size_t) ch] = juce::jmax (1, (int) (downsampleHold * (ch == 0 ? 1.0f : 1.0f + 0.15f * sp)));
                    }
                    w = holdValue[(size_t) ch];
                }
                if (! adaa)   // Downsample / Bitcrush (deliberately stepped) or Raw: the plain curve, aliasing and all
                    w = transfer (mode, w, chDrive, chOffset, chr, &curve);
                else
                {
                    // ADAA: average of the curve between the previous and the current input (via its integral)
                    const AdaaTable& tbl = *adaaTable;
                    const double v = (double) w * chDrive + chOffset;
                    const double dv = v - adaaPrev[(size_t) ch];
                    const double Fv = tbl.eval (v);
                    float y;
                    if (std::abs (dv) > 1.0e-4)
                        y = (float) ((Fv - adaaPrevF[(size_t) ch]) / dv);
                    else
                        y = midShape[(size_t) ch].get (mode, (float) (0.5 * (v + adaaPrev[(size_t) ch])), &curve, shapeVersion);
                    adaaPrev[(size_t) ch] = v;
                    adaaPrevF[(size_t) ch] = Fv;
                    w = harden (y - offShape[(size_t) ch].get (mode, chOffset, &curve, shapeVersion), chr);
                }
                if (filterPos == 2) w = runFilter (ch, w);
                if (hiActive)    w = pm.highPassLR2 (w);
                if (shelfActive) w = shelf[ch].process (w);
                chans[ch][i] = (dry + m * (w - dry)) * og;
            }
        }
    }

    double baseRate = 44100.0;
    std::array<std::unique_ptr<StereoOversampling>, 4> oversamplers;
    std::vector<float> tailMul, autoIn;
    juce::SmoothedValue<float> tailDb;
    float tgtLo = 20000.0f, tgtHi = 20.0f, tgtShelfHz = 6000.0f, tgtShelfDb = 0.0f;
    float curLo = 20000.0f, curHi = 20.0f, curShelfHz = 6000.0f, curShelfDb = 0.0f, glideStep = 0.05f;
    float autoCoef = 0.0f, msIn = 0.0f, msOut = 0.0f, autoGainNow = 1.0f;
    bool rawMode = false;
    bool tailActive = false;
    int osFactor = 1;
    double curRate = 44100.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> filterCutoff { 1000.0f };
    std::array<Svf, 4> svf;   // [channel * 2 + stage]
    int filterPos = 0, filterType = 0;
    float filterK = 1.0f, fa1 = 0.0f, fa2 = 0.0f, fa3 = 0.0f, fb1 = 0.0f, fb2 = 0.0f, fb3 = 0.0f, resComp = 1.0f;
    Curve curve;
    AdaaTable curveTable;
    bool curveTableBuilt = false, tableChanged = false;
    std::array<double, 2> adaaPrev { 0.0, 0.0 }, adaaPrevF { 0.0, 0.0 };

    /** shapeFor (mode, x, curve) is a pure function: reuse the last result while the argument (bit for bit), the mode and
        the curve are unchanged (the offset term is constant unless Offset / Spatial move; in silence both ADAA calls are). */
    struct ShapeMemo
    {
        std::uint32_t xBits = 0; std::uint64_t version = 0;   // version 0 never matches (shapeVersion starts at 1)
        float value = 0.0f;
        inline float get (int m, float x, const Curve* c, std::uint64_t ver) noexcept
        {
            std::uint32_t bits;
            std::memcpy (&bits, &x, sizeof (bits));
            if (bits != xBits || ver != version)
            {
                value = shapeFor (m, x, c);
                xBits = bits;
                version = ver;
            }
            return value;
        }
    };
    std::array<ShapeMemo, 2> offShape {}, midShape {};
    std::uint64_t shapeVersion = 1;   // 64-bit: bumped every block, must never wrap
    float downsampleHold = 1.0f;
    std::array<int, 2> holdCount { 0, 0 };
    std::array<float, 2> holdValue { 0.0f, 0.0f };
    int activeOs = -1;
    int mode = HardClip;
    Settings settings;

    juce::SmoothedValue<float> drive, boost, mix, outGain, offset, character, spatial;
    eq::Biquad loFilter[2], hiFilter[2], shelf[2];

    /**
        Linkwitz-Riley (2nd order) Lo / Hi filters for the wet path and the matching all-passes for the dry path.
        LR2 low-pass and the inverted LR2 high-pass both have exactly the phase of a 1st-order all-pass at the same
        cutoff, so dry and wet stay in phase at every frequency and the Mix knob blends without notches.
    */
    struct PhaseMatch
    {
        struct OnePole { float G = 0.5f, s = 0.0f; float lp (float x) noexcept { const float v = (x - s) * G; const float y = v + s; s = y + v; return y; } };
        OnePole lo1, lo2, loAp, hi1, hi2, hiAp;
        float frac1 = 0.0f, fracY = 0.0f;

        void setCutoffs (double rate, float loHz, float hiHz)
        {
            auto g = [rate] (float hz)
            {
                const double t = std::tan (juce::MathConstants<double>::pi * juce::jlimit (5.0, rate * 0.49, (double) hz) / rate);
                return (float) (t / (1.0 + t));
            };
            lo1.G = lo2.G = loAp.G = g (loHz);
            hi1.G = hi2.G = hiAp.G = g (hiHz);
        }
        void reset() { for (auto* p : { &lo1, &lo2, &loAp, &hi1, &hi2, &hiAp }) p->s = 0.0f; frac1 = fracY = 0.0f; }

        float lowPassLR2 (float x) noexcept  { return lo2.lp (lo1.lp (x)); }
        float highPassLR2 (float x) noexcept { const float a = x - hi1.lp (x); return -(a - hi2.lp (a)); }   // inverted: in phase with the all-pass
        float allpassLo (float x) noexcept   { return 2.0f * loAp.lp (x) - x; }
        float allpassHi (float x) noexcept   { return 2.0f * hiAp.lp (x) - x; }
        /** 1st-order Thiran all-pass: half a sample of delay, flat magnitude (matches first-order ADAA's delay). */
        float halfSample (float x) noexcept
        {
            constexpr float a = 1.0f / 3.0f;
            const float y = a * x + frac1 - a * fracY;
            frac1 = x; fracY = y;
            return y;
        }
    };
    std::array<PhaseMatch, 2> phase;
    bool loActive = false, hiActive = false, shelfActive = false;
    double lastFilterRate = 0.0;
    float lastLo = -1.0f, lastHi = -1.0f, lastShelfHz = -1.0f, lastShelfDb = -100.0f;

    float dcR = 0.999f;
    std::array<float, 2> dcX { 0.0f, 0.0f }, dcY { 0.0f, 0.0f };
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
};
