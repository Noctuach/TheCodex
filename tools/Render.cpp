// Offline test tool.
//   CodexRender <outdir>
// Renders every factory preset (kick.wav per preset), measures the tail pitch with the tuner's
// detector, checks all 12 keys, round-trips the state and snapshots the editor in both engines.
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/GUI/PluginEditor.h"
#include "../Source/DSP/EqEngine.h"
#include "../Source/DSP/DrawEnvelope.h"

static void setParam (CodexProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static constexpr double sr = 48000.0;
static constexpr int block = 512;

// Renders `hits` kicks (one per 0.4 s, each held 0.35 s)
static juce::AudioBuffer<float> render (CodexProcessor& proc, int hits, int midiNote = 60,
                                        double rate = sr, int blockSize = block, bool randomBlocks = false)
{
    proc.setRateAndBufferSizeDetails (rate, blockSize);
    proc.prepareToPlay (rate, blockSize);
    const int beat = (int) (0.4 * rate), hold = (int) (0.35 * rate), total = beat * hits + (int) (0.2 * rate);
    juce::AudioBuffer<float> out (2, total), buf (2, blockSize);
    juce::Random rng (42);
    for (int pos = 0; pos < total;)
    {
        const int n = std::min (randomBlocks ? 1 + rng.nextInt (blockSize) : blockSize, total - pos);
        buf.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        for (int k = 0; k < hits; ++k)
        {
            const int on = k * beat, off = on + hold;
            if (on >= pos && on < pos + n)   midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, (juce::uint8) 127), on - pos);
            if (off >= pos && off < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, midiNote), off - pos);
        }
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, n);
        pos += n;
    }
    return out;
}

// Same approach as the GUI tuner: decimate to ~6 kHz and run the detector on the tail.
static float measurePitch (const juce::AudioBuffer<float>& b, double startSec, double lenSec, double rate = sr)
{
    const int decim = juce::roundToInt (rate / 6000.0);
    std::vector<float> d;
    const int s0 = (int) (startSec * rate), s1 = std::min (b.getNumSamples(), (int) ((startSec + lenSec) * rate));
    for (int i = s0; i + decim <= s1; i += decim)
    {
        float acc = 0;
        for (int k = 0; k < decim; ++k) acc += b.getSample (0, i + k);
        d.push_back (acc / decim);
    }
    return detectPitch (d.data(), (int) d.size(), rate / decim, 25.0f, 400.0f).frequency;
}

static void writeWav (const juce::AudioBuffer<float>& b, const juce::File& f)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    if (auto stream = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream()))
        if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0)))
        {
            stream.release();
            writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
}

static void snapshot (CodexProcessor& proc, const juce::AudioBuffer<float>& audio, const juce::File& png, int tab = 0, int overlay = 0)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    if (auto* hk = dynamic_cast<CodexEditor*> (editor.get()))
    {
        hk->setSize (MainView::baseWidth, MainView::baseHeight);
        hk->showTab (tab);
        if (overlay == 1) hk->getView().openAbout();
        if (overlay == 2) hk->getView().openSaveDialog();
        if (overlay == 3) hk->getView().openBrowser();
    }
    // feed analyzer + tuner with the tail of a kick
    for (int i = 0; i < 6; ++i)
    {
        proc.analyzer.push (audio.getReadPointer (0, (int) (0.05 * sr) + i * 2400), 4096);
        proc.tunerFeed.push (audio.getReadPointer (0, (int) (0.1 * sr) + i * 2400), 2400);
        proc.scopeFeedL.push (audio.getReadPointer (0, (int) (0.05 * sr) + i * 2400), 2400);
        proc.scopeFeedR.push (audio.getReadPointer (1, (int) (0.05 * sr) + i * 2400), 2400);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
    }
    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    png.deleteFile();
    juce::FileOutputStream os (png);
    juce::PNGImageFormat().writeImageToStream (img, os);
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: CodexRender <outdir>\n"; return 1; }
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    outDir.createDirectory();
    int failures = 0;

    CodexProcessor proc;
    proc.setPlayConfigDetails (0, 2, sr, block);
    std::cout << "Latency: " << proc.getLatencySamples() << " samples\n\n";

    // ---- All factory presets in the key of F (F1 = 43.65 Hz) ----
    std::cout << juce::String ("Preset").paddedRight (' ', 26) << "  peak dBFS   tail pitch   cents\n";
    const float targetHz = midiToFrequency (29.0f);
    for (int i = 0; i < proc.presets.getNumFactory(); ++i)
    {
        proc.presets.loadPreset (i);
        auto audio = render (proc, 2);
        bool finite = true;
        for (int s = 0; s < audio.getNumSamples(); ++s) finite &= std::isfinite (audio.getSample (0, s));
        const float pk = juce::Decibels::gainToDecibels (audio.getMagnitude (0, 0, audio.getNumSamples()));
        const float hz = measurePitch (audio, 0.15, 0.17);
        const float cents = hz > 0 ? 1200.0f * std::log2 (hz / targetHz) : 0.0f;
        const bool ok = finite && pk < 1.0f && pk > -30.0f && hz > 0 && std::abs (cents) < 25.0f;
        failures += ok ? 0 : 1;
        std::cout << proc.presets.getFactoryNames()[i].paddedRight (' ', 26) << juce::String (pk, 1).paddedLeft (' ', 9)
                  << juce::String (hz, 2).paddedLeft (' ', 10) << " Hz" << juce::String (cents, 1).paddedLeft (' ', 8)
                  << (ok ? "" : "   <-- CHECK") << "\n";
        writeWav (audio, outDir.getChildFile (juce::File::createLegalFileName (proc.presets.getFactoryNames()[i]) + ".wav"));
    }

    // ---- Preset switching restores everything (e.g. distortion re-enabled after "909 Clean") ----
    {
        int bad = 0;
        for (int i = 0; i < proc.presets.getNumFactory(); ++i)
        {
            proc.presets.loadPreset (7);   // 909 Clean: both distortions off
            proc.presets.loadPreset (i);
            const bool d1 = proc.apvts.getRawParameterValue (ID::clip (1, "On"))->load() > 0.5f;
            const bool d2 = proc.apvts.getRawParameterValue (ID::clip (2, "On"))->load() > 0.5f;
            const bool expectOff = proc.presets.getFactoryNames()[i] == "909 Clean";
            if ((d1 && d2) == expectOff) { ++bad; std::cout << "   distortion state wrong after loading " << proc.presets.getFactoryNames()[i] << "\n"; }
        }
        std::cout << "\nPreset switching after '909 Clean' re-enables distortion: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- EQ: up to 24 bands (Pro-Q 4 limit), add / bypass / delete, older states ----
    {
        int bad = 0;
        proc.presets.loadPreset (0);
        auto& st = proc.apvts;
        for (int b = 0; b < Params::numBands; ++b) EQDisplay::deleteBand (st, 0, b);
        int added = 0;
        for (int i = 0; i < Params::numBands; ++i)
            added += EQDisplay::addBand (st, 0, eq::BandType::Bell, 30.0f * std::pow (2.0f, (float) i * 0.4f), i % 2 ? 1.5f : -1.5f, 2.0f) >= 0 ? 1 : 0;
        if (added != 24 || EQDisplay::numPresent (st, 0) != 24) ++bad;
        if (EQDisplay::addBand (st, 0, eq::BandType::Bell, 1000.0f) != -1) ++bad;            // 25th band refused
        const auto full = render (proc, 1);
        const float peak = full.getMagnitude (0, 0, full.getNumSamples());
        if (! std::isfinite (peak) || peak > 4.0f) ++bad;
        EQDisplay::setBandOn (st, 0, 3, false);                                                // bypass keeps it
        if (! EQDisplay::isPresent (st, 0, 3) || EQDisplay::numPresent (st, 0) != 24) ++bad;
        EQDisplay::deleteBand (st, 0, 3);                                                      // delete frees the slot
        if (EQDisplay::isPresent (st, 0, 3) || EQDisplay::firstFreeBand (st, 0) != 3) ++bad;
        // State from before "Used" existed: a band that is only switched On must still be present
        for (int b = 0; b < Params::numBands; ++b) EQDisplay::deleteBand (st, 0, b);
        setParam (proc, ID::band (2, "On"), 1.0f);
        if (! EQDisplay::isPresent (st, 0, 2) || EQDisplay::numPresent (st, 0) != 1) ++bad;
        std::cout << "\nEQ 24 bands (add, limit, bypass, delete, legacy state): " << (bad == 0 ? "OK" : "FAILED")
                  << "  (24-band peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1) << " dBFS)\n";
        failures += bad;
        proc.presets.loadPreset (0);
    }

    // ---- Space reverb: stability, decay time, low cut, bypass ----
    {
        int bad = 0;
        auto impulse = [] (space::SpaceReverb& rv, double seconds, float freq = 0.0f)
        {
            const int n = (int) (seconds * sr);
            juce::AudioBuffer<float> b (2, n);
            b.clear();
            if (freq > 0.0f)   // 100 ms sine burst
                for (int i = 0; i < (int) (0.1 * sr); ++i)
                    b.setSample (0, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * freq * (float) i / (float) sr)), b.setSample (1, i, b.getSample (0, i));
            else
                b.setSample (0, 0, 1.0f), b.setSample (1, 0, 1.0f);
            for (int pos = 0; pos < n; pos += 256)
                rv.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, n - pos));
            return b;
        };
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, double t0, double t1)
        {
            const int s0 = (int) (t0 * sr), s1 = (int) (t1 * sr);
            return juce::Decibels::gainToDecibels (0.5f * (b.getRMSLevel (0, s0, s1 - s0) + b.getRMSLevel (1, s0, s1 - s0)), -200.0f);
        };

        // every mode with extreme settings stays finite and bounded
        for (int m = 0; m < 6; ++m)
        {
            space::SpaceReverb rv; rv.prepare (sr);
            space::Settings st; st.enabled = true; st.mode = m; st.mix = 1.0f; st.decay = 20.0f; st.bassMult = 4.0f; st.highMult = 2.0f;
            st.lSize = 1.0f; st.eSize = 1.0f; st.lDiffusion = 1.0f; st.eDiffusion = 1.0f; st.lModDepth = 1.0f; st.eModDepth = 1.0f; st.lowCut = 20.0f; st.duck = 0.0f;
            rv.setSettings (st);
            auto b = impulse (rv, 8.0, 80.0f);
            const float pk = b.getMagnitude (0, b.getNumSamples());
            if (! std::isfinite (pk) || pk > 4.0f) { ++bad; std::cout << "   mode " << m << " unstable, peak " << pk << "\n"; }
        }

        // measured RT60 roughly follows Decay (no ducking, flat decay EQ, 1 kHz)
        std::cout << "\nSpace decay   set     measured RT60\n";
        for (float d : { 0.6f, 1.5f, 4.0f })
        {
            space::SpaceReverb rv; rv.prepare (sr);
            space::Settings st; st.enabled = true; st.mix = 1.0f; st.decay = d; st.bassMult = 1.0f; st.highMult = 1.0f; st.duck = 0.0f;
            st.highCut = 20000.0f; st.predelayMs = 0.0f; st.depth = 1.0f; st.mode = 1;
            rv.setSettings (st);
            auto b = impulse (rv, d * 2.0 + 1.0, 1000.0f);
            // slope between two windows in the late decay -> RT60
            const double t0 = 0.25, t1 = 0.25 + d * 0.5;
            const float drop = rmsDb (b, t0, t0 + 0.05) - rmsDb (b, t1, t1 + 0.05);
            const float measured = (float) (60.0 * (t1 - t0) / juce::jmax (0.1f, drop));
            std::cout << "              " << juce::String (d, 2).paddedRight (' ', 7) << " " << juce::String (measured, 2) << " s\n";
            if (measured < d * 0.6f || measured > d * 1.6f) ++bad;
        }

        // the low cut keeps the sub out of the tail
        {
            space::SpaceReverb rv; rv.prepare (sr);
            space::Settings st; st.enabled = true; st.mix = 1.0f; st.lowCut = 200.0f; st.duck = 0.0f; st.depth = 1.0f;
            rv.setSettings (st);
            auto sub = impulse (rv, 1.0, 45.0f);
            space::SpaceReverb rv2; rv2.prepare (sr); rv2.setSettings (st);
            auto mid = impulse (rv2, 1.0, 1000.0f);
            const float diff = rmsDb (mid, 0.15, 0.6) - rmsDb (sub, 0.15, 0.6);
            std::cout << "Space low cut: 45 Hz tail " << juce::String (diff, 1) << " dB below 1 kHz tail\n";
            if (diff < 12.0f) ++bad;
        }

        // switched off: bit-exact passthrough; in the plugin: off by default and the tail rings on
        {
            space::SpaceReverb rv; rv.prepare (sr);
            juce::AudioBuffer<float> b (2, 512);
            for (int i = 0; i < 512; ++i) b.setSample (0, i, std::sin ((float) i * 0.1f)), b.setSample (1, i, std::cos ((float) i * 0.1f));
            juce::AudioBuffer<float> ref (b);
            rv.process (b.getWritePointer (0), b.getWritePointer (1), 512);
            for (int i = 0; i < 512; ++i) if (b.getSample (0, i) != ref.getSample (0, i) || b.getSample (1, i) != ref.getSample (1, i)) { ++bad; break; }
            if (proc.apvts.getRawParameterValue (ID::spc ("On"))->load() > 0.5f) ++bad;
        }
        {
            proc.presets.loadPreset (8);
            setParam (proc, ID::spc ("On"), 1.0f); setParam (proc, ID::spc ("Mix"), 0.3f); setParam (proc, ID::spc ("Decay"), 2.0f);
            const auto withSpace = render (proc, 1);
            const float tail = withSpace.getRMSLevel (1, withSpace.getNumSamples() - (int) (0.2 * sr), (int) (0.2 * sr));
            const float sideRms = [&] { double s = 0; for (int i = 0; i < withSpace.getNumSamples(); ++i) { const float d = withSpace.getSample (0, i) - withSpace.getSample (1, i); s += d * d; } return (float) std::sqrt (s / withSpace.getNumSamples()); }();
            std::cout << "Space in the plugin: tail level " << juce::String (juce::Decibels::gainToDecibels (tail), 1)
                      << " dBFS, side (width) " << juce::String (juce::Decibels::gainToDecibels (sideRms), 1) << " dBFS, tail length "
                      << juce::String (proc.getTailLengthSeconds(), 2) << " s\n";
            if (sideRms < 1.0e-3f || proc.getTailLengthSeconds() < 2.0) ++bad;
            proc.presets.loadPreset (8);
            if (proc.apvts.getRawParameterValue (ID::spc ("On"))->load() > 0.5f) { ++bad; std::cout << "   preset did not reset Space\n"; }
        }
        std::cout << "Space reverb checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- Compressor: static curves (down / up / both), multiband flatness, styles, lookahead ----
    {
        int bad = 0;
        auto sine = [] (float dbfs, double seconds)
        {
            juce::AudioBuffer<float> b (2, (int) (seconds * sr));
            const float a = juce::Decibels::decibelsToGain (dbfs);
            for (int i = 0; i < b.getNumSamples(); ++i)
            { const float v = a * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) sr); b.setSample (0, i, v); b.setSample (1, i, v); }
            return b;
        };
        auto run = [] (comp::MultibandCompressor& c, juce::AudioBuffer<float> b)
        {
            for (int pos = 0; pos < b.getNumSamples(); pos += 256)
                c.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, b.getNumSamples() - pos));
            return b;
        };
        auto peakDb = [] (const juce::AudioBuffer<float>& b) { return juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples() - (int) (0.1 * sr), (int) (0.1 * sr))); };
        auto make = [] (int dir, float thr, float ratio, int style = comp::Clean)
        {
            comp::Settings s; s.enabled = true; s.style = style; s.autoGain = false;
            auto& b = s.bands[0]; b.direction = dir; b.thresholdDb = thr; b.ratio = ratio; b.kneeDb = 0.0f; b.rangeDb = 40.0f; b.attackMs = 1.0f; b.releaseMs = 50.0f;
            for (auto& o : s.bands) o = b;
            return s;
        };
        struct Case { const char* name; int dir; float in, thr, ratio, expect; };
        const Case cases[] = { { "Downward 4:1, -6 dB in, -18 thr", 0, -6.0f, -18.0f, 4.0f, -15.0f },
                               { "Upward 2:1, -40 dB in, -18 thr  ", 1, -40.0f, -18.0f, 2.0f, -29.0f },
                               { "Both 4:1, -40 dB in, -18 thr    ", 2, -40.0f, -18.0f, 4.0f, -23.5f },
                               { "Both 4:1, -6 dB in, -18 thr     ", 2, -6.0f, -18.0f, 4.0f, -15.0f } };
        std::cout << "\nCompressor static curve                 expected   measured (peak dBFS)\n";
        for (auto& c : cases)
        {
            comp::MultibandCompressor cp; cp.prepare (sr); cp.setSettings (make (c.dir, c.thr, c.ratio));
            const float got = peakDb (run (cp, sine (c.in, 1.5)));
            std::cout << "  " << c.name << "   " << juce::String (c.expect, 1).paddedLeft (' ', 7) << "   " << juce::String (got, 1).paddedLeft (' ', 7) << "\n";
            if (std::abs (got - c.expect) > 1.0f) ++bad;
        }
        // 4 bands at 1:1 sum flat (crossovers + all-pass compensation)
        {
            comp::Settings s = make (0, -18.0f, 1.0f); s.numBands = 4;
            float worst = 0.0f;
            for (float f : { 60.0f, 400.0f, 2500.0f, 9000.0f })
            {
                comp::MultibandCompressor cp; cp.prepare (sr); cp.setSettings (s);
                juce::AudioBuffer<float> b (2, (int) sr);
                for (int i = 0; i < b.getNumSamples(); ++i) { const float v = 0.5f * std::sin (juce::MathConstants<float>::twoPi * f * (float) i / (float) sr); b.setSample (0, i, v); b.setSample (1, i, v); }
                worst = std::max (worst, std::abs (peakDb (run (cp, b)) - juce::Decibels::gainToDecibels (0.5f)));
            }
            std::cout << "  4-band split at 1:1, max deviation " << juce::String (worst, 2) << " dB\n";
            if (worst > 0.5f) ++bad;
        }
        // every style on a kick: finite, and actually reduces the peak (downward)
        for (int st = 0; st < comp::numStyles; ++st)
        {
            comp::MultibandCompressor cp; cp.prepare (sr);
            auto s = make (0, -24.0f, 6.0f, st); s.numBands = 3; s.lookahead = 2;
            cp.setSettings (s);
            auto b = run (cp, sine (-3.0f, 1.0));
            const float pk = b.getMagnitude (0, b.getNumSamples());
            if (! std::isfinite (pk) || pk > 2.0f || peakDb (b) > -6.0f) { ++bad; std::cout << "   style " << comp::styleNames()[st] << " peak " << peakDb (b) << "\n"; }
            if (cp.getLatencyInSamples() != (int) std::round (0.002 * sr)) ++bad;
        }
        std::cout << "Compressor checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- EQ processing modes (Zero Latency / Natural Phase / Linear Phase) and EQ 2 ----
    {
        int bad = 0;
        auto sineGain = [] (eq::EqEngine& e, float f, bool side, int latency)
        {
            const int n = (int) (sr * 1.0);
            juce::AudioBuffer<float> b (2, n);
            for (int i = 0; i < n; ++i)
            {
                const float v = 0.25f * std::sin (juce::MathConstants<float>::twoPi * f * (float) i / (float) sr);
                b.setSample (0, i, v); b.setSample (1, i, side ? -v : v);
            }
            for (int pos = 0; pos < n; pos += 256) e.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, n - pos));
            juce::ignoreUnused (latency);
            return juce::Decibels::gainToDecibels (b.getMagnitude (0, n - (int) (0.3 * sr), (int) (0.25 * sr)) / 0.25f);
        };
        auto settle = [] (eq::EqEngine& e)
        {
            for (int i = 0; i < 500 && ! e.isLinearUpToDate(); ++i) juce::Thread::sleep (10);
            juce::AudioBuffer<float> z (2, 256);
            for (int i = 0; i < 200; ++i) { z.clear(); e.process (z.getWritePointer (0), z.getWritePointer (1), 256); juce::Thread::sleep (1); }
        };
        std::cout << "\nEQ processing modes      +6 dB bell @ 200 Hz   side low cut (50 Hz S / M)   latency\n";
        for (int mode = 0; mode < 3; ++mode)
        {
            eq::EqEngine e; e.prepare (sr, 256);
            eq::BandSettings bell; bell.on = true; bell.type = eq::BandType::Bell; bell.freq = 200.0f; bell.gainDb = 6.0f; bell.q = 1.0f;
            eq::BandSettings cut;  cut.on = true; cut.type = eq::BandType::LowCut; cut.freq = 150.0f; cut.slope = 5; cut.placement = eq::Placement::Side;
            e.setBand (0, bell); e.setBand (1, cut);
            e.setMode (mode, 1);
            settle (e);
            const float g200 = sineGain (e, 200.0f, false, 0);
            const float s50 = sineGain (e, 50.0f, true, 0), m50 = sineGain (e, 50.0f, false, 0);
            const int lat = e.getLatencyInSamples();
            std::cout << "  " << juce::String (eq::processingModeNames()[mode]).paddedRight (' ', 22) << juce::String (g200, 2).paddedLeft (' ', 8) << " dB"
                      << juce::String (s50, 1).paddedLeft (' ', 16) << " / " << juce::String (m50, 1) << " dB" << juce::String (lat).paddedLeft (' ', 14) << " samples\n";
            if (std::abs (g200 - 6.0f) > 0.3f || s50 > -30.0f || std::abs (m50 - 0.4f) > 0.3f) ++bad;   // 0.4 dB = skirt of the bell at 50 Hz
            if (mode == 0 && lat != 0) ++bad;
            if (mode == 2 && lat != eq::firLength (1, sr) / 2) ++bad;
        }
        // linear phase: impulse response symmetric around the latency (no phase shift)
        {
            eq::EqEngine e; e.prepare (sr, 256);
            eq::BandSettings bell; bell.on = true; bell.type = eq::BandType::Bell; bell.freq = 120.0f; bell.gainDb = 9.0f; bell.q = 0.8f;
            e.setBand (0, bell); e.setMode (2, 0); settle (e);
            const int lat = e.getLatencyInSamples(), n = lat * 2 + 512;
            juce::AudioBuffer<float> b (2, n); b.clear(); b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f);
            for (int pos = 0; pos < n; pos += 256) e.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, n - pos));
            float asym = 0.0f, peak = 0.0f;
            for (int k = 1; k < lat; ++k) asym = std::max (asym, std::abs (b.getSample (0, lat + k) - b.getSample (0, lat - k)));
            for (int i = 0; i < n; ++i) peak = std::max (peak, std::abs (b.getSample (0, i)));
            const bool peakAtLatency = std::abs (b.getSample (0, lat)) >= peak * 0.999f;
            std::cout << "  Linear phase impulse: peak at the latency " << (peakAtLatency ? "yes" : "NO") << ", max asymmetry " << asym << "\n";
            if (! peakAtLatency || asym > 1.0e-3f) ++bad;
        }
        // EQ 2 in the plugin: a band changes the output; latency is reported for linear phase
        {
            proc.presets.loadPreset (8);
            const auto before = render (proc, 1);
            EQDisplay::addBand (proc.apvts, 1, eq::BandType::LowCut, 200.0f);
            const auto after = render (proc, 1);
            const float d = juce::Decibels::gainToDecibels (after.getRMSLevel (0, 0, after.getNumSamples()) / before.getRMSLevel (0, 0, before.getNumSamples()));
            setParam (proc, ID::eqMode (1), 2.0f);
            render (proc, 1);
            const int lat = proc.getLatencySamples();
            std::cout << "  EQ 2 low cut at 200 Hz changes the kick by " << juce::String (d, 2) << " dB; linear-phase EQ 2 latency " << lat << " samples\n";
            if (std::abs (d) < 0.2f || lat < eq::firLength (1, sr) / 2) ++bad;
            proc.presets.loadPreset (8);
            if (EQDisplay::numPresent (proc.apvts, 1) != 0 || proc.apvts.getRawParameterValue (ID::eqMode (1))->load() != 0.0f) { ++bad; std::cout << "   preset did not reset EQ 2\n"; }
        }
        std::cout << "EQ mode checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- Drawn (breakpoint) envelopes ----
    {
        int bad = 0;
        proc.presets.loadPreset (0);
        setParam (proc, ID::envMode ('p'), 1.0f);
        setParam (proc, ID::envMode ('a'), 1.0f);
        const auto audio = render (proc, 1);
        const float target = juce::MidiMessage::getMidiNoteInHertz (Params::keyBaseNote);
        const float tail = measurePitch (audio, 0.36, 0.40);   // default pitch drawing lands on the root at 320 ms
        const float early = measurePitch (audio, 0.03, 0.05);
        // after note-off (0.35 s) the release gate fades the drawn amp envelope out
        const float lateRms = audio.getRMSLevel (0, (int) (0.55 * sr), (int) (0.05 * sr));
        std::cout << "\nDraw envelopes: early pitch " << juce::String (early, 1) << " Hz, tail pitch " << juce::String (tail, 2)
                  << " Hz (target " << juce::String (target, 2) << "), level after note-off " << juce::String (juce::Decibels::gainToDecibels (lateRms), 1) << " dBFS\n";
        if (std::abs (1200.0f * std::log2 (tail / target)) > 25.0f || early < target * 1.5f) ++bad;
        // DrawEnvelope maths: bend 0 is linear, positive bend drops faster, last level holds
        DrawEnvelope::Shape sh; sh.count = 2; sh.points[0] = { 0.0f, 1.0f, 0.0f }; sh.points[1] = { 100.0f, 0.0f, 0.0f };
        const float lin = DrawEnvelope::valueAt (sh, 50.0f);
        sh.points[0].bend = 0.8f;
        const float bent = DrawEnvelope::valueAt (sh, 50.0f), held = DrawEnvelope::valueAt (sh, 500.0f);
        std::cout << "  linear midpoint " << lin << ", bent midpoint " << bent << ", after last point " << held << "\n";
        if (std::abs (lin - 0.5f) > 1.0e-4f || bent > 0.2f || held != 0.0f) ++bad;
        std::cout << "Draw envelope checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        proc.presets.loadPreset (0);
        if (proc.apvts.getRawParameterValue (ID::envMode ('p'))->load() > 0.5f) { ++failures; std::cout << "   preset did not reset envelope mode\n"; }
    }

    // ---- Macros: each one moves the sound in its direction, centre = untouched ----
    {
        int bad = 0;
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, double t0, double t1)
        { return juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) (t0 * sr), (int) ((t1 - t0) * sr)), -120.0f); };
        proc.presets.loadPreset (7);   // 909 Clean: no distortion, so level changes are not flattened by clipping
        const auto base = render (proc, 1);
        setParam (proc, ID::macro (2), 1.0f);
        const auto longTail = render (proc, 1);
        setParam (proc, ID::macro (2), 0.0f);
        const auto shortTail = render (proc, 1);
        setParam (proc, ID::macro (2), 0.5f);
        // Punch on a noise-free synth kick (the 909's attack noise would randomise the zero crossings)
        proc.presets.loadPreset (0);
        setParam (proc, ID::clickLevel, 0.0f);
        const auto punchBase = render (proc, 1);
        setParam (proc, ID::macro (0), 1.0f);
        const auto punchy = render (proc, 1);
        setParam (proc, ID::macro (0), 0.5f);
        proc.presets.loadPreset (7);
        const auto again = render (proc, 1);
        proc.presets.loadPreset (2);   // Oldskool Raw: Width acts on the distortion (Spatial)
        const auto distorted = render (proc, 1);
        setParam (proc, ID::macro (3), 1.0f);
        const auto wide = render (proc, 1);
        setParam (proc, ID::macro (3), 0.5f);
        auto sideDb = [] (const juce::AudioBuffer<float>& b)
        {
            double s2 = 0; for (int i = 0; i < b.getNumSamples(); ++i) { const double d = b.getSample (0, i) - b.getSample (1, i); s2 += d * d; }
            return juce::Decibels::gainToDecibels ((float) std::sqrt (s2 / b.getNumSamples()), -120.0f);
        };
        // tail after note-off (0.35 s); punch = zero crossings in the first 8 ms (a deeper, harder sweep);
        // back at the centre the sound must match the original (RMS: the click is random noise)
        const float tBase = rmsDb (base, 0.42, 0.55), tLong = rmsDb (longTail, 0.42, 0.55), tShort = rmsDb (shortTail, 0.42, 0.55);
        auto crossings = [] (const juce::AudioBuffer<float>& b)
        {
            int c = 0; for (int i = 1; i < (int) (0.008 * sr); ++i) c += (b.getSample (0, i - 1) < 0.0f) != (b.getSample (0, i) < 0.0f) ? 1 : 0;
            return (float) c;
        };
        const float pBase = crossings (punchBase), pPunch = crossings (punchy);
        const float diff = std::abs (rmsDb (base, 0.0, 0.6) - rmsDb (again, 0.0, 0.6));
        std::cout << "\nMacros: tail level " << juce::String (tShort, 1) << " / " << juce::String (tBase, 1) << " / " << juce::String (tLong, 1)
                  << " dB (Tail -100 / 0 / +100 %), zero crossings in 8 ms " << pBase << " -> " << pPunch
                  << " (Punch +100 %), side " << juce::String (sideDb (distorted), 1) << " -> " << juce::String (sideDb (wide), 1)
                  << " dB (Width +100 %), back to centre: " << diff << " dB\n";
        if (! (tShort < tBase - 1.0f && tLong > tBase + 0.5f) || pPunch <= pBase || sideDb (wide) < sideDb (distorted) + 10.0f || diff > 0.05f) ++bad;
        std::cout << "Macro checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- CPU: realtime factor for a typical and a worst-case setup ----
    {
        auto bench = [&] (const char* name)
        {
            const int n = (int) (sr * 4.0);
            juce::AudioBuffer<float> buf (2, block);
            juce::MidiBuffer midi;
            proc.prepareToPlay (sr, block);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int pos = 0; pos < n; pos += block)
            {
                midi.clear();
                if (pos % (int) (sr * 0.4) < block) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                buf.clear();
                proc.processBlock (buf, midi);
            }
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            const double load = ms / 4000.0 * 100.0;
            std::cout << "  " << juce::String (name).paddedRight (' ', 46) << juce::String (load, 1).paddedLeft (' ', 6) << " % of one core\n";
            return load;
        };
        std::cout << "\nCPU (4 s of kicks at 48 kHz, this machine)\n";
        proc.presets.loadPreset (8);
        bench ("909 Oldskool Hardstyle (default settings)");
        proc.presets.loadPreset (19);   // Screech Rawstyle
        bench ("Screech Rawstyle (filters, compressor)");
        for (int st = 1; st <= 2; ++st) { setParam (proc, ID::clip (st, "OS"), 4.0f); setParam (proc, ID::clip (st, "FltPos"), 1.0f); }
        for (int e = 0; e < 2; ++e)
        {
            for (int b = 0; b < Params::numBands; ++b) EQDisplay::addBand (proc.apvts, e, eq::BandType::Bell, 40.0f * std::pow (1.3f, (float) b), 1.0f, 2.0f);
            setParam (proc, ID::eqMode (e), 2.0f);
            setParam (proc, ID::eqRes (e), 2.0f);
        }
        setParam (proc, ID::sat ("On"), 1.0f); setParam (proc, ID::sat ("Bands"), 6.0f); setParam (proc, ID::sat ("OS"), 3.0f);
        setParam (proc, ID::cmp ("On"), 1.0f); setParam (proc, ID::cmp ("Bands"), 4.0f); setParam (proc, ID::cmp ("Look"), 4.0f);
        setParam (proc, ID::spc ("On"), 1.0f);
        render (proc, 1);   // let the linear-phase filters build
        juce::Thread::sleep (300);
        const double worst = bench ("Worst case (everything on, 16x, 2 x 24-band LP EQ)");
        if (worst > 400.0) { ++failures; std::cout << "   worst case far too slow\n"; }
        proc.presets.loadPreset (0);
    }

    // ---- Output clipper: the ceiling holds (also between samples), off = untouched ----
    {
        int bad = 0;
        proc.presets.loadPreset (10);
        setParam (proc, ID::outGain, 12.0f);
        const auto hot = render (proc, 1);
        setParam (proc, ID::clipOn, 1.0f);
        setParam (proc, ID::clipCeiling, -1.0f);
        const auto clipped = render (proc, 1);
        const float hotPk = juce::Decibels::gainToDecibels (hot.getMagnitude (0, hot.getNumSamples()));
        const float clipPk = juce::Decibels::gainToDecibels (clipped.getMagnitude (0, clipped.getNumSamples()));
        // inter-sample peak estimate: 8x upsampled
        juce::dsp::Oversampling<float> up (2, 3, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        up.initProcessing ((size_t) clipped.getNumSamples());
        juce::AudioBuffer<float> copy (clipped);
        juce::dsp::AudioBlock<float> blk (copy);
        auto u = up.processSamplesUp (blk);
        float isp = 0.0f;
        for (size_t i = 0; i < u.getNumSamples(); ++i) isp = std::max (isp, std::abs (u.getSample (0, (int) i)));
        std::cout << "\nOutput clipper: +12 dB hot peak " << juce::String (hotPk, 1) << " dBFS -> " << juce::String (clipPk, 2)
                  << " dBFS (ceiling -1.0), inter-sample peak " << juce::String (juce::Decibels::gainToDecibels (isp), 2) << " dBFS, latency "
                  << proc.getLatencySamples() << " samples\n";
        if (clipPk > -0.99f || hotPk < 3.0f || juce::Decibels::gainToDecibels (isp) > 0.0f) ++bad;
        std::cout << "Output clipper checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        proc.presets.loadPreset (10);
        if (proc.apvts.getRawParameterValue (ID::clipOn)->load() > 0.5f) { ++failures; std::cout << "   preset did not reset the clipper\n"; }
    }

    // ---- Every key, both engines ----
    std::cout << "\nKey   target Hz   synth Hz   909 Hz\n";
    for (int k = 0; k < 12; ++k)
    {
        setParam (proc, ID::key, (float) k);
        const float t = midiToFrequency ((float) (29 + k));
        float res[2];
        for (int e = 0; e < 2; ++e)
        {
            proc.presets.loadPreset (e == 0 ? 0 : 7); // Init (synth) and 909 Clean
            setParam (proc, ID::key, (float) k);
            res[e] = measurePitch (render (proc, 1), 0.15, 0.17);
            if (std::abs (1200.0f * std::log2 (res[e] / t)) > 25.0f) ++failures;
        }
        std::cout << juce::String (Params::keyNames()[k]).paddedRight (' ', 4) << juce::String (t, 2).paddedLeft (' ', 10)
                  << juce::String (res[0], 2).paddedLeft (' ', 11) << juce::String (res[1], 2).paddedLeft (' ', 9) << "\n";
    }
    setParam (proc, ID::key, 0);

    // ---- Distortion: every oversampling setting, extreme characteristic / filters ----
    std::cout << "\nOversampling   latency   peak dBFS\n";
    for (int os = 0; os < 5; ++os)
    {
        proc.presets.loadPreset (10); // 909 Rawstyle
        for (int st = 1; st <= 2; ++st)
        {
            setParam (proc, ID::clip (st, "OS"), (float) os);
            setParam (proc, ID::clip (st, "Char"), 1.0f);
            setParam (proc, ID::clip (st, "Boost"), 18.0f);
            setParam (proc, ID::clip (st, "LoFilt"), 400.0f);
            setParam (proc, ID::clip (st, "HiFilt"), 1500.0f);
            setParam (proc, ID::clip (st, "ShelfG"), 18.0f);
        }
        auto audio = render (proc, 1);
        bool finite = true;
        for (int i = 0; i < audio.getNumSamples(); ++i) finite &= std::isfinite (audio.getSample (0, i));
        const float pk = juce::Decibels::gainToDecibels (audio.getMagnitude (0, 0, audio.getNumSamples()));
        if (! finite || pk > 12.0f) ++failures;
        std::cout << ClipDistortion::oversamplingNames()[os].paddedRight (' ', 15) << juce::String (proc.getLatencySamples()).paddedRight (' ', 10)
                  << juce::String (pk, 1) << (finite ? "" : "  NaN!") << "\n";
    }

    // ---- Distortion engine: aliasing per oversampling, resonant pre/post filter, X-Shaper, new modes ----
    {
        int bad = 0;
        // aliasing: 5 kHz sine, hard clip +24 dB; energy outside the harmonic series (odd multiples of 5 kHz, folded) in dB
        auto runDist = [] (ClipDistortion::Settings st, juce::AudioBuffer<float> b)
        {
            ClipDistortion d; d.prepare (sr, 256); d.setSettings (st); d.reset(); d.setSettings (st);
            for (int pos = 0; pos < b.getNumSamples(); pos += 256)
                d.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, b.getNumSamples() - pos));
            return b;
        };
        auto sineBuf = [] (float f, float amp, int n)
        {
            juce::AudioBuffer<float> b (2, n);
            for (int i = 0; i < n; ++i)
            {
                // double-precision phase: a float phase adds noise around -60 dB over long buffers
                const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * std::fmod ((double) f * i / sr, 1.0));
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            return b;
        };
        auto aliasDb = [&] (int os)
        {
            ClipDistortion::Settings st; st.mode = ClipDistortion::HardClip; st.driveDb = 24.0f; st.outDb = 0.0f; st.oversampling = os; st.dcFilter = false;
            const int N = 1 << 15;
            const float f0 = 5000.0f * (float) N / (float) sr;   // put the tone exactly on a bin
            const float f = std::round (f0) * (float) sr / (float) N;
            auto out = runDist (st, sineBuf (f, 0.5f, N + 8192));
            juce::dsp::FFT fft (15);
            std::vector<float> data ((size_t) N * 2, 0.0f);
            for (int i = 0; i < N; ++i)
            {
                // 4-term Blackman-Harris (-92 dB sidelobes) so window leakage does not mask the aliasing
                const double a = juce::MathConstants<double>::twoPi * i / N;
                const double w = 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a);
                data[(size_t) i] = out.getSample (0, i + 8192) * (float) w;
            }
            fft.performFrequencyOnlyForwardTransform (data.data());
            const int k0 = (int) std::round (f0);
            double harm = 0.0, alias = 0.0;
            std::vector<bool> isHarm ((size_t) N / 2, false);
            // only true harmonics (below Nyquist) count as wanted; everything that folded back is aliasing
            for (int h = 1; (long long) h * k0 < N / 2; h += 2)
            {
                const long long k = (long long) h * k0;
                for (int d = -6; d <= 6; ++d) if (k + d >= 0 && k + d < N / 2) isHarm[(size_t) (k + d)] = true;
            }
            const int audible = (int) (20000.0 * N / sr);   // aliasing that folds above 20 kHz is inaudible
            for (int k = 4; k < N / 2; ++k)
                if (isHarm[(size_t) k]) harm += (double) data[(size_t) k] * data[(size_t) k];
                else if (k < audible) alias += (double) data[(size_t) k] * data[(size_t) k];
            return (float) (10.0 * std::log10 (alias / juce::jmax (1.0e-30, harm)));
        };
        std::cout << "\nDistortion aliasing (5 kHz sine, hard clip +24 dB): audible (< 20 kHz) aliasing relative to the harmonics\n";
        float prev = 100.0f;
        for (int os = 0; os < 5; ++os)
        {
            const float a = aliasDb (os);
            std::cout << "  " << ClipDistortion::oversamplingNames()[os].paddedRight (' ', 6) << juce::String (a, 1).paddedLeft (' ', 8) << " dB\n";
            if (a > prev + 1.0f) ++bad;   // more oversampling must never alias more
            prev = a;
        }
        if (prev > -70.0f) ++bad;

        // resonant BP before the curve focuses the distortion: output energy at the BP frequency vs far away
        {
            ClipDistortion::Settings st; st.mode = ClipDistortion::Tanh; st.driveDb = 0.0f; st.outDb = 0.0f; st.oversampling = 1;
            st.filterPos = 1; st.filterType = ClipDistortion::BandPass; st.filterHz = 1000.0f; st.filterRes = 0.6f;
            const auto at = juce::Decibels::gainToDecibels (runDist (st, sineBuf (1000.0f, 0.1f, 24000)).getMagnitude (0, 12000, 12000) / 0.1f);
            const auto away = juce::Decibels::gainToDecibels (runDist (st, sineBuf (100.0f, 0.1f, 24000)).getMagnitude (0, 12000, 12000) / 0.1f);
            st.filterType = ClipDistortion::LowPass24; st.filterHz = 300.0f; st.filterRes = 0.0f; st.filterPos = 2;
            const auto lp = juce::Decibels::gainToDecibels (runDist (st, sineBuf (3000.0f, 0.1f, 24000)).getMagnitude (0, 12000, 12000) / 0.1f);
            std::cout << "  Pre band-pass 1 kHz: " << juce::String (at, 1) << " dB at 1 kHz, " << juce::String (away, 1)
                      << " dB at 100 Hz; post LP24 300 Hz: " << juce::String (lp, 1) << " dB at 3 kHz\n";
            if (std::abs (at) > 1.0f || away > -15.0f || lp > -30.0f) ++bad;
        }
        // X-Shaper with the default (linear) points equals Hard Clip; Asym halves are independent
        {
            ClipDistortion::Curve cv;
            float worst = 0.0f;
            for (float x = -2.0f; x <= 2.0f; x += 0.01f)
                worst = std::max (worst, std::abs (ClipDistortion::transfer (ClipDistortion::XShaper, x, 1.0f, 0.0f, 0.0f, &cv)
                                                   - ClipDistortion::transfer (ClipDistortion::HardClip, x, 1.0f, 0.0f, 0.0f)));
            cv.neg.fill (0.25f);
            const float yNeg = ClipDistortion::transfer (ClipDistortion::XShaperAsym, -1.0f, 1.0f, 0.0f, 0.0f, &cv);
            const float yPos = ClipDistortion::transfer (ClipDistortion::XShaperAsym, 1.0f, 1.0f, 0.0f, 0.0f, &cv);
            std::cout << "  X-Shaper: default curve vs hard clip max diff " << worst << "; asym halves " << yPos << " / " << yNeg << "\n";
            if (worst > 1.0e-4f || std::abs (yNeg + 0.25f) > 1.0e-4f || std::abs (yPos - 1.0f) > 1.0e-4f) ++bad;
        }
        // every mode x every filter type: finite, bounded, with modulation of the cutoff
        for (int m = 0; m < ClipDistortion::numModes; ++m)
            for (int ft = 0; ft < 6; ++ft)
            {
                ClipDistortion::Settings st; st.mode = m; st.driveDb = 36.0f; st.filterPos = 1 + (ft & 1); st.filterType = ft; st.filterRes = 1.0f; st.oversampling = 1;
                ClipDistortion d; d.prepare (sr, 256); d.setSettings (st);
                juce::AudioBuffer<float> b = sineBuf (60.0f, 0.9f, 256 * 40);
                float pk = 0.0f;
                for (int blk = 0; blk < 40; ++blk)
                {
                    st.filterHz = 50.0f * std::pow (2.0f, (float) (blk % 10));   // sweep 50 Hz .. 25 kHz
                    d.setSettings (st);
                    d.process (b.getWritePointer (0, blk * 256), b.getWritePointer (1, blk * 256), 256);
                }
                pk = b.getMagnitude (0, 0, b.getNumSamples());
                if (! std::isfinite (pk) || pk > 8.0f) { ++bad; std::cout << "   mode " << ClipDistortion::modeNames()[m] << " filter " << ft << " peak " << pk << "\n"; }
            }
        // downsample: drive raises the hold factor (staircase)
        {
            ClipDistortion::Settings st; st.mode = ClipDistortion::Downsample; st.driveDb = 48.0f; st.oversampling = 0; st.dcFilter = false; st.outDb = 0.0f;
            auto out = runDist (st, sineBuf (100.0f, 0.5f, 4800));
            int repeats = 0;
            for (int i = 2001; i < 4000; ++i) repeats += out.getSample (0, i) == out.getSample (0, i - 1) ? 1 : 0;
            std::cout << "  Downsample at max drive: " << repeats << " of 1999 samples held\n";
            if (repeats < 1800) ++bad;
        }
        std::cout << "Distortion engine checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- Robustness: sample rates, block sizes, mono layout ----
    std::cout << "\nSample rate / block size           peak dBFS   pitch Hz\n";
    struct Case { double rate; int blockSize; bool random; const char* name; };
    const Case cases[] = { { 44100, 512, false, "44.1 kHz / 512" }, { 96000, 512, false, "96 kHz / 512" },
                           { 192000, 1024, false, "192 kHz / 1024" }, { 48000, 1, false, "48 kHz / 1 sample" },
                           { 48000, 17, false, "48 kHz / 17" }, { 48000, 4096, false, "48 kHz / 4096" },
                           { 48000, 700, true, "48 kHz / random 1..700" } };
    std::vector<float> reference;
    for (const auto& c : cases)
    {
        proc.presets.loadPreset (8); // 909 Oldskool Hardstyle
        auto audio = render (proc, 1, 60, c.rate, c.blockSize, c.random);
        bool finite = true;
        for (int i = 0; i < audio.getNumSamples(); ++i) finite &= std::isfinite (audio.getSample (0, i));
        const float pk = juce::Decibels::gainToDecibels (audio.getMagnitude (0, 0, audio.getNumSamples()));
        const float hz = measurePitch (audio, 0.15, 0.17, c.rate);
        const bool ok = finite && pk < 1.0f && std::abs (1200.0f * std::log2 (hz / targetHz)) < 25.0f;
        failures += ok ? 0 : 1;
        std::cout << juce::String (c.name).paddedRight (' ', 34) << juce::String (pk, 1).paddedLeft (' ', 9)
                  << juce::String (hz, 2).paddedLeft (' ', 11) << (ok ? "" : "   <-- CHECK") << "\n";

        // Block size must not change the result (sample-accurate MIDI, chunked processing)
        if (juce::exactlyEqual (c.rate, 48000.0))
        {
            std::vector<float> v (audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples());
            if (reference.empty()) reference = v;
            else
            {
                float maxDiff = 0.0f;
                for (size_t i = 0; i < std::min (v.size(), reference.size()); ++i)
                    maxDiff = std::max (maxDiff, std::abs (v[i] - reference[i]));
                // noise click is random, so allow a small difference during the first few ms only
                float tailDiff = 0.0f;
                for (size_t i = (size_t) (0.05 * 48000); i < std::min (v.size(), reference.size()); ++i)
                    tailDiff = std::max (tailDiff, std::abs (v[i] - reference[i]));
                std::cout << "    max difference vs 1-sample blocks after 50 ms: " << tailDiff << "\n";
                if (tailDiff > 0.02f) ++failures;
            }
        }
    }
    {
        CodexProcessor monoProc;
        juce::AudioProcessor::BusesLayout mono;
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        const bool supported = monoProc.setBusesLayout (mono);
        monoProc.setRateAndBufferSizeDetails (sr, block);
        monoProc.prepareToPlay (sr, block);
        juce::AudioBuffer<float> b (1, block);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        monoProc.processBlock (b, m);
        std::cout << "Mono output layout: " << (supported && b.getMagnitude (0, 0, block) > 0.0f ? "OK" : "FAILED") << "\n";
        if (! supported) ++failures;
    }

    // ---- EQ shapes and slopes (frequency responses) ----
    {
        auto magDb = [] (const eq::BandSettings& b, double f) { return 20.0 * std::log10 (eq::magnitude (eq::design (b, sr), f, sr)); };
        eq::BandSettings b; b.on = true; b.freq = 1000.0f;

        std::cout << "\nEQ low cut, attenuation 4 octaves below the cutoff (expected ~4 x slope):";
        b.type = eq::BandType::LowCut;
        for (int sl = 0; sl < eq::numSlopes; ++sl)
        {
            b.slope = sl;
            const double a = -magDb (b, 1000.0 / 16.0), expected = 4.0 * eq::slopeDb[sl];
            std::cout << " " << eq::slopeDb[sl] << ":" << juce::roundToInt (a);
            if (std::abs (a - expected) > 4.0 || std::abs (magDb (b, 1000.0)) > 3.5 || std::abs (magDb (b, 16000.0)) > 0.2) { ++failures; std::cout << "(!)"; }
        }
        b.type = eq::BandType::AllPass; b.slope = 3;
        double apMax = 0.0;
        for (double f = 20.0; f < 20000.0; f *= 1.2) apMax = std::max (apMax, std::abs (magDb (b, f)));
        b.type = eq::BandType::TiltShelf; b.gainDb = 12.0f;
        const double tiltLo = magDb (b, 25.0), tiltHi = magDb (b, 18000.0);
        b.type = eq::BandType::FlatTilt;
        bool monotonic = true; double prev = -1e9;
        for (double f = 20.0; f < 20000.0; f *= 1.1) { const double v = magDb (b, f); monotonic &= v >= prev - 0.01; prev = v; }
        const double flatLo = magDb (b, 20.0), flatHi = magDb (b, 20000.0);
        b.type = eq::BandType::BandPass; b.q = 1.0f;
        const double bpPeak = magDb (b, 1000.0), bpFar = magDb (b, 100.0);
        std::cout << "\nAll pass max deviation " << juce::String (apMax, 3) << " dB; tilt shelf " << juce::String (tiltLo, 1) << " / +" << juce::String (tiltHi, 1)
                  << " dB; flat tilt " << juce::String (flatLo, 1) << " -> +" << juce::String (flatHi, 1) << " dB (monotonic " << (monotonic ? "yes" : "NO")
                  << "); band pass peak " << juce::String (bpPeak, 1) << " dB, 1 decade away " << juce::String (bpFar, 1) << " dB\n";
        if (apMax > 0.01 || tiltLo > -5.0 || tiltHi < 5.0 || ! monotonic || flatHi < 4.0 || std::abs (bpPeak) > 1.5 || bpFar > -12.0) ++failures;

        // Stability: every shape, slope and placement on stereo noise
        eq::ParametricEQ<Params::numBands> e;
        e.prepare (sr);
        juce::Random rng (3);
        bool finite = true;
        for (int t = 0; t < eq::numBandTypes; ++t)
            for (int pl = 0; pl < 5; ++pl)
            {
                eq::BandSettings bs; bs.on = true; bs.type = (eq::BandType) t; bs.freq = 40.0f; bs.gainDb = 30.0f; bs.q = 18.0f;
                bs.slope = eq::numSlopes - 1; bs.placement = (eq::Placement) pl;
                e.setBand (0, bs);
                float l[256], r[256];
                for (int blk = 0; blk < 40; ++blk)
                {
                    for (int i = 0; i < 256; ++i) { l[i] = rng.nextFloat() - 0.5f; r[i] = rng.nextFloat() - 0.5f; }
                    e.process (l, r, 256);
                    for (int i = 0; i < 256; ++i) finite &= std::isfinite (l[i]) && std::isfinite (r[i]) && std::abs (l[i]) < 1000.0f;
                }
            }
        std::cout << "EQ stability, all shapes x placements at extreme settings: " << (finite ? "OK" : "FAILED") << "\n";
        if (! finite) ++failures;
    }

    // ---- Stereo: spatial, M/S EQ ----
    {
        auto sideDb = [] (const juce::AudioBuffer<float>& b, double from, double to)
        {
            double m = 0, s = 0;
            for (int i = (int) (from * sr); i < (int) (to * sr); ++i)
            {
                const double L = b.getSample (0, i), R = b.getSample (1, i);
                m += 0.25 * (L + R) * (L + R); s += 0.25 * (L - R) * (L - R);
            }
            return 10.0 * std::log10 ((s + 1e-20) / (m + 1e-20));
        };
        proc.presets.loadPreset (8);
        const double monoSide = sideDb (render (proc, 1), 0.0, 0.3);
        setParam (proc, ID::clip (2, "Spatial"), 1.0f);
        const double wideSide = sideDb (render (proc, 1), 0.0, 0.3);
        // Spatial on distortion 1 (before the EQ), distortion 2 off, then a full-band Side cut on the EQ
        setParam (proc, ID::clip (2, "Spatial"), 0.0f); setParam (proc, ID::clip (1, "Spatial"), 1.0f);
        setParam (proc, ID::clip (2, "On"), 0.0f);
        const double uncutSide = sideDb (render (proc, 1), 0.0, 0.3);
        setParam (proc, ID::band (5, "On"), 1.0f); setParam (proc, ID::band (5, "Type"), 0.0f);
        setParam (proc, ID::band (5, "Freq"), 20000.0f); setParam (proc, ID::band (5, "Slope"), 5.0f);   // 48 dB/oct
        setParam (proc, ID::band (5, "Place"), 4.0f);
        const double cutSide = sideDb (render (proc, 1), 0.0, 0.3);
        std::cout << "\nSide vs Mid energy: spatial off " << juce::String (monoSide, 1) << " dB, spatial on " << juce::String (wideSide, 1)
                  << " dB; spatial before EQ " << juce::String (uncutSide, 1) << " dB -> with EQ Side cut " << juce::String (cutSide, 1) << " dB\n";
        if (monoSide > -100.0 || wideSide < -40.0 || cutSide > uncutSide - 20.0) { ++failures; std::cout << "   <-- CHECK stereo\n"; }
    }

    // ---- Saturation: bands must sum flat when bypassed; all styles stay finite ----
    {
        std::cout << "Saturation crossovers, all bands bypassed (2 and 6 bands per slope), level change dB:";
        juce::Random rng (7);
        for (int slope = 0; slope < 4; ++slope)
            for (int nb : { 2, 6 })
            {
                sat::MultibandSaturator satProc;
                sat::Settings st;
                st.enabled = true; st.numBands = nb; st.slope = slope; st.oversampling = 0;
                for (auto& b : st.bands) b.bypass = true;
                satProc.setSettings (st);
                satProc.prepare (sr, 512);
                double in = 0, out = 0;
                for (int blk = 0; blk < 100; ++blk)
                {
                    float l[512], r[512];
                    for (int i = 0; i < 512; ++i) { l[i] = r[i] = rng.nextFloat() * 2.0f - 1.0f; if (blk > 10) in += (double) l[i] * l[i]; }
                    satProc.process (l, r, 512);
                    if (blk > 10) for (int i = 0; i < 512; ++i) out += (double) l[i] * l[i];
                }
                const double d = 10.0 * std::log10 (out / in);
                std::cout << " " << juce::String (d, 2);
                if (std::abs (d) > 0.3) { ++failures; std::cout << "(!)"; }
            }
        // Mid/Side channel mode must also reconstruct perfectly
        {
            sat::MultibandSaturator satProc;
            sat::Settings st;
            st.enabled = true; st.numBands = 4; st.channelMode = 1; st.oversampling = 0;
            for (auto& b : st.bands) b.bypass = true;
            satProc.setSettings (st);
            satProc.prepare (sr, 512);
            double diff = 0, in = 0;
            for (int blk = 0; blk < 50; ++blk)
            {
                float l[512], r[512], l0[512], r0[512];
                for (int i = 0; i < 512; ++i) { l0[i] = l[i] = rng.nextFloat() - 0.5f; r0[i] = r[i] = rng.nextFloat() - 0.5f; }
                satProc.process (l, r, 512);
                if (blk > 10)
                    for (int i = 0; i < 512; ++i) { in += (double) l0[i] * l0[i]; diff += 0.0; }
                // level check only (crossovers are all-pass): compare energies
                if (blk > 10) for (int i = 0; i < 512; ++i) diff += (double) l[i] * l[i] + (double) r[i] * r[i] - (double) l0[i] * l0[i] - (double) r0[i] * r0[i];
            }
            const double err = std::abs (diff) / std::max (1.0e-9, 2.0 * in);
            std::cout << " | M/S mode energy error " << juce::String (err * 100.0, 2) << " %";
            if (err > 0.05) { ++failures; std::cout << "(!)"; }
        }
        std::cout << "\n";

        // Feedback must die away once the input stops; dynamics must compress (right) and gate (left)
        {
            auto run = [] (sat::BandSettings band, std::vector<float> input)
            {
                sat::MultibandSaturator satProc;
                sat::Settings st;
                st.enabled = true; st.numBands = 1; st.oversampling = 0;
                st.bands[0] = band;
                satProc.setSettings (st);
                satProc.prepare (sr, 512);
                std::vector<float> l (input), r (input);
                for (size_t pos = 0; pos < l.size(); pos += 512)
                    satProc.process (l.data() + pos, r.data() + pos, (int) std::min<size_t> (512, l.size() - pos));
                return l;
            };
            auto rms = [] (const std::vector<float>& v, size_t a, size_t b) { double s2 = 0; for (size_t i = a; i < b; ++i) s2 += (double) v[i] * v[i]; return std::sqrt (s2 / (double) (b - a)); };

            std::vector<float> burst ((size_t) sr * 2, 0.0f);
            for (size_t i = 0; i < (size_t) (0.3 * sr); ++i) burst[i] = 0.5f * std::sin (6.2831853f * 55.0f * (float) i / (float) sr);
            sat::BandSettings fbBand; fbBand.style = 12; fbBand.driveDb = 12.0f; fbBand.feedback = 1.0f; fbBand.feedbackHz = 300.0f;
            const auto fbOut = run (fbBand, burst);
            const double tail = rms (fbOut, (size_t) (1.5 * sr), (size_t) (2.0 * sr));

            // loud then quiet sine: compare quiet/loud ratio with and without dynamics
            std::vector<float> steps ((size_t) sr, 0.0f);
            for (size_t i = 0; i < steps.size(); ++i)
                steps[i] = (i < steps.size() / 2 ? 0.8f : 0.05f) * std::sin (6.2831853f * 200.0f * (float) i / (float) sr);
            auto ratio = [&] (float dyn)
            {
                sat::BandSettings b; b.style = 0; b.driveDb = 0.0f; b.dynamics = dyn;
                const auto o = run (b, steps);
                return 20.0 * std::log10 (rms (o, (size_t) (0.75 * sr), (size_t) sr) / rms (o, (size_t) (0.25 * sr), (size_t) (0.5 * sr)));
            };
            const double neutral = ratio (0.0f), compress = ratio (1.0f), gate = ratio (-1.0f);
            std::cout << "Saturation feedback tail after input stops: " << juce::String (juce::Decibels::gainToDecibels ((float) tail, -200.0f), 1)
                      << " dBFS; quiet-vs-loud level: neutral " << juce::String (neutral, 1) << " dB, dynamics right " << juce::String (compress, 1)
                      << " dB (compresses), left " << juce::String (gate, 1) << " dB (gates)\n";
            if (tail > 1.0e-3 || compress < neutral + 6.0 || gate > neutral - 6.0) ++failures;
        }

        proc.presets.loadPreset (8);
        setParam (proc, ID::sat ("On"), 1.0f);
        setParam (proc, ID::sat ("Bands"), 1.0f);
        bool allFinite = true; float worst = -100.0f; juce::String worstStyle;
        for (int style = 0; style < (int) sat::styles().size(); ++style)
        {
            setParam (proc, ID::satBand (0, "Style"), (float) style);
            setParam (proc, ID::satBand (0, "Drive"), 36.0f);
            setParam (proc, ID::satBand (0, "Fb"), 1.0f);
            setParam (proc, ID::satBand (0, "Dyn"), 1.0f);
            auto a = render (proc, 1);
            for (int i = 0; i < a.getNumSamples(); ++i) allFinite &= std::isfinite (a.getSample (0, i));
            const float pk = juce::Decibels::gainToDecibels (a.getMagnitude (0, 0, a.getNumSamples()));
            if (pk > worst) { worst = pk; worstStyle = sat::styles()[(size_t) style].name; }
        }
        std::cout << "Saturation, all styles at max drive / feedback / dynamics: " << (allFinite ? "finite" : "NaN!")
                  << ", loudest peak " << juce::String (worst, 1) << " dBFS (" << worstStyle << ")\n";
        if (! allFinite || worst > 18.0f) ++failures;
        setParam (proc, ID::sat ("On"), 0.0f);
    }

    // ---- State round trip ----
    proc.presets.loadPreset (9);
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    CodexProcessor proc2;
    proc2.setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "\nState round-trip preset name: " << proc2.presets.getCurrentName() << "\n";

    // ---- GUI snapshots ----
    proc.presets.loadPreset (2);  // Oldskool Raw (synth)
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_synth.png"));
    setParam (proc, ID::envMode ('p'), 1.0f);
    setParam (proc, ID::envMode ('a'), 1.0f);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_draw_envelopes.png"));
    proc.presets.loadPreset (8);  // 909 Oldskool Hardstyle
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_909.png"));
    proc.presets.loadPreset (10); // 909 Rawstyle
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_distortion.png"), 1);
    proc.presets.loadPreset (10);
    setParam (proc, ID::clip (1, "FltPos"), 1.0f); setParam (proc, ID::clip (1, "FltType"), 2.0f);
    setParam (proc, ID::clip (1, "FltFreq"), 180.0f); setParam (proc, ID::clip (1, "FltRes"), 0.55f);
    setParam (proc, ID::clip (2, "Mode"), (float) ClipDistortion::XShaperAsym);
    {
        const float ys[] = { 0.45f, 0.8f, 0.95f, 0.7f, 0.55f, 0.8f, 1.0f, 1.0f };
        for (int k = 0; k < 8; ++k)
        {
            setParam (proc, ID::clip (2, ("XP" + juce::String (k + 1)).toRawUTF8()), ys[k]);
            setParam (proc, ID::clip (2, ("XN" + juce::String (k + 1)).toRawUTF8()), std::min (1.0f, 0.3f + 0.1f * (float) k));
        }
    }
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_distortion_filter_xshaper.png"), 1);
    proc.presets.loadPreset (7);  // 909 Clean: both distortions bypassed
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_distortion_bypassed.png"), 1);
    {
        // Stereo features: spatial distortion, a Side low cut and multiband saturation
        setParam (proc, ID::clip (2, "Spatial"), 0.6f);
        setParam (proc, ID::band (5, "On"), 1.0f);
        setParam (proc, ID::band (5, "Type"), 0.0f);
        setParam (proc, ID::band (5, "Freq"), 150.0f);
        setParam (proc, ID::band (5, "Place"), 4.0f);
        setParam (proc, ID::sat ("On"), 1.0f);
        setParam (proc, ID::sat ("Bands"), 4.0f);
        auto stereoAudio = render (proc, 1);
        snapshot (proc, stereoAudio, outDir.getChildFile ("ui_stereo_eq.png"), 0);
        snapshot (proc, stereoAudio, outDir.getChildFile ("ui_saturation.png"), 2);
        snapshot (proc, stereoAudio, outDir.getChildFile ("ui_scope.png"), 6);
        writeWav (stereoAudio, outDir.getChildFile ("stereo_demo.wav"));
        proc.presets.loadPreset (10);
    }
    proc.presets.loadPreset (8);
    setParam (proc, ID::cmp ("On"), 1.0f); setParam (proc, ID::cmp ("Bands"), 3.0f);
    snapshot (proc, render (proc, 3), outDir.getChildFile ("ui_compressor.png"), 3);
    proc.presets.loadPreset (8);
    EQDisplay::addBand (proc.apvts, 1, eq::BandType::LowCut, 35.0f);
    EQDisplay::addBand (proc.apvts, 1, eq::BandType::Bell, 2800.0f, 3.0f, 0.8f);
    EQDisplay::addBand (proc.apvts, 1, eq::BandType::HighShelf, 9000.0f, -4.0f);
    setParam (proc, ID::eqMode (1), 2.0f);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_eq2.png"), 4);
    proc.presets.loadPreset (8);
    setParam (proc, ID::spc ("On"), 1.0f);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_space.png"), 5);
    setParam (proc, ID::spc ("On"), 0.0f);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_about.png"), 0, 1);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_save.png"), 0, 2);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_browser.png"), 0, 3);
    {
        // Resized window (75 %)
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (MainView::baseWidth * 3 / 4, MainView::baseHeight * 3 / 4);
        auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        auto png = outDir.getChildFile ("ui_scaled75.png");
        png.deleteFile();
        juce::FileOutputStream os (png);
        juce::PNGImageFormat().writeImageToStream (img, os);
        editor->setSize (MainView::baseWidth, MainView::baseHeight); // restore the saved UI scale
    }

    std::cout << "\n" << (failures == 0 ? "ALL CHECKS PASSED" : juce::String (failures) + " CHECK(S) FAILED") << "\n";
    return failures == 0 ? 0 : 2;
}
