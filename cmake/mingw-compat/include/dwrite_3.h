#pragma once
// Wrapper: current MinGW-w64 dwrite_3.h + constexpr flag operators (as in the Windows SDK) for this enum.
#include "dwrite_3_upstream.h"
#ifdef __cplusplus
constexpr inline DWRITE_GLYPH_IMAGE_FORMATS operator| (DWRITE_GLYPH_IMAGE_FORMATS a, DWRITE_GLYPH_IMAGE_FORMATS b) noexcept { return (DWRITE_GLYPH_IMAGE_FORMATS) ((unsigned) a | (unsigned) b); }
constexpr inline DWRITE_GLYPH_IMAGE_FORMATS operator& (DWRITE_GLYPH_IMAGE_FORMATS a, DWRITE_GLYPH_IMAGE_FORMATS b) noexcept { return (DWRITE_GLYPH_IMAGE_FORMATS) ((unsigned) a & (unsigned) b); }
inline DWRITE_GLYPH_IMAGE_FORMATS& operator|= (DWRITE_GLYPH_IMAGE_FORMATS& a, DWRITE_GLYPH_IMAGE_FORMATS b) noexcept { return a = a | b; }
#endif
