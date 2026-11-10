#pragma once
#include "Widgets.h"
#include "../PresetManager.h"

/** Preset browser: previous / list / next / save. */
class PresetBar : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar (PresetManager& pm) : presets (pm)
    {
        for (auto* b : { &prev, &next, &save })
        {
            b->setColour (juce::TextButton::textColourOffId, Colours_::text);
            addAndMakeVisible (b);
        }
        prev.onClick = [this] { presets.previous(); refresh(); };
        next.onClick = [this] { presets.next(); refresh(); };
        save.onClick = [this] { askForName(); };

        box.setTextWhenNothingSelected ("Init");
        box.onChange = [this]
        {
            if (const int id = box.getSelectedId(); id > 0)
                presets.loadPreset (id - 1); // selecting the current preset again reverts your edits
        };
        addAndMakeVisible (box);
        refresh();
        startTimerHz (4);
    }

    void refresh()
    {
        box.clear (juce::dontSendNotification);
        int id = 1;
        box.addSectionHeading ("FACTORY");
        for (const auto& n : presets.getFactoryNames()) box.addItem (n, id++);
        const auto users = presets.getUserNames();
        if (! users.isEmpty())
        {
            box.addSectionHeading ("USER");
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
        save.setBounds (r.removeFromRight (56));
        r.removeFromRight (6);
        next.setBounds (r.removeFromRight (28));
        prev.setBounds (r.removeFromLeft (28));
        box.setBounds (r.reduced (4, 0));
    }

private:
    void timerCallback() override
    {
        if (presets.getCurrentName() != lastName) refresh();   // e.g. host program change
    }

    void askForName()
    {
        auto* w = new juce::AlertWindow ("Save Preset", "Preset name:", juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", presets.getCurrentName());
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PresetBar> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
        {
            if (safe == nullptr || result != 1) return;
            safe->presets.saveUserPreset (w->getTextEditorContents ("name"));
            safe->refresh();
        }), true);
    }

    PresetManager& presets;
    juce::TextButton prev { "<" }, next { ">" }, save { "SAVE" };
    juce::ComboBox box;
    juce::String lastName;
};

/** Row of mutually exclusive buttons bound to a choice parameter (e.g. SYNTH | 909). */
class SegmentedChoice : public juce::Component
{
public:
    SegmentedChoice (juce::RangedAudioParameter& p, const juce::StringArray& labels)
        : attachment (p, [this] (float v) { setIndex (juce::roundToInt (v)); })
    {
        for (int i = 0; i < labels.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (labels[i]));
            b->setColour (juce::TextButton::textColourOffId, Colours_::textDim);
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
