// Offline test tool.
//   CodexRender <outdir>
// Renders every factory preset (kick.wav per preset), measures the tail pitch with the tuner's
// detector, checks all 12 keys, round-trips the state and snapshots the editor in both engines.
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/GUI/PluginEditor.h"
#include "../Source/DSP/EqEngine.h"
#include "../Source/DSP/StereoOversampling.h"
#include "../Source/DSP/DrawEnvelope.h"
#include "../Source/KickExporter.h"
#if JUCE_LINUX
 #include <dlfcn.h>

/** Headless X servers (Xvfb) have no window manager, so atoms such as WM_STATE do not exist and JUCE's window-state
    query fails with BadAtom; Xlib's default handler would then end the process. Ignore X errors instead (as in
    HostTest.cpp). Needed for the keyboard-focus test, which shows the editor in a real window. */
static void installTolerantXErrorHandler()
{
    using Handler = int (*) (void*, void*);
    using SetHandler = Handler (*) (Handler);
    if (auto* lib = dlopen ("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL))
        if (auto set = reinterpret_cast<SetHandler> (dlsym (lib, "XSetErrorHandler")))
            set ([] (void*, void*) -> int { return 0; });
}
#endif

static void setParam (CodexProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

/** Presets never touch the key settings: back to F, octave 0 and Fixed Key (what the pitch checks expect). */
static void resetKey (CodexProcessor& p)
{
    setParam (p, ID::key, 0.0f);
    setParam (p, ID::octave, 1.0f);   // index 1 = "0"
    setParam (p, ID::keyMode, 0.0f);
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

/** One note with a given velocity and length (seconds), rendered for totalSec. */
static juce::AudioBuffer<float> renderNote (CodexProcessor& proc, int velocity, double holdSec, double totalSec)
{
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
    const int total = (int) (totalSec * sr) / block * block, off = (int) (holdSec * sr);
    juce::AudioBuffer<float> out (2, total), buf (2, block);
    for (int pos = 0; pos < total; pos += block)
    {
        juce::MidiBuffer midi;
        if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) velocity), 0);
        if (off >= pos && off < pos + block) midi.addEvent (juce::MidiMessage::noteOff (1, 60), off - pos);
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, block);
    }
    return out;
}

/** Average frequency from rising zero crossings of the left channel between t0 and t0 + len (seconds). */
static float zeroCrossHz (const juce::AudioBuffer<float>& b, double t0, double len)
{
    const int s0 = (int) (t0 * sr), s1 = std::min (b.getNumSamples(), (int) ((t0 + len) * sr));
    int first = -1, last = -1, count = 0;
    for (int i = s0 + 1; i < s1; ++i)
        if (b.getSample (0, i - 1) < 0.0f && b.getSample (0, i) >= 0.0f)
        {
            if (first < 0) first = i; else ++count;
            last = i;
        }
    return count > 0 ? (float) (count * sr / (last - first)) : 0.0f;
}

static float rmsDb (const juce::AudioBuffer<float>& b, double t0, double len)
{
    const int s0 = (int) (t0 * sr), n = std::min ((int) (len * sr), b.getNumSamples() - s0);
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += b.getSample (0, s0 + i) * b.getSample (0, s0 + i);
    return juce::Decibels::gainToDecibels ((float) std::sqrt (e / std::max (1, n)), -120.0f);
}

// Same approach as the GUI tuner: decimate to ~6 kHz and run the detector on the tail.
/** Fundamental by a high-resolution FFT (Hann, zero-padded, parabolic peak) between 30 and 120 Hz: an independent
    reference for the time-domain pitch detector. */
static float fftFundamental (const juce::AudioBuffer<float>& b, double startSec, double lenSec, double rate = sr)
{
    const int order = 18, N = 1 << order;
    const int start = (int) (startSec * rate), len = juce::jmin ((int) (lenSec * rate), b.getNumSamples() - start, N);
    std::vector<float> data ((size_t) N * 2, 0.0f);
    for (int i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (len - 1));
        data[(size_t) i] = 0.5f * (b.getSample (0, start + i) + b.getSample (1, start + i)) * (float) w;
    }
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data());
    const int k0 = (int) (30.0 * N / rate), k1 = (int) (120.0 * N / rate);
    int best = k0;
    for (int k = k0; k <= k1; ++k) if (data[(size_t) k] > data[(size_t) best]) best = k;
    const float a = data[(size_t) best - 1], c = data[(size_t) best], d = data[(size_t) best + 1];
    const float off = 0.5f * (a - d) / (a - 2.0f * c + d);
    return (float) ((best + off) * rate / N);
}

static float measurePitch (const juce::AudioBuffer<float>& b, double startSec, double lenSec, double rate = sr, float minHz = 25.0f)
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
    return detectPitch (d.data(), (int) d.size(), rate / decim, minHz, 400.0f).frequency;
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

static void snapshot (CodexProcessor& proc, const juce::AudioBuffer<float>& audio, const juce::File& png, int tab = 0, int overlay = 0,
                      float windowScale = 1.0f, float pixelScale = 2.0f)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    if (auto* hk = dynamic_cast<CodexEditor*> (editor.get()))
    {
        hk->setSize (juce::roundToInt (MainView::baseWidth * windowScale), juce::roundToInt (MainView::baseHeight * windowScale));
        hk->showTab (tab);
        if (overlay == 1) hk->getView().openAbout();
        if (overlay == 2) hk->getView().openSaveDialog();
        if (overlay == 3) hk->getView().openBrowser();
        if (overlay == 4) hk->getView().openGuide();
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
    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, pixelScale);
    if (windowScale != 1.0f) editor->setSize (MainView::baseWidth, MainView::baseHeight); // restore the saved UI scale
    png.deleteFile();
    juce::FileOutputStream os (png);
    juce::PNGImageFormat().writeImageToStream (img, os);
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: CodexRender <outdir>\n"; return 1; }
   #if JUCE_LINUX
    installTolerantXErrorHandler();
   #endif
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    outDir.createDirectory();
    int failures = 0;

    CodexProcessor proc;
    proc.setPlayConfigDetails (0, 2, sr, block);
    // Key Mode defaults to Follow MIDI; every check below plays MIDI note 60 and expects the selected key (F1)
    setParam (proc, ID::keyMode, 0.0f);

    if (std::getenv ("CODEX_UI_SCALES") != nullptr)
    {
        // Screenshot mode: every tab and overlay at several window scales (and 100 % at 2x pixels) into outDir/scales
        const auto dir = outDir.getChildFile ("scales");
        dir.createDirectory();
        proc.presets.loadPreset (10); // 909 Rawstyle
        setParam (proc, ID::cmp ("On"), 1.0f); setParam (proc, ID::sat ("On"), 1.0f); setParam (proc, ID::spc ("On"), 1.0f);
        const auto audio = render (proc, 1);
        const std::pair<float, float> scales[] = { { 0.6f, 1.0f }, { 0.75f, 1.0f }, { 1.0f, 1.0f }, { 1.5f, 1.0f }, { 2.0f, 1.0f }, { 1.0f, 2.0f } };
        const char* overlays[] = { "about", "save", "browser", "guide" };
        for (auto [s, px] : scales)
        {
            const auto suffix = "_s" + juce::String (juce::roundToInt (s * 100)).paddedLeft ('0', 3) + (px != 1.0f ? "_px2" : "") + ".png";
            for (int tab = 0; tab <= 6; ++tab)
                snapshot (proc, audio, dir.getChildFile ("tab" + juce::String (tab) + suffix), tab, 0, s, px);
            for (int o = 0; o < 4; ++o)
                snapshot (proc, audio, dir.getChildFile (overlays[o] + suffix), 0, o + 1, s, px);
        }
        std::cout << "UI scale screenshots written to " << dir.getFullPathName() << "\n";
        return 0;
    }
    std::cout << "Latency: " << proc.getLatencySamples() << " samples\n\n";

    // ---- All factory presets in the key of F (F1 = 43.65 Hz) ----
    std::cout << juce::String ("Preset").paddedRight (' ', 26) << "  peak dBFS   tail pitch   cents   loudness\n";
    const float targetHz = midiToFrequency (29.0f);
    std::vector<std::pair<float, float>> presetLevels;   // (loudness, peak) for the level-match report
    // Loudness of one hit: K-weighted (ITU BS.1770 pre-filter and RLB high-pass), loudest 100 ms window in the first
    // 400 ms, both channels summed as in LUFS. Peak matching hid ~9 dB differences between presets.
    auto hitLoudness = [] (const juce::AudioBuffer<float>& b)
    {
        const int n = std::min (b.getNumSamples(), (int) (0.4 * sr)), win = (int) (0.1 * sr), hop = win / 4;
        std::vector<double> power ((size_t) n, 0.0);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::dsp::IIR::Filter<float> shelf, rlb;
            shelf.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 1500.0, 0.7071, juce::Decibels::decibelsToGain (4.0f));
            rlb.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 38.0, 0.5);
            for (int s = 0; s < n; ++s)
            {
                const double v = rlb.processSample (shelf.processSample (b.getSample (ch, s)));
                power[(size_t) s] += v * v;
            }
        }
        double best = 0.0;
        for (int s = 0; s + win <= n; s += hop)
        {
            double e = 0.0;
            for (int k = 0; k < win; ++k) e += power[(size_t) (s + k)];
            best = std::max (best, e / win);
        }
        return (float) (-0.691 + 10.0 * std::log10 (best + 1.0e-12));   // LUFS-style short-term value
    };
    for (int i = 0; i < proc.presets.getNumFactory(); ++i)
    {
        proc.presets.loadPreset (i);
        auto audio = render (proc, 2);
        bool finite = true;
        for (int s = 0; s < audio.getNumSamples(); ++s) finite &= std::isfinite (audio.getSample (0, s));
        const float pk = juce::Decibels::gainToDecibels (audio.getMagnitude (0, 0, audio.getNumSamples()));
        const float hz = measurePitch (audio, 0.15, 0.17);
        const float cents = hz > 0 ? 1200.0f * std::log2 (hz / targetHz) : 0.0f;
        if (std::getenv ("CODEX_FFT_PITCH") != nullptr)
            std::cout << "        FFT fundamental " << juce::String (fftFundamental (audio, 0.15, 0.17), 2) << " Hz ("
                      << juce::String (1200.0f * std::log2 (fftFundamental (audio, 0.15, 0.17) / targetHz), 1) << " c)\n";
        const bool ok = finite && pk < 1.0f && pk > -30.0f && hz > 0 && std::abs (cents) < 25.0f;
        failures += ok ? 0 : 1;
        const float loud = hitLoudness (audio);
        presetLevels.push_back ({ loud, pk });
        std::cout << proc.presets.getFactoryNames()[i].paddedRight (' ', 26) << juce::String (pk, 1).paddedLeft (' ', 9)
                  << juce::String (hz, 2).paddedLeft (' ', 10) << " Hz" << juce::String (cents, 1).paddedLeft (' ', 8)
                  << juce::String (loud, 1).paddedLeft (' ', 11) << (ok ? "" : "   <-- CHECK") << "\n";
        writeWav (audio, outDir.getChildFile (juce::File::createLegalFileName (proc.presets.getFactoryNames()[i]) + ".wav"));
    }

    // ---- Animation frames (CODEX_ANIM_FRAMES=1): a hit and a tab switch, captured mid-animation for review ----
    if (std::getenv ("CODEX_ANIM_FRAMES") != nullptr)
    {
        proc.presets.loadPreset (8);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* hk = dynamic_cast<CodexEditor*> (editor.get());
        hk->setSize (MainView::baseWidth, MainView::baseHeight);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        auto frame = [&] (const juce::String& name)
        {
            auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            auto f = outDir.getChildFile (name + ".png");
            f.deleteFile();
            juce::FileOutputStream os (f);
            juce::PNGImageFormat().writeImageToStream (img, os);
        };
        proc.hitCounter.fetch_add (1);
        for (int i = 0; i < 4; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (45); frame ("anim_hit_" + juce::String (i)); }
        hk->showTab (5);
        for (int i = 0; i < 4; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (35); frame ("anim_tab_" + juce::String (i)); }
        // easter egg: the dancer, one frame per kick
        hk->getView().toggleDancer();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        for (int i = 0; i < 6; ++i)
        {
            proc.hitCounter.fetch_add (1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (180);
            frame ("anim_dance_" + juce::String (i));
        }
        std::cout << "\nAnimation frames written\n";
    }

    // ---- Factory presets are loudness-matched (Init excluded): every preset within 3.5 dB of the median ----
    {
        std::vector<float> l;
        for (size_t i = 1; i < presetLevels.size(); ++i) l.push_back (presetLevels[i].first);
        std::sort (l.begin(), l.end());
        const float median = l[l.size() / 2], spread = std::max (median - l.front(), l.back() - median);
        const bool ok = spread < 3.5f;
        std::cout << "\nPreset loudness: median " << juce::String (median, 1) << " LUFS (short-term), largest deviation "
                  << juce::String (spread, 1) << " dB: " << (ok ? "OK" : "FAILED") << "\n";
        failures += ok ? 0 : 1;
    }

    // ---- Level match report: suggested output levels for PresetManager.cpp (loudness-matched, peaks kept below -1 dBFS) ----
    if (std::getenv ("CODEX_LEVEL_MATCH") != nullptr)
    {
        std::vector<float> l;
        for (auto& p : presetLevels) l.push_back (p.first);
        std::sort (l.begin(), l.end());
        const float target = l[l.size() / 2];
        std::cout << "\nLevel match (target " << target << " LUFS short-term):\n";
        for (int i = 0; i < proc.presets.getNumFactory(); ++i)
        {
            proc.presets.loadPreset (i);
            const float current = proc.apvts.getParameter (ID::outGain)->convertFrom0to1 (proc.apvts.getParameter (ID::outGain)->getValue());
            // with the output clipper on, allow up to 3 dB of peaks into the clipper; otherwise keep 1 dB of headroom
            const bool clipper = proc.apvts.getRawParameterValue (ID::clipOn)->load() > 0.5f;
            const float delta = clipper ? target - presetLevels[(size_t) i].first
                                        : std::min (target - presetLevels[(size_t) i].first, -1.0f - presetLevels[(size_t) i].second);
            std::cout << "    { \"" << proc.presets.getFactoryNames()[i] << "\", " << juce::String (current + delta, 1) << "f },\n";
        }
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

    // ---- StereoOversampling == juce::dsp::Oversampling (polyphase IIR, max quality), bit for bit ----
    {
        int bad = 0;
        juce::Random rng (1234);
        for (int stagesN = 1; stagesN <= 4; ++stagesN)
        {
            const int maxBlock = 300;
            juce::dsp::Oversampling<float> ref (2, (size_t) stagesN, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
            StereoOversampling mine (stagesN);
            ref.initProcessing ((size_t) maxBlock);
            mine.initProcessing ((size_t) maxBlock);
            if (ref.getLatencyInSamples() != mine.getLatencyInSamples() || ref.getOversamplingFactor() != mine.getOversamplingFactor()) ++bad;
            juce::AudioBuffer<float> a (2, maxBlock), b (2, maxBlock);
            double ph = 0.0;
            for (int blk = 0; blk < 400; ++blk)
            {
                if (blk == 150 || blk == 151 || blk == 300) { ref.reset(); mine.reset(); }   // resets (twice in a row too)
                const int n = blk % 50 == 7 ? maxBlock : 1 + rng.nextInt (maxBlock);
                for (int i = 0; i < n; ++i)
                {
                    ph += 0.013 + 0.00002 * blk;
                    const float x = (float) (0.9 * std::sin (ph)) + 0.05f * (rng.nextFloat() - 0.5f);
                    // silence stretches exercise the snap-to-zero of the filter states
                    const float g = (blk / 40) % 3 == 2 ? 0.0f : 1.0f;
                    a.setSample (0, i, g * x);
                    a.setSample (1, i, g * (blk % 2 ? -0.7f * x : 0.3f * std::tanh (3.0f * x)));
                }
                for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, a, ch, 0, n);
                juce::dsp::AudioBlock<float> ba (a.getArrayOfWritePointers(), 2, (size_t) n), bb (b.getArrayOfWritePointers(), 2, (size_t) n);
                auto ua = ref.processSamplesUp (ba);
                auto ub = mine.processSamplesUp (bb);
                if (ua.getNumSamples() != ub.getNumSamples()) { ++bad; break; }
                for (int ch = 0; ch < 2; ++ch)
                    for (size_t i = 0; i < ua.getNumSamples(); ++i)
                    {
                        if (std::memcmp (ua.getChannelPointer ((size_t) ch) + i, ub.getChannelPointer ((size_t) ch) + i, sizeof (float)) != 0) { ++bad; ch = 2; break; }
                        // a nonlinearity in the oversampled domain, identical for both
                        const float y = std::tanh (2.0f * ua.getSample (ch, (int) i));
                        ua.setSample (ch, (int) i, y);
                        ub.setSample (ch, (int) i, y);
                    }
                ref.processSamplesDown (ba);
                mine.processSamplesDown (bb);
                for (int ch = 0; ch < 2; ++ch)
                    if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) n) != 0) { ++bad; break; }
            }
        }
        std::cout << "StereoOversampling vs juce::dsp::Oversampling (2x-16x, random blocks, resets): " << (bad == 0 ? "bit-exact" : "FAILED") << "\n";
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
            // The filters are built on a worker thread and installed by process(): wait for the build (generous for slow
            // sanitizer runners), then process until it is installed, then a little more so the crossfade has finished.
            for (int i = 0; i < 6000 && ! e.isLinearBuildReadyForTesting(); ++i) juce::Thread::sleep (10);
            juce::AudioBuffer<float> z (2, 256);
            for (int i = 0; i < 6000 && ! e.isLinearUpToDate(); ++i) { z.clear(); e.process (z.getWritePointer (0), z.getWritePointer (1), 256); juce::Thread::sleep (1); }
            for (int i = 0; i < 50; ++i) { z.clear(); e.process (z.getWritePointer (0), z.getWritePointer (1), 256); }
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
            if (mode == 2 && lat != eq::firLength (1, sr) / 2 + PartitionedConvolver::B) ++bad;
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
        // Linear phase: editing a band while audio plays swaps the filter without dropping or clicking
        {
            eq::EqEngine e; e.prepare (sr, 256);
            eq::BandSettings bell; bell.on = true; bell.type = eq::BandType::Bell; bell.freq = 2000.0f; bell.gainDb = 0.5f; bell.q = 1.0f;
            e.setBand (0, bell); e.setMode (2, 1); settle (e);
            const int n = (int) (sr * 2.0);
            juce::AudioBuffer<float> b (2, n);
            for (int i = 0; i < n; ++i) { const float v = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 100.0 * i / sr); b.setSample (0, i, v); b.setSample (1, i, v); }
            for (int pos = 0; pos < n; pos += 256)
            {
                if (pos > 0 && pos % (int) (0.1 * sr) < 256)   // a new band setting every 100 ms
                {
                    bell.gainDb = bell.gainDb > 0.0f ? -0.5f : 0.5f;
                    e.setBand (0, bell); e.setMode (2, 1);
                    juce::Thread::sleep (30);   // give the worker time so swaps really happen mid-stream
                }
                e.process (b.getWritePointer (0, pos), b.getWritePointer (1, pos), std::min (256, n - pos));
            }
            // after the latency, the 100 Hz sine must be continuous: no block quieter than -1 dB of the steady level,
            // and no sample-to-sample jump larger than the sine's own slope allows
            const int lat = e.getLatencyInSamples();
            float minRms = 10.0f, maxJump = 0.0f;
            for (int pos = lat + 2048; pos + 256 < n; pos += 256)
                minRms = std::min (minRms, b.getRMSLevel (0, pos, 256));
            for (int i = lat + 2049; i < n; ++i) maxJump = std::max (maxJump, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
            const float slope = 0.3f * juce::MathConstants<float>::twoPi * 100.0f / (float) sr;
            std::cout << "  Linear phase live edits: lowest block " << juce::String (juce::Decibels::gainToDecibels (minRms / (0.3f * 0.7071f)), 2)
                      << " dB, largest step " << juce::String (maxJump / slope, 2) << "x the sine's slope\n";
            if (minRms < 0.3f * 0.7071f * 0.89f || maxJump > slope * 1.5f) ++bad;
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
            if (std::abs (d) < 0.2f || lat < eq::firLength (1, sr) / 2 + PartitionedConvolver::B) ++bad;
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
        proc.presets.loadPreset (8);   // 909 Oldskool Hardstyle (no output clipper)
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
        proc.presets.loadPreset (8);
        if (proc.apvts.getRawParameterValue (ID::clipOn)->load() > 0.5f) { ++failures; std::cout << "   preset did not reset the clipper\n"; }
    }

    // ---- Kick export (drag to DAW): WAV in the selected key, tail trimmed, the live instance untouched ----
    {
        int bad = 0;
        proc.presets.loadPreset (8);
        setParam (proc, ID::key, 3.0f);   // G#
        const auto before = proc.apvts.copyState().toXmlString();
        const auto file = KickExporter::render (proc, outDir.getChildFile ("export_test.wav"), 0.4, sr);
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr) { ++bad; std::cout << "\nExport: no file written\n"; }
        else
        {
            juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
            reader->read (&b, 0, b.getNumSamples(), 0, true, true);
            const float target = juce::MidiMessage::getMidiNoteInHertz (Params::keyBaseNote + 3);
            const float pitch = measurePitch (b, 0.2, 0.15);
            const float secs = (float) b.getNumSamples() / (float) reader->sampleRate;
            const float pk = juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples()));
            std::cout << "\nExport: " << file.getFileName() << ", " << reader->bitsPerSample << "-bit, " << juce::String (secs, 2) << " s, peak "
                      << juce::String (pk, 1) << " dBFS, pitch " << juce::String (pitch, 2) << " Hz (G#1 = " << juce::String (target, 2) << ")\n";
            if (reader->bitsPerSample != 24 || secs < 0.4f || secs > 2.0f || pk < -9.0f || pk > 0.0f
                || std::abs (1200.0f * std::log2 (pitch / target)) > 25.0f) ++bad;
            std::cout << "  default drag file name: " << KickExporter::defaultFile (proc).getFileName() << "\n";
        }
        if (proc.apvts.copyState().toXmlString() != before) { ++bad; std::cout << "   export changed the live instance\n"; }
        // with a linear-phase EQ (large latency, filters built on a worker thread) the attack must still be there
        {
            setParam (proc, ID::eqMode (0), 2.0f);
            setParam (proc, ID::eqRes (0), 3.0f);   // Very High: ~340 ms of latency
            const auto lpFile = KickExporter::render (proc, outDir.getChildFile ("export_linear.wav"), 0.4, sr);
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (lpFile));
            if (rd == nullptr) ++bad;
            else
            {
                juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
                rd->read (&b, 0, b.getNumSamples(), 0, true, true);
                const float attack = b.getMagnitude (0, 0, (int) (0.02 * sr));
                const float total = b.getMagnitude (0, 0, b.getNumSamples());
                std::cout << "  linear-phase export: attack " << juce::String (juce::Decibels::gainToDecibels (attack), 1)
                          << " dBFS in the first 20 ms (peak " << juce::String (juce::Decibels::gainToDecibels (total), 1) << "), "
                          << juce::String ((float) b.getNumSamples() / (float) rd->sampleRate, 2) << " s\n";
                if (attack < total * 0.25f) ++bad;
            }
            setParam (proc, ID::eqMode (0), 0.0f);
        }
        std::cout << "Export checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        setParam (proc, ID::key, 0.0f);
        proc.prepareToPlay (sr, block);
    }

    // ---- Sub layer (aligned with the chain latency) and velocity ----
    {
        int bad = 0;
        auto lowBandDb = [] (const juce::AudioBuffer<float>& b, double t0, double t1)
        {
            // 2nd-order low-pass at 80 Hz, then RMS
            eq::BandSettings lp; lp.on = true; lp.type = eq::BandType::HighCut; lp.freq = 80.0f; lp.slope = 3;
            auto d = eq::design (lp, sr);
            std::vector<eq::Biquad> st ((size_t) d.numStages);
            for (int k = 0; k < d.numStages; ++k) st[(size_t) k].c = d.stages[(size_t) k];
            double acc = 0; int cnt = 0;
            for (int i = 0; i < (int) (t1 * sr); ++i)
            {
                double v = b.getSample (0, i);
                for (auto& s2 : st) v = s2.process ((float) v);
                if (i >= (int) (t0 * sr)) { acc += v * v; ++cnt; }
            }
            return juce::Decibels::gainToDecibels ((float) std::sqrt (acc / juce::jmax (1, cnt)), -120.0f);
        };
        proc.presets.loadPreset (10);   // 909 Rawstyle: hard clipping thins the fundamental
        setParam (proc, ID::clip (1, "OS"), 3.0f);   // 8x: lots of latency to align against
        const auto off = render (proc, 1);
        setParam (proc, ID::subOn, 1.0f);
        setParam (proc, ID::subLevel, -6.0f);
        const auto on = render (proc, 1);
        setParam (proc, ID::subPhase, 180.0f);
        const auto inverted = render (proc, 1);
        const float lOff = lowBandDb (off, 0.15, 0.33), lOn = lowBandDb (on, 0.15, 0.33), lInv = lowBandDb (inverted, 0.15, 0.33);
        const float best = KickExporter::findBestSubPhase (proc);
        setParam (proc, ID::subPhase, best);
        const auto aligned = render (proc, 1);
        const float lAligned = lowBandDb (aligned, 0.15, 0.33);
        std::cout << "\nSub layer: ALIGN picked " << best << " degrees -> fundamental " << juce::String (lAligned, 1) << " dB";
        if (lAligned < std::max (lOn, lInv) - 0.5f || lAligned < lOff + 3.0f) ++bad;
        std::cout << "\nSub layer: fundamental (< 80 Hz) " << juce::String (lOff, 1) << " dB off, " << juce::String (lOn, 1)
                  << " dB on, " << juce::String (lInv, 1) << " dB with 180 degrees (latency " << proc.getLatencySamples() << " samples)\n";
        if (std::abs (lOn - lInv) < 3.0f) ++bad;   // the phase control must matter

        proc.presets.loadPreset (7);
        setParam (proc, ID::velocity, 1.0f);
        const auto loud = render (proc, 1, 60);
        juce::AudioBuffer<float> soft;
        {
            // half velocity
            proc.prepareToPlay (sr, block);
            juce::AudioBuffer<float> buf (2, block);
            soft.setSize (2, (int) (0.5 * sr));
            soft.clear();
            for (int pos = 0; pos + block <= soft.getNumSamples(); pos += block)
            {
                juce::MidiBuffer midi;
                if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 64), 0);
                buf.clear();
                proc.processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch) soft.copyFrom (ch, pos, buf, ch, 0, block);
            }
        }
        const float dLoud = juce::Decibels::gainToDecibels (loud.getMagnitude (0, 0, (int) (0.4 * sr)));
        const float dSoft = juce::Decibels::gainToDecibels (soft.getMagnitude (0, 0, (int) (0.4 * sr)));
        std::cout << "Velocity 100 %: velocity 127 peak " << juce::String (dLoud, 1) << " dBFS, velocity 64 peak " << juce::String (dSoft, 1) << " dBFS\n";
        if (std::abs ((dLoud - dSoft) - 6.0f) > 1.0f) ++bad;
        std::cout << "Sub / velocity checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        proc.presets.loadPreset (7);
    }

    // ---- State restore is exact even after hosts set in-between values on switches (pluginval's test) ----
    {
        int bad = 0;
        juce::Random rng (77);
        std::vector<float> saved;
        for (auto* prm : proc.getParameters()) prm->setValueNotifyingHost (rng.nextFloat());
        juce::MemoryBlock st;
        proc.getStateInformation (st);
        // what a host expects back: the value as the parameter itself represents it (switches snap to 0 / 1)
        for (auto* prm : proc.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm);
            saved.push_back (rp != nullptr ? rp->convertTo0to1 (rp->convertFrom0to1 (prm->getValue())) : prm->getValue());
        }
        for (auto* prm : proc.getParameters()) prm->setValueNotifyingHost (rng.nextFloat());
        proc.setStateInformation (st.getData(), (int) st.getSize());
        int i = 0;
        for (auto* prm : proc.getParameters())
        {
            if (std::abs (prm->getValue() - saved[(size_t) i]) > 0.1f) { if (++bad < 4) std::cout << "   not restored: " << prm->getName (40) << "\n"; }
            ++i;
        }
        std::cout << "\nState restore after random host values: " << (bad == 0 ? "OK" : juce::String (bad) + " parameters FAILED") << "\n";
        failures += bad > 0 ? 1 : 0;
        proc.presets.loadPreset (0);
        resetKey (proc);
    }

    // ---- Modified marker: clean after load, modified after an edit, key changes do not count ----
    {
        int bad = 0;
        proc.presets.loadPreset (3);
        if (proc.presets.isModified()) ++bad;
        setParam (proc, ID::key, 5.0f);
        if (proc.presets.isModified()) ++bad;
        setParam (proc, ID::clip (1, "Drive"), 3.0f);
        if (! proc.presets.isModified()) ++bad;
        proc.presets.loadPreset (3);
        if (proc.presets.isModified()) ++bad;
        setParam (proc, ID::key, 0.0f);
        std::cout << "\nModified marker: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
    }

    // ---- Fuzz: random values for every parameter, several sample rates: always finite and bounded ----
    {
        int bad = 0;
        juce::Random rng (20261110);
        const double rates[] = { 44100.0, 48000.0, 96000.0 };
        const int iterations = 60;
        float worstPeak = 0.0f;
        for (int it = 0; it < iterations; ++it)
        {
            proc.presets.loadPreset (rng.nextInt (proc.presets.getNumFactory()));
            for (auto* prm : proc.getParameters())
                if (rng.nextFloat() < 0.35f)   // perturb about a third of the parameters each time
                    prm->setValueNotifyingHost (rng.nextFloat());
            // keep the linear-phase EQs at moderate resolution so the fuzz stays quick
            for (int e = 0; e < 2; ++e) setParam (proc, ID::eqRes (e), 0.0f);
            const double rate = rates[it % 3];
            const auto out = render (proc, 1, 48 + rng.nextInt (24), rate, it % 2 ? 512 : 100);
            bool finite = true;
            for (int ch = 0; ch < 2 && finite; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    if (! std::isfinite (out.getSample (ch, i))) { finite = false; break; }
            const float pk = out.getMagnitude (0, out.getNumSamples());
            worstPeak = std::max (worstPeak, pk);
            if (! finite || pk > 7.95f) { ++bad; std::cout << "   fuzz iteration " << it << ": " << (finite ? "peak " + juce::String (pk) : juce::String ("NaN/inf")) << "\n"; }
        }
        std::cout << "\nFuzz: " << iterations << " random settings at 44.1 / 48 / 96 kHz, worst peak "
                  << juce::String (juce::Decibels::gainToDecibels (worstPeak), 1) << " dBFS: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        proc.presets.loadPreset (0);
        // presets never touch the key settings: put them back by hand
        resetKey (proc);
        proc.setRateAndBufferSizeDetails (sr, block);
        proc.prepareToPlay (sr, block);
    }

    // ---- Click audit: jump every continuous parameter during a sustained note ----
    if (std::getenv ("CODEX_CLICK_AUDIT") != nullptr)
    {
        std::vector<juce::String> clicky;
        auto setup = [&]
        {
            proc.presets.loadPreset (0);
            setParam (proc, ID::pAmount, 0.0f); setParam (proc, ID::aSustain, 1.0f); setParam (proc, ID::clickLevel, 0.0f);
            for (const auto& id : { ID::sat ("On"), ID::cmp ("On"), ID::spc ("On"), ID::clipOn, ID::subOn }) setParam (proc, id, 1.0f);
            EQDisplay::addBand (proc.apvts, 0, eq::BandType::Bell, 300.0f, 3.0f, 1.0f);
            EQDisplay::addBand (proc.apvts, 1, eq::BandType::Bell, 2000.0f, -3.0f, 1.0f);
            setParam (proc, ID::clip (1, "FltPos"), 1.0f);
        };
        auto curvature = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            float m = 0.0f;
            for (int i = juce::jmax (2, from); i < to; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    m = std::max (m, std::abs (b.getSample (ch, i) - 2.0f * b.getSample (ch, i - 1) + b.getSample (ch, i - 2)));
            return m;
        };
        int tested = 0;
        for (auto* prm : proc.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm);
            if (rp == nullptr || dynamic_cast<juce::AudioParameterFloat*> (rp) == nullptr) continue;   // switches and choices step by design
            const auto id = rp->paramID;
            if (id == ID::key || id.startsWith ("macro") || id.contains ("Used")) continue;
            setup();
            rp->setValueNotifyingHost (0.3f);
            proc.prepareToPlay (sr, block);
            const int total = (int) (1.2 * sr) / block * block, change = (int) (0.6 * sr) / block * block;   // whole blocks only
            juce::AudioBuffer<float> out (2, total), buf (2, block);
            for (int pos = 0; pos < total; pos += block)
            {
                juce::MidiBuffer midi;
                if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
                if (pos == change) rp->setValueNotifyingHost (0.7f);
                buf.clear();
                proc.processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, block);
            }
            const float before = curvature (out, change - (int) (0.15 * sr), change);
            const float after = curvature (out, change + (int) (0.25 * sr), change + (int) (0.45 * sr));
            const float around = curvature (out, change, change + (int) (0.03 * sr));
            const float ref = std::max (before, after);
            ++tested;
            if (around > 3.0f * ref + 1.0e-4f)
            {
                clicky.push_back (id);
                std::cout << "   click: " << rp->getName (40).paddedRight (' ', 40) << " x" << juce::String (around / juce::jmax (1.0e-6f, ref), 1) << "\n";
            }
        }
        std::cout << "\nClick audit: " << tested << " parameters jumped mid-note, " << (int) clicky.size() << " clicked\n";
        failures += (int) clicky.size();
        proc.presets.loadPreset (0);
    }

    // ---- Module order: encoding, Space keeps the kick, click-free reorder, old sessions keep the Classic order ----
    {
        int bad = 0;
        for (int i = 0; i < chain::numOrders; ++i)
        {
            const auto o = chain::decode (i);
            std::array<bool, chain::numModules> seen {};
            for (int m : o) seen[(size_t) m] = true;
            if (chain::encode (o) != i || std::find (seen.begin(), seen.end(), false) != seen.end()) ++bad;
        }
        if (chain::modernIndex() != 0 || chain::decode (chain::classicIndex()) != chain::classicOrder) ++bad;

        // Space at 50 % mix must not turn the kick down (the dry stays at unity), in either position
        auto earlyRms = [] (const juce::AudioBuffer<float>& b)
        {
            double e = 0.0;
            const int n = (int) (0.06 * sr);
            for (int i = 0; i < n; ++i) e += 0.5 * (b.getSample (0, i) * b.getSample (0, i) + b.getSample (1, i) * b.getSample (1, i));
            return juce::Decibels::gainToDecibels ((float) std::sqrt (e / n));
        };
        float dips[2] {};
        for (int pos = 0; pos < 2; ++pos)
        {
            proc.presets.loadPreset (0);
            setParam (proc, ID::clickLevel, 0.0f);
            setParam (proc, ID::chainOrder, (float) (pos == 0 ? chain::modernIndex() : chain::classicIndex()));
            const float dry = earlyRms (render (proc, 1));
            setParam (proc, ID::spc ("On"), 1.0f);
            setParam (proc, ID::spc ("Mix"), 0.5f);
            dips[pos] = earlyRms (render (proc, 1)) - dry;
            if (dips[pos] < -1.0f) ++bad;
        }

        // Reorder mid-note with every module on: no click
        proc.presets.loadPreset (0);
        setParam (proc, ID::pAmount, 0.0f); setParam (proc, ID::aSustain, 1.0f); setParam (proc, ID::clickLevel, 0.0f);
        for (const auto& id : { ID::sat ("On"), ID::cmp ("On"), ID::spc ("On") }) setParam (proc, id, 1.0f);
        setParam (proc, ID::clip (1, "OS"), 2.0f);   // latency in the chain, so the hit clocks move with the order
        proc.prepareToPlay (sr, block);
        const int total = (int) (1.2 * sr) / block * block, change = (int) (0.6 * sr) / block * block;
        juce::AudioBuffer<float> out (2, total), buf (2, block);
        for (int pos = 0; pos < total; pos += block)
        {
            juce::MidiBuffer midi;
            if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            if (pos == change) setParam (proc, ID::chainOrder, (float) chain::classicIndex());
            buf.clear();
            proc.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, block);
        }
        auto curvature = [&] (int from, int to)
        {
            float m = 0.0f;
            for (int i = juce::jmax (2, from); i < to; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    m = std::max (m, std::abs (out.getSample (ch, i) - 2.0f * out.getSample (ch, i - 1) + out.getSample (ch, i - 2)));
            return m;
        };
        const float ref = std::max (curvature (change - (int) (0.15 * sr), change), curvature (change + (int) (0.25 * sr), change + (int) (0.45 * sr)));
        const float around = curvature (change, change + (int) (0.03 * sr));
        if (around > 3.0f * ref + 1.0e-4f) ++bad;

        // A session saved before the order existed loads as Classic; a saved order is restored
        juce::MemoryBlock state;
        setParam (proc, ID::chainOrder, 1234.0f);
        proc.getStateInformation (state);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        setParam (proc, ID::chainOrder, 0.0f);
        proc.setStateInformation (state.getData(), (int) state.getSize());
        const int restored = (int) proc.apvts.getRawParameterValue (ID::chainOrder)->load();
        int legacy = -1;
        if (xml != nullptr)
        {
            for (auto* c : xml->getChildIterator())
                if (c->getStringAttribute ("id") == ID::chainOrder) { xml->removeChildElement (c, true); break; }
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
            setParam (proc, ID::chainOrder, 0.0f);
            proc.setStateInformation (old.getData(), (int) old.getSize());
            legacy = (int) proc.apvts.getRawParameterValue (ID::chainOrder)->load();
        }
        if (restored != 1234 || legacy != chain::classicIndex()) ++bad;

        std::cout << "\nModule order: " << chain::numOrders << " orders round-trip, Space at 50 % changes the kick by "
                  << juce::String (dips[0], 2) << " dB (first) / " << juce::String (dips[1], 2) << " dB (last), reorder mid-note x"
                  << juce::String (around / juce::jmax (1.0e-6f, ref), 2) << ", saved order " << restored << ", old session -> "
                  << chain::describe (legacy) << "\n";
        std::cout << "Module order checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        proc.presets.loadPreset (0);
    }

    // ---- 909 hardware behaviour: Tune = sweep speed, Depth, Accent, Overload, Decay Hold ----
    {
        int bad = 0;
        auto clean909 = [&]
        {
            proc.presets.loadPreset (7);   // 909 Clean: no distortion
            setParam (proc, ID::velocity, 0.0f);   // velocity does not scale the level, so accent is isolated
        };
        // Tune: slower settings keep the pitch higher for longer
        float early[3] {};
        for (int t = 0; t < 3; ++t)
        {
            clean909();
            setParam (proc, ID::tuneTime909, 0.5f * (float) t);
            setParam (proc, ID::attack909, 0.0f);
            early[t] = zeroCrossHz (renderNote (proc, 127, 0.4, 0.5), 0.015, 0.05);
        }
        if (! (early[0] < early[1] && early[1] < early[2])) ++bad;
        // Depth: no change to the speed, more depth = higher start
        clean909(); setParam (proc, ID::tune909, 0.0f); setParam (proc, ID::attack909, 0.0f);   // no click in the window
        const float shallow = zeroCrossHz (renderNote (proc, 127, 0.4, 0.5), 0.002, 0.05);
        clean909(); setParam (proc, ID::tune909, 1.0f); setParam (proc, ID::attack909, 0.0f);
        const float deep = zeroCrossHz (renderNote (proc, 127, 0.4, 0.5), 0.002, 0.05);
        if (shallow <= 0.0f || deep < shallow * 1.3f) ++bad;
        // Accent: velocity >= 100 is louder with accent up; below 100, or with accent at 0, nothing changes
        clean909(); setParam (proc, ID::accent909, 1.0f);
        const float accented = rmsDb (renderNote (proc, 127, 0.4, 0.5), 0.0, 0.1);
        const float plain = rmsDb (renderNote (proc, 90, 0.4, 0.5), 0.0, 0.1);
        clean909();
        const float off = rmsDb (renderNote (proc, 127, 0.4, 0.5), 0.0, 0.1);
        if (accented - plain < 3.0f || std::abs (off - plain) > 0.1f) ++bad;
        // Overload: squarer body (lower crest factor) in the tail
        auto crest = [] (const juce::AudioBuffer<float>& b)
        {
            const int s0 = (int) (0.12 * sr), n = (int) (0.1 * sr);
            return juce::Decibels::gainToDecibels (b.getMagnitude (0, s0, n)) - rmsDb (b, 0.12, 0.1);
        };
        clean909();
        const float crestClean = crest (renderNote (proc, 127, 0.4, 0.5));
        clean909(); setParam (proc, ID::overload909, 1.0f);
        const auto loud = renderNote (proc, 127, 0.4, 0.5);
        const float crestOver = crest (loud);
        bool finite = true;
        for (int i = 0; i < loud.getNumSamples(); ++i) finite &= std::isfinite (loud.getSample (0, i)) && std::abs (loud.getSample (0, i)) < 4.0f;
        if (crestOver > crestClean - 1.0f || ! finite) ++bad;
        // Decay Hold: the body stays up while the note is held
        clean909();
        const auto normal = renderNote (proc, 127, 1.0, 1.2);
        clean909(); setParam (proc, ID::hold909, 1.0f);
        const auto held = renderNote (proc, 127, 1.0, 1.2);
        const float dropNormal = rmsDb (normal, 0.15, 0.05) - rmsDb (normal, 0.85, 0.05);
        const float dropHeld = rmsDb (held, 0.15, 0.05) - rmsDb (held, 0.85, 0.05);
        if (dropHeld > 3.0f || dropNormal < 15.0f) ++bad;

        std::cout << "\n909 hardware: Tune (speed) 15-65 ms " << juce::String (early[0], 1) << " / " << juce::String (early[1], 1) << " / "
                  << juce::String (early[2], 1) << " Hz, Depth start " << juce::roundToInt (shallow) << " -> " << juce::roundToInt (deep)
                  << " Hz, Accent " << juce::String (accented - plain, 1) << " dB, Overload crest " << juce::String (crestClean, 1) << " -> "
                  << juce::String (crestOver, 1) << " dB, Decay Hold 0.15 -> 0.85 s " << juce::String (dropNormal, 1) << " -> "
                  << juce::String (dropHeld, 1) << " dB\n";
        std::cout << "909 hardware checks: " << (bad == 0 ? "OK" : "FAILED") << "\n";
        failures += bad;
        proc.presets.loadPreset (0);
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
        // punch / tail split in the plugin: tail drive changes the tail, not the punch
        {
            auto harmonicDb = [] (const juce::AudioBuffer<float>& b, double t0, double t1)
            {
                // energy above 300 Hz relative to the total (more distortion = more upper harmonics)
                eq::BandSettings hp; hp.on = true; hp.type = eq::BandType::LowCut; hp.freq = 300.0f; hp.slope = 3;
                auto d = eq::design (hp, sr);
                std::vector<eq::Biquad> st ((size_t) d.numStages);
                for (int k = 0; k < d.numStages; ++k) st[(size_t) k].c = d.stages[(size_t) k];
                double hi = 0, all = 0;
                for (int i = 0; i < (int) (t1 * sr); ++i)
                {
                    float v = b.getSample (0, i), h = v;
                    for (auto& f : st) h = f.process (h);
                    if (i >= (int) (t0 * sr)) { hi += (double) h * h; all += (double) v * v; }
                }
                return (float) (10.0 * std::log10 (hi / juce::jmax (1.0e-12, all)));
            };
            proc.presets.loadPreset (8);
            setParam (proc, ID::clip (1, "Drive"), 6.0f);
            const auto plain = render (proc, 1);
            setParam (proc, ID::clip (1, "TailDrv"), 18.0f);
            setParam (proc, ID::clip (1, "Split"), 40.0f);
            const auto split = render (proc, 1);
            const float punchA = harmonicDb (plain, 0.0, 0.012), punchB = harmonicDb (split, 0.0, 0.012);
            const float tailA = harmonicDb (plain, 0.15, 0.3), tailB = harmonicDb (split, 0.15, 0.3);
            std::cout << "  Punch / tail split (+18 dB tail drive): punch harmonics " << juce::String (punchA, 1) << " -> " << juce::String (punchB, 1)
                      << " dB, tail harmonics " << juce::String (tailA, 1) << " -> " << juce::String (tailB, 1) << " dB\n";
            if (std::abs (punchB - punchA) > 1.5f || tailB < tailA + 3.0f) ++bad;
            proc.presets.loadPreset (0);
        }
        // Mix must not comb-filter: at low level (almost linear) any mix setting must stay between the dry and wet response
        {
            auto response = [] (ClipDistortion::Settings st, float mix, float f)
            {
                st.mix = mix;
                const int N = 1 << 14, pre = 16384;
                ClipDistortion d; d.prepare (sr, 256); d.setSettings (st); d.reset(); d.setSettings (st);
                juce::AudioBuffer<float> b (2, N + pre);
                for (int i = 0; i < b.getNumSamples(); ++i)
                { const float v = 0.01f * (float) std::sin (juce::MathConstants<double>::twoPi * f * i / sr); b.setSample (0, i, v); b.setSample (1, i, v); }
                for (int p = 0; p < b.getNumSamples(); p += 256) d.process (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (256, b.getNumSamples() - p));
                return juce::Decibels::gainToDecibels (b.getRMSLevel (0, pre, N) / (0.01f * 0.70710678f));
            };
            struct Case { const char* name; ClipDistortion::Settings s; };
            std::vector<Case> cases;
            auto base = [] { ClipDistortion::Settings s; s.mode = ClipDistortion::Tanh; s.driveDb = 0.0f; s.outDb = 0.0f; return s; };
            { auto s = base(); cases.push_back ({ "defaults (4x)", s }); }
            { auto s = base(); s.oversampling = 0; cases.push_back ({ "oversampling off", s }); }
            { auto s = base(); s.hiFilterHz = 40.0f; cases.push_back ({ "Hi Filter 40 Hz", s }); }
            { auto s = base(); s.loFilterHz = 9000.0f; cases.push_back ({ "Lo Filter 9 kHz", s }); }
            { auto s = base(); s.filterPos = 1; s.filterType = 2; s.filterHz = 180.0f; s.filterRes = 0.55f; cases.push_back ({ "Pre band-pass 180 Hz", s }); }
            { auto s = base(); s.shelfDb = -6.0f; cases.push_back ({ "Shelf -6 dB", s }); }
            const float freqs[] = { 30.0f, 45.0f, 60.0f, 90.0f, 150.0f, 400.0f, 1000.0f, 3000.0f, 8000.0f, 15000.0f };
            std::cout << "  Mix 50 % vs the dry / wet responses (dB below the quieter of the two; > 1 dB = a notch):\n    ";
            for (float f : freqs) std::cout << juce::String (f < 1000 ? juce::String ((int) f) : juce::String ((int) (f / 1000)) + "k").paddedLeft (' ', 6);
            std::cout << "\n";
            for (auto& c : cases)
            {
                std::cout << "    ";
                float worst = 0.0f;
                for (float f : freqs)
                {
                    const float dry = response (c.s, 0.0f, f), wet = response (c.s, 1.0f, f), half = response (c.s, 0.5f, f);
                    // equal-gain sum of two signals can never be quieter than the average level if they are in phase
                    const float floorDb = juce::Decibels::gainToDecibels (0.5f * (juce::Decibels::decibelsToGain (dry) + juce::Decibels::decibelsToGain (wet)));
                    const float dip = floorDb - half;
                    worst = std::max (worst, dip);
                    std::cout << juce::String (dip, 1).paddedLeft (' ', 6);
                }
                std::cout << "   " << c.name << "\n";
                // a resonant band-pass blended with the dry signal is a filter effect by nature: reported, not failed
                if (worst > 0.5f && juce::String (c.name).startsWith ("Pre band-pass") == false) ++bad;
            }
        }
        // multiband saturation: the global Mix must not notch around the crossovers either
        {
            auto satResponse = [] (float mix, float f)
            {
                sat::Settings st; st.enabled = true; st.numBands = 4; st.mix = mix; st.oversampling = 0;
                for (auto& b : st.bands) { b.driveDb = 0.0f; b.style = 0; }
                sat::MultibandSaturator m; m.prepare (sr, 256); m.setSettings (st);
                const int N = 1 << 14, pre = 16384;
                juce::AudioBuffer<float> b (2, N + pre);
                for (int i = 0; i < b.getNumSamples(); ++i)
                { const float v = 0.01f * (float) std::sin (juce::MathConstants<double>::twoPi * f * i / sr); b.setSample (0, i, v); b.setSample (1, i, v); }
                for (int p = 0; p < b.getNumSamples(); p += 256) m.process (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (256, b.getNumSamples() - p));
                return juce::Decibels::gainToDecibels (b.getRMSLevel (0, pre, N) / (0.01f * 0.70710678f));
            };
            float worst = 0.0f;
            std::cout << "  Saturation global Mix 50 % (dB below the dry / wet average) at 4 bands, crossovers 120 / 400 / 1200 Hz:\n    ";
            for (float f : { 60.0f, 120.0f, 250.0f, 400.0f, 800.0f, 1200.0f, 3000.0f })
            {
                const float dry = satResponse (0.0f, f), wet = satResponse (1.0f, f), half = satResponse (0.5f, f);
                const float avg = juce::Decibels::gainToDecibels (0.5f * (juce::Decibels::decibelsToGain (dry) + juce::Decibels::decibelsToGain (wet)));
                worst = std::max (worst, avg - half);
                std::cout << (int) f << " Hz " << juce::String (avg - half, 1) << "   ";
            }
            std::cout << "\n";
            if (worst > 0.5f) ++bad;
        }
        // every other blend: compressor global Mix (multiband, lookahead), saturation band Mix with tone controls
        {
            auto sineThrough = [] (auto&& processor, float f)
            {
                const int N = 1 << 14, pre = 16384;
                juce::AudioBuffer<float> b (2, N + pre);
                for (int i = 0; i < b.getNumSamples(); ++i)
                { const float v = 0.01f * (float) std::sin (juce::MathConstants<double>::twoPi * f * i / sr); b.setSample (0, i, v); b.setSample (1, i, v); }
                for (int p = 0; p < b.getNumSamples(); p += 256) processor (b.getWritePointer (0, p), b.getWritePointer (1, p), std::min (256, b.getNumSamples() - p));
                return juce::Decibels::gainToDecibels (b.getRMSLevel (0, pre, N) / (0.01f * 0.70710678f));
            };
            auto dipTable = [&] (const char* name, auto&& make)
            {
                float worst = 0.0f;
                std::cout << "  " << juce::String (name).paddedRight (' ', 44);
                for (float f : { 60.0f, 120.0f, 250.0f, 400.0f, 1000.0f, 3000.0f, 8000.0f })
                {
                    auto lv = [&] (float mix) { auto p = make (mix); return sineThrough ([&] (float* l, float* r, int n) { p->process (l, r, n); }, f); };
                    const float dry = lv (0.0f), wet = lv (1.0f), half = lv (0.5f);
                    const float avg = juce::Decibels::gainToDecibels (0.5f * (juce::Decibels::decibelsToGain (dry) + juce::Decibels::decibelsToGain (wet)));
                    worst = std::max (worst, avg - half);
                    std::cout << juce::String (avg - half, 1).paddedLeft (' ', 6);
                }
                std::cout << "\n";
                return worst;
            };
            std::cout << "  Mix 50 % dip (dB) at                         60   120   250   400    1k    3k    8k\n";
            const float c1 = dipTable ("Compressor 4 bands + lookahead, global Mix", [] (float mix)
            {
                auto c = std::make_unique<comp::MultibandCompressor>(); c->prepare (sr);
                comp::Settings st; st.enabled = true; st.numBands = 4; st.lookahead = 3; st.mix = mix; st.autoGain = false;
                for (auto& b : st.bands) { b.ratio = 1.0f; b.thresholdDb = 0.0f; }
                c->setSettings (st); return c;
            });
            const float c2 = dipTable ("Saturation band Mix, tone controls on", [] (float mix)
            {
                auto m = std::make_unique<sat::MultibandSaturator>(); m->prepare (sr, 256);
                sat::Settings st; st.enabled = true; st.numBands = 1; st.oversampling = 0;
                auto& b = st.bands[0]; b.driveDb = 0.0f; b.style = 0; b.mix = mix; b.bassDb = 6.0f; b.trebleDb = -6.0f; b.presenceDb = 4.0f;
                m->setSettings (st); return m;
            });
            const float c3 = dipTable ("Saturation band Mix, 2x oversampling", [] (float mix)
            {
                auto m = std::make_unique<sat::MultibandSaturator>(); m->prepare (sr, 256);
                sat::Settings st; st.enabled = true; st.numBands = 2; st.oversampling = 1;
                for (auto& b : st.bands) { b.driveDb = 0.0f; b.style = 0; b.mix = mix; }
                m->setSettings (st); return m;
            });
            if (c1 > 0.5f || c3 > 0.5f) ++bad;
            juce::ignoreUnused (c2);   // tone controls in parallel are an EQ effect: reported
        }
        // Auto Gain: Drive changes colour, not loudness; Raw: deliberately aliasing
        {
            auto kickRms = [&] (float drive, bool autoGain)
            {
                proc.presets.loadPreset (8);
                setParam (proc, ID::clip (1, "Drive"), drive);
                setParam (proc, ID::clip (1, "AutoGain"), autoGain ? 1.0f : 0.0f);
                setParam (proc, ID::clip (2, "On"), 0.0f);
                const auto out = render (proc, 4);   // several hits: the auto gain follows slowly
                return juce::Decibels::gainToDecibels (out.getRMSLevel (0, (int) (1.2 * sr), (int) (0.35 * sr)));
            };
            const float lo = kickRms (6.0f, false), hi = kickRms (30.0f, false), loA = kickRms (6.0f, true), hiA = kickRms (30.0f, true);
            std::cout << "  Auto Gain: Drive 6 -> 30 dB changes the level by " << juce::String (hi - lo, 1) << " dB without, "
                      << juce::String (hiA - loA, 1) << " dB with Auto Gain\n";
            if (std::abs (hiA - loA) > 1.5f || std::abs (hiA - loA) > std::abs (hi - lo) * 0.5f) ++bad;
            const float rawAlias = aliasDb (ClipDistortion::rawOversampling), offAlias = aliasDb (0);
            std::cout << "  Raw (no anti-aliasing): " << juce::String (rawAlias, 1) << " dB vs Off with ADAA " << juce::String (offAlias, 1) << " dB\n";
            if (rawAlias < offAlias + 6.0f) ++bad;
            proc.presets.loadPreset (0);
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
    setParam (proc, ID::clip (1, "TailDrv"), 10.0f); setParam (proc, ID::clip (1, "Mode"), (float) ClipDistortion::Tube);
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
    snapshot (proc, render (proc, 1), outDir.getChildFile ("ui_guide.png"), 0, 4);
    // ---- Key Mode: Follow MIDI by default, old sessions without a Key Mode stay on Fixed Key ----
    {
        int bad = 0;
        {
            CodexProcessor fresh;
            const bool followDefault = fresh.apvts.getRawParameterValue (ID::keyMode)->load() > 0.5f;
            bad += followDefault ? 0 : 1;
            fresh.presets.loadPreset (0);   // Init leaves the key settings alone
            bad += fresh.apvts.getRawParameterValue (ID::keyMode)->load() > 0.5f ? 0 : 1;
            std::cout << "\nKey Mode default: " << (followDefault ? "Follow MIDI" : "Fixed Key") << ", kept by Init: "
                      << (bad == 0 ? "OK" : "FAILED") << "\n";
        }
        // Follow MIDI plays only the note name, in the kick's register: keyBaseNote + ((n - keyBaseNote) mod 12) + 12 * (oct - 1),
        // exactly where Fixed Key puts that key (any E plays the kick's E), and Key must not matter.
        const auto followTarget = [] (int n, int octIndex) { return Params::keyBaseNote + ((n - Params::keyBaseNote) % 12 + 12) % 12 + 12 * (octIndex - 1); };
        const auto centsOff = [] (float hz, float note) { return hz > 0 ? 1200.0f * std::log2 (hz / midiToFrequency (note)) : 9999.0f; };
        proc.presets.loadPreset (1);
        setParam (proc, ID::key, 5.0f);   // A#: must not matter
        setParam (proc, ID::keyMode, 1.0f);
        for (int note : { 29, 41, 60, 64 })   // default preset: F, F, C, E (E4 = 64 plays E2 like Fixed Key = E)
        {
            const int expect = followTarget (note, 1);
            const float hz = measurePitch (render (proc, 2, note), 0.15, 0.17);
            const float cents = centsOff (hz, (float) expect);
            const bool ok = std::abs (cents) < 25.0f;
            bad += ok ? 0 : 1;
            std::cout << "Follow MIDI note " << note << " -> " << juce::MidiMessage::getMidiNoteName (expect, true, true, 4) << ": "
                      << juce::String (hz, 2) << " Hz (" << juce::String (cents, 1) << " c): " << (ok ? "OK" : "FAILED") << "\n";
        }
        // Every MIDI note 0..127 with Oct -1 / 0 / +1, on a plain sine patch (no pitch sweep, nothing that bends the root):
        // the note the voice plays (the tuner target) and the measured audio pitch for every one of them (the detector
        // goes down to 15 Hz here: Oct -1 puts F..G# at 21.8..26 Hz)
        {
            setParam (proc, ID::tune, 0.0f);
            setParam (proc, ID::pAmount, 0.0f);   // the root from the first sample
            int wrongNote = 0, wrongPitch = 0, measured = 0;
            float worst = 0.0f;
            const auto start = juce::Time::getMillisecondCounterHiRes();
            for (int oct = 0; oct < 3; ++oct)
            {
                setParam (proc, ID::octave, (float) oct);
                for (int n = 0; n < 128; ++n)
                {
                    const int expect = followTarget (n, oct);
                    auto audio = render (proc, 1, n);
                    wrongNote += proc.getPlayedNote() == expect && std::abs (proc.getTargetNote() - (float) expect) < 1.0e-4f ? 0 : 1;
                    {
                        const float hz = measurePitch (audio, 0.1, 0.24, sr, 15.0f);
                        const float cents = centsOff (hz, (float) expect);
                        worst = std::max (worst, std::abs (cents));
                        wrongPitch += std::abs (cents) < 10.0f ? 0 : 1;
                        ++measured;
                        if (std::abs (cents) >= 10.0f)
                            std::cout << "  note " << n << " oct " << (oct - 1) << ": " << juce::String (hz, 2) << " Hz, expected "
                                      << juce::String (midiToFrequency ((float) expect), 2) << " Hz\n";
                    }
                }
            }
            const bool ok = wrongNote == 0 && wrongPitch == 0;
            bad += ok ? 0 : 1;
            std::cout << "Follow MIDI notes 0..127 x Oct -1/0/+1: " << (384 - wrongNote) << "/384 play keyBase + (n - keyBase) mod 12 + 12*(oct-1); "
                      << measured << " measured, worst " << juce::String (worst, 2) << " c (< 10 c), "
                      << juce::String ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) << " s: " << (ok ? "OK" : "FAILED") << "\n";
            setParam (proc, ID::octave, 1.0f);
        }
        // Tuner target (and preview) follow the last played note in Follow MIDI; the Key before any note and in Fixed Key
        {
            CodexProcessor fresh;
            setParam (fresh, ID::key, 3.0f);    // G#
            setParam (fresh, ID::keyMode, 1.0f);
            setParam (fresh, ID::tune, 0.5f);
            const float beforeNote = fresh.getTargetNote();
            bool ok = std::abs (beforeNote - ((float) fresh.getKeyNote() + 0.5f)) < 1.0e-4f;
            render (fresh, 1, 76);              // E5 -> the kick's E (E2 = 40)
            const float afterE = fresh.getTargetNote();
            ok &= std::abs (afterE - 40.5f) < 1.0e-4f;
            setParam (fresh, ID::octave, 2.0f); // Oct +1 moves the target with it
            const float afterOct = fresh.getTargetNote();
            ok &= std::abs (afterOct - 52.5f) < 1.0e-4f;
            setParam (fresh, ID::octave, 1.0f);
            setParam (fresh, ID::keyMode, 0.0f);
            const float fixedTarget = fresh.getTargetNote();
            ok &= std::abs (fixedTarget - ((float) fresh.getKeyNote() + 0.5f)) < 1.0e-4f;
            render (fresh, 1, 64);              // Fixed Key: a played note leaves the target on the Key
            ok &= std::abs (fresh.getTargetNote() - fixedTarget) < 1.0e-4f;
            setParam (fresh, ID::keyMode, 1.0f);
            ok &= std::abs (fresh.getTargetNote() - 40.5f) < 1.0e-4f;   // back in Follow MIDI: the last Follow MIDI note
            // the preview button plays that same note
            setParam (fresh, ID::tune, 0.0f);
            setParam (fresh, ID::pAmount, 0.0f);
            fresh.setRateAndBufferSizeDetails (sr, block);
            fresh.prepareToPlay (sr, block);
            juce::AudioBuffer<float> pv (2, (int) (0.4 * sr)), buf (2, block);
            juce::MidiBuffer none;
            fresh.triggerPreview (true);
            for (int pos = 0; pos < pv.getNumSamples(); pos += block)
            {
                const int n = std::min (block, pv.getNumSamples() - pos);
                buf.setSize (2, n, false, false, true);
                fresh.processBlock (buf, none);
                for (int ch = 0; ch < 2; ++ch) pv.copyFrom (ch, pos, buf, ch, 0, n);
            }
            fresh.triggerPreview (false);
            const float pvHz = measurePitch (pv, 0.1, 0.2);
            const float pvCents = centsOff (pvHz, 40.0f);
            ok &= std::abs (pvCents) < 10.0f;
            bad += ok ? 0 : 1;
            std::cout << "Follow MIDI tuner target: before a note " << juce::String (beforeNote, 2) << " (Key + Tune), after E5 "
                      << juce::String (afterE, 2) << ", Oct +1 " << juce::String (afterOct, 2) << ", Fixed Key " << juce::String (fixedTarget, 2)
                      << "; preview " << juce::String (pvHz, 2) << " Hz (" << juce::String (pvCents, 1) << " c): " << (ok ? "OK" : "FAILED") << "\n";
        }
        proc.presets.loadPreset (1);
        setParam (proc, ID::key, 5.0f);
        setParam (proc, ID::keyMode, 1.0f);
        // Export and Sub ALIGN render the selected Key also in Follow MIDI mode (their offline copy plays note 60)
        {
            const float target = juce::MidiMessage::getMidiNoteInHertz (proc.getKeyNote());
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto file = KickExporter::render (proc, outDir.getChildFile ("export_follow_midi.wav"), 0.4, sr);
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            float hz = 0.0f;
            if (reader != nullptr)
            {
                juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
                reader->read (&b, 0, b.getNumSamples(), 0, true, true);
                hz = measurePitch (b, 0.2, 0.15);
            }
            const float cents = hz > 0 ? 1200.0f * std::log2 (hz / target) : 9999.0f;
            const bool ok = std::abs (cents) < 25.0f && proc.apvts.getRawParameterValue (ID::keyMode)->load() > 0.5f;
            bad += ok ? 0 : 1;
            std::cout << "Follow MIDI export: " << juce::String (hz, 2) << " Hz (Key " << juce::String (target, 2) << " Hz, "
                      << juce::String (cents, 1) << " c): " << (ok ? "OK" : "FAILED") << "\n";
            const float followPhase = KickExporter::findBestSubPhase (proc);
            setParam (proc, ID::keyMode, 0.0f);
            const float fixedPhase = KickExporter::findBestSubPhase (proc);
            setParam (proc, ID::keyMode, 1.0f);
            const bool same = followPhase == fixedPhase;
            bad += same ? 0 : 1;
            std::cout << "Follow MIDI sub ALIGN: " << followPhase << " deg (Fixed Key " << fixedPhase << " deg): " << (same ? "OK" : "FAILED") << "\n";
        }
        // A session saved before Key Mode existed (no PARAM keyMode) restores as Fixed Key
        {
            juce::MemoryBlock st;
            proc.getStateInformation (st);
            auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.removeChild (tree.getChildWithProperty ("id", ID::keyMode), nullptr);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
            proc.setStateInformation (old.getData(), (int) old.getSize());
            const bool fixed = proc.apvts.getRawParameterValue (ID::keyMode)->load() < 0.5f;
            bad += fixed ? 0 : 1;
            std::cout << "Old session without Key Mode restores as Fixed Key: " << (fixed ? "OK" : "FAILED") << "\n";
        }
        resetKey (proc);
        failures += bad;
    }

    // ---- Keyboard focus: clicking, hovering and menus anywhere in the editor never keep the computer keyboard ----
    {
        int bad = 0;
        juce::SharedResourcePointer<CodexSettings> codexSettings;
        auto* userSettings = codexSettings->user();
        const bool hadShortcutSetting = userSettings != nullptr && userSettings->containsKey ("keyboardShortcuts");
        const bool oldShortcutSetting = userSettings != nullptr && userSettings->getBoolValue ("keyboardShortcuts", false);
        if (userSettings != nullptr) userSettings->setValue ("keyboardShortcuts", false);
        juce::MemoryBlock savedState;
        proc.getStateInformation (savedState);
        proc.presets.loadPreset (8);

        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* hk = dynamic_cast<CodexEditor*> (editor.get());
        auto& view = hk->getView();
        hk->setSize (MainView::baseWidth, MainView::baseHeight);
        // in a real window, as a host shows it (the window is the top-level component, like a plug-in wrapper)
        juce::DocumentWindow window ("Codex focus test", juce::Colours::black, juce::DocumentWindow::closeButton, true);
        window.setUsingNativeTitleBar (true);
        // like a host's plug-in wrapper, the window itself never wants the keyboard (a DocumentWindow does by default, so
        // JUCE could park focus on it when a value box closes - that is the test window, not Codex)
        window.setWantsKeyboardFocus (false);
        window.setContentNonOwned (editor.get(), true);
        window.setVisible (true);
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
        pump (150);
        auto* peer = window.getPeer();
        auto* top = peer->getComponent().getTopLevelComponent();
        juce::int64 clock = juce::Time::currentTimeMillis();
        auto send = [&] (juce::Point<float> pos, juce::ModifierKeys mods)
        {
            // presses far apart (never a double-click), releases soon after the press (a click, not a long press)
            clock += mods.isAnyMouseButtonDown() ? 700 : 60;
            peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, pos, mods, juce::MouseInputSource::defaultPressure,
                                    juce::MouseInputSource::defaultOrientation, clock);
        };
        auto focusedInEditor = [&] () -> juce::Component*
        {
            auto* f = juce::Component::getCurrentlyFocusedComponent();
            return f != nullptr && (f == top || top->isParentOf (f)) ? f : nullptr;
        };
        auto isInlineTextEditor = [] (juce::Component* f) { return dynamic_cast<juce::TextEditor*> (f) != nullptr && dynamic_cast<juce::Label*> (f->getParentComponent()) != nullptr; };
        auto closeOverlays = [&]
        {
            juce::PopupMenu::dismissAllActiveMenus();
            for (auto* c : view.getChildren())
                if (auto* o = dynamic_cast<Overlay*> (c)) o->setVisible (false);
        };
        // an empty spot of the view (for clicking away from a text field)
        juce::Point<float> background;
        for (int y = MainView::baseHeight - 4; y > 100 && background.isOrigin(); y -= 7)
            for (int x = MainView::baseWidth - 4; x > 400; x -= 7)
                if (view.getComponentAt (x, y) == &view) { background = peer->getComponent().getLocalPoint (&view, juce::Point<float> ((float) x, (float) y)); break; }

        int clicked = 0, textFields = 0, focusEvents = 0;
        juce::StringArray offenders;
        auto report = [&] (juce::Component* c, const char* when)
        {
            if (auto* f = focusedInEditor())
            {
                ++focusEvents;
                if (offenders.size() < 6)
                    offenders.add (juce::String (when) + " " + typeid (*c).name() + " -> focus on " + typeid (*f).name());
                f->giveAwayKeyboardFocus();
            }
        };
        for (int tab = 0; tab <= 6; ++tab)
        {
            hk->showTab (tab);
            pump (30);
            std::vector<juce::Component::SafePointer<juce::Component>> comps;
            std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (! child->isShowing() || dynamic_cast<ExportButton*> (child) != nullptr || dynamic_cast<Overlay*> (child) != nullptr) continue;
                    comps.emplace_back (child);
                    collect (*child);
                }
            };
            collect (view);
            for (auto& sp : comps)
            {
                auto* c = sp.getComponent();
                if (c == nullptr || ! c->isShowing() || c->getWidth() < 2 || c->getHeight() < 2) continue;
                const auto pos = peer->getComponent().getLocalPoint (c, c->getLocalBounds().toFloat().getCentre());
                if (! peer->getComponent().getLocalBounds().toFloat().contains (pos)) continue;
                ++clicked;
                send (pos, {});                                      // hover
                report (c, "hover");
                send (pos, juce::ModifierKeys::leftButtonModifier);  // press
                if (! isInlineTextEditor (focusedInEditor())) report (c, "mouse down");
                send (pos, {});                                      // release
                pump (8);                                            // combo box popups open asynchronously
                closeOverlays();
                pump (4);
                if (auto* f = focusedInEditor(); f != nullptr && isInlineTextEditor (f))
                {
                    // a knob's value box: typing a value is allowed, a click elsewhere hands the keyboard back
                    ++textFields;
                    send (background, {});
                    send (background, juce::ModifierKeys::leftButtonModifier);
                    send (background, {});
                    pump (8);
                }
                report (c, "click");
                if (hk->getView().isShowing()) hk->showTab (tab);   // a click may have switched tabs
            }
        }
        std::cout << "\nKeyboard focus: " << clicked << " components hovered and clicked on 7 pages (" << textFields
                  << " value boxes typed into and left), editor keeps no focus: "
                  << (focusEvents == 0 ? "OK" : "FAILED (" + juce::String (focusEvents) + " times)") << "\n";
        for (auto& o : offenders) std::cout << "   " << o << "\n";
        bad += focusEvents > 0 ? 1 : 0;

        // The save dialog's name field takes the keyboard; closing the dialog gives it back
        {
            int b = 0;
            view.openSaveDialog();
            auto* f = focusedInEditor();
            b += dynamic_cast<juce::TextEditor*> (f) != nullptr ? 0 : 1;
            SavePresetOverlay* save = nullptr;
            for (auto* c : view.getChildren()) if (auto* o = dynamic_cast<SavePresetOverlay*> (c)) save = o;
            juce::TextButton* cancel = nullptr;
            if (save != nullptr)
                for (auto* c : save->getChildren())
                    if (auto* tb = dynamic_cast<juce::TextButton*> (c); tb != nullptr && tb->getButtonText() == "CANCEL") cancel = tb;
            if (cancel != nullptr)
            {
                const auto pos = peer->getComponent().getLocalPoint (cancel, cancel->getLocalBounds().toFloat().getCentre());
                send (pos, {}); send (pos, juce::ModifierKeys::leftButtonModifier); send (pos, {});
            }
            else ++b;
            pump (250);
            b += focusedInEditor() == nullptr && (save == nullptr || ! save->isVisible()) ? 0 : 1;
            // the browser opens without taking the keyboard
            view.openBrowser();
            pump (30);
            b += focusedInEditor() == nullptr ? 0 : 1;
            closeOverlays();
            std::cout << "Keyboard focus: save dialog name field focused on open, released on close; browser opens unfocused: "
                      << (b == 0 ? "OK" : "FAILED") << "\n";
            bad += b;
        }

        // With "Keyboard shortcuts in Codex" on, a click gives the editor the keyboard (and switching it off releases it)
        {
            int b = 0;
            hk->showTab (0);
            pump (30);
            view.setKeyboardShortcutsEnabled (true);
            juce::Component* knob = nullptr;
            std::function<void (juce::Component&)> findSlider = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (knob != nullptr) return;
                    if (dynamic_cast<juce::Slider*> (child) != nullptr && child->isShowing()) { knob = child; return; }
                    findSlider (*child);
                }
            };
            findSlider (view);
            if (knob != nullptr)
            {
                const auto pos = peer->getComponent().getLocalPoint (knob, knob->getLocalBounds().toFloat().getCentre());
                send (pos, {}); send (pos, juce::ModifierKeys::leftButtonModifier); send (pos, {});
            }
            pump (30);
            b += focusedInEditor() != nullptr ? 0 : 1;
            const bool handled = view.keyPressed (juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0));
            b += handled ? 0 : 1;
            closeOverlays();
            view.setKeyboardShortcutsEnabled (false);
            pump (30);
            b += focusedInEditor() == nullptr ? 0 : 1;
            std::cout << "Keyboard focus: with keyboard shortcuts on a click focuses the editor, switching off releases it: "
                      << (b == 0 ? "OK" : "FAILED") << "\n";
            bad += b;
        }

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
        resetKey (proc);
        failures += bad;
    }

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
