// Native part of KeyboardFocusPolicy (KeyboardFocus.h). Kept apart from the JUCE code: <windows.h> clashes with JUCE names.
//
// JUCE has no way to hand the OS keyboard focus back to the host once a plug-in text field had it: it only clears its own
// focused component, and posts unused key messages to the parent window, which most hosts (FL Studio, Ableton Live, ...)
// do not treat as typing-keyboard input. So after typing, Codex gives the focus back to the window that had it.
// On Linux every mouse button press on a JUCE window moves the X input focus into it (LinuxComponentPeer::toFront),
// before any Component flag is looked at, so the same hand-back runs there after each click.
#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#elif defined (__linux__)
 #include <X11/Xlib.h>
 #include <dlfcn.h>
#endif
#include "KeyboardFocusNative.h"
#include <cstdint>
#include <type_traits>

namespace codex::nativefocus
{
#if defined (_WIN32)
    static bool insideWindow (HWND focus, HWND plugin)
    {
        return focus != nullptr && plugin != nullptr && (focus == plugin || ::IsChild (plugin, focus));
    }

    void* hostFocus (void* pluginWindow)
    {
        const HWND f = ::GetFocus();
        return insideWindow (f, (HWND) pluginWindow) ? nullptr : (void*) f;
    }

    bool isInside (void* pluginWindow)
    {
        return insideWindow (::GetFocus(), (HWND) pluginWindow);
    }

    void returnTo (void* preferred, void* pluginWindow)
    {
        const HWND plugin = (HWND) pluginWindow;
        HWND target = (HWND) preferred;
        if (target == nullptr || ! ::IsWindow (target) || insideWindow (target, plugin))
            target = ::GetParent (plugin);   // the host's container window (the one JUCE forwards unused keys to)
        if (target != nullptr && ::IsWindow (target))
            ::SetFocus (target);
    }
#elif defined (__linux__)
    // Xlib is used through the libX11 that JUCE has already loaded (JUCE loads it at run time too, so the plug-in does
    // not link it). Codex keeps its own display connection: the X input focus is per server, not per connection.
    // Window handles are X window ids (LinuxComponentPeer::getNativeHandle).
    namespace
    {
        struct X11
        {
            decltype (&::XOpenDisplay) openDisplay = nullptr;
            decltype (&::XGetInputFocus) getInputFocus = nullptr;
            decltype (&::XSetInputFocus) setInputFocus = nullptr;
            decltype (&::XQueryTree) queryTree = nullptr;
            decltype (&::XFree) free = nullptr;
            decltype (&::XGetWindowAttributes) getWindowAttributes = nullptr;
            decltype (&::XSetErrorHandler) setErrorHandler = nullptr;
            decltype (&::XSync) sync = nullptr;
            Display* display = nullptr;

            static X11& get()
            {
                static X11 x;
                return x;
            }

            X11()
            {
                // only when the process already uses X11 (a host or JUCE window): never start an X connection ourselves
                void* lib = ::dlopen ("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
                if (lib == nullptr) return;
                auto load = [lib] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (lib, name)); return fn != nullptr; };
                if (load (openDisplay, "XOpenDisplay") && load (getInputFocus, "XGetInputFocus") && load (setInputFocus, "XSetInputFocus")
                    && load (queryTree, "XQueryTree") && load (free, "XFree") && load (getWindowAttributes, "XGetWindowAttributes")
                    && load (setErrorHandler, "XSetErrorHandler") && load (sync, "XSync"))
                    display = openDisplay (nullptr);
            }

            bool ok() const { return display != nullptr; }

            /** Runs `fn` with X errors ignored (a window may vanish between two calls; Xlib's default handler exits). */
            template <typename Fn>
            auto trapped (Fn&& fn)
            {
                auto old = setErrorHandler ([] (Display*, XErrorEvent*) { return 0; });
                auto result = fn();
                sync (display, False);
                setErrorHandler (old);
                return result;
            }

            Window parentOf (Window w)
            {
                Window root = 0, parent = 0, *children = nullptr;
                unsigned int n = 0;
                if (queryTree (display, w, &root, &parent, &children, &n) == 0) return 0;
                if (children != nullptr) free (children);
                return parent == root ? 0 : parent;
            }

            /** True when `w` is `plugin` or one of its descendants. */
            bool inside (Window w, Window plugin)
            {
                for (int depth = 0; w != 0 && depth < 64; ++depth)
                {
                    if (w == plugin) return true;
                    w = parentOf (w);
                }
                return false;
            }

            Window focus()
            {
                Window f = 0;
                int revert = 0;
                getInputFocus (display, &f, &revert);
                return f == PointerRoot ? 0 : f;   // None is 0
            }

            bool viewable (Window w)
            {
                XWindowAttributes a {};
                return getWindowAttributes (display, w, &a) != 0 && a.map_state == IsViewable;
            }
        };
    }

    void* hostFocus (void* pluginWindow)
    {
        auto& x = X11::get();
        if (! x.ok() || pluginWindow == nullptr) return nullptr;
        const auto plugin = (Window) (uintptr_t) pluginWindow;
        return x.trapped ([&]() -> void*
        {
            const Window f = x.focus();
            return f == 0 || x.inside (f, plugin) ? nullptr : (void*) (uintptr_t) f;
        });
    }

    bool isInside (void* pluginWindow)
    {
        auto& x = X11::get();
        if (! x.ok() || pluginWindow == nullptr) return false;
        const auto plugin = (Window) (uintptr_t) pluginWindow;
        return x.trapped ([&] { return x.inside (x.focus(), plugin); });
    }

    void returnTo (void* preferred, void* pluginWindow)
    {
        auto& x = X11::get();
        if (! x.ok() || pluginWindow == nullptr) return;
        const auto plugin = (Window) (uintptr_t) pluginWindow;
        x.trapped ([&]
        {
            Window target = (Window) (uintptr_t) preferred;
            if (target == 0 || x.inside (target, plugin) || ! x.viewable (target))
                target = x.parentOf (plugin);   // the host's container window
            if (target != 0 && x.viewable (target))
                x.setInputFocus (x.display, target, RevertToParent, CurrentTime);
            return 0;
        });
    }
#else
    // macOS: unused keys travel up the responder chain to the host once no JUCE component is focused.
    void* hostFocus (void*) { return nullptr; }
    bool isInside (void*) { return false; }
    void returnTo (void*, void*) {}
#endif
}
