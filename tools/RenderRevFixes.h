// Space reverb regression checks for CodexRender (included by Render.cpp after its helpers):
//  - decay time per band follows the design (the late lines' fractional reads are all-pass, not linear: no loss
//    inside the feedback loop), at 48 and 96 kHz, decay 1 / 5 / 20 s
//  - the modulated all-pass read stays smooth (error vs the ideal moving delay)
//  - Bass Xover above High Xover no longer lifts the loop gain (decay stays at the set value)
//  - the dry / wet glides land exactly on their targets
//  - stable at the parameter extremes at every sample rate
#pragma once
#include <complex>

namespace revfix
{
    constexpr double pi = juce::MathConstants<double>::pi;

    /** Runs the reverb over a stereo buffer in place, in 512-sample blocks. */
    inline void run (space::SpaceReverb& rv, juce::AudioBuffer<float>& b, int blockSize = 512)
    {
        for (int p = 0; p < b.getNumSamples(); p += blockSize)
            rv.process (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (blockSize, b.getNumSamples() - p));
    }

    /** Settings that leave only the FDN's decay in the measurement: late field only, no ducking, wide-open cuts,
        no predelay, flat decay EQ, and no input diffusion / early send (those all-passes sit before the loop and
        smear the injection over ~0.2 s, which reads as a longer decay at Decay 1 s). */
    inline space::Settings lateOnly (int mode, float decay)
    {
        space::Settings s;
        s.enabled = true; s.mode = mode; s.mix = 1.0f; s.depth = 1.0f; s.duck = 0.0f; s.predelayMs = 0.0f;
        s.lowCut = 20.0f; s.highCut = 20000.0f; s.decay = decay;
        s.bassMult = 1.0f; s.highMult = 1.0f; s.bassXover = 250.0f; s.highXover = 4000.0f;
        s.lDiffusion = 0.0f; s.eSend = 0.0f;
        return s;
    }

    /** T60 from the Schroeder energy decay curve (-5 .. -25 dB, line fit) of the impulse response band-passed
        around f (two cascaded RBJ band-passes, Q 2, in double), both channels summed in energy. */
    inline double schroederT60 (const juce::AudioBuffer<float>& b, double rate, double f)
    {
        const double w0 = 2.0 * pi * f / rate, alpha = std::sin (w0) / (2.0 * 2.0);
        const double a0 = 1.0 + alpha;
        const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
        const int n = b.getNumSamples();
        std::vector<double> e ((size_t) n, 0.0);
        for (int c = 0; c < 2; ++c)
        {
            double s[2][2] = { { 0, 0 }, { 0, 0 } };
            for (int i = 0; i < n; ++i)
            {
                double x = b.getSample (c, i);
                for (auto& st : s)
                {
                    const double y = b0 * x + st[0];
                    st[0] = -a1 * y + st[1];
                    st[1] = b2 * x - a2 * y;
                    x = y;
                }
                e[(size_t) i] += x * x;
            }
        }
        std::vector<double> edc ((size_t) n + 1, 0.0);
        for (int i = n; i-- > 0;) edc[(size_t) i] = edc[(size_t) i + 1] + e[(size_t) i];
        if (edc[0] <= 0.0) return 0.0;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int cnt = 0;
        for (int i = 0; i < n; i += 16)
        {
            const double d = 10.0 * std::log10 (std::max (edc[(size_t) i] / edc[0], 1e-300));
            if (d > -5.0 || d < -25.0) continue;
            const double t = i / rate;
            sx += t; sy += d; sxx += t * t; sxy += t * d; ++cnt;
        }
        if (cnt < 3) return 0.0;
        return -60.0 / ((cnt * sxy - sx * sy) / (cnt * sxx - sx * sx));
    }

    /** Impulse response of the reverb, long enough for the -25 dB fit point plus a 25 dB integration margin.
        Half a second of silence first, so the mix / depth glides of a fresh instance have settled (the dry impulse
        would otherwise sit at the start of the energy decay curve). */
    inline juce::AudioBuffer<float> impulseResponse (const space::Settings& s, double rate, double maxT60)
    {
        space::SpaceReverb rv;
        rv.prepare (rate);
        rv.setSettings (s);
        {
            juce::AudioBuffer<float> settle (2, (int) (0.5 * rate));
            settle.clear();
            run (rv, settle);
        }
        juce::AudioBuffer<float> b (2, (int) ((maxT60 * 50.0 / 60.0 + 0.5) * rate));
        b.clear();
        b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f);
        run (rv, b);
        return b;
    }

    /** The decay the reverb's own design implies at f: per line, the 3-band gain |gB LP_b + gM (LP_h - LP_b) + gH HP_h|
        (TPT one-poles, gains from the mean loop delay), averaged as dB lost per second over all lines. */
    inline double designT60 (const space::Settings& s, double rate, double f)
    {
        static const double baseMs[8] = { 29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.7, 73.1 };
        const auto shape = space::modeShape (s.mode);
        const double fs = rate, lateScale = shape.lateScale * (0.35 + 1.65 * s.lSize);
        const double lDepthMean = s.lModDepth * 0.0006 * fs * shape.modScale;
        const double t60 = juce::jlimit (0.1, 20.0, (double) s.decay);
        auto onePoleLP = [&] (double hz)
        {
            const double g = std::tan (pi * juce::jlimit (5.0, fs * 0.49, hz) / fs);
            const std::complex<double> z1 = std::polar (1.0, -2.0 * pi * f / fs);
            return g * (1.0 + z1) / ((1.0 + g) - (1.0 - g) * z1);
        };
        const auto lb = onePoleLP (std::min (s.bassXover, s.highXover)), lt = onePoleLP (s.highXover);
        double sumL = 0.0, sumDb = 0.0;
        for (double ms : baseMs)
        {
            const double L = (ms * lateScale * 0.001 * fs + lDepthMean) / fs;
            auto g = [L] (double t) { return std::min (0.99995, std::pow (10.0, -3.0 * L / std::max (0.05, t))); };
            const double gB = g (t60 * s.bassMult), gM = g (t60), gH = g (t60 * s.highMult * shape.damping);
            sumL += L;
            sumDb += -20.0 * std::log10 (std::abs (gB * lb + gM * (lt - lb) + gH * (1.0 - lt)));
        }
        return 60.0 * sumL / sumDb;
    }
}

static int reverbFixTests()
{
    using namespace revfix;
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };

    // ---- 1. Decay time per band = the design's (all-pass fractional reads in the loop: no interpolation loss) ----
    {
        std::cout << "\nSpace decay per band (impulse response, Schroeder -5..-25 dB) vs design; tolerance +-5 %\n"
                     "   mode        rate   decay |   500 Hz          1 kHz           2 kHz           4 kHz\n";
        const double bands[] = { 500.0, 1000.0, 2000.0, 4000.0 };
        double worst = 0.0;
        for (int mode : { 1, 4 })   // Large Room (flat design), Plate (damping 1.15: highs decay longer)
            for (double rate : { 48000.0, 96000.0 })
                for (float decay : { 1.0f, 5.0f, 20.0f })
                {
                    const auto s = lateOnly (mode, decay);
                    double maxT = 0.0;
                    for (double f : bands) maxT = std::max (maxT, designT60 (s, rate, f));
                    const auto ir = impulseResponse (s, rate, maxT);
                    std::cout << "   " << space::modeNames()[mode].paddedRight (' ', 11) << juce::String ((int) rate).paddedLeft (' ', 6)
                              << juce::String (decay, 0).paddedLeft (' ', 6) << "  |";
                    for (double f : bands)
                    {
                        const double want = designT60 (s, rate, f), got = schroederT60 (ir, rate, f), err = got / want - 1.0;
                        worst = std::max (worst, std::abs (err));
                        std::cout << "  " << juce::String (got, 2).paddedLeft (' ', 6) << "/" << juce::String (want, 2).paddedRight (' ', 5)
                                  << juce::String (err * 100.0, 1).paddedLeft (' ', 5) << "%";
                        check (std::abs (err) <= 0.05, space::modeNames()[mode] + " " + juce::String (rate) + " Hz decay " + juce::String (decay)
                                                        + " at " + juce::String (f) + " Hz: T60 " + juce::String (got, 3) + " s vs " + juce::String (want, 3));
                    }
                    std::cout << "\n";
                }
        std::cout << "   worst deviation " << juce::String (worst * 100.0, 2) << " %\n";
    }

    // ---- 2. The modulated all-pass read is smooth: error vs the ideal moving delay (max modulation: ~46 samples
    //         of sweep at 8 Hz, Warehouse at full Late Mod Depth / Rate) ----
    {
        std::cout << "\nSpace modulated delay read vs ideal moving delay (rms / peak error, dB re the sine)\n";
        const double rate = 48000.0;
        const int n = 48000, pre = 1024;
        const struct { double f, maxRms, maxPeak; } cases[] = { { 200.0, -70.0, -62.0 }, { 1000.0, -58.0, -50.0 }, { 4000.0, -36.0, -30.0 } };
        for (const auto& c : cases)
        {
            space::Delay d;
            d.prepare (2048);
            float z1 = 0.0f;
            double se = 0.0, pk = 0.0;
            for (int i = -pre; i < n; ++i)
            {
                const double D = 300.3 + 46.0 * (1.0 + std::sin (2.0 * pi * 8.0 * i / rate));
                const float y = d.readAllpass ((float) D, z1);
                d.push ((float) std::sin (2.0 * pi * std::fmod ((double) i * c.f / rate, 1.0)));
                if (i < 0) continue;
                const double ideal = std::sin (2.0 * pi * c.f * (i - D) / rate);
                se += (y - ideal) * (y - ideal);
                pk = std::max (pk, std::abs (y - ideal));
            }
            const double rmsDb = 10.0 * std::log10 (se / n / 0.5), pkDb = 20.0 * std::log10 (pk);
            std::cout << "   " << juce::String (c.f, 0).paddedLeft (' ', 5) << " Hz: rms " << juce::String (rmsDb, 1) << " dB, peak "
                      << juce::String (pkDb, 1) << " dB\n";
            check (rmsDb <= c.maxRms && pkDb <= c.maxPeak, "modulated read at " + juce::String (c.f) + " Hz");
        }
    }

    // ---- 3. Bass Xover above High Xover: decay stays at the set value (no loop-gain overshoot between them) ----
    {
        std::cout << "\nSpace Bass Xover > High Xover (Large Room, decay 10 s x bass / high mult 2 = 20 s everywhere); tolerance +-10 %\n";
        const double rate = 48000.0;
        const struct { float bx, hx; } xs[] = { { 2000.0f, 1000.0f }, { 16000.0f, 50.0f }, { 8000.0f, 500.0f } };
        for (const auto& x : xs)
        {
            auto s = lateOnly (1, 10.0f);
            s.bassMult = 2.0f; s.highMult = 2.0f; s.bassXover = x.bx; s.highXover = x.hx;
            const auto ir = impulseResponse (s, rate, 22.0);
            std::cout << "   bassX " << juce::String (x.bx, 0).paddedLeft (' ', 5) << " highX " << juce::String (x.hx, 0).paddedLeft (' ', 5) << ":";
            for (double f : { 500.0, 1000.0, 1400.0, 2000.0, 4000.0 })
            {
                const double got = schroederT60 (ir, rate, f);
                std::cout << "  " << juce::String (f, 0) << " Hz " << juce::String (got, 2) << " s";
                check (std::abs (got / 20.0 - 1.0) <= 0.10, "bassX " + juce::String (x.bx) + " highX " + juce::String (x.hx) + " at "
                                                             + juce::String (f) + " Hz: T60 " + juce::String (got, 3) + " s vs 20 s");
            }
            std::cout << "\n";
        }
    }

    // ---- 4. Dry / wet glides land exactly on their targets ----
    {
        std::cout << "\nSpace dry / wet gain after the glide == target (bit-exact)\n";
        const double rate = 48000.0;
        for (float mix : { 0.3f, 0.5f, 0.7f, 0.9f })
        {
            const float m = juce::jlimit (0.0f, 1.0f, mix) * juce::MathConstants<float>::halfPi;
            const float dryT = std::min (1.0f, std::cos (m) * juce::MathConstants<float>::sqrt2), wetT = std::sin (m);
            // dry: an impulse 1 s in, with 500 ms predelay (the wet path is still exactly silent then)
            float dryGot = 0.0f;
            {
                auto s = lateOnly (1, 2.0f);
                s.mix = mix; s.predelayMs = 500.0f;
                space::SpaceReverb rv; rv.prepare (rate); rv.setSettings (s);
                juce::AudioBuffer<float> b (2, (int) rate + 64);
                b.clear();
                b.setSample (0, (int) rate, 1.0f); b.setSample (1, (int) rate, 1.0f);
                run (rv, b);
                dryGot = b.getSample (0, (int) rate);
            }
            // wet: the same impulse through mix 100 % (wet gain 1) and through `mix`; where the input is 0 the
            // output is exactly wet x gain
            int wetMismatch = 0;
            {
                auto s = lateOnly (1, 2.0f);
                juce::AudioBuffer<float> a (2, (int) (1.3 * rate)), b;
                a.clear();
                a.setSample (0, (int) rate, 1.0f); a.setSample (1, (int) rate, 1.0f);
                b.makeCopyOf (a);
                space::SpaceReverb ra; ra.prepare (rate); ra.setSettings (s); run (ra, a);
                s.mix = mix;
                space::SpaceReverb rb; rb.prepare (rate); rb.setSettings (s); run (rb, b);
                for (int c = 0; c < 2; ++c)
                    for (int i = (int) rate + 1; i < a.getNumSamples(); ++i)
                        if (! juce::exactlyEqual (b.getSample (c, i), a.getSample (c, i) * wetT)) ++wetMismatch;
            }
            std::cout << "   mix " << juce::String (mix, 1) << ": dry " << juce::String (dryGot, 8) << " (target " << juce::String (dryT, 8)
                      << ", off by " << juce::String (dryGot - dryT) << "), wet samples != gain x wet: " << wetMismatch << "\n";
            check (juce::exactlyEqual (dryGot, dryT), "dry gain at mix " + juce::String (mix));
            check (wetMismatch == 0, "wet gain at mix " + juce::String (mix));
        }
    }

    // ---- 5. Stability at the parameter extremes, every sample rate: finite, and the tail does not grow ----
    {
        std::cout << "\nSpace stability at the extremes (+24 dBFS noise burst 1 s, then 5 s of tail), 44.1 - 192 kHz\n";
        int nonFinite = 0;
        double worstGrowth = -1000.0;
        for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
            for (int mode = 0; mode < 6; ++mode)
                for (int cfg = 0; cfg < 2; ++cfg)
                {
                    space::Settings s;
                    s.enabled = true; s.mode = mode; s.mix = 1.0f; s.duck = 0.0f; s.decay = 20.0f; s.bassMult = 4.0f; s.highMult = 2.0f;
                    s.lowCut = 20.0f; s.highCut = 20000.0f; s.lModDepth = 1.0f; s.lModRate = 1.0f; s.eModDepth = 1.0f; s.eModRate = 1.0f;
                    s.lDiffusion = 1.0f; s.eDiffusion = 1.0f; s.width = 2.0f;
                    s.bassXover = cfg == 0 ? 16000.0f : 2000.0f; s.highXover = cfg == 0 ? 50.0f : 1000.0f; s.lSize = cfg == 0 ? 0.0f : 1.0f;
                    space::SpaceReverb rv; rv.prepare (rate); rv.setSettings (s);
                    juce::AudioBuffer<float> b (2, (int) (6.0 * rate));
                    b.clear();
                    juce::Random rnd (1234);
                    for (int i = 0; i < (int) rate; ++i)
                        for (int c = 0; c < 2; ++c) b.setSample (c, i, 15.85f * (2.0f * rnd.nextFloat() - 1.0f));
                    run (rv, b);
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < b.getNumSamples(); ++i)
                            if (! std::isfinite (b.getSample (c, i))) ++nonFinite;
                    const int n1 = (int) (1.0 * rate), n2 = (int) (2.0 * rate), n5 = (int) (5.0 * rate);
                    const float early = b.getMagnitude (n1, n2 - n1), late = b.getMagnitude (n5, b.getNumSamples() - n5);
                    const double growth = juce::Decibels::gainToDecibels (late, -300.0f) - juce::Decibels::gainToDecibels (early, -300.0f);
                    worstGrowth = std::max (worstGrowth, growth);
                    check (std::isfinite (growth) && growth <= 0.0, "tail grows: " + juce::String (rate) + " Hz mode " + juce::String (mode)
                                                                      + " cfg " + juce::String (cfg) + " by " + juce::String (growth, 2) + " dB");
                }
        std::cout << "   non-finite samples " << nonFinite << ", worst tail growth (peak 4-5 s after vs 0-1 s after the burst) "
                  << juce::String (worstGrowth, 2) << " dB\n";
        check (nonFinite == 0, "non-finite reverb output");
    }

    std::cout << "Space reverb decay / crossover / glide fixes: " << (bad == 0 ? "OK" : "FAILED") << "\n";
    return bad;
}
