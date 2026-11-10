# Contributing to Codex

Thanks for helping make Codex better. Codex is a free hardstyle kick synthesizer; the goal is a tool that genuinely
helps producers.

## Ground rules
- **Files are never deleted.** The `No file deletions` check fails any pull request that removes a file. If a file
  becomes obsolete, leave it in place and mark it as deprecated in a comment at the top (see `Source/GUI/Scope.h`).
- Work on a branch and open a pull request against `main`; `main` is protected (no force pushes, no direct deletion).
- Keep the offline tests passing (see below). New DSP features should come with a test in `tools/Render.cpp`.
- Parameter IDs are part of saved projects and presets: never rename or reuse an ID. Add new parameters instead.

## Building
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHK_BUILD_TOOLS=ON
cmake --build build --config Release
```
On Linux, install the JUCE dependencies first (see the README).

## Testing
```bash
build/CodexRender_artefacts/Release/CodexRender test-output/
```
It renders every factory preset and checks tuning, distortion, EQ, saturation, compressor and reverb behaviour, state
save/restore and the GUI (screenshots are written to the output folder). The run must end with `ALL CHECKS PASSED`.
CI (`.github/workflows/build.yml`) builds Windows, macOS and Linux, runs these tests and validates the VST3 with pluginval.

## Code style
- C++17, JUCE 8. Four-space indentation, braces on their own line, `juce::` prefixes in headers.
- No allocations or locks on the audio thread.
- User-facing text: plain English, sentence case. Every parameter needs a tooltip in `Source/GUI/Help.h`.

## Reporting bugs
Use the bug report template. Include your DAW, OS, plugin format and the steps to reproduce; a saved preset helps.
