#pragma once
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

/**
    Stereo 2x2 FIR convolver (uniform partitioned, overlap-save) for the linear-phase EQ.

      out L = in L * h_LL + in R * h_RL
      out R = in L * h_LR + in R * h_RR

    The input history is kept as a frequency-domain delay line that does not depend on the filter, so a new filter
    can be swapped in at any block boundary without losing history (unlike engines that start a new convolver from
    silence). The swap is crossfaded over one block. Filters are prepared on a worker thread (FilterSet), handed over
    through a lock-free slot and installed on the audio thread without allocating; retired sets are freed by the worker.

    Latency: one partition (B samples) from the input buffering, plus whatever delay the filters themselves contain.
*/
class PartitionedConvolver
{
public:
    static constexpr int B = 256;              // partition size
    static constexpr int fftOrder = 9;         // 2B
    static constexpr int fftSize = 2 * B, bins = B + 1;
    using Cpx = std::complex<float>;

    /** Frequency-domain partitions of the four filters. Built (and freed) on a non-realtime thread. */
    struct FilterSet
    {
        int partitions = 0, irLength = 0, tag = -1;
        bool cross = false;
        std::array<std::vector<Cpx>, 4> h;   // LL, RL, LR, RR: partitions * bins each

        /** irs: four impulse responses of equal length (any length up to maxLength). */
        static std::unique_ptr<FilterSet> create (const std::array<const float*, 4>& irs, int length, bool hasCross)
        {
            auto set = std::make_unique<FilterSet>();
            set->irLength = length;
            set->partitions = (length + B - 1) / B;
            set->cross = hasCross;
            juce::dsp::FFT fft (fftOrder);
            std::vector<float> work ((size_t) fftSize * 2);
            // normalisation of JUCE's real inverse FFT, measured (a unit impulse must come back as 1)
            std::fill (work.begin(), work.end(), 0.0f);
            work[0] = 1.0f;
            fft.performRealOnlyForwardTransform (work.data(), true);
            fft.performRealOnlyInverseTransform (work.data());
            const float scale = std::abs (work[0]) > 1.0e-9f ? 1.0f / work[0] : 1.0f;
            for (int e = 0; e < 4; ++e)
            {
                auto& dst = set->h[(size_t) e];
                dst.assign ((size_t) set->partitions * bins, Cpx {});
                if (irs[(size_t) e] == nullptr) continue;
                for (int p = 0; p < set->partitions; ++p)
                {
                    std::fill (work.begin(), work.end(), 0.0f);
                    for (int i = 0; i < B; ++i)
                    {
                        const int idx = p * B + i;
                        if (idx < length) work[(size_t) i] = irs[(size_t) e][idx];
                    }
                    fft.performRealOnlyForwardTransform (work.data(), true);
                    for (int k = 0; k < bins; ++k)
                        dst[(size_t) (p * bins + k)] = Cpx { work[(size_t) (2 * k)], work[(size_t) (2 * k + 1)] } * scale;
                }
            }
            return set;
        }
    };

    void prepare (int maxIrLength)
    {
        maxPartitions = (maxIrLength + B - 1) / B;
        for (auto& f : fdl) f.assign ((size_t) maxPartitions * bins, Cpx {});
        for (auto& b : inBuf) b.assign ((size_t) fftSize, 0.0f);
        for (auto& b : outBuf) b.assign ((size_t) B, 0.0f);
        for (auto& b : fadeBuf) b.assign ((size_t) B, 0.0f);
        work.assign ((size_t) fftSize * 2, 0.0f);
        acc.assign ((size_t) bins, Cpx {});
        fdlPos = 0;
        fill = 0;
        active.reset();
        retired.reset();
        spare.reset();
        installedLength.store (0);
        installedTag.store (-1);
        const juce::SpinLock::ScopedLockType sl (slotLock);
        pending.reset();
    }

    void reset()
    {
        for (auto& f : fdl) std::fill (f.begin(), f.end(), Cpx {});
        for (auto& b : inBuf) std::fill (b.begin(), b.end(), 0.0f);
        for (auto& b : outBuf) std::fill (b.begin(), b.end(), 0.0f);
        fill = 0;
    }

    /** Worker thread: offer a new filter set (replaces an older one that was not installed yet). */
    void offer (std::unique_ptr<FilterSet> set)
    {
        std::unique_ptr<FilterSet> old;
        {
            const juce::SpinLock::ScopedLockType sl (slotLock);
            old = std::move (pending);
            pending = std::move (set);
        }
        // `old` is freed here, on the worker
    }

    /** Worker thread: free a set the audio thread has retired. */
    void collectGarbage()
    {
        std::unique_ptr<FilterSet> dead;
        const juce::SpinLock::ScopedLockType sl (slotLock);
        dead = std::move (retired);
    }

    /** Filter length of the installed set (0 = none yet: the convolver then outputs silence). */
    int getInstalledLength() const noexcept { return installedLength.load(); }
    int getInstalledTag() const noexcept { return installedTag.load(); }
    bool hasPending() const { const juce::SpinLock::ScopedLockType sl (slotLock); return pending != nullptr; }
    /** Tests only: the tag of the set waiting to be installed (-1 = none), and whether a retired set still waits to be freed. */
    int getPendingTagForTesting() const { const juce::SpinLock::ScopedLockType sl (slotLock); return pending != nullptr ? pending->tag : -1; }
    bool hasRetiredForTesting() const { const juce::SpinLock::ScopedLockType sl (slotLock); return retired != nullptr; }

    /** Audio thread. In-place stereo processing; any block size. */
    void process (float* l, float* r, int n) noexcept
    {
        float* io[] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                inBuf[(size_t) c][(size_t) (B + fill)] = io[c][i];
                io[c][i] = outBuf[(size_t) c][(size_t) fill];
            }
            if (++fill == B)
            {
                processPartition();
                fill = 0;
            }
        }
    }

private:
    void processPartition() noexcept
    {
        // forward FFT of [previous B | new B] for both inputs into the frequency-domain delay line
        fdlPos = (fdlPos + maxPartitions - 1) % maxPartitions;
        for (int c = 0; c < 2; ++c)
        {
            std::copy (inBuf[(size_t) c].begin(), inBuf[(size_t) c].end(), work.begin());
            std::fill (work.begin() + fftSize, work.end(), 0.0f);
            fft.performRealOnlyForwardTransform (work.data(), true);
            auto* dst = fdl[(size_t) c].data() + (size_t) fdlPos * bins;
            for (int k = 0; k < bins; ++k) dst[k] = { work[(size_t) (2 * k)], work[(size_t) (2 * k + 1)] };
            std::copy (inBuf[(size_t) c].begin() + B, inBuf[(size_t) c].end(), inBuf[(size_t) c].begin());
        }

        // a new filter set? (try-lock: never blocks the audio thread)
        std::unique_ptr<FilterSet> incoming;
        {
            const juce::SpinLock::ScopedTryLockType sl (slotLock);
            if (sl.isLocked() && pending != nullptr && retired == nullptr)
                incoming = std::move (pending);
        }

        if (incoming != nullptr && active != nullptr)
        {
            // crossfade over this block: old filter fading out, new filter fading in (same input history)
            for (int c = 0; c < 2; ++c) convolveChannel (*active, c, fadeBuf[(size_t) c].data());
            auto old = std::move (active);
            active = std::move (incoming);
            for (int c = 0; c < 2; ++c)
            {
                convolveChannel (*active, c, outBuf[(size_t) c].data());
                for (int i = 0; i < B; ++i)
                {
                    const float t = (float) (i + 1) / (float) B;
                    outBuf[(size_t) c][(size_t) i] = fadeBuf[(size_t) c][(size_t) i] * (1.0f - t) + outBuf[(size_t) c][(size_t) i] * t;
                }
            }
            const juce::SpinLock::ScopedTryLockType sl (slotLock);
            if (sl.isLocked()) retired = std::move (old);
            else               spare = std::move (old);   // extremely rare: keep it until the next block
            installedLength.store (active->irLength);
            installedTag.store (active->tag);
            return;
        }
        if (incoming != nullptr)
        {
            active = std::move (incoming);
            installedLength.store (active->irLength);
            installedTag.store (active->tag);
        }
        if (spare != nullptr)
        {
            const juce::SpinLock::ScopedTryLockType sl (slotLock);
            if (sl.isLocked() && retired == nullptr) retired = std::move (spare);
        }

        for (int c = 0; c < 2; ++c)
        {
            if (active != nullptr) convolveChannel (*active, c, outBuf[(size_t) c].data());
            else std::fill (outBuf[(size_t) c].begin(), outBuf[(size_t) c].end(), 0.0f);
        }
    }

    /** One output channel: sum over partitions of X_in[k - p] * H[p] for both inputs, then one inverse FFT. */
    void convolveChannel (const FilterSet& set, int outCh, float* dest) noexcept
    {
        std::fill (acc.begin(), acc.end(), Cpx {});
        const int P = juce::jmin (set.partitions, maxPartitions);
        for (int in = 0; in < 2; ++in)
        {
            // filter index: out L <- in L: LL(0), in R: RL(1); out R <- in L: LR(2), in R: RR(3)
            const int e = outCh == 0 ? (in == 0 ? 0 : 1) : (in == 0 ? 2 : 3);
            if ((e == 1 || e == 2) && ! set.cross) continue;
            const auto& h = set.h[(size_t) e];
            const auto& x = fdl[(size_t) in];
            for (int p = 0; p < P; ++p)
            {
                const int slotIdx = (fdlPos + p) % maxPartitions;
                const Cpx* xp = x.data() + (size_t) slotIdx * bins;
                const Cpx* hp = h.data() + (size_t) p * bins;
                for (int k = 0; k < bins; ++k) acc[(size_t) k] += xp[k] * hp[k];
            }
        }
        for (int k = 0; k < bins; ++k) { work[(size_t) (2 * k)] = acc[(size_t) k].real(); work[(size_t) (2 * k + 1)] = acc[(size_t) k].imag(); }
        std::fill (work.begin() + 2 * bins, work.end(), 0.0f);
        fft.performRealOnlyInverseTransform (work.data());
        // overlap-save: the second half is valid
        for (int i = 0; i < B; ++i) dest[i] = work[(size_t) (B + i)];
    }

    juce::dsp::FFT fft { fftOrder };
    int maxPartitions = 1, fdlPos = 0, fill = 0;
    std::array<std::vector<Cpx>, 2> fdl;
    std::array<std::vector<float>, 2> inBuf, outBuf, fadeBuf;
    std::vector<float> work;
    std::vector<Cpx> acc;
    std::unique_ptr<FilterSet> active, spare;
    mutable juce::SpinLock slotLock;
    std::unique_ptr<FilterSet> pending, retired;
    std::atomic<int> installedLength { 0 }, installedTag { -1 };
};
