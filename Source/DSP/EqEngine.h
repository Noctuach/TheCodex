#pragma once
#include <juce_dsp/juce_dsp.h>
#include "ParametricEQ.h"
#include "PartitionedConvolver.h"
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

    /**
        Linear-phase FIR equaliser. A worker thread designs the four filters of the 2x2 stereo matrix (so Mid / Side /
        Left / Right bands work) and hands them to a PartitionedConvolver, which swaps them in on the audio thread
        without losing the input history. Latency = half the FIR length + one convolution partition.
    */
    class LinearPhaseEq : private juce::Thread
    {
    public:
        using Bands = std::array<BandSettings, Params::numBands>;

        LinearPhaseEq() : juce::Thread ("Codex linear-phase EQ") {}
        ~LinearPhaseEq() override { stopThread (4000); }

        void prepare (double rate, int)
        {
            stopThread (4000);
            sr = rate;
            convolver.prepare (firLength (4, rate));
            {
                const juce::SpinLock::ScopedLockType l (pending);
                dirty = true;
            }
            requested.store (true);
            builtGeneration = -1;
            startThread (juce::Thread::Priority::low);
        }

        /** Audio thread: current bands and resolution; a rebuild is requested when they change (never blocks). */
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
                requested.store (true);   // polled by the worker: no event is signalled from the audio thread
            }
        }

        /** Latency of the running filter (half its length) plus the convolver's partition. */
        int getLatency() const
        {
            const int len = convolver.getInstalledLength();
            return (len > 0 ? len / 2 : 0) + PartitionedConvolver::B;
        }

        void process (float* l, float* r, int n) noexcept { convolver.process (l, r, n); }

        /** True once the filters for the latest settings are running. */
        bool isUpToDate() const
        {
            int gen;
            { const juce::SpinLock::ScopedLockType l (pending); gen = generation; }
            return convolver.getInstalledTag() == gen;
        }

        void resetHistory() noexcept { convolver.reset(); }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                convolver.collectGarbage();
                if (requested.exchange (false))
                {
                    Bands bands;
                    int res, gen;
                    {
                        const juce::SpinLock::ScopedLockType l (pending);
                        bands = next; res = nextRes; gen = generation;
                    }
                    if (gen != builtGeneration)
                    {
                        build (bands, res, gen);
                        builtGeneration = gen;
                    }
                }
                wait (10);
            }
        }

        void build (const Bands& bands, int res, int gen)
        {
            const int N = firLength (res, sr);
            const int order = juce::roundToInt (std::log2 ((double) N));
            juce::dsp::FFT fft (order);

            std::array<BandDesign, Params::numBands> designs;
            for (int b = 0; b < Params::numBands; ++b) designs[(size_t) b] = design (bands[(size_t) b], sr);
            std::array<std::vector<float>, 4> spec;
            for (auto& sp : spec) sp.assign ((size_t) N / 2 + 1, 0.0f);
            bool hasCross = false;
            for (int k = 0; k <= N / 2; ++k)
            {
                if (threadShouldExit()) return;
                const double f = juce::jmax (1.0, (double) k * sr / (double) N);
                double m = 1.0, sd = 1.0, dl = 1.0, dr = 1.0;
                for (int b = 0; b < Params::numBands; ++b)
                {
                    const auto& d = designs[(size_t) b];
                    if (d.numStages == 0) continue;
                    const double g = magnitude (d, f, sr);
                    switch (bands[(size_t) b].placement)
                    {
                        case Placement::Stereo: m *= g; sd *= g; break;
                        case Placement::Mid:    m *= g; break;
                        case Placement::Side:   sd *= g; break;
                        case Placement::Left:   dl *= g; break;
                        case Placement::Right:  dr *= g; break;
                    }
                }
                spec[0][(size_t) k] = (float) (dl * 0.5 * (m + sd));
                spec[1][(size_t) k] = (float) (dl * 0.5 * (m - sd));   // R -> L
                spec[2][(size_t) k] = (float) (dr * 0.5 * (m - sd));   // L -> R
                spec[3][(size_t) k] = (float) (dr * 0.5 * (m + sd));
                hasCross = hasCross || std::abs (m - sd) > 1.0e-6;
            }

            // zero-phase spectrum -> impulse centred at N/2 -> Blackman window
            std::vector<float> work ((size_t) N * 2);
            std::fill (work.begin(), work.end(), 0.0f);
            work[0] = 1.0f;
            fft.performRealOnlyForwardTransform (work.data(), true);
            fft.performRealOnlyInverseTransform (work.data());
            const float scale = std::abs (work[0]) > 1.0e-9f ? 1.0f / work[0] : 1.0f;

            std::array<std::vector<float>, 4> irs;
            for (int e = 0; e < 4; ++e)
            {
                if (threadShouldExit()) return;
                std::fill (work.begin(), work.end(), 0.0f);
                for (int k = 0; k < N; ++k)
                {
                    const int kk = k <= N / 2 ? k : N - k;
                    work[(size_t) (2 * k)] = spec[(size_t) e][(size_t) kk];
                }
                fft.performRealOnlyInverseTransform (work.data());
                auto& h = irs[(size_t) e];
                h.resize ((size_t) N);
                for (int i = 0; i < N; ++i)
                {
                    const int src = (i + N / 2) % N;
                    const double w = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (N - 1))
                                   + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * i / (N - 1));
                    h[(size_t) i] = work[(size_t) src] * scale * (float) w;
                }
            }
            auto set = PartitionedConvolver::FilterSet::create ({ irs[0].data(), hasCross ? irs[1].data() : nullptr,
                                                                  hasCross ? irs[2].data() : nullptr, irs[3].data() }, N, hasCross);
            set->tag = gen;
            convolver.offer (std::move (set));
        }

        double sr = 48000.0;
        PartitionedConvolver convolver;
        mutable juce::SpinLock pending;
        Bands next {};
        int nextRes = 1, generation = 0;
        bool dirty = true;
        std::atomic<bool> requested { true };
        int builtGeneration = -1;   // worker only
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
                if (mode == 2) linear.resetHistory();
                lastMode = mode;
            }
        }

        int getLatencyInSamples() const
        {
            if (mode == 1 && os) return juce::roundToInt (os->getLatencyInSamples());
            if (mode == 2) return linear.getLatency();
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

        bool isLinearUpToDate() const { return mode != 2 || linear.isUpToDate(); }
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
