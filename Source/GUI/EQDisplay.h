#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Widgets.h"
#include "KeyboardFocus.h"
#include "../DSP/ParametricEQ.h"

class CodexProcessor;

/**
    Interactive EQ display with a real-time spectrum analyzer (workflow modelled on Pro-Q 4).
      - Drag a node: frequency and gain. Shift: fine. Ctrl/Cmd + drag vertically: Q.
      - Mouse wheel over a node: Q (cut / band-pass filters: slope). Ctrl/Cmd + wheel: gain.
      - Alt-click a node: band on/off (bypass). Double-click a node: on/off.
      - Double-click empty space: new band (up to 24); the shape depends on where you click
        (far left: low cut, far right: high cut, bottom: notch, elsewhere: bell).
      - Right-click a node: shape, slope, stereo placement, split into L/R or M/S, reset, delete.
      - Delete / Backspace: delete the selected band.
*/
class EQDisplay : public juce::Component, private juce::Timer
{
public:
    /** eqIndex: 0 = EQ 1 (between the distortions), 1 = EQ 2 (post). */
    EQDisplay (CodexProcessor&, int eqIndex = 0);
    const int eqIdx;

    std::function<void (int)> onBandSelected;
    void setSelectedBand (int b) { selected = b; repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Copies band `from` into the first free band and gives the pair complementary placements. */
    static void splitBand (juce::AudioProcessorValueTreeState&, int eqIdx, int from, bool midSide);
    /** Returns a band to its defaults, which removes it from the display. */
    static void resetBand (juce::AudioProcessorValueTreeState&, int eqIdx, int b);
    static void deleteBand (juce::AudioProcessorValueTreeState& s, int eqIdx, int b) { resetBand (s, eqIdx, b); }
    /** A band is present on the display when it is in use or switched on (older states have no "Used"). */
    static bool isPresent (juce::AudioProcessorValueTreeState&, int eqIdx, int b);
    static int  numPresent (juce::AudioProcessorValueTreeState&, int eqIdx);
    static int  firstFreeBand (juce::AudioProcessorValueTreeState&, int eqIdx, int exclude = -1);
    /** Creates a band in the first free slot; returns its index or -1 when all 24 are in use. */
    static int  addBand (juce::AudioProcessorValueTreeState&, int eqIdx, eq::BandType, float freq, float gain = 0.0f, float q = 1.0f);
    /** Bypass / enable while keeping the band on the display. */
    static void setBandOn (juce::AudioProcessorValueTreeState&, int eqIdx, int b, bool on);
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;

    float freqToX (float f) const;
    float xToFreq (float x) const;
    float gainToY (float db) const;
    float yToGain (float y) const;
    float analyzerDbToY (float db) const;

    eq::BandSettings readBand (int b, bool scaled = true) const;
    float gainScale() const;
    juce::Point<float> nodePosition (int b) const;
    int hitTestNode (juce::Point<float>) const;

    bool present (int b) const { return isPresent (apvts, eqIdx, b); }
    juce::RangedAudioParameter* param (int b, const char* suffix) const;
    void setParam (int b, const char* suffix, float value);
    void setParamGesture (int b, const char* suffix, float value);
    void beginDrag (int b);
    void endDrag();
    void showBandMenu (int b);
    void showAnalyzerMenu();

    void drawGrid (juce::Graphics&);
    void drawAnalyzer (juce::Graphics&);
    void drawCurves (juce::Graphics&);
    void drawNodes (juce::Graphics&);
    void drawHoverReadout (juce::Graphics&);

    CodexProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Rectangle<float> plot;

    int selected = 0, dragging = -1, hover = -1;
    bool qDrag = false;
    juce::Point<float> lastDragPos, mousePos;
    bool mouseInside = false;

    static constexpr float maxDb = 30.0f, minFreq = 20.0f, maxFreq = 20000.0f;   // maxDb = parameter range
    float displayRange = 12.0f;   // visible range (auto-expands beyond the chosen one)
    juce::ComboBox rangeBox, modeBox, resBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment, resAttachment;

    // Analyzer
    struct Spectrum
    {
        std::vector<float> ring, db;
        void init (int n) { ring.assign ((size_t) n, 0.0f); db.assign ((size_t) n / 2, -150.0f); }
    };
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftData, pullBuffer;
    Spectrum post, pre;
    bool showPre = false, showPost = true, freeze = false;
    float analyzerRange = 90.0f, analyzerTilt = 4.5f, analyzerDecay = 1.5f;
    juce::TextButton preButton { "PRE" }, postButton { "POST" }, freezeButton { "FREEZE" };
    IconButton analyzerMenuButton { "analyzer", Icons::menu() };
    void updateSpectrum (bool preEq, Spectrum&);
};

/** Strip beneath the display: band selector + controls for the selected band + global gain scale. */
class BandControls : public juce::Component, private juce::Timer
{
public:
    BandControls (juce::AudioProcessorValueTreeState&, int eqIndex = 0);
    const int eqIdx;

    std::function<void (int)> onBandSelected;
    void setBand (int b);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    juce::AudioProcessorValueTreeState& apvts;
    int band = -1;

    IconButton prevButton { "prev", Icons::chevronLeft() }, nextButton { "next", Icons::chevronRight() };
    juce::TextButton addButton { "+ ADD" }, deleteButton { "DELETE" };
    PowerButton onButton;
    juce::TextButton resetButton { "RESET" };
    void step (int direction);
    void select (int b) { setBand (b); if (onBandSelected) onBandSelected (b); }
    juce::Rectangle<int> bandLabelArea;
    bool hasBand = true;
    ChoiceBox type { "Shape" }, slope { "Slope" }, placement { "Stereo" };
    Knob freq { "Freq" }, gain { "Gain" }, q { "Q" };
    HeaderKnob gainScale { "Gain Scale" };
    int lastCount = -1;
    juce::Rectangle<int> toolsArea;
};
