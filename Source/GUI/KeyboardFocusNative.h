#pragma once

/** Native helpers for KeyboardFocusPolicy (KeyboardFocus.h), in KeyboardFocusNative.cpp. Plain pointers (HWND on
    Windows), no JUCE types: the Windows implementation includes <windows.h>, which clashes with JUCE names.
    Windows and Linux (X11) do something; on macOS they are no-ops. */
namespace codex::nativefocus
{
    /** The window that has the OS keyboard focus, or nullptr when it is the plug-in window (or a child of it). */
    void* hostFocus (void* pluginWindow);
    /** True when the OS keyboard focus is in the plug-in window or one of its children. */
    bool isInside (void* pluginWindow);
    /** Gives the OS keyboard focus back to `preferred` (when it still exists), else to the host's parent window. */
    void returnTo (void* preferred, void* pluginWindow);
}
