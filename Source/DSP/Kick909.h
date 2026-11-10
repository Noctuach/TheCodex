#pragma once
#include <array>

/**
    Parameter sets for the synthesized 909-style bass drum.

    The 909 BD is built from a waveshaped triangle oscillator whose pitch falls quickly
    (the "Tune" control sets how far), an attack click made from a short pulse plus a
    noise burst ("Attack"), and an exponential amplitude decay ("Decay").
    Each model below is a starting point that hardstyle / hardcore producers commonly
    pushed the machine (or its samples) towards before distorting it.
*/
struct Kick909Model
{
    const char* name;
    float sweepSemis;  // fast pitch drop: semitones above the root at the hit
    float fastMs;      // time constant of the fast drop
    float slowSemis;   // slower "settling" part of the sweep
    float slowMs;      // time constant of the slow part
    float decayMs;     // amplitude T60 at Decay = 50 %
    float click;       // attack pulse level
    float noise;       // attack noise level
    float noiseMs;     // noise burst decay
    float noiseHz;     // noise high-pass
    float shape;       // 0 = raw triangle, 1 = fully shaped (sine)
    float drive;       // circuit saturation (1 = clean)
};

inline const std::array<Kick909Model, 10>& kick909Models()
{
    static const std::array<Kick909Model, 10> models { {
        //  name                    sweep  fast  slow  slowMs decay  click noise nMs  nHz     shape drive
        { "Classic",                24.0f, 6.0f,  7.0f, 45.0f,  450.0f, 0.50f, 0.25f, 3.0f, 6000.0f, 0.85f, 1.0f },
        { "Long Tail",              22.0f, 5.0f,  6.0f, 55.0f, 1400.0f, 0.40f, 0.20f, 3.0f, 6000.0f, 0.95f, 1.0f },
        { "Punch",                  30.0f, 4.0f, 10.0f, 30.0f,  260.0f, 0.80f, 0.35f, 2.0f, 8000.0f, 0.80f, 1.5f },
        { "Deep",                   16.0f, 8.0f,  5.0f, 60.0f,  900.0f, 0.20f, 0.10f, 3.0f, 5000.0f, 1.00f, 1.0f },
        { "Hard Attack",            28.0f, 3.0f,  8.0f, 40.0f,  500.0f, 1.00f, 0.50f, 4.0f, 5000.0f, 0.80f, 1.2f },
        { "Gabber",                 36.0f, 5.0f, 12.0f, 35.0f,  350.0f, 0.70f, 0.30f, 3.0f, 7000.0f, 0.70f, 3.0f },
        { "Early Hardstyle",        26.0f, 6.0f,  9.0f, 50.0f,  700.0f, 0.60f, 0.30f, 3.0f, 6000.0f, 0.85f, 1.8f },
        { "Boom",                   20.0f, 12.0f,  8.0f, 45.0f, 1100.0f, 0.30f, 0.15f, 3.0f, 5000.0f, 0.95f, 1.0f },
        { "Driven",                 26.0f, 5.0f,  8.0f, 45.0f,  600.0f, 0.60f, 0.30f, 3.0f, 6000.0f, 0.75f, 4.0f },
        { "Short Tok",              34.0f, 2.5f, 14.0f, 18.0f,  180.0f, 0.90f, 0.45f, 2.0f, 9000.0f, 0.80f, 1.3f },
    } };
    return models;
}
