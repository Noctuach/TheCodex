#include "PluginEditor.h"
#include "../PluginProcessor.h"

using namespace juce;

namespace
{
    String utf8 (const char* s) { return String (CharPointer_UTF8 (s)); }
    const String dot = utf8 ("\xc2\xb7");

    constexpr const char* scaleKey = "uiScale";
    constexpr const char* tooltipsKey = "tooltips";
}

//==============================================================================
MainView::MainView (CodexProcessor& p, PropertiesFile* settingsFile)
    : proc (p), settings (settingsFile),
      presetBar (p.presets),
      tunerAnalyser (p), tunerChip (tunerAnalyser),
      engineSwitch (*p.apvts.getParameter (ID::engine), { "SYNTH", "909" }, Help::forParameter (ID::engine)),
      pitchEnvDisplay (p.apvts, ID::pAttack, ID::pDecay, ID::pSustain, ID::pRelease, ID::pCurve, Colours_::accent2),
      ampEnvDisplay (p.apvts, ID::aAttack, ID::aDecay, ID::aSustain, ID::aRelease, ID::aCurve, Colours_::accent),
      eqDisplay (p), bandControls (p.apvts),
      unit1 (p, 1, "DISTORTION 1  " + dot + "  PRE-EQ"),
      unit2 (p, 2, "DISTORTION 2  " + dot + "  POST-EQ"),
      scopePage (p, tunerAnalyser),
      saturationPage (p, scopePage.getHistory()),
      spacePage (p),
      compressorPage (p),
      eq2Page (p),
      aboutOverlay (JucePlugin_VersionString)
{
    auto& s = p.apvts;

    // ---- Header ----
    previewButton.setTooltip ("Preview: plays the kick in the selected key (hold to hear the full tail).");
    previewButton.onPress = [this] (bool down) { proc.triggerPreview (down); };

    undoButton.setTooltip ("Undo");
    redoButton.setTooltip ("Redo");
    menuButton.setTooltip ("Menu: presets, UI size, tooltips and About");
    undoButton.onClick = [this] { proc.undoManager.undo(); };
    redoButton.onClick = [this] { proc.undoManager.redo(); };
    menuButton.onClick = [this] { showMainMenu(); };

    presetBar.onSaveRequested = [this]
    {
        saveOverlay.setInitialName (proc.presets.getCurrentName());
        saveOverlay.open();
    };
    saveOverlay.exists = [this] (const String& n) { return proc.presets.userPresetExists (n); };
    saveOverlay.onSave = [this] (const String& n)
    {
        proc.presets.saveUserPreset (n);
        presetBar.refresh();
    };

    keyBox.attach (s, ID::key);
    octaveBox.attach (s, ID::octave);
    keyModeBox.attach (s, ID::keyMode);
    outGain.attach (s, ID::outGain);

    for (int i = 0; i < (int) tabButtons.size(); ++i)
    {
        tabButtons[(size_t) i].setComponentID ("tab");
        tabButtons[(size_t) i].onClick = [this, i] { showTab (i); };
        addAndMakeVisible (tabButtons[(size_t) i]);
    }

    // ---- Source ----
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
    engineSwitch.onChange = [this] (int) { updateEngineView(); };

    // ---- Envelopes ----
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

    // ---- EQ ----
    eqDisplay.onBandSelected = [this] (int b) { bandControls.setBand (b); };
    bandControls.onBandSelected = [this] (int b) { eqDisplay.setSelectedBand (b); };

    for (Component* c : std::initializer_list<Component*> {
             &undoButton, &redoButton, &menuButton, &presetBar, &keyBox, &octaveBox, &keyModeBox,
             &previewButton, &outGain, &meterL, &meterR, &tunerChip,
             &engineSwitch, &wave, &model, &tune, &phase, &clickLevel, &clickDecay, &clickTone, &tune909, &attack909, &decay909,
             &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve,
             &ampEnvDisplay, &aAttack, &aDecay, &aSustain, &aRelease, &aCurve,
             &eqDisplay, &bandControls, &unit1, &unit2, &scopePage, &saturationPage, &spacePage, &compressorPage, &eq2Page })
        addAndMakeVisible (c);
    addChildComponent (engineNotice);

    addChildComponent (saveOverlay);
    addChildComponent (aboutOverlay);

    setTooltipsEnabled (settings == nullptr || settings->getBoolValue (tooltipsKey, true));
    setSize (baseWidth, baseHeight);
    showTab (0);
    startTimerHz (60);
}

void MainView::setTooltipsEnabled (bool enabled)
{
    if (enabled && tooltipWindow == nullptr)
        tooltipWindow = std::make_unique<TooltipWindow> (this, 650);
    else if (! enabled)
        tooltipWindow.reset();
    if (settings != nullptr)
        settings->setValue (tooltipsKey, enabled);
}

void MainView::timerCallback()
{
    meterL.setLevel (proc.getAndResetPeak (0));
    meterR.setLevel (proc.getAndResetPeak (1));
    undoButton.setEnabled (proc.undoManager.canUndo());
    redoButton.setEnabled (proc.undoManager.canRedo());
}

void MainView::showMainMenu()
{
    PopupMenu menu, sizes;
    const float current = settings != nullptr ? (float) settings->getDoubleValue (scaleKey, 1.0) : 1.0f;
    const float scales[] = { 0.75f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    for (int i = 0; i < 7; ++i)
        sizes.addItem (100 + i, String (roundToInt (scales[i] * 100.0f)) + " %", true, std::abs (current - scales[i]) < 0.01f);

    const auto presetName = proc.presets.getCurrentName();
    menu.addSectionHeader ("Presets");
    menu.addItem (1, "Save preset...");
    menu.addItem (2, "Delete \"" + presetName + "\"", proc.presets.isCurrentUserPreset());
    menu.addItem (3, "Open preset folder");
    menu.addItem (4, "Rescan preset folder");
    menu.addSectionHeader ("Edit");
    menu.addItem (5, "Undo", proc.undoManager.canUndo());
    menu.addItem (6, "Redo", proc.undoManager.canRedo());
    menu.addSectionHeader ("View");
    menu.addSubMenu ("Interface size", sizes);
    menu.addItem (7, "Show tooltips", true, tooltipWindow != nullptr);
    menu.addSeparator();
    menu.addItem (8, "About Codex");

    SafePointer<MainView> safe (this);
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&menuButton).withMinimumWidth (220),
        [safe, scales] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& v = *safe;
            switch (r)
            {
                case 1: v.presetBar.onSaveRequested(); break;
                case 2: v.proc.presets.deleteUserPreset (v.proc.presets.getCurrentName()); v.presetBar.refresh(); break;
                case 3: v.proc.presets.getUserDirectory().startAsProcess(); break;
                case 4: v.proc.presets.refreshUserPresets(); v.presetBar.refresh(); break;
                case 5: v.proc.undoManager.undo(); break;
                case 6: v.proc.undoManager.redo(); break;
                case 7: v.setTooltipsEnabled (v.tooltipWindow == nullptr); break;
                case 8: v.aboutOverlay.open(); break;
                default:
                    if (r >= 100 && r < 107 && v.onScaleRequested) v.onScaleRequested (scales[r - 100]);
                    break;
            }
        });
}

void MainView::updateEngineView()
{
    const bool is909 = engineSwitch.getIndex() == 1;
    const bool kickTab = currentTab == 0;

    for (Component* c : std::initializer_list<Component*> { &wave, &clickLevel, &clickDecay, &clickTone })
        c->setVisible (kickTab && ! is909);
    for (Component* c : std::initializer_list<Component*> { &model, &tune909, &attack909, &decay909 })
        c->setVisible (kickTab && is909);

    // Grid:  [combo] [a] [b] / [c] [d] [e]
    auto& cell = sourceCells;
    auto comboCell = [] (Rectangle<int> r) { return r.reduced (6, 0).withSizeKeepingCentre (r.getWidth() - 12, 46); };
    if (is909)
    {
        model.setBounds (comboCell (cell[0]));
        tune909.setBounds (cell[1]); attack909.setBounds (cell[2]);
        decay909.setBounds (cell[3]); tune.setBounds (cell[4]); phase.setBounds (cell[5]);
    }
    else
    {
        wave.setBounds (comboCell (cell[0]));
        tune.setBounds (cell[1]); phase.setBounds (cell[2]);
        clickLevel.setBounds (cell[3]); clickDecay.setBounds (cell[4]); clickTone.setBounds (cell[5]);
    }

    // The 909 has its own pitch sweep; the amp ADSR only gates it (attack + release)
    for (Component* c : std::initializer_list<Component*> { &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve })
    {
        c->setEnabled (! is909);
        c->setAlpha (is909 ? 0.2f : 1.0f);
    }
    for (Component* c : std::initializer_list<Component*> { &aDecay, &aSustain, &ampEnvDisplay })
    {
        c->setEnabled (! is909);
        c->setAlpha (is909 ? 0.3f : 1.0f);
    }
    engineNotice.setVisible (is909 && kickTab);
    repaint();
}

std::vector<Component*> MainView::kickTabComponents()
{
    return { &engineSwitch, &tune, &phase,
             &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve,
             &ampEnvDisplay, &aAttack, &aDecay, &aSustain, &aRelease, &aCurve,
             &eqDisplay, &bandControls };
}

void MainView::showTab (int tab)
{
    currentTab = tab;
    for (auto* c : kickTabComponents())
        c->setVisible (tab == 0);
    unit1.setVisible (tab == 1);
    unit2.setVisible (tab == 1);
    saturationPage.setVisible (tab == 2);
    compressorPage.setVisible (tab == 3);
    eq2Page.setVisible (tab == 4);
    spacePage.setVisible (tab == 5);
    scopePage.setVisible (tab == 6);
    for (int i = 0; i < (int) tabButtons.size(); ++i)
        tabButtons[(size_t) i].setToggleState (i == tab, dontSendNotification);
    updateEngineView();
    repaint();
}

//==============================================================================
void MainView::paint (Graphics& g)
{
    // Background: flat graphite
    g.fillAll (Colours_::background);

    // Header bar
    const auto bar = getLocalBounds().removeFromTop (header.getBottom() + 8).toFloat();
    g.setColour (Colours_::panel);
    g.fillRect (bar);
    g.setColour (Colours_::outline);
    g.drawHorizontalLine ((int) bar.getBottom() - 1, 0, (float) getWidth());

    // Logo: mark + wordmark + subtitle
    {
        auto r = logoArea.toFloat();
        auto mark = r.removeFromLeft (40.0f).withSizeKeepingCentre (38.0f, 38.0f);
        Icons::draw (g, Icons::kickMark(), mark.reduced (2.0f), Colours_::accent, 1.8f);
        r.removeFromLeft (10.0f);
        g.setColour (Colours_::text);
        g.setFont (Fonts::display (28.0f, 0.22f));
        g.drawText ("CODEX", r.removeFromTop (r.getHeight() * 0.64f), Justification::bottomLeft);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (10.5f, 0.2f));
        g.drawText ("HARDSTYLE KICK SYNTH", r.translated (1.0f, 0.0f), Justification::topLeft);
    }

    g.setColour (Colours_::textDim);
    g.setFont (Fonts::label (11.5f, 0.14f));
    g.drawText ("PRESET", presetArea.withHeight (16).translated (1, 0), Justification::centredLeft);

    // Tab bar
    g.setColour (Colours_::outline);
    g.drawHorizontalLine (tabBar.getBottom() - 1, (float) tabBar.getX(), (float) tabBar.getRight());
    if (breadcrumbArea.getWidth() > 0)
    {
        // signal-flow breadcrumb, the stage of the current tab highlighted
        const StringArray steps { "OSC", "DIST", "EQ", "DIST", "SAT", "COMP", "EQ", "SPACE" };
        const auto font = Font (Fonts::label (12.0f, 0.16f));
        g.setFont (font);
        float x = (float) breadcrumbArea.getRight();
        for (int i = steps.size() - 1; i >= 0; --i)
        {
            GlyphArrangement ga;
            ga.addLineOfText (font, steps[i], 0, 0);
            const float w = ga.getBoundingBox (0, -1, true).getWidth();
            x -= w;
            const bool hi = currentTab == 0 ? (i == 0 || i == 2) : currentTab == 1 ? (i == 1 || i == 3)
                          : currentTab == 2 ? i == 4 : currentTab == 3 ? i == 5 : currentTab == 4 ? i == 6 : currentTab == 5 ? i == 7 : false;
            g.setColour (hi ? Colours_::accent : Colours_::textFaint);
            g.drawText (steps[i], Rectangle<float> (x, (float) tabBar.getY(), w + 2.0f, (float) tabBar.getHeight() - 2.0f), Justification::centredLeft);
            if (i > 0)
            {
                x -= 20.0f;
                Icons::draw (g, Icons::chevronRight(), Rectangle<float> (x + 5.0f, (float) tabBar.getCentreY() - 6.0f, 10.0f, 10.0f), Colours_::textFaint, 1.3f);
            }
        }
    }

    if (currentTab == 0)
    {
        drawSectionPanel (g, sourceArea, "SOURCE", Colours_::accent);
        drawSectionPanel (g, pitchArea, "PITCH ENVELOPE", Colours_::accent2);
        drawSectionPanel (g, ampArea, engineSwitch.getIndex() == 1 ? "AMP GATE" : "AMP ENVELOPE", Colours_::accent);
        drawSectionPanel (g, eqArea, "EQUALIZER", Colours_::accent);
    }
}

void MainView::EngineNotice::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colours_::panel.withAlpha (0.6f));
    g.fillRoundedRectangle (r, 7.0f);
    auto card = r.withSizeKeepingCentre (jmin (380.0f, r.getWidth() - 40.0f), 64.0f);
    g.setColour (Colours_::display.withAlpha (0.92f));
    g.fillRoundedRectangle (card, 8.0f);
    g.setColour (Colours_::outlineHi);
    g.drawRoundedRectangle (card, 8.0f, 1.0f);
    g.setColour (Colours_::text);
    g.setFont (Fonts::label (14.0f, 0.18f));
    g.drawText ("909 ENGINE ACTIVE", card.removeFromTop (34.0f), Justification::centredBottom);
    g.setColour (Colours_::textDim);
    g.setFont (Fonts::regular (13.0f));
    g.drawText ("The pitch sweep is built into the 909 model. Shape it with TUNE.", card.withTrimmedTop (2.0f), Justification::centredTop);
}

void MainView::resized()
{
    auto r = getLocalBounds().reduced (12);

    // ---- Header ----
    header = r.removeFromTop (52);
    {
        auto h = header;
        logoArea = h.removeFromLeft (240);
        h.removeFromLeft (10);
        auto lower = [] (Rectangle<int> a) { return a.withTrimmedTop (18).withHeight (30); };
        undoButton.setBounds (lower (h.removeFromLeft (30)));
        h.removeFromLeft (4);
        redoButton.setBounds (lower (h.removeFromLeft (30)));
        h.removeFromLeft (22);

        menuButton.setBounds (lower (h.removeFromRight (34)));
        h.removeFromRight (12);
        meterR.setBounds (h.removeFromRight (9).withTrimmedTop (4).withTrimmedBottom (2));
        h.removeFromRight (3);
        meterL.setBounds (h.removeFromRight (9).withTrimmedTop (4).withTrimmedBottom (2));
        h.removeFromRight (8);
        outGain.setBounds (h.removeFromRight (112).withTrimmedTop (6).withHeight (42));
        h.removeFromRight (16);
        keyModeBox.setBounds (h.removeFromRight (112));
        h.removeFromRight (8);
        octaveBox.setBounds (h.removeFromRight (64));
        h.removeFromRight (8);
        keyBox.setBounds (h.removeFromRight (72));
        h.removeFromRight (24);
        presetArea = h;
        presetBar.setBounds (lower (h));
    }

    r.removeFromTop (14);
    tabBar = r.removeFromTop (32);
    {
        auto t = tabBar;
        const int widths[] = { 60, 112, 112, 120, 56, 70, 68 };
        for (int i = 0; i < 7; ++i)
        {
            tabButtons[(size_t) i].setBounds (t.removeFromLeft (widths[i]));
            t.removeFromLeft (6);
        }
        // the tabs follow the signal chain, so the breadcrumb is only drawn when there is room for it
        breadcrumbArea = t.getWidth() > 640 ? t.removeFromRight (380) : juce::Rectangle<int> (t.getRight(), t.getY(), 0, t.getHeight());
        t.removeFromRight (16);
        previewButton.setBounds (t.removeFromRight (30).withSizeKeepingCentre (30, 26).translated (0, -2));
        t.removeFromRight (8);
        tunerChip.setBounds (t.removeFromRight (150).withSizeKeepingCentre (150, 26).translated (0, -2));
    }
    r.removeFromTop (12);

    // ---- Distortion / saturation / scope pages ----
    {
        auto u = r;
        unit1.setBounds (u.removeFromLeft ((u.getWidth() - 12) / 2));
        u.removeFromLeft (12);
        unit2.setBounds (u);
    }
    saturationPage.setBounds (r);
    spacePage.setBounds (r);
    compressorPage.setBounds (r);
    eq2Page.setBounds (r);
    scopePage.setBounds (r);

    // ---- Kick page ----
    auto rowA = r.removeFromTop (222);
    sourceArea = rowA.removeFromLeft (344);
    rowA.removeFromLeft (12);
    pitchArea = rowA.removeFromLeft ((rowA.getWidth() - 12) / 2);
    rowA.removeFromLeft (12);
    ampArea = rowA;
    r.removeFromTop (12);
    eqArea = r;

    engineSwitch.setBounds (sourceArea.withHeight (28).removeFromRight (116).reduced (5, 4));
    {
        auto c = sourceArea.reduced (10).withTrimmedTop (30);
        auto row1 = c.removeFromTop (c.getHeight() / 2 - 2);
        c.removeFromTop (4);
        const int w = c.getWidth() / 3;
        for (int i = 0; i < 3; ++i) sourceCells[(size_t) i] = row1.removeFromLeft (i == 2 ? row1.getWidth() : w);
        for (int i = 3; i < 6; ++i) sourceCells[(size_t) i] = c.removeFromLeft (i == 5 ? c.getWidth() : w);
    }
    updateEngineView();

    auto layoutEnv = [] (Rectangle<int> area, EnvelopeDisplay& display, std::initializer_list<Knob*> knobs)
    {
        auto c = area.reduced (10).withTrimmedTop (30);
        display.setBounds (c.removeFromTop (70));
        c.removeFromTop (6);
        const int w = c.getWidth() / (int) knobs.size();
        for (auto* k : knobs)
            k->setBounds (c.removeFromLeft (w));
    };
    engineNotice.setBounds (pitchArea.withTrimmedTop (29).reduced (1));
    layoutEnv (pitchArea, pitchEnvDisplay, { &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve });
    layoutEnv (ampArea, ampEnvDisplay, { &aAttack, &aDecay, &aSustain, &aRelease, &aCurve });

    {
        auto c = eqArea.reduced (10).withTrimmedTop (30);
        bandControls.setBounds (c.removeFromBottom (84));
        c.removeFromBottom (8);
        eqDisplay.setBounds (c);
    }

    saveOverlay.setBounds (getLocalBounds());
    aboutOverlay.setBounds (getLocalBounds());
}

//==============================================================================
CodexEditor::CodexEditor (CodexProcessor& p)
    : AudioProcessorEditor (p),
      view (p, [this]
      {
          PropertiesFile::Options o;
          o.applicationName = "Codex";
          o.filenameSuffix = ".settings";
         #if JUCE_LINUX
          o.folderName = ".config/Codex";   // keep the user's home folder tidy
         #else
          o.folderName = "Codex";
         #endif
          o.osxLibrarySubFolder = "Application Support";
          o.millisecondsBeforeSaving = 500;
          appProperties.setStorageParameters (o);
          return appProperties.getUserSettings();
      }())
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (view);
    view.onScaleRequested = [this] (float s) { applyScale (s); };

    // Read the saved size first: setting the resize limits below triggers resized() calls
    // that must not overwrite it.
    float scale = 1.0f;
    auto* s = appProperties.getUserSettings();
    if (s != nullptr && s->containsKey (scaleKey))
        scale = (float) s->getDoubleValue (scaleKey, 1.0);
    else if (auto* display = Desktop::getInstance().getDisplays().getPrimaryDisplay())
        scale = jmin (1.0f, 0.85f * (float) display->userArea.getHeight() / (float) MainView::baseHeight);   // fit smaller screens

    const auto w = MainView::baseWidth, h = MainView::baseHeight;
    setResizable (true, true);
    setResizeLimits (roundToInt (w * 0.6), roundToInt (h * 0.6), w * 2, h * 2);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    applyScale (scale);
    initialised = true;
}

CodexEditor::~CodexEditor()
{
    setLookAndFeel (nullptr);
}

void CodexEditor::applyScale (float scale)
{
    scale = jlimit (0.6f, 2.0f, scale);
    setSize (roundToInt (MainView::baseWidth * scale), roundToInt (MainView::baseHeight * scale));
}

void CodexEditor::resized()
{
    const float scale = (float) getWidth() / (float) MainView::baseWidth;
    view.setTransform (AffineTransform::scale (scale));
    view.setBounds (0, 0, MainView::baseWidth, MainView::baseHeight);
    if (initialised)
        if (auto* s = appProperties.getUserSettings())
            s->setValue (scaleKey, scale);
}
