#include "PluginEditor.h"
#include "../PluginProcessor.h"

using namespace juce;

HardKickEditor::HardKickEditor (HardKickProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      presetBar (p.presets),
      engineSwitch (*p.apvts.getParameter (ID::engine), { "SYNTH", "909" }),
      pitchEnvDisplay (p.apvts, ID::pAttack, ID::pDecay, ID::pSustain, ID::pRelease, ID::pCurve, Colours_::accent2),
      ampEnvDisplay (p.apvts, ID::aAttack, ID::aDecay, ID::aSustain, ID::aRelease, ID::aCurve, Colours_::accent),
      eqDisplay (p), bandControls (p.apvts), tuner (p)
{
    setLookAndFeel (&lnf);
    auto& s = p.apvts;

    kickButton.setComponentID ("kick");
    kickButton.setColour (TextButton::textColourOffId, Colour (0xff04101e));
    kickButton.setColour (TextButton::textColourOnId, Colour (0xff04101e));
    kickButton.onPress = [this] (bool down) { proc.triggerPreview (down); };

    keyBox.attach (s, ID::key);
    octaveBox.attach (s, ID::octave);
    keyModeBox.attach (s, ID::keyMode);

    wave.attach (s, ID::wave);
    model.attach (s, ID::model909);
    tune.attach (s, ID::tune);
    phase.attach (s, ID::phase);
    clickLevel.attach (s, ID::clickLevel);
    clickDecay.attach (s, ID::clickDecay);
    clickTone.attach (s, ID::clickTone);
    tune909.attach (s, ID::tune909);
    attack909.attach (s, ID::attack909);
    decay909.attach (s, ID::decay909);

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
             &presetBar, &keyBox, &octaveBox, &keyModeBox, &kickButton,
             &engineSwitch, &wave, &model, &tune, &phase, &clickLevel, &clickDecay, &clickTone,
             &tune909, &attack909, &decay909,
             &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve,
             &ampEnvDisplay, &aAttack, &aDecay, &aSustain, &aRelease, &aCurve,
             &eqDisplay, &bandControls, &tuner, &outGain, &meter })
        addAndMakeVisible (c);

    eqDisplay.onBandSelected = [this] (int b) { bandControls.setBand (b); };
    bandControls.onBandSelected = [this] (int b) { eqDisplay.setSelectedBand (b); };
    engineSwitch.onChange = [this] (int) { updateEngineView(); };

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

    setSize (1180, 800);
    updateEngineView();
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

void HardKickEditor::updateEngineView()
{
    const bool is909 = engineSwitch.getIndex() == 1;

    for (Component* c : std::initializer_list<Component*> { &wave, &clickLevel, &clickDecay, &clickTone })
        c->setVisible (! is909);
    for (Component* c : std::initializer_list<Component*> { &model, &tune909, &attack909, &decay909 })
        c->setVisible (is909);

    // Grid:  [combo] [a] [b] / [c] [d] [e]
    auto& cell = sourceCells;
    if (is909)
    {
        model.setBounds (cell[0].reduced (4, 12));
        tune909.setBounds (cell[1]);
        attack909.setBounds (cell[2]);
        decay909.setBounds (cell[3]);
        tune.setBounds (cell[4]);
        phase.setBounds (cell[5]);
    }
    else
    {
        wave.setBounds (cell[0].reduced (4, 12));
        tune.setBounds (cell[1]);
        phase.setBounds (cell[2]);
        clickLevel.setBounds (cell[3]);
        clickDecay.setBounds (cell[4]);
        clickTone.setBounds (cell[5]);
    }

    // The 909 has its own pitch sweep; the amp ADSR only gates it (attack + release)
    for (Component* c : std::initializer_list<Component*> { &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve })
    {
        c->setEnabled (! is909);
        c->setAlpha (is909 ? 0.25f : 1.0f);
    }
    for (Component* c : std::initializer_list<Component*> { &aDecay, &aSustain, &ampEnvDisplay })
    {
        c->setEnabled (! is909);
        c->setAlpha (is909 ? 0.3f : 1.0f);
    }
    repaint();
}

void HardKickEditor::drawPanel (Graphics& g, Rectangle<int> r, const String& title, Colour dot)
{
    const auto rf = r.toFloat();
    g.setGradientFill (ColourGradient (Colours_::panelTop, 0, rf.getY(), Colours_::panel, 0, rf.getBottom(), false));
    g.fillRoundedRectangle (rf, 8.0f);

    auto strip = rf.withHeight (26.0f);
    Path headerPath;
    headerPath.addRoundedRectangle (strip.getX(), strip.getY(), strip.getWidth(), strip.getHeight(), 8.0f, 8.0f, true, true, false, false);
    g.setColour (Colours_::header);
    g.fillPath (headerPath);
    g.setColour (Colours_::outline);
    g.drawHorizontalLine ((int) strip.getBottom(), strip.getX(), strip.getRight());
    g.drawRoundedRectangle (rf.reduced (0.5f), 8.0f, 1.0f);

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
    const auto bar = getLocalBounds().removeFromTop (header.getBottom() + 6).toFloat();
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

    drawPanel (g, sourceArea, "SOURCE", Colours_::accent);
    drawPanel (g, pitchArea, "PITCH ENVELOPE", Colours_::accent2);
    drawPanel (g, ampArea, engineSwitch.getIndex() == 1 ? "AMP GATE" : "AMP ENVELOPE", Colours_::accent);
    drawPanel (g, eqArea, "EQUALIZER", Colours_::accent);
    drawPanel (g, clip1Area, String (CharPointer_UTF8 ("DISTORTION 1  \xc2\xb7  PRE-EQ")), Colours_::accent);
    drawPanel (g, clip2Area, String (CharPointer_UTF8 ("DISTORTION 2  \xc2\xb7  POST-EQ")), Colours_::accent);
    drawPanel (g, outArea, String (CharPointer_UTF8 ("TUNER  \xc2\xb7  MASTER")), Colours_::accent);
}

void HardKickEditor::paintOverChildren (Graphics& g)
{
    if (engineSwitch.getIndex() != 1)
        return;

    auto r = pitchArea.toFloat().withTrimmedTop (27.0f).reduced (1.0f);
    g.setColour (Colours_::panel.withAlpha (0.55f));
    g.fillRect (r);
    g.setColour (Colours_::text);
    g.setFont (FontOptions (12.0f, Font::bold).withKerningFactor (0.1f));
    g.drawText ("909 ENGINE", r.withTrimmedBottom (r.getHeight() * 0.5f), Justification::centredBottom);
    g.setColour (Colours_::textDim);
    g.setFont (FontOptions (11.0f));
    g.drawText ("Pitch sweep is built into the 909 model: use its TUNE knob",
                r.withTrimmedTop (r.getHeight() * 0.5f + 4.0f), Justification::centredTop);
}

void HardKickEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    header = r.removeFromTop (42);

    {
        auto h = header;
        h.removeFromLeft (190);
        kickButton.setBounds (h.removeFromRight (120).reduced (0, 3));
        h.removeFromRight (14);
        keyModeBox.setBounds (h.removeFromRight (110));
        h.removeFromRight (6);
        octaveBox.setBounds (h.removeFromRight (64));
        h.removeFromRight (6);
        keyBox.setBounds (h.removeFromRight (70));
        h.removeFromRight (30);
        presetBar.setBounds (h.removeFromRight (420).withSizeKeepingCentre (420, 28).translated (0, 6));
    }

    r.removeFromTop (14);
    auto rowA = r.removeFromTop (200);
    sourceArea = rowA.removeFromLeft (330);
    rowA.removeFromLeft (10);
    pitchArea = rowA.removeFromLeft ((rowA.getWidth() - 10) / 2);
    rowA.removeFromLeft (10);
    ampArea = rowA;

    r.removeFromTop (10);
    auto rowC = r.removeFromBottom (150);
    r.removeFromBottom (10);
    eqArea = r;

    clip1Area = rowC.removeFromLeft (420);
    rowC.removeFromLeft (10);
    clip2Area = rowC.removeFromLeft (420);
    rowC.removeFromLeft (10);
    outArea = rowC;

    // Source: engine switch in the header strip, 2 x 3 grid below
    engineSwitch.setBounds (sourceArea.withHeight (26).removeFromRight (110).reduced (4, 3));
    {
        auto c = sourceArea.reduced (8).withTrimmedTop (24);
        auto row1 = c.removeFromTop (c.getHeight() / 2 - 3);
        c.removeFromTop (6);
        const int w = c.getWidth() / 3;
        for (int i = 0; i < 3; ++i) sourceCells[(size_t) i] = row1.removeFromLeft (w);
        for (int i = 3; i < 6; ++i) sourceCells[(size_t) i] = c.removeFromLeft (w);
        sourceCells[2] = sourceCells[2].withRight (row1.getRight());   // absorb rounding
        sourceCells[5] = sourceCells[5].withRight (c.getRight());
    }
    updateEngineView();

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

    {
        auto c = eqArea.reduced (8).withTrimmedTop (24);
        bandControls.setBounds (c.removeFromBottom (78));
        c.removeFromBottom (6);
        eqDisplay.setBounds (c);
    }

    Rectangle<int> clipAreas[] = { clip1Area, clip2Area };
    for (int i = 0; i < 2; ++i)
    {
        clips[i].on.setBounds (clipAreas[i].withHeight (26).removeFromRight (30).reduced (4));
        auto c = clipAreas[i].reduced (8).withTrimmedTop (24);
        auto left = c.removeFromLeft (118);
        clips[i].mode.setBounds (left.withSizeKeepingCentre (left.getWidth() - 6, 44));
        const int w = c.getWidth() / 4;
        clips[i].drive.setBounds (c.removeFromLeft (w));
        clips[i].bias.setBounds (c.removeFromLeft (w));
        clips[i].mix.setBounds (c.removeFromLeft (w));
        clips[i].out.setBounds (c);
    }

    {
        auto c = outArea.reduced (8).withTrimmedTop (24);
        meter.setBounds (c.removeFromRight (16));
        c.removeFromRight (6);
        tuner.setBounds (c.removeFromLeft (180).withSizeKeepingCentre (180, 88));
        c.removeFromLeft (4);
        outGain.setBounds (c);
    }
}
