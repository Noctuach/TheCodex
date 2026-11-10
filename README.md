# HardKick

An oldskool hardstyle kick synthesizer (VST3 / AU / Standalone), built the way kicks were made
before sample packs: a swept oscillator, envelopes, and a **distort → EQ → distort** chain.
No samples anywhere.

![HardKick](docs/screenshot.png)

## Signal flow

```
OSC + PITCH ENV + CLICK  →  CLIP 1 (pre-EQ)  →  6-band EQ  →  CLIP 2 (post-EQ)  →  OUTPUT
```

| Section | What it does |
|---|---|
| **Oscillator** | Sine / Triangle / Warm Sine. Phase resets on every hit (identical punch every time), adjustable start phase. Noise **click** layer with decay and tone. |
| **Pitch envelope (ADSR)** | The classic pitch sweep. *Amount* (up to 96 semitones) above the played note, A/D/S/R and a *Curve* control (1 = linear, higher = snappier exponential drop). |
| **Amp envelope (ADSR)** | Volume shape with its own curve. Hold the MIDI note longer for a longer tail. |
| **Clip 1 / Clip 2** | 4× oversampled clippers: Hard, Soft, Tanh, Fold. *Bias* adds asymmetry (even harmonics), plus Mix and Output. Each stage has a power button. |
| **EQ** | 6 bands: Low Cut, Low Shelf, Bell, Notch, High Shelf, High Cut (cuts at 12/24/48 dB/oct), with a live spectrum analyzer. |

The MIDI note sets the pitch (the envelope sweeps down *to* that note), and *Tune* shifts it.
The **KICK** button plays F1 (~43.7 Hz), a common hardstyle root.

### EQ mouse controls
- **Drag a node**: frequency / gain (**Shift** for fine adjustments)
- **Mouse wheel** over a node: Q
- **Double-click a node**: band on/off
- **Double-click empty space**: adds a bell band there
- **Right-click a node**: filter type, slope, reset

## Building

Requires CMake ≥ 3.22 and a C++17 compiler (Visual Studio 2022 on Windows, Xcode on macOS).
JUCE 8 is downloaded automatically. You can point to your own copy with `-DJUCE_DIR=/path/to/JUCE`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Plugins end up in `build/HardKick_artefacts/Release/` (`VST3/HardKick.vst3`, `Standalone/`, and `AU/` on macOS).
Copy the `.vst3` to your plugin folder:
- Windows: `C:\Program Files\Common Files\VST3`
- macOS: `~/Library/Audio/Plug-Ins/VST3` (AU: `~/Library/Audio/Plug-Ins/Components`)

On Linux, install the JUCE dependencies first (e.g. `libasound2-dev libfreetype-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfontconfig1-dev libgl1-mesa-dev`).

### Offline test tool
`-DHK_BUILD_TOOLS=ON` builds `HardKickRender`. It renders four kicks through the full plugin to a WAV file
and saves a PNG snapshot of the GUI:
```bash
HardKickRender kick.wav ui.png
```

## Source layout
```
Source/
  Parameters.*          all parameters (IDs, ranges, defaults)
  PluginProcessor.*     MIDI handling and the processing chain
  DSP/CurveEnvelope.h   ADSR with curve
  DSP/KickVoice.h       oscillator, pitch sweep, click
  DSP/ClipDistortion.h  oversampled clipper
  DSP/ParametricEQ.h    biquad EQ (shared by the DSP and the GUI curve)
  GUI/                  look and feel, widgets, EQ display, editor
```
