#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "DSP/KickVoice.h"
#include "DSP/ClipDistortion.h"
#include "DSP/ParametricEQ.h"
#include "PresetManager.h"

/** Lock-free single-producer/single-consumer sample FIFO feeding the spectrum analyzer. */
class AnalyzerFifo
{
public:
    void push (const float* data, int num) noexcept
    {
        int s1, b1, s2, b2;
        fifo.prepareToWrite (num, s1, b1, s2, b2);
        if (b1 > 0) std::copy (data, data + b1, buffer.begin() + s1);
        if (b2 > 0) std::copy (data + b1, data + b1 + b2, buffer.begin() + s2);
        fifo.finishedWrite (b1 + b2);
    }

    int pull (float* dest, int maxNum) noexcept
    {
        int s1, b1, s2, b2;
        fifo.prepareToRead (maxNum, s1, b1, s2, b2);
        if (b1 > 0) std::copy (buffer.begin() + s1, buffer.begin() + s1 + b1, dest);
        if (b2 > 0) std::copy (buffer.begin() + s2, buffer.begin() + s2 + b2, dest + b1);
        fifo.finishedRead (b1 + b2);
        return b1 + b2;
    }

private:
    static constexpr int size = 32768;
    juce::AbstractFifo fifo { size };
    std::array<float, size> buffer {};
};

class HardKickProcessor : public juce::AudioProcessor
{
public:
    HardKickProcessor();
    ~HardKickProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HardKick"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return presets.getNumFactory(); }
    int getCurrentProgram() override { return juce::jmax (0, presets.getCurrentIndex()); }
    void setCurrentProgram (int index) override { presets.loadPreset (index); }
    const juce::String getProgramName (int index) override { return presets.getFactoryNames()[index]; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Called by the GUI "KICK" button: press = note on, release = note off. */
    void triggerPreview (bool down) { (down ? previewOn : previewOff).store (true); }

    ClipDistortion& getClip (int stage) { return stage == 1 ? clip1 : clip2; }

    /** Peak since last read (for the GUI meter). */
    float getAndResetPeak() { return peak.exchange (0.0f); }

    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets { apvts };
    AnalyzerFifo analyzer, tunerFeed;

    /** MIDI note of the selected key (F1..E2, shifted by the octave setting). */
    int getKeyNote() const;
    /** Exact root the kick is tuned to, including the Tune parameter (fractional MIDI note). */
    float getTargetNote() const { return (float) getKeyNote() + tune->load(); }

private:
    void updateParameters();
    void handleMidi (const juce::MidiMessage&);

    static constexpr int chunkSize = 256;

    KickVoice voice;
    ClipDistortion clip1, clip2;
    eq::ParametricEQ<Params::numBands> equaliser;
    std::vector<float> mono;
    juce::SmoothedValue<float> outGain;

    std::atomic<bool> previewOn { false }, previewOff { false };
    std::atomic<float> peak { 0.0f };

    struct BandParams { std::atomic<float>* on, * type, * freq, * gain, * q, * slope; };
    struct ClipParams { std::atomic<float> *on, *mode, *drive, *boost, *chr, *bias, *loFilt, *hiFilt, *shelfF, *shelfG, *mix, *out, *dc, *os; };

    std::atomic<float>* p (const juce::String& id) { return apvts.getRawParameterValue (id); }

    std::atomic<float> *engine, *model909, *tune909, *attack909, *decay909, *key, *octave, *keyMode;
    std::atomic<float> *wave, *tune, *phase, *clickLevel, *clickDecay, *clickTone;
    std::atomic<float> *pAmount, *pAttack, *pDecay, *pSustain, *pRelease, *pCurve;
    std::atomic<float> *aAttack, *aDecay, *aSustain, *aRelease, *aCurve;
    std::atomic<float> *outGainParam;
    ClipParams clipParams[2];
    BandParams bandParams[Params::numBands];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardKickProcessor)
};
