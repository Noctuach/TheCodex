#pragma once
#include "ClipDistortion.h"
#include "CustomCurve.h"

/**
    Seeding a custom curve from a distortion mode (so drawing starts from the sound you hear), the starter shapes and
    the symmetry / flip helpers of the curve editor. Message thread only (a few ms per seed).
*/
namespace CurveSeed
{
    /** True when the mode's curve is flat beyond |v| = 1 (then a reach of 1 seeds it exactly). */
    inline bool flatBeyondOne (int mode, const ClipDistortion::Curve& legacy)
    {
        const float pos1 = ClipDistortion::shapeFor (mode, 1.0f, &legacy), neg1 = ClipDistortion::shapeFor (mode, -1.0f, &legacy);
        for (float v = 1.0f; v <= 16.0f; v += 1.0f / 64.0f)
            if (std::abs (ClipDistortion::shapeFor (mode, v, &legacy) - pos1) >= 1.0e-6f
                || std::abs (ClipDistortion::shapeFor (mode, -v, &legacy) - neg1) >= 1.0e-6f) return false;
        return true;
    }

    /** Where a saturating curve has settled: the smallest V (>= 1) beyond which the curve stays within 2e-4 of its
        value at the end of the range on both sides (0 when it never settles, e.g. the folds). */
    inline float settledFrom (int mode, const ClipDistortion::Curve& legacy, float range)
    {
        const float pos = ClipDistortion::shapeFor (mode, range, &legacy), neg = ClipDistortion::shapeFor (mode, -range, &legacy);
        float v = range;
        for (; v >= 1.0f; v -= 1.0f / 32.0f)
            if (std::abs (ClipDistortion::shapeFor (mode, v, &legacy) - pos) >= 2.0e-4f
                || std::abs (ClipDistortion::shapeFor (mode, -v, &legacy) - neg) >= 2.0e-4f) break;
        if (v >= range) return 0.0f;
        return std::max (1.0f, v + 1.0f / 32.0f);
    }

    struct Result { FreeCurve curve; float maxError = 0.0f; };
    inline Result fromMode (int mode, const ClipDistortion::Curve& legacy, float reach);

    /** Input range the seeded curve covers: the largest shaper input the current settings produce, with headroom. When
        that range is so wide that 64 points cannot follow the curve (a saturating Type at extreme drive), it stops where
        the Type's curve has settled instead: the rest is held flat, exactly as the Type does. */
    inline float reachFor (int mode, const ClipDistortion::Curve& legacy, float driveGain, float boostGain,
                           float tailDriveDb, float spatial, float offset)
    {
        if (flatBeyondOne (mode, legacy)) return 1.0f;
        if (mode == ClipDistortion::Downsample) driveGain = 1.0f;
        const float g = driveGain * boostGain * std::max (1.0f, juce::Decibels::decibelsToGain (tailDriveDb)) * (1.0f + 0.6f * spatial);
        const float r = std::clamp (1.25f * g + std::abs (offset) + 0.3f * spatial, FreeCurve::minReach, FreeCurve::maxReach);
        const float settled = settledFrom (mode, legacy, std::max (r, 16.0f));
        if (settled > 0.0f && settled < r && fromMode (mode, legacy, r).maxError > 0.005f)
            return std::clamp (settled, FreeCurve::minReach, FreeCurve::maxReach);
        return r;
    }

    namespace detail
    {
        struct Sampled
        {
            std::vector<float> u, y;
            std::vector<bool> corner, jumpZone;
        };

        inline float maxError (const FreeCurve& c, const Sampled& s)
        {
            const FreeCurveShape sh (c);
            float worst = 0.0f;
            for (size_t k = 0; k < s.u.size(); ++k)
                if (! s.jumpZone[k]) worst = std::max (worst, std::abs (sh.evalU (s.u[k]) - s.y[k]));
            return worst;
        }
    }

    /**
        Free-point approximation of a mode's shape over [-reach, reach]: corners become sharp points, extrema smooth
        points, then the worst-fitting sample is added until the error is below 0.002 (at most 64 points), and points
        that are not needed are pruned again.
    */
    inline Result fromMode (int mode, const ClipDistortion::Curve& legacy, float reach)
    {
        reach = std::clamp (reach, FreeCurve::minReach, FreeCurve::maxReach);
        constexpr int N = 4097;
        auto shape = [&] (float u) { return ClipDistortion::shapeFor (mode, u * reach, &legacy); };
        float asym = 0.0f;
        for (int k = 0; k < N; ++k)
        {
            const float u = -1.0f + 2.0f * (float) k / (float) (N - 1);
            asym = std::max (asym, std::abs (shape (u) + shape (-u)));
        }
        const bool sym = asym < 1.0e-5f;

        detail::Sampled s;
        const int first = sym ? (N - 1) / 2 : 0;
        for (int k = first; k < N; ++k)
        {
            const float u = -1.0f + 2.0f * (float) k / (float) (N - 1);
            s.u.push_back (u);
            s.y.push_back (std::clamp (shape (u), -1.0f, 1.0f));
        }
        const int M = (int) s.u.size();
        s.corner.assign ((size_t) M, false);
        s.jumpZone.assign ((size_t) M, false);
        const float du = s.u[1] - s.u[0];
        std::vector<float> slope ((size_t) M - 1);
        for (int k = 0; k < M - 1; ++k) slope[(size_t) k] = (s.y[(size_t) k + 1] - s.y[(size_t) k]) / du;

        FreeCurve c;
        c.symmetric = sym;
        c.reach = reach;
        c.count = 0;
        struct Cand { float x, y; bool sharp; float prominence; };
        std::vector<Cand> cands;
        cands.push_back ({ s.u.front(), s.y.front(), false, 1.0e9f });
        // jumps (a step between two samples) and corners
        std::vector<float> jumpAt;
        // slope change per sample; a corner is an isolated spike of it (on a smooth curve it varies slowly). A corner that
        // falls between two samples splits its spike over two neighbours, so the spike is measured over a pair.
        std::vector<float> D ((size_t) M, 0.0f);
        for (int k = 1; k < M - 1; ++k) D[(size_t) k] = std::abs (slope[(size_t) k] - slope[(size_t) k - 1]);
        auto Dat = [&] (int k) { return k >= 1 && k < M - 1 ? D[(size_t) k] : 0.0f; };
        for (int k = 1; k < M - 1; ++k)
        {
            const float a = slope[(size_t) k - 1], b = slope[(size_t) k];
            if (Dat (k) < Dat (k - 1) || Dat (k) <= Dat (k + 1) * 0.999f) continue;   // local maximum of the slope change only
            const float spike = Dat (k) + std::max (Dat (k - 1), Dat (k + 1));
            const float around = std::max ({ Dat (k - 2), Dat (k + 2), Dat (k - 3), Dat (k + 3) });
            const float sMax = std::max ({ std::abs (a), std::abs (b), std::abs (k >= 2 ? slope[(size_t) k - 2] : a), std::abs (k + 1 < M - 1 ? slope[(size_t) k + 1] : b) });
            const bool isCorner = spike > 0.3f * sMax + 0.5f && spike > 4.0f * around;
            if (! isCorner) continue;
            // a jump: one very steep sample step between two flatter neighbours
            const float stepY = s.y[(size_t) k + 1] - s.y[(size_t) k];
            const bool jump = k + 1 < M - 1 && std::abs (stepY) > 0.02f && std::abs (b) > 8.0f * std::max (std::abs (a), std::abs (slope[(size_t) k + 1])) + 1.0f;
            if (jump)
            {
                const float mid = 0.5f * (s.u[(size_t) k] + s.u[(size_t) k + 1]);
                const float h = 0.5f * FreeCurve::minDx;
                cands.push_back ({ mid - h, s.y[(size_t) k], true, 1.0e8f });
                cands.push_back ({ mid + h, s.y[(size_t) k + 1], true, 1.0e8f });
                jumpAt.push_back (mid);
                s.corner[(size_t) k] = s.corner[(size_t) k + 1] = true;
                ++k;
                continue;
            }
            const float prom = std::abs (b - a);
            cands.push_back ({ s.u[(size_t) k], s.y[(size_t) k], true, 1.0e7f + prom });
            s.corner[(size_t) k] = true;
        }
        // local extrema (smooth)
        for (int k = 1; k < M - 1; ++k)
        {
            if (s.corner[(size_t) k]) continue;
            const float a = slope[(size_t) k - 1], b = slope[(size_t) k];
            if (a * b < 0.0f)
            {
                const float prom = std::abs (s.y[(size_t) k] - 0.5f * (s.y[(size_t) std::max (0, k - 8)] + s.y[(size_t) std::min (M - 1, k + 8)]));
                cands.push_back ({ s.u[(size_t) k], s.y[(size_t) k], false, prom });
            }
        }
        cands.push_back ({ s.u.back(), s.y.back(), false, 1.0e9f });
        for (int k = 0; k < M; ++k)
            for (float j : jumpAt)
                if (std::abs (s.u[(size_t) k] - j) < 0.004f) s.jumpZone[(size_t) k] = true;

        // too many features (folds at extreme drive): keep the most prominent
        if ((int) cands.size() > FreeCurve::maxPoints)
        {
            std::vector<Cand> inner (cands.begin() + 1, cands.end() - 1);
            std::sort (inner.begin(), inner.end(), [] (const Cand& a, const Cand& b) { return a.prominence > b.prominence; });
            inner.resize ((size_t) FreeCurve::maxPoints - 2);
            Cand f = cands.front(), l = cands.back();
            cands.clear();
            cands.push_back (f);
            cands.insert (cands.end(), inner.begin(), inner.end());
            cands.push_back (l);
        }
        std::sort (cands.begin(), cands.end(), [] (const Cand& a, const Cand& b) { return a.x < b.x; });
        for (const auto& q : cands)
            if (c.count < FreeCurve::maxPoints) c.p[(size_t) c.count++] = { q.x, q.y, 0.0f, q.sharp };
        c.sanitise();

        // greedy refinement: add the worst sample as a smooth point (keeping the best fit seen: with few points left, an
        // added point can make the fit worse elsewhere)
        constexpr float target = 0.002f;
        std::vector<bool> blocked ((size_t) M, false);
        FreeCurve best = c;
        float bestErr = 1.0e9f;
        for (int iter = 0; iter < 4 * M; ++iter)
        {
            const FreeCurveShape sh (c);
            float worst = 0.0f, all = 0.0f; int at = -1;
            for (int k = 0; k < M; ++k)
            {
                if (s.jumpZone[(size_t) k]) continue;
                const float e = std::abs (sh.evalU (s.u[(size_t) k]) - s.y[(size_t) k]);
                all = std::max (all, e);
                if (! blocked[(size_t) k] && e > worst) { worst = e; at = k; }
            }
            if (all < bestErr) { bestErr = all; best = c; }
            if (at < 0 || all < target || c.count >= FreeCurve::maxPoints) break;
            if (c.insert (s.u[(size_t) at], s.y[(size_t) at], false) < 0)
                for (int k = std::max (0, at - 4); k <= std::min (M - 1, at + 4); ++k) blocked[(size_t) k] = true;   // too close to a point
        }
        c = best;

        // reverse pruning: drop the smooth points the fit does not need
        const float limit = std::max (target, bestErr * 1.0001f);
        for (int i = c.count - 2; i >= 1; --i)
        {
            if (c.p[(size_t) i].sharp) continue;
            FreeCurve t = c;
            t.remove (i);
            if (detail::maxError (t, s) < limit) c = t;
        }
        return { c, detail::maxError (c, s) };
    }

    enum Starter { SoftKnee = 0, HardWall, WarmTube, DoubleFold, Staircase, SCurve, InvertedKnee, HalfWaveCrush, numStarters };

    inline juce::StringArray starterNames()
    {
        return { "Soft Knee", "Hard Wall", "Warm Tube", "Double Fold", "Staircase", "S-Curve", "Inverted Knee", "Half-Wave Crush" };
    }

    namespace detail
    {
        inline FreeCurve make (bool sym, float reach, std::initializer_list<FreeCurve::Point> pts)
        {
            FreeCurve c;
            c.symmetric = sym;
            c.reach = reach;
            c.count = 0;
            for (const auto& q : pts) c.p[(size_t) c.count++] = q;
            c.sanitise();
            return c;
        }
    }

    /** The starter shapes of the curve menu (original designs). */
    inline FreeCurve starter (int which, float reach)
    {
        using P = FreeCurve::Point;
        switch (which)
        {
            case SoftKnee:     return detail::make (true, reach, { P { 0, 0 }, P { 0.3f, 0.36f }, P { 0.62f, 0.78f }, P { 1, 0.94f } });
            case HardWall:     return detail::make (true, reach, { P { 0, 0, 0, true }, P { 0.55f, 0.9f, 0, true }, P { 1, 0.97f } });
            case WarmTube:     return detail::make (false, reach, { P { -1, -0.62f }, P { -0.45f, -0.5f }, P { -0.15f, -0.2f }, P { 0, 0 },
                                                                    P { 0.3f, 0.36f }, P { 0.62f, 0.78f }, P { 1, 0.94f } });
            case DoubleFold:   return detail::make (true, reach, { P { 0, 0 }, P { 0.28f, 0.92f }, P { 0.52f, -0.15f }, P { 0.78f, 0.7f }, P { 1, 0.35f } });
            case Staircase:
            {
                FreeCurve c;
                c.symmetric = true; c.reach = reach; c.count = 0;
                c.p[(size_t) c.count++] = { 0, 0, 0, true };
                for (int k = 1; k <= 4; ++k)
                {
                    const float x = 0.2f * (float) k;
                    c.p[(size_t) c.count++] = { x - 0.5f * FreeCurve::minDx, 0.25f * (float) (k - 1), 0, true };
                    c.p[(size_t) c.count++] = { x + 0.5f * FreeCurve::minDx, 0.25f * (float) k, 0, true };
                }
                c.p[(size_t) c.count++] = { 1, 1, 0, true };
                c.sanitise();
                return c;
            }
            case SCurve:       return detail::make (true, reach, { P { 0, 0 }, P { 0.5f, 0.2f, -0.4f }, P { 1, 1 } });
            case InvertedKnee: return detail::make (true, reach, { P { 0, 0 }, P { 0.3f, -0.36f }, P { 0.62f, -0.78f }, P { 1, -0.94f } });
            case HalfWaveCrush:return detail::make (false, reach, { P { -1, -0.15f }, P { -0.2f, -0.1f }, P { 0, 0, 0, true },
                                                                    P { 0.55f, 0.9f, 0, true }, P { 1, 0.97f } });
            default:           return FreeCurve::linear (true, reach);
        }
    }

    /** Symmetric -> Asymmetric expands the mirror (sounds identical); Asymmetric -> Symmetric keeps x >= 0 and locks (0, 0). */
    inline FreeCurve toggledSymmetry (const FreeCurve& in)
    {
        FreeCurve c;
        c.reach = in.reach;
        c.count = 0;
        if (in.symmetric)
        {
            c.symmetric = false;
            // mirrored half, left to right: point i of the positive half becomes (-x, -y); the segment between the mirrors of
            // i + 1 and i takes the bend of segment i, negated (1 - curve (1 - t, b) = curve (t, -b))
            for (int i = in.count - 1; i >= 1; --i)
            {
                const auto& q = in.p[(size_t) i];
                c.p[(size_t) c.count++] = { -q.x, -q.y, -in.p[(size_t) i - 1].bend, q.sharp };
            }
            for (int i = 0; i < in.count && c.count < FreeCurve::maxPoints; ++i) c.p[(size_t) c.count++] = in.p[(size_t) i];
            if (in.count * 2 - 1 > FreeCurve::maxPoints)
            {
                // does not fit: keep the positive half exact and a thinned mirror
                c.count = 0;
                const int room = FreeCurve::maxPoints - in.count;
                for (int k = 0; k < room; ++k)
                {
                    const int i = in.count - 1 - (int) std::lround ((double) k * (in.count - 2) / std::max (1, room - 1));
                    const auto& q = in.p[(size_t) juce::jlimit (1, in.count - 1, i)];
                    c.p[(size_t) c.count++] = { -q.x, -q.y, 0.0f, q.sharp };
                }
                for (int i = 0; i < in.count; ++i) c.p[(size_t) c.count++] = in.p[(size_t) i];
            }
        }
        else
        {
            c.symmetric = true;
            const FreeCurveShape sh (in);
            c.p[(size_t) c.count++] = { 0.0f, 0.0f, 0.0f, false };
            for (int i = 0; i < in.count; ++i)
            {
                const auto& q = in.p[(size_t) i];
                if (q.x == 0.0f) { c.p[0].bend = q.bend; c.p[0].sharp = q.sharp; }
                else if (q.x > 0.0f) c.p[(size_t) c.count++] = q;
            }
            juce::ignoreUnused (sh);
        }
        c.sanitise();
        return c;
    }

    inline FreeCurve flipped (const FreeCurve& in)
    {
        FreeCurve c = in;
        for (int i = 0; i < c.count; ++i) c.p[(size_t) i].y = -c.p[(size_t) i].y;
        c.sanitise();
        return c;
    }

    /** Asymmetric: the negative half becomes the odd mirror of the positive half (the curve passes through 0 at u = 0). */
    inline FreeCurve mirroredPosToNeg (const FreeCurve& in)
    {
        if (in.symmetric) return in;
        return toggledSymmetry (toggledSymmetry (in));
    }

    /** Every corner smooth / sharp. */
    inline FreeCurve withAllSharp (const FreeCurve& in, bool sharp)
    {
        FreeCurve c = in;
        for (int i = 0; i < c.count; ++i) c.p[(size_t) i].sharp = sharp;
        return c;
    }
}
