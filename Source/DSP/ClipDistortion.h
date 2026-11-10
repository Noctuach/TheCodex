#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

/**
    Oversampled clip distortion stage.
    drive -> (bias) -> waveshaper -> remove bias offset -> dry/wet mix -> output gain -> DC blocker
*/
class ClipDistortion
{
public:
    enum Mode { HardClip = 0, SoftClip, Tanh, Overdrive, Tube, Diode, Fuzz, SineFold, TriFold, Rectify, Bitcrush, AsymClip };

    static juce::StringArray modeNames()
    {
        return { "Hard Clip", "Soft Clip", "Tanh", "Overdrive", "Tube", "Diode",
                 "Fuzz", "Sine Fold", "Tri Fold", "Rectify", "Bitcrush", "Asym Clip" };
    }

    static constexpr int oversamplingOrder = 2; // 2^2 = 4x

    void prepare (double sr, int maxBlockSize)
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            1, oversamplingOrder, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampler->initProcessing ((size_t) maxBlockSize);

        const double osRate = sr * (1 << oversamplingOrder);
        for (auto* s : { &drive, &mix, &outGain, &bias })
            s->reset (osRate, 0.02);

        drive.setCurrentAndTargetValue (targetDrive);
        mix.setCurrentAndTargetValue (targetMix);
        outGain.setCurrentAndTargetValue (targetOut);
        bias.setCurrentAndTargetValue (targetBias);

        // DC blocker (~10 Hz) at the base rate
        dcR = (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sr);
        reset();
    }

    void reset()
    {
        if (oversampler != nullptr)
            oversampler->reset();
        dcX = dcY = 0.0f;
    }

    float getLatencyInSamples() const { return oversampler != nullptr ? oversampler->getLatencyInSamples() : 0.0f; }

    void setParameters (bool enabled, int newMode, float driveDb, float newBias, float newMix, float outDb)
    {
        mode = newMode;
        // Always run through the oversampler so latency stays constant when bypassed.
        targetDrive = juce::Decibels::decibelsToGain (driveDb);
        targetMix   = enabled ? newMix : 0.0f;
        targetOut   = enabled ? juce::Decibels::decibelsToGain (outDb) : 1.0f;
        targetBias  = newBias;

        drive.setTargetValue (targetDrive);
        mix.setTargetValue (targetMix);
        outGain.setTargetValue (targetOut);
        bias.setTargetValue (targetBias);
    }

    void process (float* data, int numSamples) noexcept
    {
        float* channels[] = { data };
        juce::dsp::AudioBlock<float> block (channels, 1, (size_t) numSamples);
        auto up = oversampler->processSamplesUp (block);
        float* p = up.getChannelPointer (0);

        for (size_t i = 0; i < up.getNumSamples(); ++i)
        {
            const float dry = p[i];
            const float b   = bias.getNextValue();
            const float wet = shape (dry * drive.getNextValue() + b) - shape (b);
            const float m   = mix.getNextValue();
            p[i] = (dry + m * (wet - dry)) * outGain.getNextValue();
        }

        oversampler->processSamplesDown (block);

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = data[i];
            dcY = x - dcX + dcR * dcY;
            dcX = x;
            data[i] = dcY;
        }
    }

    static float shapeFor (int m, float x) noexcept
    {
        using juce::MathConstants;
        switch (m)
        {
            case HardClip:  return juce::jlimit (-1.0f, 1.0f, x);
            case SoftClip:
                if (x >= 1.0f)  return 1.0f;
                if (x <= -1.0f) return -1.0f;
                return 1.5f * (x - x * x * x / 3.0f);
            case Tanh:      return std::tanh (x);
            case Overdrive: return std::atan (x * 1.5f) * (2.0f / MathConstants<float>::pi);
            case Tube:      // asymmetric: negative half saturates earlier -> even harmonics
                return x >= 0.0f ? std::tanh (x) : std::tanh (x * 1.6f) * 0.75f;
            case Diode:     // forward-biased diode curve, strongly asymmetric
                return x >= 0.0f ? 1.0f - std::exp (-1.5f * x) : -0.6f * (1.0f - std::exp (1.2f * x));
            case Fuzz:      // very fast saturation towards a square wave
                return (x >= 0.0f ? 1.0f : -1.0f) * (1.0f - std::exp (-4.0f * std::abs (x)));
            case SineFold:  return std::sin (x * MathConstants<float>::halfPi);
            case TriFold:
            {
                float t = (x + 1.0f) * 0.25f;
                t -= std::floor (t);
                return 1.0f - 4.0f * std::abs (t - 0.5f);
            }
            case Rectify:   return std::tanh (std::abs (x)); // full-wave: adds an octave-up buzz
            case Bitcrush:
            {
                const float c = juce::jlimit (-1.0f, 1.0f, x);
                return std::round (c * 8.0f) / 8.0f;
            }
            case AsymClip:  return juce::jlimit (-0.55f, 1.0f, x);
            default:        return x;
        }
    }

private:
    float shape (float x) const noexcept { return shapeFor (mode, x); }

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int mode = HardClip;
    float targetDrive = 1.0f, targetMix = 1.0f, targetOut = 1.0f, targetBias = 0.0f;
    juce::SmoothedValue<float> drive, mix, outGain, bias;
    float dcR = 0.999f, dcX = 0.0f, dcY = 0.0f;
};
