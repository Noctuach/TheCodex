#pragma once
#include <array>
#include <juce_core/juce_core.h>

/**
    Order of the processing modules after the oscillator. The order is one parameter ("chainOrder"): the index of the
    permutation (Lehmer code), so it is saved with sessions, presets, A/B and undo like every other setting.

    Modern (index 0, the default) follows how hardstyle kicks are built today: the reverb goes first, so the
    distortions after it fuse the room into the tail and widen it, instead of a reverb fading the finished kick.
    Classic is the order of Codex 1.0 (reverb last), used for sessions and presets saved before the order existed.
*/
namespace chain
{
    enum Module { Space, Dist1, Eq1, Dist2, Saturation, Compressor, Eq2, numModules };
    using Order = std::array<int, numModules>;

    inline const char* shortName (int m)
    {
        static const char* n[] = { "SPACE", "D1", "EQ1", "D2", "SAT", "COMP", "EQ2" };
        return n[juce::jlimit (0, (int) numModules - 1, m)];
    }

    inline const char* longName (int m)
    {
        static const char* n[] = { "Space", "Distortion 1", "EQ 1", "Distortion 2", "Saturation", "Compressor", "EQ 2" };
        return n[juce::jlimit (0, (int) numModules - 1, m)];
    }

    constexpr int factorial (int n) { return n <= 1 ? 1 : n * factorial (n - 1); }
    constexpr int numOrders = factorial (numModules);

    inline Order decode (int index)
    {
        index = juce::jlimit (0, numOrders - 1, index);
        std::array<int, numModules> pool {};
        for (int i = 0; i < numModules; ++i) pool[(size_t) i] = i;
        Order o {};
        int left = numModules;
        for (int i = 0; i < numModules; ++i)
        {
            const int f = factorial (numModules - 1 - i);
            const int k = index / f;
            index %= f;
            o[(size_t) i] = pool[(size_t) k];
            for (int j = k; j < left - 1; ++j) pool[(size_t) j] = pool[(size_t) j + 1];
            --left;
        }
        return o;
    }

    inline int encode (const Order& o)
    {
        int index = 0;
        for (int i = 0; i < numModules; ++i)
        {
            int smallerAfter = 0;
            for (int j = i + 1; j < numModules; ++j) smallerAfter += o[(size_t) j] < o[(size_t) i] ? 1 : 0;
            index += smallerAfter * factorial (numModules - 1 - i);
        }
        return index;
    }

    inline const Order modernOrder  { Space, Dist1, Eq1, Dist2, Saturation, Compressor, Eq2 };
    inline const Order classicOrder { Dist1, Eq1, Dist2, Saturation, Compressor, Eq2, Space };
    inline int modernIndex()  { return encode (modernOrder); }    // 0
    inline int classicIndex() { return encode (classicOrder); }

    inline juce::String describe (int index)
    {
        const auto o = decode (index);
        juce::StringArray s;
        for (int m : o) s.add (shortName (m));
        return s.joinIntoString (" > ");
    }
}
