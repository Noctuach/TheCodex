#pragma once
#include <array>
#include <cmath>
#include <algorithm>

/**
    Breakpoint envelope ("draw" mode), as found in dedicated kick synthesizers:
    up to 8 points (time in ms, level 0..1), each segment with its own bend (-1..1, 0 = straight line).
    The first point is always at 0 ms. After the last point the envelope holds the last level.
*/
class DrawEnvelope
{
public:
    static constexpr int maxPoints = 8;

    struct Point { float timeMs = 0.0f, level = 0.0f, bend = 0.0f; };
    struct Shape
    {
        std::array<Point, maxPoints> points {};
        int count = 2;
        bool operator== (const Shape& o) const
        {
            if (count != o.count) return false;
            for (int i = 0; i < count; ++i)
                if (points[(size_t) i].timeMs != o.points[(size_t) i].timeMs || points[(size_t) i].level != o.points[(size_t) i].level
                    || points[(size_t) i].bend != o.points[(size_t) i].bend) return false;
            return true;
        }
    };

    /** Segment shape: bend > 0 drops fast then flattens (exponential, the classic kick pitch drop); bend < 0 the opposite. */
    static float curve (float u, float bend) noexcept
    {
        const float k = bend * 9.0f;
        if (std::abs (k) < 1.0e-3f) return u;
        return (1.0f - std::exp (-k * u)) / (1.0f - std::exp (-k));
    }

    /** Level at a time (ms) for a shape; used by the DSP and the editor so both always agree. */
    static float valueAt (const Shape& s, float tMs) noexcept
    {
        const int n = std::clamp (s.count, 1, maxPoints);
        if (tMs <= 0.0f) return s.points[0].level;
        for (int i = 0; i < n - 1; ++i)
        {
            const auto& a = s.points[(size_t) i];
            const auto& b = s.points[(size_t) i + 1];
            if (tMs < b.timeMs)
            {
                const float len = std::max (0.001f, b.timeMs - a.timeMs);
                const float u = std::clamp ((tMs - a.timeMs) / len, 0.0f, 1.0f);
                return a.level + (b.level - a.level) * curve (u, a.bend);
            }
        }
        return s.points[(size_t) n - 1].level;
    }

    /** Makes the shape valid: first point at 0 ms, times non-decreasing, levels 0..1. */
    static Shape sanitise (Shape s)
    {
        s.count = std::clamp (s.count, 2, maxPoints);
        s.points[0].timeMs = 0.0f;
        for (int i = 0; i < s.count; ++i)
        {
            auto& p = s.points[(size_t) i];
            p.level = std::clamp (p.level, 0.0f, 1.0f);
            p.bend = std::clamp (p.bend, -1.0f, 1.0f);
            if (i > 0) p.timeMs = std::max (p.timeMs, s.points[(size_t) i - 1].timeMs);
        }
        return s;
    }

    void setSampleRate (double sr) { msPerSample = (float) (1000.0 / sr); }
    void setShape (const Shape& s) { shape = sanitise (s); }

    void noteOn() noexcept { tMs = 0.0f; running = true; }
    void reset() noexcept { tMs = 0.0f; running = false; }

    /** True once the envelope has passed its last point. */
    bool isFinished() const noexcept { return tMs >= shape.points[(size_t) shape.count - 1].timeMs; }
    float lastLevel() const noexcept { return shape.points[(size_t) shape.count - 1].level; }

    float next() noexcept
    {
        if (! running) return 0.0f;
        const float v = valueAt (shape, tMs);
        if (! isFinished()) tMs += msPerSample;
        return v;
    }

private:
    Shape shape;
    float msPerSample = 1000.0f / 44100.0f, tMs = 0.0f;
    bool running = false;
};
