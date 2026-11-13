#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "KeyboardFocusNative.h"

/**
    Keeps the computer keyboard with the host.

    By default nothing in the editor takes keyboard focus: clicks, drags, hovering, menus and combo boxes leave it where it
    was, so the DAW's typing keyboard (computer keys as MIDI notes), its transport and its shortcuts keep working while you
    use Codex. On Windows a focused plug-in window swallows keys (JUCE only posts unused ones to the parent window, which
    most hosts ignore), so this matters most there.

    Text fields are the exception: the preset name, the browser search and a knob value being typed in take the keyboard
    while they are in use. Clicking anywhere else, Return, Escape or closing the dialog releases it, and on Windows the OS
    focus is handed back to the host window that had it. On Linux a mouse click always moves the X input focus into the
    plug-in window (JUCE does it before any component flag is checked), so there it is handed back after every click.

    With "Keyboard shortcuts in Codex" switched on (menu, stored in the user settings) the old behaviour returns: a click
    gives the editor the keyboard so Ctrl/Cmd+Z/Y/B/S, Ctrl/Cmd+arrows and Delete on the EQ work.

    The flags are applied by a sweep over the whole editor (on start, on a mode change and ten times a second), so
    components created later (overlays, slider text boxes rebuilt by a look-and-feel change, list rows) are covered too.
*/
class KeyboardFocusPolicy : private juce::FocusChangeListener, private juce::MouseListener, private juce::Timer
{
public:
    /** Components that take keys while shortcuts are on (the main view, the EQ displays, the overlays). */
    static void markKeyTarget (juce::Component& c) { c.getProperties().set (keyTargetProperty, true); }
    /** Text fields always keep JUCE's own focus behaviour. */
    static bool isTextEntry (const juce::Component* c) { return dynamic_cast<const juce::TextEditor*> (c) != nullptr; }

    /** root: the editor's main view. isPlugin: false in the standalone app (its window may keep the keyboard). */
    KeyboardFocusPolicy (juce::Component& rootComponent, bool isPlugin) : root (rootComponent), plugin (isPlugin)
    {
        markKeyTarget (root);
        auto& d = juce::Desktop::getInstance();
        d.addFocusChangeListener (this);
        d.addGlobalMouseListener (this);
        startTimerHz (10);
    }

    ~KeyboardFocusPolicy() override
    {
        auto& d = juce::Desktop::getInstance();
        d.removeFocusChangeListener (this);
        d.removeGlobalMouseListener (this);
    }

    void setShortcutsEnabled (bool on)
    {
        shortcuts = on;
        apply();
        if (! on) releaseFocus();
    }
    bool areShortcutsEnabled() const { return shortcuts; }

    /** Sets the focus flags on every component in the editor. Cheap (a flag per component); also run by the timer. */
    void apply()
    {
        applyTo (root);
        // The plug-in wrapper (the window's top-level component) brings itself to front on every click, and
        // Component::toFront (true) then grabs the keyboard focus for the first component that wants it.
        if (plugin)
            if (auto* top = root.getTopLevelComponent(); top != &root)
                top->setBroughtToFrontOnMouseClick (false);
    }

    /** Takes the keyboard away from Codex: JUCE's focus, and on Windows and Linux the OS focus (back to the host). */
    void releaseFocus()
    {
        if (auto* f = juce::Component::getCurrentlyFocusedComponent(); f != nullptr && isInside (f))
            f->giveAwayKeyboardFocus();
        returnOsFocus();
    }

    /** True when `c` is part of this editor's window. */
    bool isInside (const juce::Component* c) const
    {
        if (c == nullptr) return false;
        auto* top = root.getTopLevelComponent();
        return c == top || top->isParentOf (c);
    }

private:
    static constexpr const char* keyTargetProperty = "codexKeyTarget";

    void applyTo (juce::Component& c)
    {
        if (isTextEntry (&c))
            return;   // JUCE defaults: wants focus, and a click focuses it
        const bool target = (bool) c.getProperties()[keyTargetProperty];
        c.setWantsKeyboardFocus (shortcuts && target);
        c.setMouseClickGrabsKeyboardFocus (shortcuts);
        for (auto* child : c.getChildren())
            applyTo (*child);
    }

    void* peerHandle() const
    {
        auto* peer = root.getPeer();
        return peer != nullptr ? peer->getNativeHandle() : nullptr;
    }

    void returnOsFocus()
    {
        if (! plugin) return;
        if (auto* h = peerHandle(); h != nullptr && codex::nativefocus::isInside (h))
            codex::nativefocus::returnTo (lastHostFocus, h);
    }

    void timerCallback() override
    {
        apply();
        if (plugin)
            if (auto* h = peerHandle())
                if (auto* f = codex::nativefocus::hostFocus (h))
                    lastHostFocus = f;   // where the keyboard goes back to after typing in a text field
    }

    // Asynchronous, after any focus change in the process
    void globalFocusChanged (juce::Component* focused) override
    {
        if (shortcuts) return;
        if (focused != nullptr && isInside (focused) && ! isTextEntry (focused))
            focused->giveAwayKeyboardFocus();   // something took focus that should not have: give it back
        if (focused == nullptr || ! isTextEntry (focused))
            returnOsFocus();
    }

    // Global mouse listener: called for every component in the process, after the click's own focus handling
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (shortcuts || ! isInside (e.eventComponent)) return;
        auto* f = juce::Component::getCurrentlyFocusedComponent();
        // a click outside the text field being typed in ends the typing
        if (f != nullptr && isInside (f) && f != e.eventComponent && ! f->isParentOf (e.eventComponent))
            releaseFocus();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (shortcuts || ! isInside (e.eventComponent)) return;
        // the host or the OS may have focused the plug-in window on the click: give the keyboard back
        if (! isTextEntry (juce::Component::getCurrentlyFocusedComponent()))
            returnOsFocus();
    }

    juce::Component& root;
    const bool plugin;
    bool shortcuts = false;
    void* lastHostFocus = nullptr;
};
