#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

/** McLeod pitch method (normalised square difference function). Used by the tuner. */
struct PitchResult { float frequency = 0.0f, clarity = 0.0f; };

inline PitchResult detectPitch (const float* x, int n, double sampleRate, float minHz, float maxHz)
{
    const int minLag = std::max (2, (int) std::floor (sampleRate / maxHz));
    const int maxLag = std::min (n / 2, (int) std::ceil (sampleRate / minHz));
    if (maxLag <= minLag + 2)
        return {};

    // A decaying tail biases the NSDF towards shorter lags (reads sharp). Estimate the exponential
    // decay from the two halves of the window and flatten it before analysis.
    std::vector<float> flat (x, x + n);
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

inline const char* noteNameFor (int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midiNote % 12) + 12) % 12];
}

inline int octaveFor (int midiNote) { return (int) std::floor (midiNote / 12.0) - 1; }

inline float frequencyToMidi (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
inline float midiToFrequency (float note) { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
