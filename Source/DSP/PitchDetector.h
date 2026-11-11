#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

struct PitchResult { float frequency = 0.0f, clarity = 0.0f; };

namespace pitchdetail
{
    /** Magnitude of the Hann-windowed spectrum of x at frequency hz. */
    inline double windowedMagnitude (const std::vector<float>& x, double hz, double sampleRate)
    {
        const int n = (int) x.size();
        const double w = 2.0 * 3.14159265358979 * hz / sampleRate;
        const double c = std::cos (w), sn = std::sin (w);
        double re = 0.0, im = 0.0, cr = 1.0, ci = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * i / (n - 1));
            re += x[(size_t) i] * win * cr;
            im -= x[(size_t) i] * win * ci;
            const double nr = cr * c - ci * sn;
            ci = cr * sn + ci * c;
            cr = nr;
        }
        return std::sqrt (re * re + im * im);
    }

    inline PitchResult nsdfPitch (const float* x, int n, double sampleRate, int minLag, int maxLag)
    {
        std::vector<float> flat (x, x + n);

        // A decaying tail biases the NSDF towards shorter lags (reads sharp). Estimate the exponential
        // decay from the two halves of the window and flatten it before analysis.
        {
            double e1 = 0.0, e2 = 0.0;
            for (int i = 0; i < n / 2; ++i)  e1 += (double) x[i] * x[i];
            for (int i = n / 2; i < n; ++i)  e2 += (double) x[i] * x[i];
            if (e1 > 0.0 && e2 > 0.0)
            {
                const double ratio = std::clamp (std::sqrt (e2 / e1), 0.02, 1.0);      // amplitude ratio over n/2 samples
                const double perSample = std::pow (ratio, -1.0 / (n / 2.0));
                double g = 1.0;
                for (int i = 0; i < n; ++i) { flat[(size_t) i] = (float) (x[i] * g); g *= perSample; }
            }
        }
        x = flat.data();

        std::vector<float> nsdf ((size_t) maxLag + 2, 0.0f);
        for (int tau = 0; tau <= maxLag + 1; ++tau)
        {
            double acf = 0.0, m = 0.0;
            for (int i = 0; i < n - tau; ++i)
            {
                acf += (double) x[i] * x[i + tau];
                m   += (double) x[i] * x[i] + (double) x[i + tau] * x[i + tau];
            }
            nsdf[(size_t) tau] = m > 0.0 ? (float) (2.0 * acf / m) : 0.0f;
        }
    
        // Collect the maximum of each positive lobe after the first zero crossing
        std::vector<int> peaks;
        int tau = 1;
        while (tau <= maxLag && nsdf[(size_t) tau] > 0.0f) ++tau;
        while (tau <= maxLag)
        {
            while (tau <= maxLag && nsdf[(size_t) tau] <= 0.0f) ++tau;
            int best = -1;
            while (tau <= maxLag && nsdf[(size_t) tau] > 0.0f)
            {
                if (best < 0 || nsdf[(size_t) tau] > nsdf[(size_t) best]) best = tau;
                ++tau;
            }
            if (best > 0) peaks.push_back (best);
        }
        if (peaks.empty())
            return {};
    
        float highest = 0.0f;
        for (int p : peaks) highest = std::max (highest, nsdf[(size_t) p]);
    
        for (int p : peaks)
        {
            if (p < minLag || nsdf[(size_t) p] < 0.9f * highest)
                continue;
            const float a = nsdf[(size_t) p - 1], b = nsdf[(size_t) p], c = nsdf[(size_t) p + 1];
            const float denom = a - 2.0f * b + c;
            const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
            return { (float) (sampleRate / ((float) p + shift)), b };
        }
        return {};
    }
}

/** McLeod pitch method (normalised square difference function). Used by the tuner.
    Heavily distorted kicks carry strong harmonics and DC that pull the NSDF sharp (up to 20 cents on bit-crushed
    rawstyle kicks), so a first estimate is refined on a copy low-passed just above the fundamental. */
inline PitchResult detectPitch (const float* x, int n, double sampleRate, float minHz, float maxHz)
{
    const int minLag = std::max (2, (int) std::floor (sampleRate / maxHz));
    const int maxLag = std::min (n / 2, (int) std::ceil (sampleRate / minHz));
    if (maxLag <= minLag + 2)
        return {};

    const auto first = pitchdetail::nsdfPitch (x, n, sampleRate, minLag, maxLag);
    if (first.frequency <= 0.0f)
        return first;

    // Refine on the spectrum: the peak of the windowed fundamental is not pulled by the harmonics the way the NSDF
    // lags are. Search +-50 cents around the first estimate in 1-cent steps, then interpolate.
    std::vector<float> v (x, x + n);
    double mean = 0.0;
    for (float s : v) mean += s;
    mean /= n;
    for (auto& s : v) s -= (float) mean;

    constexpr int steps = 50;
    double mags[2 * steps + 1];
    int best = steps;
    for (int k = 0; k <= 2 * steps; ++k)
    {
        mags[k] = pitchdetail::windowedMagnitude (v, first.frequency * std::pow (2.0, (k - steps) / 1200.0), sampleRate);
        if (mags[k] > mags[best]) best = k;
    }
    if (best == 0 || best == 2 * steps)
        return first;   // no clear spectral peak near the estimate: keep the NSDF answer
    const double a = mags[best - 1], b = mags[best], c = mags[best + 1], denom = a - 2.0 * b + c;
    const double cents = (best - steps) + (std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0);
    return { (float) (first.frequency * std::pow (2.0, cents / 1200.0)), first.clarity };
}

inline const char* noteNameFor (int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midiNote % 12) + 12) % 12];
}

inline int octaveFor (int midiNote) { return (int) std::floor (midiNote / 12.0) - 1; }

inline float frequencyToMidi (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
inline float midiToFrequency (float note) { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
