#pragma once
#include <juce_dsp/juce_dsp.h>
#include "ParametricEQ.h"
#include <array>
#include <cmath>

/**
    Multiband saturation (up to 6 bands), modelled on the feature set of a classic multiband saturator:
    crossovers with 6 / 12 / 24 / 48 dB/oct slopes and, per band: style, drive, mix, feedback (with
    frequency), dynamics, tone, level, mute / solo / bypass. Global: oversampling, mix and output.

    Crossovers are Linkwitz-Riley (12, 24, 48 dB/oct) or first-order complementary (6 dB/oct) with
    all-pass phase compensation, so the bands sum back to a flat response when nothing is processed.
*/
namespace sat
{
    struct StyleInfo { const char* category; const char* name; };

    inline const std::array<StyleInfo, 20>& styles()
    {
        static const std::array<StyleInfo, 20> s { {
            { "Tube", "Clean Tube" }, { "Tube", "Warm Tube" }, { "Tube", "Driven Tube" },
            { "Tape", "Clean Tape" }, { "Tape", "Warm Tape" }, { "Tape", "Worn Tape" },
            { "Transformer", "Subtle Transformer" }, { "Transformer", "Iron Transformer" },
            { "Amp", "Clean Amp" }, { "Amp", "Crunch Amp" }, { "Amp", "Lead Amp" },
            { "Clip", "Soft Clip" }, { "Clip", "Hard Clip" }, { "Clip", "Foldback" }, { "Clip", "Rectifier" },
            { "FX", "Bitcrush" }, { "FX", "Downsample" }, { "FX", "Smear" }, { "FX", "Sub Fuzz" }, { "FX", "Wreck" },
        } };
        return s;
    }

    inline juce::StringArray slopeNames()        { return { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }; }
    inline juce::StringArray oversamplingNames() { return { "Off", "2x", "4x", "8x" }; }
    inline juce::StringArray channelModeNames()  { return { "Stereo", "Mid/Side" }; }

    constexpr int maxBands = 6;

    struct BandSettings
    {
        int   style = 1;
        float driveDb = 6.0f, mix = 1.0f, feedback = 0.0f, feedbackHz = 1000.0f;
        float dynamics = 0.0f, levelDb = 0.0f;
        float bassDb = 0.0f, midDb = 0.0f, trebleDb = 0.0f, presenceDb = 0.0f;   // tone controls
        float drivePan = 0.0f, levelPan = 0.0f;                                  // -1 = left/mid ... +1 = right/side
        bool  mute = false, solo = false, bypass = false;
    };

    struct Settings
    {
        bool  enabled = false;
        int   numBands = 3;
        int   slope = 2;
        int   oversampling = 1;
        int   channelMode = 0;          // 0 = stereo (L/R), 1 = mid/side
        float mix = 1.0f, outDb = 0.0f, inDb = 0.0f;
        std::array<float, maxBands - 1> crossovers { 120.0f, 400.0f, 1200.0f, 3500.0f, 9000.0f };
        std::array<BandSettings, maxBands> bands {};
    };

    //==========================================================================
    /** Up to four cascaded biquads. */
    struct Cascade
    {
        std::array<eq::Biquad, 4> s {};
        int n = 0;
        inline float process (float x) noexcept { for (int i = 0; i < n; ++i) x = s[(size_t) i].process (x); return x; }
        void reset() { for (auto& b : s) b.reset(); }
    };

    inline eq::Coeffs firstOrder (bool lowpass, double fc, double rate) { return eq::firstOrder (! lowpass, fc, rate); }
    inline eq::Coeffs secondOrder (bool lowpass, double fc, double q, double rate) { return eq::secondOrder (! lowpass, fc, q, rate); }

    /** Designs the low-pass and high-pass halves of one crossover. */
    inline void designCrossover (int slope, double fc, double rate, Cascade& lp, Cascade& hp)
    {
        lp.n = hp.n = 0;
        auto add = [] (Cascade& c, eq::Coeffs k) { c.s[(size_t) c.n++].c = k; };
        switch (slope)
        {
            case 0: add (lp, firstOrder (true, fc, rate)); break;                       // complementary
            case 1: for (int i = 0; i < 2; ++i) { add (lp, firstOrder (true, fc, rate)); add (hp, firstOrder (false, fc, rate)); } break;
            case 2: for (int i = 0; i < 2; ++i) { add (lp, secondOrder (true, fc, 0.70710678, rate)); add (hp, secondOrder (false, fc, 0.70710678, rate)); } break;
            default:
                for (int i = 0; i < 2; ++i)
                    for (double q : { 0.54119610, 1.30656296 })
                    { add (lp, secondOrder (true, fc, q, rate)); add (hp, secondOrder (false, fc, q, rate)); }
                break;
        }
    }

    struct Crossover
    {
        Cascade lp, hp;
        int slope = 2;
        inline void split (float x, float& lo, float& hi) noexcept
        {
            lo = lp.process (x);
            if (slope == 0) hi = x - lo;                   // first-order complementary
            else            hi = (slope == 1 ? -1.0f : 1.0f) * hp.process (x);   // LR2 needs an inverted high band
        }
        void reset() { lp.reset(); hp.reset(); }
    };

    /** Same phase response as a crossover, flat magnitude (LP + HP of a Linkwitz-Riley pair). */
    struct Allpass
    {
        Cascade lp, hp;
        int slope = 2;
        inline float process (float x) noexcept
        {
            if (slope == 0) return x;
            const float a = lp.process (x), b = hp.process (x);
            return a + (slope == 1 ? -b : b);
        }
        void reset() { lp.reset(); hp.reset(); }
    };

    //==========================================================================
    /** One-pole low-pass helper. */
    struct OnePole
    {
        float z = 0.0f, a = 0.5f;
        void setCutoff (double hz, double rate) { a = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * hz / rate)); }
        inline float process (float x) noexcept { z += a * (x - z); return z; }
        void reset() { z = 0.0f; }
    };

    /** Per band, per channel state of the style processors. */
    struct StyleState
    {
        OnePole pre, post, post2, low;
        float held = 0.0f, slew = 0.0f, lastLow = 0.0f, flip = 1.0f;
        int holdCount = 0;
        void reset() { pre.reset(); post.reset(); post2.reset(); low.reset(); held = slew = lastLow = 0.0f; flip = 1.0f; holdCount = 0; }
    };

    struct StyleConstants
    {
        double rate = 48000.0;
        int downsampleHold = 8;
        float smearStep = 0.02f;
    };

    inline void configureStyleState (int style, StyleState& st, const StyleConstants& k)
    {
        const double r = k.rate;
        switch (style)
        {
            case 1:  st.post.setCutoff (12000.0, r); break;
            case 3:  st.post.setCutoff (14000.0, r); break;
            case 4:  st.post.setCutoff (8000.0, r); break;
            case 5:  st.post.setCutoff (5000.0, r); st.post2.setCutoff (7000.0, r); break;
            case 6:  st.low.setCutoff (200.0, r); break;
            case 7:  st.low.setCutoff (250.0, r); break;
            case 8:  st.pre.setCutoff (700.0, r); st.post.setCutoff (6000.0, r); break;
            case 9:  st.pre.setCutoff (700.0, r); st.post.setCutoff (5000.0, r); break;
            case 10: st.pre.setCutoff (800.0, r); st.post.setCutoff (4500.0, r); st.post2.setCutoff (4500.0, r); break;
            case 18: st.low.setCutoff (300.0, r); break;
            default: break;
        }
    }

    inline float processStyle (int style, float x, StyleState& st, const StyleConstants& k) noexcept
    {
        x = juce::jlimit (-8.0f, 8.0f, x);
        switch (style)
        {
            case 0:  return std::tanh (x + 0.12f * x * x);                                          // Clean Tube
            case 1:  return st.post.process (x >= 0.0f ? std::tanh (1.3f * x) : std::tanh (0.8f * x)); // Warm Tube
            case 2:  return std::tanh (1.5f * (std::tanh (2.0f * x + 0.3f) - 0.29131f));           // Driven Tube
            case 3:
            {
                const float c = juce::jlimit (-1.0f, 1.0f, x);
                return st.post.process (1.5f * (c - c * c * c / 3.0f));                             // Clean Tape
            }
            case 4:  return st.post.process (0.6366f * std::atan (1.8f * x));                       // Warm Tape
            case 5:  return st.post2.process (st.post.process (0.6366f * std::atan (2.5f * x)));    // Worn Tape
            case 6:                                                                                  // Subtle Transformer
            {
                const float lo = st.low.process (x);
                // lows saturate, highs pass through a soft limiter (transparent at normal levels)
                return std::tanh (x - lo) + std::tanh (1.6f * lo) / 1.6f;
            }
            case 7:                                                                                  // Iron Transformer
            {
                const float lo = st.low.process (x);
                return std::tanh (0.9f * (x - lo) + std::tanh (2.5f * lo + 0.1f) - 0.0997f);
            }
            case 8:                                                                                  // Clean Amp
            {
                const float presence = x - st.pre.process (x);
                return st.post.process (std::tanh (1.5f * (x + 0.6f * presence)));
            }
            case 9:                                                                                  // Crunch Amp
            {
                const float v = 3.0f * (x + 0.8f * (x - st.pre.process (x)));
                return st.post.process (v >= 0.0f ? std::tanh (v) : -0.85f * std::tanh (-0.8f * v));
            }
            case 10:                                                                                 // Lead Amp
            {
                const float v = 6.0f * (x + (x - st.pre.process (x)));
                return st.post2.process (st.post.process (std::tanh (2.5f * std::tanh (v))));
            }
            case 11:                                                                                 // Soft Clip
            {
                const float c = juce::jlimit (-1.0f, 1.0f, x);
                return 1.5f * (c - c * c * c / 3.0f);
            }
            case 12: return juce::jlimit (-1.0f, 1.0f, x);                                          // Hard Clip
            case 13:                                                                                 // Foldback
            {
                float t = (x + 1.0f) * 0.25f;
                t -= std::floor (t);
                return 1.0f - 4.0f * std::abs (t - 0.5f);
            }
            case 14: return std::tanh (std::abs (x)) * 1.2f;                                        // Rectifier
            case 15: return std::round (juce::jlimit (-1.0f, 1.0f, x) * 16.0f) / 16.0f;             // Bitcrush
            case 16:                                                                                 // Downsample
                if (st.holdCount-- <= 0) { st.held = x; st.holdCount = k.downsampleHold - 1; }
                return std::tanh (st.held);
            case 17:                                                                                 // Smear
                st.slew += juce::jlimit (-k.smearStep, k.smearStep, x - st.slew);
                return std::tanh (1.2f * st.slew);
            case 19:                                                                                 // Wreck: crush + rate reduction + clip
            {
                if (st.holdCount-- <= 0) { st.held = x; st.holdCount = 2 * k.downsampleHold - 1; }
                const float c = juce::jlimit (-1.0f, 1.0f, st.held * 1.5f);
                return std::round (c * 6.0f) / 6.0f;
            }
            case 18:                                                                                 // Sub Fuzz
            {
                const float lo = st.low.process (x);
                if (st.lastLow < 0.0f && lo >= 0.0f) st.flip = -st.flip;   // octave-down flip-flop
                st.lastLow = lo;
                const float fuzz = (x >= 0.0f ? 1.0f : -1.0f) * (1.0f - std::exp (-4.0f * std::abs (x)));
                return 0.6f * fuzz + 0.5f * std::tanh (3.0f * std::abs (lo)) * st.flip;
            }
            default: return x;
        }
    }

    //==========================================================================
    class MultibandSaturator
    {
    public:
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
            activeOs = -2;
            applySettings (true);
        }

        void reset()
        {
            for (auto& os : oversamplers) if (os != nullptr) os->reset();
            for (auto& ch : channels) ch.reset();
            dcX = { 0.0f, 0.0f }; dcY = { 0.0f, 0.0f };
        }

        void setSettings (const Settings& s) { settings = s; applySettings (false); }
        bool isEnabled() const noexcept { return settings.enabled; }

        float getLatencyInSamples() const
        {
            if (! settings.enabled || settings.oversampling == 0) return 0.0f;
            const auto& os = oversamplers[(size_t) settings.oversampling - 1];
            return os != nullptr ? os->getLatencyInSamples() : 0.0f;
        }

        float getAndResetBandPeak (int b) { return bandPeaks[(size_t) b].exchange (0.0f); }

        void process (float* left, float* right, int numSamples) noexcept
        {
            if (! settings.enabled)
                return;

            if (activeOs >= 0)
            {
                float* chans[] = { left, right };
                juce::dsp::AudioBlock<float> block (chans, 2, (size_t) numSamples);
                auto up = oversamplers[(size_t) activeOs]->processSamplesUp (block);
                processSamples (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
                oversamplers[(size_t) activeOs]->processSamplesDown (block);
            }
            else
                processSamples (left, right, numSamples);

            float* chans[] = { left, right };
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < numSamples; ++i)
                {
                    const float x = chans[c][i];
                    dcY[(size_t) c] = x - dcX[(size_t) c] + dcR * dcY[(size_t) c];
                    dcX[(size_t) c] = x;
                    chans[c][i] = dcY[(size_t) c];
                }
        }

    private:
        struct BandState
        {
            StyleState style;
            eq::Biquad feedbackFilter;
            std::array<eq::Biquad, 4> tone {};
            float lastOut = 0.0f, env = 0.0f, dynGain = 1.0f;
            int dynCounter = 0;
            void reset() { style.reset(); feedbackFilter.reset(); for (auto& t : tone) t.reset(); lastOut = env = 0.0f; dynGain = 1.0f; dynCounter = 0; }
        };

        struct Channel
        {
            std::array<Crossover, maxBands - 1> xover {};
            std::array<std::array<Allpass, maxBands - 1>, maxBands - 1> allpass {};
            std::array<Allpass, maxBands - 1> dryPhase {};   // the crossover network's phase, for a notch-free global Mix
            std::array<BandState, maxBands> bands {};
            void reset()
            {
                for (auto& x : xover) x.reset();
                for (auto& row : allpass) for (auto& a : row) a.reset();
                for (auto& a : dryPhase) a.reset();
                for (auto& b : bands) b.reset();
            }
        };

        void applySettings (bool force)
        {
            const int os = settings.oversampling - 1;
            const bool rateChanged = force || os != activeOs;
            if (rateChanged)
            {
                activeOs = os;
                rate = baseRate * (os >= 0 ? (1 << (os + 1)) : 1);
                if (os >= 0 && oversamplers[(size_t) os] != nullptr) oversamplers[(size_t) os]->reset();
                for (auto& ch : channels) ch.reset();
                constants.rate = rate;
                constants.downsampleHold = std::max (1, (int) std::round (rate / 8000.0));
                constants.smearStep = (float) (0.02 * 48000.0 / rate);
                for (auto* v : { &globalMix, &globalOut, &globalIn }) v->reset (rate, 0.02);
                for (int b = 0; b < maxBands; ++b)
                    for (auto* v : { &drive[(size_t) b], &mixes[(size_t) b], &levels[(size_t) b], &levelPans[(size_t) b] }) v->reset (rate, 0.02);
                glideK = (float) (1.0 - std::exp (-32.0 / (0.012 * rate)));   // ~12 ms glides, updated every 32 samples
                dynSlew = (float) (1.0 - std::exp (-8.0 / (0.004 * rate)));   // ~4 ms (dynamics gain is updated every 8 samples)
                last = Settings {};
                last.numBands = -1;
            }

            const auto& s = settings;
            auto target = [rateChanged] (juce::SmoothedValue<float>& v, float t) { if (rateChanged) v.setCurrentAndTargetValue (t); else v.setTargetValue (t); };
            target (globalMix, s.mix);
            target (globalOut, juce::Decibels::decibelsToGain (s.outDb));
            target (globalIn, juce::Decibels::decibelsToGain (s.inDb));

            // Crossovers (sorted and kept a little apart)
            std::array<float, maxBands - 1> xf {};
            float prev = 20.0f;
            for (int j = 0; j < maxBands - 1; ++j)
            {
                xf[(size_t) j] = juce::jlimit (prev * 1.05f, (float) (rate * 0.45), s.crossovers[(size_t) j]);
                prev = xf[(size_t) j];
            }
            const bool topology = rateChanged || s.slope != last.slope || s.numBands != last.numBands;
            targetXf = xf;
            if (! topology) xf = lastXf;   // a frequency change glides in processSamples (no click)
            const bool xoverChanged = topology || lastXf != xf;
            if (xoverChanged)
            {
                for (auto& ch : channels)
                {
                    for (int j = 0; j < maxBands - 1; ++j)
                    {
                        auto& x = ch.xover[(size_t) j];
                        x.slope = s.slope;
                        designCrossover (s.slope, xf[(size_t) j], rate, x.lp, x.hp);
                        for (int i = 0; i < maxBands - 1; ++i)
                        {
                            auto& a = ch.allpass[(size_t) i][(size_t) j];
                            a.slope = s.slope;
                            designCrossover (s.slope, xf[(size_t) j], rate, a.lp, a.hp);
                        }
                        auto& dp = ch.dryPhase[(size_t) j];
                        dp.slope = s.slope;
                        designCrossover (s.slope, xf[(size_t) j], rate, dp.lp, dp.hp);
                    }
                    if (s.slope != last.slope || s.numBands != last.numBands) ch.reset();
                }
                lastXf = xf;
            }

            for (int b = 0; b < maxBands; ++b)
            {
                const auto& bs = s.bands[(size_t) b];
                const auto& lb = last.bands[(size_t) b];
                target (drive[(size_t) b], juce::Decibels::decibelsToGain (bs.driveDb));
                target (mixes[(size_t) b], bs.mix);
                target (levels[(size_t) b], juce::Decibels::decibelsToGain (bs.levelDb));

                target (levelPans[(size_t) b], bs.levelPan);
                toneTarget[(size_t) b] = { bs.bassDb, bs.midDb, bs.trebleDb, bs.presenceDb };
                if (rateChanged) { toneCurrent[(size_t) b] = toneTarget[(size_t) b]; designTone (b); }
                if (rateChanged || bs.style != lb.style || bs.feedbackHz != lb.feedbackHz)
                {
                    // feedback: band-pass (constant 0 dB peak) at the feedback frequency
                    const double w0 = juce::MathConstants<double>::twoPi * std::min ((double) bs.feedbackHz, rate * 0.45) / rate;
                    const double alpha = std::sin (w0) / (2.0 * 2.0), cw = std::cos (w0), a0 = 1.0 + alpha;
                    const eq::Coeffs bp { alpha / a0, 0.0, -alpha / a0, -2.0 * cw / a0, (1.0 - alpha) / a0 };
                    for (auto& ch : channels)
                    {
                        auto& st = ch.bands[(size_t) b];
                        st.feedbackFilter.c = bp;
                        if (rateChanged || bs.style != lb.style)
                        {
                            st.style.reset();
                            configureStyleState (bs.style, st.style, constants);
                        }
                    }
                }
            }
            anySolo = false;
            for (int b = 0; b < s.numBands; ++b) anySolo = anySolo || s.bands[(size_t) b].solo;
            last = s;
        }

        /** Tone controls: bass (low shelf 120 Hz), mid (bell 800 Hz), treble (high shelf 5 kHz), presence (bell 3 kHz). */
        void designTone (int b) noexcept
        {
            const auto& g = toneCurrent[(size_t) b];
            auto bell = [this] (double f, double gainDb, double q)
            {
                eq::BandSettings bb; bb.on = true; bb.type = eq::BandType::Bell; bb.freq = (float) f; bb.gainDb = (float) gainDb; bb.q = (float) q;
                return eq::design (bb, rate).stages[0];
            };
            const std::array<eq::Coeffs, 4> c { eq::shelf (false, 120.0, g[0], 0.7, rate), bell (800.0, g[1], 0.7),
                                                eq::shelf (true, 5000.0, g[2], 0.7, rate), bell (3000.0, g[3], 1.2) };
            for (auto& ch : channels)
                for (size_t t = 0; t < 4; ++t) ch.bands[(size_t) b].tone[t].c = c[t];
        }

        void designCrossovers (const std::array<float, maxBands - 1>& xf) noexcept
        {
            for (auto& ch : channels)
                for (int j = 0; j < maxBands - 1; ++j)
                {
                    designCrossover (settings.slope, xf[(size_t) j], rate, ch.xover[(size_t) j].lp, ch.xover[(size_t) j].hp);
                    for (int i = 0; i < maxBands - 1; ++i)
                        designCrossover (settings.slope, xf[(size_t) j], rate, ch.allpass[(size_t) i][(size_t) j].lp, ch.allpass[(size_t) i][(size_t) j].hp);
                    designCrossover (settings.slope, xf[(size_t) j], rate, ch.dryPhase[(size_t) j].lp, ch.dryPhase[(size_t) j].hp);
                }
        }

        /** Every 32 samples: crossovers and tone controls glide towards their targets (no clicks when they move). */
        void glide() noexcept
        {
            const float k = glideK;
            if (lastXf != targetXf)
            {
                for (size_t j = 0; j < lastXf.size(); ++j)
                {
                    auto& cur = lastXf[j];
                    cur = std::exp (std::log (cur) + k * (std::log (targetXf[j]) - std::log (cur)));
                    if (std::abs (cur / targetXf[j] - 1.0f) < 0.002f) cur = targetXf[j];
                }
                designCrossovers (lastXf);
            }
            for (int b = 0; b < maxBands; ++b)
            {
                auto& cur = toneCurrent[(size_t) b];
                const auto& tgt = toneTarget[(size_t) b];
                if (cur == tgt) continue;
                for (size_t t = 0; t < 4; ++t)
                {
                    cur[t] += k * (tgt[t] - cur[t]);
                    if (std::abs (cur[t] - tgt[t]) < 0.02f) cur[t] = tgt[t];
                }
                designTone (b);
            }
        }

        void processSamples (float* l, float* r, int n) noexcept
        {
            const int nb = juce::jlimit (1, maxBands, settings.numBands);
            const float envAttack = (float) (1.0 - std::exp (-1.0 / (0.002 * rate)));
            const float envRelease = (float) (1.0 - std::exp (-1.0 / (0.08 * rate)));
            float* chans[] = { l, r };
            std::array<float, maxBands> bandSig {};
            std::array<float, maxBands> peaks {};

            for (int i = 0; i < n; ++i)
            {
                if ((i & 31) == 0) glide();
                std::array<float, maxBands> dr {}, mx {}, lv {}, lp {};
                for (int b = 0; b < nb; ++b)
                {
                    dr[(size_t) b] = drive[(size_t) b].getNextValue();
                    mx[(size_t) b] = mixes[(size_t) b].getNextValue();
                    lv[(size_t) b] = levels[(size_t) b].getNextValue();
                    lp[(size_t) b] = levelPans[(size_t) b].getNextValue();
                }
                const float gm = globalMix.getNextValue(), go = globalOut.getNextValue(), gi = globalIn.getNextValue();
                const bool ms = settings.channelMode == 1;

                // Input gain, then optional mid/side encoding (channel 0 = mid, channel 1 = side)
                std::array<float, 2> in { chans[0][i] * gi, chans[1][i] * gi };
                if (ms) in = { 0.5f * (in[0] + in[1]), 0.5f * (in[0] - in[1]) };
                std::array<float, 2> out {};

                for (int c = 0; c < 2; ++c)
                {
                    auto& ch = channels[(size_t) c];
                    const float input = in[(size_t) c];

                    // Split into bands
                    float rest = input;
                    for (int j = 0; j < nb - 1; ++j)
                        ch.xover[(size_t) j].split (rest, bandSig[(size_t) j], rest);
                    bandSig[(size_t) nb - 1] = rest;
                    // Phase-align lower bands with the higher crossovers
                    for (int b = 0; b < nb - 1; ++b)
                        for (int j = b + 1; j < nb - 1; ++j)
                            bandSig[(size_t) b] = ch.allpass[(size_t) b][(size_t) j].process (bandSig[(size_t) b]);

                    float sum = 0.0f;
                    for (int b = 0; b < nb; ++b)
                    {
                        const auto& bs = settings.bands[(size_t) b];
                        auto& st = ch.bands[(size_t) b];
                        const float dry = bandSig[(size_t) b];
                        float y = dry;

                        if (! bs.bypass)
                        {
                            const float a = std::abs (dry);
                            st.env += (a > st.env ? envAttack : envRelease) * (a - st.env);

                            // Dynamics (Saturn behaviour): right = compression (loud parts down, pumping),
                            // left = gating / expansion (quiet parts down).
                            if (st.dynCounter-- <= 0)
                            {
                                st.dynCounter = 7;
                                const float dynTarget = std::abs (bs.dynamics) < 0.001f ? 1.0f
                                           : juce::jlimit (0.03f, 4.0f, std::pow (juce::jmax (1.0e-4f, st.env / 0.25f), -0.85f * bs.dynamics));
                                st.dynGain += dynSlew * (dynTarget - st.dynGain);   // slewed: a Dynamics change never steps
                            }

                            // Drive with pan: shifts the drive between the channels (L/R, or M/S in mid/side mode)
                            const float panDb = 6.0f * bs.drivePan;
                            const float chDrive = dr[(size_t) b] * juce::Decibels::decibelsToGain (c == 0 ? -panDb : panDb);

                            // Feedback is reduced automatically when the band goes quiet, so it cannot ring forever
                            const float fbAmount = bs.feedback * 0.95f * juce::jmin (1.0f, st.env * 50.0f);
                            const float fb = st.feedbackFilter.process (st.lastOut) * fbAmount;
                            float w = processStyle (bs.style, dry * chDrive + fb, st.style, constants);
                            st.lastOut = w;
                            w *= 1.0f / std::sqrt (chDrive);   // automatic level compensation for drive
                            w *= st.dynGain;
                            if (std::abs (bs.bassDb) > 0.01f)     w = st.tone[0].process (w);
                            if (std::abs (bs.midDb) > 0.01f)      w = st.tone[1].process (w);
                            if (std::abs (bs.trebleDb) > 0.01f)   w = st.tone[2].process (w);
                            if (std::abs (bs.presenceDb) > 0.01f) w = st.tone[3].process (w);

                            const float pan = lp[(size_t) b];
                            const float lvPan = c == 0 ? (pan > 0.0f ? 1.0f - pan : 1.0f) : (pan < 0.0f ? 1.0f + pan : 1.0f);
                            y = (dry + mx[(size_t) b] * (w - dry)) * lv[(size_t) b] * lvPan;
                        }

                        const bool audible = ! bs.mute && (! anySolo || bs.solo);
                        if (audible)
                        {
                            sum += y;
                            peaks[(size_t) b] = std::max (peaks[(size_t) b], std::abs (y));
                        }
                    }
                    // the bands' sum carries the crossovers' all-pass phase: give the dry signal the same phase
                    // so the global Mix blends without notches around the crossover frequencies
                    float dryAligned = input;
                    for (int j = 0; j < nb - 1; ++j) dryAligned = ch.dryPhase[(size_t) j].process (dryAligned);   // always run: no state jumps when Mix moves
                    out[(size_t) c] = dryAligned + gm * (sum - dryAligned);
                }

                if (ms) out = { out[0] + out[1], out[0] - out[1] };
                chans[0][i] = out[0] * go;
                chans[1][i] = out[1] * go;
            }

            for (int b = 0; b < nb; ++b)
                if (peaks[(size_t) b] > bandPeaks[(size_t) b].load())
                    bandPeaks[(size_t) b].store (peaks[(size_t) b]);
        }

        double baseRate = 48000.0, rate = 48000.0;
        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers;
        int activeOs = -2;
        Settings settings, last;
        std::array<float, maxBands - 1> lastXf {}, targetXf {};
        std::array<std::array<float, 4>, maxBands> toneCurrent {}, toneTarget {};
        std::array<juce::SmoothedValue<float>, maxBands> levelPans;
        float glideK = 0.1f, dynSlew = 0.1f;
        StyleConstants constants;
        std::array<Channel, 2> channels {};
        juce::SmoothedValue<float> globalMix, globalOut, globalIn;
        std::array<juce::SmoothedValue<float>, maxBands> drive, mixes, levels;
        bool anySolo = false;
        float dcR = 0.999f;
        std::array<float, 2> dcX { 0.0f, 0.0f }, dcY { 0.0f, 0.0f };
        std::array<std::atomic<float>, maxBands> bandPeaks {};
    };
}
