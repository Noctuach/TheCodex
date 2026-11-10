#pragma once
#include "Overlays.h"
#include "../PresetManager.h"

/**
    Full preset browser: search, categories, favourites. Click a preset to audition it, double-click or press
    Enter to load it and close. Favourites are stored in the user's Codex settings.
*/
class PresetBrowserOverlay : public Overlay, private juce::ListBoxModel, private juce::TextEditor::Listener
{
public:
    PresetBrowserOverlay (PresetManager& pm, juce::PropertiesFile* settingsFile) : presets (pm), settings (settingsFile)
    {
        cardSize = { 820, 590 };
        search.setTextToShowWhenEmpty ("Search presets...", Colours_::textFaint);
        search.setFont (juce::Font (Fonts::regular (15.0f)));
        search.setIndents (12, 8);
        search.addListener (this);
        addAndMakeVisible (search);

        const juce::StringArray filters = juce::StringArray { "All", "Favourites", "User" };
        auto cats = filters;
        cats.addArray (PresetManager::getCategories());
        for (int i = 0; i < cats.size(); ++i)
        {
            auto* b = filterButtons.add (new juce::TextButton (cats[i].toUpperCase()));
            b->setComponentID ("pill");
            b->setClickingTogglesState (false);
            b->onClick = [this, i] { setFilter (i); };
            addAndMakeVisible (b);
        }
        filterNames = cats;

        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, Colours_::display);
        list.setColour (juce::ListBox::outlineColourId, Colours_::outline);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        closeButton.onClick = [this] { close(); };
        addAndMakeVisible (closeButton);
        loadFavourites();
        setFilter (0);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey) { loadSelected (true); return true; }
        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            const int row = juce::jlimit (0, juce::jmax (0, (int) rows.size() - 1), list.getSelectedRow() + (k == juce::KeyPress::upKey ? -1 : 1));
            list.selectRow (row);
            loadSelected (false);
            return true;
        }
        return Overlay::keyPressed (k);
    }

protected:
    void opened() override
    {
        presets.refreshUserPresets();
        rebuild();
        search.grabKeyboardFocus();
    }

    void paintCard (juce::Graphics& g, juce::Rectangle<int> c) override
    {
        auto r = c.reduced (24, 18);
        g.setColour (Colours_::text);
        g.setFont (Fonts::display (20.0f, 0.18f));
        g.drawText ("PRESETS", r.removeFromTop (30), juce::Justification::centredLeft);
        g.setColour (Colours_::textFaint);
        g.setFont (Fonts::regular (12.5f));
        g.drawText (juce::String ((int) rows.size()) + " presets  \xc2\xb7  click to audition, double-click or Enter to load, star to favourite",
                    c.reduced (24, 14).removeFromBottom (18), juce::Justification::centredLeft);
    }

    void layoutCard (juce::Rectangle<int> c) override
    {
        auto r = c.reduced (24, 18);
        auto top = r.removeFromTop (30);
        closeButton.setBounds (top.removeFromRight (30));
        r.removeFromTop (12);
        search.setBounds (r.removeFromTop (36));
        r.removeFromTop (12);
        r.removeFromBottom (28);
        auto left = r.removeFromLeft (170);
        for (auto* b : filterButtons)
        {
            b->setBounds (left.removeFromTop (30));
            left.removeFromTop (6);
        }
        r.removeFromLeft (14);
        list.setBounds (r);
    }

private:
    struct Row { int index; juce::String name, category; bool user; };

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (rowNumber < 0 || rowNumber >= (int) rows.size()) return;
        const auto& row = rows[(size_t) rowNumber];
        const bool current = row.index == presets.getCurrentIndex();
        if (selected || current)
        {
            g.setColour (Colours_::accent.withAlpha (selected ? 0.16f : 0.08f));
            g.fillRect (0, 0, w, h);
        }
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (12, 0);
        const bool fav = favourites.contains (row.name);
        Icons::draw (g, Icons::star(), r.removeFromLeft (20).toFloat().withSizeKeepingCentre (16.0f, 16.0f),
                     fav ? juce::Colour (0xffffc94d) : Colours_::textFaint, 1.3f);
        r.removeFromLeft (10);
        g.setColour (current ? Colours_::accentHi : Colours_::text);
        g.setFont (Fonts::medium (14.5f));
        g.drawText (row.name, r, juce::Justification::centredLeft);
        g.setColour (Colours_::textDim);
        g.setFont (Fonts::label (11.5f, 0.08f));
        g.drawText ((row.user ? juce::String ("USER") : row.category.toUpperCase()), r, juce::Justification::centredRight);
        g.setColour (Colours_::outline.withAlpha (0.6f));
        g.drawHorizontalLine (h - 1, 0.0f, (float) w);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        if (e.x < 44) { toggleFavourite (rows[(size_t) row].name); return; }
        loadSelected (false);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { if (row >= 0) loadSelected (true); }
    void returnKeyPressed (int) override { loadSelected (true); }
    void textEditorTextChanged (juce::TextEditor&) override { rebuild(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { if (! rows.empty()) { list.selectRow (0); loadSelected (true); } }

    void loadSelected (bool andClose)
    {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= (int) rows.size()) return;
        presets.loadPreset (rows[(size_t) row].index);
        list.repaint();
        if (andClose) close();
    }

    void setFilter (int f)
    {
        filter = f;
        for (int i = 0; i < filterButtons.size(); ++i) filterButtons[i]->setToggleState (i == f, juce::dontSendNotification);
        rebuild();
    }

    void rebuild()
    {
        rows.clear();
        const auto text = search.getText().trim();
        const auto factoryNames = presets.getFactoryNames();
        const auto users = presets.getUserNames();
        auto consider = [&] (const Row& row)
        {
            if (text.isNotEmpty() && ! row.name.containsIgnoreCase (text) && ! row.category.containsIgnoreCase (text)) return;
            const auto& fname = filterNames[filter];
            if (filter == 1 && ! favourites.contains (row.name)) return;
            if (filter == 2 && ! row.user) return;
            if (filter >= 3 && (row.user || row.category != fname)) return;
            rows.push_back (row);
        };
        for (int i = 0; i < factoryNames.size(); ++i) consider ({ i, factoryNames[i], presets.getFactoryCategory (i), false });
        for (int i = 0; i < users.size(); ++i) consider ({ factoryNames.size() + i, users[i], "User", true });
        list.updateContent();
        list.repaint();
        int sel = -1;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].index == presets.getCurrentIndex()) sel = (int) i;
        if (sel >= 0) { list.selectRow (sel, false, true); list.scrollToEnsureRowIsOnscreen (sel); }
        repaint();
    }

    void loadFavourites()
    {
        if (settings != nullptr) favourites.addTokens (settings->getValue ("favouritePresets"), "\n", "");
        favourites.removeEmptyStrings();
    }
    void toggleFavourite (const juce::String& name)
    {
        if (favourites.contains (name)) favourites.removeString (name); else favourites.add (name);
        if (settings != nullptr) { settings->setValue ("favouritePresets", favourites.joinIntoString ("\n")); settings->saveIfNeeded(); }
        if (filter == 1) rebuild(); else list.repaint();
    }

    PresetManager& presets;
    juce::PropertiesFile* settings;
    juce::TextEditor search;
    juce::OwnedArray<juce::TextButton> filterButtons;
    juce::StringArray filterNames, favourites;
    juce::ListBox list;
    IconButton closeButton { "close", Icons::close(), false };
    std::vector<Row> rows;
    int filter = 0;
};
