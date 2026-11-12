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
      tunerAnalyser (p), tunerChip (tunerAnalyser), chainStrip (p.apvts), sweep909 (p.apvts),
      engineSwitch (*p.apvts.getParameter (ID::engine), { "SYNTH", "909" }, Help::forParameter (ID::engine)),
      pitchEnvDisplay (p.apvts, ID::pAttack, ID::pDecay, ID::pSustain, ID::pRelease, ID::pCurve, Colours_::accent2),
      ampEnvDisplay (p.apvts, ID::aAttack, ID::aDecay, ID::aSustain, ID::aRelease, ID::aCurve, Colours_::accent),
      pitchDrawEditor (p.apvts, 'p', Colours_::accent2, "st"),
      ampDrawEditor (p.apvts, 'a', Colours_::accent, "%"),
      pitchModeSwitch (*p.apvts.getParameter (ID::envMode ('p')), { "ADSR", "DRAW" }, Help::forParameter (ID::envMode ('p'))),
      ampModeSwitch (*p.apvts.getParameter (ID::envMode ('a')), { "ADSR", "DRAW" }, Help::forParameter (ID::envMode ('a'))),
      eqDisplay (p), bandControls (p.apvts),
      unit1 (p, 1, "DISTORTION 1  " + dot + "  PRE-EQ"),
      unit2 (p, 2, "DISTORTION 2  " + dot + "  POST-EQ"),
      scopePage (p, tunerAnalyser),
      saturationPage (p, scopePage.getHistory()),
      spacePage (p),
      compressorPage (p),
      eq2Page (p),
      browserOverlay (p.presets, settingsFile),
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
    tune909.attach (s, ID::tuneTime909);   // the original Tune knob: speed of the pitch drop
    depth909.attach (s, ID::tune909);      // Tune Depth (mod): how far it drops
    accent909.attach (s, ID::accent909);
    overload909.attach (s, ID::overload909);
    hold909.setComponentID ("pill");
    hold909.setClickingTogglesState (true);
    hold909.setTooltip (Help::forParameter (ID::hold909));
    hold909Attachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, ID::hold909, hold909);
    for (Component* c : std::initializer_list<Component*> { &depth909, &accent909, &overload909, &hold909, &sweep909 })
        addChildComponent (c);
    attack909.attach (s, ID::attack909);
    decay909.attach (s, ID::decay909);
    engineSwitch.onChange = [this] (int) { updateEngineView(); };
    // ---- Footer ----
    for (int m = 0; m < ID::numMacros; ++m)
    {
        macroKnobs[(size_t) m].attach (s, ID::macro (m));
        macroKnobs[(size_t) m].slider.setTooltip (Help::forParameter (ID::macro (m)));
        macroKnobs[(size_t) m].slider.getProperties().set ("bipolar", true);
        addAndMakeVisible (macroKnobs[(size_t) m]);
    }
    for (auto* b : { &abA, &abB })
    {
        b->setComponentID ("seg");
        b->setTooltip ("A/B compare: switch between two versions of your sound. Edits go to the selected slot.");
        addAndMakeVisible (b);
    }
    abA.setConnectedEdges (Button::ConnectedOnRight);
    abB.setConnectedEdges (Button::ConnectedOnLeft);
    abA.setToggleState (true, dontSendNotification);
    abA.onClick = [this] { switchAB (0); };
    abB.onClick = [this] { switchAB (1); };
    clipButton.setComponentID ("pill");
    clipButton.setClickingTogglesState (true);
    clipButton.setTooltip (Help::forParameter (ID::clipOn));
    clipAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, ID::clipOn, clipButton);
    clipCeiling.attach (s, ID::clipCeiling);
    clipSoftness.attach (s, ID::clipSoft);
    clipCeiling.slider.setTooltip (Help::forParameter (ID::clipCeiling));
    clipSoftness.slider.setTooltip (Help::forParameter (ID::clipSoft));
    for (Component* c : std::initializer_list<Component*> { &clipButton, &clipCeiling, &clipSoftness, &exportButton }) addAndMakeVisible (c);
    abCopy.setComponentID ("pill");
    abCopy.setTooltip ("Copy the current slot to the other one");
    abCopy.onClick = [this] { abSlots[(size_t) (1 - abCurrent)] = proc.apvts.copyState(); };
    addAndMakeVisible (abCopy);

    subPowerAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, ID::subOn, subPower);
    subPower.setTooltip (Help::forParameter (ID::subOn));
    subPower.onStateChange = [this] { repaint (subArea); };
    subAlign.setComponentID ("pill");
    subAlign.setTooltip ("Align: finds the sub phase that gives the fullest low end for the current sound (renders the kick offline at 24 phases).");
    subAlign.onClick = [this]
    {
        subAlign.setButtonText ("...");
        const float best = KickExporter::findBestSubPhase (proc);
        if (auto* prm = proc.apvts.getParameter (ID::subPhase))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (best));
            prm->endChangeGesture();
        }
        if (auto* on = proc.apvts.getParameter (ID::subOn)) { on->beginChangeGesture(); on->setValueNotifyingHost (1.0f); on->endChangeGesture(); }
        subAlign.setButtonText ("ALIGN");
    };
    addAndMakeVisible (subAlign);
    subLevel.attach (s, ID::subLevel);
    subLowPass.attach (s, ID::subLowPass);
    subFade.attach (s, ID::subFade);
    subPhase.attach (s, ID::subPhase);
    velocity.attach (s, ID::velocity);
    for (Component* c : std::initializer_list<Component*> { &subPower, &subLevel, &subLowPass, &subFade, &subPhase, &velocity })
        addAndMakeVisible (c);

    pitchModeSwitch.onChange = [this] (int) { updateEngineView(); };
    ampModeSwitch.onChange = [this] (int) { updateEngineView(); };
    pitchDrawEditor.levelToText = [this] (float level) { return level * proc.apvts.getRawParameterValue (ID::pAmount)->load(); };

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
    chainStrip.onModuleClicked = [this] (int module) { showTab (ChainStrip::tabFor (module)); };
    eqDisplay.onBandSelected = [this] (int b) { bandControls.setBand (b); };
    bandControls.onBandSelected = [this] (int b) { eqDisplay.setSelectedBand (b); };

    for (Component* c : std::initializer_list<Component*> {
             &undoButton, &redoButton, &menuButton, &presetBar, &keyBox, &octaveBox, &keyModeBox,
             &previewButton, &outGain, &meterL, &meterR, &tunerChip, &chainStrip,
             &engineSwitch, &wave, &model, &tune, &phase, &clickLevel, &clickDecay, &clickTone, &tune909, &attack909, &decay909,
             &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve,
             &ampEnvDisplay, &aAttack, &aDecay, &aSustain, &aRelease, &aCurve,
             &pitchDrawEditor, &ampDrawEditor, &pitchModeSwitch, &ampModeSwitch,
             &eqDisplay, &bandControls, &unit1, &unit2, &scopePage, &saturationPage, &spacePage, &compressorPage, &eq2Page })
        addAndMakeVisible (c);
    addChildComponent (engineNotice);
    addChildComponent (pageVeil);
    addChildComponent (dancer);

    addChildComponent (saveOverlay);
    addChildComponent (browserOverlay);
    addChildComponent (guideOverlay);
    presetBar.onBrowseRequested = [this] { browserOverlay.open(); };
    addChildComponent (aboutOverlay);

    setOpaque (true);   // paint() fills the whole view
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

void MainView::setUnderlineTarget (bool snap)
{
    const auto b = tabButtons[(size_t) currentTab].getBounds().toFloat();
    underlineTarget = { b.getX() + 6.0f, b.getBottom() - 2.5f, b.getWidth() - 12.0f, 2.0f };
    if (snap || underline.isEmpty()) underline = underlineTarget;
    repaint (tabBar);
}

void MainView::animate()
{
    // tab underline: ease towards the selected tab
    if (underline != underlineTarget)
    {
        const auto old = underline;
        auto ease = [] (float a, float b) { return std::abs (b - a) < 0.3f ? b : a + (b - a) * 0.24f; };
        underline = { ease (underline.getX(), underlineTarget.getX()), underlineTarget.getY(),
                      ease (underline.getWidth(), underlineTarget.getWidth()), underlineTarget.getHeight() };
        repaint (old.getUnion (underline).expanded (2.0f, 18.0f).getSmallestIntegerContainer());
    }
    // page fade-in
    if (pageVeil.isVisible())
    {
        pageVeil.amount *= 0.72f;
        if (pageVeil.amount < 0.01f) pageVeil.setVisible (false);
        else pageVeil.repaint();
    }
    // a hit: the logo pulses and the chain lights up in processing order
    const unsigned int hits = proc.hitCounter.load (std::memory_order_relaxed);
    if (hits != lastHits)
    {
        lastHits = hits;
        logoPulse = 1.0f;
        chainStrip.flash();
    }
    if (logoPulse > 0.0f)
    {
        logoPulse = logoPulse < 0.02f ? 0.0f : logoPulse * 0.88f;
        repaint (logoArea.withWidth (60).expanded (12));
    }
}

void MainView::mouseUp (const MouseEvent& e)
{
    // the book in the logo has a secret
    if (logoArea.withWidth (44).toFloat().contains (e.position))
        dancer.toggle();
}

void MainView::timerCallback()
{
    animate();
    meterL.setLevel (proc.getAndResetPeak (0));
    meterR.setLevel (proc.getAndResetPeak (1));
    undoButton.setEnabled (proc.undoManager.canUndo());
    redoButton.setEnabled (proc.undoManager.canRedo());
    const float cpu = (float) proc.getCpuLoad();
    cpuShown += 0.1f * (cpu - cpuShown);
    const float clipDb = Decibels::gainToDecibels (1.0f + proc.getClipper().getAndResetClip() * 4.0f);
    clipShown = jmax (clipDb, clipShown - 0.25f);
    const bool clipOn = proc.apvts.getRawParameterValue (ID::clipOn)->load() > 0.5f;
    clipCeiling.setAlpha (clipOn ? 1.0f : 0.4f);
    clipSoftness.setAlpha (clipOn ? 1.0f : 0.4f);
    if (++footerTick % 4 == 0) repaint (footer);
}

void MainView::showMainMenu()
{
    PopupMenu menu, sizes;
    // The tick follows the actual window (after a drag, an auto-fit or a host resize the saved value can differ);
    // sizes larger than the editor's screen are greyed out, but 75 % and the current size always stay available.
    const float current = getWindowScale ? getWindowScale() : 1.0f;
    const float fit = getFitScale ? getFitScale() : 2.0f;
    const float scales[] = { 0.75f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    const int currentWidth = roundToInt (current * (float) baseWidth);
    bool matched = false;
    for (float sc : scales)
        matched |= std::abs (roundToInt (sc * (float) baseWidth) - currentWidth) <= 1;
    if (! matched)
        sizes.addSectionHeader ("Current: " + String (roundToInt (current * 100.0f)) + " %");
    for (int i = 0; i < 7; ++i)
    {
        const bool ticked = std::abs (roundToInt (scales[i] * (float) baseWidth) - currentWidth) <= 1;
        sizes.addItem (100 + i, String (roundToInt (scales[i] * 100.0f)) + " %", ticked || i == 0 || scales[i] <= fit + 0.01f, ticked);
    }
    sizes.addSeparator();
    sizes.addItem (107, "Fit to screen");

    const auto presetName = proc.presets.getCurrentName();
    menu.addSectionHeader ("Presets");
    { PopupMenu::Item it ("Browse presets..."); it.itemID = 9; it.isEnabled = true; it.shortcutKeyDescription = KeyPress ('b', ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons(); menu.addItem (it); }
    { PopupMenu::Item it ("Save preset..."); it.itemID = 1; it.isEnabled = true; it.shortcutKeyDescription = KeyPress ('s', ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons(); menu.addItem (it); }
    menu.addItem (2, "Delete \"" + presetName + "\"", proc.presets.isCurrentUserPreset());
    menu.addItem (3, "Open preset folder");
    menu.addItem (4, "Rescan preset folder");
    menu.addSectionHeader ("Edit");
    { PopupMenu::Item it ("Undo"); it.itemID = 5; it.isEnabled = proc.undoManager.canUndo(); it.shortcutKeyDescription = KeyPress ('z', ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons(); menu.addItem (it); }
    { PopupMenu::Item it ("Redo"); it.itemID = 6; it.isEnabled = proc.undoManager.canRedo(); it.shortcutKeyDescription = KeyPress ('z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 0).getTextDescriptionWithIcons(); menu.addItem (it); }
    menu.addSectionHeader ("View");
    menu.addSubMenu ("Interface size", sizes);
    menu.addItem (7, "Show tooltips", true, tooltipWindow != nullptr);
    menu.addSeparator();
    menu.addItem (10, "Quick guide");
    menu.addItem (8, "About Codex");

    SafePointer<MainView> safe (this);
    const float popupScale = [this] { auto* l = dynamic_cast<CodexLookAndFeel*> (&getLookAndFeel()); return l != nullptr ? l->popupScale() : 1.0f; }();
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&menuButton).withMinimumWidth (roundToInt (220.0f * popupScale)),
        [safe, scales, fit] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& v = *safe;
            switch (r)
            {
                case 1: v.presetBar.onSaveRequested(); break;
                case 9: v.browserOverlay.open(); break;
                case 10: v.guideOverlay.open(); break;
                case 2: v.proc.presets.deleteUserPreset (v.proc.presets.getCurrentName()); v.presetBar.refresh(); break;
                case 3: v.proc.presets.getUserDirectory().startAsProcess(); break;
                case 4: v.proc.presets.refreshUserPresets(); v.presetBar.refresh(); break;
                case 5: v.proc.undoManager.undo(); break;
                case 6: v.proc.undoManager.redo(); break;
                case 7: v.setTooltipsEnabled (v.tooltipWindow == nullptr); break;
                case 8: v.aboutOverlay.open(); break;
                default:
                    if (r >= 100 && r < 107 && v.onScaleRequested) v.onScaleRequested (scales[r - 100]);
                    if (r == 107 && v.onScaleRequested) v.onScaleRequested (fit);
                    break;
            }
        });
}

bool MainView::keyPressed (const KeyPress& k)
{
    // Shortcuts (when the plugin window has keyboard focus; text fields keep their own keys)
    const auto mods = k.getModifiers();
    if (! mods.isCommandDown()) return false;
    const int code = k.getKeyCode();
    if (code == 'Z' || code == 'z')
    {
        if (mods.isShiftDown()) proc.undoManager.redo(); else proc.undoManager.undo();
        return true;
    }
    if (code == 'Y' || code == 'y') { proc.undoManager.redo(); return true; }
    if (code == 'B' || code == 'b') { browserOverlay.open(); return true; }
    if (code == 'S' || code == 's') { presetBar.onSaveRequested(); return true; }
    if (code == KeyPress::rightKey || code == KeyPress::downKey) { proc.presets.next(); presetBar.refresh(); return true; }
    if (code == KeyPress::leftKey || code == KeyPress::upKey) { proc.presets.previous(); presetBar.refresh(); return true; }
    return false;
}

void MainView::switchAB (int slot)
{
    abA.setToggleState (slot == 0, dontSendNotification);
    abB.setToggleState (slot == 1, dontSendNotification);
    if (slot == abCurrent) return;

    // The key belongs to the track, not the sound: keep it when switching
    const float key = proc.apvts.getRawParameterValue (ID::key)->load(), oct = proc.apvts.getRawParameterValue (ID::octave)->load(),
                mode = proc.apvts.getRawParameterValue (ID::keyMode)->load();
    abSlots[(size_t) abCurrent] = proc.apvts.copyState();
    if (! abSlots[(size_t) slot].isValid())
        abSlots[(size_t) slot] = abSlots[(size_t) abCurrent].createCopy();
    proc.apvts.replaceState (abSlots[(size_t) slot].createCopy());
    for (auto [id, v] : { std::pair<String, float> { ID::key, key }, { ID::octave, oct }, { ID::keyMode, mode } })
        if (auto* p = proc.apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v));
    abCurrent = slot;
    presetBar.refresh();
}

void MainView::updateEngineView()
{
    const bool is909 = engineSwitch.getIndex() == 1;
    const bool kickTab = currentTab == 0;

    for (Component* c : std::initializer_list<Component*> { &wave, &clickLevel, &clickDecay, &clickTone })
        c->setVisible (kickTab && ! is909);
    for (Component* c : std::initializer_list<Component*> { &model, &tune909, &attack909, &decay909,
                                                             &depth909, &accent909, &overload909, &hold909, &sweep909 })
        c->setVisible (kickTab && is909);
    // with the 909 engine the pitch panel holds the 909's own sweep and circuit controls
    for (Component* c : std::initializer_list<Component*> { &pitchEnvDisplay, &pitchDrawEditor, &pAmount, &pAttack, &pDecay,
                                                             &pSustain, &pRelease, &pCurve })
        if (is909) c->setVisible (false);

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

    // Envelope modes: ADSR or Draw (breakpoints). In Draw mode only Amount (pitch) and Release (amp) still apply.
    const bool pitchDraw = pitchModeSwitch.getIndex() == 1, ampDraw = ampModeSwitch.getIndex() == 1 && ! is909;
    pitchEnvDisplay.setVisible (kickTab && ! pitchDraw && ! is909);
    pitchDrawEditor.setVisible (kickTab && pitchDraw && ! is909);
    ampEnvDisplay.setVisible (kickTab && ! ampDraw);
    ampDrawEditor.setVisible (kickTab && ampDraw);
    pitchModeSwitch.setVisible (kickTab && ! is909);
    ampModeSwitch.setVisible (kickTab && ! is909);
    auto enable = [] (Component& c, bool on) { c.setEnabled (on); c.setAlpha (on ? 1.0f : Ui::disabledAlpha); };

    // The 909 has its own pitch sweep; the amp ADSR only gates it (attack + release)
    for (Component* c : std::initializer_list<Component*> { &pitchEnvDisplay, &pitchDrawEditor, &pAmount })
        enable (*c, ! is909);
    for (Component* c : std::initializer_list<Component*> { &pAttack, &pDecay, &pSustain, &pRelease, &pCurve })
        enable (*c, ! is909 && ! pitchDraw);
    for (Component* c : std::initializer_list<Component*> { &aDecay, &aSustain, &ampEnvDisplay })
        enable (*c, ! is909 && ! ampDraw);
    enable (aAttack, ! ampDraw);
    // aCurve shapes the release in both modes
    engineNotice.setVisible (false);   // replaced by the 909 sweep display and circuit controls
    repaint();
}

std::vector<Component*> MainView::kickTabComponents()
{
    return { &engineSwitch, &tune, &phase,
             &pitchEnvDisplay, &pAmount, &pAttack, &pDecay, &pSustain, &pRelease, &pCurve,
             &ampEnvDisplay, &aAttack, &aDecay, &aSustain, &aRelease, &aCurve,
             &pitchModeSwitch, &ampModeSwitch,
             &subPower, &subLevel, &subLowPass, &subFade, &subPhase, &subAlign, &velocity,
             &eqDisplay, &bandControls };
}

void MainView::showTab (int tab)
{
    const bool changed = tab != currentTab;
    currentTab = tab;
    setUnderlineTarget (false);
    if (changed)
    {
        pageVeil.amount = 0.85f;   // the new page fades in from the background
        pageVeil.setVisible (true);
        pageVeil.toFront (false);
    }
    chainStrip.setCurrentTab (tab);
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
    // Structural rules are snapped to device pixels (crisp at 75 / 150 %), together with the fills they edge
    const Px::Snap px (g, *this);
    const float hair = px.hairline();

    // Background: black with a faint warm lift in the centre
    const auto headerBar = getLocalBounds().removeFromTop (header.getBottom() + 8).toFloat().withBottom (px.y ((float) header.getBottom() + 8.0f));
    g.fillAll (Colours_::background);
    g.setGradientFill (ColourGradient (Colour (0xff0d0b09), (float) getWidth() * 0.5f, (float) getHeight() * 0.45f,
                                       Colours_::background, 0.0f, (float) getHeight() * 1.1f, true));
    g.fillRect (getLocalBounds().toFloat().withTop (headerBar.getBottom()));

    // Header bar: brushed-dark gradient with a sun-orange hairline that fades out at both ends
    g.setGradientFill (ColourGradient (Colour (0xff161514), 0, headerBar.getY(), Colour (0xff0c0c0c), 0, headerBar.getBottom(), false));
    g.fillRect (headerBar);
    g.setColour (Colours::black);
    g.fillRect (Rectangle<float> (0.0f, headerBar.getBottom() - hair, (float) getWidth(), hair));
    {
        ColourGradient line (Colours_::accent.withAlpha (0.0f), 0, 0, Colours_::accent.withAlpha (0.0f), (float) getWidth(), 0, false);
        line.addColour (0.25, Colours_::accent.withAlpha (0.55f));
        line.addColour (0.75, Colours_::accent.withAlpha (0.55f));
        g.setGradientFill (line);
        g.fillRect (Rectangle<float> (0.0f, headerBar.getBottom() - hair, (float) getWidth(), hair));
        ColourGradient glow (Colours_::accent.withAlpha (0.07f), (float) getWidth() * 0.5f, headerBar.getBottom(),
                             Colours_::accent.withAlpha (0.0f), (float) getWidth() * 0.5f, headerBar.getBottom() + 10.0f, false);
        g.setGradientFill (glow);
        g.fillRect (Rectangle<float> (0.0f, headerBar.getBottom(), (float) getWidth(), 10.0f));
    }

    // Logo: mark + wordmark + subtitle
    {
        auto r = logoArea.toFloat();
        auto mark = r.removeFromLeft (40.0f).withSizeKeepingCentre (38.0f, 38.0f);
        if (logoPulse > 0.0f)
        {
            // a soft glow behind the mark on every hit
            g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (0.35f * logoPulse), mark.getCentreX(), mark.getCentreY(),
                                               Colours_::accent.withAlpha (0.0f), mark.getCentreX() + 30.0f, mark.getCentreY(), true));
            g.fillEllipse (mark.expanded (10.0f));
        }
        Icons::draw (g, Icons::kickMark(), mark.reduced (2.0f), Colours_::accent.interpolatedWith (Colours_::accentHi, logoPulse), 1.8f + 0.6f * logoPulse);
        r.removeFromLeft (10.0f);
        g.setColour (Colours_::text);
        g.setFont (Fonts::display (28.0f, 0.22f));
        g.drawText ("CODEX", r.removeFromTop (r.getHeight() * 0.64f), Justification::bottomLeft);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (Fonts::Size::micro, 0.16f));
        g.drawText ("NOCTURNAL AUDIO", r.translated (1.0f, 0.0f), Justification::topLeft);
    }

    g.setColour (Colours_::textDim);
    g.setFont (Fonts::label (Fonts::Size::caption, 0.12f));
    g.drawText ("PRESET", presetArea.withHeight (16).translated (1, 0), Justification::centredLeft);

    // Footer
    {
        g.setColour (Colours_::outline);
        px.hline ((float) footer.getY(), (float) footer.getX(), (float) footer.getRight());
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (Fonts::Size::caption, 0.1f));
        g.drawText ("MACROS", footer.withTrimmedTop (4).withWidth (72), Justification::centredLeft);
        auto cpuArea = footer.withTrimmedTop (4).removeFromRight (76);
        // clip activity: a small bar under the CLIP button
        if (clipShown > 0.05f && proc.apvts.getRawParameterValue (ID::clipOn)->load() > 0.5f)
        {
            auto bar = clipButton.getBounds().toFloat().withY ((float) clipButton.getBottom() + 3.0f).withHeight (3.0f);
            g.setColour (Colours_::warn);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * jlimit (0.05f, 1.0f, clipShown / 6.0f)), 1.5f);
        }
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (Fonts::Size::caption, 0.1f));
        g.drawText ("A / B", abA.getBounds().translated (-48, 0).withWidth (44), Justification::centredRight);
        g.setFont (Fonts::medium (12.0f));
        g.setColour (cpuShown > 0.7f ? Colours_::warn : Colours_::textDim);
        g.drawText ("CPU " + String (roundToInt (cpuShown * 100.0f)) + " %", cpuArea, Justification::centredRight);
    }

    // Tab bar, with the sliding underline of the selected tab (glow rising from it)
    g.setColour (Colours_::outline);
    px.hline ((float) tabBar.getBottom() - 1.0f, (float) tabBar.getX(), (float) tabBar.getRight());
    if (! underline.isEmpty())
    {
        g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (0.16f), underline.getCentreX(), underline.getY(),
                                           Colours_::accent.withAlpha (0.0f), underline.getCentreX(), underline.getY() - 14.0f, false));
        g.fillRect (underline.withY (underline.getY() - 14.0f).withHeight (14.0f));
        g.setColour (Colours_::accent);
        g.fillRoundedRectangle (underline, 1.0f);
    }
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
            g.setColour (hi ? Colours_::accent : Colours_::textMuted);
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
        drawSectionPanel (g, *this, sourceArea, "SOURCE", Colours_::accent);
        drawSectionPanel (g, *this, pitchArea, engineSwitch.getIndex() == 1 ? "909 PITCH SWEEP  /  CIRCUIT" : "PITCH ENVELOPE", Colours_::accent2);
        drawSectionPanel (g, *this, ampArea, engineSwitch.getIndex() == 1 ? "AMP GATE" : "AMP ENVELOPE", Colours_::accent);
        drawSectionPanel (g, *this, eqArea, "EQUALIZER", Colours_::accent);
        const bool subOn = proc.apvts.getRawParameterValue (ID::subOn)->load() > 0.5f;
        drawSectionPanel (g, *this, subArea, "SUB LAYER", subOn ? Colours_::good : Colours_::textFaint);
        drawGroupCaption (g, *this, velocityCaption, "DYNAMICS");
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

    // ---- Footer: macros | A/B | CPU ----
    footer = r.removeFromBottom (40);
    r.removeFromBottom (10);
    {
        auto f = footer.withTrimmedTop (4);
        f.removeFromLeft (70);   // "MACROS" caption
        for (auto& k : macroKnobs)
        {
            k.setBounds (f.removeFromLeft (104).withHeight (36));
            f.removeFromLeft (2);
        }
        f.removeFromLeft (10);
        clipButton.setBounds (f.removeFromLeft (48).withSizeKeepingCentre (48, 24));
        f.removeFromLeft (6);
        clipCeiling.setBounds (f.removeFromLeft (102).withHeight (36));
        clipSoftness.setBounds (f.removeFromLeft (86).withHeight (36));
        auto right = f.removeFromRight (264);
        exportButton.setBounds (f.withSizeKeepingCentre (jmin (f.getWidth() - 8, 116), 26));
        right.removeFromRight (78);   // CPU readout
        abCopy.setBounds (right.removeFromRight (58).withSizeKeepingCentre (58, 24));
        right.removeFromRight (8);
        abB.setBounds (right.removeFromRight (36).withSizeKeepingCentre (36, 24));
        abA.setBounds (right.removeFromRight (36).withSizeKeepingCentre (36, 24));
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
        // signal chain (drag to reorder) between the tabs and the tuner
        breadcrumbArea = {};
        t.removeFromRight (16);
        previewButton.setBounds (t.removeFromRight (30).withSizeKeepingCentre (30, 26).translated (0, -2));
        t.removeFromRight (8);
        tunerChip.setBounds (t.removeFromRight (150).withSizeKeepingCentre (150, 26).translated (0, -2));
        t.removeFromRight (14);
        t.removeFromLeft (8);
        chainStrip.setBounds (t.withTrimmedBottom (2));
        setUnderlineTarget (true);
        pageVeil.setBounds (getLocalBounds().withTop (tabBar.getBottom()).withBottom (footer.getY()));
        dancer.setBounds (Rectangle<int> (170, 230).withPosition (getWidth() - 12 - 180, footer.getY() - 236));
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
    subArea = r.removeFromRight (206);
    r.removeFromRight (12);
    eqArea = r;
    {
        subPower.setBounds (subArea.withHeight (28).removeFromRight (32).reduced (2));
        auto c = subArea.reduced (10).withTrimmedTop (30);
        auto velRow = c.removeFromBottom (100);
        velocityCaption = c.removeFromBottom (18);
        velocity.setBounds (velRow.withSizeKeepingCentre (96, velRow.getHeight()));
        const int rowGap = 14;   // keeps the first row's values clear of the second row's captions
        const int rowH = jmin (110, (c.getHeight() - rowGap) / 2);
        auto row1 = c.removeFromTop (rowH);
        c.removeFromTop (rowGap);
        auto row2 = c.removeFromTop (rowH);
        subLevel.setBounds (row1.removeFromLeft (row1.getWidth() / 2)); subLowPass.setBounds (row1);
        subFade.setBounds (row2.removeFromLeft (row2.getWidth() / 2)); subPhase.setBounds (row2);
        subAlign.setBounds (subPhase.getBounds().withY (subPhase.getBottom() + 2).withHeight (22).withSizeKeepingCentre (64, 22));
    }

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
        // 909 circuit controls in the pitch panel: the sweep display over the envelope display, four cells below
        sweep909.setBounds (pitchEnvDisplay.getBounds());
        auto row = pAmount.getBounds().getUnion (pCurve.getBounds());
        const int w = row.getWidth() / 4;
        depth909.setBounds (row.removeFromLeft (w));
        accent909.setBounds (row.removeFromLeft (w));
        overload909.setBounds (row.removeFromLeft (w));
        hold909.setBounds (row.withSizeKeepingCentre (jmin (row.getWidth() - 12, 118), 30));
    }
    pitchDrawEditor.setBounds (pitchEnvDisplay.getBounds());
    ampDrawEditor.setBounds (ampEnvDisplay.getBounds());
    pitchModeSwitch.setBounds (pitchArea.withHeight (28).removeFromRight (116).reduced (5, 4));
    ampModeSwitch.setBounds (ampArea.withHeight (28).removeFromRight (116).reduced (5, 4));

    {
        auto c = eqArea.reduced (10).withTrimmedTop (30);
        bandControls.setBounds (c.removeFromBottom (84));
        c.removeFromBottom (8);
        eqDisplay.setBounds (c);
    }

    saveOverlay.setBounds (getLocalBounds());
    browserOverlay.setBounds (getLocalBounds());
    guideOverlay.setBounds (getLocalBounds());
    aboutOverlay.setBounds (getLocalBounds());
}

//==============================================================================
CodexEditor::CodexEditor (CodexProcessor& p)
    : AudioProcessorEditor (p), view (p, settings->user())
{
    setLookAndFeel (&lnf);
    setOpaque (true);
    addAndMakeVisible (view);
    view.onScaleRequested = [this] (float s) { applyScale (s); };
    view.getWindowScale = [this] { return (float) getWidth() / (float) MainView::baseWidth; };
    view.getFitScale = [this] { return maxScaleForScreen(); };

    // Read the saved size first: setting the resize limits below triggers resized() calls that must not overwrite it.
    // The saved size is the user's preference: it is clamped to the current screen when applied, but not rewritten,
    // so a 200 % chosen on a large monitor comes back there.
    float scale = 1.0f;
    auto* s = settings->user();
    if (s != nullptr && s->containsKey (scaleKey))
        scale = jlimit (minScale, maxScale, (float) s->getDoubleValue (scaleKey, 1.0));
    else if (auto* display = Desktop::getInstance().getDisplays().getPrimaryDisplay())
        scale = jlimit (minScale, 1.0f, 0.85f * (float) display->userArea.getHeight() / (float) MainView::baseHeight);   // fit smaller screens
    scale = jmin (scale, maxScaleForScreen());

    const auto w = MainView::baseWidth, h = MainView::baseHeight;
    setResizable (true, true);
    setResizeLimits (roundToInt ((float) w * minScale), roundToInt ((float) h * minScale), roundToInt ((float) w * maxScale), roundToInt ((float) h * maxScale));
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    applyScale (scale);
    initialised = true;
}

CodexEditor::~CodexEditor()
{
    setLookAndFeel (nullptr);
}

float CodexEditor::maxScaleForScreen() const
{
    const auto& displays = Desktop::getInstance().getDisplays();
    const Displays::Display* d = nullptr;
    if (getPeer() != nullptr)
        d = displays.getDisplayForRect (getScreenBounds());
    if (d == nullptr)
        d = displays.getDisplayForPoint (Desktop::getMousePosition());
    if (d == nullptr)
        d = displays.getPrimaryDisplay();
    if (d == nullptr || d->userArea.getHeight() < 200)
        return maxScale;   // no usable screen information (headless)
    const auto ua = d->userArea.toFloat();
    // leave room for the host's title bar and toolbar
    return jlimit (0.6f, maxScale, jmin (0.95f * ua.getWidth() / (float) MainView::baseWidth,
                                         (ua.getHeight() - 60.0f) / (float) MainView::baseHeight));
}

void CodexEditor::applyScale (float scale)
{
    // 0.6 is reachable only through the screen fit on very small screens; the menu and the resize corner stop at 75 %
    scale = jlimit (0.6f, maxScale, scale);
    setSize (roundToInt ((float) MainView::baseWidth * scale), roundToInt ((float) MainView::baseHeight * scale));
}

void CodexEditor::parentHierarchyChanged()
{
    AudioProcessorEditor::parentHierarchyChanged();
    // Once, when the editor first lands on a screen: shrink (never grow) if the host opened it on a smaller display than
    // the one the constructor measured. Not saved: the stored preference stays as it was.
    if (screenChecked || getPeer() == nullptr)
        return;
    screenChecked = true;
    const float fit = maxScaleForScreen();
    if ((float) getWidth() > fit * (float) MainView::baseWidth + 1.0f)
    {
        const ScopedValueSetter<bool> noSave (initialised, false);
        applyScale (fit);
    }
}

void CodexEditor::paint (Graphics& g)
{
    // covers the sub-pixel strip the uniformly scaled view can leave at the bottom edge
    g.fillAll (Colours_::background);
}

void CodexEditor::resized()
{
    const float scale = (float) getWidth() / (float) MainView::baseWidth;
    lnf.uiScale = scale;   // first, so popups and tooltips opened from now on use it
    view.setTransform (AffineTransform::scale (scale));
    view.setBounds (0, 0, MainView::baseWidth, MainView::baseHeight);
    if (initialised)
        if (auto* s = settings->user())
            s->setValue (scaleKey, scale);
}
