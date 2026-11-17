#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <algorithm>

/**
    Antiderivative anti-aliasing (first order, Parker et al. 2016): instead of f(v[n]) the stage outputs
    (F(v[n]) - F(v[n-1])) / (v[n] - v[n-1]), where F is the integral of the curve. This removes most of the
    aliasing that clipping corners create, on top of oversampling. F is tabulated once per curve
    (cubic Hermite between nodes), extended linearly beyond the table for saturating curves and wrapped
    for the periodic folds.

    RangeTimes4: the table covers +-RangeTimes4 / 4 (a template parameter so the range stays a compile-time constant:
    ClipDistortion::AdaaTable = BasicAdaaTable<64> is the +-16 table of the fixed modes and the X-Shaper, unchanged;
    BasicAdaaTable<4> covers +-1 for the custom curves, which are evaluated in their own drawn domain).
*/
template <int RangeTimes4>
struct BasicAdaaTable
{
    static constexpr double range = RangeTimes4 / 4.0;
    static constexpr int size = 8192;
    std::vector<double> F, f;
    bool periodic = false;
    double period = 4.0, h = 0.0;

    /** Allocates once; rebuilding later (e.g. while an X-Shaper point is dragged) does not allocate. */
    template <typename Fn>
    void build (Fn&& fn, bool isPeriodic)
    {
        periodic = isPeriodic;
        const double lo = periodic ? -0.5 * period : -range, hi = periodic ? 0.5 * period : range;
        h = (hi - lo) / (size - 1);
        if ((int) F.size() != size) { F.resize (size); f.resize (size); }
        std::fill (F.begin(), F.end(), 0.0);
        for (int i = 0; i < size; ++i) f[(size_t) i] = fn ((float) (lo + h * i));
        // trapezoid integration from the centre so F(0) = 0
        const int mid = (size - 1) / 2;
        for (int i = mid + 1; i < size; ++i) F[(size_t) i] = F[(size_t) i - 1] + 0.5 * h * (f[(size_t) i - 1] + f[(size_t) i]);
        for (int i = mid - 1; i >= 0; --i)    F[(size_t) i] = F[(size_t) i + 1] - 0.5 * h * (f[(size_t) i + 1] + f[(size_t) i]);
        const double offsetAtZero = F[(size_t) mid] + (0.0 - (lo + h * mid)) * f[(size_t) mid];
        for (auto& v : F) v -= offsetAtZero;
    }

    double eval (double v) const noexcept
    {
        const double lo = periodic ? -0.5 * period : -range, hi = periodic ? 0.5 * period : range;
        if (periodic)
        {
            v = std::fmod (v - lo, period);
            if (v < 0.0) v += period;
            v += lo;
        }
        else if (v >= hi) return F.back() + f.back() * (v - hi);
        else if (v <= lo) return F.front() + f.front() * (v - lo);
        const double t = (v - lo) / h;
        const int i = juce::jlimit (0, size - 2, (int) t);
        const double u = t - i;
        // cubic Hermite with the curve itself as the derivative
        const double u2 = u * u, u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * F[(size_t) i] + (u3 - 2 * u2 + u) * h * f[(size_t) i]
             + (-2 * u3 + 3 * u2) * F[(size_t) i + 1] + (u3 - u2) * h * f[(size_t) i + 1];
    }

    /** The curve itself: linear interpolation of the tabulated values, held at the ends (non-periodic tables). */
    float shapeAt (double x) const noexcept
    {
        const double t = (x + range) / h;
        if (t <= 0.0) return (float) f.front();
        if (t >= (double) (size - 1)) return (float) f.back();
        const int i = (int) t;
        const double u = t - i;
        return (float) (f[(size_t) i] + u * (f[(size_t) i + 1] - f[(size_t) i]));
    }
};
