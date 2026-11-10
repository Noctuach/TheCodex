#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Widgets.h"
#include "../DSP/ParametricEQ.h"

class CodexProcessor;

/**
    Interactive EQ curve with a real-time spectrum analyzer.
      - Drag a node: frequency (x) and gain (y)
      - Mouse wheel over a node: Q
      - Double-click a node: toggle the band on/off
      - Double-click empty space: activate a free band as a bell at that position
      - Right-click a node: choose the filter type / slope
      - Shift + drag: fine adjustment
*/
class EQDisplay : public juce::Component, private juce::Timer
{
public:
    explicit EQDisplay (CodexProcessor&);

    std::function<void (int)> onBandSelected;
    void setSelectedBand (int b) { selected = b; repaint(); }
    void setDisplayRange (float db) { rangeDb = db; repaint(); }
    void setAnalyzerEnabled (bool on) { analyzerOn = on; repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;

    float freqToX (float f) const;
    float xToFreq (float x) const;
    float gainToY (float db) const;
    float yToGain (float y) const;
    float analyzerDbToY (float db) const;

    eq::BandSettings readBand (int b) const;
    juce::Point<float> nodePosition (int b) const;
    int hitTestNode (juce::Point<float>) const;
    bool typeUsesGain (eq::BandType t) const;

    juce::RangedAudioParameter* param (int b, const char* suffix) const;
    void setParam (int b, const char* suffix, float value);
    void beginDrag (int b);
    void endDrag();
    void showBandMenu (int b);

    void drawGrid (juce::Graphics&);
    void drawAnalyzer (juce::Graphics&);
    void drawCurves (juce::Graphics&);
    void drawNodes (juce::Graphics&);

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Rectangle<float> plot;

    int selected = 0, dragging = -1, hover = -1;
    juce::Point<float> lastDragPos;

    static constexpr float maxDb = 24.0f, minFreq = 20.0f, maxFreq = 20000.0f;  // maxDb = parameter range
    float rangeDb = 24.0f;      // visible gain range
    bool analyzerOn = true;

    // Analyzer
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> ring, fftData, spectrumDb, pullBuffer;
};

/** Pro-Q style strip beneath the display: band selector + controls for the selected band. */
class BandControls : public juce::Component
{
public:
    explicit BandControls (juce::AudioProcessorValueTreeState&);

    std::function<void (int)> onBandSelected;
    std::function<void (float)> onRangeChanged;
    std::function<void (bool)> onAnalyzerToggled;
    void setBand (int b);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    int band = -1;

    juce::TextButton bandButtons[6];
    PowerButton onButton;
    ChoiceBox type { "Type" }, slope { "Slope" };
    Knob freq { "Freq" }, gain { "Gain" }, q { "Q" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    juce::TextButton range12 { juce::String (juce::CharPointer_UTF8 ("\xc2\xb1" "12 dB")) }, range24 { juce::String (juce::CharPointer_UTF8 ("\xc2\xb1" "24 dB")) };
    juce::TextButton analyzerButton { "ANALYZER" }, resetButton { "RESET BAND" };
    juce::Rectangle<int> toolsArea;
};
