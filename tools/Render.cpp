// Offline test tool: renders kicks through the full plugin to a WAV file and snapshots the editor to PNG.
// Usage: HardKickRender <out.wav> [out.png]
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"

static void setParam (HardKickProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: HardKickRender out.wav [out.png]\n"; return 1; }
    juce::ScopedJuceInitialiser_GUI init;

    const double sr = 48000.0;
    const int block = 512;
    HardKickProcessor proc;
    proc.setPlayConfigDetails (0, 2, sr, block);

    // A typical starting point: tighten the low end, give the EQ something to show
    setParam (proc, ID::band (0, "On"), 1.0f);              // low cut at 30 Hz
    setParam (proc, ID::band (2, "On"), 1.0f);              // bell
    setParam (proc, ID::band (2, "Freq"), 180.0f);
    setParam (proc, ID::band (2, "Gain"), 6.0f);
    setParam (proc, ID::band (2, "Q"), 1.4f);
    setParam (proc, ID::band (4, "On"), 1.0f);              // dip
    setParam (proc, ID::band (4, "Freq"), 3500.0f);
    setParam (proc, ID::band (4, "Gain"), -8.0f);

    proc.prepareToPlay (sr, block);
    std::cout << "Latency: " << proc.getLatencySamples() << " samples\n";

    // Four kicks at 150 BPM (0.4 s apart), each held for 0.3 s, on F1
    const int beat = (int) (0.4 * sr), hold = (int) (0.3 * sr), total = beat * 4 + (int) (0.3 * sr);
    juce::AudioBuffer<float> out (2, total), buf (2, block);
    out.clear();

    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        buf.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        for (int k = 0; k < 4; ++k)
        {
            const int on = k * beat, off = on + hold;
            if (on >= pos && on < pos + n)   midi.addEvent (juce::MidiMessage::noteOn (1, 29, (juce::uint8) 127), on - pos);
            if (off >= pos && off < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, 29), off - pos);
        }
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, buf, ch, 0, n);
    }

    float pk = out.getMagnitude (0, total);
    bool finite = true;
    for (int i = 0; i < total; ++i) finite &= std::isfinite (out.getSample (0, i));
    std::cout << "Peak: " << juce::Decibels::gainToDecibels (pk) << " dBFS, RMS(first kick): "
              << juce::Decibels::gainToDecibels (out.getRMSLevel (0, 0, beat)) << " dBFS, finite=" << finite << "\n";

    juce::File wavFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    wavFile.deleteFile();
    juce::WavAudioFormat wav;
    if (auto stream = std::unique_ptr<juce::FileOutputStream> (wavFile.createOutputStream()))
        if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0)))
        {
            stream.release();
            writer->writeFromAudioSampleBuffer (out, 0, total);
        }
    std::cout << "Wrote " << wavFile.getFullPathName() << "\n";

    // State save/restore round trip
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    HardKickProcessor proc2;
    proc2.setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "State round-trip band3 freq: " << proc2.apvts.getRawParameterValue (ID::band (2, "Freq"))->load() << "\n";

    if (argc >= 3)
    {
        // Feed the analyzer with the last kick so the snapshot shows a spectrum
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        proc.analyzer.push (out.getReadPointer (0, beat * 3), 4096);
        for (int i = 0; i < 3; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
        auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        juce::File pngFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        pngFile.deleteFile();
        juce::FileOutputStream os (pngFile);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::cout << "Wrote " << pngFile.getFullPathName() << "\n";
    }
    return 0;
}
