# Codex

**A free hardstyle kick synthesizer.** Codex builds kicks the way they were made before sample packs: a swept oscillator
(or a synthesized 909), distortion, EQ, and distortion again. There are no samples; every kick is generated in real time,
in the key of your track.

![Codex: kick page](docs/screenshot.png)

| 909 engine | Distortion page |
|---|---|
| ![909 engine](docs/screenshot-909.png) | ![Distortion page](docs/screenshot-distortion.png) |

Formats: **VST3** (Windows, macOS, Linux), **AU** (macOS) and a **Standalone** app.

---

## Quick start
1. Load Codex on an instrument track and play any note (or press **KICK** in the top bar).
2. Set **KEY** to the key of your track. With *Fixed Key* mode, every MIDI note plays the kick in that key.
3. Browse the presets with the arrows next to the preset name.
4. Hover over any control to see what it does.

The **tuner** (bottom right) measures the actual root note coming out of the plugin, after all distortion,
so you can see your kick is in key.

## Your first hardstyle kick, step by step
1. Start from **Init** (or **909 Clean** for the 909 engine).
2. **Punch:** in *Pitch Envelope*, set *Amount* to 40–60 st and *Decay* to 60–150 ms. Lower *Curve* gives a longer "zap",
   higher gives a snappier hit.
3. **Length:** in *Amp Envelope*, set *Decay*/*Sustain* for the body; the note length controls the tail.
4. **First distortion:** *Dist 1* with *Overdrive* or *Hard Clip* and 18–30 dB of drive.
5. **EQ between the distortions:** this is the classic trick. Cut a narrow band around the low-mids (100–200 Hz) and boost
   around 300–500 Hz; whatever you boost here gets pushed hardest by the second distortion.
6. **Second distortion:** *Dist 2* with *Hard Clip* or *Diode* and a lower drive (6–14 dB). Use its *Hi Filter* (40–80 Hz)
   to keep the sub clean.
7. Check the tuner and the output meter (keep the peak marker out of the red).

## Controls

### Top bar
| Control | What it does |
|---|---|
| Undo / Redo | Undo parameter changes and preset loads. |
| Preset | Factory presets first, then your own. Selecting the current preset again reverts your edits. |
| Save | Saves a user preset (`Documents/Codex/Presets/*.cdxpreset`). |
| Key / Oct | Root note, **F to E** (F1 = 43.65 Hz … E2 = 82.41 Hz), plus an octave shift. Presets never change the key. |
| Key Mode | *Fixed Key* plays the selected key from any MIDI note; *Follow MIDI* plays the note you press. |
| KICK | Plays the kick in the selected key. |
| Menu | Save/delete presets, open the preset folder, interface size (75–200 %), tooltips on/off, About. |

### Kick page
| Section | Controls |
|---|---|
| **Source: Synth** | *Wave* (Sine, Triangle, Warm Sine), *Fine Tune*, *Phase* (start phase), *Click* (noise transient: level, decay, tone). |
| **Source: 909** | Synthesized 909-style bass drum. *909 Model* (Classic, Long Tail, Punch, Deep, Hard Attack, Gabber, Early Hardstyle, Boom, Driven, Short Tok) plus the original's *Tune* (sweep depth), *Attack* (click) and *Decay*. |
| **Pitch Envelope** | ADSR for the pitch sweep, with *Amount* (semitones above the root) and *Curve*. Inactive in 909 mode. |
| **Amp Envelope** | ADSR for volume with *Curve*. In 909 mode it acts as a gate (attack and release only). |
| **Equalizer** | 6 bands: Low/High Cut (12/24/48 dB/oct), Low/High Shelf, Bell, Notch. Live spectrum analyzer, ±12/±24 dB view. |

**EQ mouse controls:** drag a node to set frequency/gain · mouse wheel for Q · double-click a node to turn it on/off ·
double-click empty space to add a band · right-click a node for type and slope · Shift for fine moves.

### Distortion page
Two full distortion units, one **before** and one **after** the EQ:

| Group | Controls |
|---|---|
| Type | 12 curves: Hard Clip, Soft Clip, Tanh, Overdrive, Tube, Diode, Fuzz, Sine Fold, Tri Fold, Rectify, Bitcrush, Asym Clip. The display shows the live transfer curve and the current input level. |
| Shape | *Drive*, *Boost* (input gain on dry and wet), *Characteristic* (morphs any curve towards a square wave), *Offset* (asymmetry). |
| Wet filters / tone | *Lo Filter* (low-pass before the distortion), *Hi Filter* (high-pass after it), *Shelf Freq/Gain*. |
| Output | *Mix* (dry/wet), *Output*. |
| Options | *Oversampling* (Off/2x/4x/8x) and *DC Filter*. |

The bottom row of the kick page has quick controls for both stages; **EDIT** opens the full unit.

### Output
**Scope** (the first 500 ms of every hit), **tuner**, output level and meter.

### Mouse and keyboard
Double-click any knob to reset it · Shift-drag for fine control · click a value to type it in · drag the bottom-right
corner to resize the window.

---

## Building from source
Requires CMake ≥ 3.22 and a C++17 compiler (Visual Studio 2022, Xcode 14+, or GCC/Clang). JUCE 8 is downloaded
automatically (or use `-DJUCE_DIR=/path/to/JUCE`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Plugins are written to `build/Codex_artefacts/Release/`. On Linux, install the JUCE dependencies first:
`libasound2-dev libfreetype-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfontconfig1-dev libgl1-mesa-dev`.

**Continuous integration:** `.github/workflows/build.yml` builds Windows, macOS (universal) and Linux, runs the offline
tests, validates the VST3 with [pluginval](https://github.com/Tracktion/pluginval) and uploads ready-to-use zips.

**Tests:** configure with `-DHK_BUILD_TOOLS=ON` and run `CodexRender out/`. It renders every factory preset,
checks the tuning of every key in both engines, every oversampling setting, several sample rates and block sizes, the mono
layout and state save/restore, and saves GUI screenshots.

## Project layout
```
Source/
  Parameters.*          all parameters (IDs, ranges, defaults)
  PresetManager.*       factory and user presets
  PluginProcessor.*     MIDI, the processing chain, scope capture
  DSP/                  KickVoice (synth + 909 engines), Kick909 models, CurveEnvelope,
                        ClipDistortion, ParametricEQ, PitchDetector
  GUI/                  look-and-feel, widgets, EQ, distortion units, tuner, scope, overlays, editor
Resources/Fonts/        Barlow (SIL Open Font License)
docs/                   screenshots, research notes, release checklist
tools/Render.cpp        offline test tool
```

## Credits and licences
- Built with [JUCE](https://juce.com).
- Fonts: [Barlow](https://github.com/jpt/barlow) by The Barlow Project Authors, SIL Open Font License 1.1
  (`Resources/Fonts/OFL-Barlow.txt`, included with every build).
- VST is a registered trademark of Steinberg Media Technologies GmbH.
- Background research: [docs/RESEARCH.md](docs/RESEARCH.md).
