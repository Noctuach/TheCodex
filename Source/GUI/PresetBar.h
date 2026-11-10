#pragma once
#include "Widgets.h"
#include "../PresetManager.h"

/** Preset browser: previous / list / next / save. */
class PresetBar : public juce::Component, private juce::Timer
{
public:
    std::function<void()> onSaveRequested;

    explicit PresetBar (PresetManager& pm) : presets (pm)
    {
        prev.setTooltip ("Previous preset");
        next.setTooltip ("Next preset");
        save.setTooltip ("Save preset: stores the current sound as a user preset");
        box.setTooltip ("Presets: factory presets first, then your own. Selecting the current preset again reverts your edits.");
        prev.onClick = [this] { presets.previous(); refresh(); };
        next.onClick = [this] { presets.next(); refresh(); };
        save.onClick = [this] { if (onSaveRequested) onSaveRequested(); };

        box.setTextWhenNothingSelected ("Init");
        box.onChange = [this]
        {
            if (const int id = box.getSelectedId(); id > 0)
                presets.loadPreset (id - 1);
        };
        for (auto* c : std::initializer_list<juce::Component*> { &prev, &next, &save, &box })
            addAndMakeVisible (c);
        refresh();
        startTimerHz (4);
    }

    void refresh()
    {
        box.clear (juce::dontSendNotification);
        int id = 1;
        box.addSectionHeading ("Factory");
        for (const auto& n : presets.getFactoryNames()) box.addItem (n, id++);
        const auto users = presets.getUserNames();
        if (! users.isEmpty())
        {
            box.addSectionHeading ("User");
            for (const auto& n : users) box.addItem (n, id++);
        }
        lastName = presets.getCurrentName();
        const int idx = presets.getCurrentIndex();
        if (idx >= 0) box.setSelectedId (idx + 1, juce::dontSendNotification);
        else          box.setText (lastName, juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        save.setBounds (r.removeFromRight (r.getHeight()));
        r.removeFromRight (6);
        next.setBounds (r.removeFromRight (r.getHeight()));
        r.removeFromRight (4);
        prev.setBounds (r.removeFromLeft (r.getHeight()));
        r.removeFromLeft (4);
        box.setBounds (r);
    }

private:
    void timerCallback() override
    {
        if (presets.getCurrentName() != lastName) refresh();   // host program change, undo, ...
    }

    PresetManager& presets;
    IconButton prev { "prev", Icons::chevronLeft() }, next { "next", Icons::chevronRight() }, save { "save", Icons::save() };
    juce::ComboBox box;
    juce::String lastName;
};

/** Row of mutually exclusive buttons bound to a choice parameter (e.g. SYNTH | 909). */
class SegmentedChoice : public juce::Component
{
public:
    SegmentedChoice (juce::RangedAudioParameter& p, const juce::StringArray& labels, const juce::String& tooltip)
        : attachment (p, [this] (float v) { setIndex (juce::roundToInt (v)); })
    {
        for (int i = 0; i < labels.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (labels[i]));
            b->setComponentID ("seg");
            b->setTooltip (tooltip);
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < labels.size() - 1) edges |= juce::Button::ConnectedOnRight;
            b->setConnectedEdges (edges);
            b->onClick = [this, i] { attachment.setValueAsCompleteGesture ((float) i); };
            addAndMakeVisible (b);
        }
        attachment.sendInitialUpdate();
    }

    std::function<void (int)> onChange;
    int getIndex() const { return index; }

    void resized() override
    {
        auto r = getLocalBounds();
        const int w = r.getWidth() / juce::jmax (1, buttons.size());
        for (auto* b : buttons) b->setBounds (r.removeFromLeft (w));
    }

private:
    void setIndex (int i)
    {
        index = i;
        for (int b = 0; b < buttons.size(); ++b)
            buttons[b]->setToggleState (b == i, juce::dontSendNotification);
        if (onChange) onChange (i);
    }

    juce::OwnedArray<juce::TextButton> buttons;
    juce::ParameterAttachment attachment;
    int index = 0;
};
