#pragma once
#include "CurveEnvelope.h"
#include <juce_core/juce_core.h>
#include <cmath>

/**
    Single monophonic kick voice, built the old-fashioned way:
    oscillator + pitch envelope (the "sweep") + amp envelope + a short noise click.
*/
class KickVoice
{
public:
    enum Wave { Sine = 0, Triangle, WarmSine };

    struct Settings
    {
        int   wave = Sine;
        float tuneSemis = 0.0f;
        float startPhaseDeg = 0.0f;
        float pitchAmountSemis = 48.0f;
        float clickLevel = 0.15f;
        float clickDecayMs = 4.0f;
        float clickToneHz = 3500.0f;
    };

    CurveEnvelope pitchEnv, ampEnv;

    void prepare (double sr)
    {
        sampleRate = sr;
        pitchEnv.setSampleRate (sr);
        ampEnv.setSampleRate (sr);
        reset();
    }

    void reset()
    {
        pitchEnv.reset();
        ampEnv.reset();
        clickEnv = 0.0f;
        phase = 0.0;
        hpState = hpPrevIn = 0.0f;
        currentNote = -1;
    }

    void setSettings (const Settings& s)
    {
        settings = s;
        clickMul = std::exp (-1.0f / (float) (std::max (0.1f, s.clickDecayMs) * 0.001 * sampleRate));
        const float rc = 1.0f / (juce::MathConstants<float>::twoPi * s.clickToneHz);
        const float dt = 1.0f / (float) sampleRate;
        hpCoeff = rc / (rc + dt);
    }

    void noteOn (int midiNote)
    {
        currentNote = midiNote;
        baseFreq = 440.0 * std::pow (2.0, (midiNote - 69) / 12.0);
        phase = settings.startPhaseDeg / 360.0;   // hard phase reset: identical punch on every hit
        pitchEnv.noteOn();
        ampEnv.noteOn();
        clickEnv = 1.0f;
    }

    void noteOff (int midiNote)
    {
        if (midiNote != currentNote)
            return;
        pitchEnv.noteOff();
        ampEnv.noteOff();
    }

    void allNotesOff()
    {
        pitchEnv.noteOff();
        ampEnv.noteOff();
    }

    bool isActive() const noexcept { return ampEnv.isActive() || clickEnv > 1.0e-5f; }

    void render (float* out, int numSamples) noexcept
    {
        const double nyquistGuard = 0.45 * sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            if (! isActive())
            {
                out[i] = 0.0f;
                continue;
            }

            const float pEnv = pitchEnv.next();
            const float aEnv = ampEnv.next();

            const double semis = settings.tuneSemis + pEnv * settings.pitchAmountSemis;
            const double freq  = std::min (baseFreq * std::exp2 (semis / 12.0), nyquistGuard);

            float osc = oscillator (phase);
            phase += freq / sampleRate;
            phase -= std::floor (phase);

            // Click: high-passed white noise with a fast exponential decay
            float click = 0.0f;
            if (clickEnv > 1.0e-5f)
            {
                const float noise = random.nextFloat() * 2.0f - 1.0f;
                hpState = hpCoeff * (hpState + noise - hpPrevIn);
                hpPrevIn = noise;
                click = hpState * clickEnv * settings.clickLevel;
                clickEnv *= clickMul;
            }

            out[i] = osc * aEnv + click;
        }
    }

private:
    float oscillator (double ph) const noexcept
    {
        const float p = (float) ph;
        switch (settings.wave)
        {
            case Triangle:
                if (p < 0.25f) return 4.0f * p;
                if (p < 0.75f) return 2.0f - 4.0f * p;
                return 4.0f * p - 4.0f;

            case WarmSine:
            {
                static const float norm = 1.0f / std::tanh (2.0f);
                return std::tanh (2.0f * std::sin (juce::MathConstants<float>::twoPi * p)) * norm;
            }

            default:
                return std::sin (juce::MathConstants<float>::twoPi * p);
        }
    }

    double sampleRate = 44100.0;
    Settings settings;
    double baseFreq = 55.0, phase = 0.0;
    int currentNote = -1;

    float clickEnv = 0.0f, clickMul = 0.99f;
    float hpCoeff = 0.5f, hpState = 0.0f, hpPrevIn = 0.0f;
    juce::Random random;
};
