// Renders the easter-egg dancer (Source/GUI/Dancer.h) to a contact sheet PNG, for reviewing its design:
//   row 1: eight kicks, one frame each, captured ~120 ms after the hit, at 2x
//   row 2: idle sway (no kicks for 2 s), four frames at 2x
//   row 3: the same kick frames at actual size (1x), as the plug-in shows them
// Usage: CodexDancerPreview <out.png>
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>
#include "../Source/GUI/Dancer.h"

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: CodexDancerPreview <out.png>\n"; return 1; }
    juce::ScopedJuceInitialiser_GUI init;
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

    std::atomic<unsigned int> hits { 0 };
    Dancer dancer (hits);
    dancer.setBounds (0, 0, 170, 230);   // the size the plug-in gives it
    dancer.toggle();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);

    auto snap = [&] (float scale)
    {
        // over a panel-coloured backdrop with a few controls' worth of texture, like the real page behind it
        juce::Image img (juce::Image::ARGB, juce::roundToInt (170 * scale), juce::roundToInt (230 * scale), true);
        juce::Graphics g (img);
        g.fillAll (Colours_::panel);
        g.setColour (Colours_::outline);
        for (int y = 20; y < 230; y += 46) g.drawHorizontalLine (juce::roundToInt (y * scale), 0.0f, 170.0f * scale);
        g.addTransform (juce::AffineTransform::scale (scale));
        dancer.paintEntireComponent (g, true);
        return img;
    };

    std::vector<juce::Image> kicks2x, kicks1x, idle;
    for (int i = 0; i < 8; ++i)
    {
        hits.fetch_add (1);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
        kicks2x.push_back (snap (2.0f));
        kicks1x.push_back (snap (1.0f));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (2000);
    for (int i = 0; i < 4; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        idle.push_back (snap (2.0f));
    }

    const int w2 = 340, h2 = 460, gap = 8, label = 22;
    const int width = 8 * (w2 + gap) + gap;
    const int height = label + h2 + gap + label + h2 + gap + label + 230 + gap;
    juce::Image sheet (juce::Image::RGB, width, height, true);
    juce::Graphics g (sheet);
    g.fillAll (juce::Colour (0xff050505));
    g.setColour (juce::Colours::white);
    g.setFont (16.0f);
    int y = 0;
    g.drawText ("KICKS (one frame per hit, 2x)", gap, y, width, label, juce::Justification::centredLeft); y += label;
    for (int i = 0; i < 8; ++i) g.drawImageAt (kicks2x[(size_t) i], gap + i * (w2 + gap), y);
    y += h2 + gap;
    g.drawText ("IDLE SWAY (no kicks, 2x)", gap, y, width, label, juce::Justification::centredLeft); y += label;
    for (int i = 0; i < 4; ++i) g.drawImageAt (idle[(size_t) i], gap + i * (w2 + gap), y);
    y += h2 + gap;
    g.drawText ("ACTUAL SIZE (1x)", gap, y, width, label, juce::Justification::centredLeft); y += label;
    for (int i = 0; i < 8; ++i) g.drawImageAt (kicks1x[(size_t) i], gap + i * (170 + gap), y);

    juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    out.deleteFile();
    juce::FileOutputStream os (out);
    if (! juce::PNGImageFormat().writeImageToStream (sheet, os)) { std::cerr << "could not write " << out.getFullPathName() << "\n"; return 2; }
    std::cout << "wrote " << out.getFullPathName() << "\n";
    return 0;
}
