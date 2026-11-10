#pragma once
#include "Widgets.h"
#include "../KickExporter.h"

/**
    Drag the kick into the DAW: dragging this button renders the current kick (in the selected key) to a WAV
    file and starts an external file drag. Clicking it opens a "save as" dialog instead.
*/
class ExportButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ExportButton (CodexProcessor& p) : proc (p)
    {
        setTooltip ("Export: drag this onto a track in your DAW to drop the kick as audio (24-bit WAV, in the selected key). Click to save the file instead.");
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool hot = isMouseOver() || busy;
        g.setColour (hot ? Colours_::accent.withAlpha (0.16f) : Colours_::panelTop);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (hot ? Colours_::accent.withAlpha (0.8f) : Colours_::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        auto icon = r.removeFromLeft (r.getHeight()).reduced (5.0f);
        Icons::draw (g, Icons::exportArrow(), icon, hot ? Colours_::accentHi : Colours_::textDim, 1.5f);
        g.setColour (hot ? Colours_::accentHi : Colours_::textDim);
        g.setFont (Fonts::label (12.0f, 0.08f));
        g.drawText (busy ? "RENDERING" : "DRAG KICK", r.withTrimmedRight (6.0f), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { dragStarted = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStarted || e.getDistanceFromDragStart() < 6) return;
        dragStarted = true;
        const auto file = renderNow (KickExporter::defaultFile (proc));
        if (file.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragStarted) return;
        chooser = std::make_unique<juce::FileChooser> ("Export kick", juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                                         .getChildFile (KickExporter::defaultFile (proc).getFileName()), "*.wav");
        juce::Component::SafePointer<ExportButton> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr) return;
                                  auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  safe->renderNow (f.withFileExtension ("wav"));
                              });
    }

private:
    juce::File renderNow (const juce::File& target)
    {
        busy = true;
        repaint();
        const auto f = KickExporter::render (proc, target);
        busy = false;
        repaint();
        return f;
    }

    CodexProcessor& proc;
    std::unique_ptr<juce::FileChooser> chooser;
    bool dragStarted = false, busy = false;
};
