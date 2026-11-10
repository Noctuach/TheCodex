#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

/**
    Factory presets (compiled in) + user presets (XML files in the user's documents folder).
    Key, octave and key mode are deliberately NOT part of a preset: pick your track's key
    once and browse presets without losing it.
*/
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState&);

    int getNumFactory() const { return (int) factory.size(); }
    juce::StringArray getFactoryNames() const;
    /** Style category of a factory preset (Hardstyle, Rawstyle, Euphoric, ...). */
    juce::String getFactoryCategory (int index) const;
    static juce::StringArray getCategories();
    juce::StringArray getUserNames() const;

    /** Combined list: factory presets first, then user presets. */
    int getNumPresets() const { return getNumFactory() + getUserNames().size(); }
    void loadPreset (int combinedIndex);
    int getCurrentIndex() const;
    juce::String getCurrentName() const;
    void next()     { step (1); }
    void previous() { step (-1); }

    bool saveUserPreset (const juce::String& name);
    bool deleteUserPreset (const juce::String& name);
    bool userPresetExists (const juce::String& name) const { return getUserNames().contains (name); }
    bool isCurrentUserPreset() const { return getUserNames().contains (getCurrentName()); }
    void refreshUserPresets();
    juce::File getUserDirectory() const;

    static const juce::Identifier presetNameProperty;

private:
    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;
    };

    void step (int delta);
    void loadFactory (int index);
    void loadUser (const juce::String& name);
    void resetToDefaults();
    void setValue (const juce::String& id, float value);
    void setCurrentName (const juce::String& name);
    bool isKeyParameter (const juce::String& id) const;

    juce::AudioProcessorValueTreeState& apvts;
    std::vector<FactoryPreset> factory;
    juce::StringArray userNames;
    juce::CriticalSection userNamesLock;   // hosts may query programs from another thread
};
