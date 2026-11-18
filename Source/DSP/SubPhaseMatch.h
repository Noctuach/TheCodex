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
      multiband compressor: their band sums carry LP + HP = an all-pass) and of the 10 Hz DC filters (saturator, and
      each distortion while its DC filter is in: it is the unit's last stage, so its phase reaches the output). Each
      LR crossover's LP + HP is applied as the equivalent lower-order all-pass (LR2 -> 1st order, LR4 -> one
      2nd-order section, LR8 -> two), designed with the same bilinear prototype (pre-warped when the module designs
      it oversampled), so the phase is the same at a quarter of the cost. A crossover section only fades in / out
      (20 ms smoothstep) while parked near Nyquist, where it is practically the identity in the sub's range, so the
      fade never cancels the sub at the crossover; it then glides (~8 ms) to its frequency, and glides back to the
      park before it leaves. The DC sections fade in place (a 10 Hz high-pass is ~14 degrees at 40 Hz: the mix
      cannot cancel).

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
        static constexpr int satSections = sat::maxBands - 1, compSections = 3, dcSections = 3;
        static constexpr int numSections = satSections + compSections + dcSections;
        enum { satDc = satSections + compSections, clip1Dc, clip2Dc };

        void prepare (double sampleRate) noexcept
        {
            rate = sampleRate;
            park = (float) (0.4 * sampleRate);
            step = (float) (1.0 / (0.02 * sampleRate));
            glideK = (float) (1.0 - std::exp (-(double) glideEvery / (0.004 * sampleRate)));   // 2 x 4 ms stages: ~8 ms glides
            dcR = (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sampleRate);   // as the modules' DC filters
            for (auto& s : sections) s = Section {};
            for (int k = satDc; k < numSections; ++k) setDc (sections[(size_t) k]);
            glideCount = 0;
        }

        /** A crossover all-pass (slope as sat::designCrossover: 0 = 6 dB complementary (no phase), 1 = LR2, 2 = LR4,
            3 = LR8) at fc, as the module designs it at designRate (the saturator works oversampled): the section runs
            at the base rate with fc pre-warped so its phase at low frequencies is the module's. on = false takes the
            section out. A section only fades in or out (20 ms) while it is parked at 0.4 x the sample rate, where it is
            practically the identity below a few kHz (the sub's range), so the dry / all-pass mix never cancels; it then
            glides (~8 ms) to its frequency. Leaving (off, or a new slope) glides back to the park first. */
        void setCrossover (int index, bool on, int slope, float fc, double designRate) noexcept
        {
            auto& s = sections[(size_t) index];
            s.on = on && slope > 0;
            if (s.on)
            {
                s.slopeWant = slope;
                if (fc != s.fcIn || designRate != s.rateIn)
                {
                    s.fcIn = fc;
                    s.rateIn = designRate;
                    s.fcWant = designRate == rate ? fc : warp (fc, designRate);
                }
            }
            plan (s);
        }
        void setCrossover (int index, bool on, int slope, float fc) noexcept { setCrossover (index, on, slope, fc, rate); }

        void setDcFilter (int index, bool on) noexcept { sections[(size_t) index].target = on ? 1.0f : 0.0f; }

        /** True when any section runs or fades (otherwise process() is the identity and need not be called). */
        bool isActive() const noexcept
        {
            for (const auto& s : sections) if (s.amount > 0.0f || s.target > 0.0f) return true;
            return false;
        }

        /** While the sub layer is silent (or right after prepare): every section jumps to its wanted state and forgets
            its history (nothing to fade). */
        void snapAndClear() noexcept
        {
            for (auto& s : sections)
            {
                if (s.isDc) s.amount = s.target;
                else if (s.on)
                {
                    if (s.slope != s.slopeWant || s.fc != s.fcWant || s.rampLeft > 0) design (s, s.slopeWant, s.fcWant);
                    s.amount = s.target = 1.0f;
                    s.fcGoal = s.fcWant;
                }
                else s.amount = s.target = 0.0f;
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
                if (s.amount <= 0.0f && ! s.isDc)   // fade-in starts: state as if x had been held (no ringing at the park)
                    for (int k = 0; k < s.numFilters; ++k) { auto& st = s.f[(size_t) k]; if (st.order == 2) { st.s1 = 0.0; st.s2 = x; } else st.s1 = x; }
                if (s.rampLeft > 0)   // a glide step: the frequency moves sample by sample (no steps in the phase)
                {
                    s.gNow = --s.rampLeft == 0 ? s.gEnd : s.gNow + s.dg;
                    for (int k = 0; k < s.numFilters; ++k) s.f[(size_t) k].setG (s.gNow);
                }
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
        static constexpr int glideEvery = 16;

        /** One all-pass stage as a TPT state-variable filter (bilinear, pre-warped at fc: the same transfer function as
            the reversed-denominator biquad of the crossover's LP prototype, but its state stays meaningful while the
            frequency moves, so a glide does not click). order 1: 2 LP - x; order 2: x - 2 k BP. The DC sections use
            a plain one-pole high-pass (y = x - x[n-1] + R y[n-1]) in the same state. */
        struct Stage
        {
            double g = 0.0, k = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, s1 = 0.0, s2 = 0.0;
            int order = 0;   // 0 = DC one-pole high-pass (a1 = R)

            void set (int ord, double gIn, double q) noexcept
            {
                order = ord;
                k = ord == 2 ? 1.0 / q : 0.0;
                setG (gIn);
            }
            void setG (double gIn) noexcept
            {
                g = gIn;
                if (order == 1) { a1 = g / (1.0 + g); return; }
                a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
            }
            inline float process (float xf) noexcept
            {
                const double x = xf;
                if (order == 2)
                {
                    const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
                    s1 = 2.0 * v1 - s1; s2 = 2.0 * v2 - s2;
                    return (float) (x - 2.0 * k * v1);
                }
                if (order == 1)
                {
                    const double v = (x - s1) * a1, lp = v + s1;
                    s1 = lp + v;
                    return (float) (2.0 * lp - x);
                }
                const double y = x - s1 + a1 * s2;
                s1 = x; s2 = y;
                return (float) y;
            }
        };

        struct Section
        {
            std::array<Stage, 2> f {};
            int numFilters = 0, slope = -1, slopeWant = -1;
            bool on = false, isDc = false;
            float fc = -1.0f, fcGoal = -1.0f, fcWant = -1.0f, fcIn = -1.0f, goalFor = -1.0f;
            float logFc = 0.0f, logMid = 0.0f, logGoal = 0.0f, amount = 0.0f, target = 0.0f;
            double rateIn = 0.0, gNow = 0.0, gEnd = 0.0, dg = 0.0;
            int rampLeft = 0;
        };

        /** fc of a bilinear design at designRate -> the base-rate fc with the same low-frequency response. */
        float warp (float fc, double designRate) const noexcept
        {
            const double t = std::tan (juce::MathConstants<double>::pi * std::min ((double) fc, 0.49 * designRate) / designRate) * designRate / rate;
            return (float) (rate / juce::MathConstants<double>::pi * std::atan (t));
        }

        /** Decides the fade target and the frequency to glide to (see setCrossover). Invariant: a section that is
            partly faded in (0 < amount < 1) sits at the park frequency. */
        void plan (Section& s) noexcept
        {
            if (s.isDc) return;
            if (s.amount <= 0.0f)   // out: free to redesign
            {
                if (! s.on) { s.target = 0.0f; return; }
                if (s.slope != s.slopeWant || s.fc != park) design (s, s.slopeWant, park);
                s.fcGoal = park;
                s.target = 1.0f;
                return;
            }
            const bool keep = s.on && s.slope == s.slopeWant;
            if (s.amount >= 1.0f && s.target >= 1.0f)   // fully in
            {
                if (keep) { s.fcGoal = s.fcWant; return; }
                s.fcGoal = park;                         // leaving: back to the park first, then fade out
                if (s.fc == park) s.target = 0.0f;
                return;
            }
            s.fcGoal = park;                             // fading (at the park)
            s.target = keep ? 1.0f : 0.0f;
        }

        /** Every glideEvery samples: sections not at their goal frequency take the next glide step (state kept). */
        void glide() noexcept
        {
            for (auto& s : sections)
            {
                if (s.isDc || (s.amount <= 0.0f && s.target <= 0.0f)) continue;
                plan (s);
                if (s.fc == s.fcGoal) continue;
                if (s.goalFor != s.fcGoal) { s.goalFor = s.fcGoal; s.logGoal = std::log (s.fcGoal); }
                // two one-pole stages in log frequency: the glide starts and ends without a corner in its speed
                s.logMid += glideK * (s.logGoal - s.logMid);
                const float lf = s.logFc + glideK * (s.logMid - s.logFc);
                float f = std::exp (lf);
                const bool there = std::abs (lf - s.logGoal) < 0.001f && std::abs (s.logMid - s.logGoal) < 0.001f;
                if (there) f = s.fcGoal;
                design (s, s.slope, f, there ? s.logGoal : lf, true);
                plan (s);
            }
        }

        void setDc (Section& s) noexcept
        {
            s.isDc = true;
            s.numFilters = 1;
            s.f[0].order = 0;
            s.f[0].a1 = (double) dcR;   // y = x - x[n-1] + R y[n-1]
        }

        /** LP + HP of the crossover = the all-pass whose poles are the LP prototype's (as sat::designCrossover designs
            it, bilinear with the frequency pre-warped): LR2 -> first order, LR4 -> Q 0.707, LR8 -> Q 0.54 and 1.31. */
        void design (Section& s, int slope, float fc, float logFc, bool glideStep = false) noexcept
        {
            const double g = std::tan (eq::pi_ * eq::clampFreq (fc, rate) / rate);
            if (glideStep && slope == s.slope)   // ramps to g over the next glideEvery samples (see process())
            {
                s.gEnd = g;
                s.dg = (g - s.gNow) / glideEvery;
                s.rampLeft = glideEvery;
                s.fc = fc;
                s.logFc = logFc;
                return;
            }
            s.gNow = s.gEnd = g;
            s.rampLeft = 0;
            if (slope != s.slope) for (auto& st : s.f) { st.s1 = st.s2 = 0.0; }
            if (slope == 1)      { s.f[0].set (1, g, 0.0); s.numFilters = 1; }
            else if (slope == 3) { s.f[0].set (2, g, 0.54119610); s.f[1].set (2, g, 1.30656296); s.numFilters = 2; }
            else                 { s.f[0].set (2, g, 0.70710678); s.numFilters = 1; }
            s.slope = slope;
            if (! glideStep) s.logMid = logFc;   // a jump (not a glide step): the glide restarts from rest here
            s.fc = fc;
            s.logFc = logFc;
        }
        void design (Section& s, int slope, float fc) noexcept { design (s, slope, fc, std::log (fc)); }

        std::array<Section, numSections> sections {};
        double rate = 48000.0;
        float park = 19200.0f, step = 0.001f, glideK = 0.1f, dcR = 0.999f;
        int glideCount = 0;
    };
}
