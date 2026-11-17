// Compressor regression checks for CodexRender (included by Render.cpp after its helpers):
//  - feedback-detector styles (Classic, Vari-Mu): the band Gain / make-up sits outside the feedback loop, so the
//    gain reduction does not depend on it
//  - every other style (and the feedback styles at 0 dB make-up) is bit-exact to the code before that fix
#pragma once

namespace compfix
{
    inline juce::AudioBuffer<float> sine (float hz, float dbfs, int n)
    {
        juce::AudioBuffer<float> b (2, n);
        const float a = juce::Decibels::decibelsToGain (dbfs);
        for (int i = 0; i < n; ++i)
        {
            const float v = a * (float) std::sin (juce::MathConstants<double>::twoPi * std::fmod ((double) hz * i / sr, 1.0));
            b.setSample (0, i, v); b.setSample (1, i, 0.8f * v);
        }
        return b;
    }

    inline void run (comp::MultibandCompressor& c, juce::AudioBuffer<float>& b, int blockSize = 256)
    {
        for (int p = 0; p < b.getNumSamples(); p += blockSize)
            c.process (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (blockSize, b.getNumSamples() - p));
    }

    inline comp::Settings settings (int style, float gainDb, bool autoGain)
    {
        comp::Settings s; s.enabled = true; s.style = style; s.autoGain = autoGain;
        auto& b = s.bands[0];
        b.direction = 0; b.thresholdDb = -24.0f; b.ratio = 4.0f; b.kneeDb = 6.0f; b.rangeDb = 30.0f;
        b.attackMs = 5.0f; b.releaseMs = 80.0f; b.gainDb = gainDb;
        for (auto& o : s.bands) o = b;
        return s;
    }

    /** Gain reduction (dB) on a steady -6 dBFS 1 kHz sine, measured from the output level with the band Gain removed. */
    inline float measuredGr (int style, float gainDb)
    {
        comp::MultibandCompressor c; c.prepare (sr); c.setSettings (settings (style, gainDb, false));
        const int n = (int) (1.5 * sr), tail = (int) (0.2 * sr);
        auto b = sine (1000.0f, -6.0f, n);
        run (c, b);
        return juce::Decibels::gainToDecibels (b.getMagnitude (0, n - tail, tail)) - gainDb - (-6.0f);
    }

    /** FNV-1a over the float bits of a render: several band layouts / gains / switches for one style. */
    inline uint64_t styleHash (int style, bool includeMakeup)
    {
        uint64_t h = 1469598103934665603ull;
        auto add = [&h] (const juce::AudioBuffer<float>& b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float v = b.getSample (ch, i);
                    uint32_t u; std::memcpy (&u, &v, 4);
                    for (int k = 0; k < 4; ++k) { h ^= (u >> (8 * k)) & 0xffu; h *= 1099511628211ull; }
                }
        };
        const int n = (int) (0.4 * sr);
        // a decaying kick-like sweep plus a sine: crosses the threshold both ways
        juce::AudioBuffer<float> src (2, n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr, f = 45.0 + 400.0 * std::exp (-t * 30.0);
            ph += f / sr;
            const float v = (float) (0.9 * std::exp (-t * 6.0) * std::sin (juce::MathConstants<double>::twoPi * std::fmod (ph, 1.0))
                                     + 0.1 * std::sin (juce::MathConstants<double>::twoPi * std::fmod (3000.0 * t, 1.0)));
            src.setSample (0, i, v); src.setSample (1, i, 0.7f * v);
        }
        for (int layout = 0; layout < 3; ++layout)
        {
            comp::Settings s = settings (style, 0.0f, false);
            if (layout == 1) { s.numBands = 3; s.lookahead = 2; }
            if (layout == 2) { s.numBands = 4; s.holdMs = 5.0f; s.mix = 0.7f; s.outDb = -2.0f; s.bands[2].bypass = true; }
            if (includeMakeup)
            {
                s.autoGain = layout != 0;
                for (int b = 0; b < comp::maxBands; ++b) s.bands[(size_t) b].gainDb = 3.0f * (float) b - 2.0f;
            }
            // settings first: prepare() then starts every smoother at its target (no make-up glide from the defaults)
            comp::MultibandCompressor c; c.setSettings (s); c.prepare (sr);
            auto b = src;
            run (c, b, 128);
            add (b);
        }
        return h;
    }
}

static int compressorFeedbackMakeupTests()
{
    int bad = 0;
    std::cout << "\nCompressor feedback styles: gain reduction with band Gain 0 dB vs +12 dB (steady sine)\n";
    for (int style : { (int) comp::Classic, (int) comp::VariMu })
    {
        const float gr0 = compfix::measuredGr (style, 0.0f), gr12 = compfix::measuredGr (style, 12.0f);
        const bool ok = std::abs (gr0 - gr12) <= 0.05f && gr0 < -3.0f;
        std::cout << "  " << comp::styleNames()[style].paddedRight (' ', 9) << " GR " << juce::String (gr0, 2) << " dB vs "
                  << juce::String (gr12, 2) << " dB: " << (ok ? "OK" : "FAILED") << "\n";
        if (! ok) ++bad;
    }

    // bit-exactness: the hashes were taken from the code before the fix. Non-feedback styles are hashed with
    // band gains and auto make-up; the feedback styles only at 0 dB make-up (the case the fix leaves unchanged).
    static const uint64_t expected[comp::numStyles] = {
        0x4927fde18dd6f420ull, 0xa802491eef9461b0ull, 0x2fedb391bad5ec21ull, 0xa51da6ff89448772ull,
        0x58701bb08a4ae441ull, 0x9281a9a2eb2e518dull, 0x5775a5dd76dafbdull,  0x4de1009058a957b8ull,
        0x85d2681e68f76cull,   0xa99d4735815fe6ull,   0x2bcd17772ffa1392ull };
    const bool dump = std::getenv ("CODEX_COMP_HASH_DUMP") != nullptr;
    int changed = 0;
    for (int style = 0; style < comp::numStyles; ++style)
    {
        const bool feedback = style == comp::Classic || style == comp::VariMu;
        const uint64_t h = compfix::styleHash (style, ! feedback);
        if (dump) std::cout << "  hash " << style << " 0x" << juce::String::toHexString ((juce::int64) h) << "ull\n";
        if (h != expected[style]) { ++changed; std::cout << "   FAILED: style " << comp::styleNames()[style] << " output changed\n"; }
    }
    std::cout << "  other styles bit-exact: " << (changed == 0 ? "OK" : "FAILED") << "\n";
    return bad + changed;
}
