#pragma once
#include <juce_dsp/juce_dsp.h>

/**
    Output clipper (4x oversampled): linear below the knee, a smooth (tanh) knee up to the ceiling,
    then a hard ceiling at the base rate so inter-sample overshoot from the downsampling filter can
    never exceed it. Softness 0 = hard clip, 1 = a wide knee starting 12 dB below the ceiling.
*/
class OutputClipper
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os->initProcessing ((size_t) maxBlock);
        ceilingGain.reset (sampleRate * 4.0, 0.02);
        juce::ignoreUnused (sampleRate);
    }

    void setParameters (bool on, float ceilingDb, float softness)
    {
        if (on && ! enabled && os != nullptr) os->reset();   // no stale filter state when switched back on
        enabled = on;
        ceilingGain.setTargetValue (juce::Decibels::decibelsToGain (ceilingDb));
        knee = juce::jlimit (0.0f, 1.0f, softness);
    }

    int getLatencyInSamples() const { return enabled && os != nullptr ? juce::roundToInt (os->getLatencyInSamples()) : 0; }

    /** Static curve for a signal normalised to the ceiling (|x| = 1 at the ceiling). */
    static float curve (float x, float softness) noexcept
    {
        const float a = std::abs (x);
        const float kneeStart = 1.0f - 0.75f * softness;   // softness 1: knee from -12 dB
        if (a <= kneeStart || softness <= 0.0f) return juce::jlimit (-1.0f, 1.0f, x);
        const float width = 1.0f - kneeStart;
        const float y = kneeStart + width * std::tanh ((a - kneeStart) / width);
        return x < 0.0f ? -y : y;
    }

    void process (float* l, float* r, int n) noexcept
    {
        if (! enabled || os == nullptr) return;
        float* ch[] = { l, r };
        juce::dsp::AudioBlock<float> block (ch, 2, (size_t) n);
        auto up = os->processSamplesUp (block);
        auto* ul = up.getChannelPointer (0);
        auto* ur = up.getChannelPointer (1);
        float reduction = 0.0f;
        for (size_t i = 0; i < up.getNumSamples(); ++i)
        {
            const float c = ceilingGain.getNextValue(), inv = 1.0f / c;
            for (auto* p : { ul, ur })
            {
                const float in = p[i];
                p[i] = c * curve (in * inv, knee);
                reduction = juce::jmax (reduction, std::abs (in) - std::abs (p[i]));
            }
        }
        os->processSamplesDown (block);
        const float c = ceilingGain.getCurrentValue();
        for (int i = 0; i < n; ++i) { l[i] = juce::jlimit (-c, c, l[i]); r[i] = juce::jlimit (-c, c, r[i]); }
        if (reduction > clipAmount.load()) clipAmount.store (reduction);
    }

    /** Largest amount clipped since the last call (linear), for the GUI. */
    float getAndResetClip() { return clipAmount.exchange (0.0f); }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> ceilingGain { 1.0f };
    float knee = 0.3f;
    bool enabled = false;
    std::atomic<float> clipAmount { 0.0f };
};
