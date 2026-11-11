#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

/**
    "Space": a stereo room reverb for adding width and ambience to the kick.
    The control set follows ValhallaRoom's layout (Mix, Predelay, Decay, High Cut, Depth,
    Early / Late sections, Decay EQ with bass and high multipliers), plus kick-specific
    extras: a Low Cut that keeps the sub out of the reverb, Width, and Ducking.

    Structure:
      input -> low cut -> predelay -+-> early reflections (multi-tap + modulated diffusion) ---+
                                    |                                                         |  Depth (equal-power)
                                    +-> (+ Early Send) -> input diffusion -> 8-line FDN ------+-> width -> high cut -> duck -> mix
    The late field is a feedback delay network (Hadamard matrix, modulated delay lines) with a
    3-band decay filter in every line so bass and highs can decay faster or slower than the mids.
*/
namespace space
{
    inline const juce::StringArray& modeNames()
    {
        static const juce::StringArray n { "Room", "Large Room", "Chamber", "Hall", "Plate", "Warehouse" };
        return n;
    }

    struct Settings
    {
        bool  enabled = false;
        int   mode = 0;
        float mix = 0.25f, predelayMs = 10.0f, decay = 1.2f, highCut = 8000.0f, lowCut = 200.0f;
        float depth = 0.6f, width = 1.0f, duck = 0.3f;
        // Early
        float eSize = 0.5f, eCross = 0.3f, eSend = 0.5f, eDiffusion = 0.6f, eModRate = 0.8f, eModDepth = 0.3f;
        // Late
        float lSize = 0.5f, lCross = 0.4f, lDiffusion = 0.7f, lModRate = 0.6f, lModDepth = 0.3f;
        // Decay EQ
        float bassMult = 0.8f, bassXover = 250.0f, highMult = 0.6f, highXover = 4000.0f;
    };

    /** Character of each mode: size scaling, diffusion, early level, modulation and brightness. */
    struct ModeShape { float lateScale, earlyScale, diffusion, earlyLevel, modScale, damping; };
    inline ModeShape modeShape (int m)
    {
        static const ModeShape s[] = {
            { 0.70f, 0.60f, 0.85f, 1.00f, 1.0f, 1.00f },   // Room
            { 1.00f, 1.00f, 0.90f, 0.90f, 1.0f, 1.00f },   // Large Room
            { 0.90f, 0.80f, 1.00f, 0.75f, 0.8f, 0.85f },   // Chamber
            { 1.50f, 1.40f, 0.95f, 0.60f, 1.2f, 0.95f },   // Hall
            { 0.60f, 0.35f, 1.10f, 0.25f, 0.6f, 1.15f },   // Plate (dense, bright, few reflections)
            { 1.70f, 1.80f, 0.75f, 0.85f, 1.6f, 0.75f },   // Warehouse (big, wobbly, darker)
        };
        return s[juce::jlimit (0, 5, m)];
    }

    /** Topology-preserving one-pole filter. */
    struct OnePole
    {
        float G = 0.5f, s = 0.0f;
        void set (double sr, float hz)
        {
            const double g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (5.0, sr * 0.49, (double) hz) / sr);
            G = (float) (g / (1.0 + g));
        }
        float lp (float x) noexcept { const float v = (x - s) * G; const float y = v + s; s = y + v; return y; }
        float hp (float x) noexcept { return x - lp (x); }
        void reset() { s = 0.0f; }
    };

    /** Power-of-two circular delay with fractional (linear) reads. */
    struct Delay
    {
        std::vector<float> buf;
        int mask = 0, w = 0;
        void prepare (int maxSamples)
        {
            int n = 1;
            while (n < maxSamples + 4) n <<= 1;
            buf.assign ((size_t) n, 0.0f);
            mask = n - 1;
            w = 0;
        }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
        void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }
        /** Sample written `d` samples ago (d >= 1). */
        float read (float d) const noexcept
        {
            const int i = (int) d;
            const float f = d - (float) i;
            const float a = buf[(size_t) ((w - i) & mask)], b = buf[(size_t) ((w - i - 1) & mask)];
            return a + f * (b - a);
        }
        float readInt (int d) const noexcept { return buf[(size_t) ((w - d) & mask)]; }
    };

    /** Schroeder all-pass with a (modulatable) fractional delay. */
    struct AllPass
    {
        Delay d;
        float process (float x, float delaySamples, float g) noexcept
        {
            const float z = d.read (delaySamples);
            const float v = x + g * z;
            d.push (v);
            return z - g * v;
        }
    };

    class SpaceReverb
    {
    public:
        static constexpr int numLines = 8;

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            const auto ms = [this] (double t) { return (int) std::ceil (t * 0.001 * sr); };
            for (auto& p : predelay) p.prepare (ms (510.0));
            for (auto& e : earlyLine) e.prepare (ms (260.0));
            for (auto& ch : earlyAp) for (auto& a : ch) a.d.prepare (ms (40.0));
            for (auto& ch : inAp) for (auto& a : ch) a.d.prepare (ms (45.0));
            for (auto& l : lines) l.prepare (ms (420.0));
            reset();
            lastHash = -1.0f;
            setSettings (settings);
        }

        void reset()
        {
            for (auto& p : predelay) p.reset();
            for (auto& e : earlyLine) e.reset();
            for (auto& ch : earlyAp) for (auto& a : ch) a.d.reset();
            for (auto& ch : inAp) for (auto& a : ch) a.d.reset();
            for (auto& l : lines) l.reset();
            for (auto& f : lowCutF) f.reset();
            for (auto& f : highCutF) f.reset();
            for (auto& f : bassF) f.reset();
            for (auto& f : trebleF) f.reset();
            env = 0.0f;
            for (int i = 0; i < numLines; ++i) curLen[(size_t) i] = targetLen[(size_t) i];
        }

        void setSettings (const Settings& s)
        {
            if (s.enabled && ! settings.enabled)
                reset();   // start clean when switched on
            settings = s;
            const auto shape = modeShape (s.mode);
            const float fs = (float) sr;

            // Late delay lengths (mutually prime-ish), scaled by Late Size and the mode
            static const float baseMs[numLines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f };
            const float lateScale = shape.lateScale * (0.35f + 1.65f * s.lSize);
            for (int i = 0; i < numLines; ++i)
                targetLen[(size_t) i] = baseMs[i] * lateScale * 0.001f * fs;

            // Decay: per-band feedback gains from T60 (Decay x multipliers)
            const float t60 = juce::jlimit (0.1f, 20.0f, s.decay);
            for (int i = 0; i < numLines; ++i)
            {
                const float lenS = targetLen[(size_t) i] / fs;
                auto g = [lenS] (float t) { return juce::jmin (0.99995f, std::pow (10.0f, -3.0f * lenS / juce::jmax (0.05f, t))); };
                gMid[(size_t) i]  = g (t60);
                gBass[(size_t) i] = g (t60 * s.bassMult);
                gHigh[(size_t) i] = g (t60 * s.highMult * shape.damping);
                bassF[(size_t) i].set (sr, s.bassXover);
                trebleF[(size_t) i].set (sr, s.highXover);
            }

            // Early reflections: tap times scale with Early Size
            earlySpanSamples = (10.0f + 140.0f * s.eSize) * shape.earlyScale * 0.001f * fs;
            earlySpanSamples = juce::jlimit (8.0f, 250.0f * 0.001f * fs, earlySpanSamples);

            for (auto& f : lowCutF) f.set (sr, s.lowCut);
            for (auto& f : highCutF) f.set (sr, s.highCut);

            predelaySamples = juce::jlimit (1.0f, 500.0f * 0.001f * fs, s.predelayMs * 0.001f * fs);
            earlyDiffG = 0.72f * juce::jlimit (0.0f, 1.0f, s.eDiffusion * shape.diffusion);
            lateDiffG  = 0.70f * juce::jlimit (0.0f, 1.0f, s.lDiffusion * shape.diffusion);
            earlyLevel = shape.earlyLevel;
            modScale   = shape.modScale;

            const float d = juce::jlimit (0.0f, 1.0f, s.depth) * juce::MathConstants<float>::halfPi;
            earlyGainT = std::cos (d);
            lateGainT  = std::sin (d);
            const float m = juce::jlimit (0.0f, 1.0f, s.mix) * juce::MathConstants<float>::halfPi;
            // The kick must not fade when the room is added: the dry stays at unity up to 50 % and only then falls
            // (to silence at 100 %); the wet follows the equal-power curve.
            dryGainT = std::min (1.0f, std::cos (m) * juce::MathConstants<float>::sqrt2);
            wetGainT = std::sin (m);
            // a 100 % dry/wet-free reverb at 0 % mix: keep the dry exactly unity
            if (s.mix <= 0.0f) { dryGainT = 1.0f; wetGainT = 0.0f; }
            releaseCoef = std::exp (-1.0f / (0.12f * fs));
            attackCoef  = std::exp (-1.0f / (0.001f * fs));
        }

        const Settings& getSettings() const { return settings; }
        double tailSeconds() const { return settings.enabled ? settings.predelayMs * 0.001 + settings.decay * 1.5 : 0.0; }

        void process (float* left, float* right, int numSamples) noexcept
        {
            if (! settings.enabled)
                return;

            const float fs = (float) sr;
            const float eRateInc = juce::MathConstants<float>::twoPi * (0.05f + 4.95f * settings.eModRate * settings.eModRate) / fs;
            const float lRateInc = juce::MathConstants<float>::twoPi * (0.05f + 4.95f * settings.lModRate * settings.lModRate) / fs * modScale;
            const float eDepth = settings.eModDepth * 0.0012f * fs * modScale;   // up to ~1.2 ms of sweep
            const float lDepth = settings.lModDepth * 0.0006f * fs * modScale;   // up to ~0.6 ms
            const float eCross = settings.eCross, lCross = settings.lCross, eSend = settings.eSend;
            const float width = settings.width, duck = settings.duck;
            const float smooth = 0.002f;

            static const float tapPos[8]  = { 0.043f, 0.117f, 0.191f, 0.302f, 0.419f, 0.553f, 0.716f, 1.0f };
            static const float tapGain[8] = { 0.84f, -0.71f, 0.62f, -0.55f, 0.47f, -0.38f, 0.30f, -0.22f };
            static const float apMs[2][2] = { { 5.3f, 8.9f }, { 6.1f, 9.7f } };
            static const float inApMs[2][4] = { { 4.7f, 3.6f, 12.7f, 9.3f }, { 5.1f, 3.3f, 13.9f, 8.7f } };

            for (int n = 0; n < numSamples; ++n)
            {
                const float dryL = left[n], dryR = right[n];

                // smoothed gains
                earlyGain += (earlyGainT - earlyGain) * smooth;
                lateGain  += (lateGainT - lateGain) * smooth;
                dryGain   += (dryGainT - dryGain) * smooth;
                wetGain   += (wetGainT - wetGain) * smooth;
                curPredelay += (predelaySamples - curPredelay) * smooth;
                curSpan     += (earlySpanSamples - curSpan) * smooth;

                // ducking follower on the dry signal
                const float lvl = 0.5f * (std::abs (dryL) + std::abs (dryR));
                env = lvl > env ? lvl + (env - lvl) * attackCoef : lvl + (env - lvl) * releaseCoef;

                // low cut (12 dB/oct) before the reverb: keeps the sub out of the tail
                float inL = lowCutF[1].hp (lowCutF[0].hp (dryL));
                float inR = lowCutF[3].hp (lowCutF[2].hp (dryR));

                predelay[0].push (inL);
                predelay[1].push (inR);
                const float pL = predelay[0].read (curPredelay), pR = predelay[1].read (curPredelay);

                // ---- Early reflections ----
                earlyLine[0].push (pL + eCross * pR);
                earlyLine[1].push (pR + eCross * pL);
                float eL = 0.0f, eR = 0.0f;
                for (int t = 0; t < 8; ++t)
                {
                    // slightly different tap times per side for width
                    eL += tapGain[t] * earlyLine[0].read (1.0f + curSpan * tapPos[t]);
                    eR += tapGain[t] * earlyLine[1].read (1.0f + curSpan * tapPos[(t + 3) & 7] * 0.93f + 0.07f * curSpan * tapPos[t]);
                }
                ePhase += eRateInc;
                if (ePhase > juce::MathConstants<float>::twoPi) ePhase -= juce::MathConstants<float>::twoPi;
                const float em0 = std::sin (ePhase), em1 = std::cos (ePhase);
                for (int a = 0; a < 2; ++a)
                {
                    eL = earlyAp[0][(size_t) a].process (eL, apMs[0][a] * 0.001f * fs + eDepth * (a ? em1 : em0) + eDepth + 1.0f, earlyDiffG);
                    eR = earlyAp[1][(size_t) a].process (eR, apMs[1][a] * 0.001f * fs - eDepth * (a ? em1 : em0) + eDepth + 1.0f, earlyDiffG);
                }
                eL *= 0.35f * earlyLevel;
                eR *= 0.35f * earlyLevel;

                // ---- Late field ----
                float lInL = pL + eSend * eL, lInR = pR + eSend * eR;
                for (int a = 0; a < 4; ++a)
                {
                    lInL = inAp[0][(size_t) a].process (lInL, inApMs[0][a] * 0.001f * fs, lateDiffG);
                    lInR = inAp[1][(size_t) a].process (lInR, inApMs[1][a] * 0.001f * fs, lateDiffG);
                }
                const float injL = lInL + lCross * lInR, injR = lInR + lCross * lInL;

                lPhase += lRateInc;
                if (lPhase > juce::MathConstants<float>::twoPi) lPhase -= juce::MathConstants<float>::twoPi;

                std::array<float, numLines> x;
                for (int i = 0; i < numLines; ++i)
                {
                    curLen[(size_t) i] += (targetLen[(size_t) i] - curLen[(size_t) i]) * 0.0005f;
                    const float mod = lDepth * (1.0f + std::sin (lPhase + (float) i * 0.785398f));
                    float v = lines[(size_t) i].read (juce::jmax (2.0f, curLen[(size_t) i] + mod));
                    // 3-band decay: bass / mid / high decay at their own rate
                    const float low  = bassF[(size_t) i].lp (v);
                    const float high = trebleF[(size_t) i].hp (v);
                    const float mid  = v - low - high;
                    x[(size_t) i] = low * gBass[(size_t) i] + mid * gMid[(size_t) i] + high * gHigh[(size_t) i];
                }
                const float outL = 0.30f * (x[0] - x[2] + x[4] - x[6]);
                const float outR = 0.30f * (x[1] - x[3] + x[5] - x[7]);

                hadamard (x);
                for (int i = 0; i < numLines; ++i)
                {
                    const float inj = (i & 1) ? injR : injL;
                    lines[(size_t) i].push (x[(size_t) i] + inj * 0.35f);
                }

                // ---- Combine, width, high cut, ducking, mix ----
                float wL = earlyGain * eL + lateGain * outL;
                float wR = earlyGain * eR + lateGain * outR;
                const float mid = 0.5f * (wL + wR), side = 0.5f * (wL - wR) * width;
                wL = mid + side; wR = mid - side;
                wL = highCutF[1].lp (highCutF[0].lp (wL));
                wR = highCutF[3].lp (highCutF[2].lp (wR));

                const float duckGain = 1.0f - duck * juce::jmin (1.0f, env * 2.0f);
                left[n]  = dryL * dryGain + wL * wetGain * duckGain;
                right[n] = dryR * dryGain + wR * wetGain * duckGain;
            }
        }

    private:
        static void hadamard (std::array<float, numLines>& v) noexcept
        {
            for (int h = 1; h < numLines; h <<= 1)
                for (int i = 0; i < numLines; i += h << 1)
                    for (int j = i; j < i + h; ++j)
                    {
                        const float a = v[(size_t) j], b = v[(size_t) (j + h)];
                        v[(size_t) j] = a + b;
                        v[(size_t) (j + h)] = a - b;
                    }
            const float norm = 0.35355339f;   // 1 / sqrt(8): energy preserving
            for (auto& s : v) s *= norm;
        }

        double sr = 48000.0;
        Settings settings;
        float lastHash = -1.0f;

        std::array<Delay, 2> predelay, earlyLine;
        std::array<std::array<AllPass, 2>, 2> earlyAp;
        std::array<std::array<AllPass, 4>, 2> inAp;
        std::array<Delay, numLines> lines;
        std::array<OnePole, 4> lowCutF, highCutF;
        std::array<OnePole, numLines> bassF, trebleF;
        std::array<float, numLines> targetLen {}, curLen {}, gMid {}, gBass {}, gHigh {};

        float predelaySamples = 1.0f, curPredelay = 1.0f, earlySpanSamples = 100.0f, curSpan = 100.0f;
        float earlyDiffG = 0.5f, lateDiffG = 0.5f, earlyLevel = 1.0f, modScale = 1.0f;
        float earlyGainT = 0.7f, lateGainT = 0.7f, dryGainT = 1.0f, wetGainT = 0.0f;
        float earlyGain = 0.7f, lateGain = 0.7f, dryGain = 1.0f, wetGain = 0.0f;
        float ePhase = 0.0f, lPhase = 0.0f, env = 0.0f, attackCoef = 0.0f, releaseCoef = 0.0f;
    };
}
