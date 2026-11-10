#pragma once
#include <juce_dsp/juce_dsp.h>
#include "ParametricEQ.h"
#include "../Parameters.h"

/**
    One equaliser with Pro-Q 4's three processing modes:

      Zero Latency   minimum-phase IIR filters (the classic, no delay).
      Natural Phase  the same filters running at twice the sample rate, so the response near Nyquist
                     matches the analog prototype instead of being cramped; small latency from the
                     oversampling filters.
      Linear Phase   an FIR with exactly the same magnitude response as the IIR curve and no phase shift,
                     at the cost of latency (half the FIR length). Resolution sets the FIR length: longer
                     filters are more accurate in the low end. Mid/side and left/right placements are
                     handled with a 2x2 filter matrix (L->L, R->L, L->R, R->R).
*/
namespace eq
{
    inline const juce::StringArray& processingModeNames() { static const juce::StringArray n { "Zero Latency", "Natural Phase", "Linear Phase" }; return n; }
    inline const juce::StringArray& resolutionNames() { static const juce::StringArray n { "Low", "Medium", "High", "Very High", "Maximum" }; return n; }

    /** FIR length for a resolution at a sample rate (scaled so the frequency resolution stays the same at high rates). */
    inline int firLength (int resolution, double sampleRate)
    {
        int n = 4096 << juce::jlimit (0, 4, resolution);
        for (double r = sampleRate; r > 50000.0; r *= 0.5) n <<= 1;
        return juce::jmin (n, 1 << 18);
    }

    class LinearPhaseEq : private juce::Thread
    {
    public:
        using Bands = std::array<BandSettings, Params::numBands>;

        LinearPhaseEq() : juce::Thread ("Codex linear-phase EQ") {}
        ~LinearPhaseEq() override { stopThread (4000); }

        void prepare (double rate, int maxBlock)
        {
            stopThread (4000);
            sr = rate;
            const juce::dsp::ProcessSpec spec { rate, (juce::uint32) maxBlock, 1 };
            for (auto& c : conv) { c.prepare (spec); c.reset(); }
            for (auto& b : scratch) b.assign ((size_t) maxBlock, 0.0f);
            for (auto& b : ins) b.assign ((size_t) maxBlock, 0.0f);
            dirty = true;
            built.store (-1);
            startThread (juce::Thread::Priority::low);
        }

        /** Called from the audio thread with the current bands and resolution; rebuilds in the background when they change. */
        void update (const Bands& bands, int resolution)
        {
            const juce::SpinLock::ScopedTryLockType lock (pending);
            if (! lock.isLocked()) return;
            if (bands != next || resolution != nextRes || dirty)
            {
                next = bands;
                nextRes = resolution;
                dirty = false;
                generation++;
                notify();
            }
        }

        int getLatency (int resolution) const { return firLength (resolution, sr) / 2; }

        void process (float* l, float* r, int n) noexcept
        {
            std::copy (l, l + n, ins[0].begin());
            std::copy (r, r + n, ins[1].begin());
            auto run = [&] (juce::dsp::Convolution& c, int input, float* out)
            {
                float* inPtr = ins[(size_t) input].data();
                juce::dsp::AudioBlock<float> inB (&inPtr, 1, (size_t) n), outB (&out, 1, (size_t) n);
                c.process (juce::dsp::ProcessContextNonReplacing<float> (inB, outB));
            };
            run (conv[0], 0, l);
            run (conv[3], 1, r);
            if (cross.load (std::memory_order_relaxed))
            {
                float* s0 = scratch[0].data();
                float* s1 = scratch[1].data();
                run (conv[1], 1, s0);
                run (conv[2], 0, s1);
                for (int i = 0; i < n; ++i) { l[i] += s0[i]; r[i] += s1[i]; }
            }
        }

        /** True once the filters for the current settings have been handed to the convolution engines. */
        bool isUpToDate() const { return built.load() == generationSnapshot(); }

    private:
        int generationSnapshot() const { const juce::SpinLock::ScopedLockType l (pending); return generation; }

        void run() override
        {
            juce::HeapBlock<float> fftBuf;
            while (! threadShouldExit())
            {
                Bands bands;
                int res, gen;
                {
                    const juce::SpinLock::ScopedLockType l (pending);
                    bands = next; res = nextRes; gen = generation;
                }
                if (gen != built.load())
                {
                    build (bands, res);
                    built.store (gen);
                }
                wait (-1);
            }
        }

        void build (const Bands& bands, int res)
        {
            const int N = firLength (res, sr);
            const int order = juce::roundToInt (std::log2 ((double) N));
            juce::dsp::FFT fft (order);

            // magnitudes of the four matrix elements on the FFT grid
            std::array<BandDesign, Params::numBands> designs;
            for (int b = 0; b < Params::numBands; ++b) designs[(size_t) b] = design (bands[(size_t) b], sr);
            std::array<std::vector<float>, 4> spec;
            for (auto& s : spec) s.assign ((size_t) N / 2 + 1, 0.0f);
            bool hasCross = false;
            for (int k = 0; k <= N / 2; ++k)
            {
                const double f = juce::jmax (1.0, (double) k * sr / (double) N);
                double m = 1.0, s = 1.0, dl = 1.0, dr = 1.0;
                for (int b = 0; b < Params::numBands; ++b)
                {
                    const auto& d = designs[(size_t) b];
                    if (d.numStages == 0) continue;
                    const double g = magnitude (d, f, sr);
                    switch (bands[(size_t) b].placement)
                    {
                        case Placement::Stereo: m *= g; s *= g; break;
                        case Placement::Mid:    m *= g; break;
                        case Placement::Side:   s *= g; break;
                        case Placement::Left:   dl *= g; break;
                        case Placement::Right:  dr *= g; break;
                    }
                }
                spec[0][(size_t) k] = (float) (dl * 0.5 * (m + s));
                spec[1][(size_t) k] = (float) (dl * 0.5 * (m - s));   // R -> L
                spec[2][(size_t) k] = (float) (dr * 0.5 * (m - s));   // L -> R
                spec[3][(size_t) k] = (float) (dr * 0.5 * (m + s));
                hasCross = hasCross || std::abs (m - s) > 1.0e-6;
            }

            std::vector<float> work ((size_t) N * 2);
            for (int e = 0; e < 4; ++e)
            {
                if (threadShouldExit()) return;
                // zero-phase real spectrum -> impulse centred at N/2 -> Blackman window
                std::fill (work.begin(), work.end(), 0.0f);
                for (int k = 0; k < N; ++k)
                {
                    const int kk = k <= N / 2 ? k : N - k;
                    work[(size_t) (2 * k)] = spec[(size_t) e][(size_t) kk];
                }
                fft.performRealOnlyInverseTransform (work.data());
                const float scale = inverseScale (fft, N);
                juce::AudioBuffer<float> ir (1, N);
                auto* h = ir.getWritePointer (0);
                for (int i = 0; i < N; ++i)
                {
                    const int src = (i + N / 2) % N;   // circular shift: the impulse peak lands on N/2
                    const double w = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (N - 1))
                                   + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * i / (N - 1));
                    h[i] = work[(size_t) src] * scale * (float) w;
                }
                conv[(size_t) e].loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::no,
                                                      juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
            }
            cross.store (hasCross);
        }

        /** Normalisation of JUCE's inverse real FFT, measured once (a flat spectrum must give a unit impulse). */
        static float inverseScale (juce::dsp::FFT& fft, int N)
        {
            std::vector<float> t ((size_t) N * 2, 0.0f);
            for (int k = 0; k < N; ++k) t[(size_t) (2 * k)] = 1.0f;
            fft.performRealOnlyInverseTransform (t.data());
            return std::abs (t[0]) > 1.0e-9f ? 1.0f / t[0] : 1.0f;
        }

        double sr = 48000.0;
        std::array<juce::dsp::Convolution, 4> conv;   // LL, RL (R->L), LR (L->R), RR
        std::array<std::vector<float>, 2> scratch, ins;
        std::atomic<bool> cross { false };
        juce::SpinLock pending;
        Bands next {};
        int nextRes = 1, generation = 0;
        bool dirty = true;
        std::atomic<int> built { -1 };
    };

    /** The equaliser as used by the plugin: band settings + processing mode. */
    class EqEngine
    {
    public:
        void prepare (double rate, int maxBlock)
        {
            sr = rate;
            zeroLatency.prepare (rate);
            natural.prepare (rate * 2.0);
            os = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
            os->initProcessing ((size_t) maxBlock);
            linear.prepare (rate, maxBlock);
            lastMode = -1;
        }

        void setBand (int b, const BandSettings& s)
        {
            bands[(size_t) b] = s;
            zeroLatency.setBand (b, s);
            natural.setBand (b, s);
        }

        void setMode (int m, int resolution)
        {
            mode = juce::jlimit (0, 2, m);
            res = juce::jlimit (0, 4, resolution);
            if (mode == 2) linear.update (bands, res);
            if (mode != lastMode)
            {
                zeroLatency.reset(); natural.reset();
                if (os) os->reset();
                lastMode = mode;
            }
        }

        int getLatencyInSamples() const
        {
            if (mode == 1 && os) return juce::roundToInt (os->getLatencyInSamples());
            if (mode == 2) return linear.getLatency (res);
            return 0;
        }

        void process (float* l, float* r, int n) noexcept
        {
            if (mode == 0) { zeroLatency.process (l, r, n); return; }
            if (mode == 2) { linear.process (l, r, n); return; }
            float* ch[] = { l, r };
            juce::dsp::AudioBlock<float> block (ch, 2, (size_t) n);
            auto up = os->processSamplesUp (block);
            natural.process (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
            os->processSamplesDown (block);
        }

        bool isLinearUpToDate() const { return linear.isUpToDate(); }
        int getMode() const { return mode; }

    private:
        double sr = 48000.0;
        ParametricEQ<Params::numBands> zeroLatency, natural;
        std::unique_ptr<juce::dsp::Oversampling<float>> os;
        LinearPhaseEq linear;
        std::array<BandSettings, Params::numBands> bands {};
        int mode = 0, res = 1, lastMode = -1;
    };
}
