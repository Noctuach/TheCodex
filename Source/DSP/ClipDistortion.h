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
    enum Mode { Hard = 0, Soft, Tanh, Fold };

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
        switch (m)
        {
            case Hard: return juce::jlimit (-1.0f, 1.0f, x);
            case Soft:
            {
                if (x >= 1.0f)  return 1.0f;
                if (x <= -1.0f) return -1.0f;
                return 1.5f * (x - x * x * x / 3.0f);
            }
            case Tanh: return std::tanh (x);
            case Fold: return std::sin (x * juce::MathConstants<float>::halfPi);
            default:   return x;
        }
    }

private:
    float shape (float x) const noexcept { return shapeFor (mode, x); }

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int mode = Hard;
    float targetDrive = 1.0f, targetMix = 1.0f, targetOut = 1.0f, targetBias = 0.0f;
    juce::SmoothedValue<float> drive, mix, outGain, bias;
    float dcR = 0.999f, dcX = 0.0f, dcY = 0.0f;
};
