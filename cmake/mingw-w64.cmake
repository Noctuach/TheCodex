# NOTE: the MinGW-w64 11 packages in Ubuntu 24.04 lack the Direct2D 1.2/1.3 headers that JUCE 8 needs.
# Pass -DMINGW_OVERLAY_INCLUDE=<dir> with d2d1_2.h, d2d1_3.h (and helpers) from current MinGW-w64 sources.
# Official Windows builds are made with Visual Studio by .github/workflows/build.yml.
# Cross-compile Windows x64 binaries from Linux with MinGW-w64 (posix threads).
# Usage: cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
# JUCE expects <cstring> to be pulled in indirectly, which MinGW does not do
set(CMAKE_CXX_FLAGS_INIT "-include cstring")
if(MINGW_OVERLAY_INCLUDE)
    string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${MINGW_OVERLAY_INCLUDE}")
endif()
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Ship a self-contained DLL (no MinGW runtime DLLs needed on the user's machine)
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
