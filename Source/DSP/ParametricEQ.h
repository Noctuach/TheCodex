#pragma once
#include <array>
#include <cmath>
#include <complex>
#include <algorithm>

namespace eq
{
    /** Filter shapes (the first six keep their original indices). */
    enum class BandType { LowCut = 0, LowShelf, Bell, Notch, HighShelf, HighCut, BandPass, TiltShelf, FlatTilt, AllPass };

    inline const char* const* bandTypeNames()
    {
        static const char* names[] = { "Low Cut", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut",
                                       "Band Pass", "Tilt Shelf", "Flat Tilt", "All Pass" };
        return names;
    }
    constexpr int numBandTypes = 10;

    /** Slopes in dB/oct, selected by index. */
    constexpr int slopeDb[] = { 6, 12, 18, 24, 36, 48, 72, 96 };
    constexpr int numSlopes = 8;
    constexpr int defaultSlope = 3; // 24 dB/oct

    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    /** Pro-Q-style per-band stereo placement. */
    enum class Placement { Stereo = 0, Left, Right, Mid, Side };

    struct BandSettings
    {
        bool on = false;
        Placement placement = Placement::Stereo;
        BandType type = BandType::Bell;
        float freq = 1000.0f, gainDb = 0.0f, q = 0.707f;
        int slope = defaultSlope; // index into slopeDb

        bool operator== (const BandSettings& o) const
        {
            return on == o.on && placement == o.placement && type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q && slope == o.slope;
        }
        bool operator!= (const BandSettings& o) const { return ! (*this == o); }
    };

    constexpr int maxStages = 8;

    struct BandDesign
    {
        std::array<Coeffs, maxStages> stages;
        int numStages = 0;
    };

    inline bool typeUsesGain (BandType t)
    {
        return t == BandType::Bell || t == BandType::LowShelf || t == BandType::HighShelf
            || t == BandType::TiltShelf || t == BandType::FlatTilt;
    }
    inline bool typeUsesSlope (BandType t) { return t == BandType::LowCut || t == BandType::HighCut || t == BandType::BandPass; }

    //==========================================================================
    // Building blocks (also used by the saturation crossovers and the distortion filters)
    constexpr double pi_ = 3.14159265358979323846;

    inline Coeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return Coeffs { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    inline double clampFreq (double f, double sr) { return std::clamp (f, 5.0, sr * 0.49); }

    inline Coeffs firstOrder (bool highpass, double fc, double sr)
    {
        const double K = std::tan (pi_ * clampFreq (fc, sr) / sr);
        const double a1 = (K - 1.0) / (K + 1.0);
        if (! highpass) return { K / (1.0 + K), K / (1.0 + K), 0.0, a1, 0.0 };
        return { 1.0 / (1.0 + K), -1.0 / (1.0 + K), 0.0, a1, 0.0 };
    }

    inline Coeffs secondOrder (bool highpass, double fc, double q, double sr)
    {
        const double w0 = 2.0 * pi_ * clampFreq (fc, sr) / sr, cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * std::max (0.05, q));
        if (highpass) return normalise ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
        return normalise ((1 - cw) / 2, (1 - cw), (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    inline Coeffs shelf (bool high, double fc, double gainDb, double q, double sr)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * pi_ * clampFreq (fc, sr) / sr, cw = std::cos (w0);
        const double sq = 2.0 * std::sqrt (A) * std::sin (w0) / (2.0 * std::max (0.05, q));
        if (! high)
            return normalise (A * ((A + 1) - (A - 1) * cw + sq), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sq),
                              (A + 1) + (A - 1) * cw + sq, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sq);
        return normalise (A * ((A + 1) + (A - 1) * cw + sq), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sq),
                          (A + 1) - (A - 1) * cw + sq, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sq);
    }

    /** Butterworth low/high-pass of any order (odd orders get a first-order section). order 2 uses userQ. */
    inline void addButterworth (BandDesign& d, bool highpass, int order, double fc, double userQ, double sr)
    {
        auto add = [&d] (Coeffs c) { if (d.numStages < maxStages) d.stages[(size_t) d.numStages++] = c; };
        if (order <= 1) { add (firstOrder (highpass, fc, sr)); return; }
        if (order == 2) { add (secondOrder (highpass, fc, userQ, sr)); return; }
        if (order % 2 == 1)
        {
            add (firstOrder (highpass, fc, sr));
            for (int k = 1; k <= (order - 1) / 2; ++k)
                add (secondOrder (highpass, fc, 1.0 / (2.0 * std::cos (k * pi_ / order)), sr));
        }
        else
            for (int k = 0; k < order / 2; ++k)
                add (secondOrder (highpass, fc, 1.0 / (2.0 * std::cos ((2 * k + 1) * pi_ / (2.0 * order))), sr));
    }

    /** All EQ shapes. Shared by the DSP and the GUI curve. */
    inline BandDesign design (const BandSettings& s, double sampleRate)
    {
        BandDesign d;
        if (! s.on)
            return d;

        const double sr   = sampleRate;
        const double f    = clampFreq ((double) s.freq, sr);
        const double w0   = 2.0 * pi_ * f / sr;
        const double cosw = std::cos (w0), sinw = std::sin (w0);
        const double A    = std::pow (10.0, s.gainDb / 40.0);
        const double Q    = std::max (0.05, (double) s.q);
        const int order   = slopeDb[std::clamp (s.slope, 0, numSlopes - 1)] / 6;
        auto add = [&d] (Coeffs c) { if (d.numStages < maxStages) d.stages[(size_t) d.numStages++] = c; };

        switch (s.type)
        {
            case BandType::LowCut:  addButterworth (d, true,  order, f, Q, sr); break;
            case BandType::HighCut: addButterworth (d, false, order, f, Q, sr); break;
            case BandType::Bell:
            {
                const double alpha = sinw / (2.0 * Q);
                add (normalise (1 + alpha * A, -2 * cosw, 1 - alpha * A, 1 + alpha / A, -2 * cosw, 1 - alpha / A));
                break;
            }
            case BandType::Notch:
            {
                const double alpha = sinw / (2.0 * Q);
                add (normalise (1, -2 * cosw, 1, 1 + alpha, -2 * cosw, 1 - alpha));
                break;
            }
            case BandType::LowShelf:  add (shelf (false, f, s.gainDb, Q, sr)); break;
            case BandType::HighShelf: add (shelf (true,  f, s.gainDb, Q, sr)); break;
            case BandType::BandPass:
            {
                if (order <= 2)
                {
                    // constant 0 dB peak band-pass
                    const double alpha = sinw / (2.0 * Q);
                    add (normalise (alpha, 0.0, -alpha, 1 + alpha, -2 * cosw, 1 - alpha));
                }
                else
                {
                    // high-pass and low-pass around the centre; Q sets the width (Q 1 = one octave)
                    const double bw = std::pow (2.0, 1.0 / (2.0 * Q));
                    const int side = std::min (8, (order + 1) / 2);
                    addButterworth (d, true,  side, f / bw, 0.70710678, sr);
                    addButterworth (d, false, side, f * bw, 0.70710678, sr);

                    // the two skirts overlap: normalise so the centre frequency passes at 0 dB
                    const std::complex<double> z1 = std::polar (1.0, -w0), z2 = z1 * z1;
                    double mag = 1.0;
                    for (int i = 0; i < d.numStages; ++i)
                    {
                        const auto& c = d.stages[(size_t) i];
                        mag *= std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
                    }
                    if (mag > 1.0e-6)
                    {
                        auto& c0 = d.stages[0];
                        c0.b0 /= mag; c0.b1 /= mag; c0.b2 /= mag;
                    }
                }
                break;
            }
            case BandType::TiltShelf:
                // tilts around the frequency: lows down and highs up (or the other way round)
                add (shelf (false, f, -0.5 * s.gainDb, Q * 0.7, sr));
                add (shelf (true,  f,  0.5 * s.gainDb, Q * 0.7, sr));
                break;
            case BandType::FlatTilt:
                // a straight tilt over the whole spectrum: three gentle tilt pairs spread over ~8 octaves
                for (double m : { 1.0 / 16.0, 1.0, 16.0 })
                {
                    add (shelf (false, f * m, -s.gainDb / 6.0, 0.5, sr));
                    add (shelf (true,  f * m,  s.gainDb / 6.0, 0.5, sr));
                }
                break;
            case BandType::AllPass:
            {
                const double alpha = sinw / (2.0 * Q);
                add (normalise (1 - alpha, -2 * cosw, 1 + alpha, 1 + alpha, -2 * cosw, 1 - alpha));
                break;
            }
        }
        return d;
    }

    /** Magnitude (linear) of a band design at a given frequency. */
    inline double magnitude (const BandDesign& d, double freq, double sampleRate)
    {
        const std::complex<double> z1 = std::polar (1.0, -2.0 * pi_ * freq / sampleRate);
        const std::complex<double> z2 = z1 * z1;
        double mag = 1.0;
        for (int i = 0; i < d.numStages; ++i)
        {
            const auto& c = d.stages[i];
            mag *= std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
        }
        return mag;
    }

    /** Transposed direct form II biquad (double precision for stable low-frequency cuts). */
    struct Biquad
    {
        Coeffs c;
        double s1 = 0, s2 = 0;

        inline float process (float xf) noexcept
        {
            const double x = xf;
            const double y = c.b0 * x + s1;
            s1 = c.b1 * x - c.a1 * y + s2;
            s2 = c.b2 * x - c.a2 * y;
            return (float) y;
        }
        void reset() { s1 = s2 = 0; }
    };

    template <int NumBands>
    class ParametricEQ
    {
    public:
        void prepare (double sr)
        {
            sampleRate = sr;
            for (int b = 0; b < NumBands; ++b)
                rebuild (b);
            reset();
        }

        void reset()
        {
            for (auto& band : filters)
                for (auto& ch : band)
                    for (auto& f : ch)
                        f.reset();
        }

        void setBand (int b, const BandSettings& s)
        {
            auto& cur = settings[(size_t) b];
            if (s == cur)
                return;

            const bool topologyChanged = s.on != cur.on || s.type != cur.type || s.slope != cur.slope || s.placement != cur.placement;
            cur = s;
            rebuild (b);
            if (topologyChanged)
                for (auto& ch : filters[(size_t) b])
                    for (auto& f : ch)
                        f.reset();
        }

        /** Stereo processing; each band works on Stereo (L and R), Left, Right, Mid or Side. */
        void process (float* left, float* right, int numSamples) noexcept
        {
            for (int b = 0; b < NumBands; ++b)
            {
                const int n = designs[(size_t) b].numStages;
                if (n == 0) continue;
                auto& f = filters[(size_t) b];

                switch (settings[(size_t) b].placement)
                {
                    case Placement::Stereo:
                        runStages (f[0], n, left, numSamples);
                        runStages (f[1], n, right, numSamples);
                        break;
                    case Placement::Left:  runStages (f[0], n, left, numSamples);  break;
                    case Placement::Right: runStages (f[1], n, right, numSamples); break;
                    case Placement::Mid:
                    case Placement::Side:
                    {
                        const bool mid = settings[(size_t) b].placement == Placement::Mid;
                        for (int i = 0; i < numSamples; ++i)
                        {
                            float m = 0.5f * (left[i] + right[i]);
                            float s = 0.5f * (left[i] - right[i]);
                            float& target = mid ? m : s;
                            for (int st = 0; st < n; ++st)
                                target = f[0][(size_t) st].process (target);
                            left[i] = m + s;
                            right[i] = m - s;
                        }
                        break;
                    }
                }
            }
        }

    private:
        static void runStages (std::array<Biquad, maxStages>& stages, int n, float* data, int numSamples) noexcept
        {
            for (int st = 0; st < n; ++st)
            {
                auto& f = stages[(size_t) st];
                for (int i = 0; i < numSamples; ++i)
                    data[i] = f.process (data[i]);
            }
        }

        void rebuild (int b)
        {
            designs[(size_t) b] = design (settings[(size_t) b], sampleRate);
            for (auto& ch : filters[(size_t) b])
                for (int st = 0; st < designs[(size_t) b].numStages; ++st)
                    ch[(size_t) st].c = designs[(size_t) b].stages[(size_t) st];
        }

        double sampleRate = 44100.0;
        std::array<BandSettings, NumBands> settings {};
        std::array<BandDesign, NumBands> designs {};
        std::array<std::array<std::array<Biquad, maxStages>, 2>, NumBands> filters {};
    };
}
