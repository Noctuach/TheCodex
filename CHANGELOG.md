# Changelog

All notable changes to Codex. Versions follow [Semantic Versioning](https://semver.org).

## [Unreleased]
### Added
- **Drag kick to DAW** (footer): drag the button onto a track to drop the current kick as a 24-bit WAV in the selected
  key (named e.g. "Codex - Screech Rawstyle - G#1.wav"), or click it to save the file. Rendered offline with a copy of
  the plugin; the tail is trimmed automatically.
- **Output clipper** (footer): 4x oversampled, adjustable knee, hard ceiling (a +12 dB hot kick comes out at exactly
  the ceiling; inter-sample peaks stay within 0.5 dB of it). Off by default.
- **Preset browser**: search, style categories (Hardstyle, Rawstyle, Euphoric, Early Hardstyle, Hardcore, Tok, FX),
  favourites (stored in your settings), click to audition, double-click / Enter to load. Opens from the grid button
  next to the preset name or the menu.
- **Macros**: Punch, Dirt, Tail and Width knobs in the new footer; each sweeps several parameters at once on top of the
  preset (centre = as the preset is set).
- **A/B compare** (with copy) and a **CPU** readout in the footer.
- Six new factory presets that use the new features: Screech Rawstyle, X-Shaped Zaag, Euphoric Wide Hall,
  Upward Tail Crunch, Drawn Modern Hardstyle, Downsampled Hardcore.
- **Draw envelopes**: pitch and amp envelopes can switch from ADSR to DRAW mode: up to 8 breakpoints with a bend per
  segment, edited directly in the display (drag points and bend handles, double-click to add/delete, right-click for
  shapes). Pitch Amount and the amp Release still apply.
- **Distortion engine upgrade** (synth-style workflow):
  - Resonant **Pre / Post filter** per unit (Low Pass 12/24, Band Pass, High Pass 12/24, Notch), zero-delay SVF that
    stays clean under fast changes; response strip under the transfer display (drag: frequency + resonance).
  - **X-Shaper** and **X-Shaper (Asym)**: draw the distortion curve directly in the transfer display; preset curves on
    right-click.
  - New modes: **Diode 2**, **Zero-Square**, **Downsample** (Drive sets the rate reduction).
  - **Antiderivative anti-aliasing** on every curve: audible aliasing on a 5 kHz hard-clipped sine drops by 12-28 dB
    (4x: -64 dB, 8x: -89 dB, 16x: -91 dB). New **16x** oversampling option.
- **EQ 2 tab**: a second 24-band equaliser after the Compressor (before Space).
- **EQ processing modes** (Pro-Q 4): Zero Latency, Natural Phase (2x oversampled, analog-matched response) and
  Linear Phase (FIR with a 2x2 matrix for mid/side and left/right bands; resolution Low to Maximum), for both EQs.
- **Compressor tab**: multiband (1–4 bands) downward, upward and two-way compression. Styles modelled on FabFilter
  Pro-C 3 (Clean, Punch, Smooth, Bus, Mastering, Classic, Vari-Mu, Opto, Pumping, TTM) plus an LA-3A-style
  T4 Opto with Compress / Limit. Threshold, ratio, knee, range, attack, release, hold, lookahead, side-chain high-pass,
  auto gain, parallel mix. Scrolling level / gain-change display with a draggable threshold and a knee display.
- **Space tab**: stereo room reverb modelled on ValhallaRoom's controls (mode, predelay, decay, depth, early and late
  sections, decay EQ with bass/high multipliers) plus Low Cut, Width and Ducking for kicks. Off by default.
- **EQ**: up to 24 bands (Pro-Q 4 limit) with add, delete and bypass; only bands in use are shown.
- Distortion units show a clear **BYPASSED** state with an ENABLE button.

### Changed
- Cleaner, flatter interface (graphite palette, flat panels, thinner knobs, subtle glows), modelled on modern
  mastering suites such as Ozone 12.
- All displays animate at 60 fps; the scope, spectrum and stereo field render at the screen's full pixel density, and
  the oscilloscope captures 4x more detail per hit.
- Envelope displays are editable: drag the attack, decay/sustain, release and curve points directly.
- Tabs follow the signal chain: Kick, Distortion, Saturation, Compressor, EQ 2, Space, Scope.
- Smaller Drive knobs in the distortion units and the saturation band panel.
- The host's tail length follows the reverb decay.

### Fixed
- Switching from *909 Clean* (both distortions bypassed on purpose) to another preset re-enables the distortion units
  (covered by an automated test).

## [1.0.0] - first test build
- Synth and 909 engines with key selection F–E, pitch and amp ADSR, two distortion units (Distroyer-style), a Pro-Q-style
  EQ with mid/side placement, multiband saturation (Saturn-style), scope page, tuner, presets, undo/redo, resizable UI.
