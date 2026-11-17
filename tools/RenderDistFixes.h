// Distortion regression checks for CodexRender (included by Render.cpp after its helpers):
//  - no step after prepare() / reset() when Offset (or Spatial) is not 0 (ADAA history seeded at the operating point)
//  - a switched-off unit is transparent: its DC filter does not run, and toggling it stays click-free
#pragma once
#include <complex>

namespace distfix
{
    /** Runs a unit over a buffer in 256-sample blocks (in place). */
    inline void run (ClipDistortion& d, juce::AudioBuffer<float>& b, int blockSize = 256)
    {
        for (int p = 0; p < b.getNumSamples(); p += blockSize)
            d.process (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (blockSize, b.getNumSamples() - p));
    }

    /** Largest |output| of a unit fed with n samples of silence. */
    inline float silentPeak (ClipDistortion& d, int n)
    {
        juce::AudioBuffer<float> b (2, n);
        b.clear();
        run (d, b);
        return b.getMagnitude (0, n);
    }

    inline juce::AudioBuffer<float> sine (double rate, float hz, float amp, int n, float dc = 0.0f)
    {
        juce::AudioBuffer<float> b (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = dc + amp * (float) std::sin (juce::MathConstants<double>::twoPi * std::fmod ((double) hz * i / rate, 1.0));
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        return b;
    }

    /** Complex amplitude of channel 0 at hz over [start, start + len). */
    inline std::complex<double> bin (const juce::AudioBuffer<float>& b, double rate, float hz, int start, int len)
    {
        std::complex<double> acc;
        for (int i = start; i < start + len; ++i)
            acc += (double) b.getSample (0, i) * std::polar (1.0, -juce::MathConstants<double>::twoPi * hz * i / rate);
        return acc;
    }
}

static int distortionFixTests()
{
    using namespace distfix;
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };

    // ---- 1. Offset: no step after prepare() / a sample-rate change / reset() with silent input ----
    {
        std::cout << "\nDistortion: no step after prepare / rate change / reset with Offset != 0 (peak of silent output)\n";
        struct Case { const char* name; int mode; float offset, spatial; int os; bool custom; };
        const Case cases[] = {
            { "Hard Clip, Offset 1.0, OS Off",  ClipDistortion::HardClip, 1.0f, 0.0f, 0, false },
            { "Hard Clip, Offset 1.0, OS 4x",   ClipDistortion::HardClip, 1.0f, 0.0f, 2, false },
            { "Hard Clip, Offset 1.0, OS 16x",  ClipDistortion::HardClip, 1.0f, 0.0f, 4, false },
            { "Tanh, Offset -0.4, Spatial 0.7", ClipDistortion::Tanh, -0.4f, 0.7f, 1, false },
            { "Sine Fold, Offset 0.8, OS Off",  ClipDistortion::SineFold, 0.8f, 0.0f, 0, false },
            { "X-Shaper (Asym), Offset -0.6",   ClipDistortion::XShaperAsym, -0.6f, 0.0f, 0, false },
            { "Tube, Offset 0, Spatial 1.0",    ClipDistortion::Tube, 0.0f, 1.0f, 0, false },
            { "Custom curve, Offset 0.5, Off",  ClipDistortion::Tanh, 0.5f, 0.0f, 0, true },
            { "Custom curve, Offset -0.7, 4x",  ClipDistortion::Tanh, -0.7f, 0.3f, 2, true },
        };
        const auto custom = curvetest::bentCurve();
        for (const auto& c : cases)
        {
            ClipDistortion::Settings st;
            st.mode = c.mode; st.offset = c.offset; st.spatial = c.spatial; st.oversampling = c.os; st.driveDb = 6.0f; st.custom = c.custom;
            st.curve.pos = { 0.3f, 0.55f, 0.7f, 0.8f, 0.88f, 0.94f, 0.98f, 1.0f };
            ClipDistortion d;
            d.prepare (sr, 256);
            d.setSettings (st);
            if (c.custom) { d.publishCustomCurve (&custom); d.setSettings (st); }
            silentPeak (d, 8192);                       // settings glide in from the defaults
            const float settled = silentPeak (d, 4096); // steady state (0)
            d.prepare (44100.0, 256);                   // sample-rate change
            const float afterPrepare = silentPeak (d, 4096);
            d.reset();
            const float afterReset = silentPeak (d, 4096);
            d.prepare (sr, 256);                        // back, with the settings already in place
            const float afterPrepare2 = silentPeak (d, 4096);
            const float worst = std::max ({ afterPrepare, afterReset, afterPrepare2 });
            std::cout << "  " << juce::String (c.name).paddedRight (' ', 34) << " settled " << settled << ", after prepare " << afterPrepare
                      << ", after reset " << afterReset << ", after prepare (48k) " << afterPrepare2 << (c.custom ? (d.isCustomActive() ? "  [custom]" : "  [custom NOT active]") : "") << "\n";
            check (! c.custom || d.isCustomActive(), juce::String (c.name) + ": custom curve active");
            check (worst <= settled + 1.0e-6f, juce::String (c.name) + ": step after prepare / reset");
        }
    }

    // ---- 2. A switched-off unit is transparent (its DC filter does not run) ----
    {
        std::cout << "\nDistortion off: transparent at low frequencies (DC filter only runs while the unit is on)\n";
        const int n = (int) sr * 2;   // whole cycles of 30 and 50 Hz
        auto offUnit = [] (int os, bool dc)
        {
            ClipDistortion::Settings st; st.enabled = false; st.mode = ClipDistortion::Tanh; st.driveDb = 18.0f; st.dcFilter = dc; st.oversampling = os;
            auto d = std::make_unique<ClipDistortion>();
            d->prepare (sr, 256); d->setSettings (st); d->reset(); d->setSettings (st);
            return d;
        };
        for (float hz : { 30.0f, 50.0f })
        {
            const auto in = sine (sr, hz, 0.5f, n);
            // OS Off and Raw: bit-exact, DC switch on (the factory default)
            for (int os : { 0, ClipDistortion::rawOversampling })
            {
                auto d = offUnit (os, true);
                auto b = in; run (*d, b);
                float diff = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = (int) (0.1 * sr); i < n; ++i)   // after the 20 ms switch-off glide from the prepare() defaults
                        diff = std::max (diff, std::abs (b.getSample (ch, i) - in.getSample (ch, i)));
                std::cout << "  " << (int) hz << " Hz, OS " << ClipDistortion::oversamplingNames()[os] << ", DC on: max |out - in| " << diff << " (from 100 ms)\n";
                check (diff == 0.0f, juce::String ((int) hz) + " Hz OS " + ClipDistortion::oversamplingNames()[os] + ": off unit not bit-exact");
            }
            // OS 4x (the default): gain 0 dB and phase 0 deg once the oversampler's latency is removed, and the DC switch
            // makes no difference at all
            {
                auto dOn = offUnit (2, true), dOff = offUnit (2, false);
                auto bOn = in, bOff = in; run (*dOn, bOn); run (*dOff, bOff);
                float diff = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i) diff = std::max (diff, std::abs (bOn.getSample (ch, i) - bOff.getSample (ch, i)));
                const int start = (int) sr / 2, len = (int) sr;
                const auto h = bin (bOn, sr, hz, start, len) / bin (in, sr, hz, start, len);
                const double lat = dOn->getLatencyInSamples();
                const double gainDb = juce::Decibels::gainToDecibels (std::abs (h));
                const double phaseDeg = std::remainder (std::arg (h) + juce::MathConstants<double>::twoPi * hz * lat / sr, juce::MathConstants<double>::twoPi)
                                        * 180.0 / juce::MathConstants<double>::pi;
                std::cout << "  " << (int) hz << " Hz, OS 4x, DC on: " << juce::String (gainDb, 4) << " dB, " << juce::String (phaseDeg, 3)
                          << " deg (latency " << lat << " removed); DC on vs off max diff " << diff << "\n";
                check (std::abs (gainDb) < 0.005 && std::abs (phaseDeg) < 0.1, juce::String ((int) hz) + " Hz OS 4x: off unit filters");
                check (diff == 0.0f, juce::String ((int) hz) + " Hz OS 4x: DC switch changes an off unit");
            }
        }
    }

    // ---- 3. Toggling On and the DC switch stays click-free (the DC filter fades in / out) ----
    {
        std::cout << "\nDistortion: On / DC switch toggles on a signal with DC (largest sample step)\n";
        const int n = (int) sr;
        const auto in = sine (sr, 40.0f, 0.2f, n, 0.3f);
        float inStep = 0.0f;
        for (int i = 1; i < n; ++i) inStep = std::max (inStep, std::abs (in.getSample (0, i) - in.getSample (0, i - 1)));
        auto maxStep = [] (const juce::AudioBuffer<float>& b, int from)
        {
            float m = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = from + 1; i < b.getNumSamples(); ++i) m = std::max (m, std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
            return m;
        };
        struct Toggle { const char* name; bool on0, dc0, on1, dc1; };
        const Toggle toggles[] = { { "On -> Off", true, true, false, true }, { "Off -> On", false, true, true, true },
                                   { "DC on -> off", true, true, true, false }, { "DC off -> on", true, false, true, true } };
        for (int variant = 0; variant < 2; ++variant)
        for (const auto& t : toggles)
        {
            ClipDistortion::Settings st; st.mode = ClipDistortion::Tanh; st.driveDb = 0.0f; st.oversampling = 0; st.outDb = 0.0f;
            if (variant == 1)   // OS 4x, Lo / Hi filters (dry all-passes) and a pre band-pass
            {
                st.oversampling = 2; st.loFilterHz = 8000.0f; st.hiFilterHz = 120.0f;
                st.filterPos = 1; st.filterType = ClipDistortion::BandPass; st.filterHz = 200.0f; st.filterRes = 0.3f;
            }
            st.enabled = t.on0; st.dcFilter = t.dc0;
            ClipDistortion d; d.prepare (sr, 256); d.setSettings (st); d.reset(); d.setSettings (st);
            auto b = in;
            const int half = (n / 2 / 256) * 256;   // on a block boundary
            for (int p = 0; p < n; p += 256)
            {
                if (p == half) { st.enabled = t.on1; st.dcFilter = t.dc1; d.setSettings (st); }
                d.process (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (256, n - p));
            }
            const float step = maxStep (b, half - 2048);
            std::cout << "  " << (variant == 0 ? "OS Off, " : "OS 4x + filters, ") << juce::String (t.name).paddedRight (' ', 14) << " largest step " << step << " (input " << inStep << ")\n";
            check (step < 4.0f * inStep, juce::String (variant == 0 ? "OS Off " : "OS 4x + filters ") + t.name + ": click on toggle");
        }
    }
    return bad;
}

/** Whole processor, factory default settings with both distortions off: the DC switches make no difference (an off
    unit's DC filter no longer runs), so there is no low-frequency loss from switched-off units. */
static int processorDistOffTest()
{
    int bad = 0;
    CodexProcessor p;
    p.setPlayConfigDetails (0, 2, sr, block);
    p.setNonRealtime (true);
    setParam (p, ID::keyMode, 0.0f);
    for (int st = 1; st <= 2; ++st) setParam (p, ID::clip (st, "On"), 0.0f);
    auto take = [&p]
    {
        p.seedNoiseForTesting (1);
        return render (p, 1);
    };
    const auto a = take(), a2 = take();
    for (int st = 1; st <= 2; ++st) setParam (p, ID::clip (st, "DC"), 0.0f);
    const auto b = take();
    float rep = 0.0f, diff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            rep = std::max (rep, std::abs (a.getSample (ch, i) - a2.getSample (ch, i)));
            diff = std::max (diff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        }
    // level of the low end (fundamental region) with the DC switches on vs off, 50-300 ms
    const float lvlOn = rmsDb (a, 0.05, 0.25), lvlOff = rmsDb (b, 0.05, 0.25);
    std::cout << "\nProcessor defaults, both distortions off: DC switches on vs off: max diff " << diff << " (repeat " << rep << "), level "
              << juce::String (lvlOn - lvlOff, 3) << " dB: " << (diff == 0.0f && rep == 0.0f ? "OK" : "FAILED") << "\n";
    if (rep != 0.0f) { ++bad; std::cout << "   FAILED: render not repeatable\n"; }
    if (diff != 0.0f) { ++bad; std::cout << "   FAILED: switched-off distortions still filter the low end\n"; }
    return bad;
}
