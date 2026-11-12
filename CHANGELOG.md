# Changelog

All notable changes to Codex. Versions follow [Semantic Versioning](https://semver.org).

## [Unreleased]

### Added
- New logo: an open book (a codex), with a kick's wave on the right page. Some say it hides a secret.
- Animation details: every hit lights the chain strip in processing order (the signal passing through) and pulses the logo; the tab underline slides to the selected tab and the page fades in; knob hover highlights fade in and out; overlays (preset browser, save, guide, about) fade in and out.
- 909 engine, after a second round of hardware research: **Tune** now works like the original (speed of the pitch drop); the old Tune is **Depth** (the classic Fraser / Whittle Tune Depth mod). New **Accent** (velocity 100+ hits louder and harder), **Overload** (waveshaper drive towards a square, after the Tiptop BD909 / Fraser drive mod) and **Decay Hold** (the body holds while the note is held). They live in the pitch panel with the 909 engine, with a live drawing of the sweep. Defaults match the previous sound, so sessions and presets are unchanged.
- Module order: drag the chips in the tab bar to reorder Space, Distortion 1, EQ 1, Distortion 2, Saturation, Compressor and EQ 2 (click a chip to open it, right-click for Modern / Classic). Reordering fades for 6 ms so it never clicks, and the order is saved with sessions, presets, A/B and undo.
- New default order (Modern): Space first, so the distortions build the reverb into the tail. Old sessions and user presets keep the Classic order (Space last) and sound unchanged.
- Preset "Reverb-First Rawstyle".

### Changed
- New colour scheme: true black with a sun-orange accent (peach for the pitch envelope, amber instead of green, warm tones for the bands); the tuner turns orange when the kick is in key.
- Premium finish: machined knobs (lit body, drop shadow, glowing value arc), panels with a soft shadow, lit top edge and an accent marker, a fading sun-orange line under the header, glowing tab underline and selected buttons.
- Space Mix keeps the dry kick at full level up to 50 % (it used to fall to -3 dB at 50 % and silence at 100 %, which made the kick fade when the reverb was turned up).

### Fixed
- Linux: a rare crash of hosts built on JUCE (pluginval among them) when the last Codex instance was removed. The cause is a use-after-free in JUCE's Linux VST3 host run loop, reproduced under AddressSanitizer. Codex no longer runs an always-on 20 Hz timer, which made it about twice as likely; latency changes are now sent to the host only when they happen. CI retries pluginval once on Linux if, and only if, the validator segfaults.

### Added (earlier)
- Distortion **Auto Gain** (AUTO button in each unit's header): the output follows the input level, so Drive changes
  the character instead of the loudness (Drive 6 -> 30 dB: -0.1 dB instead of +5.5 dB).
- Distortion oversampling option **Raw**: no anti-aliasing at all, for the gritty digital sound of classic drum
  machines (inspired by Sonic Charge Microtonic, whose distortion aliases on purpose).
- Quick guide (menu), keyboard shortcuts, and a "*" marker on the preset name when the sound has been edited.
- **Sub layer** (kick page): a clean sine on the oscillator's own phase (follows the sweep), faded in after the punch,
  low-passed and added after all effects, delayed by the chain latency so it stays aligned. **ALIGN** finds the best
  phase automatically (on 909 Rawstyle: +9.8 dB of clean fundamental).
- **Velocity** sensitivity.
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
- **Punch / tail split** per distortion unit: *Tail Drive* changes the drive for the tail relative to the punch,
  reached *Split* ms after each hit (sample-accurate; e.g. +18 dB adds 18 dB of tail harmonics while the punch stays
  identical). The transfer display shows the tail's curve dashed.
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
- Factory presets are now loudness-matched (K-weighted short-term loudness) instead of peak-matched. Switching presets used to jump by up to 16 dB in perceived level (e.g. 909 Rawstyle was 9 dB quieter than 909 Euphoric at the same peak); 20 of 24 presets now sit within 0.1 dB of each other, and the rest within 3 dB, all peaking below -1 dBFS. 909 Rawstyle and Upward Tail Crunch use the output clipper to tame their spiky transients.
- Envelope display could build a path from NaN coordinates on its first paint (found by the new sanitizer CI job).
- Tuner read up to 22 cents sharp on heavily distorted kicks (bit-crushed, rawstyle): the pitch estimate is now refined on the windowed spectrum, and matches an FFT reference within 0.1 cent on every preset. The 909 presets were in tune all along.
- 909 pitch sweep lands on the root sooner (floor level 0.06), so short kicks settle in key.
- **No more clicks when parameters move during playback.** An automated audit jumps all 421 continuous parameters
  mid-note; 18 clicked (distortion Hi Filter / Shelf Gain / Tail Drive, saturation crossovers, Treble, Presence,
  Dynamics, Level Pan, compressor Output / Gain / Ratio). Filters and crossovers now glide, gains are smoothed and
  the compressor's curve parameters move smoothly; the audit now runs in CI and fails on any click.
- Closing the editor could touch freed memory (Compressor page: button attachments destroyed after their buttons),
  a possible crash or hang when the plugin window closes. Found with AddressSanitizer; the full test suite now runs
  clean under it.
- **Mix knobs no longer notch.** The multiband saturation's Mix blended the dry signal with bands that had passed
  through the crossovers' phase shift: at 50 % it cut up to 26 dB at a crossover (400 Hz) and 4-6 dB elsewhere. The
  distortion units' Mix did the same around the Lo / Hi filters (4 dB at 30 Hz with the Hi Filter at 40 Hz). The dry
  signal now gets exactly the wet path's phase (crossover all-passes; Linkwitz-Riley Lo / Hi filters with matching
  all-passes; a half-sample all-pass for the anti-aliasing delay), so any Mix setting blends cleanly (within 0.1 dB).
  The compressor's Mix had the same problem with 2-4 bands (up to 25 dB at a crossover) and is fixed the same way.
- Linear-phase EQ rebuilt on a custom partitioned convolver: filters are designed on a worker thread and swapped in
  on the audio thread without losing the input history, so editing a band during playback is seamless and the
  reported latency always matches the running filter (previously the filter swap could briefly drop audio).
- Kick export and sub ALIGN wait for the linear-phase filters and measure after the chain latency.
- Sub layer delay buffer sized for the worst-case chain latency; velocity applied sample-accurately at note-on.
- Host latency changes are applied on the message thread; no allocation or blocking calls on the audio thread from
  the ADAA tables or the EQ worker; Bitcrush excluded from ADAA; clipper oversampler reset when re-enabled.
- Switching from *909 Clean* (both distortions bypassed on purpose) to another preset re-enables the distortion units
  (covered by an automated test).

## [1.0.0] - first test build
- Synth and 909 engines with key selection F–E, pitch and amp ADSR, two distortion units (Distroyer-style), a Pro-Q-style
  EQ with mid/side placement, multiband saturation (Saturn-style), scope page, tuner, presets, undo/redo, resizable UI.
