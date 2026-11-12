#pragma once
#include "Widgets.h"

/**
    Easter egg: click the book in the header and a dancer joins you. Every kick moves her to the next pose with a
    bounce; between kicks she sways. Click the book again and she leaves.

    An original anime-style character, drawn procedurally: platinum hair dyed sun-orange at the tips, a dark
    oversized bomber over a white tee, an orange pleated skirt, knee socks and white sneakers, cel shaded and lit
    from behind by an orange stage light. Hair, side locks and skirt run on small springs, so they follow through
    and swing a moment behind the body.
*/
class Dancer : public juce::Component, private juce::Timer
{
public:
    explicit Dancer (std::atomic<unsigned int>& hits) : hitCounter (hits)
    {
        lastHits = hitCounter.load();
        fromVals = rest().v;
        setInterceptsMouseClicks (false, false);   // never in the way of the controls under it
    }

    void toggle()
    {
        if (isVisible() && targetAlpha > 0.5f) { targetAlpha = 0.0f; return; }
        targetAlpha = 1.0f;
        alpha = std::max (alpha, 0.0f);
        lastHits = hitCounter.load();
        primed = false;
        const Vals pz = currentPose();
        resetHair (makeRig (pz), pz[pFling]);
        setVisible (true);
        toFront (false);
        startTimerHz (60);
    }

    bool isDancing() const { return isVisible() && targetAlpha > 0.5f; }

    void paint (juce::Graphics& g) override
    {
        if (alpha < 0.004f) return;
        const auto b = getLocalBounds().toFloat();
        const float k = std::min (b.getWidth() / 170.0f, b.getHeight() / 230.0f);
        if (! (k > 0.0f)) return;
        // everything below is drawn on a 170 x 230 design grid
        g.addTransform (juce::AffineTransform::scale (k).translated ((b.getWidth() - 170.0f * k) * 0.5f,
                                                                     (b.getHeight() - 230.0f * k) * 0.5f));
        drawScene (g);
    }

private:
    //==============================================================================================================
    // pose parameters: angles in radians (0 = straight down, + = towards screen right), distances in design px
    enum Param : size_t { pLean, pTilt, pArmL, pForeL, pArmR, pForeR, pThighL, pShinL, pThighR, pShinR,
                          pAir, pShift, pTurn, pFlare, pFling, pBrow, numParams };
    enum Eyes  : int { eOpen, eWinkL, eWinkR, eHappy, eSparkle, eFierce };
    enum Mouth : int { mSmile, mGrin, mO, mCat, mSmirk };
    enum Hand  : int { hRelax, hFist, hOpen, hPeace, hPoint, hHeart };

    using Vals = std::array<float, numParams>;
    struct Move { Vals v; int eyes, mouth, handL, handR; };

    static const Move& rest()
    {
        //                        lean  tilt  armL  foreL armR  foreR thL   shL   thR   shR   air  shift turn flare fling brow
        static const Move m { { { 0.0f, 0.08f, -0.22f, 0.15f, 0.22f, -0.15f, 0.03f, 0.0f, -0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.3f } },
                              eOpen, mSmile, hRelax, hRelax };
        return m;
    }

    static const std::array<Move, 8>& moves()
    {
        static const std::array<Move, 8> m { {
            // hand heart at the chest, knees in, wink
            { { {  0.06f,  0.22f, -0.25f,  2.76f,  0.25f, -2.76f,  0.10f, -0.25f, -0.10f,  0.25f,  0.0f, -3.0f,  0.0f,  0.15f,  0.0f,  0.6f } }, eWinkR,   mCat,   hHeart, hHeart },
            // point at the sky, hand on hip, toe tap out
            { { { -0.12f,  0.20f, -0.75f,  1.45f,  2.55f,  0.15f,  0.05f,  0.00f,  0.55f, -0.25f,  0.0f, -5.0f,  0.2f,  0.3f,  0.2f,  0.8f } }, eSparkle, mGrin,  hFist,  hPoint },
            // body wave, both arms sweep out to the left
            { { { -0.18f, -0.20f, -1.75f,  0.35f, -0.90f, -0.80f, -0.45f,  0.10f,  0.12f,  0.15f,  0.0f,  6.0f, -0.3f,  0.35f,  0.5f,  0.2f } }, eOpen,    mSmile, hOpen,  hOpen  },
            // spin: three-quarter turn, skirt flared, hair flung out
            { { {  0.05f, -0.15f, -0.50f,  1.90f,  2.30f,  0.60f,  0.18f, -0.05f, -0.12f,  0.12f,  2.0f,  0.0f,  0.85f, 1.0f, -1.0f,  0.5f } }, eHappy,   mGrin,  hOpen,  hOpen  },
            // lunge with a peace sign, wink
            { { { -0.10f, -0.10f, -2.00f,  0.00f,  0.90f, -2.40f, -0.50f,  0.30f,  0.30f,  0.00f,  0.0f,  4.0f, -0.35f, 0.3f,  0.4f, -0.6f } }, eWinkL,   mSmirk, hPeace, hFist  },
            // jump, arms in a V
            { { {  0.00f,  0.10f, -2.60f, -0.20f,  2.60f,  0.20f, -0.08f, -0.75f,  0.08f,  0.75f, 14.0f,  0.0f,  0.0f,  0.6f,  0.0f,  1.0f } }, eSparkle, mGrin,  hOpen,  hOpen  },
            // sweep up to the right
            { { {  0.20f,  0.25f,  0.90f,  1.10f,  2.20f, -0.30f, -0.10f, -0.15f,  0.50f, -0.10f,  0.0f, -6.0f,  0.35f, 0.4f, -0.5f,  0.3f } }, eOpen,    mO,     hOpen,  hOpen  },
            // hardstyle stomp: wide squat, fist pump
            { { {  0.05f, -0.10f, -1.90f, -1.10f,  0.50f,  0.30f, -0.60f,  0.75f,  0.60f, -0.75f,  0.0f,  0.0f,  0.0f,  0.5f,  0.3f, -1.0f } }, eFierce,  mGrin,  hFist,  hFist  },
        } };
        return m;
    }

    //==============================================================================================================
    static juce::Point<float> dir (float a) noexcept { return { std::sin (a), std::cos (a) }; }

    static float easeOutBack (float t) noexcept
    {
        const float u = juce::jlimit (0.0f, 1.0f, t) - 1.0f;
        constexpr float c1 = 1.5f, c3 = c1 + 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }

    float easedBlend() const noexcept { return easeOutBack (blend); }

    // the pose between kicks: last pose -> target pose, drifting to the rest pose when the kicks stop
    Vals baseVals() const
    {
        const auto& b = moves()[(size_t) to].v;
        const float t = easedBlend();
        Vals v {};
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = fromVals[i] + (b[i] - fromVals[i]) * t;
        const auto& r = rest().v;
        for (size_t i = 0; i < v.size(); ++i)
            v[i] += (r[i] - v[i]) * idle * 0.9f;
        return v;
    }

    Vals currentPose() const
    {
        Vals v = baseVals();
        // idle sway: hips side to side, head and arms countering, a soft knee bob
        const float sw = std::sin (clockPhase (5.0f)) * idle;
        const float bob = (0.5f + 0.5f * std::sin (clockPhase (10.0f))) * idle;
        v[pShift] += 4.0f * sw;
        v[pLean]  -= 0.06f * sw;
        v[pTilt]  += 0.14f * sw;
        v[pArmL]  += 0.06f * sw;
        v[pArmR]  += 0.06f * sw;
        // the hit: knees give and spring back
        const float give = bounce + 0.12f * bob;
        v[pThighL] -= 0.22f * give; v[pShinL] += 0.44f * give;
        v[pThighR] += 0.22f * give; v[pShinR] -= 0.44f * give;
        v[pBrow] += 0.5f * bounce;
        return v;
    }

    struct Face { int eyes, mouth, handL, handR; };

    Face currentFace() const
    {
        if (idle > 0.6f) return { rest().eyes, rest().mouth, rest().handL, rest().handR };
        if (easedBlend() < 0.3f) return fromFace;
        const auto& m = moves()[(size_t) to];
        return { m.eyes, m.mouth, m.handL, m.handR };
    }

    //==============================================================================================================
    struct Rig
    {
        juce::Point<float> hip, up, side, neck, head;
        std::array<juce::Point<float>, 2> sh, el, ha, hj, kn, an;
        std::array<float, 2> foreAngle {}, shinAngle {};
        float ws = 1.0f, headAng = 0.0f;
        juce::AffineTransform headXf;
        juce::Point<float> at (float x, float y) const { return hip + side * (x * ws) + up * y; }
    };

    static constexpr float headScale = 1.28f;   // big anime head, readable at actual size

    static Rig makeRig (const Vals& p)
    {
        Rig q;
        q.up   = { std::sin (p[pLean]), -std::cos (p[pLean]) };
        q.side = { std::cos (p[pLean]),  std::sin (p[pLean]) };
        q.ws = 1.0f - 0.3f * std::min (1.0f, std::abs (p[pTurn]));

        // feet planted: the lower foot stands on the floor, the hips sit wherever the legs put them
        const std::array<float, 2> thigh { p[pThighL], p[pThighR] }, shin { p[pShinL], p[pShinR] };
        float drop = 0.0f;
        for (size_t i = 0; i < 2; ++i)
            drop = std::max (drop, 2.0f + 40.0f * std::cos (thigh[i]) + 39.0f * std::cos (thigh[i] + shin[i]));
        q.hip = { 85.0f + p[pShift], 208.0f - drop - p[pAir] };

        const std::array<float, 2> arm { p[pArmL], p[pArmR] }, fore { p[pForeL], p[pForeR] };
        for (size_t i = 0; i < 2; ++i)
        {
            const float sgn = i == 0 ? -1.0f : 1.0f;
            q.hj[i] = q.hip + juce::Point<float> (sgn * 6.0f * q.ws, 2.0f);
            q.kn[i] = q.hj[i] + dir (thigh[i]) * 40.0f;
            q.shinAngle[i] = thigh[i] + shin[i];
            q.an[i] = q.kn[i] + dir (q.shinAngle[i]) * 39.0f;

            q.sh[i] = q.at (sgn * 12.0f, 45.0f);
            q.el[i] = q.sh[i] + dir (arm[i]) * 26.0f;
            q.foreAngle[i] = arm[i] + fore[i];
            q.ha[i] = q.el[i] + dir (q.foreAngle[i]) * 23.0f;
        }
        q.neck = q.at (0.0f, 49.0f);
        q.headAng = p[pLean] + p[pTilt];
        const float neckAng = p[pLean] + 0.5f * p[pTilt];
        q.head = q.neck + juce::Point<float> (std::sin (neckAng), -std::cos (neckAng)) * (19.0f * headScale + 2.0f);
        q.headXf = juce::AffineTransform::scale (headScale).rotated (q.headAng).translated (q.head);   // big anime head
        return q;
    }

    //==============================================================================================================
    // geometry helpers. Every filled piece is wound the same way, so overlapping pieces in one path merge cleanly.
    static void addPoly (juce::Path& path, const juce::Point<float>* pts, int n, float round)
    {
        if (n < 3) return;
        float area = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % n];
            area += a.x * b.y - b.x * a.y;
        }
        juce::Path poly;
        for (int j = 0; j < n; ++j)
        {
            const auto& pt = pts[area >= 0.0f ? j : n - 1 - j];
            if (j == 0) poly.startNewSubPath (pt); else poly.lineTo (pt);
        }
        poly.closeSubPath();
        path.addPath (round > 0.0f ? poly.createPathWithRoundedCorners (round) : poly);
    }

    // a tapered tube along a polyline, with rounded ends
    static void addRibbon (juce::Path& path, const juce::Point<float>* c, const float* w, int n, float round)
    {
        constexpr int maxN = 12;
        n = juce::jlimit (2, maxN, n);
        std::array<juce::Point<float>, 2 * maxN + 2> poly;
        size_t m = 0;
        auto tangentAt = [c, n] (int i)
        {
            const auto t = c[std::min (i + 1, n - 1)] - c[std::max (i - 1, 0)];
            const float len = t.getDistanceFromOrigin();
            return len > 1.0e-4f ? t / len : juce::Point<float> (0.0f, 1.0f);
        };
        poly[m++] = c[0] - tangentAt (0) * (w[0] * 0.8f);
        for (int i = 0; i < n; ++i)
        {
            const auto t = tangentAt (i);
            poly[m++] = c[i] + juce::Point<float> (-t.y, t.x) * w[i];
        }
        poly[m++] = c[n - 1] + tangentAt (n - 1) * (w[n - 1] * 0.8f);
        for (int i = n - 1; i >= 0; --i)
        {
            const auto t = tangentAt (i);
            poly[m++] = c[i] - juce::Point<float> (-t.y, t.x) * w[i];
        }
        addPoly (path, poly.data(), (int) m, round);
    }

    static void addCircle (juce::Path& path, juce::Point<float> c, float r)
    {
        std::array<juce::Point<float>, 12> pts;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) pts.size();
            pts[i] = c + juce::Point<float> (std::sin (a), std::cos (a)) * r;
        }
        addPoly (path, pts.data(), (int) pts.size(), r * 0.5f);
    }

    // a strand of hair: quadratic curve from root, tapering to a point
    static void addStrand (juce::Path& path, juce::Point<float> p0, juce::Point<float> p1, juce::Point<float> p2, float w0)
    {
        constexpr int n = 8;
        std::array<juce::Point<float>, n> c;
        std::array<float, n> w;
        for (size_t j = 0; j < (size_t) n; ++j)
        {
            const float t = (float) j / (float) (n - 1), u = 1.0f - t;
            c[j] = p0 * (u * u) + p1 * (2.0f * u * t) + p2 * (t * t);
            w[j] = w0 * std::pow (u, 0.75f) + 0.25f;
        }
        addRibbon (path, c.data(), w.data(), n, 0.0f);
    }

    // a hand at the wrist w, the forearm pointing along d. 'side' says which way the thumb sits.
    // Skin shapes go into 'path' (filled and cel shaded with the arms), finger creases into 'lines'.
    static void addHand (juce::Path& path, juce::Path& lines, juce::Point<float> w, juce::Point<float> d, int shape, float side)
    {
        using P = juce::Point<float>;
        const P n (-d.y * side, d.x * side);   // towards the thumb
        auto finger = [&path] (P base, P tip, float w0, float w1)
        {
            const std::array<P, 2> f { base, tip };
            const std::array<float, 2> fw { w0, w1 };
            addRibbon (path, f.data(), fw.data(), 2, 0.4f);
        };
        auto box = [&path, d, n] (P c, float along, float across)   // a rounded block aligned with the forearm
        {
            const std::array<P, 4> pts { c - d * along - n * across, c + d * along - n * across,
                                         c + d * along + n * across, c - d * along + n * across };
            addPoly (path, pts.data(), 4, std::min (along, across) * 0.8f);
        };
        switch (shape)
        {
            case hFist:
            case hPoint:
            {
                const P c = w + d * 2.8f;
                box (c, 3.2f, 3.7f);
                lines.startNewSubPath (c + d * 1.3f - n * 2.4f);           // knuckle line
                lines.quadraticTo (c + d * 1.9f, c + d * 1.3f + n * 2.4f);
                lines.startNewSubPath (c - d * 1.2f + n * 3.0f);            // thumb across the front
                lines.lineTo (c + d * 0.6f + n * 0.6f);
                if (shape == hPoint)
                    finger (c + d * 1.5f + n * 1.4f, c + d * 8.5f + n * 1.6f, 1.25f, 1.0f);
                break;
            }
            case hPeace:
            {
                const P c = w + d * 2.4f;
                box (c, 2.8f, 3.2f);
                finger (c + d * 1.2f + n * 1.5f, c + d * 8.2f + n * 3.6f, 1.15f, 0.95f);
                finger (c + d * 1.2f - n * 0.6f, c + d * 8.2f - n * 2.0f, 1.15f, 0.95f);
                lines.startNewSubPath (c + d * 1.6f - n * 1.8f);            // the curled fingers
                lines.lineTo (c + d * 2.0f - n * 3.0f);
                break;
            }
            case hOpen:
            {
                const P c = w + d * 2.6f;
                box (c, 3.0f, 3.6f);
                for (int k = 0; k < 3; ++k)                                 // fingers fanned out
                {
                    const float u = (float) k - 1.0f;
                    const P base = c + d * 1.6f - n * (u * 2.1f + 0.4f);
                    finger (base, base + d * 4.6f - n * (u * 1.6f), 1.3f, 1.1f);
                }
                finger (c + n * 2.2f, c + n * 5.6f + d * 2.2f, 1.35f, 1.15f); // thumb
                break;
            }
            case hHeart:
                addCircle (path, w + d * 1.6f, 3.0f);
                break;
            default:
                addCircle (path, w + d * 1.4f, 3.5f);
                break;
        }
    }

    static void addStar (juce::Path& path, juce::Point<float> c, float r, float rot)
    {
        std::array<juce::Point<float>, 8> pts;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const float a = rot + juce::MathConstants<float>::pi * 0.25f * (float) i;
            pts[i] = c + juce::Point<float> (std::sin (a), std::cos (a)) * (i % 2 == 0 ? r : r * 0.28f);
        }
        addPoly (path, pts.data(), (int) pts.size(), 0.0f);
    }

    juce::Colour A (juce::Colour c) const { return c.withMultipliedAlpha (alpha); }

    // the stage backdrop and the stage light, at full opacity, rendered at 'px' device pixels per design unit
    // (each image is cropped to where its gradient is not fully transparent)
    static constexpr float glowRadius = 76.0f;
    static juce::Rectangle<float> stageBoxRect() { return juce::Rectangle<float> (176.0f, 176.0f).withCentre ({ 85.0f, 125.0f }); }

    void renderBackdrops (float px) const
    {
        using namespace juce;
        cachedScale = px;
        const auto box = stageBoxRect();
        stageImage = Image (Image::ARGB, roundToInt (box.getWidth() * px) + 1, roundToInt (box.getHeight() * px) + 1, true);
        {
            Graphics gi (stageImage);
            gi.addTransform (AffineTransform::translation (-box.getX(), -box.getY()).scaled (px));
            gi.setGradientFill (ColourGradient (Colours_::background.withAlpha (0.92f), box.getCentreX(), box.getCentreY(),
                                                Colours_::background.withAlpha (0.0f), box.getRight(), box.getCentreY(), true));
            gi.fillEllipse (box);
        }
        const int gs = roundToInt (2.0f * glowRadius * px) + 1;
        glowImage = Image (Image::ARGB, gs, gs, true);
        {
            Graphics gi (glowImage);
            gi.addTransform (AffineTransform::scale (px));
            gi.setGradientFill (ColourGradient (Colours_::accent, glowRadius, glowRadius,
                                                Colours_::accent.withAlpha (0.0f), 2.0f * glowRadius, glowRadius, true));
            gi.fillEllipse (0.0f, 0.0f, 2.0f * glowRadius, 2.0f * glowRadius);
        }
    }

    // cel shading, lit from behind: a rim of light along the upper edges, a shadow along the lower left, ink outline.
    // The rim is the shape itself; the shade is the shape shifted left-down and right-down (so the rim shows along the
    // top and both sides), the base is the shape shifted right-up (so the shade shows along the lower left).
    // 'silhouette' inks only the outer contour: the ink goes underneath at twice the width and the fill covers the
    // inner half, so the seams where the pieces of one path overlap (fingers on a palm) stay clean.
    void cel (juce::Graphics& g, const juce::Path& p, const juce::FillType& base, const juce::FillType& shade,
              juce::Colour rimColour, float rim, float sh, float line, bool silhouette = false) const
    {
        using juce::AffineTransform;
        // mitered: every piece is a closed outline, and round joins cost an arc per vertex at 60 frames a second
        const juce::PathStrokeType ink (silhouette ? 2.0f * line : line, juce::PathStrokeType::mitered);
        g.setColour (A (inkColour()));
        if (silhouette)
            g.strokePath (p, ink);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (p);
            g.setColour (rimColour);
            g.fillAll();
            g.reduceClipRegion (p, AffineTransform::translation (-rim, rim * 0.8f));
            g.setFillType (shade);
            g.fillPath (p, AffineTransform::translation (rim, rim * 0.8f));
            // the base also stays clear of the left rim: it is shifted right by sh, which is always more than rim
            g.setFillType (base);
            g.fillPath (p, AffineTransform::translation (sh, -sh));
        }
        if (! silhouette)
        {
            g.setColour (A (inkColour()));
            g.strokePath (p, ink);
        }
    }

    void cel (juce::Graphics& g, const juce::Path& p, juce::Colour base, juce::Colour shade, juce::Colour rimColour,
              float rim = 1.3f, float sh = 2.0f, float line = 0.9f, bool silhouette = false) const
    {
        jassert (sh > rim);
        cel (g, p, juce::FillType (A (base)), juce::FillType (A (shade)), A (rimColour), rim, sh, line, silhouette);
    }

    static juce::Colour inkColour() { return juce::Colour (0xff1e0f0a); }

    //==============================================================================================================
    void drawScene (juce::Graphics& g) const
    {
        using namespace juce;
        const Vals p = currentPose();
        const Rig q = makeRig (p);
        const Face face = currentFace();
        const float b = bounce;

        // The two big soft gradients are pre-rendered once per pixel scale and blitted: a radial gradient under a
        // scaled (HiDPI) transform costs a matrix and a square root per pixel, which was a quarter of the frame.
        const float px = juce::jlimit (0.25f, 8.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        if (stageImage.isNull() || std::abs (px - cachedScale) > 1.0e-3f)
            renderBackdrops (px);
        auto blit = [&g, px] (const Image& img, Point<float> topLeft, float opacity)
        {
            Graphics::ScopedSaveState s (g);
            g.setImageResamplingQuality (Graphics::lowResamplingQuality);   // whole device pixels: a plain copy
            g.setOpacity (juce::jlimit (0.0f, 1.0f, opacity));
            g.drawImageTransformed (img, AffineTransform::scale (1.0f / px).translated (topLeft));
        };

        // a dark stage behind the figure, so the controls under it don't show through
        blit (stageImage, { stageBox.getX(), stageBox.getY() }, alpha);

        // the stage light behind her, pulsing with the kick
        const Point<float> glowC (q.hip.x * 0.5f + 42.5f, q.neck.y + 8.0f);
        blit (glowImage, glowC - Point<float> (glowRadius, glowRadius), alpha * (0.38f + 0.4f * b));

        // floor: a hot spot and the contact shadow
        const float floorX = 85.0f + (q.hip.x - 85.0f) * 0.6f;
        g.setGradientFill (ColourGradient (A (Colours_::accent.withAlpha (0.45f + 0.35f * b)), floorX, 214.0f,
                                           A (Colours_::accent.withAlpha (0.0f)), floorX + 46.0f + 10.0f * b, 214.0f, true));
        g.fillEllipse (Rectangle<float> (92.0f + 20.0f * b, 13.0f).withCentre ({ floorX, 214.0f }));
        g.setColour (A (Colours_::background.withAlpha (0.75f)));
        g.fillEllipse (Rectangle<float> (44.0f - p[pAir], 5.0f).withCentre ({ floorX, 214.5f }));
        if (b > 0.02f)
        {
            // a thin shock ring runs out across the floor on every kick
            const float grow = 1.0f - b;
            g.setColour (A (Colours_::accentHi.withAlpha (0.75f * b)));
            g.drawEllipse (Rectangle<float> (56.0f + 92.0f * grow, 8.0f + 15.0f * grow).withCentre ({ floorX, 214.5f }), 1.3f);
        }

        {
            Graphics::ScopedSaveState squash (g);
            g.addTransform (AffineTransform::scale (1.0f + 0.035f * b, 1.0f - 0.05f * b, q.hip.x, 214.0f));
            drawFigure (g, p, q, face);
        }

        // light flecks thrown off by the kicks
        Path stars;
        for (const auto& f : flecks)
            if (f.life > 0.0f)
                addStar (stars, { f.x, f.y }, f.size * (0.4f + 0.6f * f.life) * (1.0f + 0.25f * std::sin (f.life * 9.0f)), f.rot);
        if (! stars.isEmpty())
        {
            g.setColour (A (Colours_::accent.withAlpha (0.18f)));
            g.strokePath (stars, PathStrokeType (1.6f));
            g.setColour (A (Colours_::accentHi.interpolatedWith (Colours::white, 0.3f).withAlpha (0.75f)));
            g.fillPath (stars);
        }
    }

    void drawFigure (juce::Graphics& g, const Vals& p, const Rig& q, const Face& face) const
    {
        using namespace juce;
        const float b = bounce;
        const Colour skin (0xffffe6d6), skinSh (0xfff0b49c), hair (0xfff7ece2), hairSh (0xffd7b4a2),
                     jacket (0xff2a2622), jacketSh (0xff181512), sleeve (0xff3a332d), sleeveSh (0xff221e1a), tee (0xfff8f1ea), teeSh (0xffd9c9bb),
                     skirtSh (0xffc4560a), sock (0xff221e27), sockSh (0xff141217), shoe (0xfffcf8f3), shoeSh (0xffd8cbbf);
        const Colour rim = Colours_::accent, rimHi = Colours_::accentHi;
        const float fling = p[pFling];

        //--- long back hair: crown mass plus seven strands swinging on the spring --------------------------------
        {
            Path back;
            std::array<Point<float>, 16> crown;
            for (size_t i = 0; i < crown.size(); ++i)
            {
                const float a = MathConstants<float>::twoPi * (float) i / (float) crown.size();
                crown[i] = Point<float> (std::sin (a) * 21.0f, -2.5f - std::cos (a) * 21.5f).transformedBy (q.headXf);
            }
            addPoly (back, crown.data(), (int) crown.size(), 3.0f);
            for (size_t i = 0; i < numStrands; ++i)
            {
                // the strand hangs from where the head is now, and its mid point and tip trail on the verlet chain
                const auto root = strandRest (q, fling, i)[0];
                const auto mid = root + (hairNow[i][0] - hairRoot[i]);
                const auto tip = root + (hairNow[i][1] - hairRoot[i]);
                addStrand (back, root, mid * 2.0f - (root + tip) * 0.5f, tip, 6.5f);
            }
            // the glow behind the hair, so the arms read in front of it
            g.setColour (A (Colours_::accent.withAlpha (0.2f + 0.18f * b)));
            g.strokePath (back, PathStrokeType (3.6f));
            ColourGradient base (A (hair), 0.0f, q.head.y - 10.0f, A (Colours_::accent), 0.0f, q.head.y + 82.0f, false);
            base.addColour (0.5, A (Colours_::accent2));
            ColourGradient shade (A (hairSh), 0.0f, q.head.y - 10.0f, A (Colour (0xffc0560c)), 0.0f, q.head.y + 82.0f, false);
            cel (g, back, FillType (base), FillType (shade), A (rimHi), 1.4f, 2.6f, 0.9f);
        }

        //--- legs, socks, sneakers -------------------------------------------------------------------------------
        {
            Path legs, socks, shoes, soles;
            for (size_t i = 0; i < 2; ++i)
            {
                const std::array<Point<float>, 3> c { q.hj[i], q.kn[i], q.an[i] };
                const std::array<float, 3> w { 7.0f, 4.5f, 3.1f };
                addRibbon (legs, c.data(), w.data(), 3, 2.0f);
                const std::array<Point<float>, 2> s { q.kn[i] + (q.an[i] - q.kn[i]) * 0.2f, q.an[i] };
                const std::array<float, 2> sw { 4.7f, 3.4f };
                addRibbon (socks, s.data(), sw.data(), 2, 1.5f);

                const float sgn = i == 0 ? -1.0f : 1.0f;
                const auto foot = q.an[i] + Point<float> (sgn * 2.8f, 3.2f);
                const auto xf = AffineTransform::rotation (sgn * 0.1f + q.shinAngle[i] * 0.25f).translated (foot);
                Path s1; s1.addEllipse (-7.2f, -4.0f, 14.4f, 8.0f); s1.applyTransform (xf); shoes.addPath (s1);
                Path s2; s2.addEllipse (-7.5f, -2.8f, 15.0f, 7.4f); s2.applyTransform (xf); soles.addPath (s2);
            }
            cel (g, legs, skin, skinSh, rimHi);
            cel (g, socks, sock, sockSh, rim, 1.1f);
            g.setColour (A (Colours_::accent));
            g.fillPath (soles);
            g.setColour (A (inkColour()));
            g.strokePath (soles, PathStrokeType (0.8f));
            cel (g, shoes, shoe, shoeSh, rimHi, 1.0f, 1.8f, 0.8f);
        }

        //--- pleated skirt --------------------------------------------------------------------------------------
        {
            const float hw = 19.0f + 11.0f * p[pFlare] + 1.5f * b;
            const Point<float> hemC = q.hip + Point<float> (skirtX * 0.6f, 20.0f - 5.0f * p[pFlare]);
            std::array<Point<float>, 11> pts;
            pts[0] = q.at (-11.5f, 10.0f);
            for (size_t j = 0; j <= 8; ++j)
            {
                const float u = -1.0f + 0.25f * (float) j;
                pts[j + 1] = hemC + Point<float> (u * hw + skirtX * 0.5f * (1.0f - u * u),
                                                 (j % 2 == 1 ? 1.6f : 0.0f) - 7.0f * p[pFlare] * u * u);
            }
            pts[10] = q.at (11.5f, 10.0f);
            Path skirt;
            addPoly (skirt, pts.data(), (int) pts.size(), 1.0f);
            cel (g, skirt, Colours_::accent, skirtSh, rimHi);
            Path folds;
            for (size_t j = 2; j <= 8; j += 2)
            {
                const float u = -1.0f + 0.25f * (float) (j - 1);
                folds.startNewSubPath (q.at (u * 9.0f, 9.0f));
                folds.lineTo (pts[j]);
            }
            g.setColour (A (skirtSh));
            g.strokePath (folds, PathStrokeType (0.9f));
            Path band;
            band.startNewSubPath (q.at (-11.5f, 10.0f));
            band.lineTo (q.at (11.5f, 10.0f));
            g.setColour (A (Colour (0xff8a3a08)));
            g.strokePath (band, PathStrokeType (2.0f));
        }

        //--- neck, tee, bomber jacket ---------------------------------------------------------------------------
        Path head;
        {
            const float fx = p[pTurn] * 2.5f;
            head.startNewSubPath (fx, 19.0f);
            head.cubicTo (fx - 6.0f, 18.5f, -13.0f, 12.0f, -15.2f, 4.0f);
            head.cubicTo (-17.2f, -4.0f, -15.5f, -18.5f, 0.0f, -19.0f);
            head.cubicTo (15.5f, -18.5f, 17.2f, -4.0f, 15.2f, 4.0f);
            head.cubicTo (13.0f, 12.0f, fx + 6.0f, 18.5f, fx, 19.0f);
            head.closeSubPath();
            head.applyTransform (q.headXf);
        }
        {
            Path neck;
            const std::array<Point<float>, 2> c { q.at (0.0f, 44.0f), q.neck + (q.head - q.neck) * 0.5f };
            const std::array<float, 2> w { 3.8f, 3.4f };
            addRibbon (neck, c.data(), w.data(), 2, 1.0f);
            cel (g, neck, skinSh, Colour (0xffd99a80), rimHi);
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (neck);
            g.setColour (A (Colour (0xffd99a80)));
            g.fillPath (head, AffineTransform::translation (0.0f, 3.0f));
        }
        {
            Path teePath;
            const std::array<Point<float>, 9> t { q.at (-5.0f, 49.0f), q.at (-12.0f, 45.0f), q.at (-12.5f, 28.0f), q.at (-10.5f, 8.0f),
                                                  q.at (10.5f, 8.0f), q.at (12.5f, 28.0f), q.at (12.0f, 45.0f), q.at (5.0f, 49.0f),
                                                  q.at (0.0f, 44.5f) };
            addPoly (teePath, t.data(), (int) t.size(), 2.0f);
            cel (g, teePath, tee, teeSh, rimHi);
            // a little sun on the tee
            g.setColour (A (Colours_::accent));
            g.fillEllipse (Rectangle<float> (4.6f, 4.6f).withCentre (q.at (0.0f, 31.0f)));

            Path jk;
            for (int side = 0; side < 2; ++side)
            {
                const float s = side == 0 ? -1.0f : 1.0f;
                const std::array<Point<float>, 8> j { q.at (s * 4.5f, 50.5f), q.at (s * 16.0f, 46.5f), q.at (s * 17.5f, 30.0f),
                                                      q.at (s * 16.0f, 13.0f), q.at (s * 14.5f, 9.5f), q.at (s * 5.5f, 11.0f),
                                                      q.at (s * 6.5f, 30.0f), q.at (s * 5.0f, 44.0f) };
                addPoly (jk, j.data(), (int) j.size(), 2.0f);
            }
            g.setColour (A (Colours_::accent.withAlpha (0.25f + 0.2f * b)));
            g.strokePath (jk, PathStrokeType (3.5f));
            cel (g, jk, jacket, jacketSh, rim, 1.4f);
            Path ribs;
            for (int side = 0; side < 2; ++side)
            {
                const float s = side == 0 ? -1.0f : 1.0f;
                ribs.startNewSubPath (q.at (s * 15.0f, 11.0f)); ribs.lineTo (q.at (s * 6.0f, 12.2f));       // hem
                ribs.startNewSubPath (q.at (s * 4.8f, 49.5f));  ribs.lineTo (q.at (s * 5.6f, 44.5f));       // collar
            }
            g.setColour (A (Colours_::accent));
            g.strokePath (ribs, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        //--- arms -----------------------------------------------------------------------------------------------
        {
            Path skinArms, sleeves, cuffs, handLines;
            for (size_t i = 0; i < 2; ++i)
            {
                const auto fore = q.ha[i] - q.el[i];
                const std::array<Point<float>, 2> fa { q.el[i] + fore * 0.4f, q.ha[i] };
                const std::array<float, 2> fw { 2.7f, 2.3f };
                addRibbon (skinArms, fa.data(), fw.data(), 2, 1.0f);
                addHand (skinArms, handLines, q.ha[i], dir (q.foreAngle[i]), i == 0 ? face.handL : face.handR,
                         i == 0 ? 1.0f : -1.0f);
                const std::array<Point<float>, 3> sl { q.sh[i], q.el[i], q.el[i] + fore * 0.52f };
                const std::array<float, 3> sw { 6.0f, 5.0f, 5.4f };
                addRibbon (sleeves, sl.data(), sw.data(), 3, 2.0f);
                const std::array<Point<float>, 2> cf { q.el[i] + fore * 0.46f, q.el[i] + fore * 0.66f };
                const std::array<float, 2> cw { 5.7f, 5.0f };
                addRibbon (cuffs, cf.data(), cw.data(), 2, 1.0f);
            }
            cel (g, skinArms, skin, skinSh, rimHi, 1.0f, 1.8f, 0.8f, true);   // one clean contour round each hand
            g.setColour (A (inkColour().withAlpha (0.85f)));
            g.strokePath (handLines, PathStrokeType (0.65f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (A (Colours_::accent.withAlpha (0.2f + 0.2f * b)));
            g.strokePath (sleeves, PathStrokeType (3.0f));
            cel (g, sleeves, sleeve, sleeveSh, rim, 1.4f);
            cel (g, cuffs, Colours_::accent, skirtSh, rimHi, 1.0f, 1.5f, 0.8f);
        }

        //--- head -----------------------------------------------------------------------------------------------
        Path bangs;
        {
            bangs.startNewSubPath (-17.8f, 4.5f);
            bangs.cubicTo (-22.5f, -12.0f, -13.0f, -26.5f, 0.5f, -26.0f);
            bangs.cubicTo (14.5f, -26.0f, 23.0f, -12.0f, 17.8f, 4.5f);
            bangs.quadraticTo (15.0f, -3.0f, 12.5f, -7.0f);
            bangs.quadraticTo (12.0f, -2.0f, 9.2f, -0.6f);
            bangs.quadraticTo (8.5f, -4.5f, 6.0f, -8.5f);
            bangs.quadraticTo (5.0f, -3.5f, 2.0f, -1.6f);
            bangs.quadraticTo (1.0f, -5.5f, -1.5f, -9.0f);
            bangs.quadraticTo (-2.5f, -3.5f, -5.2f, -1.2f);
            bangs.quadraticTo (-5.5f, -5.0f, -8.0f, -7.5f);
            bangs.quadraticTo (-9.5f, -2.0f, -12.8f, 1.2f);
            bangs.quadraticTo (-13.2f, -3.0f, -14.8f, -4.0f);
            bangs.quadraticTo (-16.5f, 0.0f, -17.8f, 4.5f);
            bangs.closeSubPath();
            bangs.applyTransform (AffineTransform::translation (p[pTurn] * 1.5f, 0.0f).followedBy (q.headXf));
        }
        cel (g, head, skin, skinSh, rimHi, 1.2f, 2.4f, 0.9f);
        {
            // the fringe casts a shadow on the forehead
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (head);
            g.setColour (A (skinSh.withAlpha (0.85f)));
            g.fillPath (bangs, AffineTransform::translation (0.6f, 2.4f));
        }
        drawFace (g, q, p, face);

        //--- side locks and the ahoge, the fringe, a hair clip -------------------------------------------------
        {
            Path locks;
            for (int i = 0; i < 2; ++i)
            {
                const float s = i == 0 ? -1.0f : 1.0f;
                const auto root = Point<float> (s * 14.2f, -2.0f).transformedBy (q.headXf);
                const float sway = hairX * 0.45f + fling * 14.0f;
                const float lift = hairY * 0.5f + std::abs (fling) * 8.0f;
                addStrand (locks, root, root + Point<float> (s * 3.5f + sway * 0.3f, 18.0f - lift * 0.3f),
                           root + Point<float> (s * 1.5f + sway, 36.0f - lift), 3.6f);
            }
            const auto a0 = Point<float> (1.0f, -24.0f).transformedBy (q.headXf);
            const auto a1 = Point<float> (-4.0f, -34.0f).transformedBy (q.headXf);
            const auto a2 = Point<float> (8.0f + hairX * 0.2f, -31.0f - 2.0f * b).transformedBy (q.headXf);
            addStrand (locks, a0, a1, a2, 1.6f);
            cel (g, locks, hair, hairSh, rimHi, 1.0f, 1.8f, 0.8f);
        }
        cel (g, bangs, hair, hairSh, rimHi, 1.3f, 2.2f, 0.9f);
        {
            // fringe strand lines
            Path lines;
            const std::array<std::array<float, 4>, 4> l { { { -6.0f, -20.0f, -9.5f, -9.0f }, { 2.0f, -21.0f, 0.5f, -10.0f },
                                                           { 9.0f, -19.0f, 10.0f, -9.0f }, { -13.0f, -14.0f, -15.5f, -5.0f } } };
            for (const auto& s : l)
            {
                lines.startNewSubPath (Point<float> (s[0], s[1]).transformedBy (q.headXf));
                lines.lineTo (Point<float> (s[2], s[3]).transformedBy (q.headXf));
            }
            g.setColour (A (hairSh));
            g.strokePath (lines, PathStrokeType (0.8f, PathStrokeType::curved, PathStrokeType::rounded));

            // sun hair clip
            const auto clip = Point<float> (-12.0f, -13.0f).transformedBy (q.headXf);
            g.setColour (A (inkColour()));
            g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (clip));
            g.setColour (A (Colours_::accent.interpolatedWith (Colours_::accentHi, b)));
            g.fillEllipse (Rectangle<float> (4.4f, 4.4f).withCentre (clip));
        }
        drawBrows (g, q, p, face);

        //--- the hand heart, spin swooshes ----------------------------------------------------------------------
        if (face.handL == hHeart && face.handR == hHeart)
        {
            const auto c = (q.ha[0] + q.ha[1]) * 0.5f + Point<float> (0.0f, -1.0f - 3.0f * b);
            const float s = 9.5f * (1.0f + 0.3f * b);
            Path h;
            h.startNewSubPath (c.x, c.y + 0.9f * s);
            h.cubicTo (c.x - 1.25f * s, c.y + 0.05f * s, c.x - 0.85f * s, c.y - 1.05f * s, c.x, c.y - 0.4f * s);
            h.cubicTo (c.x + 0.85f * s, c.y - 1.05f * s, c.x + 1.25f * s, c.y + 0.05f * s, c.x, c.y + 0.9f * s);
            h.closeSubPath();
            g.setColour (A (Colour (0xfffff3e6).withAlpha (0.75f + 0.25f * b)));   // a pale halo, so it reads on the tee
            g.strokePath (h, PathStrokeType (4.5f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setGradientFill (ColourGradient (A (Colours_::accent), c.x, c.y - 0.6f * s, A (Colour (0xffc4560a)), c.x, c.y + s, false));
            g.fillPath (h);
            g.setColour (A (inkColour()));
            g.strokePath (h, PathStrokeType (1.1f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (A (Colours::white.withAlpha (0.9f)));
            g.fillEllipse (c.x - 0.75f * s, c.y - 0.6f * s, 0.45f * s, 0.4f * s);
        }
        if (p[pFlare] > 0.55f)
        {
            Path sw;
            sw.addCentredArc (q.hip.x, q.hip.y + 10.0f, 38.0f, 9.0f, 0.0f, 1.9f, 4.1f, true);
            sw.addCentredArc (q.hip.x, q.hip.y - 22.0f, 32.0f, 7.0f, 0.0f, -1.2f, 1.0f, true);
            g.setColour (A (Colours_::accentHi.withAlpha (juce::jlimit (0.0f, 0.6f, (p[pFlare] - 0.55f) * 1.1f))));
            g.strokePath (sw, PathStrokeType (0.9f, PathStrokeType::curved, PathStrokeType::rounded));
        }
    }

    //==============================================================================================================
    // the back hair at rest for this pose: root, the point half way down and the tip of strand i
    static constexpr size_t numStrands = 7;

    std::array<juce::Point<float>, 3> strandRest (const Rig& q, float fling, size_t i) const
    {
        using P = juce::Point<float>;
        const float fi = (float) i - 3.0f;
        const auto root = P (fi * 4.6f, -4.0f + std::abs (fi) * 1.2f).transformedBy (q.headXf);
        const float len = 82.0f - std::abs (fi) * 6.0f;
        const float spread = fi * 5.0f;
        const float sway = fling * 40.0f * (0.75f + 0.1f * std::abs (fi))
                         + std::sin (clockPhase (4.0f) + (float) i * 0.9f) * 1.3f;
        const float lift = std::abs (fling) * len * 0.38f;
        const P ctrl = root + P (spread * 1.7f + sway * 0.25f, len * 0.42f - lift * 0.3f);
        const P tip  = root + P (spread + sway, len - lift);
        return { root, root * 0.25f + ctrl * 0.5f + tip * 0.25f, tip };
    }

    // the clock wraps every clockPeriod seconds; every periodic motion makes a whole number of cycles in that time
    static constexpr float clockPeriod = 13.6f;
    float clockPhase (float cycles) const noexcept { return clock * (juce::MathConstants<float>::twoPi * cycles / clockPeriod); }

    //==============================================================================================================
    // the face, drawn in head-local coordinates (origin at the centre of the head, chin at y = 19)
    int eyeState (const Face& face, bool screenLeft) const
    {
        const bool blink = std::fmod (clock, 3.4f) < 0.1f && idle > 0.3f;
        switch (face.eyes)
        {
            case eHappy:  return eHappy;
            case eWinkL:  return screenLeft ? eWinkL : (blink ? eWinkR : eOpen);
            case eWinkR:  return screenLeft ? (blink ? eWinkR : eOpen) : eWinkL;
            default:      return blink ? eWinkR : face.eyes;   // eWinkR here means "closed in a blink"
        }
    }

    void drawEye (juce::Graphics& g, juce::Point<float> c, float so, float wScale, int state, float gaze) const
    {
        using namespace juce;
        const Colour ink = A (inkColour());
        const float ew = 5.0f * wScale, eh = 6.2f;
        const PathStrokeType thick (1.6f, PathStrokeType::curved, PathStrokeType::rounded);

        if (state == eHappy)       // ^
        {
            Path a;
            a.startNewSubPath (c.x - ew, c.y + 1.4f);
            a.quadraticTo (c.x, c.y - 4.2f, c.x + ew, c.y + 1.4f);
            g.setColour (ink); g.strokePath (a, thick);
            return;
        }
        if (state == eWinkL)       // > or <, pointing at the nose
        {
            Path a;
            a.startNewSubPath (c.x + so * ew, c.y - 3.0f);
            a.lineTo (c.x - so * ew * 0.85f, c.y + 0.4f);
            a.lineTo (c.x + so * ew, c.y + 3.0f);
            g.setColour (ink); g.strokePath (a, thick);
            return;
        }
        if (state == eWinkR)       // blink: a soft closed lid with a lash flick
        {
            Path a;
            a.startNewSubPath (c.x - ew, c.y + 0.6f);
            a.quadraticTo (c.x, c.y + 3.4f, c.x + ew, c.y + 0.6f);
            a.lineTo (c.x + so * (ew + 1.4f), c.y - 0.6f);
            g.setColour (ink); g.strokePath (a, thick);
            return;
        }

        Path white;
        white.addEllipse (c.x - ew, c.y - eh, ew * 2.0f, eh * 2.0f);
        g.setColour (A (Colour (0xfffffbf7)));
        g.fillPath (white);
        {
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (white);
            const Point<float> ic (c.x + gaze - so * 0.3f, c.y + 0.7f);
            const float iw = ew * 0.8f, ih = eh;
            g.setGradientFill (ColourGradient (A (Colour (0xff5e1c04)), ic.x, ic.y - ih, A (Colour (0xffffa84e)), ic.x, ic.y + ih, false));
            g.fillEllipse (ic.x - iw, ic.y - ih, iw * 2.0f, ih * 2.0f);
            g.setColour (A (Colour (0xff280a02)));
            g.fillEllipse (ic.x - iw * 0.48f, ic.y - ih * 0.62f, iw * 0.96f, ih * 1.05f);
            g.setColour (A (Colours::white));
            g.fillEllipse (ic.x - iw * 0.85f, ic.y - ih * 0.82f, 3.4f, 3.6f);
            g.fillEllipse (ic.x + iw * 0.2f, ic.y + ih * 0.28f, 1.8f, 1.8f);
            if (state == eSparkle)
            {
                Path st;
                addStar (st, { ic.x + iw * 0.35f, ic.y - ih * 0.25f }, 2.2f, 0.0f);
                g.fillPath (st);
            }
            if (state == eFierce)   // lids drop towards the nose
            {
                Path lid;
                lid.startNewSubPath (c.x - ew - 1.0f, c.y - eh - 1.0f);
                lid.lineTo (c.x + ew + 1.0f, c.y - eh - 1.0f);
                lid.lineTo (c.x + so * (ew + 1.0f), c.y - eh * 0.55f);
                lid.lineTo (c.x - so * (ew + 1.0f), c.y - eh * 0.05f);
                lid.closeSubPath();
                g.setColour (A (Colour (0xffffe6d6)));
                g.fillPath (lid);
            }
        }
        Path lash;
        if (state == eFierce)
        {
            lash.startNewSubPath (c.x - so * ew, c.y - eh * 0.05f);
            lash.lineTo (c.x + so * ew * 1.05f, c.y - eh * 0.58f);
        }
        else
        {
            lash.startNewSubPath (c.x - so * ew, c.y - eh * 0.25f);
            lash.quadraticTo (c.x - so * ew * 0.2f, c.y - eh * 1.32f, c.x + so * ew * 1.05f, c.y - eh * 0.6f);
        }
        lash.lineTo (c.x + so * ew * 1.45f, c.y - eh * 0.95f);   // the flick at the outer corner
        g.setColour (ink);
        g.strokePath (lash, PathStrokeType (2.1f, PathStrokeType::curved, PathStrokeType::rounded));
        Path lower;
        lower.startNewSubPath (c.x + so * ew * 0.98f, c.y + eh * 0.3f);
        lower.quadraticTo (c.x + so * ew * 0.7f, c.y + eh * 1.0f, c.x - so * ew * 0.3f, c.y + eh * 1.02f);
        g.setColour (ink);
        g.strokePath (lower, PathStrokeType (1.25f, PathStrokeType::curved, PathStrokeType::rounded));
    }

    void drawFace (juce::Graphics& g, const Rig& q, const Vals& p, const Face& face) const
    {
        using namespace juce;
        Graphics::ScopedSaveState s (g);
        g.addTransform (q.headXf);
        const float turnAmt = juce::jlimit (-1.0f, 1.0f, p[pTurn]);
        const float fx = turnAmt * 4.5f;
        const Colour ink = A (inkColour());

        // blush: a soft patch with three hatched strokes
        Path hatch;
        for (float sx : { -1.0f, 1.0f })
        {
            const float squeeze = 1.0f - 0.3f * std::max (0.0f, sx * turnAmt);
            const Point<float> bc (sx * 10.6f + fx * 0.7f, 11.0f);
            g.setColour (A (Colour (0xffff8f78).withAlpha (0.45f + 0.25f * bounce)));
            g.fillEllipse (Rectangle<float> (6.4f * squeeze, 2.8f).withCentre (bc));
            for (int j = -1; j <= 1; ++j)
            {
                const float x = bc.x + 1.8f * squeeze * (float) j;
                hatch.startNewSubPath (x + 0.7f, bc.y - 1.1f);
                hatch.lineTo (x - 0.5f, bc.y + 1.1f);
            }
        }
        g.setColour (A (Colour (0xffe0503e).withAlpha (0.8f)));
        g.strokePath (hatch, PathStrokeType (0.55f, PathStrokeType::curved, PathStrokeType::rounded));

        const float gaze = turnAmt * 1.2f;
        drawEye (g, { -7.6f + fx, 3.8f }, -1.0f, 1.0f - 0.32f * std::max (0.0f, -turnAmt), eyeState (face, true), gaze);
        drawEye (g, {  7.6f + fx, 3.8f },  1.0f, 1.0f - 0.32f * std::max (0.0f,  turnAmt), eyeState (face, false), gaze);

        // nose
        Path nose;
        nose.startNewSubPath (fx * 1.1f + 0.5f, 9.4f);
        nose.lineTo (fx * 1.1f, 10.4f);
        g.setColour (A (Colour (0xffc98a72)));
        g.strokePath (nose, PathStrokeType (0.8f, PathStrokeType::curved, PathStrokeType::rounded));

        // mouth
        const Point<float> m (fx * 0.9f, 13.8f);
        const PathStrokeType line (1.2f, PathStrokeType::curved, PathStrokeType::rounded);
        Path mouth;
        switch (face.mouth)
        {
            case mGrin:
            case mO:
            {
                if (face.mouth == mGrin)
                {
                    const float o = 1.0f + 0.25f * bounce;
                    mouth.startNewSubPath (m.x - 4.2f, m.y - 1.2f);
                    mouth.quadraticTo (m.x, m.y - 0.4f, m.x + 4.2f, m.y - 1.2f);
                    mouth.cubicTo (m.x + 3.6f, m.y + 4.0f * o, m.x - 3.6f, m.y + 4.0f * o, m.x - 4.2f, m.y - 1.2f);
                    mouth.closeSubPath();
                }
                else
                {
                    mouth.addEllipse (m.x - 4.0f, m.y - 2.0f, 8.0f, 5.2f + bounce);
                }
                g.setColour (A (Colour (0xff6e1a12)));
                g.fillPath (mouth);
                {
                    Graphics::ScopedSaveState ms (g);
                    g.reduceClipRegion (mouth);
                    g.setColour (A (Colour (0xffff7a68)));
                    g.fillEllipse (m.x - 2.6f, m.y + 1.0f, 5.2f, 4.0f);
                }
                g.setColour (ink);
                g.strokePath (mouth, PathStrokeType (0.9f));
                break;
            }
            case mCat:
                mouth.startNewSubPath (m.x - 3.8f, m.y - 0.9f);                   // the cat 'w'
                mouth.quadraticTo (m.x - 1.9f, m.y + 2.2f, m.x, m.y - 0.1f);
                mouth.quadraticTo (m.x + 1.9f, m.y + 2.2f, m.x + 3.8f, m.y - 0.9f);
                g.setColour (ink); g.strokePath (mouth, line);
                break;
            case mSmirk:
                mouth.startNewSubPath (m.x - 2.4f, m.y + 0.4f);
                mouth.quadraticTo (m.x + 0.6f, m.y + 1.4f, m.x + 2.9f, m.y - 1.3f);
                g.setColour (ink); g.strokePath (mouth, line);
                break;
            default:
                mouth.startNewSubPath (m.x - 3.4f, m.y - 0.6f);
                mouth.quadraticTo (m.x, m.y + 2.2f, m.x + 3.4f, m.y - 0.6f);
                g.setColour (ink); g.strokePath (mouth, line);
                break;
        }
    }

    // brows sit over the fringe, as anime brows do
    void drawBrows (juce::Graphics& g, const Rig& q, const Vals& p, const Face& face) const
    {
        using namespace juce;
        Graphics::ScopedSaveState s (g);
        g.addTransform (q.headXf);
        const float fx = juce::jlimit (-1.0f, 1.0f, p[pTurn]) * 4.5f;
        const float br = face.eyes == eFierce ? std::min (p[pBrow], -0.6f) : p[pBrow];
        Path brows;
        for (float so : { -1.0f, 1.0f })
        {
            const float cx = so * 7.4f + fx, by = -5.6f - br * 1.6f;
            brows.startNewSubPath (cx - so * 3.2f, by + std::max (0.0f, -br) * 2.2f - std::max (0.0f, br) * 0.5f);
            brows.quadraticTo (cx + so * 0.5f, by - 1.3f, cx + so * 4.0f, by + 0.9f);
        }
        g.setColour (A (Colour (0xff5a2c1c)));
        g.strokePath (brows, PathStrokeType (1.1f, PathStrokeType::curved, PathStrokeType::rounded));
    }

    //==============================================================================================================
    static constexpr float tickSeconds = 1.0f / 60.0f;

    void timerCallback() override
    {
        constexpr float dt = tickSeconds;
        clock = std::fmod (clock + dt, clockPeriod);
        const unsigned int hits = hitCounter.load (std::memory_order_relaxed);
        if (hits != lastHits)
        {
            lastHits = hits;
            fromVals = baseVals();
            fromFace = currentFace();
            to = (to + 1) % (int) moves().size();
            blend = 0.0f;
            bounce = 1.0f;
            sinceHit = 0.0f;
            idle = 0.0f;
            spawnFlecks();
        }
        blend = std::min (1.0f, blend + dt / 0.16f);
        bounce *= 0.9f;
        sinceHit = std::min (sinceHit + dt, 100.0f);
        idle += ((sinceHit > 1.5f ? 1.0f : 0.0f) - idle) * 0.05f;   // starts swaying once the kicks stop

        // secondary motion: hair and skirt lag behind the body on damped springs
        const Vals pose = currentPose();
        const Rig q = makeRig (pose);
        if (! primed)
        {
            prevHeadX = q.head.x; prevHipY = q.hip.y; prevHipX = q.hip.x;
            resetHair (q, pose[pFling]);
            primed = true;
        }
        const float vx = (q.head.x - prevHeadX) / dt, vy = (q.hip.y - prevHipY) / dt, vhx = (q.hip.x - prevHipX) / dt;
        prevHeadX = q.head.x; prevHipY = q.hip.y; prevHipX = q.hip.x;
        auto spring = [] (float& x, float& v, float target, float k, float c)
        {
            v += (k * (target - x) - c * v) * tickSeconds;
            v = juce::jlimit (-600.0f, 600.0f, v);
            x = juce::jlimit (-30.0f, 30.0f, x + v * tickSeconds);
        };
        spring (hairX, hairV, juce::jlimit (-22.0f, 22.0f, -vx * 0.09f), 140.0f, 11.0f);
        spring (hairY, hairYV, juce::jlimit (-14.0f, 18.0f, vy * 0.07f), 160.0f, 12.0f);
        spring (skirtX, skirtV, juce::jlimit (-10.0f, 10.0f, -vhx * 0.05f), 260.0f, 15.0f);
        for (float* x : { &hairX, &hairV, &hairY, &hairYV, &skirtX, &skirtV, &prevHeadX, &prevHipY, &prevHipX })
            if (! std::isfinite (*x)) *x = 0.0f;
        stepHair (q, pose[pFling]);

        for (auto& f : flecks)
        {
            if (f.life <= 0.0f) continue;
            f.life = std::max (0.0f, f.life - dt / 0.75f);
            f.y -= 14.0f * dt;
            f.rot += 2.0f * dt;
        }

        alpha += (targetAlpha - alpha) * 0.18f;
        if (targetAlpha < 0.5f && alpha < 0.02f)
        {
            alpha = 0.0f;
            setVisible (false);
            stopTimer();
            stageImage = glowImage = {};   // free the caches while she is away
            return;
        }
        // once she has settled into the idle sway, 30 frames a second is plenty: the motion still steps at 60 Hz,
        // only every other frame is painted
        const bool settled = idle > 0.9f && bounce < 0.01f && std::abs (targetAlpha - alpha) < 0.01f;
        if (! settled || (++tickCount & 1u) == 0)
            repaint();
    }

    // back hair: each strand is a three-node verlet chain (root pinned to the head), pulled towards its rest shape.
    // Outer strands are softer, so the fan whips out after a big move and settles strand by strand.
    void resetHair (const Rig& q, float fling)
    {
        for (size_t i = 0; i < numStrands; ++i)
        {
            const auto r = strandRest (q, fling, i);
            hairRoot[i] = r[0];
            hairNow[i] = hairPrev[i] = { r[1], r[2] };
        }
    }

    void stepHair (const Rig& q, float fling)
    {
        using P = juce::Point<float>;
        auto reach = [] (P anchor, P& pt, float len)
        {
            const P d = pt - anchor;
            const float l = d.getDistanceFromOrigin();
            if (l > len && l > 1.0e-3f) pt = anchor + d * (len / l);
        };
        for (size_t i = 0; i < numStrands; ++i)
        {
            const auto r = strandRest (q, fling, i);
            const float soft = 1.0f - 0.09f * std::abs ((float) i - 3.0f);
            const std::array<float, 2> stiff { 0.2f * soft, 0.1f * soft };
            hairRoot[i] = r[0];
            for (size_t j = 0; j < 2; ++j)
            {
                const P vel = (hairNow[i][j] - hairPrev[i][j]) * 0.88f;
                hairPrev[i][j] = hairNow[i][j];
                hairNow[i][j] += vel + P (0.0f, 0.2f) + (r[j + 1] - hairNow[i][j]) * stiff[j];
            }
            const float l1 = r[0].getDistanceFrom (r[1]), l2 = r[1].getDistanceFrom (r[2]);
            for (int pass = 0; pass < 2; ++pass)
            {
                reach (r[0], hairNow[i][0], l1);
                reach (hairNow[i][0], hairNow[i][1], l2);
            }
            bool finite = true;
            for (size_t j = 0; j < 2; ++j)
                finite = finite && std::isfinite (hairNow[i][j].x + hairNow[i][j].y + hairPrev[i][j].x + hairPrev[i][j].y);
            if (! finite)
                hairNow[i] = hairPrev[i] = { r[1], r[2] };
        }
    }

    void spawnFlecks()
    {
        // three small sparks, out at the sides below the shoulders: never between the head and the arms
        const float side0 = rng.nextBool() ? -1.0f : 1.0f;
        for (int n = 0; n < 3; ++n)
        {
            auto& f = flecks[nextFleck];
            nextFleck = (nextFleck + 1) % flecks.size();
            const float side = n == 1 ? -side0 : side0;
            f.x = 85.0f + side * (64.0f + 12.0f * rng.nextFloat());
            f.y = 96.0f + 34.0f * (float) n + 18.0f * rng.nextFloat();
            f.size = 2.0f + 1.4f * rng.nextFloat();
            f.rot = 0.4f * rng.nextFloat();
            f.life = 1.0f;
        }
    }

    struct Fleck { float x = 0.0f, y = 0.0f, size = 0.0f, rot = 0.0f, life = 0.0f; };

    std::atomic<unsigned int>& hitCounter;
    unsigned int lastHits = 0;
    int to = 0;
    Vals fromVals {};
    Face fromFace { eOpen, mSmile, hRelax, hRelax };
    float blend = 1.0f, bounce = 0.0f, clock = 0.0f, sinceHit = 10.0f, idle = 1.0f, alpha = 0.0f, targetAlpha = 0.0f;
    float hairX = 0.0f, hairV = 0.0f, hairY = 0.0f, hairYV = 0.0f, skirtX = 0.0f, skirtV = 0.0f;
    float prevHeadX = 0.0f, prevHipY = 0.0f, prevHipX = 0.0f;
    bool primed = false;
    std::array<juce::Point<float>, numStrands> hairRoot {};
    std::array<std::array<juce::Point<float>, 2>, numStrands> hairNow {}, hairPrev {};
    std::array<Fleck, 9> flecks {};
    size_t nextFleck = 0;
    juce::Random rng { 0x4b1d };
    unsigned int tickCount = 0;
    const juce::Rectangle<float> stageBox = stageBoxRect();
    mutable juce::Image stageImage, glowImage;   // caches, rebuilt in paint when the pixel scale changes
    mutable float cachedScale = 0.0f;
};
