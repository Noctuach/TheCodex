#pragma once
#include <juce_dsp/juce_dsp.h>
#include "ParametricEQ.h"
#include "StereoOversampling.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined (__SSE2__) || defined (_M_X64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <emmintrin.h>
 #define CODEX_SAT_SSE2 1
#else
 #define CODEX_SAT_SSE2 0
#endif

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

    forcedinline float processStyle (int style, float x, StyleState& st, const StyleConstants& k) noexcept
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
    /**
        Stereo cascades for the saturator's crossover network, run over a sub-block with the two channels (L / R or
        M / S) in the two lanes of one SSE2 double register. Both channels' filters always share their coefficients
        (every design path writes the same values to both), so lane c holds channel c's state. Each lane does exactly
        what eq::Biquad::process does: x converted to double, y = b0*x + s1, s1 = (b1*x - a1*y) + s2,
        s2 = b2*x - a2*y as separate multiplies and adds, y rounded to float before the next stage. SSE2 double
        arithmetic is IEEE (no FMA, no extended precision), so the result is bit-identical to the scalar code.
        Other targets run the scalar Biquad per channel (same expressions, so the compiler contracts them as before).
        Signals are interleaved: x[2 * i + c].
    */
    namespace stereo
    {
       #if CODEX_SAT_SSE2
        struct Stage
        {
            __m128d b0, b1, b2, a1, a2, s1, s2;
            void load (const eq::Biquad& l, const eq::Biquad& r) noexcept
            {
                b0 = _mm_set1_pd (l.c.b0); b1 = _mm_set1_pd (l.c.b1); b2 = _mm_set1_pd (l.c.b2);
                a1 = _mm_set1_pd (l.c.a1); a2 = _mm_set1_pd (l.c.a2);
                s1 = _mm_set_pd (r.s1, l.s1); s2 = _mm_set_pd (r.s2, l.s2);
            }
            void store (eq::Biquad& l, eq::Biquad& r) const noexcept
            {
                _mm_storel_pd (&l.s1, s1); _mm_storeh_pd (&r.s1, s1);
                _mm_storel_pd (&l.s2, s2); _mm_storeh_pd (&r.s2, s2);
            }
            /** One sample of both lanes; x and the result are float values held as doubles. */
            inline __m128d run (__m128d x) noexcept
            {
                const __m128d y = _mm_add_pd (_mm_mul_pd (b0, x), s1);
                s1 = _mm_add_pd (_mm_sub_pd (_mm_mul_pd (b1, x), _mm_mul_pd (a1, y)), s2);
                s2 = _mm_sub_pd (_mm_mul_pd (b2, x), _mm_mul_pd (a2, y));
                return _mm_cvtps_pd (_mm_cvtpd_ps (y));   // (float) y, per lane
            }
        };

        inline __m128d load2 (const float* p) noexcept { return _mm_cvtps_pd (_mm_castpd_ps (_mm_load_sd (reinterpret_cast<const double*> (p)))); }
        inline void store2 (float* p, __m128d v) noexcept { _mm_store_sd (reinterpret_cast<double*> (p), _mm_castps_pd (_mm_cvtpd_ps (v))); }

        /** ya = A (x) and, if Dual, yb = B (x), where A and B are N-stage cascades (lane c = channel c). */
        template <int N, bool Dual>
        inline void runN (Cascade& aL, Cascade& aR, Cascade* bL, Cascade* bR, const float* x, float* ya, float* yb, int len) noexcept
        {
            Stage A[N], Bs[Dual ? N : 1];
            for (int k = 0; k < N; ++k) A[k].load (aL.s[(size_t) k], aR.s[(size_t) k]);
            if constexpr (Dual) for (int k = 0; k < N; ++k) Bs[k].load (bL->s[(size_t) k], bR->s[(size_t) k]);
            for (int i = 0; i < len; ++i)
            {
                const __m128d xv = load2 (x + 2 * i);
                __m128d va = xv;
                for (int k = 0; k < N; ++k) va = A[k].run (va);
                store2 (ya + 2 * i, va);
                if constexpr (Dual)
                {
                    __m128d vb = xv;
                    for (int k = 0; k < N; ++k) vb = Bs[k].run (vb);
                    store2 (yb + 2 * i, vb);
                }
            }
            for (int k = 0; k < N; ++k) A[k].store (aL.s[(size_t) k], aR.s[(size_t) k]);
            if constexpr (Dual) for (int k = 0; k < N; ++k) Bs[k].store (bL->s[(size_t) k], bR->s[(size_t) k]);
        }
       #endif

        /** ya = A (x), and yb = B (x) when bL / bR are given (B has the same number of stages as A). */
        inline void run (Cascade& aL, Cascade& aR, Cascade* bL, Cascade* bR, const float* x, float* ya, float* yb, int len) noexcept
        {
            jassert (aL.n == aR.n && (bL == nullptr || (bL->n == aL.n && bR->n == aL.n)));
           #if CODEX_SAT_SSE2
            const bool dual = bL != nullptr;
            switch (aL.n)
            {
                case 0:  break;
                case 1:  dual ? runN<1, true> (aL, aR, bL, bR, x, ya, yb, len) : runN<1, false> (aL, aR, bL, bR, x, ya, yb, len); return;
                case 2:  dual ? runN<2, true> (aL, aR, bL, bR, x, ya, yb, len) : runN<2, false> (aL, aR, bL, bR, x, ya, yb, len); return;
                case 3:  dual ? runN<3, true> (aL, aR, bL, bR, x, ya, yb, len) : runN<3, false> (aL, aR, bL, bR, x, ya, yb, len); return;
                case 4:  dual ? runN<4, true> (aL, aR, bL, bR, x, ya, yb, len) : runN<4, false> (aL, aR, bL, bR, x, ya, yb, len); return;
                default: break;
            }
           #endif
            Cascade* as[] = { &aL, &aR };
            Cascade* bs[] = { bL, bR };
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < len; ++i)
                {
                    ya[2 * i + c] = as[c]->process (x[2 * i + c]);
                    if (bs[c] != nullptr) yb[2 * i + c] = bs[c]->process (x[2 * i + c]);
                }
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
                oversamplers[(size_t) i] = std::make_unique<StereoOversampling> (i + 1);
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
            // memo of the dynamics std::pow: the last arguments (compared bit for bit) and result
            std::uint32_t powBaseBits = 0, powExpBits = 0;
            float powResult = 0.0f;
            bool powValid = false;
            void reset() { style.reset(); feedbackFilter.reset(); for (auto& t : tone) t.reset(); lastOut = env = 0.0f; dynGain = 1.0f; dynCounter = 0; powValid = false; }

            /** std::pow (base, e), reusing the previous result when both arguments are bit-identical (pure function). */
            float powMemo (float base, float e) noexcept
            {
                std::uint32_t bb, eb;
                std::memcpy (&bb, &base, sizeof (bb));
                std::memcpy (&eb, &e, sizeof (eb));
                if (! powValid || bb != powBaseBits || eb != powExpBits)
                {
                    powResult = std::pow (base, e);
                    powBaseBits = bb; powExpBits = eb; powValid = true;
                }
                return powResult;
            }
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

        /**
            The audio is processed in sub-blocks of up to 32 samples, starting at sample 0, 32, 64 ... of each call (the
            glide runs before each one, as it did every 32 samples). Within a sub-block every filter runs stage by stage
            over all its samples with both channels in one SIMD register, and each band's nonlinear part runs in a loop
            of its own; every sample goes through exactly the same operations as in a plain per-sample loop, so the
            output is unchanged (bit for bit).
        */
        void processSamples (float* l, float* r, int n) noexcept
        {
            const int nb = juce::jlimit (1, maxBands, settings.numBands);
            const float envAttack = (float) (1.0 - std::exp (-1.0 / (0.002 * rate)));
            const float envRelease = (float) (1.0 - std::exp (-1.0 / (0.08 * rate)));
            const bool ms = settings.channelMode == 1;
            std::array<float, maxBands> peaks {};
            // a drive smoother that is not ramping now stays constant for this whole call (targets change between calls)
            for (int b = 0; b < nb; ++b) driveConstant[(size_t) b] = ! drive[(size_t) b].isSmoothing();

            for (int i0 = 0; i0 < n; i0 += subBlock)
            {
                const int len = std::min (subBlock, n - i0);
                glide();

                // smoothers: each advanced once per sample (bands >= nb are not advanced, as before)
                for (int b = 0; b < nb; ++b)
                    for (int i = 0; i < len; ++i)
                    {
                        drBuf[(size_t) b][(size_t) i] = drive[(size_t) b].getNextValue();
                        mxBuf[(size_t) b][(size_t) i] = mixes[(size_t) b].getNextValue();
                        lvBuf[(size_t) b][(size_t) i] = levels[(size_t) b].getNextValue();
                        lpBuf[(size_t) b][(size_t) i] = levelPans[(size_t) b].getNextValue();
                    }
                for (int i = 0; i < len; ++i)
                {
                    gmBuf[(size_t) i] = globalMix.getNextValue();
                    goBuf[(size_t) i] = globalOut.getNextValue();
                    giBuf[(size_t) i] = globalIn.getNextValue();
                }

                // Input gain, then optional mid/side encoding (channel 0 = mid, channel 1 = side)
                float* in = inBuf.data();
                for (int i = 0; i < len; ++i)
                {
                    const float gi = giBuf[(size_t) i];
                    float a = l[i0 + i] * gi, b = r[i0 + i] * gi;
                    if (ms) { const float m = 0.5f * (a + b), sd = 0.5f * (a - b); a = m; b = sd; }
                    in[2 * i] = a; in[2 * i + 1] = b;
                }

                auto& c0 = channels[0];
                auto& c1 = channels[1];
                const int len2 = 2 * len;

                // Split into bands
                float* rest = restBuf.data();
                std::copy (in, in + len2, rest);
                for (int j = 0; j < nb - 1; ++j)
                {
                    auto& x0 = c0.xover[(size_t) j];
                    auto& x1 = c1.xover[(size_t) j];
                    float* lo = bandBuf[(size_t) j].data();
                    if (x0.slope == 0)
                    {
                        stereo::run (x0.lp, x1.lp, nullptr, nullptr, rest, lo, nullptr, len);
                        for (int k = 0; k < len2; ++k) rest[k] = rest[k] - lo[k];   // first-order complementary
                    }
                    else
                    {
                        float* hi = tmpBuf.data();
                        stereo::run (x0.lp, x1.lp, &x0.hp, &x1.hp, rest, lo, hi, len);
                        const float sign = x0.slope == 1 ? -1.0f : 1.0f;   // LR2 needs an inverted high band
                        for (int k = 0; k < len2; ++k) rest[k] = sign * hi[k];
                    }
                }
                std::copy (rest, rest + len2, bandBuf[(size_t) nb - 1].data());

                // Phase-align lower bands with the higher crossovers
                for (int b = 0; b < nb - 1; ++b)
                    for (int j = b + 1; j < nb - 1; ++j)
                        runAllpass (c0.allpass[(size_t) b][(size_t) j], c1.allpass[(size_t) b][(size_t) j], bandBuf[(size_t) b].data(), len);

                // Bands (nonlinear part), summed in band order
                std::fill (sumBuf.begin(), sumBuf.begin() + len2, 0.0f);
                for (int b = 0; b < nb; ++b)
                {
                    const auto& bs = settings.bands[(size_t) b];
                    const bool audible = ! bs.mute && (! anySolo || bs.solo);
                    peaks[(size_t) b] = std::max (peaks[(size_t) b], processBand (b, len, envAttack, envRelease, audible));
                }

                // the bands' sum carries the crossovers' all-pass phase: give the dry signal the same phase
                // so the global Mix blends without notches around the crossover frequencies (always run: no state jumps when Mix moves)
                float* dryAligned = in;
                for (int j = 0; j < nb - 1; ++j) runAllpass (c0.dryPhase[(size_t) j], c1.dryPhase[(size_t) j], dryAligned, len);

                for (int i = 0; i < len; ++i)
                {
                    const float gm = gmBuf[(size_t) i], go = goBuf[(size_t) i];
                    std::array<float, 2> out;
                    for (int c = 0; c < 2; ++c)
                    {
                        const float d = dryAligned[2 * i + c];
                        out[(size_t) c] = d + gm * (sumBuf[(size_t) (2 * i + c)] - d);
                    }
                    if (ms) out = { out[0] + out[1], out[0] - out[1] };
                    l[i0 + i] = out[0] * go;
                    r[i0 + i] = out[1] * go;
                }
            }

            for (int b = 0; b < nb; ++b)
                if (peaks[(size_t) b] > bandPeaks[(size_t) b].load())
                    bandPeaks[(size_t) b].store (peaks[(size_t) b]);
        }

        /** In place, both channels: Allpass::process for every sample. */
        void runAllpass (Allpass& a0, Allpass& a1, float* x, int len) noexcept
        {
            if (a0.slope == 0) return;   // passes the input through
            float* ya = tmpBuf.data();
            float* yb = tmpBuf2.data();
            stereo::run (a0.lp, a1.lp, &a0.hp, &a1.hp, x, ya, yb, len);
            if (a0.slope == 1) for (int k = 0; k < 2 * len; ++k) x[k] = ya[k] + -yb[k];
            else               for (int k = 0; k < 2 * len; ++k) x[k] = ya[k] + yb[k];
        }

        /** One band, both channels, over the sub-block: adds the band's output to sumBuf if audible; returns its peak.
            The two channels' sample-by-sample recursions are independent and run interleaved in one loop. */
        float processBand (int b, int len, float envAttack, float envRelease, bool audible) noexcept
        {
            switch (settings.bands[(size_t) b].style)
            {
                case 0:  return processBandStyle<0>  (b, len, envAttack, envRelease, audible);
                case 1:  return processBandStyle<1>  (b, len, envAttack, envRelease, audible);
                case 2:  return processBandStyle<2>  (b, len, envAttack, envRelease, audible);
                case 3:  return processBandStyle<3>  (b, len, envAttack, envRelease, audible);
                case 4:  return processBandStyle<4>  (b, len, envAttack, envRelease, audible);
                case 5:  return processBandStyle<5>  (b, len, envAttack, envRelease, audible);
                case 6:  return processBandStyle<6>  (b, len, envAttack, envRelease, audible);
                case 7:  return processBandStyle<7>  (b, len, envAttack, envRelease, audible);
                case 8:  return processBandStyle<8>  (b, len, envAttack, envRelease, audible);
                case 9:  return processBandStyle<9>  (b, len, envAttack, envRelease, audible);
                case 10: return processBandStyle<10> (b, len, envAttack, envRelease, audible);
                case 11: return processBandStyle<11> (b, len, envAttack, envRelease, audible);
                case 12: return processBandStyle<12> (b, len, envAttack, envRelease, audible);
                case 13: return processBandStyle<13> (b, len, envAttack, envRelease, audible);
                case 14: return processBandStyle<14> (b, len, envAttack, envRelease, audible);
                case 15: return processBandStyle<15> (b, len, envAttack, envRelease, audible);
                case 16: return processBandStyle<16> (b, len, envAttack, envRelease, audible);
                case 17: return processBandStyle<17> (b, len, envAttack, envRelease, audible);
                case 18: return processBandStyle<18> (b, len, envAttack, envRelease, audible);
                case 19: return processBandStyle<19> (b, len, envAttack, envRelease, audible);
                default: return processBandStyle<-1> (b, len, envAttack, envRelease, audible);
            }
        }

        template <int Style>
        float processBandStyle (int b, int len, float envAttack, float envRelease, bool audible) noexcept
        {
            const auto& bs = settings.bands[(size_t) b];
            const float* sig = bandBuf[(size_t) b].data();
            const float* dr = drBuf[(size_t) b].data();
            const float* mx = mxBuf[(size_t) b].data();
            const float* lv = lvBuf[(size_t) b].data();
            const float* lp = lpBuf[(size_t) b].data();
            const bool tone0 = std::abs (bs.bassDb) > 0.01f, tone1 = std::abs (bs.midDb) > 0.01f;
            const bool tone2 = std::abs (bs.trebleDb) > 0.01f, tone3 = std::abs (bs.presenceDb) > 0.01f;
            const bool bypass = bs.bypass;
            float peak = 0.0f;

            // The drive pan gain is a plain setting (constant over the call); while the band's drive is not smoothing,
            // the level compensation 1 / sqrt (drive) is constant too. Same expressions as per sample, same values.
            const float panDb = 6.0f * bs.drivePan;
            const float panGain[2] = { juce::Decibels::decibelsToGain (-panDb), juce::Decibels::decibelsToGain (panDb) };
            const bool driveConst = driveConstant[(size_t) b];
            float invDrive[2] = { 1.0f, 1.0f };
            if (driveConst)
                for (int c = 0; c < 2; ++c)
                {
                    const float chDrive = drive[(size_t) b].getTargetValue() * panGain[c];
                    invDrive[c] = 1.0f / std::sqrt (chDrive);
                }

            auto one = [&] (int i, int c, BandState& st) noexcept
            {
                const float dry = sig[2 * i + c];
                float y = dry;

                if (! bypass)
                {
                    const float a = std::abs (dry);
                    st.env += (a > st.env ? envAttack : envRelease) * (a - st.env);

                    // Dynamics (Saturn behaviour): right = compression (loud parts down, pumping),
                    // left = gating / expansion (quiet parts down).
                    if (st.dynCounter-- <= 0)
                    {
                        st.dynCounter = 7;
                        const float dynTarget = std::abs (bs.dynamics) < 0.001f ? 1.0f
                                   : juce::jlimit (0.03f, 4.0f, st.powMemo (juce::jmax (1.0e-4f, st.env / 0.25f), -0.85f * bs.dynamics));
                        st.dynGain += dynSlew * (dynTarget - st.dynGain);   // slewed: a Dynamics change never steps
                    }

                    // Drive with pan: shifts the drive between the channels (L/R, or M/S in mid/side mode)
                    const float chDrive = dr[i] * panGain[c];

                    // Feedback is reduced automatically when the band goes quiet, so it cannot ring forever
                    const float fbAmount = bs.feedback * 0.95f * juce::jmin (1.0f, st.env * 50.0f);
                    const float fb = st.feedbackFilter.process (st.lastOut) * fbAmount;
                    float w = processStyle (Style, dry * chDrive + fb, st.style, constants);
                    st.lastOut = w;
                    w *= driveConst ? invDrive[c] : 1.0f / std::sqrt (chDrive);   // automatic level compensation for drive
                    w *= st.dynGain;
                    if (tone0) w = st.tone[0].process (w);
                    if (tone1) w = st.tone[1].process (w);
                    if (tone2) w = st.tone[2].process (w);
                    if (tone3) w = st.tone[3].process (w);

                    const float pan = lp[i];
                    const float lvPan = c == 0 ? (pan > 0.0f ? 1.0f - pan : 1.0f) : (pan < 0.0f ? 1.0f + pan : 1.0f);
                    y = (dry + mx[i] * (w - dry)) * lv[i] * lvPan;
                }

                if (audible)
                {
                    sumBuf[(size_t) (2 * i + c)] += y;
                    peak = std::max (peak, std::abs (y));
                }
            };

            auto& st0 = channels[0].bands[(size_t) b];
            auto& st1 = channels[1].bands[(size_t) b];
            for (int i = 0; i < len; ++i)
            {
                one (i, 0, st0);
                one (i, 1, st1);
            }
            return peak;
        }

        static constexpr int subBlock = 32;
        using SubBuffer = std::array<float, subBlock>;
        using StereoBuffer = std::array<float, 2 * subBlock>;   // interleaved: [2 * i + channel]
        std::array<SubBuffer, maxBands> drBuf {}, mxBuf {}, lvBuf {}, lpBuf {};
        SubBuffer gmBuf {}, goBuf {}, giBuf {};
        std::array<bool, maxBands> driveConstant {};
        alignas (16) std::array<StereoBuffer, maxBands> bandBuf {};
        alignas (16) StereoBuffer inBuf {}, restBuf {}, tmpBuf {}, tmpBuf2 {}, sumBuf {};

        double baseRate = 48000.0, rate = 48000.0;
        std::array<std::unique_ptr<StereoOversampling>, 3> oversamplers;
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
