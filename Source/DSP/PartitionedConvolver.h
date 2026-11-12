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

    /** Bins per partition in the split (re / im) layout: 257 rounded up to a whole number of MAC lanes (the extra bins stay 0). */
    static constexpr int lanes = 8, binsPad = (bins + lanes - 1) / lanes * lanes;

    /** Frequency-domain partitions of the four filters. Built (and freed) on a non-realtime thread. */
    struct FilterSet
    {
        int partitions = 0, irLength = 0, tag = -1;
        bool cross = false;
        // LL, RL, LR, RR: partitions * binsPad each, real and imaginary parts in separate arrays
        std::array<std::vector<float>, 4> re, im;

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
                auto& dr = set->re[(size_t) e];
                auto& di = set->im[(size_t) e];
                dr.assign ((size_t) set->partitions * binsPad, 0.0f);
                di.assign ((size_t) set->partitions * binsPad, 0.0f);
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
                    {
                        // same value as std::complex<float> * float (each part scaled)
                        const Cpx c = Cpx { work[(size_t) (2 * k)], work[(size_t) (2 * k + 1)] } * scale;
                        dr[(size_t) (p * binsPad + k)] = c.real();
                        di[(size_t) (p * binsPad + k)] = c.imag();
                    }
                }
            }
            return set;
        }
    };

    void prepare (int maxIrLength)
    {
        maxPartitions = (maxIrLength + B - 1) / B;
        for (auto& f : fdlRe) f.assign ((size_t) maxPartitions * binsPad, 0.0f);
        for (auto& f : fdlIm) f.assign ((size_t) maxPartitions * binsPad, 0.0f);
        for (auto& b : inBuf) b.assign ((size_t) fftSize, 0.0f);
        for (auto& b : outBuf) b.assign ((size_t) B, 0.0f);
        for (auto& b : fadeBuf) b.assign ((size_t) B, 0.0f);
        work.assign ((size_t) fftSize * 2, 0.0f);
        for (auto& w : outWork) w.assign ((size_t) fftSize * 2, 0.0f);
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
        for (auto& f : fdlRe) std::fill (f.begin(), f.end(), 0.0f);
        for (auto& f : fdlIm) std::fill (f.begin(), f.end(), 0.0f);
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
            auto* dr = fdlRe[(size_t) c].data() + (size_t) fdlPos * binsPad;
            auto* di = fdlIm[(size_t) c].data() + (size_t) fdlPos * binsPad;
            for (int k = 0; k < bins; ++k) { dr[k] = work[(size_t) (2 * k)]; di[k] = work[(size_t) (2 * k + 1)]; }
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
            convolve (*active, fadeBuf[0].data(), fadeBuf[1].data());
            auto old = std::move (active);
            active = std::move (incoming);
            convolve (*active, outBuf[0].data(), outBuf[1].data());
            for (int c = 0; c < 2; ++c)
            {
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

        if (active != nullptr) convolve (*active, outBuf[0].data(), outBuf[1].data());
        else for (auto& o : outBuf) std::fill (o.begin(), o.end(), 0.0f);
    }

    /** Both output channels: sum over partitions of X_in[k - p] * H[p] for both inputs, then one inverse FFT each.

        Per bin and output channel, the products are added in the same order as a plain loop over in (L, R) and
        p (0 .. P-1) would add them, starting from 0, with each complex product formed as (xr*hr - xi*hi, xr*hi + xi*hr)
        before it is added: the result is the same as accumulating std::complex<float> products, bit for bit.
        The loops run over blocks of `lanes` bins (accumulators held in registers across all partitions), with both
        outputs computed from each input load, and the circular delay line walked as two linear runs. */
    void convolve (const FilterSet& set, float* dest0, float* dest1) noexcept
    {
        const int P = juce::jmin (set.partitions, maxPartitions);
        const int run1 = juce::jmin (P, maxPartitions - fdlPos);   // slots fdlPos .. maxPartitions-1, then 0 ..
        float* w0 = outWork[0].data();
        float* w1 = outWork[1].data();

        for (int k0 = 0; k0 < binsPad; k0 += lanes)
        {
            float a0r[lanes] = {}, a0i[lanes] = {}, a1r[lanes] = {}, a1i[lanes] = {};
            for (int in = 0; in < 2; ++in)
            {
                // out L <- in L: LL(0), in R: RL(1); out R <- in L: LR(2), in R: RR(3)
                const int e0 = in == 0 ? 0 : 1, e1 = in == 0 ? 2 : 3;
                const bool use0 = e0 != 1 || set.cross, use1 = e1 != 2 || set.cross;
                const float* xR = fdlRe[(size_t) in].data() + k0;
                const float* xI = fdlIm[(size_t) in].data() + k0;
                const float* h0R = set.re[(size_t) e0].data() + k0;
                const float* h0I = set.im[(size_t) e0].data() + k0;
                const float* h1R = set.re[(size_t) e1].data() + k0;
                const float* h1I = set.im[(size_t) e1].data() + k0;
                if (use0 && use1)
                {
                    macRun<true, true> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, fdlPos, 0, run1);
                    macRun<true, true> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, -run1, run1, P);
                }
                else if (use0)
                {
                    macRun<true, false> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, fdlPos, 0, run1);
                    macRun<true, false> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, -run1, run1, P);
                }
                else if (use1)
                {
                    macRun<false, true> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, fdlPos, 0, run1);
                    macRun<false, true> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, -run1, run1, P);
                }
            }
            for (int j = 0; j < lanes; ++j)
            {
                const int k = k0 + j;
                if (k >= bins) break;
                w0[2 * k] = a0r[j]; w0[2 * k + 1] = a0i[j];
                w1[2 * k] = a1r[j]; w1[2 * k + 1] = a1i[j];
            }
        }

        float* dests[] = { dest0, dest1 };
        for (int c = 0; c < 2; ++c)
        {
            auto& w = outWork[(size_t) c];
            std::fill (w.begin() + 2 * bins, w.end(), 0.0f);
            fft.performRealOnlyInverseTransform (w.data());
            // overlap-save: the second half is valid
            for (int i = 0; i < B; ++i) dests[c][i] = w[(size_t) (B + i)];
        }
    }

    /** Partitions p = pBegin .. pEnd-1, delay-line slot p + slotOffset, for one block of `lanes` bins. */
    template <bool Out0, bool Out1>
    static inline void macRun (const float* xR, const float* xI, const float* h0R, const float* h0I, const float* h1R, const float* h1I,
                               float* __restrict a0r, float* __restrict a0i, float* __restrict a1r, float* __restrict a1i,
                               int slotOffset, int pBegin, int pEnd) noexcept
    {
        for (int p = pBegin; p < pEnd; ++p)
        {
            const size_t xo = (size_t) (p + slotOffset) * binsPad, ho = (size_t) p * binsPad;
            for (int j = 0; j < lanes; ++j)
            {
                const float xr = xR[xo + (size_t) j], xi = xI[xo + (size_t) j];
                if (Out0)
                {
                    const float hr = h0R[ho + (size_t) j], hi = h0I[ho + (size_t) j];
                    const float re = xr * hr - xi * hi;
                    const float im = xr * hi + xi * hr;
                    a0r[j] += re;
                    a0i[j] += im;
                }
                if (Out1)
                {
                    const float hr = h1R[ho + (size_t) j], hi = h1I[ho + (size_t) j];
                    const float re = xr * hr - xi * hi;
                    const float im = xr * hi + xi * hr;
                    a1r[j] += re;
                    a1i[j] += im;
                }
            }
        }
    }

    juce::dsp::FFT fft { fftOrder };
    int maxPartitions = 1, fdlPos = 0, fill = 0;
    std::array<std::vector<float>, 2> fdlRe, fdlIm;   // frequency-domain delay line, maxPartitions * binsPad per input
    std::array<std::vector<float>, 2> inBuf, outBuf, fadeBuf;
    std::vector<float> work;
    std::array<std::vector<float>, 2> outWork;        // per output channel: accumulated spectrum, then inverse FFT
    std::unique_ptr<FilterSet> active, spare;
    mutable juce::SpinLock slotLock;
    std::unique_ptr<FilterSet> pending, retired;
    std::atomic<int> installedLength { 0 }, installedTag { -1 };
};
