// Loads a built Codex.vst3 through JUCE's VST3 hosting (like a DAW would) and checks that it
// instantiates, reports its buses and parameters, plays MIDI, saves/restores state and opens its editor.
//   CodexHostTest <path/to/Codex.vst3>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: CodexHostTest <Codex.vst3>\n"; return 1; }
    std::cout << std::unitbuf;   // flush every line (useful when a step hangs)
    std::cout << "Loading " << argv[1] << "\n";
    juce::ScopedJuceInitialiser_GUI init;
    int failures = 0;
    auto check = [&failures] (bool ok, const juce::String& what) { std::cout << (ok ? "  OK    " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; };

    juce::AudioPluginFormatManager formats;
    formats.addFormat (new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (found, juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]).getFullPathName());
    check (found.size() == 1, "plug-in found in the bundle");
    if (found.isEmpty()) return 2;
    const auto& desc = *found[0];
    std::cout << "        " << desc.name << " by " << desc.manufacturerName << ", version " << desc.version
              << ", category " << desc.category << ", instrument: " << (desc.isInstrument ? "yes" : "no") << "\n";
    check (desc.isInstrument, "registers as an instrument");

    juce::String error;
    auto plugin = formats.createPluginInstance (desc, 48000.0, 512, error);
    check (plugin != nullptr, "instantiates" + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));
    if (plugin == nullptr) return 3;

    check (plugin->getTotalNumOutputChannels() == 2, "stereo output");
    check (plugin->acceptsMidi(), "accepts MIDI");
    std::cout << "        " << plugin->getParameters().size() << " parameters, latency " << plugin->getLatencySamples() << " samples\n";
    check (plugin->getParameters().size() > 100, "parameters exposed to the host");

    plugin->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    float peak = 0.0f;
    bool finite = true;
    for (int blk = 0; blk < 60; ++blk)
    {
        juce::MidiBuffer midi;
        if (blk == 2)  midi.addEvent (juce::MidiMessage::noteOn (1, 41, (juce::uint8) 110), 100);
        if (blk == 40) midi.addEvent (juce::MidiMessage::noteOff (1, 41), 0);
        buffer.clear();
        plugin->processBlock (buffer, midi);
        peak = std::max (peak, buffer.getMagnitude (0, 512));
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i) finite &= std::isfinite (buffer.getSample (ch, i));
    }
    std::cout << "        output peak " << juce::Decibels::gainToDecibels (peak) << " dBFS\n";
    check (peak > 0.05f && peak < 2.0f && finite, "plays a note (audible, finite, not clipping hard)");

    // Program (preset) switching through the host
    std::cout << "        " << plugin->getNumPrograms() << " programs\n";
    check (plugin->getNumPrograms() >= 19, "factory presets exposed as programs");
    plugin->setCurrentProgram (8);
    check (plugin->getProgramName (plugin->getCurrentProgram()).isNotEmpty(), "program change: " + plugin->getProgramName (plugin->getCurrentProgram()));

    // State save / restore
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    auto* p0 = plugin->getParameters()[10];
    const float before = p0->getValue();
    p0->setValueNotifyingHost (before > 0.5f ? 0.1f : 0.9f);
    plugin->setStateInformation (state.getData(), (int) state.getSize());
    check (std::abs (p0->getValue() - before) < 1.0e-4f, "state save / restore (" + juce::String ((int) state.getSize()) + " bytes)");

    // Editor: shown in a real window while the message loop runs (like a DAW), then closed
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorIfNeeded());
        check (editor != nullptr && editor->getWidth() > 100, "editor opens (" + (editor ? juce::String (editor->getWidth()) + " x " + juce::String (editor->getHeight()) : juce::String ("none")) + ")");
        if (editor != nullptr)
        {
            juce::DocumentWindow window ("Codex host test", juce::Colours::black, juce::DocumentWindow::closeButton, true);
            window.setUsingNativeTitleBar (true);
            window.setContentNonOwned (editor.get(), true);
            window.setVisible (true);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);   // let it draw for a while
            check (window.isShowing(), "editor shown in a window and drawn for 1.5 s");
            window.clearContentComponent();
            window.setVisible (false);
            editor.reset();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        }
        std::cout << "        editor closed\n";
    }

    plugin->releaseResources();
    std::cout << "        resources released\n";
    plugin.reset();
    std::cout << "        plug-in deleted\n";
    std::cout << (failures == 0 ? "HOST TEST PASSED" : juce::String (failures) + " HOST CHECK(S) FAILED") << "\n";
    return failures == 0 ? 0 : 4;
}
