#pragma once
#include "Widgets.h"
#include "../DSP/Chain.h"

/**
    Signal-flow strip in the tab bar: one chip per module in processing order. Drag a chip to move the module,
    click it to open its page, right-click for the Modern / Classic orders. Modules that are switched off are dimmed.
*/
class ChainStrip : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    std::function<void (int module)> onModuleClicked;

    explicit ChainStrip (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        param = apvts.getParameter (ID::chainOrder);
        onParams = { apvts.getRawParameterValue (ID::spc ("On")), apvts.getRawParameterValue (ID::clip (1, "On")),
                     apvts.getRawParameterValue (ID::eqOn (0)), apvts.getRawParameterValue (ID::clip (2, "On")),
                     apvts.getRawParameterValue (ID::sat ("On")), apvts.getRawParameterValue (ID::cmp ("On")),
                     apvts.getRawParameterValue (ID::eqOn (1)) };
        setTooltip ("Signal chain: drag a module to change the processing order, click it to open it. "
                    "Right-click for the Modern (reverb first) and Classic (reverb last) orders.");
        startTimerHz (15);
    }

    /** Tab shown by the editor, so the modules on it are highlighted. */
    void setCurrentTab (int tab) { currentTab = tab; repaint(); }

    static int tabFor (int module)
    {
        switch (module)
        {
            case chain::Space: return 5;
            case chain::Dist1: case chain::Dist2: return 1;
            case chain::Eq1: return 0;
            case chain::Saturation: return 2;
            case chain::Compressor: return 3;
            case chain::Eq2: return 4;
            default: return 0;
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        layoutChips();
        g.setFont (Fonts::label (10.5f, 0.1f));
        for (int i = 0; i < chain::numModules; ++i)
        {
            const int module = shownOrder[(size_t) i];
            auto r = chips[(size_t) i];
            if (module == dragModule)
                continue;   // drawn last, under the mouse
            if (i > 0)
                Icons::draw (g, Icons::chevronRight(), Rectangle<float> ((float) r.getX() - gap + 1.0f, r.getCentreY() - 4.0f, 7.0f, 8.0f),
                             Colours_::textFaint, 1.2f);
            drawChip (g, r, module);
        }
        if (dragModule >= 0)
        {
            const auto r = chips[(size_t) dragSlot].withX (jlimit (0.0f, (float) getWidth() - chips[(size_t) dragSlot].getWidth(),
                                                                    dragX - dragGrab));
            g.setColour (Colours_::accent.withAlpha (0.25f));
            g.fillRoundedRectangle (chips[(size_t) dragSlot].reduced (1.0f), 4.0f);
            drawChip (g, r.translated (0.0f, -1.0f), dragModule, true);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showMenu(); return; }
        pressedSlot = slotAt (e.position.x);
        dragModule = -1;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressedSlot < 0 || e.mods.isPopupMenu()) return;
        if (dragModule < 0)
        {
            if (e.getDistanceFromDragStart() < 4) return;
            dragModule = shownOrder[(size_t) pressedSlot];
            dragGrab = e.mouseDownPosition.x - chips[(size_t) pressedSlot].getX();
        }
        dragX = e.position.x;
        // move the module to the slot under the dragged chip's centre
        auto order = shownOrder;
        const int from = (int) (std::find (order.begin(), order.end(), dragModule) - order.begin());
        int to = 0;
        const float centre = dragX - dragGrab + chips[(size_t) from].getWidth() * 0.5f;
        for (int i = 0; i < chain::numModules; ++i)
            if (centre > chips[(size_t) i].getCentreX()) to = i;
        if (to != from)
        {
            const int m = order[(size_t) from];
            if (to > from) std::rotate (order.begin() + from, order.begin() + from + 1, order.begin() + to + 1);
            else           std::rotate (order.begin() + to, order.begin() + from, order.begin() + from + 1);
            order[(size_t) to] = m;
            shownOrder = order;
        }
        dragSlot = (int) (std::find (shownOrder.begin(), shownOrder.end(), dragModule) - shownOrder.begin());
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (dragModule >= 0)
            setOrder (shownOrder);
        else if (pressedSlot >= 0 && onModuleClicked)
            onModuleClicked (shownOrder[(size_t) pressedSlot]);
        dragModule = -1;
        pressedSlot = -1;
        repaint();
    }

private:
    static constexpr float gap = 9.0f;

    void timerCallback() override
    {
        if (dragModule >= 0) return;
        const int index = currentIndex();
        std::array<bool, chain::numModules> on {};
        for (int i = 0; i < chain::numModules; ++i) on[(size_t) i] = onParams[(size_t) i] != nullptr && onParams[(size_t) i]->load() > 0.5f;
        if (index != shownIndex || on != shownOn)
        {
            shownIndex = index;
            shownOrder = chain::decode (index);
            shownOn = on;
            repaint();
        }
    }

    int currentIndex() const { return juce::roundToInt (param->convertFrom0to1 (param->getValue())); }

    void setOrder (const chain::Order& o)
    {
        const int index = chain::encode (o);
        if (index == currentIndex()) return;
        if (auto* um = apvts.undoManager) um->beginNewTransaction ("Change module order");
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
        param->endChangeGesture();
        shownIndex = index;
        shownOrder = o;
    }

    void showMenu()
    {
        juce::PopupMenu m;
        const int index = currentIndex();
        m.addSectionHeader ("Module order");
        m.addItem (1, "Modern (Space first)", true, index == chain::modernIndex());
        m.addItem (2, "Classic (Space last)", true, index == chain::classicIndex());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r)
        {
            if (r == 1) setOrder (chain::modernOrder);
            if (r == 2) setOrder (chain::classicOrder);
            repaint();
        });
    }

    void layoutChips()
    {
        const auto font = juce::Font (Fonts::label (10.5f, 0.1f));
        std::array<float, chain::numModules> w {};
        float total = 0.0f;
        for (int i = 0; i < chain::numModules; ++i)
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (font, chain::shortName (shownOrder[(size_t) i]), 0, 0);
            w[(size_t) i] = ga.getBoundingBox (0, -1, true).getWidth() + 14.0f;
            total += w[(size_t) i];
        }
        total += gap * (chain::numModules - 1);
        const float scale = total > (float) getWidth() ? (float) getWidth() / total : 1.0f;
        float x = (float) getWidth() - total * scale;   // right-aligned, next to the tuner
        const float h = juce::jmin (22.0f, (float) getHeight() - 4.0f);
        for (int i = 0; i < chain::numModules; ++i)
        {
            chips[(size_t) i] = { x, ((float) getHeight() - h) * 0.5f, w[(size_t) i] * scale, h };
            x += (w[(size_t) i] + gap) * scale;
        }
    }

    int slotAt (float x) const
    {
        for (int i = 0; i < chain::numModules; ++i)
            if (x >= chips[(size_t) i].getX() - gap * 0.5f && x < chips[(size_t) i].getRight() + gap * 0.5f) return i;
        return -1;
    }

    void drawChip (juce::Graphics& g, juce::Rectangle<float> r, int module, bool lifted = false)
    {
        using namespace juce;
        const bool on = shownOn[(size_t) module];
        const bool here = ChainStrip::tabFor (module) == currentTab;
        g.setColour (lifted ? Colours_::panelTop.brighter (0.15f) : Colours_::display);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (lifted ? Colours_::accent : here ? Colours_::accent.withAlpha (0.7f) : Colours_::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (here ? Colours_::accent : on ? Colours_::text : Colours_::textFaint);
        g.drawText (chain::shortName (module), r, Justification::centred);
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::RangedAudioParameter* param = nullptr;
    std::array<std::atomic<float>*, chain::numModules> onParams {};
    std::array<juce::Rectangle<float>, chain::numModules> chips;
    chain::Order shownOrder = chain::modernOrder;
    std::array<bool, chain::numModules> shownOn {};
    int shownIndex = -1, currentTab = 0, pressedSlot = -1, dragModule = -1, dragSlot = 0;
    float dragX = 0.0f, dragGrab = 0.0f;
};
