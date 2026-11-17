#pragma once
#include "ParametricEQ.h"
#include "MultibandSaturator.h"
#include <array>
#include <cmath>

/**
    Phase match for the clean sub layer, which is added after the effect chain: it must carry the chain's own delay
    and low-frequency phase, or it no longer lines up with the processed kick at 30-120 Hz.

    - FractionalDelay: a first-order Thiran all-pass for the fractional part of the chain latency (the oversamplers'
      latencies are fractional; the sub's ring buffer only gives whole samples). Flat magnitude, phase delay = d
      at low frequencies (d in [0.5, 1.5) is used, where it is most accurate).
    - PhaseMatch: the all-pass phase of every Linkwitz-Riley crossover in the chain (multiband saturator and
      multiband compressor: their band sums carry LP + HP = an all-pass) and the saturator's 10 Hz DC filter. Each
      LR crossover's LP + HP is applied as the equivalent lower-order all-pass (LR2 -> 1st order, LR4 -> one
      2nd-order section, LR8 -> two), designed with the same bilinear prototype, so the phase is the same at a
      quarter of the cost. Sections fade in / out over 20 ms on a smoothstep (no click when a module or its band
      count is switched); a crossover that moves glides to its new frequency (~8 ms) and is redesigned in place.
      The distortions' DC filters are not matched: a driven distortion sits in front of (or is) a clipping stage
      that squares the waveform again, so their phase hardly reaches the kick's fundamental (matching them made
      Sub ALIGN pick a 30 degree worse phase on the driven presets).

    Allocation-free; everything is a fixed-size array.
*/
namespace subalign
{
    struct FractionalDelay
    {
        double a = 0.0, x1 = 0.0, y1 = 0.0;
        bool active = false;

        /** d: fractional delay in samples (0 < d < 2); active = false bypasses it (and clears its state). */
        void set (bool on, double d) noexcept
        {
            if (! on) { if (active) { x1 = y1 = 0.0; } active = false; return; }
            a = (1.0 - d) / (1.0 + d);
            active = true;
        }
        void reset() noexcept { x1 = y1 = 0.0; }
        inline float process (float xf) noexcept
        {
            const double x = xf;
            const double y = a * x + x1 - a * y1;
            x1 = x; y1 = y;
            return (float) y;
        }
    };

    class PhaseMatch
    {
    public:
        static constexpr int satSections = sat::maxBands - 1, compSections = 3, dcSections = 1;
        static constexpr int numSections = satSections + compSections + dcSections;
        enum { satDc = satSections + compSections };

        void prepare (double sampleRate) noexcept
        {
            rate = sampleRate;
            step = (float) (1.0 / (0.02 * sampleRate));
            glideK = (float) (1.0 - std::exp (-(double) glideEvery / (0.008 * sampleRate)));   // ~8 ms glides
            dcR = (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sampleRate);   // as the modules' DC filters
            for (auto& s : sections) s = Section {};
            for (int k = satDc; k < numSections; ++k) setDc (sections[(size_t) k]);
            glideCount = 0;
        }

        /** A crossover all-pass (slope as sat::designCrossover: 0 = 6 dB complementary (no phase), 1 = LR2, 2 = LR4,
            3 = LR8) at fc; on = false fades the section out. A new frequency glides in (~8 ms, no click); a new slope
            fades the section out, and back in with the new design. */
        void setCrossover (int index, bool on, int slope, float fc) noexcept
        {
            auto& s = sections[(size_t) index];
            on = on && slope > 0;
            s.target = on ? 1.0f : 0.0f;
            if (! on) return;
            s.fcTarget = fc;
            if (slope != s.slope)
            {
                if (s.amount > 0.0f) { s.target = 0.0f; return; }   // fade out first, then redesign
                design (s, slope, fc);
            }
            else if (s.amount <= 0.0f && s.fc != fc)
                design (s, slope, fc);   // silent: no need to glide
        }

        void setDcFilter (int index, bool on) noexcept { sections[(size_t) index].target = on ? 1.0f : 0.0f; }

        /** True when any section runs or fades (otherwise process() is the identity and need not be called). */
        bool isActive() const noexcept
        {
            for (const auto& s : sections) if (s.amount > 0.0f || s.target > 0.0f) return true;
            return false;
        }

        /** While the sub layer is silent: every section jumps to its target and forgets its history (nothing to fade). */
        void snapAndClear() noexcept
        {
            for (auto& s : sections)
            {
                s.amount = s.target;
                if (s.slope > 0 && s.fc != s.fcTarget) design (s, s.slope, s.fcTarget);
                for (auto& f : s.f) { f.s1 = f.s2 = 0.0; }
            }
        }

        /** Fault recovery: forgets every section's filter history (fades and designs are kept). */
        void clearState() noexcept
        {
            for (auto& s : sections)
                for (auto& f : s.f) { f.s1 = f.s2 = 0.0; }
        }

        inline float process (float x) noexcept
        {
            if (++glideCount >= glideEvery) { glideCount = 0; glide(); }
            for (auto& s : sections)
            {
                if (s.amount <= 0.0f && s.target <= 0.0f) continue;
                float y = x;
                for (int k = 0; k < s.numFilters; ++k) y = s.f[(size_t) k].process (y);
                if (s.amount == s.target) { x = y; continue; }   // fully in (amount 1)
                s.amount = s.target > s.amount ? std::min (s.target, s.amount + step) : std::max (s.target, s.amount - step);
                const float g = s.amount * s.amount * (3.0f - 2.0f * s.amount);   // smoothstep: no corner at either end
                x += g * (y - x);
                if (s.amount <= 0.0f) for (auto& f : s.f) { f.s1 = f.s2 = 0.0; }   // fully out: clean state for next time
            }
            return x;
        }

    private:
        static constexpr int glideEvery = 2;

        struct Section
        {
            std::array<eq::Biquad, 2> f {};
            int numFilters = 0, slope = -1;
            float fc = -1.0f, fcTarget = -1.0f, amount = 0.0f, target = 0.0f;
        };

        /** Every 2 samples: crossovers that moved glide to their new frequency (redesigned in place, state kept). */
        void glide() noexcept
        {
            for (auto& s : sections)
            {
                if (s.slope <= 0 || s.fc == s.fcTarget || (s.amount <= 0.0f && s.target <= 0.0f)) continue;
                float f = std::exp (std::log (s.fc) + glideK * (std::log (s.fcTarget) - std::log (s.fc)));
                if (std::abs (f / s.fcTarget - 1.0f) < 0.001f) f = s.fcTarget;
                design (s, s.slope, f);
            }
        }

        void setDc (Section& s) noexcept
        {
            s.numFilters = 1;
            s.f[0].c = { 1.0, -1.0, 0.0, -(double) dcR, 0.0 };   // y = x - x[n-1] + R y[n-1]
        }

        /** LP + HP of the crossover = the all-pass whose poles are the LP prototype's: reverse its denominator. */
        void design (Section& s, int slope, float fc) noexcept
        {
            sat::Cascade lp, hp;
            sat::designCrossover (slope, fc, rate, lp, hp);
            const int n = slope == 3 ? 2 : 1;   // LR8: the two Butterworth sections (Q 0.54, 1.31); LR2 / LR4: one
            for (int k = 0; k < n; ++k)
            {
                const auto& c = lp.s[(size_t) k].c;
                s.f[(size_t) k].c = slope == 1 ? eq::Coeffs { c.a1, 1.0, 0.0, c.a1, 0.0 }     // (a1 + z^-1) / (1 + a1 z^-1)
                                               : eq::Coeffs { c.a2, c.a1, 1.0, c.a1, c.a2 };   // (a2 + a1 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2)
            }
            if (slope != s.slope) for (auto& f : s.f) { f.s1 = f.s2 = 0.0; }
            s.numFilters = n;
            s.slope = slope;
            s.fc = fc;
        }

        std::array<Section, numSections> sections {};
        double rate = 48000.0;
        float step = 0.001f, glideK = 0.1f, dcR = 0.999f;
        int glideCount = 0;
    };
}
