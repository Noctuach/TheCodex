#pragma once
#include "CurveEnvelope.h"
#include "Kick909.h"
#include "DrawEnvelope.h"
#include <juce_core/juce_core.h>
#include <cmath>

/**
    Single monophonic kick voice with two engines:

    - Synth: oscillator + ADSR pitch envelope (the "sweep") + ADSR amp envelope + noise click.
    - 909:   synthesized 909-style bass drum (shaped triangle, two-stage pitch drop,
             pulse + noise attack, exponential decay). The amp ADSR acts as a gate
             (attack/release) on top of the 909's own decay.
*/
class KickVoice
{
public:
    enum Wave { Sine = 0, Triangle, WarmSine };
    enum Engine { SynthEngine = 0, Engine909 };

    struct Settings
    {
        int   engine = SynthEngine;
        int   wave = Sine;
        float tuneSemis = 0.0f;
        float startPhaseDeg = 0.0f;
        float pitchAmountSemis = 48.0f;
        float clickLevel = 0.15f;
        float clickDecayMs = 4.0f;
        float clickToneHz = 3500.0f;

        int   model909 = 0;
        float tune909 = 0.5f, attack909 = 0.5f, decay909 = 0.5f;

        // Clean sub layer: a sine on the oscillator's own phase (so it follows the sweep and stays aligned),
        // faded in after the punch. Mixed in after the effects chain by the processor.
        float subFadeMs = 60.0f, subPhaseDeg = 0.0f;
    };

    /** Optional second output: the clean sub layer (same length as `out`), or nullptr. */
    float* subOut = nullptr;

    CurveEnvelope pitchEnv, ampEnv;
    DrawEnvelope pitchDraw, ampDraw;
    bool pitchDrawMode = false, ampDrawMode = false;   // draw mode: breakpoints instead of ADSR (synth engine)

    void prepare (double sr)
    {
        sampleRate = sr;
        msPerSample = (float) (1000.0 / sr);
        pitchEnv.setSampleRate (sr);
        ampEnv.setSampleRate (sr);
        pitchDraw.setSampleRate (sr);
        ampDraw.setSampleRate (sr);
        reset();
    }

    void reset()
    {
        pitchEnv.reset();
        ampEnv.reset();
        pitchDraw.reset();
        ampDraw.reset();
        clickEnv = pulseEnv = env909 = fast909 = slow909 = 0.0f;
        phase = 0.0;
        hpState = hpPrevIn = 0.0f;
        currentNote = -1;
    }

    void setSettings (const Settings& s)
    {
        settings = s;
        const auto& m = kick909Models()[(size_t) juce::jlimit (0, (int) kick909Models().size() - 1, s.model909)];
        const auto sr = (float) sampleRate;
        auto mulFor = [sr] (float ms) { return std::exp (-1.0f / (std::max (0.05f, ms) * 0.001f * sr)); };

        if (s.engine == Engine909)
        {
            const float decayScale = std::exp2 ((s.decay909 - 0.5f) * 4.0f);       // 0.25x .. 4x
            const float t60Samples = m.decayMs * decayScale * 0.001f * sr;
            ampMul909   = std::exp (-6.9078f / std::max (1.0f, t60Samples));
            fastMul909  = mulFor (m.fastMs);
            slowMul909  = mulFor (m.slowMs);
            sweepScale  = std::exp2 ((s.tune909 - 0.5f) * 2.0f);                   // 0.5x .. 2x
            const float attackScale = s.attack909 * 2.0f;                          // 0 .. 2x
            pulseLevel  = m.click * attackScale;
            noiseLevel  = m.noise * attackScale;
            clickMul    = mulFor (m.noiseMs);
            pulseMul    = mulFor (0.35f);
            setHighPass (m.noiseHz);
            model = m;
        }
        else
        {
            clickMul = mulFor (s.clickDecayMs);
            noiseLevel = s.clickLevel;
            setHighPass (s.clickToneHz);
        }
    }

    /** midiNote identifies the key for note-off; pitchNote (may be fractional) sets the root frequency. */
    void noteOn (int midiNote, float pitchNote)
    {
        currentNote = midiNote;
        baseFreq = 440.0 * std::pow (2.0, (pitchNote - 69.0) / 12.0);
        phase = settings.startPhaseDeg / 360.0;   // hard phase reset: identical punch on every hit
        pitchEnv.noteOn();
        ampEnv.noteOn();
        pitchDraw.noteOn();
        ampDraw.noteOn();
        clickEnv = 1.0f;
        pulseEnv = 1.0f;
        env909 = fast909 = slow909 = 1.0f;
        subTime = 0.0f;
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

    bool isActive() const noexcept
    {
        if (settings.engine == Engine909)
            return (ampEnv.isActive() && env909 > 1.0e-5f) || clickEnv > 1.0e-5f;
        if (ampDrawMode && ampDraw.isFinished() && ampDraw.lastLevel() < 1.0e-5f)
            return clickEnv > 1.0e-5f;   // the drawn amp envelope has ended at silence
        return ampEnv.isActive() || clickEnv > 1.0e-5f;
    }

    void render (float* out, int numSamples) noexcept
    {
        if (settings.engine == Engine909) render909 (out, numSamples);
        else                              renderSynth (out, numSamples);
    }

private:
    void setHighPass (float hz)
    {
        const float rc = 1.0f / (juce::MathConstants<float>::twoPi * hz);
        const float dt = 1.0f / (float) sampleRate;
        hpCoeff = rc / (rc + dt);
    }

    float nextNoise() noexcept
    {
        const float noise = random.nextFloat() * 2.0f - 1.0f;
        hpState = hpCoeff * (hpState + noise - hpPrevIn);
        hpPrevIn = noise;
        return hpState;
    }

    void advancePhase (double semis) noexcept
    {
        const double freq = std::min (baseFreq * std::exp2 (semis / 12.0), 0.45 * sampleRate);
        phase += freq / sampleRate;
        phase -= std::floor (phase);
    }

    void renderSynth (float* out, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (! isActive()) { out[i] = 0.0f; if (subOut != nullptr) subOut[i] = 0.0f; continue; }

            // Draw mode: breakpoints shape the hit; the ADSR still provides the release (as a gate) on note-off
            const float pEnv = pitchDrawMode ? pitchDraw.next() : pitchEnv.next();
            const float aEnv = ampDrawMode ? ampDraw.next() * ampEnv.next() : ampEnv.next();
            const float osc = oscillator (phase);
            if (subOut != nullptr) subOut[i] = subSample (aEnv);
            advancePhase (settings.tuneSemis + pEnv * settings.pitchAmountSemis);

            float click = 0.0f;
            if (clickEnv > 1.0e-5f)
            {
                click = nextNoise() * clickEnv * noiseLevel;
                clickEnv *= clickMul;
            }
            out[i] = osc * aEnv + click;
        }
    }

    void render909 (float* out, int numSamples) noexcept
    {
        const float driveNorm = model.drive > 1.0f ? 1.0f / std::tanh (model.drive) : 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            if (! isActive()) { out[i] = 0.0f; if (subOut != nullptr) subOut[i] = 0.0f; continue; }

            const float gate = ampEnv.next();
            // the distorted 909 tail is sustained by clipping long after env909 has decayed: the sub follows the note gate
            if (subOut != nullptr) subOut[i] = subSample (gate);

            // Shaped triangle: shape = 1 gives an exact sine, lower values keep the triangle's edge
            const float tri = triangle ((float) phase);
            const float shaped = std::sin (tri * juce::MathConstants<float>::halfPi);
            float body = tri + model.shape * (shaped - tri);

            // The sweeps land exactly on the root (instead of approaching it forever), so the
            // tail is in key: each envelope is offset by a small floor and clamped at zero.
            constexpr float floorLevel = 0.02f, floorNorm = 1.0f / (1.0f - floorLevel);
            const float fastEnv = std::max (0.0f, (fast909 - floorLevel) * floorNorm);
            const float slowEnv = std::max (0.0f, (slow909 - floorLevel) * floorNorm);
            advancePhase (settings.tuneSemis + sweepScale * (model.sweepSemis * fastEnv + model.slowSemis * slowEnv));
            fast909 *= fastMul909;
            slow909 *= slowMul909;

            body *= env909;
            env909 *= ampMul909;

            if (model.drive > 1.0f)
                body = std::tanh (body * model.drive) * driveNorm;

            float attack = 0.0f;
            if (clickEnv > 1.0e-5f)
            {
                attack = pulseEnv * pulseLevel + nextNoise() * clickEnv * noiseLevel;
                pulseEnv *= pulseMul;
                clickEnv *= clickMul;
            }

            out[i] = (body + attack) * gate;
        }
    }

    /** Clean sine on the oscillator's phase, faded in (raised cosine) over subFadeMs. */
    float subSample (float level) noexcept
    {
        const float fadeMs = std::max (0.1f, settings.subFadeMs);
        const float u = std::min (1.0f, subTime / fadeMs);
        subTime += msPerSample;
        const float fade = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * u);
        return std::sin (juce::MathConstants<float>::twoPi * (float) phase + settings.subPhaseDeg * (juce::MathConstants<float>::pi / 180.0f))
               * level * fade;
    }

    static float triangle (float p) noexcept
    {
        if (p < 0.25f) return 4.0f * p;
        if (p < 0.75f) return 2.0f - 4.0f * p;
        return 4.0f * p - 4.0f;
    }

    float oscillator (double ph) const noexcept
    {
        const float p = (float) ph;
        switch (settings.wave)
        {
            case Triangle: return triangle (p);
            case WarmSine:
            {
                static const float norm = 1.0f / std::tanh (2.0f);
                return std::tanh (2.0f * std::sin (juce::MathConstants<float>::twoPi * p)) * norm;
            }
            default: return std::sin (juce::MathConstants<float>::twoPi * p);
        }
    }

    double sampleRate = 44100.0;
    Settings settings;
    Kick909Model model = kick909Models()[0];
    double baseFreq = 55.0, phase = 0.0;
    int currentNote = -1;
    float subTime = 0.0f, msPerSample = 1000.0f / 44100.0f;

    float clickEnv = 0.0f, clickMul = 0.99f, noiseLevel = 0.0f;
    float pulseEnv = 0.0f, pulseMul = 0.9f, pulseLevel = 0.0f;
    float env909 = 0.0f, ampMul909 = 0.999f;
    float fast909 = 0.0f, fastMul909 = 0.99f, slow909 = 0.0f, slowMul909 = 0.99f, sweepScale = 1.0f;
    float hpCoeff = 0.5f, hpState = 0.0f, hpPrevIn = 0.0f;
    juce::Random random;
};
