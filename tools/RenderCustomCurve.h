// Custom distortion curve checks for CodexRender (included by Render.cpp after its helpers).
#pragma once
#include "../Source/DSP/CurveSeed.h"
#include "../Source/GUI/DistortionView.h"
#include <thread>
#include <new>
#include <cstdlib>

//==============================================================================
// Allocation counter for the real-time checks: counts operator new on threads that ask for it.
static thread_local bool countAllocs = false;
static std::atomic<long> allocCount { 0 };

void* operator new (std::size_t n)
{
    if (countAllocs) allocCount.fetch_add (1);
    if (void* p = std::malloc (n > 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (countAllocs) allocCount.fetch_add (1);
    if (void* p = std::malloc (n > 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace curvetest
{
    inline float maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        float m = 0.0f;
        const int n = std::min (a.getNumSamples(), b.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i) m = std::max (m, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        return a.getNumSamples() == b.getNumSamples() ? m : 1.0e9f;
    }
    inline float relDiffDb (const juce::AudioBuffer<float>& ref, const juce::AudioBuffer<float>& b)
    {
        double e = 0.0, s = 0.0;
        const int n = std::min (ref.getNumSamples(), b.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const double d = (double) ref.getSample (ch, i) - b.getSample (ch, i);
                e += d * d;
                s += (double) ref.getSample (ch, i) * ref.getSample (ch, i);
            }
        return (float) (10.0 * std::log10 ((e + 1.0e-30) / (s + 1.0e-30)));
    }
    /** Renders a state in a fresh processor (deterministic). */
    inline juce::AudioBuffer<float> renderState (const juce::MemoryBlock& state, int hits = 2)
    {
        CodexProcessor p;
        p.setPlayConfigDetails (0, 2, sr, block);
        p.setStateInformation (state.getData(), (int) state.getSize());
        p.seedNoiseForTesting (11);
        return render (p, hits);
    }
    inline juce::MemoryBlock stateOf (CodexProcessor& p) { juce::MemoryBlock m; p.getStateInformation (m); return m; }
    inline juce::MemoryBlock editState (const juce::MemoryBlock& st, std::function<void (juce::ValueTree&)> fn)
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        auto tree = juce::ValueTree::fromXml (*xml);
        fn (tree);
        juce::MemoryBlock out;
        juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), out);
        return out;
    }
    inline FreeCurve bentCurve()
    {
        FreeCurve c;
        c.symmetric = true; c.reach = 3.0f; c.count = 0;
        c.p[(size_t) c.count++] = { 0.0f, 0.0f, 0.0f, false };
        c.p[(size_t) c.count++] = { 0.25f, 0.55f, 0.4f, false };
        c.p[(size_t) c.count++] = { 0.5f, 0.8f, -0.3f, true };
        c.p[(size_t) c.count++] = { 0.8f, 0.6f, 0.2f, false };
        c.p[(size_t) c.count++] = { 1.0f, 0.9f, 0.0f, false };
        c.sanitise();
        return c;
    }
    inline float curveDiff (const FreeCurve& a, const FreeCurve& b)
    {
        const FreeCurveShape sa (a), sb (b);
        float m = 0.0f;
        for (int k = 0; k <= 4000; ++k) { const float u = -1.0f + 2.0f * (float) k / 4000.0f; m = std::max (m, std::abs (sa.evalU (u) - sb.evalU (u))); }
        return m;
    }
}

//==============================================================================
/** Pure curve maths, seeding and the ADAA table. */
static int customCurveUnitTests()
{
    using namespace curvetest;
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };
    std::cout << "\nCustom curves: evaluation\n";
    {
        const FreeCurveShape lin (FreeCurve::linear (true, 1.0f)), linA (FreeCurve::linear (false, 1.0f));
        float w = 0.0f, wa = 0.0f, whc = 0.0f;
        for (float u = -1.0f; u <= 1.0f; u += 0.001f) { w = std::max (w, std::abs (lin.evalU (u) - u)); wa = std::max (wa, std::abs (linA.evalU (u) - u)); }
        for (float v = -2.0f; v <= 2.0f; v += 0.001f) whc = std::max (whc, std::abs (lin.evalV (v) - ClipDistortion::shapeFor (ClipDistortion::HardClip, v)));
        std::cout << "  linear = identity (sym " << w << ", asym " << wa << "), = Hard Clip within " << whc << "\n";
        check (w < 1.0e-6f && wa < 1.0e-6f && whc < 1.0e-6f, "linear curve");
    }
    {
        // sharp / sharp with bend 0: a straight line; with a bend: the envelope's exponential
        FreeCurve c; c.symmetric = true; c.count = 0;
        c.p[(size_t) c.count++] = { 0, 0, 0, true };
        c.p[(size_t) c.count++] = { 0.5f, 0.8f, 0, true };
        c.p[(size_t) c.count++] = { 1, 1, 0, true };
        c.sanitise();
        const FreeCurveShape s (c);
        float w = 0.0f;
        for (float u = 0.0f; u <= 0.5f; u += 0.001f) w = std::max (w, std::abs (s.evalU (u) - 1.6f * u));
        c.setBend (0, 0.5f);
        const FreeCurveShape b (c);
        const float mid = b.evalU (0.25f), expect = 0.8f * DrawEnvelope::curve (0.5f, 0.5f);
        c.setBend (0, -0.5f);
        const float midNeg = FreeCurveShape (c).evalU (0.25f);
        std::cout << "  sharp segment linear within " << w << "; bend +0.5 mid " << mid << " (envelope " << expect << "), bend -0.5 mid " << midNeg << "\n";
        check (w < 1.0e-6f && std::abs (mid - expect) < 1.0e-5f && mid > 0.4f && midNeg < 0.4f, "bend sign / straight segment");
    }
    {
        // smooth monotone data: monotone, no overshoot
        FreeCurve c; c.symmetric = false; c.count = 0;
        const float xs[] = { -1, -0.7f, -0.2f, 0.0f, 0.1f, 0.4f, 0.45f, 1 }, ys[] = { -1, -0.95f, -0.3f, 0.0f, 0.6f, 0.62f, 0.9f, 0.95f };
        for (int i = 0; i < 8; ++i) c.p[(size_t) c.count++] = { xs[i], ys[i], 0, false };
        c.sanitise();
        const FreeCurveShape s (c);
        bool mono = true, inside = true;
        float prev = -2.0f;
        for (float u = -1.0f; u <= 1.0f; u += 0.0005f)
        {
            const float y = s.evalU (u);
            mono &= y >= prev - 1.0e-6f;
            prev = y;
            const int i = s.segmentFor (u);
            inside &= y >= std::min (c.p[(size_t) i].y, c.p[(size_t) i + 1].y) - 1.0e-5f && y <= std::max (c.p[(size_t) i].y, c.p[(size_t) i + 1].y) + 1.0e-5f;
        }
        // fold: extrema exactly on the points (slope 0), never beyond them
        FreeCurve f; f.symmetric = true; f.count = 0;
        f.p[(size_t) f.count++] = { 0, 0 }; f.p[(size_t) f.count++] = { 0.3f, 0.9f }; f.p[(size_t) f.count++] = { 0.6f, -0.2f }; f.p[(size_t) f.count++] = { 1, 0.5f };
        f.sanitise();
        const FreeCurveShape fs (f);
        float peak = -2.0f;
        for (float u = 0.0f; u <= 1.0f; u += 0.0005f) peak = std::max (peak, fs.evalU (u));
        const float slope = (fs.evalU (0.3001f) - fs.evalU (0.2999f)) / 0.0002f;
        // sharp point: different one-sided slopes; smooth point: C1
        FreeCurve k = f; k.setSharp (1, true);
        const FreeCurveShape ks (k);
        const float sl = (ks.evalU (0.3f) - ks.evalU (0.299f)) / 0.001f, sr2 = (ks.evalU (0.301f) - ks.evalU (0.3f)) / 0.001f;
        FreeCurve m = c;
        const FreeCurveShape ms (m);
        const float a1 = (ms.evalU (0.1f) - ms.evalU (0.0999f)) / 0.0001f, a2 = (ms.evalU (0.1001f) - ms.evalU (0.1f)) / 0.0001f;
        std::cout << "  smooth monotone: " << (mono && inside ? "monotone, no overshoot" : "OVERSHOOT") << "; fold peak " << peak << " slope " << slope
                  << "; sharp corner slopes " << sl << " / " << sr2 << "; smooth join " << a1 << " / " << a2 << "\n";
        check (mono && inside, "monotone");
        check (std::abs (peak - 0.9f) < 1.0e-4f && std::abs (slope) < 0.05f, "fold extremum on the point");
        check (std::abs (sl - sr2) > 1.0f, "sharp corner");
        check (std::abs (a1 - a2) < 0.02f * std::max (1.0f, std::abs (a1)), "smooth join C1");
    }
    {
        // symmetry, independent halves, flat beyond the ends, Sym -> Asym is the same curve
        const auto c = bentCurve();
        const FreeCurveShape s (c);
        float odd = 0.0f;
        for (float u = 0.0f; u <= 1.0f; u += 0.001f) odd = std::max (odd, std::abs (s.evalU (u) + s.evalU (-u)));
        const auto a = CurveSeed::toggledSymmetry (c);
        const float same = curveDiff (c, a);
        auto a2 = a;
        a2.move (1, a2.p[1].x, -0.2f);   // a negative-half point
        const FreeCurveShape s2 (a2), sa (a);
        float posSame = 0.0f;
        for (float u = 0.0f; u <= 1.0f; u += 0.001f) posSame = std::max (posSame, std::abs (s2.evalU (u) - sa.evalU (u)));
        const bool flat = s.evalV (10.0f * c.reach) == s.evalU (1.0f) && s.evalV (-10.0f * c.reach) == -s.evalU (1.0f);
        std::cout << "  symmetric odd within " << odd << "; sym -> asym same within " << same << " (" << a.count << " points); asym halves independent "
                  << posSame << "; flat beyond the range: " << (flat ? "yes" : "NO") << "\n";
        check (odd < 1.0e-6f && same < 1.0e-5f && ! a.symmetric && posSame < 1.0e-6f && flat, "symmetry");
        // flip, mirror
        const auto fl = CurveSeed::flipped (c);
        const auto mi = CurveSeed::mirroredPosToNeg (CurveSeed::starter (CurveSeed::WarmTube, 2.0f));
        const FreeCurveShape fls (fl), mis (mi);
        float fw = 0.0f, mw = 0.0f;
        for (float u = -1.0f; u <= 1.0f; u += 0.001f) { fw = std::max (fw, std::abs (fls.evalU (u) + s.evalU (u))); mw = std::max (mw, std::abs (mis.evalU (u) + mis.evalU (-u))); }
        check (fw < 1.0e-6f && mw < 1.0e-5f && ! mi.symmetric, "flip / mirror");
    }
    {
        // text form: exact round trip; garbage is rejected or repaired
        const auto c = CurveSeed::starter (CurveSeed::Staircase, 7.5f);
        FreeCurve back;
        const bool ok = FreeCurve::fromString (c.toString(), back);
        FreeCurve g1, g2, g3, g4;
        const bool r1 = FreeCurve::fromString ("2|S|1|0,0,0,0;1,1,0,0", g1);
        const bool r2 = FreeCurve::fromString ("nonsense", g2);
        juce::String many ("1|A|2.5|");
        juce::Random rng (3);
        for (int i = 0; i < 90; ++i) many << (i ? ";" : "") << rng.nextFloat() * 2.0f - 1.0f << "," << rng.nextFloat() * 3.0f - 1.5f << ",nan,1";
        const bool r3 = FreeCurve::fromString (many, g3);
        bool sorted = g3.count >= 2 && g3.count <= FreeCurve::maxPoints && g3.p[0].x == -1.0f && g3.p[(size_t) g3.count - 1].x == 1.0f;
        for (int i = 1; i < g3.count; ++i) sorted &= g3.p[(size_t) i].x - g3.p[(size_t) i - 1].x >= FreeCurve::spacingTol && std::abs (g3.p[(size_t) i].y) <= 1.0f;
        const bool r4 = FreeCurve::fromString ("1|S|1|0.5,0.5,0,0", g4);
        std::cout << "  text round trip: " << (ok && back == c && back.toString() == c.toString() ? "exact" : "DIFFERS") << "; bad version / garbage rejected: "
                  << (! r1 && ! r2 && ! r4 ? "yes" : "NO") << "; 90 unordered NaN points -> " << g3.count << " valid points\n";
        check (ok && back == c && back.toString() == c.toString() && ! r1 && ! r2 && ! r4 && r3 && sorted, "text form");
    }
    {
        // ADAA table: the lookup follows the curve; the antiderivative's difference quotient is the curve's mean
        CustomCurveBank bank;
        const auto c = bentCurve();
        bank.publish (&c);
        int front = 0, prev = 1;
        const bool got = bank.tryAcquire (front, prev);
        const auto& sl = bank.slot (front);
        const FreeCurveShape s (c);
        float wShape = 0.0f, wAdaa = 0.0f;
        for (float v = -4.0f; v <= 4.0f; v += 0.0037f)
        {
            wShape = std::max (wShape, std::abs (sl.shapeV (v) - s.evalV (v)));
            const float a = v, b = v + 0.05f;
            double mean = 0.0;
            for (int k = 0; k < 200; ++k) mean += s.evalV (a + (b - a) * ((float) k + 0.5f) / 200.0f);
            mean /= 200.0;
            wAdaa = std::max (wAdaa, (float) std::abs ((sl.Fv (b) - sl.Fv (a)) / (b - a) - mean));
        }
        std::cout << "  compiled table: lookup within " << wShape << ", ADAA quotient within " << wAdaa << "\n";
        check (got && sl.valid && wShape < 1.0e-3f && wAdaa < 2.0e-3f, "compiled table");
    }

    // ---- Seeding: every Type at several drives is reproduced by its seed ----
    std::cout << "Custom curves: seeding from each Type (max error over the seeded range, points)\n";
    {
        const ClipDistortion::Curve legacyDefault;
        ClipDistortion::Curve asymCurve;
        for (int k = 0; k < 8; ++k) { asymCurve.pos[(size_t) k] = std::min (1.0f, 0.3f + 0.12f * (float) k); asymCurve.neg[(size_t) k] = 0.6f * (float) (k + 1) / 8.0f; }
        for (int m = 0; m < ClipDistortion::numModes; ++m)
        {
            juce::String line ("   " + ClipDistortion::modeNames()[m].paddedRight (' ', 16));
            const auto& legacy = m == ClipDistortion::XShaperAsym ? asymCurve : legacyDefault;
            for (float driveDb : { 0.0f, 6.0f, 18.0f, 36.0f })
            {
                const float reach = CurveSeed::reachFor (m, legacy, juce::Decibels::decibelsToGain (driveDb), 1.0f, 0.0f, 0.0f, 0.0f);
                const auto seed = CurveSeed::fromMode (m, legacy, reach);
                const FreeCurveShape s (seed.curve);
                const float R = seed.curve.reach;
                float err = 0.0f;
                for (int k = 0; k <= 20000; ++k)
                {
                    const float v = -R + 2.0f * R * (float) k / 20000.0f;
                    if (m == ClipDistortion::Bitcrush)
                    {
                        const float q = std::abs (v) * 8.0f;   // steps at (k + 0.5) / 8
                        if (std::abs (q - std::floor (q) - 0.5f) * (1.0f / 8.0f) < 0.004f * R && std::abs (v) < 1.0f) continue;
                    }
                    err = std::max (err, std::abs (s.evalV (v) - ClipDistortion::shapeFor (m, v, &legacy)));
                }
                const bool fold = m == ClipDistortion::SineFold || m == ClipDistortion::TriFold;
                const float bound = m == ClipDistortion::Bitcrush || (fold && R > 12.0f) ? 0.05f : 0.01f;
                const bool asymMode = m == ClipDistortion::Tube || m == ClipDistortion::Diode || m == ClipDistortion::Rectify
                                   || m == ClipDistortion::AsymClip || m == ClipDistortion::XShaperAsym;
                const bool symOk = seed.curve.symmetric == ! asymMode;
                const bool ok = err < bound && symOk && seed.curve.count <= FreeCurve::maxPoints;
                line << "  " << juce::String ((int) driveDb).paddedLeft (' ', 2) << " dB: " << juce::String (err, 4) << " / " << seed.curve.count
                     << (seed.curve.symmetric ? "s" : "a") << (ok ? "" : " FAIL");
                if (! ok) ++bad;
            }
            std::cout << line << "\n";
        }
    }
    return bad;
}

//==============================================================================
/** Processor level: seed renders, legacy compatibility, round trips, undo, export, presets, A/B. */
static int customCurveProcessorTests (const juce::File& outDir)
{
    using namespace curvetest;
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };
    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

    // ---- the seed sounds like the Type ----
    std::cout << "\nCustom curves: a kick through each Type vs through its seeded custom curve (difference, rel. to the signal)\n";
    {
        CodexProcessor p;
        p.setPlayConfigDetails (0, 2, sr, block);
        setParam (p, ID::keyMode, 0.0f);
        juce::String line ("  ");
        for (int m : { (int) ClipDistortion::HardClip, (int) ClipDistortion::SoftClip, (int) ClipDistortion::Tanh, (int) ClipDistortion::Overdrive,
                       (int) ClipDistortion::Tube, (int) ClipDistortion::Diode2, (int) ClipDistortion::SineFold, (int) ClipDistortion::Bitcrush,
                       (int) ClipDistortion::Downsample, (int) ClipDistortion::XShaperAsym })
        {
            p.presets.loadPreset (0);
            setParam (p, ID::keyMode, 0.0f);
            setParam (p, ID::clip (1, "Mode"), (float) m);
            p.seedNoiseForTesting (5);
            const auto ref = render (p, 1);
            const auto seed = p.seedCustomCurve (1);
            p.getCustomCurves().set (1, &seed, nullptr);
            setParam (p, ID::clip (1, "Cust"), 1.0f);
            p.seedNoiseForTesting (5);
            const auto cust = render (p, 1);
            const bool active = p.getClip (1).isCustomActive();
            const float d = relDiffDb (ref, cust);
            const bool loose = m == ClipDistortion::Bitcrush || m == ClipDistortion::Downsample || m == ClipDistortion::SineFold;
            const bool ok = active && d < (loose ? -25.0f : -40.0f);
            line << ClipDistortion::modeNames()[m] << " " << juce::String (d, 1) << " dB" << (ok ? "" : " FAIL") << (active ? "" : " (not active)") << ", ";
            if (! ok) ++bad;
            setParam (p, ID::clip (1, "Cust"), 0.0f);
            p.getCustomCurves().clearAll (nullptr);
        }
        std::cout << line << "\n";
    }

    // ---- old X-Shaper projects: identical with or without the new state ----
    {
        CodexProcessor p;
        p.setPlayConfigDetails (0, 2, sr, block);
        setParam (p, ID::keyMode, 0.0f);
        const int zaag = p.presets.getFactoryNames().indexOf ("X-Shaped Zaag");
        int states = 0, same = 0;
        for (int variant = 0; variant < 2; ++variant)
        {
            if (variant == 0) p.presets.loadPreset (juce::jmax (0, zaag));
            else
            {
                p.presets.loadPreset (0);
                setParam (p, ID::clip (2, "Mode"), (float) ClipDistortion::XShaperAsym);
                setParam (p, ID::clip (2, "Drive"), 14.0f);
                for (int k = 0; k < 8; ++k)
                {
                    setParam (p, ID::clip (2, ("XP" + juce::String (k + 1)).toRawUTF8()), std::sin (0.4f * (float) (k + 1)));
                    setParam (p, ID::clip (2, ("XN" + juce::String (k + 1)).toRawUTF8()), 0.1f * (float) k);
                }
            }
            setParam (p, ID::keyMode, 0.0f);
            const auto base = stateOf (p);
            const auto ref = renderState (base);
            const auto c = CurveSeed::starter (CurveSeed::DoubleFold, 4.0f);
            const juce::MemoryBlock variants[] = {
                editState (base, [] (juce::ValueTree& t) { for (int s = 1; s <= 2; ++s) t.removeChild (t.getChildWithProperty ("id", ID::clip (s, "Cust")), nullptr);
                                                           t.removeChild (t.getChildWithName (ID::curvesNode), nullptr); }),
                editState (base, [] (juce::ValueTree& t) { for (int s = 1; s <= 2; ++s) t.getChildWithProperty ("id", ID::clip (s, "Cust")).setProperty ("value", 1.0f, nullptr); }),
                editState (base, [&c] (juce::ValueTree& t) { juce::ValueTree n (ID::curvesNode); n.setProperty (ID::curveProp (1), c.toString(), nullptr);
                                                             n.setProperty (ID::curveProp (2), c.toString(), nullptr); t.appendChild (n, nullptr); })
            };
            for (const auto& v : variants)
            {
                ++states;
                const float d = maxDiff (ref, renderState (v));
                if (d == 0.0f) ++same; else std::cout << "   legacy variant differs by " << d << "\n";
            }
        }
        // factory presets never carry a custom curve
        int clean = 0;
        const auto c = CurveSeed::starter (CurveSeed::HardWall, 2.0f);
        for (int i = 0; i < p.presets.getNumFactory(); ++i)
        {
            p.getCustomCurves().set (1, &c, nullptr);
            setParam (p, ID::clip (1, "Cust"), 1.0f);
            p.presets.loadPreset (i);
            clean += (! p.getCustomCurves().has (1) && ! p.getCustomCurves().has (2) && ! p.isCustomEnabled (1) && ! p.isCustomEnabled (2)) ? 1 : 0;
        }
        std::cout << "Custom curves: old X-Shaper states (no Custom Curve / switch on without a curve / curve stored but off) render identically: "
                  << same << " / " << states << "; factory presets without a custom curve: " << clean << " / " << p.presets.getNumFactory() << "\n";
        check (same == states && clean == p.presets.getNumFactory(), "legacy compatibility");
    }

    // ---- round trips: session, A/B, user preset, export, undo ----
    {
        CodexProcessor p;
        p.setPlayConfigDetails (0, 2, sr, block);
        p.presets.loadPreset (0);
        setParam (p, ID::keyMode, 0.0f);
        const auto c1 = bentCurve();
        const auto c2 = CurveSeed::starter (CurveSeed::WarmTube, 5.0f);
        p.getCustomCurves().set (1, &c1, nullptr);
        p.getCustomCurves().set (2, &c2, nullptr);
        setParam (p, ID::clip (1, "Cust"), 1.0f);
        setParam (p, ID::clip (2, "Cust"), 1.0f);
        const auto st = stateOf (p);
        CodexProcessor q;
        q.setStateInformation (st.getData(), (int) st.getSize());
        const bool sessionOk = q.getCustomCurves().fingerprint() == p.getCustomCurves().fingerprint() && q.isCustomEnabled (1) && q.isCustomEnabled (2);
        const auto a = renderState (st), b = renderState (stateOf (q));
        const bool renderOk = maxDiff (a, b) == 0.0f;
        // the custom curve really plays
        const auto offState = editState (st, [] (juce::ValueTree& t) { for (int s = 1; s <= 2; ++s) t.getChildWithProperty ("id", ID::clip (s, "Cust")).setProperty ("value", 0.0f, nullptr); });
        const float audible = relDiffDb (renderState (offState), a);

        // A / B: copyState / replaceState
        auto saved = p.apvts.copyState();
        const auto c3 = CurveSeed::starter (CurveSeed::SCurve, 2.0f);
        p.getCustomCurves().set (1, &c3, nullptr);
        p.apvts.replaceState (saved);
        render (p, 1);
        const bool abOk = p.getCustomCurves().get (1) == c1 && p.getClip (1).activeCustomSlot().curve == c1 && p.getClip (1).isCustomActive();

        // export without a message loop plays the custom curve
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto readFile = [&fm] (const juce::File& f)
        {
            juce::AudioBuffer<float> buf;
            if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f)); r != nullptr)
            {
                buf.setSize (2, (int) r->lengthInSamples);
                r->read (&buf, 0, buf.getNumSamples(), 0, true, true);
            }
            return buf;
        };
        const auto withCurve = readFile (KickExporter::render (p, outDir.getChildFile ("export_custom_curve.wav"), 0.3, sr));
        setParam (p, ID::clip (1, "Cust"), 0.0f); setParam (p, ID::clip (2, "Cust"), 0.0f);
        const auto without = readFile (KickExporter::render (p, outDir.getChildFile ("export_custom_curve_off.wav"), 0.3, sr));
        const float exportDiff = relDiffDb (without, withCurve);
        setParam (p, ID::clip (1, "Cust"), 1.0f); setParam (p, ID::clip (2, "Cust"), 1.0f);

        // user preset: saved and loaded with the curve; the modified marker follows curve edits
        const juce::String name ("zz Codex curve test");
        bool presetOk = p.presets.saveUserPreset (name);
        presetOk &= ! p.presets.isModified();
        p.getCustomCurves().set (1, &c3, nullptr);
        presetOk &= p.presets.isModified();
        p.presets.loadPreset (p.presets.getNumFactory() + p.presets.getUserNames().indexOf (name));
        presetOk &= p.getCustomCurves().get (1) == c1 && p.getCustomCurves().get (2) == c2 && p.isCustomEnabled (1) && ! p.presets.isModified();
        // a preset saved before the custom curves existed: no curve, switch off
        {
            auto xml = juce::parseXML (p.presets.getUserDirectory().getChildFile (name + ".cdxpreset"));
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.removeChild (tree.getChildWithName (ID::curvesNode), nullptr);
            for (int s = 1; s <= 2; ++s) tree.removeChild (tree.getChildWithProperty ("id", ID::clip (s, "Cust")), nullptr);
            tree.createXml()->writeTo (p.presets.getUserDirectory().getChildFile (name + ".cdxpreset"));
            p.presets.refreshUserPresets();
            p.presets.loadPreset (p.presets.getNumFactory() + p.presets.getUserNames().indexOf (name));
            presetOk &= ! p.getCustomCurves().has (1) && ! p.isCustomEnabled (1) && ! p.isCustomEnabled (2);
        }
        p.presets.deleteUserPreset (name);

        // undo: converting and an edit are two steps back to "Type, no curve"; redo brings both back
        p.presets.loadPreset (0);
        p.undoManager.clearUndoHistory();
        p.beginCustomEdit (1, "Draw distortion curve", true);
        const auto seeded = p.getCustomCurves().get (1);
        pump (50);
        p.undoManager.beginNewTransaction ("Move curve point");
        auto moved = seeded;
        moved.move (1, moved.p[1].x, 0.3f);
        p.getCustomCurves().set (1, &moved, &p.undoManager);
        moved.move (1, moved.p[1].x, 0.2f);
        p.getCustomCurves().set (1, &moved, &p.undoManager);
        pump (200);
        render (p, 1);
        juce::String undoSteps;
        bool undoOk = p.getClip (1).isCustomActive() && p.getClip (1).activeCustomSlot().curve == moved;
        undoSteps << (undoOk ? "1" : "0");
        p.undoManager.undo();
        pump (50);
        undoOk &= p.getCustomCurves().get (1) == seeded && p.isCustomEnabled (1);
        undoSteps << (undoOk ? "1" : "0");
        p.undoManager.undo();
        pump (50);
        render (p, 1);
        undoOk &= ! p.getCustomCurves().has (1) && ! p.isCustomEnabled (1) && ! p.getClip (1).isCustomActive();
        undoSteps << (undoOk ? "1" : "0");
        p.undoManager.redo(); p.undoManager.redo();
        pump (50);
        render (p, 1);
        undoOk &= p.getCustomCurves().get (1) == moved && p.isCustomEnabled (1) && p.getClip (1).isCustomActive();
        undoSteps << (undoOk ? "1" : "0");
        if (! undoOk) std::cout << "   undo steps " << undoSteps << " has " << (int) p.getCustomCurves().has (1) << " on " << (int) p.isCustomEnabled (1)
                                << " active " << (int) p.getClip (1).isCustomActive() << "\n";

        std::cout << "Custom curves: session round trip " << (sessionOk && renderOk ? "exact" : "FAILED") << " (curve audible: "
                  << juce::String (audible, 1) << " dB vs Type); A/B " << (abOk ? "OK" : "FAILED") << "; export plays the curve ("
                  << juce::String (exportDiff, 1) << " dB vs off); user preset + modified marker + old preset " << (presetOk ? "OK" : "FAILED")
                  << "; undo 2 steps / redo " << (undoOk ? "OK" : "FAILED") << "\n";
        check (sessionOk && renderOk && audible > -30.0f, "session round trip");
        check (abOk, "A/B");
        check (exportDiff > -30.0f, "export with custom curve");
        check (presetOk, "user preset");
        check (undoOk, "undo / redo");
    }
    return bad;
}

//==============================================================================
/** Audio thread: no allocation while curves are published from another thread; crossfades remove the clicks. */
static int customCurveRealtimeTests()
{
    using namespace curvetest;
    int bad = 0;
    ClipDistortion::Settings st;
    st.mode = ClipDistortion::Tanh; st.driveDb = 6.0f; st.oversampling = 1; st.dcFilter = false; st.outDb = 0.0f; st.custom = true;
    {
        ClipDistortion d;
        d.prepare (sr, 256);
        const auto first = CurveSeed::starter (CurveSeed::SoftKnee, 3.0f);
        d.publishCustomCurve (&first);
        d.setSettings (st);
        std::array<const double*, 4> data {};
        for (int i = 0; i < 4; ++i) data[(size_t) i] = d.customSlotForTesting (i).tbl.F.data();
        std::atomic<bool> stop { false };
        std::thread writer ([&]
        {
            juce::Random rng (9);
            for (int k = 0; k < 2000 && ! stop.load(); ++k)
            {
                FreeCurve c;
                c.symmetric = rng.nextBool();
                c.reach = 1.0f + rng.nextFloat() * 8.0f;
                c.count = 2 + rng.nextInt (20);
                for (int i = 0; i < c.count; ++i)
                    c.p[(size_t) i] = { rng.nextFloat() * 2.0f - 1.0f, rng.nextFloat() * 2.0f - 1.0f, rng.nextFloat() * 2.0f - 1.0f, rng.nextBool() };
                c.sanitise();
                d.publishCustomCurve (rng.nextInt (10) == 0 ? nullptr : &c);
            }
        });
        juce::AudioBuffer<float> buf (2, 256);
        bool finite = true;
        long allocs = 0;
        juce::Random rng (4);
        for (int b = 0; b < 500; ++b)
        {
            for (int i = 0; i < 256; ++i) { const float v = 0.8f * std::sin (0.01f * (float) (b * 256 + i)); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            st.custom = rng.nextInt (8) != 0;
            allocCount.store (0);
            countAllocs = true;
            d.setSettings (st);
            d.process (buf.getWritePointer (0), buf.getWritePointer (1), 256);
            countAllocs = false;
            allocs += allocCount.load();
            for (int i = 0; i < 256; ++i) finite &= std::isfinite (buf.getSample (0, i)) && std::abs (buf.getSample (0, i)) < 8.0f;
        }
        stop.store (true);
        writer.join();
        bool sameStorage = true;
        for (int i = 0; i < 4; ++i) sameStorage &= d.customSlotForTesting (i).tbl.F.data() == data[(size_t) i] && (int) d.customSlotForTesting (i).tbl.F.size() == 8192;
        std::cout << "\nCustom curves: 500 audio blocks while another thread published 2000 curves: " << allocs << " allocations on the audio thread, output "
                  << (finite ? "finite" : "NOT FINITE") << ", tables " << (sameStorage ? "never reallocated" : "REALLOCATED") << "\n";
        if (allocs != 0 || ! finite || ! sameStorage) ++bad;
    }

    // ---- crossfade: edits while playing do not click ----
    {
        st.oversampling = 0; st.driveDb = 0.0f; st.custom = true; st.mode = ClipDistortion::Tanh;
        const auto wall = CurveSeed::starter (CurveSeed::HardWall, 1.0f);
        const auto fold = CurveSeed::starter (CurveSeed::DoubleFold, 1.0f);
        const int n = 48000, blk = 64, change = 24000 + 160;   // the switch lands near a peak of the 50 Hz input
        auto run = [&] (const FreeCurve* a, const FreeCurve* b, bool toggle, float fadeMs)
        {
            ClipDistortion d;
            d.prepare (sr, blk);
            d.setCustomFadeMs (fadeMs);
            auto s = st;
            s.custom = ! toggle;
            d.publishCustomCurve (a);
            d.setSettings (s);
            juce::AudioBuffer<float> out (2, n);
            for (int i = 0; i < n; ++i) { const float v = 0.9f * std::sin (juce::MathConstants<float>::twoPi * 50.0f * (float) i / (float) sr); out.setSample (0, i, v); out.setSample (1, i, v); }
            for (int pos = 0; pos < n; pos += blk)
            {
                if (pos == change - change % blk)
                {
                    if (toggle) s.custom = true; else d.publishCustomCurve (b);
                }
                d.setSettings (s);
                d.process (out.getWritePointer (0, pos), out.getWritePointer (1, pos), blk);
            }
            return out;
        };
        auto jump = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            float m = 0.0f;
            for (int i = std::max (1, from); i < to; ++i) m = std::max (m, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
            return m;
        };
        const int at = change - change % blk, win = (int) (0.003 * sr);
        const float steadyWall = jump (run (&wall, &wall, false, 5.0f), 2000, n), steadyFold = jump (run (&fold, &fold, false, 5.0f), 2000, n);
        const float ref = std::max (steadyWall, steadyFold);
        const float swap = jump (run (&wall, &fold, false, 5.0f), at, at + win);
        const float swapHard = jump (run (&wall, &fold, false, 0.0f), at, at + win);
        // Custom Curve switched on mid-note (Type: Tanh at 0 dB -> a fold)
        auto tanhRef = st; tanhRef.custom = false;
        const float steadyTanh = [&] { ClipDistortion d; d.prepare (sr, blk); d.setSettings (tanhRef); juce::AudioBuffer<float> o (2, n);
                                       for (int i = 0; i < n; ++i) { const float v = 0.9f * std::sin (juce::MathConstants<float>::twoPi * 50.0f * (float) i / (float) sr); o.setSample (0, i, v); o.setSample (1, i, v); }
                                       for (int pos = 0; pos < n; pos += blk) { d.setSettings (tanhRef); d.process (o.getWritePointer (0, pos), o.getWritePointer (1, pos), blk); }
                                       return jump (o, 2000, n); }();
        const float toggle = jump (run (&fold, &fold, true, 5.0f), at, at + win);
        const float toggleHard = jump (run (&fold, &fold, true, 0.0f), at, at + win);
        const bool swapOk = swap <= 1.25f * ref + 1.0e-3f, toggleOk = toggle <= 1.25f * std::max (steadyFold, steadyTanh) + 1.0e-3f;
        const bool detects = swapHard > 1.25f * ref + 1.0e-3f && toggleHard > 1.25f * std::max (steadyFold, steadyTanh) + 1.0e-3f;
        std::cout << "Custom curves: largest sample step after a curve swap mid-note " << juce::String (swap, 4) << " (steady " << juce::String (ref, 4)
                  << ", without the crossfade " << juce::String (swapHard, 4) << "); Custom Curve switched on mid-note " << juce::String (toggle, 4)
                  << " (without " << juce::String (toggleHard, 4) << "): " << (swapOk && toggleOk && detects ? "no clicks" : "FAILED") << "\n";
        if (! (swapOk && toggleOk && detects)) ++bad;
    }
    return bad;
}

//==============================================================================
/** Click audit additions (CODEX_CLICK_AUDIT): custom curve changes mid-note in the full plug-in. */
static int customCurveClickAudit (CodexProcessor& proc, const std::function<void()>& setup,
                                  const std::function<float (const juce::AudioBuffer<float>&, int, int)>& curvature)
{
    int clicky = 0;
    const auto wall = CurveSeed::starter (CurveSeed::HardWall, 4.0f), fold = CurveSeed::starter (CurveSeed::DoubleFold, 4.0f);
    struct Case { const char* name; std::function<void()> before, at; };
    const Case cases[] = {
        { "custom curve swap (Hard Wall -> Double Fold)",
          [&] { proc.getCustomCurves().set (1, &wall, nullptr); setParam (proc, ID::clip (1, "Cust"), 1.0f); },
          [&] { proc.getCustomCurves().set (1, &fold, nullptr); } },
        { "Custom Curve switched on (Double Fold)",
          [&] { proc.getCustomCurves().set (1, &fold, nullptr); setParam (proc, ID::clip (1, "Cust"), 0.0f); },
          [&] { setParam (proc, ID::clip (1, "Cust"), 1.0f); } },
        { "Custom Curve switched off",
          [&] { proc.getCustomCurves().set (1, &fold, nullptr); setParam (proc, ID::clip (1, "Cust"), 1.0f); },
          [&] { setParam (proc, ID::clip (1, "Cust"), 0.0f); } },
        { "X-Shaper points moved while the custom curve plays",
          [&] { setParam (proc, ID::clip (1, "Mode"), (float) ClipDistortion::XShaper); proc.getCustomCurves().set (1, &wall, nullptr); setParam (proc, ID::clip (1, "Cust"), 1.0f); },
          [&] { for (int k = 0; k < 8; ++k) setParam (proc, ID::clip (1, ("XP" + juce::String (k + 1)).toRawUTF8()), 0.2f); } },
    };
    for (const auto& c : cases)
    {
        setup();
        c.before();
        proc.prepareToPlay (sr, block);
        const int total = (int) (1.2 * sr) / block * block, change = (int) (0.6 * sr) / block * block;
        juce::AudioBuffer<float> out (2, total), buf (2, block);
        for (int pos = 0; pos < total; pos += block)
        {
            juce::MidiBuffer midi;
            if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            if (pos == change) c.at();
            buf.clear();
            proc.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, block);
        }
        const float before = curvature (out, change - (int) (0.15 * sr), change);
        const float after = curvature (out, change + (int) (0.25 * sr), change + (int) (0.45 * sr));
        const float around = curvature (out, change, change + (int) (0.03 * sr));
        const float ref = std::max (before, after);
        const bool click = around > 3.0f * ref + 1.0e-4f;
        std::cout << "   " << juce::String (c.name).paddedRight (' ', 52) << " x" << juce::String (around / juce::jmax (1.0e-6f, ref), 2)
                  << (click ? "  CLICK" : "") << "\n";
        clicky += click ? 1 : 0;
        proc.getCustomCurves().clearAll (nullptr);
        setParam (proc, ID::clip (1, "Cust"), 0.0f);
    }
    return clicky;
}

//==============================================================================
/** The editor with the mouse: add, drag, bend, delete, pencil, menus, Type pick; the keyboard is never taken. */
static int customCurveGuiTest (CodexProcessor& proc)
{
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };
    juce::MemoryBlock savedState;
    proc.getStateInformation (savedState);
    juce::SharedResourcePointer<CodexSettings> codexSettings;
    auto* userSettings = codexSettings->user();
    const bool hadShortcutSetting = userSettings != nullptr && userSettings->containsKey ("keyboardShortcuts");
    const bool oldShortcutSetting = userSettings != nullptr && userSettings->getBoolValue ("keyboardShortcuts", false);
    if (userSettings != nullptr) userSettings->setValue ("keyboardShortcuts", false);   // the plug-in's default
    proc.presets.loadPreset (0);   // Init: Distortion 1 = Tanh
    proc.undoManager.clearUndoHistory();

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    auto* hk = dynamic_cast<CodexEditor*> (editor.get());
    hk->setSize (MainView::baseWidth, MainView::baseHeight);
    juce::DocumentWindow window ("Codex curve test", juce::Colours::black, juce::DocumentWindow::closeButton, true);
    window.setUsingNativeTitleBar (true);
    window.setContentNonOwned (editor.get(), true);
    window.setVisible (true);
    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    hk->showTab (1);
    pump (150);
    auto* peer = window.getPeer();
    auto* top = peer->getComponent().getTopLevelComponent();
    TransferDisplay* td = nullptr;
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* t = dynamic_cast<TransferDisplay*> (ch); t != nullptr && t->getStage() == 1) td = t;
            find (*ch);
        }
    };
    find (hk->getView());
    if (td == nullptr) { std::cout << "\nCustom curve editor: display not found FAILED\n"; return 1; }
    auto& logic = td->logic();
    auto& store = proc.getCustomCurves();
    juce::int64 clock = juce::Time::currentTimeMillis();
    auto toPeer = [&] (juce::Point<float> p) { return peer->getComponent().getLocalPoint (td, p); };
    auto sendAt = [&] (juce::Point<float> localPos, juce::ModifierKeys mods, int dtMs)
    {
        clock += dtMs;
        peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, toPeer (localPos), mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, clock);
    };
    const auto left = juce::ModifierKeys::leftButtonModifier;
    auto click = [&] (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        sendAt (p, {}, 700); sendAt (p, mods.withFlags (juce::ModifierKeys::leftButtonModifier), 20); sendAt (p, {}, 60);
        pump (20);
    };
    auto doubleClick = [&] (juce::Point<float> p)
    {
        sendAt (p, {}, 700); sendAt (p, left, 20); sendAt (p, {}, 40); sendAt (p, left, 60); sendAt (p, {}, 40);
        pump (40);
    };
    auto drag = [&] (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
    {
        sendAt (from, {}, 700);
        sendAt (from, mods.withFlags (juce::ModifierKeys::leftButtonModifier), 20);
        for (int k = 1; k <= 12; ++k) sendAt (from + (to - from) * ((float) k / 12.0f), mods.withFlags (juce::ModifierKeys::leftButtonModifier), 16);
        sendAt (to, {}, 30);
        pump (40);
    };
    auto focused = [&] { auto* f = juce::Component::getCurrentlyFocusedComponent(); return f != nullptr && (f == top || top->isParentOf (f)); };
    int focusHits = 0;
    juce::StringArray focusLog;
    auto noFocus = [&]
    {
        if (! focused()) return;
        ++focusHits;
        auto* f = juce::Component::getCurrentlyFocusedComponent();
        focusLog.add ("after step " + juce::String (focusHits) + ": " + typeid (*f).name());
        f->giveAwayKeyboardFocus();
    };
    auto nearestPoint = [&] (float u, float y)
    {
        int best = -1; float bd = 1.0e9f;
        const auto& c = logic.getCurve();
        for (int i = 0; i < c.count; ++i)
            if (const float d = std::hypot (c.p[(size_t) i].x - u, c.p[(size_t) i].y - y); d < bd) { bd = d; best = i; }
        return std::make_pair (best, bd);
    };
    juce::StringArray log;
    struct NoticeCounter : juce::AudioProcessorListener
    {
        int n = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override { if (d.nonParameterStateChanged) ++n; }
    } notices;
    proc.addListener (&notices);

    // 1. a click on the Type's curve changes nothing
    const auto centre = td->plotToScreen (0.0f, 0.0f);
    click (centre);
    pump (40);
    check (! store.has (1) && ! proc.isCustomEnabled (1), "single click changes nothing");
    noFocus();

    // 1b. converting with a gesture drawn ON the Type's curve (Custom off: the display's x is the input before Drive)
    //     changes only what was drawn: a double-click on the curve adds a point on the seed, a pencil stroke along the
    //     curve leaves the seed as it was
    const auto seed = proc.seedCustomCurve (1);
    {
        FreeCurveShape seedShape;
        seedShape.compile (seed);
        auto diffToSeed = [&]
        {
            float d = 0.0f;
            for (int k = 0; k <= 400; ++k) { const float u = -1.0f + 2.0f * (float) k / 400.0f; d = std::max (d, std::abs (logic.getShape().evalU (u) - seedShape.evalU (u))); }
            return d;
        };
        auto onType = [&] (float x) { return td->plotToScreen (x, logic.legacyCurveY (x)); };
        const float g = logic.legacyGain(), off = logic.legacyOffset();
        doubleClick (onType (0.2f));
        pump (40);
        const float dClick = diffToSeed();
        const bool clickOk = store.has (1) && proc.isCustomEnabled (1) && logic.getCurve().count == seed.count + 1;
        const float clickU = logic.getCurve().p[(size_t) juce::jmax (0, nearestPoint ((0.2f * g + off) / seed.reach, 2.0f).first)].x;
        proc.undoManager.undo(); pump (60);
        check (clickOk && dClick < 0.01f && ! store.has (1) && ! proc.isCustomEnabled (1), "double-click on the Type's curve converts without a dent");
        log.add ("double-click on the Type's curve (drive x" + juce::String (g, 2) + ", range " + juce::String (seed.reach, 2) + "): point at input "
                 + juce::String (clickU, 3) + ", curve vs seed max " + juce::String (dClick, 4));

        auto& pencil = td->getPencilButton();
        click (td->getLocalPoint (&pencil, pencil.getLocalBounds().toFloat().getCentre()));
        sendAt (onType (0.12f), {}, 700);
        sendAt (onType (0.12f), left, 20);
        for (int k = 1; k <= 60; ++k) sendAt (onType (0.12f + 0.33f * (float) k / 60.0f), left, 8);
        sendAt (onType (0.45f), {}, 20);
        pump (40);
        const float dPencil = diffToSeed();
        const bool pencilOk = store.has (1) && proc.isCustomEnabled (1);
        click (td->getLocalPoint (&pencil, pencil.getLocalBounds().toFloat().getCentre()));
        proc.undoManager.undo(); pump (60);
        check (pencilOk && dPencil < 0.02f && ! logic.pencil && ! store.has (1) && ! proc.isCustomEnabled (1), "pencil stroke along the Type's curve converts without a jump");
        log.add ("pencil along the Type's curve: curve vs seed max " + juce::String (dPencil, 4));
        noFocus();
    }

    // 2. double-click above the curve: converts (seeded) and adds a point there (at the input the Type's display showed)
    const int seedCount = seed.count;
    const float addU = logic.legacyToU (0.6f, seed.reach);
    const auto target = td->plotToScreen (0.6f, 0.6f);
    doubleClick (target);
    pump (40);
    const auto [added, addedDist] = nearestPoint (addU, 0.6f);
    check (store.has (1) && proc.isCustomEnabled (1) && logic.getCurve().count == seedCount + 1 && addedDist < 0.03f, "double-click adds a point (converts)");
    log.add ("double-click: seed " + juce::String (seedCount) + " points + 1, point at " + juce::String (logic.getCurve().p[(size_t) juce::jmax (0, added)].x, 3)
             + ", " + juce::String (logic.getCurve().p[(size_t) juce::jmax (0, added)].y, 3));
    noFocus();

    // 3. one undo step back to the Type, redo
    proc.undoManager.undo(); pump (60);
    const bool undone = ! store.has (1) && ! proc.isCustomEnabled (1);
    proc.undoManager.redo(); pump (60);
    check (undone && store.has (1) && proc.isCustomEnabled (1) && logic.getCurve().count == seedCount + 1, "undo / redo of the conversion");

    // 4. drag the point (+10, -20) px; past its right neighbour it stops at the neighbour
    float trackU = addU, trackY = 0.6f;
    {
        const auto [i, dist] = nearestPoint (addU, 0.6f);
        juce::ignoreUnused (dist);
        const auto before = logic.getCurve().p[(size_t) i];
        const auto from = td->plotToScreen (before.x, before.y);
        drag (from, from + juce::Point<float> (10.0f, -20.0f));
        const auto after = logic.getCurve().p[(size_t) i];
        const float ppu = logic.pixelsPerUnit();
        const bool moved = std::abs ((after.x - before.x) - 10.0f / ppu) < 0.01f && std::abs ((after.y - before.y) - 20.0f / ppu) < 0.01f;
        const float nextX = logic.getCurve().p[(size_t) i + 1].x;
        const auto from2 = td->plotToScreen (after.x, after.y);
        drag (from2, from2 + juce::Point<float> (300.0f, 0.0f));
        const auto& c = logic.getCurve();
        bool ordered = true;
        for (int k = 1; k < c.count; ++k) ordered &= c.p[(size_t) k].x > c.p[(size_t) k - 1].x;
        const bool clamped = c.count == seedCount + 1 && c.p[(size_t) i].x < nextX && c.p[(size_t) i].x > nextX - 0.01f;
        log.add ("drag: moved by " + juce::String ((after.x - before.x) * ppu, 1) + ", " + juce::String ((after.y - before.y) * ppu, 1) + " px, clamped at the neighbour: "
                 + (clamped ? "yes" : "no"));
        check (moved && ordered && clamped, "point drag");
        noFocus();
        // back to where the first drag left it
        proc.undoManager.undo(); pump (40);
        trackU = logic.getCurve().p[(size_t) i].x;
        trackY = logic.getCurve().p[(size_t) i].y;
    }

    // 5. drag a diamond down 30 px: the bend follows the envelope editor's rule
    {
        const auto& c = logic.getCurve();
        int seg = -1;
        for (int k = 0; k < c.count - 1 && seg < 0; ++k)
            if ((c.p[(size_t) k + 1].x - c.p[(size_t) k].x) * logic.pixelsPerUnit() > 30.0f && std::abs (c.p[(size_t) k + 1].y - c.p[(size_t) k].y) > 0.05f) seg = k;
        if (seg >= 0)
        {
            const float b0 = c.p[(size_t) seg].bend;
            const bool rising = c.p[(size_t) seg + 1].y > c.p[(size_t) seg].y;
            const auto from = logic.diamondPos (seg, false);
            drag (from, from + juce::Point<float> (0.0f, 30.0f));
            const float b1 = logic.getCurve().p[(size_t) seg].bend;
            const bool ok = rising ? b1 < b0 - 0.1f : b1 > b0 + 0.1f;
            log.add ("diamond: bend " + juce::String (b0, 2) + " -> " + juce::String (b1, 2) + (rising ? " (rising segment)" : " (falling segment)"));
            check (ok, "diamond bend");
        }
        else check (false, "no diamond to bend");
        noFocus();
    }

    // 6. double-click the point: deleted
    {
        const auto [i, dist] = nearestPoint (trackU, trackY);
        const int before = logic.getCurve().count;
        doubleClick (td->plotToScreen (logic.getCurve().p[(size_t) i].x, logic.getCurve().p[(size_t) i].y));
        check (dist < 0.05f && logic.getCurve().count == before - 1, "double-click deletes a point");
        noFocus();
    }

    // 7. pencil: a stroke over u 0.2 .. 0.6 at y 0.5, one undo step
    {
        auto& pencil = td->getPencilButton();
        click (td->getLocalPoint (&pencil, pencil.getLocalBounds().toFloat().getCentre()));
        const bool on = logic.pencil;
        const auto textBefore = store.text (1);
        const auto a = td->plotToScreen (0.2f, 0.5f), b = td->plotToScreen (0.6f, 0.5f);
        sendAt (a, {}, 700);
        sendAt (a, left, 20);
        for (int k = 1; k <= 60; ++k) sendAt (a + (b - a) * ((float) k / 60.0f), left, 8);
        sendAt (b, {}, 20);
        pump (40);
        const float y = logic.getShape().evalU (0.4f);
        const auto textAfter = store.text (1);
        proc.undoManager.undo(); pump (40);
        const bool oneStep = store.text (1) == textBefore;
        proc.undoManager.redo(); pump (40);
        log.add ("pencil: curve at 0.4 = " + juce::String (y, 3) + ", undo restores the curve before the stroke: " + (oneStep ? "yes" : "no"));
        check (on && std::abs (y - 0.5f) < 0.03f && oneStep && store.text (1) == textAfter, "pencil stroke");
        click (td->getLocalPoint (&pencil, pencil.getLocalBounds().toFloat().getCentre()));
        check (! logic.pencil, "pencil off");
        noFocus();
    }

    // 8. right-click on empty space opens the curve menu
    {
        const auto p = td->plotToScreen (-0.6f, 0.7f);
        sendAt (p, {}, 700);
        sendAt (p, juce::ModifierKeys::rightButtonModifier, 20);
        sendAt (p, {}, 40);
        pump (80);
        check (juce::PopupMenu::dismissAllActiveMenus(), "right-click menu");
        pump (40);
        noFocus();
    }

    // 9. a starter shape, then Asymmetric: the same curve
    {
        logic.applyStarter (CurveSeed::Staircase);
        const auto sym = logic.getCurve();
        logic.setSymmetric (false);
        const float d = curvetest::curveDiff (sym, logic.getCurve());
        check (sym.symmetric && ! logic.getCurve().symmetric && d < 1.0e-5f, "starter + asymmetric");
        log.add ("Staircase starter -> Asymmetric: curve unchanged within " + juce::String (d, 7));
    }

    // 10. picking a Type turns the curve off; loading a preset with a curve keeps it on
    {
        ArmedComboBox* box = nullptr;
        std::function<void (juce::Component&)> findBox = [&] (juce::Component& c)
        {
            for (auto* ch : c.getChildren()) { if (auto* b = dynamic_cast<ArmedComboBox*> (ch)) box = b; if (box == nullptr) findBox (*ch); }
        };
        findBox (*td->getParentComponent());
        const juce::String name ("zz Codex curve gui test");
        proc.presets.saveUserPreset (name);
        bool ok = box != nullptr;
        if (box != nullptr)
        {
            box->arm();
            box->setSelectedId (ClipDistortion::SoftClip + 1, juce::sendNotificationAsync);
            pump (120);
            ok &= ! proc.isCustomEnabled (1) && store.has (1);
            proc.presets.loadPreset (proc.presets.getNumFactory() + proc.presets.getUserNames().indexOf (name));
            pump (500);
            ok &= proc.isCustomEnabled (1) && store.has (1) && logic.isActive();
        }
        proc.presets.deleteUserPreset (name);
        check (ok, "Type pick turns the curve off, preset load keeps it");
        noFocus();
    }

    // 11. the host hears about curve edits (they are not parameters): one notice per drag, one per undo, none for a
    //     session the host restores itself
    {
        pump (400);
        const auto& c = logic.getCurve();
        const int i = juce::jmax (1, c.count / 2);
        const auto from = td->plotToScreen (c.p[(size_t) i].x, c.p[(size_t) i].y);
        notices.n = 0;
        const auto textBefore = store.text (1);
        drag (from, from + juce::Point<float> (0.0f, 12.0f));
        pump (400);
        const int perDrag = notices.n;
        bool changed = store.text (1) != textBefore;
        juce::MemoryBlock dragged;
        proc.getStateInformation (dragged);
        notices.n = 0;
        proc.undoManager.undo(); pump (400);
        const int perUndo = notices.n;
        notices.n = 0;
        proc.setStateInformation (dragged.getData(), (int) dragged.getSize());   // a different curve, set by the host
        pump (400);
        changed &= store.text (1) != textBefore;
        const int perRestore = notices.n;
        log.add ("host notices (project modified): drag " + juce::String (perDrag) + ", undo " + juce::String (perUndo) + ", restored session "
                 + juce::String (perRestore));
        check (changed && perDrag == 1 && perUndo == 1 && perRestore == 0, "host notified of curve edits");
        noFocus();
    }

    check (focusHits == 0, "keyboard focus taken");
    std::cout << "\nCustom curve editor (mouse): " << (bad == 0 ? "OK" : juce::String (bad) + " FAILED") << ", keyboard focus never taken: "
              << (focusHits == 0 ? "yes" : "NO") << "\n";
    for (auto& l : log) std::cout << "   " << l << "\n";
    for (auto& l : focusLog) std::cout << "   keyboard focus " << l << "\n";
    proc.removeListener (&notices);

    window.clearContentComponent();
    window.setVisible (false);
    editor.reset();
    pump (50);
    if (userSettings != nullptr)
    {
        if (hadShortcutSetting) userSettings->setValue ("keyboardShortcuts", oldShortcutSetting);
        else userSettings->removeValue ("keyboardShortcuts");
    }
    proc.setStateInformation (savedState.getData(), (int) savedState.getSize());
    return bad;
}

/** A custom curve on each unit for screenshots: unit 1 symmetric with bends, unit 2 an asymmetric starter. */
static void setupCustomCurvesForScreenshots (CodexProcessor& proc)
{
    auto c1 = proc.seedCustomCurve (1);
    const int i = c1.insert (0.35f, 0.92f, false);
    if (i >= 0) c1.setBend (i, 0.55f);
    if (c1.count > 3) c1.setBend (1, -0.4f);
    proc.getCustomCurves().set (1, &c1, nullptr);
    setParam (proc, ID::clip (1, "Cust"), 1.0f);
    auto c2 = CurveSeed::starter (CurveSeed::WarmTube, juce::jmax (2.0f, proc.seedCustomCurve (2).reach));
    c2.setBend (1, 0.5f);
    c2.setSharp (3, true);
    proc.getCustomCurves().set (2, &c2, nullptr);
    setParam (proc, ID::clip (2, "Cust"), 1.0f);
}

/** Hover on a point of unit 1's curve (screenshots have no mouse). */
static void hoverCustomPoint (juce::Component& editorComp)
{
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* t = dynamic_cast<TransferDisplay*> (ch); t != nullptr && t->getStage() == 1 && t->logic().isActive())
            {
                t->resized();
                const auto& cv = t->logic().getCurve();
                int best = 1;
                for (int k = 1; k < cv.count - 1; ++k) if (cv.p[(size_t) k].bend > cv.p[(size_t) best].bend) best = k;
                t->logic().setHoverForTesting ({ CurveEditLogic::Handle::Point, juce::jmin (best, cv.count - 1), false });
            }
            find (*ch);
        }
    };
    find (editorComp);
}
