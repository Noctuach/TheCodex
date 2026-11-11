#pragma once
#include "MultibandSaturator.h"   // Linkwitz-Riley crossover helpers (sat::Crossover / sat::Allpass)
#include <atomic>

/**
    Multiband compressor (1-4 bands) with downward, upward and two-way ("both") compression.

    Control set modelled on FabFilter Pro-C 3 (Threshold, Ratio, Knee, Range, Attack, Release,
    Hold, Lookahead, Auto Gain, dry/wet, side-chain high-pass, compression styles) and on the
    Teletronix / UAD LA-3A (T4 opto cell: ~10 ms attack, two-stage program-dependent release
    with memory, Compress / Limit switch).

    Gain computer (dB domain, x = detector level, T = threshold, R = ratio, W = knee):
      Downward: reduces the level above T by (x - T)(1 - 1/R), soft knee over W.
      Upward:   raises the level below T by (T - x)(1 - 1/R), soft knee over W.
      Both:     both at once: T becomes a target level (Pro-C 3's TTM idea).
    Range limits the maximum gain change in either direction. Upward gain fades out on silence so
    noise floors are not pulled up.
*/
namespace comp
{
    constexpr int maxBands = 4;

    enum Style { Clean, Punch, Smooth, Bus, Mastering, Classic, VariMu, Opto, T4Opto, Pumping, TTM, numStyles };
    inline const juce::StringArray& styleNames()
    {
        static const juce::StringArray n { "Clean", "Punch", "Smooth", "Bus", "Mastering", "Classic", "Vari-Mu",
                                           "Opto", "T4 Opto (LA-3A)", "Pumping", "TTM (Up + Down)" };
        return n;
    }
    inline const juce::StringArray& directionNames() { static const juce::StringArray n { "Downward", "Upward", "Both" }; return n; }
    inline const juce::StringArray& lookaheadNames() { static const juce::StringArray n { "Off", "1 ms", "2 ms", "5 ms", "10 ms" }; return n; }
    inline float lookaheadMs (int i) { const float v[] = { 0.0f, 1.0f, 2.0f, 5.0f, 10.0f }; return v[juce::jlimit (0, 4, i)]; }

    /** True when the style sets its own timing (the Attack / Release knobs are ignored or scaled). */
    inline bool styleHasFixedTiming (int s) { return s == T4Opto; }

    struct BandSettings
    {
        int   direction = 0;
        float thresholdDb = -18.0f, ratio = 4.0f, kneeDb = 6.0f, rangeDb = 30.0f;
        float attackMs = 10.0f, releaseMs = 120.0f, gainDb = 0.0f;
        bool  mute = false, solo = false, bypass = false;
    };

    struct Settings
    {
        bool  enabled = false;
        int   numBands = 1, style = Clean, lookahead = 0;
        std::array<float, maxBands - 1> crossovers { 120.0f, 1000.0f, 6000.0f };
        float holdMs = 0.0f, scHighPass = 20.0f, mix = 1.0f, outDb = 0.0f;
        bool  autoGain = true, limit = false;
        std::array<BandSettings, maxBands> bands {};
    };

    /** Static gain change (dB) for a detector level, including soft knee, range and direction. */
    inline float gainComputer (float x, const BandSettings& b, int style, bool limit)
    {
        float R = juce::jmax (1.0f, b.ratio), W = juce::jmax (0.0f, b.kneeDb);
        if (style == Smooth || style == Opto) W += 6.0f;
        if (style == T4Opto) { R = limit ? 12.0f : 3.0f; W = limit ? 4.0f : 14.0f; }
        if (style == VariMu) R = 1.0f + (R - 1.0f) * juce::jlimit (0.2f, 2.5f, 1.0f + (x - b.thresholdDb) / 18.0f);   // ratio grows with level
        const float T = b.thresholdDb, slope = 1.0f - 1.0f / R;
        const int dir = style == TTM ? 2 : b.direction;

        float down = 0.0f, up = 0.0f;
        if (dir != 1)
        {
            const float d = x - T;
            if (W > 0.0f && std::abs (d) <= W * 0.5f) down = -slope * (d + W * 0.5f) * (d + W * 0.5f) / (2.0f * W);
            else if (d > 0.0f)                        down = -slope * d;
        }
        if (dir != 0)
        {
            const float d = T - x;
            if (W > 0.0f && std::abs (d) <= W * 0.5f) up = slope * (d + W * 0.5f) * (d + W * 0.5f) / (2.0f * W);
            else if (d > 0.0f)                        up = slope * d;
            // do not pull up silence: fade upward gain out between -60 and -80 dB
            up *= juce::jlimit (0.0f, 1.0f, (x + 80.0f) / 20.0f);
        }
        const float range = juce::jmax (0.0f, b.rangeDb);
        return juce::jlimit (-range, range, down + up);
    }

    /** Auto make-up: an educated guess from the static curve, like Pro-C's Auto Gain. */
    inline float autoMakeup (const BandSettings& b, int style, bool limit)
    {
        const float g = gainComputer (-6.0f, b, style, limit);
        return juce::jlimit (-24.0f, 24.0f, g < 0.0f ? -0.5f * g : -0.5f * gainComputer (-36.0f, b, style, limit) * 0.5f);
    }

    /** Lock-free ring of meter frames for the GUI (single producer, single consumer). */
    struct MeterFrame { float inDb, outDb; std::array<float, maxBands> grDb; };
    class MeterFifo
    {
    public:
        void push (const MeterFrame& f) noexcept
        {
            const int w = write.load (std::memory_order_relaxed);
            frames[(size_t) (w & (size - 1))] = f;
            write.store (w + 1, std::memory_order_release);
        }
        /** Copies frames written since the last call (at most `max`); returns how many. */
        int pull (MeterFrame* dest, int max) noexcept
        {
            const int w = write.load (std::memory_order_acquire);
            int n = juce::jmin (w - read, max, size);
            const int start = w - n;
            for (int i = 0; i < n; ++i) dest[i] = frames[(size_t) ((start + i) & (size - 1))];
            read = w;
            return n;
        }
    private:
        static constexpr int size = 1024;
        std::array<MeterFrame, size> frames {};
        std::atomic<int> write { 0 };
        int read = 0;
    };

    class MultibandCompressor
    {
    public:
        MeterFifo meters;
        std::array<std::atomic<float>, maxBands> currentGr {};

        void prepare (double sampleRate)
        {
            rate = sampleRate;
            const int maxLook = (int) std::ceil (0.010 * rate) + 2;
            for (auto& ch : channels)
            {
                for (auto& d : ch.delay) d.assign ((size_t) maxLook + 1, 0.0f);
                ch.dryDelay.assign ((size_t) maxLook + 1, 0.0f);
            }
            frameLen = juce::jmax (32, (int) (rate / 400.0));   // ~400 frames per second for the display
            lastXf.fill (-1.0f);
            paramGlide = (float) (1.0 - std::exp (-1.0 / (0.01 * rate)));
            for (auto& m : makeupSmooth) m.reset (rate, 0.02);
            outSmooth.reset (rate, 0.02);
            mixSmooth.reset (rate, 0.02);
            reset();
            setSettings (settings);
            smoothBands = settings.bands;   // start at the settings, glide from there
            for (int b = 0; b < maxBands; ++b) makeupSmooth[(size_t) b].setCurrentAndTargetValue (makeup[(size_t) b]);
            outSmooth.setCurrentAndTargetValue (outGain);
            mixSmooth.setCurrentAndTargetValue (settings.mix);
        }

        void reset()
        {
            for (auto& ch : channels)
            {
                for (auto& x : ch.xover) x.reset();
                for (auto& row : ch.allpass) for (auto& a : row) a.reset();
                for (auto& a : ch.dryPhase) a.reset();
                for (auto& d : ch.delay) std::fill (d.begin(), d.end(), 0.0f);
                std::fill (ch.dryDelay.begin(), ch.dryDelay.end(), 0.0f);
                ch.pos = 0;
            }
            for (auto& b : bands) b = BandState {};
            for (auto& g : currentGr) g.store (0.0f);
        }

        int getLatencyInSamples() const { return settings.enabled ? lookSamples : 0; }

        void setSettings (const Settings& s)
        {
            if (s.enabled && ! settings.enabled) reset();
            settings = s;
            lookSamples = (int) std::round (lookaheadMs (s.lookahead) * 0.001 * rate);

            std::array<float, maxBands - 1> xf {};
            float prev = 20.0f;
            for (int j = 0; j < maxBands - 1; ++j)
            {
                xf[(size_t) j] = juce::jlimit (prev * 1.05f, (float) (rate * 0.45), s.crossovers[(size_t) j]);
                prev = xf[(size_t) j];
            }
            if (xf != lastXf || s.numBands != lastBands)
            {
                for (auto& ch : channels)
                    for (int j = 0; j < maxBands - 1; ++j)
                    {
                        auto& x = ch.xover[(size_t) j];
                        x.slope = 2;   // 24 dB/oct Linkwitz-Riley
                        sat::designCrossover (2, xf[(size_t) j], rate, x.lp, x.hp);
                        for (int i = 0; i < maxBands - 1; ++i)
                        {
                            auto& a = ch.allpass[(size_t) i][(size_t) j];
                            a.slope = 2;
                            sat::designCrossover (2, xf[(size_t) j], rate, a.lp, a.hp);
                        }
                        auto& dp = ch.dryPhase[(size_t) j];
                        dp.slope = 2;
                        sat::designCrossover (2, xf[(size_t) j], rate, dp.lp, dp.hp);
                    }
                if (s.numBands != lastBands)
                    for (auto& ch : channels) { for (auto& x : ch.xover) x.reset(); for (auto& row : ch.allpass) for (auto& a : row) a.reset(); for (auto& a : ch.dryPhase) a.reset(); }
                lastXf = xf;
                lastBands = s.numBands;
            }

            // side-chain high-pass (detector only)
            const double g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (10.0, rate * 0.45, (double) s.scHighPass) / rate);
            scG = (float) (g / (1.0 + g));

            anySolo = false;
            for (int b = 0; b < s.numBands; ++b) anySolo = anySolo || s.bands[(size_t) b].solo;
            for (int b = 0; b < maxBands; ++b)
            {
                const auto& bs = s.bands[(size_t) b];
                makeup[(size_t) b] = juce::Decibels::decibelsToGain (bs.gainDb + (s.autoGain ? autoMakeup (bs, s.style, s.limit) : 0.0f));
            }
            outGain = juce::Decibels::decibelsToGain (s.outDb);
            for (int b = 0; b < maxBands; ++b) makeupSmooth[(size_t) b].setTargetValue (makeup[(size_t) b]);
            outSmooth.setTargetValue (outGain);
        }

        void process (float* l, float* r, int n) noexcept
        {
            if (! settings.enabled) return;

            const int nb = juce::jlimit (1, maxBands, settings.numBands);
            const int style = settings.style;
            const float holdSamples = settings.holdMs * 0.001f * (float) rate + (style == Pumping ? 0.03f * (float) rate : 0.0f);
            float* chans[] = { l, r };

            // per band timing coefficients
            std::array<float, maxBands> attC {}, relFast {}, relSlow {};
            for (int b = 0; b < nb; ++b)
            {
                const auto& bs = settings.bands[(size_t) b];
                float att = bs.attackMs, rel = bs.releaseMs;
                switch (style)
                {
                    case Punch:     att *= 1.6f; break;                     // lets more transient through
                    case Smooth:    att *= 2.0f; rel *= 1.5f; break;
                    case Opto:      att = juce::jmax (att, 5.0f); rel *= 1.8f; break;
                    case T4Opto:    att = 10.0f; rel = 60.0f; break;        // fast stage of the T4 cell
                    case Mastering: att = juce::jmax (att, 0.5f); break;
                    case Pumping:   rel *= 1.3f; break;
                    default: break;
                }
                auto coef = [this] (float ms) { return (float) std::exp (-1.0 / (juce::jmax (0.005, (double) ms) * 0.001 * rate)); };
                attC[(size_t) b] = coef (att);
                // program-dependent styles recover fast after short peaks and slower after long compression
                const bool program = style == Clean || style == Smooth || style == Bus || style == Opto;
                relFast[(size_t) b] = coef (rel * (style == T4Opto ? 1.0f : program ? 0.5f : 1.0f));
                relSlow[(size_t) b] = coef (rel * (style == T4Opto ? 25.0f : program ? 2.0f : 1.0f));
            }
            const float rmsC = (float) std::exp (-1.0 / ((style == Smooth || style == Bus ? 0.025 : style == Punch ? 0.008 : 0.012) * rate));
            const float peakC = (float) std::exp (-1.0 / (0.02 * rate));
            const float memUp = (float) (1.0 / (2.0 * rate)), memDown = (float) (1.0 / (6.0 * rate));
            mixSmooth.setTargetValue (settings.mix);
            const int len = (int) channels[0].dryDelay.size();

            for (int i = 0; i < n; ++i)
            {
                const float outNow = outSmooth.getNextValue();
                const float wetMix = mixSmooth.getNextValue(), dryMix = 1.0f - wetMix;
                std::array<std::array<float, maxBands>, 2> sig {};
                float inPeak = 0.0f;
                for (int c = 0; c < 2; ++c)
                {
                    auto& ch = channels[(size_t) c];
                    const float x = chans[c][i];
                    inPeak = juce::jmax (inPeak, std::abs (x));
                    float rest = x;
                    for (int j = 0; j < nb - 1; ++j) ch.xover[(size_t) j].split (rest, sig[(size_t) c][(size_t) j], rest);
                    sig[(size_t) c][(size_t) nb - 1] = rest;
                    for (int b = 0; b < nb - 1; ++b)
                        for (int j = b + 1; j < nb - 1; ++j)
                            sig[(size_t) c][(size_t) b] = ch.allpass[(size_t) b][(size_t) j].process (sig[(size_t) c][(size_t) b]);
                }

                std::array<float, 2> out {};
                for (int b = 0; b < nb; ++b)
                {
                    // static curve parameters glide (~10 ms) so threshold / ratio / knee / range moves never step
                    auto& sm = smoothBands[(size_t) b];
                    const auto& tgt = settings.bands[(size_t) b];
                    sm.thresholdDb += paramGlide * (tgt.thresholdDb - sm.thresholdDb);
                    sm.ratio       += paramGlide * (tgt.ratio - sm.ratio);
                    sm.kneeDb      += paramGlide * (tgt.kneeDb - sm.kneeDb);
                    sm.rangeDb     += paramGlide * (tgt.rangeDb - sm.rangeDb);
                    sm.direction = tgt.direction; sm.mute = tgt.mute; sm.solo = tgt.solo; sm.bypass = tgt.bypass;
                    const auto& bs = sm;
                    auto& st = bands[(size_t) b];

                    // ---- detector: stereo-linked, side-chain high-passed, peak or RMS; feedback styles use the output ----
                    float det = 0.0f;
                    const bool feedback = style == Classic || style == VariMu;
                    for (int c = 0; c < 2; ++c)
                    {
                        float v = feedback ? st.lastOut[(size_t) c] : sig[(size_t) c][(size_t) b];
                        if (b == 0)
                        {
                            const float t = (v - st.hp[(size_t) c]) * scG;
                            const float lp = t + st.hp[(size_t) c];
                            st.hp[(size_t) c] = lp + t;
                            v -= lp;
                        }
                        det = juce::jmax (det, std::abs (v));
                    }
                    // level detector: peak envelope (instant rise, ~20 ms fall) or RMS (window per style)
                    st.peak = det > st.peak ? det : det + peakC * (st.peak - det);
                    st.rms = det * det + rmsC * (st.rms - det * det);
                    const bool peakStyle = style == Clean || style == Mastering || style == TTM || style == Pumping || style == Classic;
                    const float lvl = peakStyle ? st.peak : std::sqrt (juce::jmax (0.0f, st.rms)) * 1.41421356f;
                    const float xDb = juce::Decibels::gainToDecibels (juce::jmax (lvl, 1.0e-6f), -120.0f);
                    const float target = gainComputer (xDb, bs, style, settings.limit);

                    // ---- smoothing (dB): attack towards more change, program-dependent release ----
                    const float cur = st.gr;
                    const bool attacking = std::abs (target) > std::abs (cur) && (target * cur >= 0.0f);
                    if (attacking)
                    {
                        st.gr = target + attC[(size_t) b] * (cur - target);
                        st.slow = target + attC[(size_t) b] * (st.slow - target);
                        st.hold = holdSamples;
                    }
                    else if (st.hold > 0.0f)
                        st.hold -= 1.0f;
                    else
                    {
                        // release: fast for short peaks, slower after long periods of gain change (memory)
                        st.slow = target + relSlow[(size_t) b] * (st.slow - target);
                        const float fast = target + relFast[(size_t) b] * (cur - target);
                        if (style == T4Opto)
                        {
                            // two-stage T4 release: ~60 ms for the first half, then 0.5-5 s depending on history
                            const float slowC = (float) std::exp (-1.0 / ((0.5 + 4.5 * st.memory) * rate));
                            st.slow = target + slowC * (st.slow - target);
                            st.gr = 0.5f * fast + 0.5f * st.slow;
                        }
                        else if (style == Pumping)
                        {
                            // release in linear gain: a deep, audible swell back
                            const float g = juce::Decibels::decibelsToGain (cur), gt = juce::Decibels::decibelsToGain (target);
                            st.gr = juce::Decibels::gainToDecibels (gt + relSlow[(size_t) b] * (g - gt), -80.0f);
                        }
                        else
                            st.gr = juce::jmap (st.memory, fast, st.slow);
                    }
                    st.memory = std::abs (st.gr) > 2.0f ? st.memory + memUp * (1.0f - st.memory) : st.memory * (1.0f - memDown);

                    const float g = juce::Decibels::decibelsToGain (st.gr) * makeupSmooth[(size_t) b].getNextValue();
                    for (int c = 0; c < 2; ++c)
                    {
                        auto& ch = channels[(size_t) c];
                        auto& d = ch.delay[(size_t) b];
                        // lookahead: the gain is applied to the delayed band signal
                        d[(size_t) ch.pos] = sig[(size_t) c][(size_t) b];
                        const float delayed = d[(size_t) ((ch.pos - lookSamples + len) % len)];
                        float y = bs.bypass ? delayed : delayed * g;
                        st.lastOut[(size_t) c] = y;
                        if (bs.mute || (anySolo && ! bs.solo)) y = 0.0f;
                        out[(size_t) c] += y;
                    }
                    st.peakGr = std::abs (st.gr) > std::abs (st.peakGr) ? st.gr : st.peakGr;
                }

                float outPeak = 0.0f;
                for (int c = 0; c < 2; ++c)
                {
                    auto& ch = channels[(size_t) c];
                    // the bands' sum carries the crossovers' all-pass phase: the dry signal gets the same phase
                    float dryAligned = chans[c][i];
                    for (int j = 0; j < nb - 1; ++j) dryAligned = ch.dryPhase[(size_t) j].process (dryAligned);
                    ch.dryDelay[(size_t) ch.pos] = dryAligned;
                    const float dry = ch.dryDelay[(size_t) ((ch.pos - lookSamples + len) % len)];
                    const float y = (dry * dryMix + out[(size_t) c] * wetMix) * outNow;
                    chans[c][i] = y;
                    outPeak = juce::jmax (outPeak, std::abs (y));
                    ch.pos = (ch.pos + 1) % len;
                }

                // meter frames for the display
                frameIn = juce::jmax (frameIn, inPeak);
                frameOut = juce::jmax (frameOut, outPeak);
                if (++frameCount >= frameLen)
                {
                    MeterFrame f;
                    f.inDb = juce::Decibels::gainToDecibels (frameIn, -100.0f);
                    f.outDb = juce::Decibels::gainToDecibels (frameOut, -100.0f);
                    for (int b = 0; b < maxBands; ++b)
                    {
                        f.grDb[(size_t) b] = b < nb ? bands[(size_t) b].peakGr : 0.0f;
                        currentGr[(size_t) b].store (f.grDb[(size_t) b], std::memory_order_relaxed);
                        bands[(size_t) b].peakGr = 0.0f;
                    }
                    meters.push (f);
                    frameIn = frameOut = 0.0f;
                    frameCount = 0;
                }
            }
        }

        const Settings& getSettings() const { return settings; }

    private:
        struct BandState
        {
            float gr = 0.0f, slow = 0.0f, rms = 0.0f, peak = 0.0f, hold = 0.0f, memory = 0.0f, peakGr = 0.0f;
            std::array<float, 2> hp {}, lastOut {};
        };
        struct Channel
        {
            std::array<sat::Crossover, maxBands - 1> xover {};
            std::array<std::array<sat::Allpass, maxBands - 1>, maxBands - 1> allpass {};
            std::array<sat::Allpass, maxBands - 1> dryPhase {};   // crossover phase for the dry signal (notch-free Mix)
            std::array<std::vector<float>, maxBands> delay;
            std::vector<float> dryDelay;
            int pos = 0;
        };

        double rate = 48000.0;
        Settings settings;
        std::array<Channel, 2> channels;
        std::array<BandState, maxBands> bands {};
        std::array<float, maxBands> makeup {};
        std::array<float, maxBands - 1> lastXf {};
        int lastBands = -1, lookSamples = 0, frameLen = 120, frameCount = 0;
        float scG = 0.0f, outGain = 1.0f, frameIn = 0.0f, frameOut = 0.0f, paramGlide = 0.001f;
        std::array<BandSettings, maxBands> smoothBands {};
        std::array<juce::SmoothedValue<float>, maxBands> makeupSmooth;
        juce::SmoothedValue<float> outSmooth, mixSmooth;
        bool anySolo = false;
    };
}
