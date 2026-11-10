#pragma once
#include <cmath>
#include <algorithm>

/**
    ADSR envelope with an adjustable curve for decay and release.
    curve = 1 is linear; larger values give a sharper, more exponential "snap"
    (the classic fast pitch drop of a hardstyle kick).
*/
class CurveEnvelope
{
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setSampleRate (double sr) { sampleRate = sr; updateIncrements(); }

    void setParameters (float attackMs, float decayMs, float sustainLevel, float releaseMs, float curveAmount)
    {
        aMs = attackMs; dMs = decayMs; rMs = releaseMs;
        sustain = std::clamp (sustainLevel, 0.0f, 1.0f);
        curve = std::max (1.0f, curveAmount);
        updateIncrements();
    }

    void noteOn()
    {
        startLevel = level; // retrigger from the current level to avoid a jump in amplitude
        t = 0.0f;
        stage = Stage::Attack;
    }

    void noteOff()
    {
        if (stage == Stage::Idle || stage == Stage::Release)
            return;
        startLevel = level;
        t = 0.0f;
        stage = Stage::Release;
    }

    void reset() { stage = Stage::Idle; level = 0.0f; t = 0.0f; }

    bool isActive() const noexcept { return stage != Stage::Idle; }
    Stage getStage() const noexcept { return stage; }

    float next() noexcept
    {
        switch (stage)
        {
            case Stage::Idle:
                level = 0.0f;
                break;

            case Stage::Attack:
                t += attackInc;
                if (t >= 1.0f) { level = 1.0f; t = 0.0f; stage = Stage::Decay; }
                else           level = startLevel + (1.0f - startLevel) * t;
                break;

            case Stage::Decay:
                t += decayInc;
                if (t >= 1.0f) { level = sustain; t = 0.0f; stage = Stage::Sustain; }
                else           level = sustain + (1.0f - sustain) * shape (1.0f - t);
                break;

            case Stage::Sustain:
                level = sustain;
                break;

            case Stage::Release:
                t += releaseInc;
                if (t >= 1.0f) { level = 0.0f; t = 0.0f; stage = Stage::Idle; }
                else           level = startLevel * shape (1.0f - t);
                break;
        }
        return level;
    }

    /** Same curve maths, exposed so the GUI can draw the exact shape. */
    static float shapeFor (float x, float curve) noexcept { return std::pow (x, curve); }

private:
    float shape (float x) const noexcept { return shapeFor (x, curve); }

    float incFor (float ms) const noexcept
    {
        const float samples = (float) (ms * 0.001 * sampleRate);
        return samples <= 1.0f ? 1.0f : 1.0f / samples;
    }

    void updateIncrements()
    {
        attackInc  = incFor (aMs);
        decayInc   = incFor (dMs);
        releaseInc = incFor (rMs);
    }

    double sampleRate = 44100.0;
    float aMs = 1.0f, dMs = 100.0f, rMs = 100.0f, sustain = 0.0f, curve = 1.0f;
    float attackInc = 1.0f, decayInc = 1.0f, releaseInc = 1.0f;

    Stage stage = Stage::Idle;
    float level = 0.0f, startLevel = 0.0f, t = 0.0f;
};
