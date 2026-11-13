#pragma once
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>
#include <cstdint>

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

    /** Spectra are stored in blocks of `lanes` bins (257 rounded up to 264; the extra bins stay 0), block-major:
        [block][partition][lanes real parts, then lanes imaginary parts], so the MAC streams through memory linearly
        (one 64-byte line per block and partition). */
    static constexpr int lanes = 8, binsPad = (bins + lanes - 1) / lanes * lanes, numBlocks = binsPad / lanes, blockFloats = 2 * lanes;
    /** Index of bin k's real part (the imaginary part follows `lanes` floats later) in a spectrum array of `slots` partitions. */
    static constexpr size_t binIndex (int slot, int k, int slots) noexcept
    {
        return ((size_t) (k / lanes) * (size_t) slots + (size_t) slot) * blockFloats + (size_t) (k % lanes);
    }

    /** Frequency-domain partitions of the four filters. Built (and freed) on a non-realtime thread. */
    struct FilterSet
    {
        int partitions = 0, irLength = 0, tag = -1;
        bool cross = false;
        // LL, RL, LR, RR: partitions * binsPad * 2 floats each (layout: see binIndex)
        std::array<std::vector<float>, 4> h;

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
                dst.assign ((size_t) set->partitions * binsPad * 2, 0.0f);
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
                        const size_t idx = binIndex (p, k, set->partitions);
                        dst[idx] = c.real();
                        dst[idx + lanes] = c.imag();
                    }
                }
            }
            return set;
        }
    };

    void prepare (int maxIrLength)
    {
        maxPartitions = (maxIrLength + B - 1) / B;
        for (auto& f : fdl) f.assign ((size_t) maxPartitions * binsPad * 2, 0.0f);
        slotZero.assign ((size_t) maxPartitions, 1);           // the delay line starts cleared
        segments.assign ((size_t) maxPartitions + 2, Segment {});
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
        for (auto& f : fdl) std::fill (f.begin(), f.end(), 0.0f);
        std::fill (slotZero.begin(), slotZero.end(), (std::uint8_t) 1);
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
        // An input window that is exactly zero in both channels has an all-zero spectrum: its slot is only marked
        // (no FFT), and the MAC skips it (its products are +-0, which never change a sum that starts at +0).
        bool silent = true;
        for (int c = 0; c < 2 && silent; ++c)
            for (float v : inBuf[(size_t) c])
                if (v != 0.0f) { silent = false; break; }
        slotZero[(size_t) fdlPos] = silent ? 1 : 0;
        for (int c = 0; c < 2; ++c)
        {
            if (silent)
            {
                std::copy (inBuf[(size_t) c].begin() + B, inBuf[(size_t) c].end(), inBuf[(size_t) c].begin());
                continue;
            }
            std::copy (inBuf[(size_t) c].begin(), inBuf[(size_t) c].end(), work.begin());
            std::fill (work.begin() + fftSize, work.end(), 0.0f);
            fft.performRealOnlyForwardTransform (work.data(), true);
            auto* dst = fdl[(size_t) c].data();
            for (int k = 0; k < bins; ++k)
            {
                const size_t idx = binIndex (fdlPos, k, maxPartitions);
                dst[idx] = work[(size_t) (2 * k)];
                dst[idx + lanes] = work[(size_t) (2 * k + 1)];
            }
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
            convolve (*active, fadeBuf[0].data(), fadeBuf[1].data(), false);
            auto old = std::move (active);
            active = std::move (incoming);
            convolve (*active, outBuf[0].data(), outBuf[1].data(), false);
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

        if (active != nullptr) convolve (*active, outBuf[0].data(), outBuf[1].data(), true);
        else for (auto& o : outBuf) std::fill (o.begin(), o.end(), 0.0f);
    }

    /** Both output channels: sum over partitions of X_in[k - p] * H[p] for both inputs, then one inverse FFT each.

        Per bin and output channel, the products are added in the same order as a plain loop over in (L, R) and
        p (0 .. P-1) would add them, starting from 0, with each complex product formed as (xr*hr - xi*hi, xr*hi + xi*hr)
        before it is added: the result is the same as accumulating std::complex<float> products, bit for bit.
        The loops run over blocks of `lanes` bins (accumulators held in registers across all partitions), with both
        outputs computed from each input load, and the circular delay line walked as linear runs that skip silent slots.
        zeroShortcut: when every slot is silent, write silence without the inverse FFTs (not used while crossfading). */
    void convolve (const FilterSet& set, float* dest0, float* dest1, bool zeroShortcut) noexcept
    {
        const int P = juce::jmin (set.partitions, maxPartitions);
        // runs of partitions whose delay-line slot is not silent (slot = p + offset; the ring wraps at maxPartitions)
        int numSegments = 0;
        for (int p = 0; p < P;)
        {
            const int offset = fdlPos + p < maxPartitions ? fdlPos : fdlPos - maxPartitions;
            const int end = fdlPos + p < maxPartitions ? juce::jmin (P, maxPartitions - fdlPos) : P;
            while (p < end)
            {
                while (p < end && slotZero[(size_t) (p + offset)] != 0) ++p;
                if (p == end) break;
                const int begin = p;
                while (p < end && slotZero[(size_t) (p + offset)] == 0) ++p;
                segments[(size_t) numSegments++] = { begin, p, offset };
            }
        }
        if (numSegments == 0 && zeroShortcut)
        {
            // every product is zero: the output is silence
            std::fill (dest0, dest0 + B, 0.0f);
            std::fill (dest1, dest1 + B, 0.0f);
            return;
        }
        float* w0 = outWork[0].data();
        float* w1 = outWork[1].data();

        for (int blk = 0; blk < numBlocks; ++blk)
        {
            const int k0 = blk * lanes;
            float a0r[lanes] = {}, a0i[lanes] = {}, a1r[lanes] = {}, a1i[lanes] = {};
            for (int in = 0; in < 2; ++in)
            {
                // out L <- in L: LL(0), in R: RL(1); out R <- in L: LR(2), in R: RR(3)
                const int e0 = in == 0 ? 0 : 1, e1 = in == 0 ? 2 : 3;
                const bool use0 = e0 != 1 || set.cross, use1 = e1 != 2 || set.cross;
                const float* xR = fdl[(size_t) in].data() + (size_t) blk * (size_t) maxPartitions * blockFloats;
                const float* xI = xR + lanes;
                const float* h0R = set.h[(size_t) e0].data() + (size_t) blk * (size_t) set.partitions * blockFloats;
                const float* h0I = h0R + lanes;
                const float* h1R = set.h[(size_t) e1].data() + (size_t) blk * (size_t) set.partitions * blockFloats;
                const float* h1I = h1R + lanes;
                if (use0 && use1)
                {
                    for (int g = 0; g < numSegments; ++g)
                    {
                        const auto& sg = segments[(size_t) g];
                        macRun<true, true> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, sg.offset, sg.begin, sg.end);
                    }
                }
                else if (use0)
                {
                    for (int g = 0; g < numSegments; ++g)
                    {
                        const auto& sg = segments[(size_t) g];
                        macRun<true, false> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, sg.offset, sg.begin, sg.end);
                    }
                }
                else if (use1)
                {
                    for (int g = 0; g < numSegments; ++g)
                    {
                        const auto& sg = segments[(size_t) g];
                        macRun<false, true> (xR, xI, h0R, h0I, h1R, h1I, a0r, a0i, a1r, a1i, sg.offset, sg.begin, sg.end);
                    }
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
            const size_t xo = (size_t) (p + slotOffset) * blockFloats, ho = (size_t) p * blockFloats;
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
    std::array<std::vector<float>, 2> fdl;           // frequency-domain delay line per input, maxPartitions slots (layout: see binIndex)
    std::vector<std::uint8_t> slotZero;              // per delay-line slot: the input window was exactly zero (spectrum not stored)
    struct Segment { int begin = 0, end = 0, offset = 0; };
    std::vector<Segment> segments;                   // non-silent partition runs, rebuilt per convolution
    std::array<std::vector<float>, 2> inBuf, outBuf, fadeBuf;
    std::vector<float> work;
    std::array<std::vector<float>, 2> outWork;        // per output channel: accumulated spectrum, then inverse FFT
    std::unique_ptr<FilterSet> active, spare;
    mutable juce::SpinLock slotLock;
    std::unique_ptr<FilterSet> pending, retired;
    std::atomic<int> installedLength { 0 }, installedTag { -1 };
};
