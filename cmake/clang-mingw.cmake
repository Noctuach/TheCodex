# Cross-compile Windows x64 binaries from Linux with clang + the MinGW-w64 runtime (test builds only;
# official Windows builds are made with Visual Studio by .github/workflows/build.yml).
#   cmake/mingw-compat/fetch-headers.sh build-win/mingw-overlay
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-mingw.cmake \
#         -DMINGW_OVERLAY_INCLUDE=$PWD/build-win/mingw-overlay -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win --target Codex_VST3
# Requires: clang, lld, mingw-w64 (posix threads).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_SYSROOT /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# use the posix-thread flavour of the MinGW libstdc++ (std::thread etc.)
set(gccdir /usr/lib/gcc/x86_64-w64-mingw32/13-posix)
set(cxxinc "-isystem ${gccdir}/include/c++ -isystem ${gccdir}/include/c++/x86_64-w64-mingw32 -isystem ${gccdir}/include/c++/backward")
# target Windows 10 RS5+ (JUCE 8 needs Windows 10; the newest Direct2D interfaces are guarded by NTDDI_VERSION)
set(winver "-DNTDDI_VERSION=0x0A000006")
# -femulated-tls: match the thread-local storage model of the GCC-built MinGW libstdc++
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ ${cxxinc} -include cstring ${winver} -femulated-tls")
set(CMAKE_C_FLAGS_INIT "${winver}")
# Codex does not need the Windows Media codec (its MinGW headers are incomplete)
string(APPEND CMAKE_CXX_FLAGS_INIT " -DJUCE_USE_WINDOWS_MEDIA_FORMAT=0")
if(MINGW_OVERLAY_INCLUDE)
    string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${MINGW_OVERLAY_INCLUDE}")
    if(EXISTS "${MINGW_OVERLAY_INCLUDE}/codex_mingw_compat.h")
        string(APPEND CMAKE_CXX_FLAGS_INIT " -include ${MINGW_OVERLAY_INCLUDE}/codex_mingw_compat.h")
    endif()
    string(APPEND CMAKE_C_FLAGS_INIT " -isystem ${MINGW_OVERLAY_INCLUDE}")
endif()

# self-contained DLL: no MinGW runtime DLLs needed on the user's machine
set(linkflags "-fuse-ld=lld -L${gccdir} -static -static-libgcc -static-libstdc++")
# JUCE links these with #pragma comment(lib) under MSVC; MinGW needs them spelled out
# (everything static, including the libstdc++/libgcc/winpthread the compiler adds afterwards)
set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,-Bstatic -lpthread -limm32 -lwinmm -lversion -lws2_32 -lwininet -lshlwapi -ld2d1 -ldwrite -ldxgi -ld3d11 -ldcomp -lcomctl32 -luxtheme -lrpcrt4 -ldbghelp -lcrypt32 -lsetupapi -liphlpapi -lpsapi -lwtsapi32 -lshcore -ldwmapi -lmsimg32 -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${linkflags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${linkflags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${linkflags}")
