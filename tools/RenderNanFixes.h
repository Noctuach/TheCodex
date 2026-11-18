// Non-finite recovery and distortion-filter resonance checks for CodexRender (included by Render.cpp after its helpers):
//  - one NaN inside the effect chain no longer silences the plug-in until the next prepare: the output guard clears
//    every module's state at the end of that chunk and fades back in, so the output is finite, free of denormals and
//    playing again within two blocks, and the following hits match a clean render
//  - the restart after a recovery is faded in at the chain's input, so it reaches the output without a step after any
//    latency (linear-phase EQ, lookahead, sub delay): no larger sample-to-sample step than the clean render
//  - a NaN parameter value (from a host or a damaged state) never reaches the DSP: it falls back to the default, and the
//    parameter itself, the state tree and a saved / loaded session hold the default too
//  - the distortion's resonant filter keeps its tuned level: LP12 at full resonance peaks at +17.9 dB (pinned +-0.1 dB)
#pragma once
#include <cfloat>
#include <complex>
#include <functional>
#include <limits>
#include <memory>

namespace nanfix
{
    constexpr double hitEvery = 0.5;   // seconds between note-ons (each held 0.35 s)
    constexpr int numHits = 4;

    /** Renders numHits kicks in 512-sample blocks with a fixed noise seed; nanAt >= 0 puts a NaN into the voice signal
        (before every effect) at that sample. It must lie in the first 256-sample chunk of its block. */
    inline juce::AudioBuffer<float> renderHits (CodexProcessor& p, int nanAt)
    {
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
        p.seedNoiseForTesting (7);
        for (int i = 0; i < 6000 && ! p.isEqBuildReadyForTesting(); ++i) juce::Thread::sleep (10);   // linear-phase EQ
        const int beat = (int) (hitEvery * sr), hold = (int) (0.35 * sr), total = beat * numHits + (int) (0.2 * sr);
        juce::AudioBuffer<float> out (2, total), buf (2, block);
        for (int pos = 0; pos < total; pos += block)
        {
            const int n = std::min (block, total - pos);
            buf.setSize (2, n, false, false, true);
            buf.clear();
            juce::MidiBuffer midi;
            for (int k = 0; k < numHits; ++k)
            {
                const int on = k * beat, off = on + hold;
                if (on >= pos && on < pos + n)   midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), on - pos);
                if (off >= pos && off < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, 60), off - pos);
            }
            if (nanAt >= pos && nanAt < pos + n) { jassert (nanAt - pos < 256); p.injectNonFiniteForTesting (nanAt - pos); }
            p.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, n);
        }
        return out;
    }

    inline double rmsDbOf (const juce::AudioBuffer<float>& b, int start, int len)
    {
        double acc = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = start; i < start + len; ++i) acc += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return 10.0 * std::log10 (acc / (2.0 * len) + 1.0e-30);
    }

    /** Every module on (both distortions oversampled, saturator, compressor, reverb, both EQs with a band, sub layer and
        output clipper), the EQs in the given processing mode. */
    inline void everythingOn (CodexProcessor& p, int eqMode)
    {
        setParam (p, ID::keyMode, 0.0f);
        for (int st = 1; st <= 2; ++st) { setParam (p, ID::clip (st, "On"), 1); setParam (p, ID::clip (st, "OS"), (float) (st == 1 ? 2 : 1));
                                          setParam (p, ID::clip (st, "Drive"), 9.0f); setParam (p, ID::clip (st, "FltPos"), 2); setParam (p, ID::clip (st, "FltRes"), 0.6f); }
        setParam (p, ID::sat ("On"), 1); setParam (p, ID::sat ("Bands"), 3); setParam (p, ID::sat ("OS"), 1);
        setParam (p, ID::cmp ("On"), 1); setParam (p, ID::cmp ("Bands"), 3);
        setParam (p, ID::spc ("On"), 1); setParam (p, ID::spc ("Mix"), 0.3f);
        for (int e = 0; e < 2; ++e)
        {
            setParam (p, ID::eqOn (e), 1); setParam (p, ID::eqMode (e), (float) eqMode);
            setParam (p, ID::eqBand (e, 0, "On"), 1); setParam (p, ID::eqBand (e, 0, "Gain"), 4.0f);
        }
        setParam (p, ID::subOn, 1); setParam (p, ID::clipOn, 1);
    }

    /** Peak gain (dB) of a distortion unit's LP12 filter (post, HardClip in its linear range, OS Off) at resonance res,
        cutoff fc: a sine scan around fc, each measured after the resonance has settled, with the first-order ADAA's
        known 0.5 (1 + z^-1) response divided out. */
    inline double svfPeakDb (float res, float fc, double& peakHz)
    {
        ClipDistortion::Settings s;
        s.enabled = true; s.mode = ClipDistortion::HardClip; s.driveDb = 0.0f; s.mix = 1.0f; s.outDb = 0.0f; s.dcFilter = false;
        s.oversampling = 0; s.filterPos = 2; s.filterType = ClipDistortion::LowPass12; s.filterHz = fc; s.filterRes = res;
        constexpr float amp = 0.05f;   // far below the clip point: the curve is the identity
        const int settle = (int) (0.15 * sr), len = (int) (0.1 * sr);
        auto gainAt = [&] (double hz)
        {
            ClipDistortion d;
            d.setSettings (s);
            d.prepare (sr, 256);
            juce::AudioBuffer<float> b (2, settle + len);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * std::fmod (hz * i / sr, 1.0));
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            for (int pos = 0; pos < b.getNumSamples(); pos += 256)
                d.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, b.getNumSamples() - pos));
            std::complex<double> acc; double wsum = 0.0;
            for (int i = 0; i < len; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (i + 0.5) / len);
                acc += w * (double) b.getSample (0, settle + i) * std::polar (1.0, -juce::MathConstants<double>::twoPi * hz * (settle + i) / sr);
                wsum += w;
            }
            const double adaa = std::cos (juce::MathConstants<double>::pi * hz / sr);
            return 2.0 * std::abs (acc) / wsum / amp / adaa;
        };
        double best = 0.0, bestHz = fc;
        for (double hz = 0.9 * fc; hz <= 1.1 * fc; hz += 0.005 * fc) { const double g = gainAt (hz); if (g > best) { best = g; bestHz = hz; } }
        const double centre = bestHz;
        for (double hz = centre - 0.005 * fc; hz <= centre + 0.005 * fc; hz += 0.0002 * fc) { const double g = gainAt (hz); if (g > best) { best = g; bestHz = hz; } }
        peakHz = bestHz;
        return 20.0 * std::log10 (best);
    }
}

static int nanRecoveryTests()
{
    using namespace nanfix;
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };

    // ---- 1. One NaN inside the chain: every module recovers, playing again within two blocks (+ the chain latency) ----
    //  The restart is faded in where it enters the chain (voice and sub layer), so it reaches the output smoothly after
    //  any latency: from the recovery to one block past the latency, the largest sample-to-sample step and the loudest
    //  64-sample window must not exceed the clean render's (a cleared linear-phase EQ, lookahead line or sub delay used
    //  to let the mid-kick restart through as an unfaded step once its latency had passed)
    {
        std::cout << "\nNon-finite recovery: one NaN into the chain (all modules on) vs a clean render\n"
                  << "  EQ mode / lookahead              recoveries  non-finite  denormals  blk+2 dB (clean)   hit 3 / hit 4 dB vs clean"
                     "   onset max step (clean)   onset peak 64 dB (clean)\n";
        const int nanAt = 23 * block + 100;          // 0.25 s: in the first hit's tail, first chunk of its block
        const int beat = (int) (hitEvery * sr);
        auto maxStep = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            float m = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = from + 1; i < to; ++i) m = std::max (m, std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
            return m;
        };
        auto peakWindowDb = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            double m = -300.0;
            for (int i = from; i + 64 <= to; i += 16) m = std::max (m, rmsDbOf (b, i, 64));
            return m;
        };
        // look: compressor lookahead (choice index, 4 = 10 ms); comp false: compressor off. With the compressor on, the
        // recovery clears its (non-finite) gain reduction, so the tail after it plays about 4 dB louder until the next
        // hit (6 dB bound): the level bound is tight (1 dB) only without it
        struct Case { int mode, look; bool comp; };
        const Case cases[] = { { 0, 0, true }, { 1, 0, true }, { 2, 0, true }, { 0, 4, true }, { 0, 0, false }, { 2, 0, false } };
        for (const auto& c : cases)
        {
            const int mode = c.mode;
            auto setup = [&c, mode] (CodexProcessor& p)
            {
                everythingOn (p, mode);
                setParam (p, ID::cmp ("Look"), (float) c.look);
                setParam (p, ID::cmp ("On"), c.comp ? 1.0f : 0.0f);
            };
            auto clean = std::make_unique<CodexProcessor>();
            setup (*clean);
            const auto ref = renderHits (*clean, -1);
            auto hurt = std::make_unique<CodexProcessor>();
            setup (*hurt);
            const auto out = renderHits (*hurt, nanAt);

            int nonFinite = 0, denormals = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    const float v = out.getSample (ch, i);
                    if (! std::isfinite (v)) ++nonFinite;
                    else if (! juce::exactlyEqual (v, 0.0f) && std::abs (v) < FLT_MIN) ++denormals;
                }
            // the guard mutes the non-finite samples (exact zeros where the clean render plays) and clears every state
            // at the end of that chunk
            int muted = nanAt;
            while (muted < out.getNumSamples() && ! (juce::exactlyEqual (out.getSample (0, muted), 0.0f) && juce::exactlyEqual (out.getSample (1, muted), 0.0f)
                                                     && ! juce::exactlyEqual (ref.getSample (0, muted), 0.0f))) ++muted;
            const int recoverAt = (muted / 256 + 1) * 256;
            // playing again two blocks after the recovery (it fades in over 10 ms), plus the chain's latency: a cleared
            // linear-phase EQ outputs its new input only after its latency, and its FIR needs about a block of the restarted
            // signal on both sides of its centre before its output reaches the clean level
            const int at = recoverAt + 2 * block + hurt->getLatencySamples();
            const double after = rmsDbOf (out, at, block), afterRef = rmsDbOf (ref, at, block);
            // the onset: from the recovery to one block past the latency
            const float step = maxStep (out, recoverAt, at + block), stepRef = maxStep (ref, recoverAt, at + block);
            const double onset = peakWindowDb (out, recoverAt, at + block), onsetRef = peakWindowDb (ref, recoverAt, at + block);
            // later hits: the state cleared by the recovery (reverb tail, compressor memory) has died away
            const double h3 = rmsDbOf (out, 2 * beat, beat) - rmsDbOf (ref, 2 * beat, beat);
            const double h4 = rmsDbOf (out, 3 * beat, beat) - rmsDbOf (ref, 3 * beat, beat);
            const int rec = hurt->safetyRecoveries.load();
            const juce::String m = juce::String (eq::processingModeNames()[mode]) + (c.look > 0 ? " / " + comp::lookaheadNames()[c.look] : juce::String())
                                   + (c.comp ? juce::String() : juce::String (", comp off"));
            const double levelMargin = c.comp ? 6.0 : 1.0;
            std::cout << "  " << m.paddedRight (' ', 31) << juce::String (rec).paddedLeft (' ', 10)
                      << juce::String (nonFinite).paddedLeft (' ', 12) << juce::String (denormals).paddedLeft (' ', 11)
                      << juce::String (after, 1).paddedLeft (' ', 10) << " (" << juce::String (afterRef, 1) << ")"
                      << juce::String (h3, 3).paddedLeft (' ', 14) << " / " << juce::String (h4, 3)
                      << juce::String (step, 4).paddedLeft (' ', 16) << " (" << juce::String (stepRef, 4) << ")"
                      << juce::String (onset, 1).paddedLeft (' ', 18) << " (" << juce::String (onsetRef, 1) << ")\n";
            check (clean->safetyRecoveries.load() == 0 && ! clean->safetyTripped.load(), m + ": the clean render tripped the guard");
            check (rec == 1, m + ": recoveries " + juce::String (rec) + " (one reset must clear every non-finite state)");
            check (nonFinite == 0 && denormals == 0, m + ": non-finite or denormal output samples");
            check (after > afterRef - 6.0 && after > -60.0, m + ": not playing again two blocks after the NaN");
            check (after < afterRef + levelMargin, m + ": louder than the clean render two blocks after the recovery");
            check (step <= 1.1f * stepRef, m + ": the restart steps (clicks) after the chain latency");
            check (onset < onsetRef + levelMargin, m + ": the restart overshoots the clean render's level");
            check (std::abs (h3) < 0.5 && std::abs (h4) < 0.1, m + ": later hits differ from the clean render");
        }
    }

    // ---- 2. A NaN parameter value (as a host could send) falls back to the default: bit-identical to the default ----
    {
        std::cout << "\nNon-finite parameters: NaN in Tune / Dist 1 Drive / Space Mix -> the default value (bit-identical render)\n";
        const juce::String ids[] = { ID::tune, ID::clip (1, "Drive"), ID::spc ("Mix") };
        for (const auto& id : ids)
        {
            auto clean = std::make_unique<CodexProcessor>();
            setParam (*clean, ID::keyMode, 0.0f); setParam (*clean, ID::spc ("On"), 1); setParam (*clean, ID::clip (1, "On"), 1);
            auto* prm = clean->apvts.getParameter (id);
            prm->setValueNotifyingHost (prm->getDefaultValue());
            const auto ref = renderHits (*clean, -1);

            auto hurt = std::make_unique<CodexProcessor>();
            setParam (*hurt, ID::keyMode, 0.0f); setParam (*hurt, ID::spc ("On"), 1); setParam (*hurt, ID::clip (1, "On"), 1);
            {
                // what the plug-in wrappers do with a host value: setValue, then tell the listeners (the state's raw value)
                auto* h = hurt->apvts.getParameter (id);
                const float nan = std::numeric_limits<float>::quiet_NaN();
                h->setValue (nan);
                h->sendValueChangedMessageToListeners (nan);
            }
            const bool reached = ! std::isfinite (hurt->apvts.getRawParameterValue (id)->load());
            const auto out = renderHits (*hurt, -1);
            float maxDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i) maxDiff = std::max (maxDiff, std::abs (out.getSample (ch, i) - ref.getSample (ch, i)));
            const float raw = hurt->apvts.getRawParameterValue (id)->load();
            std::cout << "  " << id.paddedRight (' ', 12) << " NaN reached the raw value: " << (reached ? "yes" : "no")
                      << ", now " << raw << ", peak " << juce::String (out.getMagnitude (0, out.getNumSamples()), 3)
                      << ", max |diff| vs default " << maxDiff << ", guard tripped: " << (hurt->safetyTripped.load() ? "yes" : "no") << "\n";
            check (juce::exactlyEqual (maxDiff, 0.0f) && ! hurt->safetyTripped.load() && std::isfinite (raw), id + ": a NaN parameter changed the output");
            // the parameter itself (what the host and the GUI read) and the saved state are back at the default too
            auto* h = hurt->apvts.getParameter (id);
            const float value = h->getValue();
            const auto saved = hurt->apvts.copyState().getChildWithProperty ("id", id).getProperty ("value");
            std::cout << "  " << juce::String().paddedRight (' ', 12) << " parameter value now " << value << " (default "
                      << h->getDefaultValue() << "), saved state value " << saved.toString() << "\n";
            check (juce::exactlyEqual (value, h->getDefaultValue()), id + ": the parameter still reads a non-finite value");
            check (std::isfinite ((float) saved) && juce::approximatelyEqual ((float) saved, h->convertFrom0to1 (h->getDefaultValue())),
                   id + ": the state tree still holds a non-finite value");
        }
    }

    // ---- 3. A damaged session with a NaN / inf value loads (and saves) the default ----
    {
        std::cout << "\nNon-finite state: a saved session with NaN in Tune and inf in Space Mix loads the defaults\n";
        auto src = std::make_unique<CodexProcessor>();
        auto tree = src->apvts.copyState();
        tree.getChildWithProperty ("id", ID::tune).setProperty ("value", std::numeric_limits<double>::quiet_NaN(), nullptr);
        tree.getChildWithProperty ("id", ID::spc ("Mix")).setProperty ("value", std::numeric_limits<double>::infinity(), nullptr);
        juce::MemoryBlock blob;
        if (auto xml = tree.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, blob);
        auto dst = std::make_unique<CodexProcessor>();
        dst->setStateInformation (blob.getData(), (int) blob.getSize());
        juce::MemoryBlock again;
        dst->getStateInformation (again);
        juce::String savedNaN;
        if (auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize()))
        {
            const auto t = juce::ValueTree::fromXml (*xml);
            for (auto child : t)
                if (child.hasProperty ("value") && ! std::isfinite ((float) child.getProperty ("value"))) savedNaN << child.getProperty ("id").toString() << " ";
        }
        for (const auto& id : { ID::tune, ID::spc ("Mix") })
        {
            auto* h = dst->apvts.getParameter (id);
            const float raw = dst->apvts.getRawParameterValue (id)->load();
            std::cout << "  " << id.paddedRight (' ', 12) << " parameter " << h->getValue() << " (default " << h->getDefaultValue()
                      << "), raw " << raw << "\n";
            check (juce::exactlyEqual (h->getValue(), h->getDefaultValue()) && juce::approximatelyEqual (raw, h->convertFrom0to1 (h->getDefaultValue())),
                   id + ": a non-finite saved value did not load as the default");
        }
        std::cout << "  non-finite values in the state saved again: " << (savedNaN.isEmpty() ? juce::String ("none") : savedNaN) << "\n";
        check (savedNaN.isEmpty(), "the plug-in saved a non-finite parameter value: " + savedNaN);
    }
    return bad;
}

static int svfResonancePeakTest()
{
    int bad = 0;
    // Pinned on purpose (the presets are tuned to it): LP12 at full resonance (Q 18) peaks at
    // Q / sqrt (1 - 1 / (4 Q^2)) * resComp = 18.007 * 0.43644 = +17.907 dB. A change here must be deliberate.
    const double q = 18.0, formula = 20.0 * std::log10 (q / std::sqrt (1.0 - 1.0 / (4.0 * q * q)) / std::sqrt (q / 4.0 + 0.75));
    double hz = 0.0;
    const double peak = nanfix::svfPeakDb (1.0f, 1000.0f, hz);
    std::cout << "\nDistortion filter LP12, resonance 1, 1 kHz: peak " << juce::String (peak, 3) << " dB at " << juce::String (hz, 1)
              << " Hz (formula " << juce::String (formula, 3) << " dB, pinned 17.9 +- 0.1 dB)\n";
    if (std::abs (peak - 17.9) > 0.1) { ++bad; std::cout << "   FAILED: resonance peak moved\n"; }
    return bad;
}
