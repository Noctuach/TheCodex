#pragma once
// Declarations present in the Windows SDK but missing from MinGW-w64 (cross-compiled test builds only).
#ifdef __cplusplus
enum CaretPosition { CaretPosition_Unknown = 0, CaretPosition_EndOfLine = 1, CaretPosition_BeginningOfLine = 2 };
#endif
