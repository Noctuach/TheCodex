#pragma once
#include <juce_core/juce_core.h>
#include "DrawEnvelope.h"
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>

/**
    Free-point distortion curve ("custom curve"): the transfer curve a user draws on the distortion display.

    The curve lives in a drawn domain u (Symmetric: 0..1, mirrored with odd symmetry; Asymmetric: -1..1) and maps to
    the shaper input v = u * reach, so a curve seeded from a mode at a high drive still covers the whole signal.
    Beyond the ends the curve is held flat.

    Each point carries a bend for the segment to its right (the same exponential warp as the drawable envelopes,
    DrawEnvelope::curve) and a smooth / sharp flag. Smooth points get a limited three-point tangent (Fritsch-Carlson), so a
    curve never overshoots its points and the extrema of a fold sit exactly on the points; a sharp point uses the
    chord on each side (a corner). A segment between two sharp points with bend 0 is a straight line.

    Pure model: no GUI and no threading. Edited on the message thread, compiled into a lookup table by
    CustomCurveBank (DSP) and evaluated directly by the editor.
*/
struct FreeCurve
{
    static constexpr int   maxPoints = 64;
    static constexpr float minDx = 0.002f;
    static constexpr float spacingTol = minDx * 0.99f;   // spacing check with room for the 1e-5 value grid
    static constexpr float minReach = 1.0f, maxReach = 64.0f;

    struct Point
    {
        float x = 0.0f, y = 0.0f, bend = 0.0f;   // bend shapes the segment to the RIGHT of the point
        bool sharp = false;
        bool operator== (const Point& o) const { return x == o.x && y == o.y && bend == o.bend && sharp == o.sharp; }
    };

    std::array<Point, maxPoints> p {};
    int   count = 2;
    bool  symmetric = true;
    float reach = 1.0f;

    float domainLo() const noexcept { return symmetric ? 0.0f : -1.0f; }

    static FreeCurve linear (bool sym = true, float reach = 1.0f)
    {
        FreeCurve c;
        c.symmetric = sym;
        c.reach = reach;
        c.count = 2;
        c.p[0] = { sym ? 0.0f : -1.0f, sym ? 0.0f : -1.0f, 0.0f, false };
        c.p[1] = { 1.0f, 1.0f, 0.0f, false };
        c.sanitise();
        return c;
    }

    /** Values are kept on a 1e-5 grid so the text form (state, presets, undo) round-trips bit for bit. */
    static float quantise (float v) noexcept
    {
        if (! std::isfinite (v)) return 0.0f;
        return (float) (std::round ((double) v * 100000.0) / 100000.0);
    }

    /** Makes the curve valid: finite values, y and bend in -1..1, sorted x, minimum spacing, endpoints pinned. */
    void sanitise()
    {
        reach = std::isfinite (reach) ? std::clamp (reach, minReach, maxReach) : 1.0f;
        reach = quantise (reach);
        count = std::clamp (count, 0, maxPoints);
        const float lo = domainLo();
        for (int i = 0; i < count; ++i)
        {
            auto& q = p[(size_t) i];
            q.x = std::isfinite (q.x) ? std::clamp (q.x, lo, 1.0f) : lo;
            q.y = std::isfinite (q.y) ? std::clamp (q.y, -1.0f, 1.0f) : 0.0f;
            q.bend = std::isfinite (q.bend) ? std::clamp (q.bend, -1.0f, 1.0f) : 0.0f;
        }
        if (count < 2)
        {
            const Point first = count == 1 ? p[0] : Point {};
            p[0] = { lo, symmetric ? 0.0f : -1.0f, 0.0f, false };
            p[1] = { 1.0f, count == 1 ? first.y : 1.0f, 0.0f, false };
            count = 2;
        }
        std::stable_sort (p.begin(), p.begin() + count, [] (const Point& a, const Point& b) { return a.x < b.x; });
        p[0].x = lo;
        p[(size_t) count - 1].x = 1.0f;
        // minimum spacing: drop the later colliding point (never an endpoint)
        std::array<Point, maxPoints> out {};
        int n = 0;
        out[(size_t) n++] = p[0];
        for (int i = 1; i < count - 1; ++i)
        {
            const auto& q = p[(size_t) i];
            if (q.x - out[(size_t) n - 1].x < spacingTol || 1.0f - q.x < spacingTol) continue;
            out[(size_t) n++] = q;
        }
        out[(size_t) n++] = p[(size_t) count - 1];
        p = out;
        count = n;
        if (symmetric) { p[0].x = 0.0f; p[0].y = 0.0f; }
        for (int i = 0; i < count; ++i)
        {
            auto& q = p[(size_t) i];
            q.x = quantise (q.x); q.y = quantise (q.y); q.bend = quantise (q.bend);
        }
        p[(size_t) count - 1].bend = 0.0f;   // no segment to the right of the last point
        for (int i = count; i < maxPoints; ++i) p[(size_t) i] = Point {};
    }

    static juce::String fmt (float v)
    {
        const long long n = (long long) std::llround ((double) v * 100000.0);
        const long long a = n < 0 ? -n : n;
        return (n < 0 ? "-" : "") + juce::String (a / 100000) + "." + juce::String (a % 100000).paddedLeft ('0', 5);
    }

    /** "1|S|<reach>|x,y,bend,k;..."  (S = symmetric, A = asymmetric; k = 1 sharp, 0 smooth). */
    juce::String toString() const
    {
        juce::String s ("1|");
        s << (symmetric ? "S" : "A") << "|" << fmt (reach) << "|";
        for (int i = 0; i < count; ++i)
        {
            const auto& q = p[(size_t) i];
            if (i > 0) s << ";";
            s << fmt (q.x) << "," << fmt (q.y) << "," << fmt (q.bend) << "," << (q.sharp ? "1" : "0");
        }
        return s;
    }

    /** Parses toString(); false on an unknown version or garbage. The result is always sanitised. */
    static bool fromString (const juce::String& text, FreeCurve& out)
    {
        out = linear();
        const auto parts = juce::StringArray::fromTokens (text, "|", "");
        if (parts.size() != 4 || parts[0] != "1" || (parts[1] != "S" && parts[1] != "A")) return false;
        FreeCurve c;
        c.symmetric = parts[1] == "S";
        c.reach = (float) parts[2].getDoubleValue();
        const auto pts = juce::StringArray::fromTokens (parts[3], ";", "");
        if (pts.size() < 2) return false;
        c.count = 0;
        for (const auto& t : pts)
        {
            if (c.count >= maxPoints) break;
            const auto v = juce::StringArray::fromTokens (t, ",", "");
            if (v.size() != 4) return false;
            auto& q = c.p[(size_t) c.count++];
            q.x = (float) v[0].getDoubleValue();
            q.y = (float) v[1].getDoubleValue();
            q.bend = (float) v[2].getDoubleValue();
            q.sharp = v[3].getIntValue() != 0;
        }
        c.sanitise();
        out = c;
        return true;
    }

    bool operator== (const FreeCurve& o) const
    {
        if (count != o.count || symmetric != o.symmetric || reach != o.reach) return false;
        for (int i = 0; i < count; ++i)
            if (! (p[(size_t) i] == o.p[(size_t) i])) return false;
        return true;
    }
    bool operator!= (const FreeCurve& o) const { return ! (*this == o); }

    bool isEndpoint (int i) const noexcept { return i == 0 || i == count - 1; }
    /** True for points that cannot be deleted (the endpoints, which includes the symmetric origin). */
    bool isLocked (int i) const noexcept { return isEndpoint (i); }

    // ---- editing helpers (each keeps the curve valid) ----

    /** Adds a point; returns its index, or -1 when the curve is full or the spot is too close to another point. */
    int insert (float x, float y, bool sharp = false)
    {
        if (count >= maxPoints || ! std::isfinite (x) || ! std::isfinite (y)) return -1;
        x = quantise (x);
        if (x <= domainLo() + minDx || x >= 1.0f - minDx) return -1;
        int at = 1;
        while (at < count && p[(size_t) at].x < x) ++at;
        if (x - p[(size_t) at - 1].x < minDx || p[(size_t) at].x - x < minDx) return -1;
        for (int i = count; i > at; --i) p[(size_t) i] = p[(size_t) i - 1];
        // the new point takes over the right part of the segment it splits (keeps the left segment's bend)
        p[(size_t) at] = { x, quantise (std::clamp (y, -1.0f, 1.0f)), p[(size_t) at - 1].bend, sharp };
        ++count;
        return at;
    }

    bool remove (int i)
    {
        if (i <= 0 || i >= count - 1) return false;
        for (int k = i; k < count - 1; ++k) p[(size_t) k] = p[(size_t) k + 1];
        p[(size_t) --count] = Point {};
        return true;
    }

    /** Moves a point: x stays between its neighbours (minimum spacing), endpoints move in y only, the symmetric origin is locked. */
    void move (int i, float x, float y)
    {
        if (i < 0 || i >= count) return;
        if (symmetric && i == 0) return;
        auto& q = p[(size_t) i];
        if (std::isfinite (y)) q.y = quantise (std::clamp (y, -1.0f, 1.0f));
        if (! isEndpoint (i) && std::isfinite (x))
        {
            const float lo = p[(size_t) i - 1].x + minDx, hi = p[(size_t) i + 1].x - minDx;
            q.x = quantise (std::clamp (x, lo, std::max (lo, hi)));   // within the spacing tolerance of sanitise()
        }
    }

    void setBend (int i, float b)    { if (i >= 0 && i < count - 1) p[(size_t) i].bend = quantise (std::clamp (b, -1.0f, 1.0f)); }
    void setSharp (int i, bool s)    { if (i >= 0 && i < count) p[(size_t) i].sharp = s; }

    /** Pencil: replaces the points inside the stroke's span by a simplified version of the stroke.
        uy: samples in (u, y); tol: simplification tolerance in y units. */
    void applyStroke (const std::vector<juce::Point<float>>& uy, float tol)
    {
        constexpr int cells = 256;
        const float lo = domainLo();
        const float width = 1.0f - lo;
        std::array<float, cells + 1> cellY {};
        std::array<bool, cells + 1> used {};
        for (auto s : uy)
        {
            float u = s.x, y = s.y;
            if (! std::isfinite (u) || ! std::isfinite (y)) continue;
            if (symmetric && u < 0.0f) { u = -u; y = -y; }
            u = std::clamp (u, lo, 1.0f);
            y = std::clamp (y, -1.0f, 1.0f);
            const int c = (int) std::lround ((u - lo) / width * (float) cells);
            cellY[(size_t) c] = y;   // the last sample in a cell wins
            used[(size_t) c] = true;
        }
        std::vector<juce::Point<float>> pts;
        for (int c = 0; c <= cells; ++c)
            if (used[(size_t) c]) pts.push_back ({ lo + width * (float) c / (float) cells, cellY[(size_t) c] });
        if (pts.size() < 2) return;
        const float ua = pts.front().x, ub = pts.back().x;

        // remove the existing points strictly inside the span (never the endpoints)
        for (int i = count - 2; i >= 1; --i)
            if (p[(size_t) i].x >= ua - 0.5f * minDx && p[(size_t) i].x <= ub + 0.5f * minDx) remove (i);

        // Ramer-Douglas-Peucker, loosened until the result fits
        std::vector<int> keep;
        for (float t = std::max (1.0e-4f, tol);; t *= 2.0f)
        {
            keep.clear();
            std::vector<bool> mark (pts.size(), false);
            mark.front() = mark.back() = true;
            std::vector<std::pair<int, int>> stack { { 0, (int) pts.size() - 1 } };
            while (! stack.empty())
            {
                auto [a, b] = stack.back();
                stack.pop_back();
                float worst = 0.0f; int at = -1;
                for (int k = a + 1; k < b; ++k)
                {
                    const float f = (pts[(size_t) k].x - pts[(size_t) a].x) / std::max (1.0e-9f, pts[(size_t) b].x - pts[(size_t) a].x);
                    const float d = std::abs (pts[(size_t) k].y - (pts[(size_t) a].y + f * (pts[(size_t) b].y - pts[(size_t) a].y)));
                    if (d > worst) { worst = d; at = k; }
                }
                if (at >= 0 && worst > t) { mark[(size_t) at] = true; stack.push_back ({ a, at }); stack.push_back ({ at, b }); }
            }
            for (size_t k = 0; k < pts.size(); ++k) if (mark[k]) keep.push_back ((int) k);
            if ((int) keep.size() + count <= maxPoints || t > 4.0f) break;
        }
        // the point left of the stroke now leads into drawn data: straighten its bend
        for (int i = count - 1; i >= 0; --i)
            if (p[(size_t) i].x < ua) { p[(size_t) i].bend = 0.0f; break; }
        const float cell = width / (float) cells;
        for (int k : keep)
        {
            const auto q = pts[(size_t) k];
            if (q.x <= lo + cell * 0.5f)        { if (! symmetric) p[0].y = quantise (q.y); continue; }
            if (q.x >= 1.0f - cell * 0.5f)      { p[(size_t) count - 1].y = quantise (q.y); continue; }
            const int at = insert (q.x, q.y, false);
            if (at >= 0) p[(size_t) at].bend = 0.0f;
        }
        sanitise();
    }
};

/** A FreeCurve compiled for evaluation (tangents precomputed). */
struct FreeCurveShape
{
    FreeCurve c;
    std::array<float, FreeCurve::maxPoints> mL {}, mR {};   // slope arriving at / leaving each point (dy/du)

    FreeCurveShape() { compile (FreeCurve::linear()); }
    explicit FreeCurveShape (const FreeCurve& curve) { compile (curve); }

    void compile (const FreeCurve& curve)
    {
        c = curve;
        const int n = c.count;
        auto chord = [this] (int i) { const auto& a = c.p[(size_t) i]; const auto& b = c.p[(size_t) i + 1];
                                      return (b.y - a.y) / std::max (1.0e-9f, b.x - a.x); };
        for (int i = 0; i < n; ++i)
        {
            const auto& q = c.p[(size_t) i];
            const bool hasL = i > 0, hasR = i < n - 1;
            float dl = hasL ? chord (i - 1) : 0.0f, dr = hasR ? chord (i) : 0.0f;
            if (! hasL && c.symmetric) { dl = dr; }   // symmetric origin: a phantom mirrored point
            if (! hasL && ! c.symmetric) dl = dr;      // curve endpoints: the adjacent chord
            if (! hasR) dr = dl;
            if (q.sharp)
            {
                mL[(size_t) i] = dl;
                mR[(size_t) i] = dr;
            }
            else
            {
                // weighted three-point slope, limited to 3x the smaller chord (Fritsch-Carlson): monotone data stays
                // monotone with no overshoot; a change of direction gets slope 0, so extrema sit exactly on the points
                float m = 0.0f;
                if (dl * dr > 0.0f)
                {
                    const float hl = hasL ? q.x - c.p[(size_t) i - 1].x : 1.0f, hr = hasR ? c.p[(size_t) i + 1].x - q.x : 1.0f;
                    m = (dl * hr + dr * hl) / std::max (1.0e-9f, hl + hr);
                    const float lim = 3.0f * std::min (std::abs (dl), std::abs (dr));
                    m = std::clamp (m, -lim, lim);
                }
                mL[(size_t) i] = mR[(size_t) i] = m;
            }
        }
        if (n >= 2)
        {
            // curve endpoints use the adjacent chord (not a mirrored or zero slope); the symmetric origin keeps its phantom slope
            if (! c.symmetric) mL[0] = mR[0] = chord (0);
            mL[(size_t) n - 1] = mR[(size_t) n - 1] = chord (n - 2);
        }
    }

    /** Value in the drawn domain; held flat beyond the ends. */
    float evalU (float u) const noexcept
    {
        if (c.symmetric && u < 0.0f) return -evalHalf (-u);
        return evalHalf (u);
    }
    float evalV (float v) const noexcept { return evalU (v / c.reach); }

    /** Segment index for u (x_i <= u < x_{i+1}), clamped to valid segments. */
    int segmentFor (float u) const noexcept
    {
        int lo = 0, hi = c.count - 1;
        while (hi - lo > 1)
        {
            const int mid = (lo + hi) / 2;
            if (c.p[(size_t) mid].x <= u) lo = mid; else hi = mid;
        }
        return lo;
    }

    float evalSegment (int i, float u) const noexcept
    {
        const auto& a = c.p[(size_t) i];
        const auto& b = c.p[(size_t) i + 1];
        const float dx = b.x - a.x;
        const float t = std::clamp ((u - a.x) / std::max (1.0e-9f, dx), 0.0f, 1.0f);
        const float s = DrawEnvelope::curve (t, a.bend);
        const float s2 = s * s, s3 = s2 * s;
        const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f, h10 = s3 - 2.0f * s2 + s, h01 = -2.0f * s3 + 3.0f * s2, h11 = s3 - s2;
        return h00 * a.y + h10 * dx * mR[(size_t) i] + h01 * b.y + h11 * dx * mL[(size_t) i + 1];
    }

private:
    float evalHalf (float u) const noexcept
    {
        const int n = c.count;
        if (u <= c.p[0].x) return c.p[0].y;
        if (u >= c.p[(size_t) n - 1].x) return c.p[(size_t) n - 1].y;
        return evalSegment (segmentFor (u), u);
    }
};
