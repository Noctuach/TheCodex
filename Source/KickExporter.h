#pragma once
#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>

/**
    Renders the current kick to a WAV file with an offline copy of the plugin (same state, same key),
    so the instance that is playing is never touched. The note is held for `holdSeconds`, then the
    tail (release, reverb) is rendered until it falls below -90 dBFS (at most 4 s more).
*/
struct KickExporter
{
    static juce::File render (CodexProcessor& source, const juce::File& file, double holdSeconds = 0.4, double sampleRate = 0.0)
    {
        juce::MemoryBlock state;
        source.getStateInformation (state);
        const double rate = sampleRate > 0.0 ? sampleRate : (source.getSampleRate() > 0.0 ? source.getSampleRate() : 48000.0);

        CodexProcessor offline;
        offline.setStateInformation (state.getData(), (int) state.getSize());
        constexpr int block = 512;
        offline.setPlayConfigDetails (0, 2, rate, block);
        offline.setRateAndBufferSizeDetails (rate, block);
        offline.prepareToPlay (rate, block);
        offline.setNonRealtime (true);

        // let the linear-phase EQ filters build before rendering
        juce::AudioBuffer<float> warm (2, block);
        juce::MidiBuffer none;
        for (int i = 0; i < 20; ++i) { warm.clear(); offline.processBlock (warm, none); juce::Thread::sleep (2); }

        const int latency = offline.getLatencySamples();
        const int hold = (int) (holdSeconds * rate);
        const int maxLen = hold + (int) (4.0 * rate) + latency;
        juce::AudioBuffer<float> out (2, maxLen + block);
        out.clear();
        int written = 0, quietRun = 0;
        const float quiet = juce::Decibels::decibelsToGain (-90.0f);
        juce::AudioBuffer<float> buf (2, block);
        for (int pos = 0; pos < maxLen; pos += block)
        {
            juce::MidiBuffer midi;
            const int note = 60;
            if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            if (pos <= hold && hold < pos + block) midi.addEvent (juce::MidiMessage::noteOff (1, note), hold - pos);
            buf.clear();
            offline.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, block);
            written = pos + block;
            if (pos > hold + latency)
            {
                quietRun = buf.getMagnitude (0, block) < quiet ? quietRun + block : 0;
                if (quietRun > (int) (0.05 * rate)) break;   // the tail has died away
            }
        }
        offline.releaseResources();

        // drop the plugin latency at the start, then trim trailing silence
        int start = juce::jmin (latency, written), end = written;
        while (end > start + 1 && std::abs (out.getSample (0, end - 1)) < quiet && std::abs (out.getSample (1, end - 1)) < quiet) --end;
        const int len = juce::jmax (1, end - start);
        // a 2 ms fade at the very end so the file never ends on a click
        const int fade = juce::jmin (len, (int) (0.002 * rate));
        for (int ch = 0; ch < 2; ++ch) out.applyGainRamp (ch, start + len - fade, fade, 1.0f, 0.0f);

        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr) return {};
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 2, 24, {}, 0));
        if (writer == nullptr) return {};
        stream.release();   // the writer owns the stream now
        writer->writeFromAudioSampleBuffer (out, start, len);
        writer.reset();
        return file;
    }

    /** "Codex - <preset> - <key>.wav" in a temp folder, for dragging into a DAW. */
    static juce::File defaultFile (CodexProcessor& p)
    {
        // key name as the plugin shows it (F1 = 43.65 Hz); '#' is kept, it is valid in file names
        const int note = p.getKeyNote();
        const auto keyName = juce::MidiMessage::getMidiNoteName (note, true, true, 4);
        const auto name = juce::File::createLegalFileName ("Codex - " + p.presets.getCurrentName()) + " - " + keyName + ".wav";
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Codex Exports");
        dir.createDirectory();
        return dir.getChildFile (name);
    }
};
