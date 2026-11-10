#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "DSP/KickVoice.h"
#include "DSP/ClipDistortion.h"
#include "DSP/ParametricEQ.h"
#include "DSP/MultibandSaturator.h"
#include "DSP/SpaceReverb.h"
#include "DSP/Compressor.h"
#include "DSP/EqEngine.h"
#include "DSP/OutputClipper.h"
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

class CodexProcessor : public juce::AudioProcessor
{
public:
    CodexProcessor();
    ~CodexProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Codex"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return reverb.tailSeconds(); }

    int getNumPrograms() override { return presets.getNumFactory(); }
    int getCurrentProgram() override { return juce::jmax (0, presets.getCurrentIndex()); }
    void setCurrentProgram (int index) override { presets.loadPreset (index); }
    const juce::String getProgramName (int index) override { return presets.getFactoryNames()[index]; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Called by the GUI preview button: press = note on, release = note off. */
    void triggerPreview (bool down) { (down ? previewOn : previewOff).store (true); }

    ClipDistortion& getClip (int stage) { return stage == 1 ? clip1 : clip2; }
    sat::MultibandSaturator& getSaturator() { return saturator; }
    space::SpaceReverb& getReverb() { return reverb; }
    comp::MultibandCompressor& getCompressor() { return compressor; }
    /** Share of the available processing time used by the audio callback (0..1, for the CPU readout). */
    double getCpuLoad() const { return loadMeasurer.getLoadAsProportion(); }
    OutputClipper& getClipper() { return clipper; }

    /** Output peak since last read, per channel (0 = left, 1 = right). */
    float getAndResetPeak (int channel) { return peaks[(size_t) channel].exchange (0.0f); }

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets { apvts };

    //==========================================================================
    // Hit-synced scope capture: min/max of Left, Right, Mid and Side for each display point
    // (written by the audio thread from each note-on, read by the GUI).
    static constexpr int scopePoints = 3200;   // hit capture resolution (min/max per point)
    enum ScopeChannel { ScopeLeft = 0, ScopeRight, ScopeMid, ScopeSide, numScopeChannels };
    std::array<std::array<float, scopePoints>, numScopeChannels> scopeMin {}, scopeMax {};
    std::atomic<int> scopeWritten { 0 }, scopeGeneration { 0 };
    std::atomic<float> scopeWindowSeconds { 0.5f };   // set by the GUI
    std::atomic<float> scopeCapturedSeconds { 0.5f }; // window of the current capture

    /** Mid signal for the EQ analyzer and the tuner; left/right for the scope page. */
    AnalyzerFifo analyzer, analyzerPre, analyzerPre2, tunerFeed, scopeFeedL, scopeFeedR;   // analyzerPre(2): EQ 1 / EQ 2 input (mid)

    /** MIDI note of the selected key (F1..E2, shifted by the octave setting). */
    int getKeyNote() const;
    /** Exact root the kick is tuned to, including the Tune parameter (fractional MIDI note). */
    float getTargetNote() const { return (float) getKeyNote() + tune->load(); }

private:
    void updateParameters();
    void handleMidi (const juce::MidiMessage&, int positionInChunk);
    void startScope();
    void feedScope (float left, float right) noexcept;
    void updateLatency();

    static constexpr int chunkSize = 256;

    KickVoice voice;
    ClipDistortion clip1, clip2;
    std::array<eq::EqEngine, ID::numEqs> equalisers;   // [0] between the distortions, [1] post (before Space)
    sat::MultibandSaturator saturator;
    space::SpaceReverb reverb;
    comp::MultibandCompressor compressor;
    juce::AudioProcessLoadMeasurer loadMeasurer;
    OutputClipper clipper;
    std::atomic<float> *clipOnParam = nullptr, *clipCeilParam = nullptr, *clipSoftParam = nullptr;
    std::vector<float> left, right, mid;
    juce::SmoothedValue<float> outGain;

    std::atomic<bool> previewOn { false }, previewOff { false };
    int captureArmAt = -1, scopePos = -1, scopeCount = 0, scopeSamplesPerPoint = 40;
    std::array<float, numScopeChannels> scopeCurMin {}, scopeCurMax {};
    std::array<std::atomic<float>, 2> peaks {};

    struct BandParams { std::atomic<float>* on, * type, * freq, * gain, * q, * slope, * place; };
    struct ClipParams { std::atomic<float> *on, *mode, *drive, *boost, *chr, *bias, *loFilt, *hiFilt, *shelfF, *shelfG, *mix, *out, *dc, *os, *spatial,
                               *fltPos, *fltType, *fltFreq, *fltRes;
                        std::array<std::atomic<float>*, ClipDistortion::numCurvePoints> xp {}, xn {}; };
    struct SatBandParams { std::atomic<float> *style, *drive, *mix, *fb, *fbFreq, *dyn, *bass, *midT, *treble, *pres, *drivePan, *levelPan, *level, *mute, *solo, *bypass; };

    std::atomic<float>* p (const juce::String& id) { return apvts.getRawParameterValue (id); }

    std::atomic<float> *engine, *model909, *tune909, *attack909, *decay909, *key, *octave, *keyMode;
    std::atomic<float> *wave, *tune, *phase, *clickLevel, *clickDecay, *clickTone;
    std::atomic<float> *pAmount, *pAttack, *pDecay, *pSustain, *pRelease, *pCurve;
    std::atomic<float> *aAttack, *aDecay, *aSustain, *aRelease, *aCurve;
    std::atomic<float> *outGainParam, *eqScaleParam;
    ClipParams clipParams[2];
    BandParams bandParams[ID::numEqs][Params::numBands];
    std::atomic<float>* eqScaleParams[ID::numEqs] {}, * eqOnParams[ID::numEqs] {}, * eqModeParams[ID::numEqs] {}, * eqResParams[ID::numEqs] {};
    std::atomic<float>* spcParams[25] {};
    std::atomic<float>* drawModes[2] {}, * drawCounts[2] {};
    std::atomic<float>* macros[ID::numMacros] {};
    std::atomic<float>* drawT[2][DrawEnvelope::maxPoints] {}, * drawL[2][DrawEnvelope::maxPoints] {}, * drawB[2][DrawEnvelope::maxPoints] {};
    std::atomic<float>* cmpGlobal[13] {};
    std::atomic<float>* cmpBandParams[comp::maxBands][11] {};
    std::atomic<float> *satOn, *satBands, *satSlope, *satOs, *satMix, *satOut, *satIn, *satMode;
    std::array<std::atomic<float>*, sat::maxBands - 1> satX {};
    SatBandParams satBandParams[sat::maxBands];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CodexProcessor)
};
