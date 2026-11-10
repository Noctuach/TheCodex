// Offline test tool.
//   CodexRender <outdir>
// Renders every factory preset (kick.wav per preset), measures the tail pitch with the tuner's
// detector, checks all 12 keys, round-trips the state and snapshots the editor in both engines.
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/GUI/PluginEditor.h"

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
    }
    // feed analyzer + tuner with the tail of a kick
    for (int i = 0; i < 6; ++i)
    {
        proc.analyzer.push (audio.getReadPointer (0, (int) (0.05 * sr) + i * 2400), 4096);
        proc.tunerFeed.push (audio.getReadPointer (0, (int) (0.1 * sr) + i * 2400), 2400);
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
    for (int os = 0; os < 4; ++os)
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
    proc.presets.loadPreset (8);  // 909 Oldskool Hardstyle
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_909.png"));
    proc.presets.loadPreset (10); // 909 Rawstyle
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_distortion.png"), 1);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_about.png"), 0, 1);
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_save.png"), 0, 2);
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
