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
#include "DSP/SubPhaseMatch.h"
#include "DSP/Chain.h"
#include "PresetManager.h"
#include "CustomCurveStore.h"

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
        for (int i = 0; i < b1 + b2; ++i)   // reader side: a non-finite sample (cut by the output guard) must not stick in a display
            if (! std::isfinite (dest[i])) dest[i] = 0.0f;
        return b1 + b2;
    }

private:
    static constexpr int size = 32768;
    juce::AbstractFifo fifo { size };
    std::array<float, size> buffer {};
};

class CodexProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    CodexProcessor();
    ~CodexProcessor() override { cancelPendingUpdate(); }

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
    std::atomic<bool> safetyTripped { false };   // set if a non-finite sample was ever replaced (diagnostics)
    std::atomic<int> safetyRecoveries { 0 };     // how often the guard reset the DSP state after a non-finite sample
    /** Tests only: the next processed chunk gets a NaN at this sample of its voice signal (before every effect), as a
        fault inside the chain would put one there (not called by the plug-in). */
    void injectNonFiniteForTesting (int sampleInChunk) noexcept { nanTestSample.store (juce::jlimit (0, chunkSize - 1, sampleInChunk)); }
    /** True when every filter is ready (linear-phase EQ filters built and installed); used before offline renders. */
    bool isSettled() const { return equalisers[0].isLinearUpToDate() && equalisers[1].isLinearUpToDate(); }

    /** Tests / benchmarks only: makes renders reproducible by fixing the click-noise seed (not called by the plug-in). */
    void seedNoiseForTesting (juce::int64 seed) { voice.seedNoise (seed); }
    /** Tests / benchmarks only: every linear-phase EQ is up to date or has its new filters built, ready to install. */
    bool isEqBuildReadyForTesting() const { return equalisers[0].isLinearBuildReadyForTesting() && equalisers[1].isLinearBuildReadyForTesting(); }

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
    /** Key index (0 = F .. 11 = E) of a MIDI note's note name: Follow MIDI keeps only the pitch class. */
    static int keyIndexForMidiNote (int midiNote) noexcept
    {
        return ((midiNote - Params::keyBaseNote) % 12 + 12) % 12;
    }
    /** Kick note a MIDI note plays in Follow MIDI: its note name in the kick register (F1..E2, shifted by Oct),
        exactly where Fixed Key would put that key. */
    int getFollowNote (int midiNote) const;
    /** Kick note the instance plays now: in Follow MIDI the note name of the last MIDI note (in the current Oct),
        otherwise (or before any note was played) the selected Key. Used by the tuner target and the preview button. */
    int getPlayedNote() const;
    /** Counts note-ons (MIDI and preview), so the editor can animate each hit. */
    std::atomic<unsigned int> hitCounter { 0 };

    /** Exact root the kick is tuned to, including the Tune parameter (fractional MIDI note). */
    float getTargetNote() const { return (float) getPlayedNote() + tune->load(); }

    //==========================================================================
    // Custom distortion curves (message thread; see CustomCurveStore)
    CustomCurveStore& getCustomCurves() { return customCurves; }
    /** Tests: host notices sent for curve edits so far (pending ones are sent first). */
    int flushCurveChangeNoticesForTesting() { if (curveChangeNotifier.isPending()) curveChangeNotifier.flushForTesting(); return curveChangeNotifier.notices; }
    /** The distortion settings the unit (1 or 2) gets from the parameters and macros right now. */
    ClipDistortion::Settings buildClipSettings (int stage) const;
    /** A custom curve that reproduces the unit's current Type at its current settings. */
    FreeCurve seedCustomCurve (int stage) const;
    bool isCustomEnabled (int stage) const;
    /** Switches "Dist N Custom Curve" through the state tree, so it joins the open undo transaction. */
    void setCustomEnabled (int stage, bool on);
    /** Starts an edit of the unit's curve (one undo step): seeds a curve from the Type when there is none (or always,
        with reseed) and switches the custom curve on. */
    void beginCustomEdit (int stage, const juce::String& transactionName, bool reseed = false);

private:
    void updateParameters();
    /** Audio thread: a non-finite sample reached the output guard. Clears the DSP state of every module that can hold
        it (allocation- and lock-free) and starts a short fade-in, so the plug-in plays on instead of staying silent. */
    void recoverFromNonFinite() noexcept;
    void handleMidi (const juce::MidiMessage&, int positionInChunk);
    void startScope();
    void feedScope (float left, float right) noexcept;
    void updateLatency();
    void handleAsyncUpdate() override;
    std::atomic<int> latencyTarget { 0 };
    std::atomic<int> lastFollowKey { -1 };   // key index of the last Follow MIDI note (audio thread writes, GUI reads); -1 = none yet

    static constexpr int chunkSize = 256;
    // Non-finite guard: every parameter's raw value with a finite fallback (its default), checked once per block, so a
    // NaN / inf a host or a damaged state sends never reaches the DSP. Built in the constructor (no audio-thread allocation).
    struct GuardedParam { std::atomic<float>* raw; float fallback; juce::RangedAudioParameter* param; };
    std::vector<GuardedParam> paramGuard;
    // the audio-thread guard only fixes the raw value: the parameter itself (what the host, the GUI and the saved state
    // read) is set to its default on the message thread (at once when the guard runs there: offline renders, tests)
    std::atomic<bool> paramRepairPending { false };
    void repairNonFiniteParameters();
    /** Message thread: every PARAM value in a state tree that is not finite becomes the parameter's default. */
    void sanitiseParameterState (juce::ValueTree& state) const;
    float recoverFade = 1.0f;     // < 1: the output fading back in after a recovery
    float recoverInFade = 1.0f;   // < 1: the chain's input (voice and sub) fading back in after a recovery
    std::atomic<int> nanTestSample { -1 };

    KickVoice voice;
    ClipDistortion clip1, clip2;
    CustomCurveStore customCurves { apvts, { &clip1, &clip2 } };
    /** Tells the host that the plug-in state changed outside its parameters (a custom curve edit), so the project is
        marked modified. Message thread only for the host call; a whole drag or pencil stroke coalesces into one notice. */
    struct CurveChangeNotifier : private juce::AsyncUpdater, private juce::Timer
    {
        explicit CurveChangeNotifier (juce::AudioProcessor& p) : processor (p) {}
        ~CurveChangeNotifier() override { cancelPendingUpdate(); stopTimer(); }
        void changed() { triggerAsyncUpdate(); }   // any thread
        void cancel() { cancelPendingUpdate(); if (juce::MessageManager::existsAndIsCurrentThread()) stopTimer(); }
        bool isPending() const { return isUpdatePending() || isTimerRunning(); }
        void flushForTesting() { cancelPendingUpdate(); stopTimer(); notify(); }
        int notices = 0;   // tests
    private:
        void handleAsyncUpdate() override { startTimer (250); }   // restarted by every change of a drag
        void timerCallback() override { stopTimer(); notify(); }
        void notify()
        {
            ++notices;
            processor.updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
        }
        juce::AudioProcessor& processor;
    } curveChangeNotifier { *this };
    bool restoringState = false;   // setStateInformation: the host itself set the state, no change notice
    std::array<eq::EqEngine, ID::numEqs> equalisers;   // [0] between the distortions, [1] post (before Space)
    sat::MultibandSaturator saturator;
    space::SpaceReverb reverb;
    comp::MultibandCompressor compressor;
    juce::AudioProcessLoadMeasurer loadMeasurer;
    OutputClipper clipper;
    std::atomic<float> *clipOnParam = nullptr, *clipCeilParam = nullptr, *clipSoftParam = nullptr;
    std::vector<float> left, right, mid, sub, hitTime, hitTime2;
    // Clean sub layer: delayed by the effect chain's latency (whole samples in the ring buffer, the fraction in a
    // Thiran all-pass) and given the phase of the chain's crossovers, so it stays phase-aligned with the kick
    std::vector<float> subDelay;
    int subDelayPos = 0;
    subalign::FractionalDelay subFrac;
    subalign::PhaseMatch subMatch;
    space::OnePole subLp1, subLp2;
    juce::SmoothedValue<float> subGain;
    std::atomic<float> *subOnParam = nullptr, *subLevelParam = nullptr, *subLpParam = nullptr, *subFadeParam = nullptr,
                       *subPhaseParam = nullptr, *velocityParam = nullptr;
    /** Latency of the effect chain (before the sub layer is added), fractional; the clipper's comes after it. */
    float chainLatency() const;
    void updateSubPhaseMatch();
    void processModule (int module, float* l, float* r, int n, float& latencySoFar);

    // Module order: switched while the chain is faded out (a few ms), so reordering never clicks
    std::atomic<float>* chainOrderParam = nullptr;
    int activeOrderIndex = 0;
    chain::Order activeOrder = chain::modernOrder;
    float orderFade = 1.0f;
    bool orderFadingOut = false;
    juce::SmoothedValue<float> outGain;

    std::atomic<bool> previewOn { false }, previewOff { false };
    int captureArmAt = -1, scopePos = -1, scopeCount = 0, scopeSamplesPerPoint = 40;
    std::array<float, numScopeChannels> scopeCurMin {}, scopeCurMax {};
    std::array<std::atomic<float>, 2> peaks {};

    struct BandParams { std::atomic<float>* on, * type, * freq, * gain, * q, * slope, * place; };
    struct ClipParams { std::atomic<float> *on, *mode, *drive, *boost, *chr, *bias, *loFilt, *hiFilt, *shelfF, *shelfG, *mix, *out, *dc, *os, *spatial,
                               *fltPos, *fltType, *fltFreq, *fltRes, *tailDrv, *split, *autoGain, *cust;
                        std::array<std::atomic<float>*, ClipDistortion::numCurvePoints> xp {}, xn {}; };
    struct SatBandParams { std::atomic<float> *style, *drive, *mix, *fb, *fbFreq, *dyn, *bass, *midT, *treble, *pres, *drivePan, *levelPan, *level, *mute, *solo, *bypass; };

    std::atomic<float>* p (const juce::String& id) { return apvts.getRawParameterValue (id); }

    std::atomic<float> *engine, *model909, *tune909, *attack909, *decay909, *key, *octave, *keyMode;
    std::atomic<float> *tuneTime909, *accent909, *overload909, *hold909;
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
