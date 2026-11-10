// Symbols that the Windows SDK / MSVC provide but MinGW-w64 does not.
// Only compiled into MinGW cross-builds (see cmake/clang-mingw.cmake); official builds use Visual Studio.
#include <windows.h>
#include <dxgi.h>
#include <sapi.h>
#define JUCE_CORE_INCLUDE_COM_SMART_PTR 1
#include <juce_core/juce_core.h>
#include <cstring>
#include <ctime>

// JUCE asks for __uuidof (smartPointer); MSVC resolves this to the wrapped interface's IID.
template <> const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGISurface>>() { return __mingw_uuidof<IDXGISurface>(); }
template <> const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGIDevice>>()  { return __mingw_uuidof<IDXGIDevice>(); }

// Speech API voice class (used by JUCE's accessibility code); value from sapi.h in the Windows SDK.
extern "C" const CLSID CLSID_SpVoice = { 0x96749377, 0x3391, 0x11D2, { 0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96 } };

// Audio sub-format IDs from ksmedia.h (used by JUCE's audio device code in the tools).
extern "C" const GUID KSDATAFORMAT_SUBTYPE_PCM        = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
extern "C" const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// _get_tzname is exported by the Windows C runtime but missing from MinGW's msvcrt import library.
extern "C" errno_t __cdecl codex_get_tzname (size_t* returnValue, char* buffer, size_t size, int index)
{
    if (index < 0 || index > 1) return EINVAL;
    const char* name = _tzname[index];
    const size_t len = std::strlen (name) + 1;
    if (returnValue != nullptr) *returnValue = len;
    if (buffer == nullptr) return 0;
    if (size < len) return ERANGE;
    std::memcpy (buffer, name, len);
    return 0;
}
extern "C" { errno_t (__cdecl* __imp__get_tzname) (size_t*, char*, size_t, int) = codex_get_tzname; }
