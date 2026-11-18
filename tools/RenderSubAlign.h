// Latency and sub-layer alignment checks for CodexRender (included by Render.cpp after its helpers):
//  - the host latency is the rounded sum of every stage's fractional latency (not a sum of rounded stages), and the
//    processed signal really arrives at the reported latency (within the rounding residual)
//  - the sub layer carries the chain's fractional latency, the crossovers' all-pass phase (multiband saturator and
//    compressor, the saturator's pre-warped for its oversampled design) and the saturator's and distortions' DC
//    filters: it stays in phase with the processed kick at 30-120 Hz; with a whole-sample latency and neither module
//    on it is exactly the old integer delay; its phase-match sections switch and glide without clicks or level dips,
//    and are already in place right after prepareToPlay
#pragma once
#include <complex>
#include <memory>
#include <cstring>
#include <functional>
#include "../Source/DSP/SubPhaseMatch.h"

namespace subalign_test
{
    using cd = std::complex<double>;

    /** The voice as a steady sine of hz: no sweep, click or attack, full sustain, sub layer flat (0 dB, no fade). */
    inline void steadySine (CodexProcessor& p, double hz)
    {
        setParam (p, ID::engine, 0); setParam (p, ID::wave, 0); setParam (p, ID::pAmount, 0); setParam (p, ID::clickLevel, 0);
        setParam (p, ID::aAttack, 0); setParam (p, ID::aSustain, 1); setParam (p, ID::aRelease, 1); setParam (p, ID::velocity, 0);
        setParam (p, ID::phase, 0); setParam (p, ID::keyMode, 0); setParam (p, ID::key, 0); setParam (p, ID::octave, 1);
        // key F, octave 0 = MIDI note 29; Tune moves it to hz
        setParam (p, ID::tune, (float) (12.0 * std::log2 (hz / 440.0) + 69.0 - Params::keyBaseNote));
        setParam (p, ID::subLevel, 0.0f); setParam (p, ID::subLowPass, 300.0f); setParam (p, ID::subFade, 0.0f); setParam (p, ID::subPhase, 0.0f);
    }

    /** Every module off and transparent: distortions off at OS Off, saturator / compressor / reverb off, EQ bands off. */
    inline void allOff (CodexProcessor& p)
    {
        setParam (p, ID::outGain, 0); setParam (p, ID::clipOn, 0);
        for (int st = 1; st <= 2; ++st) { setParam (p, ID::clip (st, "On"), 0); setParam (p, ID::clip (st, "OS"), 0); }
        setParam (p, ID::sat ("On"), 0); setParam (p, ID::cmp ("On"), 0); setParam (p, ID::spc ("On"), 0);
        for (int e = 0; e < 2; ++e) { setParam (p, ID::eqMode (e), 0); for (int b = 0; b < Params::numBands; ++b) setParam (p, ID::eqBand (e, b, "On"), 0); }
    }

    /** prepareToPlay, note-on at sample 0, len samples (512-sample blocks); channel 0. */
    inline std::vector<float> renderL (CodexProcessor& p, int len)
    {
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
        std::vector<float> out ((size_t) len);
        juce::AudioBuffer<float> buf (2, block);
        for (int pos = 0; pos < len; pos += block)
        {
            const int n = std::min (block, len - pos);
            buf.setSize (2, n, false, false, true);
            buf.clear();
            juce::MidiBuffer midi;
            if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            p.processBlock (buf, midi);
            std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, out.begin() + pos);
        }
        return out;
    }

    /** Complex amplitude at hz over [start, end), Hann window. */
    inline cd tone (const std::vector<float>& x, double hz, int start, int end)
    {
        cd acc;
        const double w = juce::MathConstants<double>::twoPi * hz / sr;
        for (int i = start; i < end; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (i - start) / (end - start));
            acc += win * (double) x[(size_t) i] * std::polar (1.0, -w * i);
        }
        return acc;
    }

    inline double wrapDeg (double d) { while (d > 180.0) d -= 360.0; while (d <= -180.0) d += 360.0; return d; }

    /** Phase (degrees) of the sub layer against the processed kick at hz: renders sub off / on, sub = difference. */
    inline double subVsChainDeg (const std::function<void (CodexProcessor&)>& configure, double hz)
    {
        auto p = std::make_unique<CodexProcessor>();
        configure (*p);
        steadySine (*p, hz);
        const int len = (int) (3.0 * sr);
        setParam (*p, ID::subOn, 0);
        const auto a = renderL (*p, len);
        setParam (*p, ID::subOn, 1);
        const auto b = renderL (*p, len);
        std::vector<float> s ((size_t) len);
        for (int i = 0; i < len; ++i) s[(size_t) i] = b[(size_t) i] - a[(size_t) i];
        const int from = (int) (1.0 * sr);
        return std::arg (tone (s, hz, from, len) / tone (a, hz, from, len)) * 180.0 / juce::MathConstants<double>::pi;
    }
}

static int latencyAndSubAlignTests()
{
    using namespace subalign_test;
    int bad = 0;
    auto check = [&] (bool ok, const juce::String& what) { if (! ok) { ++bad; std::cout << "   FAILED: " << what << "\n"; } };

    // ---- 1. Reported latency = round (sum of the stages' fractional latencies), for every oversampling combination ----
    {
        std::cout << "\nLatency: reported = round (sum of fractional stage latencies), D1 x D2 x Saturation x Clipper x EQ 1 / EQ 2 modes\n";
        auto p = std::make_unique<CodexProcessor>();
        allOff (*p);
        p->setRateAndBufferSizeDetails (sr, block);
        p->prepareToPlay (sr, block);
        juce::dsp::Oversampling<float> eqOs (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        eqOs.initProcessing ((size_t) block);   // integer-latency compensation: a whole number of samples
        const double eqNatural = eqOs.getLatencyInSamples();
        auto osLat = [] (int factorIndex) { return factorIndex > 0 ? (double) StereoOversampling (factorIndex).getLatencyInSamples() : 0.0; };
        juce::AudioBuffer<float> buf (2, 1);
        juce::MidiBuffer none;
        int combos = 0, wrong = 0, offByOne = 0;
        for (int d1 = 0; d1 <= 4; ++d1)
            for (int d2 = 0; d2 <= 4; ++d2)
                for (int so = -1; so <= 3; ++so)           // -1 = saturation off, else its OS index (Off, 2x, 4x, 8x)
                    for (int cl = 0; cl <= 1; ++cl)
                        for (int eqm = 0; eqm <= 3; ++eqm)  // EQ 1 / EQ 2 in Zero Latency or Natural Phase
                        {
                            setParam (*p, ID::clip (1, "OS"), (float) d1);
                            setParam (*p, ID::clip (2, "OS"), (float) d2);
                            setParam (*p, ID::sat ("On"), so >= 0 ? 1.0f : 0.0f);
                            setParam (*p, ID::sat ("OS"), (float) juce::jmax (0, so));
                            setParam (*p, ID::clipOn, (float) cl);
                            setParam (*p, ID::eqMode (0), (float) (eqm & 1));
                            setParam (*p, ID::eqMode (1), (float) (eqm >> 1));
                            buf.clear();
                            p->processBlock (buf, none);   // applies the parameters (and the latency, on this thread)
                            const double sum = osLat (d1) + osLat (d2) + (so > 0 ? osLat (so) : 0.0) + (cl ? osLat (2) : 0.0)
                                               + ((eqm & 1) ? eqNatural : 0.0) + ((eqm >> 1) ? eqNatural : 0.0);
                            const int expected = juce::roundToInt (sum), got = p->getLatencySamples();
                            ++combos;
                            if (got != expected)
                            {
                                if (wrong++ < 4)
                                    std::cout << "   D1 OS " << d1 << ", D2 OS " << d2 << ", Sat OS " << so << ", clipper " << cl << ", EQ modes " << (eqm & 1) << "/" << (eqm >> 1)
                                              << ": sum " << juce::String (sum, 3) << " -> expected " << expected << ", reported " << got << "\n";
                                if (std::abs (got - expected) == 1) ++offByOne;
                            }
                        }
        std::cout << "   " << combos << " combinations, " << wrong << " reported wrong (" << offByOne << " by one sample)\n";
        check (wrong == 0, "reported latency is round (sum of fractional latencies) in every combination");
    }

    // ---- 2. The processed signal arrives at the reported latency (residual <= 0.5 samples) ----
    {
        std::cout << "\nLatency: measured delay of the processed signal (phase delay of a 52 Hz sine) vs reported\n";
        struct Case { const char* name; int d1, d2, satOs; bool clip; int eqMode1; };
        const Case cases[] = {
            { "D1 16x + D2 8x + clipper",            4, 3, -1, true,  0 },
            { "D1 16x + D2 16x + clipper",           4, 4, -1, true,  0 },
            { "D1 4x + D2 2x + clipper",             2, 1, -1, true,  0 },
            { "D1 8x + Sat 8x + clipper",            3, 0,  3, true,  0 },
            { "D1 16x + D2 4x + Sat 2x + EQ Natural", 4, 2,  1, false, 1 },
            { "clipper only",                        0, 0, -1, true,  0 },
        };
        const double hz = 52.0, w = juce::MathConstants<double>::twoPi * hz / sr;
        auto ref = std::make_unique<CodexProcessor>();
        allOff (*ref);
        steadySine (*ref, hz);
        setParam (*ref, ID::outGain, -12.0f);
        const int len = (int) (2.5 * sr), from = (int) (1.0 * sr);
        const cd x = tone (renderL (*ref, len), hz, from, len);
        for (const auto& c : cases)
        {
            auto p = std::make_unique<CodexProcessor>();
            allOff (*p);
            steadySine (*p, hz);
            setParam (*p, ID::outGain, -12.0f);   // under the clipper's ceiling: the clipper stays linear
            setParam (*p, ID::clip (1, "OS"), (float) c.d1);
            setParam (*p, ID::clip (2, "OS"), (float) c.d2);
            if (c.satOs >= 0)
            {
                // one band, bypassed: only the oversampler and the 10 Hz DC filter (divided out below)
                setParam (*p, ID::sat ("On"), 1); setParam (*p, ID::sat ("Bands"), 1); setParam (*p, ID::sat ("OS"), (float) c.satOs);
                setParam (*p, ID::satBand (0, "Bypass"), 1);
            }
            setParam (*p, ID::clipOn, c.clip ? 1.0f : 0.0f);
            setParam (*p, ID::eqMode (0), (float) c.eqMode1);
            cd h = tone (renderL (*p, len), hz, from, len) / x;
            if (c.satOs >= 0)
            {
                const double R = (double) (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sr);
                const cd z1 = std::polar (1.0, -w);
                h /= (1.0 - z1) / (1.0 - R * z1);
            }
            const int reported = p->getLatencySamples();
            const double measured = reported - std::arg (h * std::polar (1.0, w * reported)) / w;
            const double residual = measured - reported;
            std::cout << "   " << juce::String (c.name).paddedRight (' ', 40) << "measured " << juce::String (measured, 3) << ", reported " << reported
                      << ", residual " << juce::String (residual, 3) << "\n";
            check (std::abs (residual) <= 0.5 + 1.0e-3, juce::String (c.name) + ": delay within half a sample of the reported latency");
        }
    }

    // ---- 3. Sub layer in phase with the processed kick (40-120 Hz) ----
    {
        std::cout << "\nSub layer: phase against the processed kick, relative to the all-off chain (degrees)\n";
        const double freqs[] = { 40.0, 52.0, 80.0, 120.0 };
        struct Cfg { const char* name; std::function<void (CodexProcessor&)> set; double limit; };   // degrees
        auto satAndComp = [] (CodexProcessor& p)
        {
            // Saturation at its defaults (3 bands, 24 dB/oct, 2x) with the bands bypassed, Compressor 3 bands at ratio 1:
            // the chain is linear, so the sub must follow its crossover phase exactly
            setParam (p, ID::sat ("On"), 1);
            for (int b = 0; b < sat::maxBands; ++b) setParam (p, ID::satBand (b, "Bypass"), 1);
            setParam (p, ID::cmp ("On"), 1); setParam (p, ID::cmp ("Bands"), 3); setParam (p, ID::cmp ("Auto"), 0);
            for (int b = 0; b < 3; ++b) setParam (p, ID::cmpBand (b, "Ratio"), 1.0f);
        };
        const Cfg cfgs[] = {
            { "Sat 3 bands + Comp 3 bands (D1 / D2 off at 4x)", [satAndComp] (CodexProcessor& p)
              { allOff (p); for (int st = 1; st <= 2; ++st) setParam (p, ID::clip (st, "OS"), 2); satAndComp (p); }, 2.0 },
            { "Sat 4 bands LR8 (X1 45 Hz, 4x) + Comp 3 bands, D1 16x", [satAndComp] (CodexProcessor& p)
              {
                  allOff (p); satAndComp (p);
                  setParam (p, ID::clip (1, "OS"), 4);
                  setParam (p, ID::sat ("Bands"), 4); setParam (p, ID::sat ("Slope"), 3); setParam (p, ID::sat ("OS"), 2); setParam (p, ID::sat ("X1"), 45.0f);
              }, 2.0 },
            { "Sat 3 bands LR2 (X1 70 Hz, 8x), D1 2x + D2 4x off", [] (CodexProcessor& p)
              {
                  allOff (p);
                  for (int st = 1; st <= 2; ++st) setParam (p, ID::clip (st, "OS"), (float) st);
                  setParam (p, ID::sat ("OS"), 3);
                  setParam (p, ID::sat ("On"), 1); setParam (p, ID::sat ("Slope"), 1); setParam (p, ID::sat ("X1"), 70.0f);
                  for (int b = 0; b < sat::maxBands; ++b) setParam (p, ID::satBand (b, "Bypass"), 1);
              }, 2.0 },
            // the saturator designs its crossovers at the oversampled rate: the sub's base-rate sections are pre-warped
            // (high crossovers at 8x were up to 5 degrees off at 120 Hz without it)
            { "Sat 6 bands LR8 8x, X1-X5 at 16 kHz", [] (CodexProcessor& p)
              {
                  allOff (p);
                  setParam (p, ID::sat ("On"), 1); setParam (p, ID::sat ("Bands"), 6); setParam (p, ID::sat ("Slope"), 3); setParam (p, ID::sat ("OS"), 3);
                  for (int j = 1; j <= 5; ++j) setParam (p, ID::sat (("X" + juce::String (j)).toRawUTF8()), 16000.0f);
                  for (int b = 0; b < sat::maxBands; ++b) setParam (p, ID::satBand (b, "Bypass"), 1);
              }, 2.0 },
            // a driven distortion's DC filter is its last stage: its phase reaches the output, and the sub follows it
            { "factory defaults, D2 off, + Saturation (bands bypassed) + Compressor 3 bands", [satAndComp] (CodexProcessor& p)
              { setParam (p, ID::clip (2, "On"), 0); satAndComp (p); }, 2.0 },
            { "factory defaults, D1 off, + Saturation (bands bypassed) + Compressor 3 bands", [satAndComp] (CodexProcessor& p)
              { setParam (p, ID::clip (1, "On"), 0); satAndComp (p); }, 2.0 },
            // the whole default chain: D1 (Tanh +18 dB) and its DC filter feed D2 (Hard Clip) and the driven saturator
            // bands, which square the DC filter's tilt partly away again. That part is not linear (it depends on the
            // level), so a linear phase match cannot follow it exactly: ~5.6 degrees at 40 Hz remain (-22.5 before the
            // distortions' DC filters were matched)
            { "factory defaults + Saturation + Compressor 3 bands", [] (CodexProcessor& p)
              { setParam (p, ID::sat ("On"), 1); setParam (p, ID::cmp ("On"), 1); setParam (p, ID::cmp ("Bands"), 3); }, 7.0 },
        };
        double worst = 0.0;
        for (const auto& cfg : cfgs)
        {
            std::cout << "   " << cfg.name << ":";
            std::vector<double> ds;
            for (double hz : freqs)
            {
                const double d = wrapDeg (subVsChainDeg (cfg.set, hz) - subVsChainDeg ([] (CodexProcessor& p) { allOff (p); }, hz));
                std::cout << "  " << (int) hz << " Hz " << juce::String (d, 2);
                ds.push_back (d);
            }
            std::cout << "\n";
            for (size_t k = 0; k < ds.size(); ++k)
            {
                if (cfg.limit <= 2.0) worst = std::max (worst, std::abs (ds[k]));
                check (std::abs (ds[k]) < cfg.limit, juce::String (cfg.name) + " at " + juce::String (freqs[k]) + " Hz: sub within "
                                                          + juce::String (cfg.limit, 0) + " degrees of the chain");
            }
        }
        std::cout << "   worst " << juce::String (worst, 2) << " degrees (linear chains)\n";
    }

    // ---- 4. Whole-sample latency, no crossover / DC filter: the sub is exactly the old integer delay ----
    {
        std::cout << "\nSub layer: whole-sample latency without crossovers is a pure delay (bit-exact)\n";
        const int len = (int) (0.5 * sr);
        auto off = std::make_unique<CodexProcessor>();
        allOff (*off);
        steadySine (*off, 52.0);
        setParam (*off, ID::subOn, 1);
        const auto a = renderL (*off, len);
        auto look = std::make_unique<CodexProcessor>();
        allOff (*look);
        steadySine (*look, 52.0);
        setParam (*look, ID::subOn, 1);
        // compressor, 1 band at ratio 1, unity gain, lookahead: delays everything (kick and sub) by whole samples only
        setParam (*look, ID::cmp ("On"), 1); setParam (*look, ID::cmp ("Bands"), 1); setParam (*look, ID::cmp ("Auto"), 0);
        setParam (*look, ID::cmpBand (0, "Ratio"), 1.0f); setParam (*look, ID::cmp ("Look"), 3);
        const auto b = renderL (*look, len);
        const int lat = look->getLatencySamples();
        int mismatches = 0;
        // from 50 ms on: the sub level glides in over the first 20 ms after prepare, at the output (after the delay)
        for (int i = (int) (0.05 * sr); i + lat < len; ++i) mismatches += std::memcmp (&b[(size_t) (i + lat)], &a[(size_t) i], sizeof (float)) != 0 ? 1 : 0;   // bit for bit
        std::cout << "   latency " << lat << " samples, " << mismatches << " samples (after 50 ms) differ from the all-off render shifted by it\n";
        check (lat > 0 && mismatches == 0, "whole-sample latency: sub (and kick) bit-exact with the all-off render, delayed");
    }
    // ---- 5. The sub's phase match switches, moves and changes slope without clicks ----
    {
        std::cout << "\nSub layer: phase-match sections switch / move without clicks (largest 2nd difference, 50 Hz sine at 0.5)\n";
        using PM = subalign::PhaseMatch;
        PM pm;
        pm.prepare (sr);
        const double w = juce::MathConstants<double>::twoPi * 50.0 / sr;
        const double steady = 0.5 * w * w;   // |2nd difference| of the clean sine
        const int len = (int) (1.6 * sr), chunk = 256;
        float y1 = 0.0f, y2 = 0.0f;
        double worst = 0.0;
        float fc = 120.0f;
        int slope = 2;
        for (int pos = 0; pos < len; pos += chunk)
        {
            const double t = pos / sr;
            const bool on = t >= 0.2 && t < 1.3;
            if (t >= 0.6) fc = 400.0f;                                // a crossover jumps (the compressor's do)
            if (t >= 0.8) fc = std::min (2000.0f, fc * 1.02f);        // ... and glides
            if (t >= 1.0) slope = 3;                                  // slope change while it plays
            pm.setCrossover (0, on, slope, fc);
            pm.setCrossover (PM::satSections, t >= 0.3, 2, 1000.0f);
            pm.setDcFilter (PM::satDc, t >= 0.4 && t < 1.4);
            for (int i = pos; i < std::min (len, pos + chunk); ++i)
            {
                const float y = pm.process (0.5f * (float) std::sin (w * i));
                if (i >= 2) worst = std::max (worst, (double) std::abs (y - 2.0f * y1 + y2));
                y2 = y1; y1 = y;
            }
        }
        std::cout << "   " << juce::String (worst / steady, 2) << " x the clean sine's\n";
        check (worst < 4.0 * steady, "phase-match switching / crossover moves stay click-free");
    }
    // ---- 6. Right after prepareToPlay the sub is already phase-matched (no 20 ms fade of the match at full sub level) ----
    {
        std::cout << "\nSub layer: level right after prepare, 120 Hz sine, Saturation 3 bands (X1 120 Hz) + Compressor 3 bands (linear)\n";
        const double hz = 120.0;
        auto make = [hz] (bool subOn)
        {
            auto p = std::make_unique<CodexProcessor>();
            allOff (*p);
            setParam (*p, ID::sat ("On"), 1);
            for (int b = 0; b < sat::maxBands; ++b) setParam (*p, ID::satBand (b, "Bypass"), 1);
            setParam (*p, ID::cmp ("On"), 1); setParam (*p, ID::cmp ("Bands"), 3); setParam (*p, ID::cmp ("Auto"), 0);
            for (int b = 0; b < 3; ++b) setParam (*p, ID::cmpBand (b, "Ratio"), 1.0f);
            steadySine (*p, hz);
            setParam (*p, ID::subOn, subOn ? 1.0f : 0.0f);
            return renderL (*p, (int) (1.2 * sr));
        };
        const auto a = make (false), b = make (true);
        const int period = (int) std::ceil (sr / hz);
        auto peak = [&] (int from)
        {
            double m = 0.0;
            for (int i = from; i < from + period; ++i) m = std::max (m, (double) std::abs (b[(size_t) i] - a[(size_t) i]));
            return m;
        };
        const double steady = peak ((int) (1.0 * sr));
        double lowest = 1.0e9;
        // from 10 ms on: the sine, its low-pass and the all-passes start from rest at sample 0 (a few ms of onset)
        for (int from = (int) (0.010 * sr); from <= (int) (0.024 * sr); from += 24) lowest = std::min (lowest, peak (from) / steady);
        std::cout << "   lowest one-cycle sub peak 10-32 ms after prepare: " << juce::String (juce::Decibels::gainToDecibels (lowest), 2) << " dB of the steady level\n";
        check (lowest > juce::Decibels::decibelsToGain (-1.0), "sub at full level from the first cycle after prepare (match not fading in)");
    }

    // ---- 7. Switching a crossover section in / out / to another slope never dips the sub (no dry + all-pass cancellation) ----
    {
        std::cout << "\nSub layer: level while phase-match sections switch (80 Hz sine at its crossover frequency)\n";
        using PM = subalign::PhaseMatch;
        PM pm;
        pm.prepare (sr);
        const double hz = 80.0, w = juce::MathConstants<double>::twoPi * hz / sr;
        const int len = (int) (1.2 * sr), chunk = 256, period = (int) std::ceil (sr / hz);
        std::vector<float> y ((size_t) len);
        for (int pos = 0; pos < len; pos += chunk)
        {
            const double t = pos / sr;
            pm.setCrossover (0, t >= 0.1 && t < 0.7, t >= 0.4 ? 3 : 2, 80.0f);   // in, slope change while in, out
            pm.setCrossover (PM::satSections, t >= 0.2 && t < 0.9, 2, 80.0f);
            for (int i = pos; i < std::min (len, pos + chunk); ++i) y[(size_t) i] = pm.process (0.5f * (float) std::sin (w * i));
        }
        double lowest = 1.0e9;
        for (int from = (int) (0.05 * sr); from + period <= len; from += 24)
        {
            double m = 0.0;
            for (int i = from; i < from + period; ++i) m = std::max (m, (double) std::abs (y[(size_t) i]));
            lowest = std::min (lowest, m / 0.5);
        }
        std::cout << "   lowest one-cycle peak: " << juce::String (juce::Decibels::gainToDecibels (lowest), 2) << " dB\n";
        // what remains (~ -0.8 dB for a few ms) is the glide sweeping the section's phase through the sine's own frequency
        check (lowest > juce::Decibels::decibelsToGain (-1.5), "phase-match fades keep the sub's level (no cancellation at the crossover)");
    }
    return bad;
}
