# Changelog

All notable changes to Codex. Versions follow [Semantic Versioning](https://semver.org).

## [Unreleased]
### Added
- **Compressor tab**: multiband (1–4 bands) downward, upward and two-way compression. Styles modelled on FabFilter
  Pro-C 3 (Clean, Punch, Smooth, Bus, Mastering, Classic, Vari-Mu, Opto, Pumping, TTM) plus an LA-3A-style
  T4 Opto with Compress / Limit. Threshold, ratio, knee, range, attack, release, hold, lookahead, side-chain high-pass,
  auto gain, parallel mix. Scrolling level / gain-change display with a draggable threshold and a knee display.
- **Space tab**: stereo room reverb modelled on ValhallaRoom's controls (mode, predelay, decay, depth, early and late
  sections, decay EQ with bass/high multipliers) plus Low Cut, Width and Ducking for kicks. Off by default.
- **EQ**: up to 24 bands (Pro-Q 4 limit) with add, delete and bypass; only bands in use are shown.
- Distortion units show a clear **BYPASSED** state with an ENABLE button.

### Changed
- Smaller Drive knobs in the distortion units and the saturation band panel.
- The host's tail length follows the reverb decay.

### Fixed
- Switching from *909 Clean* (both distortions bypassed on purpose) to another preset re-enables the distortion units
  (covered by an automated test).

## [1.0.0] - first test build
- Synth and 909 engines with key selection F–E, pitch and amp ADSR, two distortion units (Distroyer-style), a Pro-Q-style
  EQ with mid/side placement, multiband saturation (Saturn-style), scope page, tuner, presets, undo/redo, resizable UI.
