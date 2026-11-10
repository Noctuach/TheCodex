#pragma once
#include <array>
#include <cmath>
#include <complex>
#include <algorithm>

namespace eq
{
    enum class BandType { LowCut = 0, LowShelf, Bell, Notch, HighShelf, HighCut };

    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    struct BandSettings
    {
        bool on = false;
        BandType type = BandType::Bell;
        float freq = 1000.0f, gainDb = 0.0f, q = 0.707f;
        int slope = 1; // 0 = 12, 1 = 24, 2 = 48 dB/oct

        bool operator== (const BandSettings& o) const
        {
            return on == o.on && type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q && slope == o.slope;
        }
        bool operator!= (const BandSettings& o) const { return ! (*this == o); }
    };

    constexpr int maxStages = 4;

    struct BandDesign
    {
        std::array<Coeffs, maxStages> stages;
        int numStages = 0;
    };

    inline int stagesForSlope (int slope) { return slope <= 0 ? 1 : (slope == 1 ? 2 : 4); }

    /** RBJ-cookbook biquads; cut filters are Butterworth cascades. Shared by DSP and GUI. */
    inline BandDesign design (const BandSettings& s, double sampleRate)
    {
        BandDesign d;
        if (! s.on)
            return d;

        constexpr double pi = 3.14159265358979323846;
        const double f    = std::clamp ((double) s.freq, 10.0, sampleRate * 0.49);
        const double w0   = 2.0 * pi * f / sampleRate;
        const double cosw = std::cos (w0), sinw = std::sin (w0);
        const double A    = std::pow (10.0, s.gainDb / 40.0);
        const double Q    = std::max (0.05, (double) s.q);

        auto normalise = [] (double b0, double b1, double b2, double a0, double a1, double a2)
        {
            return Coeffs { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        };

        switch (s.type)
        {
            case BandType::LowCut:
            case BandType::HighCut:
            {
                const int k = stagesForSlope (s.slope);
                const bool hp = s.type == BandType::LowCut;
                for (int i = 0; i < k; ++i)
                {
                    // First stage at 12 dB/oct follows the user Q (resonance); higher orders are Butterworth.
                    const double q = k == 1 ? Q : 1.0 / (2.0 * std::cos (pi * (2.0 * i + 1.0) / (4.0 * k)));
                    const double alpha = sinw / (2.0 * q);
                    if (hp) d.stages[i] = normalise ((1 + cosw) / 2, -(1 + cosw), (1 + cosw) / 2, 1 + alpha, -2 * cosw, 1 - alpha);
                    else    d.stages[i] = normalise ((1 - cosw) / 2,  (1 - cosw), (1 - cosw) / 2, 1 + alpha, -2 * cosw, 1 - alpha);
                }
                d.numStages = k;
                break;
            }
            case BandType::Bell:
            {
                const double alpha = sinw / (2.0 * Q);
                d.stages[0] = normalise (1 + alpha * A, -2 * cosw, 1 - alpha * A, 1 + alpha / A, -2 * cosw, 1 - alpha / A);
                d.numStages = 1;
                break;
            }
            case BandType::Notch:
            {
                const double alpha = sinw / (2.0 * Q);
                d.stages[0] = normalise (1, -2 * cosw, 1, 1 + alpha, -2 * cosw, 1 - alpha);
                d.numStages = 1;
                break;
            }
            case BandType::LowShelf:
            {
                const double sq = 2.0 * std::sqrt (A) * sinw / (2.0 * Q);
                d.stages[0] = normalise (A * ((A + 1) - (A - 1) * cosw + sq),
                                         2 * A * ((A - 1) - (A + 1) * cosw),
                                         A * ((A + 1) - (A - 1) * cosw - sq),
                                         (A + 1) + (A - 1) * cosw + sq,
                                         -2 * ((A - 1) + (A + 1) * cosw),
                                         (A + 1) + (A - 1) * cosw - sq);
                d.numStages = 1;
                break;
            }
            case BandType::HighShelf:
            {
                const double sq = 2.0 * std::sqrt (A) * sinw / (2.0 * Q);
                d.stages[0] = normalise (A * ((A + 1) + (A - 1) * cosw + sq),
                                         -2 * A * ((A - 1) + (A + 1) * cosw),
                                         A * ((A + 1) + (A - 1) * cosw - sq),
                                         (A + 1) - (A - 1) * cosw + sq,
                                         2 * ((A - 1) - (A + 1) * cosw),
                                         (A + 1) - (A - 1) * cosw - sq);
                d.numStages = 1;
                break;
            }
        }
        return d;
    }

    /** Magnitude (linear) of a band design at a given frequency. */
    inline double magnitude (const BandDesign& d, double freq, double sampleRate)
    {
        constexpr double pi = 3.14159265358979323846;
        const std::complex<double> z1 = std::polar (1.0, -2.0 * pi * freq / sampleRate);
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
                for (auto& f : band)
                    f.reset();
        }

        void setBand (int b, const BandSettings& s)
        {
            if (s == settings[b])
                return;

            const bool topologyChanged = s.on != settings[b].on || s.type != settings[b].type || s.slope != settings[b].slope;
            settings[b] = s;
            rebuild (b);
            if (topologyChanged)
                for (auto& f : filters[b])
                    f.reset();
        }

        void process (float* data, int numSamples) noexcept
        {
            for (int b = 0; b < NumBands; ++b)
            {
                const int n = designs[b].numStages;
                for (int st = 0; st < n; ++st)
                {
                    auto& f = filters[b][st];
                    for (int i = 0; i < numSamples; ++i)
                        data[i] = f.process (data[i]);
                }
            }
        }

    private:
        void rebuild (int b)
        {
            designs[b] = design (settings[b], sampleRate);
            for (int st = 0; st < designs[b].numStages; ++st)
                filters[b][st].c = designs[b].stages[st];
        }

        double sampleRate = 44100.0;
        std::array<BandSettings, NumBands> settings {};
        std::array<BandDesign, NumBands> designs {};
        std::array<std::array<Biquad, maxStages>, NumBands> filters {};
    };
}
