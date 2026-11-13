#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#if defined (__SSE2__) || defined (_M_X64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <xmmintrin.h>
 #define CODEX_OS_SSE 1
#else
 #define CODEX_OS_SSE 0
#endif

/**
    Stereo clone of juce::dsp::Oversampling<float> with filterHalfBandPolyphaseIIR, maximum quality and no integer
    latency compensation (JUCE 8.0.4), for factors 2, 4, 8 and 16.

    The stages are designed exactly as JUCE's constructor designs them (same transition widths and attenuations, same
    FilterDesign call, same coefficient order) and run the same per-sample all-pass recursions in the same order
    (output = alpha * input + state; state = input - alpha * output, in float), including the snap-to-zero of every
    state after each up and down pass and the delayed-path handling of the down-sampler. The only difference is that
    the left and right channels run in two lanes of one SSE register (separate multiplies and adds, never FMA), so the
    output is bit-identical to JUCE's. The reported latency is read from a juce::dsp::Oversampling built in the
    constructor. CodexRender checks the equivalence (all factors, random block sizes, resets).
    All buffers are allocated in initProcessing().
*/
class StereoOversampling
{
public:
    /** numStages: 1 = 2x ... 4 = 16x. */
    explicit StereoOversampling (int numStagesToUse)
    {
        jassert (numStagesToUse >= 1 && numStagesToUse <= 4);
        latency = juce::dsp::Oversampling<float> (2, (size_t) numStagesToUse, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false)
                      .getLatencyInSamples();
        for (int n = 0; n < numStagesToUse; ++n)
        {
            // same values and expressions as juce::dsp::Oversampling's constructor (isMaximumQuality = true)
            const auto twUp   = 0.10f * (n == 0 ? 0.5f : 1.0f);
            const auto twDown = 0.12f * (n == 0 ? 0.5f : 1.0f);
            const auto gaindBStartUp = -90.0f, gaindBStartDown = -75.0f, gaindBFactorUp = 10.0f, gaindBFactorDown = 10.0f;
            stages.push_back (Stage (twUp, gaindBStartUp + gaindBFactorUp * (float) n, twDown, gaindBStartDown + gaindBFactorDown * (float) n));
        }
    }

    void initProcessing (size_t maxSamplesBeforeOversampling)
    {
        size_t numSamples = maxSamplesBeforeOversampling;
        for (auto& s : stages)
        {
            numSamples *= 2;
            for (auto& b : s.buffer) b.assign (numSamples, 0.0f);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stages) s.reset();
    }

    float getLatencyInSamples() const noexcept { return latency; }
    size_t getOversamplingFactor() const noexcept { return (size_t) 1 << stages.size(); }

    juce::dsp::AudioBlock<float> processSamplesUp (const juce::dsp::AudioBlock<const float>& input) noexcept
    {
        jassert (input.getNumChannels() == 2);
        const float* inL = input.getChannelPointer (0);
        const float* inR = input.getChannelPointer (1);
        size_t n = input.getNumSamples();
        for (auto& s : stages)
        {
            jassert (n * 2 <= s.buffer[0].size());
            s.up (inL, inR, n);
            inL = s.buffer[0].data();
            inR = s.buffer[1].data();
            n *= 2;
        }
        auto& last = stages.back();
        upChannels[0] = last.buffer[0].data();   // a member: the block keeps a pointer to this array
        upChannels[1] = last.buffer[1].data();
        return juce::dsp::AudioBlock<float> (upChannels.data(), 2, n);
    }

    void processSamplesDown (juce::dsp::AudioBlock<float>& output) noexcept
    {
        jassert (output.getNumChannels() == 2);
        size_t n = output.getNumSamples();
        for (size_t k = 0; k + 1 < stages.size(); ++k) n *= 2;   // samples at the input of the last stage's down pass / 2
        for (size_t k = stages.size() - 1; k > 0; --k)
        {
            auto& prev = stages[k - 1];
            stages[k].down (prev.buffer[0].data(), prev.buffer[1].data(), n);
            n /= 2;
        }
        stages[0].down (output.getChannelPointer (0), output.getChannelPointer (1), n);
    }

private:
    struct Stage
    {
        std::vector<float> coeffsUp, coeffsDown;   // direct path, then delayed path (without its first, pure delay, section)
        std::vector<float> v1Up[2], v1Down[2];
        float delayDown[2] { 0.0f, 0.0f };
        std::array<std::vector<float>, 2> buffer;  // this stage's oversampled signal (2 * input length)

        Stage (float twUp, float dbUp, float twDown, float dbDown)
        {
            using FD = juce::dsp::FilterDesign<float>;
            auto structureUp = FD::designIIRLowpassHalfBandPolyphaseAllpassMethod (twUp, dbUp);
            auto structureDown = FD::designIIRLowpassHalfBandPolyphaseAllpassMethod (twDown, dbDown);
            for (int i = 0; i < structureUp.directPath.size(); ++i)    coeffsUp.push_back (structureUp.directPath.getObjectPointer (i)->coefficients[0]);
            for (int i = 1; i < structureUp.delayedPath.size(); ++i)   coeffsUp.push_back (structureUp.delayedPath.getObjectPointer (i)->coefficients[0]);
            for (int i = 0; i < structureDown.directPath.size(); ++i)  coeffsDown.push_back (structureDown.directPath.getObjectPointer (i)->coefficients[0]);
            for (int i = 1; i < structureDown.delayedPath.size(); ++i) coeffsDown.push_back (structureDown.delayedPath.getObjectPointer (i)->coefficients[0]);
            for (int c = 0; c < 2; ++c) { v1Up[c].assign (coeffsUp.size(), 0.0f); v1Down[c].assign (coeffsDown.size(), 0.0f); }
        }

        void reset() noexcept
        {
            for (int c = 0; c < 2; ++c)
            {
                std::fill (v1Up[c].begin(), v1Up[c].end(), 0.0f);
                std::fill (v1Down[c].begin(), v1Down[c].end(), 0.0f);
                delayDown[c] = 0.0f;
            }
            for (auto& b : buffer) std::fill (b.begin(), b.end(), 0.0f);
        }

        /** in (n samples per channel) -> buffer (2n): even samples from the direct path, odd from the delayed path. */
        void up (const float* inL, const float* inR, size_t n) noexcept
        {
            const int numStages = (int) coeffsUp.size();
            const int delayedStages = numStages / 2, directStages = numStages - delayedStages;
            const float* coeffs = coeffsUp.data();
            float* outL = buffer[0].data();
            float* outR = buffer[1].data();
           #if CODEX_OS_SSE
            __m128 v[maxCoeffs];
            jassert (numStages <= maxCoeffs);
            for (int k = 0; k < numStages; ++k) v[k] = _mm_setr_ps (v1Up[0][(size_t) k], v1Up[1][(size_t) k], 0.0f, 0.0f);
            for (size_t i = 0; i < n; ++i)
            {
                const __m128 x = _mm_unpacklo_ps (_mm_load_ss (inL + i), _mm_load_ss (inR + i));
                __m128 input = x;
                for (int k = 0; k < directStages; ++k)
                {
                    const __m128 alpha = _mm_set1_ps (coeffs[k]);
                    const __m128 output = _mm_add_ps (_mm_mul_ps (alpha, input), v[k]);
                    v[k] = _mm_sub_ps (input, _mm_mul_ps (alpha, output));
                    input = output;
                }
                _mm_store_ss (outL + (i << 1), input);
                _mm_store_ss (outR + (i << 1), _mm_shuffle_ps (input, input, _MM_SHUFFLE (1, 1, 1, 1)));
                input = x;
                for (int k = directStages; k < numStages; ++k)
                {
                    const __m128 alpha = _mm_set1_ps (coeffs[k]);
                    const __m128 output = _mm_add_ps (_mm_mul_ps (alpha, input), v[k]);
                    v[k] = _mm_sub_ps (input, _mm_mul_ps (alpha, output));
                    input = output;
                }
                _mm_store_ss (outL + (i << 1) + 1, input);
                _mm_store_ss (outR + (i << 1) + 1, _mm_shuffle_ps (input, input, _MM_SHUFFLE (1, 1, 1, 1)));
            }
            storeStates (v, v1Up, numStages);
           #else
            const float* ins[] = { inL, inR };
            float* outs[] = { outL, outR };
            for (int c = 0; c < 2; ++c)
            {
                float* lv1 = v1Up[c].data();
                for (size_t i = 0; i < n; ++i)
                {
                    float input = ins[c][i];
                    for (int k = 0; k < directStages; ++k)
                    {
                        const float alpha = coeffs[k];
                        const float output = alpha * input + lv1[k];
                        lv1[k] = input - alpha * output;
                        input = output;
                    }
                    outs[c][i << 1] = input;
                    input = ins[c][i];
                    for (int k = directStages; k < numStages; ++k)
                    {
                        const float alpha = coeffs[k];
                        const float output = alpha * input + lv1[k];
                        lv1[k] = input - alpha * output;
                        input = output;
                    }
                    outs[c][(i << 1) + 1] = input;
                }
            }
           #endif
            snap (v1Up);
        }

        /** buffer (2n) -> out (n samples per channel). */
        void down (float* outL, float* outR, size_t n) noexcept
        {
            const int numStages = (int) coeffsDown.size();
            const int delayedStages = numStages / 2, directStages = numStages - delayedStages;
            const float* coeffs = coeffsDown.data();
            const float* inL = buffer[0].data();
            const float* inR = buffer[1].data();
           #if CODEX_OS_SSE
            __m128 v[maxCoeffs];
            jassert (numStages <= maxCoeffs);
            for (int k = 0; k < numStages; ++k) v[k] = _mm_setr_ps (v1Down[0][(size_t) k], v1Down[1][(size_t) k], 0.0f, 0.0f);
            __m128 delay = _mm_setr_ps (delayDown[0], delayDown[1], 0.0f, 0.0f);
            const __m128 half = _mm_set1_ps (0.5f);
            for (size_t i = 0; i < n; ++i)
            {
                __m128 input = _mm_unpacklo_ps (_mm_load_ss (inL + (i << 1)), _mm_load_ss (inR + (i << 1)));
                for (int k = 0; k < directStages; ++k)
                {
                    const __m128 alpha = _mm_set1_ps (coeffs[k]);
                    const __m128 output = _mm_add_ps (_mm_mul_ps (alpha, input), v[k]);
                    v[k] = _mm_sub_ps (input, _mm_mul_ps (alpha, output));
                    input = output;
                }
                const __m128 directOut = input;
                input = _mm_unpacklo_ps (_mm_load_ss (inL + (i << 1) + 1), _mm_load_ss (inR + (i << 1) + 1));
                for (int k = directStages; k < numStages; ++k)
                {
                    const __m128 alpha = _mm_set1_ps (coeffs[k]);
                    const __m128 output = _mm_add_ps (_mm_mul_ps (alpha, input), v[k]);
                    v[k] = _mm_sub_ps (input, _mm_mul_ps (alpha, output));
                    input = output;
                }
                const __m128 y = _mm_mul_ps (_mm_add_ps (delay, directOut), half);
                _mm_store_ss (outL + i, y);
                _mm_store_ss (outR + i, _mm_shuffle_ps (y, y, _MM_SHUFFLE (1, 1, 1, 1)));
                delay = input;
            }
            storeStates (v, v1Down, numStages);
            _mm_store_ss (&delayDown[0], delay);
            _mm_store_ss (&delayDown[1], _mm_shuffle_ps (delay, delay, _MM_SHUFFLE (1, 1, 1, 1)));
           #else
            const float* ins[] = { inL, inR };
            float* outs[] = { outL, outR };
            for (int c = 0; c < 2; ++c)
            {
                float* lv1 = v1Down[c].data();
                float delay = delayDown[c];
                for (size_t i = 0; i < n; ++i)
                {
                    float input = ins[c][i << 1];
                    for (int k = 0; k < directStages; ++k)
                    {
                        const float alpha = coeffs[k];
                        const float output = alpha * input + lv1[k];
                        lv1[k] = input - alpha * output;
                        input = output;
                    }
                    const float directOut = input;
                    input = ins[c][(i << 1) + 1];
                    for (int k = directStages; k < numStages; ++k)
                    {
                        const float alpha = coeffs[k];
                        const float output = alpha * input + lv1[k];
                        lv1[k] = input - alpha * output;
                        input = output;
                    }
                    outs[c][i] = (delay + directOut) * 0.5f;
                    delay = input;
                }
                delayDown[c] = delay;
            }
           #endif
            snap (v1Down);
        }

        static constexpr int maxCoeffs = 32;

       #if CODEX_OS_SSE
        static void storeStates (const __m128* v, std::vector<float>* states, int numStages) noexcept
        {
            for (int k = 0; k < numStages; ++k)
            {
                alignas (16) float lanes[4];
                _mm_store_ps (lanes, v[k]);
                states[0][(size_t) k] = lanes[0];
                states[1][(size_t) k] = lanes[1];
            }
        }
       #endif

        static void snap (std::vector<float>* states) noexcept
        {
            for (int c = 0; c < 2; ++c)
                for (auto& s : states[c])
                    juce::dsp::util::snapToZero (s);
        }
    };

    std::vector<Stage> stages;
    std::array<float*, 2> upChannels { nullptr, nullptr };
    float latency = 0.0f;
};
