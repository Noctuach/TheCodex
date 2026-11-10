#include "PluginEditor.h"
#include "../PluginProcessor.h"

using namespace juce;

HardKickEditor::HardKickEditor (HardKickProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      pitchEnvDisplay (p.apvts, ID::pAttack, ID::pDecay, ID::pSustain, ID::pRelease, ID::pCurve, Colours_::accent2),
      ampEnvDisplay (p.apvts, ID::aAttack, ID::aDecay, ID::aSustain, ID::aRelease, ID::aCurve, Colours_::accent),
      eqDisplay (p), bandControls (p.apvts)
{
    setLookAndFeel (&lnf);
    auto& s = p.apvts;

    kickButton.setComponentID ("kick");
    kickButton.setColour (TextButton::textColourOffId, Colour (0xff04101e));
    kickButton.setColour (TextButton::textColourOnId, Colour (0xff04101e));
    kickButton.onPress = [this] (bool down) { proc.triggerPreview (down); };
    addAndMakeVisible (kickButton);

    wave.attach (s, ID::wave);
    tune.attach (s, ID::tune);
    phase.attach (s, ID::phase);
    clickLevel.attach (s, ID::clickLevel);
    clickDecay.attach (s, ID::clickDecay);
    clickTone.attach (s, ID::clickTone);

    pAmount.attach (s, ID::pAmount);
    pAttack.attach (s, ID::pAttack);
    pDecay.attach (s, ID::pDecay);
    pSustain.attach (s, ID::pSustain);
    pRelease.attach (s, ID::pRelease);
    pCurve.attach (s, ID::pCurve);

    aAttack.attach (s, ID::aAttack);
    aDecay.attach (s, ID::aDecay);
    aSustain.attach (s, ID::aSustain);
    aRelease.attach (s, ID::aRelease);
    aCurve.attach (s, ID::aCurve);

    for (Component* c : std::initializer_list<Component*> {
             &wave, &tune, &phase, &clickLevel, &clickDecay, &clickTone,
             &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve,
             &ampEnvDisplay, &aAttack, &aDecay, &aSustain, &aRelease, &aCurve,
             &eqDisplay, &bandControls, &outGain, &meter })
        addAndMakeVisible (c);

    eqDisplay.onBandSelected = [this] (int b) { bandControls.setBand (b); };
    bandControls.onBandSelected = [this] (int b) { eqDisplay.setSelectedBand (b); };

    for (int i = 0; i < 2; ++i)
    {
        auto& c = clips[i];
        const int stage = i + 1;
        c.onAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clip (stage, "On"), c.on);
        c.mode.attach (s, ID::clip (stage, "Mode"));
        c.drive.attach (s, ID::clip (stage, "Drive"));
        c.bias.attach (s, ID::clip (stage, "Bias"));
        c.mix.attach (s, ID::clip (stage, "Mix"));
        c.out.attach (s, ID::clip (stage, "Out"));
        for (Component* comp : std::initializer_list<Component*> { &c.on, &c.mode, &c.drive, &c.bias, &c.mix, &c.out })
            addAndMakeVisible (comp);
    }

    outGain.attach (s, ID::outGain);

    setSize (1100, 790);
    startTimerHz (30);
}

HardKickEditor::~HardKickEditor()
{
    setLookAndFeel (nullptr);
}

void HardKickEditor::timerCallback()
{
    meter.setLevel (proc.getAndResetPeak());
}

void HardKickEditor::drawPanel (Graphics& g, Rectangle<int> r, const String& title, Colour dot)
{
    const auto rf = r.toFloat();
    g.setGradientFill (ColourGradient (Colours_::panelTop, 0, rf.getY(), Colours_::panel, 0, rf.getBottom(), false));
    g.fillRoundedRectangle (rf, 8.0f);

    // Header strip
    auto strip = rf.withHeight (26.0f);
    Path headerPath;
    headerPath.addRoundedRectangle (strip.getX(), strip.getY(), strip.getWidth(), strip.getHeight(), 8.0f, 8.0f, true, true, false, false);
    g.setColour (Colours_::header);
    g.fillPath (headerPath);
    g.setColour (Colours_::outline);
    g.drawHorizontalLine ((int) strip.getBottom(), strip.getX(), strip.getRight());
    g.drawRoundedRectangle (rf.reduced (0.5f), 8.0f, 1.0f);

    // Title with a little coloured indicator
    g.setColour (dot.withAlpha (0.3f));
    g.fillEllipse (rf.getX() + 9.0f, strip.getCentreY() - 5.0f, 10.0f, 10.0f);
    g.setColour (dot);
    g.fillEllipse (rf.getX() + 11.0f, strip.getCentreY() - 3.0f, 6.0f, 6.0f);
    g.setColour (Colour (0xffb7c6e6));
    g.setFont (FontOptions (11.0f, Font::bold).withKerningFactor (0.12f));
    g.drawText (title, strip.withTrimmedLeft (26.0f), Justification::centredLeft);
}

void HardKickEditor::paint (Graphics& g)
{
    g.setGradientFill (ColourGradient (Colour (0xff0d1426), 0, 0, Colour (0xff070b15), 0, (float) getHeight(), false));
    g.fillAll();

    // Top bar
    const auto bar = getLocalBounds().removeFromTop (header.getBottom() + 4).toFloat();
    g.setGradientFill (ColourGradient (Colour (0xff18233f), 0, 0, Colour (0xff0f1629), 0, bar.getBottom(), false));
    g.fillRect (bar);
    g.setColour (Colours_::outline);
    g.drawHorizontalLine ((int) bar.getBottom(), 0, (float) getWidth());

    // Logo
    auto logo = header.withTrimmedLeft (6).toFloat();
    g.setFont (FontOptions (24.0f, Font::bold).withKerningFactor (0.06f));
    GlyphArrangement ga;
    ga.addLineOfText (g.getCurrentFont(), "HARD", 0, 0);
    const float hardW = ga.getBoundingBox (0, -1, true).getWidth();
    g.setColour (Colours_::text);
    g.drawText ("HARD", logo, Justification::centredLeft);
    g.setColour (Colours_::accent);
    g.drawText ("KICK", logo.withTrimmedLeft (hardW + 2.0f), Justification::centredLeft);
    g.setColour (Colours_::textDim);
    g.setFont (FontOptions (9.5f, Font::bold).withKerningFactor (0.2f));
    g.drawText ("OLDSKOOL KICK SYNTH", logo.withTrimmedLeft (hardW * 2.0f + 18.0f), Justification::centredLeft);

    // Centre "screen" with the signal flow
    const auto sc = screen.toFloat();
    g.setColour (Colours_::display);
    g.fillRoundedRectangle (sc, 5.0f);
    g.setColour (Colours_::outline);
    g.drawRoundedRectangle (sc.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (Colours_::accent);
    g.setFont (FontOptions (11.5f, Font::bold).withKerningFactor (0.1f));
    g.drawText (String (CharPointer_UTF8 ("OSC  \xe2\x96\xb8  CLIP 1  \xe2\x96\xb8  EQ  \xe2\x96\xb8  CLIP 2  \xe2\x96\xb8  OUT")),
                sc, Justification::centred);

    drawPanel (g, oscArea, "OSCILLATOR", Colours_::accent);
    drawPanel (g, pitchArea, "PITCH ENVELOPE", Colours_::accent2);
    drawPanel (g, ampArea, "AMP ENVELOPE", Colours_::accent);
    drawPanel (g, eqArea, "EQUALIZER", Colours_::accent);
    drawPanel (g, clip1Area, String (CharPointer_UTF8 ("CLIP 1  \xc2\xb7  PRE-EQ")), Colours_::accent);
    drawPanel (g, clip2Area, String (CharPointer_UTF8 ("CLIP 2  \xc2\xb7  POST-EQ")), Colours_::accent);
    drawPanel (g, outArea, "MASTER", Colours_::accent);
}

void HardKickEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    header = r.removeFromTop (40);
    kickButton.setBounds (header.withTrimmedRight (2).removeFromRight (120).reduced (0, 2));
    screen = header.withSizeKeepingCentre (380, 30);

    r.removeFromTop (14);
    auto rowA = r.removeFromTop (200);
    oscArea = rowA.removeFromLeft (310);
    rowA.removeFromLeft (10);
    pitchArea = rowA.removeFromLeft ((rowA.getWidth() - 10) / 2);
    rowA.removeFromLeft (10);
    ampArea = rowA;

    r.removeFromTop (10);
    auto rowC = r.removeFromBottom (150);
    r.removeFromBottom (10);
    eqArea = r;

    clip1Area = rowC.removeFromLeft (440);
    rowC.removeFromLeft (10);
    clip2Area = rowC.removeFromLeft (440);
    rowC.removeFromLeft (10);
    outArea = rowC;

    // Oscillator: 2 rows x 3
    {
        auto c = oscArea.reduced (8).withTrimmedTop (24);
        auto row1 = c.removeFromTop (c.getHeight() / 2 - 3);
        c.removeFromTop (6);
        const int w = c.getWidth() / 3;
        wave.setBounds (row1.removeFromLeft (w).reduced (4, 12));
        tune.setBounds (row1.removeFromLeft (w));
        phase.setBounds (row1);
        clickLevel.setBounds (c.removeFromLeft (w));
        clickDecay.setBounds (c.removeFromLeft (w));
        clickTone.setBounds (c);
    }

    // Envelopes: display on top, knobs underneath
    auto layoutEnv = [] (Rectangle<int> area, EnvelopeDisplay& display, std::initializer_list<Knob*> knobs)
    {
        auto c = area.reduced (8).withTrimmedTop (24);
        display.setBounds (c.removeFromTop (62));
        c.removeFromTop (4);
        const int w = c.getWidth() / (int) knobs.size();
        for (auto* k : knobs)
            k->setBounds (c.removeFromLeft (w));
    };
    layoutEnv (pitchArea, pitchEnvDisplay, { &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve });
    layoutEnv (ampArea, ampEnvDisplay, { &aAttack, &aDecay, &aSustain, &aRelease, &aCurve });

    // EQ
    {
        auto c = eqArea.reduced (8).withTrimmedTop (24);
        bandControls.setBounds (c.removeFromBottom (78));
        c.removeFromBottom (6);
        eqDisplay.setBounds (c);
    }

    // Clip stages
    Rectangle<int> clipAreas[] = { clip1Area, clip2Area };
    for (int i = 0; i < 2; ++i)
    {
        clips[i].on.setBounds (clipAreas[i].withHeight (26).removeFromRight (30).reduced (4));
        auto c = clipAreas[i].reduced (8).withTrimmedTop (24);
        auto left = c.removeFromLeft (110);
        clips[i].mode.setBounds (left.withSizeKeepingCentre (left.getWidth() - 6, 44));
        const int w = c.getWidth() / 4;
        clips[i].drive.setBounds (c.removeFromLeft (w));
        clips[i].bias.setBounds (c.removeFromLeft (w));
        clips[i].mix.setBounds (c.removeFromLeft (w));
        clips[i].out.setBounds (c);
    }

    // Output
    {
        auto c = outArea.reduced (8).withTrimmedTop (24);
        meter.setBounds (c.removeFromRight (18));
        c.removeFromRight (8);
        outGain.setBounds (c);
    }
}
