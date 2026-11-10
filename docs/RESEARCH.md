# Research notes: oldskool hardstyle kicks and the 909

Background for the design of Codex's 909 engine, distortion chain and key handling.

## The 909 bass drum
- The TR-909 kick is a **sine-like oscillator plus a noisy click**, shaped by three envelopes: pitch, amplitude and the click. The pitch envelope's decay rate is **fixed** on the original; *Tune* changes how far the pitch sweeps, and *Decay* sets the amplitude decay ([Waldorf manual](https://www.manualsdir.com/manuals/743343/waldorf-edition.html?page=27)).
- Roland built the **attack and the resonant body as separate circuits** ([Waldorf manual](https://www.manualsdir.com/manuals/743343/waldorf-edition.html?page=27)). Analog clones expose *Attack, Decay, Tune, Accent, Level*, and the Tiptop BD909 adds a *Tune Attack* control ([Tiptop BD909](https://www.kraftmusic.com/tiptop-audio-bd909-analog-bass-drum-module.html)).
- Part of the 909's punch comes from a slightly **longer pitch-envelope decay** than an 808 ([MusicRadar](https://musicradar.com/how-to/how-to-recreate-classic-analogue-drum-sounds-in-your-daw-and-with-hardware)).
- Typical recreations sweep from about **150–200 Hz down to 45–55 Hz** over 30–60 ms, with a 300–450 ms amp decay and a short 1–5 ms click ([MusicRadar](https://musicradar.com/how-to/how-to-recreate-classic-analogue-drum-sounds-in-your-daw-and-with-hardware), [DEV Community](https://dev.to/sendotltd/sample-free-drum-synthesis-in-web-audio-building-kick-snare-and-hi-hat-from-oscillators-in-60-2c0k)).

**In Codex:** the 909 engine is a shaped triangle (shape 1.0 = sine) with a two-stage exponential pitch drop, a pulse + high-passed noise attack, and an exponential decay. Its front panel copies the original's **Tune / Attack / Decay**. One deliberate change from the hardware: the sweep **lands exactly on the root** within about 4 time constants, so the tail is in key (the tuner confirms ±3 cents).

### 909 models
The 10 models are my own starting points, not measured from a real unit. Each one stands for a direction producers commonly pushed the 909 in:

| Model | Idea |
|---|---|
| Classic | Stock 909: medium sweep, medium attack, ~450 ms decay |
| Long Tail | Long, clean tail for euphoric kicks |
| Punch | Short and tight with a strong attack (raw / tok) |
| Deep | Small sweep, round, soft attack |
| Hard Attack | Attack and noise maxed |
| Gabber | Big sweep, short, saturated circuit (the "overdrive a 909 beyond recognition" approach) |
| Early Hardstyle | Mid-length, a little overdriven: the early-2000s starting point |
| Boom | Slower sweep, long decay |
| Driven | Heavily saturated circuit |
| Short Tok | Very fast sweep, very short decay |

## Old-school hardstyle kick method
- The foundation is a **909 kick** ([EDMProd](https://www.edmprod.com/how-to-make-hardstyle/)).
- Use **alternating EQ and distortion**: each EQ changes how the next distortion reacts ([Native Instruments](https://www.native-instruments.com/blogs/blog/hardstyle-music)). NI's example cuts around **50 Hz and 170 Hz** and boosts around **383 Hz** before distorting.
- One producer's chain: saturation → mid-range EQ (to bring out the transient) → distortion → EQ to remove a resonance → compression ([eMastered](https://emastered.com/blog/how-to-make-hardstyle)).
- Old-school gabber: **overdrive the 909 hard, cut mids, boost bass** ([Renoise forum](https://forum.renoise.com/t/old-school-gabber-kicks-production-method/43338)).

**In Codex:** the fixed chain is *Distortion 1 → EQ → Distortion 2*, the classic order. Several factory presets use the NI EQ moves directly ("909 Oldskool Hardstyle", "909 Rawstyle"). The gabber presets use the cut-mids / boost-bass approach.

## Tuning / key
- Tune the kick to the **root of the track**. Reference roots: F1 ≈ 43.7 Hz, A1 = 55 Hz ([BeatKey](https://beatkey.pages.dev/how-to-make-hardstyle-music)).
- Tuning to the root can cause phase issues against the bass; try a note up or down, the third or the fifth ([Fader Pro](https://blog.faderpro.com/?p=1215)).

**In Codex:** *Key* runs **F to E**: F1 (43.65 Hz) up to E2 (82.41 Hz), the octave that hardstyle kick roots sit in. *Oct* shifts it by an octave. *Mode* = Fixed Key (any MIDI note plays the selected key) or Follow MIDI. Presets never change the key.

## Other kick synths (feature comparison)
- **Kick 3 (Sonic Academy):** dedicated kick synth; node-based pitch and amp envelopes ([MusicTech](https://musictech.com/guides/buyers-guide/best-kick-drum-plugins-2026-tested-reviewed), [Dubspot](https://blog.dubspot.com/plugins/compare/kick-2-vs-substance)).
- **Punchbox (D16):** analog-style and experimental; sample layers for the click ([Splice](https://splice.com/blog/sonic-academy-kick2-vs-d16-punchbox)). Its synth part is a single sine oscillator with Punch and Spike controls ([Pro Tools Expert](https://www.pro-tools-expert.com/production-expert-1/5-awesome-kick-drum-plugins-worth-having-on-your-radar)).
- **Hardstyle-specific tools:** Nimble Kick (a sampler for pitching hardstyle kicks, with tail separation) and Lussive Audio's HardKick, a native plugin built for hard-kick production ([MusicTech](https://musictech.com/guides/buyers-guide/best-kick-drum-plugins-2026-tested-reviewed)). That name clash is why this project was renamed from "HardKick" to **Codex**.

**What Codex takes from these:** a built-in tuner, keeping the key separate from the sound, and a dedicated chain instead of generic sample layers. Unlike the samplers, nothing here is sample-based.

## Distortion section: Cubase Distroyer as a reference
Steinberg's Distroyer (introduced in Cubase 10) covers everything from smooth overdrive to extreme clipping ([Steinberg plug-in reference](https://archive.steinberg.help/nuendo_plugin_reference/v13/en/_shared/topics/plug_ref/distroyer_r.html)). Its documented controls:
- **Characteristic**: low values give a smooth overdrive; higher values push the waveform towards a rectangle for extreme distortion.
- **Offset**: changes the symmetry by moving the operating point of the curve. **DC Filter** removes the DC offset that large Offset values cause.
- **Lo Filter**: a low-pass on the wet signal *before* distortion. **Hi Filter**: a high-pass on the distorted wet signal. Sound on Sound calls these the most crucial controls ([Sound on Sound](https://www.soundonsound.com/cubase-distroyer-plugin)).
- **Boost** (on dry and wet), **Oversampling**, plus **Shelf Freq / Gain**, **Tone**, **Spatial**, **Mix** and **Output** in recent versions ([Cubase 14 reference](https://www.steinberg.help/r/cubase-pro/cubaseplugref/14.0/en/_shared/topics/plug_ref/distroyer_r.html)).

**In Codex:** each distortion stage follows that signal flow (Boost → Lo Filter → Drive/curve/Offset → Characteristic → Hi Filter → Shelf → Mix → Output → DC Filter) in its own blue-themed design. Codex adds a selectable base curve (12 types) under the Characteristic morph, and a live transfer-curve display. *Spatial* is left out because the kick is mono. The full Steinberg and Sound on Sound pages were blocked from the build environment, so this section is based on search summaries of those pages.

## Mid/side EQ: FabFilter Pro-Q 3 as a reference
- Pro-Q 3 sets the stereo channel **per band**: Stereo, Left, Right, Mid or Side. Pro-Q 2 only had one L/R or M/S mode for the whole instance ([MusicRadar](https://musicradar.com/news/fabfilter-pro-q-3-is-coming-and-you-dont-have-long-to-wait-for-it), [Sage Audio](https://www.sageaudio.com/blog/audio-plugins/fabfilter-pro-q-3-10-things-you-need-to-know.php)).
- The placement is set from the band's right-click menu ("Stereo Placement"). A classic use is a Side-only low cut to keep the low end mono ([Sage Audio](https://www.sageaudio.com/blog/audio-plugins/fabfilter-pro-q-3-10-things-you-need-to-know.php)).

**In Codex:** each EQ band has a Stereo/Left/Right/Mid/Side placement (band strip, node right-click menu and a badge on the node). The display draws the Mid curve and, when any band is Mid- or Side-only, a dashed Side curve.
Because a kick is generated in mono, the distortion units also gained Distroyer's **Spatial** control, which distorts left and right differently. That is what creates stereo content for the M/S EQ, saturation and stereo scope to work on.

## Multiband saturation: FabFilter Saturn 1 / 2 as a reference
- Saturn 2: up to **6 bands**, crossover slopes **6, 12, 24 and 48 dB/oct**. Per band: drive, mix, feedback, dynamics, tone and level. There are 28 styles grouped as tube, tape, transformer, amp and FX; Saturn 1 had 16 ([FabFilter help](https://www.fabfilter.com/help/saturn), [MusicTech](https://www.musictech.net/news/fabfilter-saturn-2/), [Magnetic Mag](https://magneticmag.com/2023/02/fabfilters-saturn-2-review/)).

**In Codex:** the same band structure and per-band controls (plus mute/solo/bypass), with 19 original styles in the same categories (names and curves are Codex's own). Crossovers are Linkwitz-Riley with all-pass compensation; the test tool verifies that bypassed bands sum flat at every slope.

## Scope page: Vengeance VPS Scope as a reference
- VPS Scope combines an oscilloscope (synced or free running, zoomable), a spectrum analyzer and a stereo field view (vector-scope, correlation, M/S modes) ([KVR](https://www.kvraudio.com/product/vps-scope-by-vengeance-sound), [MusicRadar](https://musicradar.com/computermusic/free-pc-mac-analyser-plugin-vengeance-sound-scope-618493)).

**In Codex:** a hit-synced or free-running oscilloscope (100 ms – 2 s, L/R/M/S), a vectorscope with correlation and width, a spectrum with slope and peak hold, a tuner and L/R level meters.

The FabFilter help pages could not be opened from the build environment (blocked host), so these notes are based on search results quoting them, plus reviews.

## Update: built against the Pro-Q 4 and Saturn 2 user manuals
The official manuals were supplied, so the EQ and saturation sections were checked against them directly.

**Pro-Q 4 manual** (Display and workflow, Band controls, Stereo options, Spectrum analyzer, Output options):
- Shapes: Bell, Low/High Shelf, Low/High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt, All Pass. *Codex: all ten.*
- Slope from 0 to 96 dB/oct (brickwall for cuts), fractional slopes. *Codex: 6, 12, 18, 24, 36, 48, 72, 96 dB/oct; no brickwall or fractional slopes.*
- Gain ±30 dB; display ranges 3/6/12/30 dB that expand while dragging. *Codex: same.*
- Per-band stereo placement, a **split** button that turns a band into L+R or M+S, a result curve per channel group, "Reset Placement". *Codex: same, in the right-click menu.*
- Analyzer: Pre/Post, Freeze, range 60/90/120 dB, speed, tilt (4.5 dB/oct default). *Codex: same (no side-chain or external spectra).*
- Gain Scale. *Codex: yes.* Auto Gain and phase invert are not included.
- Mouse: Alt-click bypass, Ctrl/Cmd-drag for Q, wheel for slope on cut filters, frequency shown on hover. *Codex: yes.*
- Not included: dynamic EQ, spectral dynamics, EQ Sketch, EQ Match, linear/natural phase modes, character modes, piano display.

**Saturn 2 manual** (Interactive multi-band display, Band controls, Input and output controls):
- Up to 6 bands, add a band with "+" (it copies the band it splits), select, remove; drag level buttons (Alt: drive, Ctrl/Cmd-click: reset); drag crossovers; slope button on the crossover (6/12/24/48 dB/oct); solo/mute with Ctrl/Cmd-click for exclusive. *Codex: same; the slope is global.*
- Band controls: Enabled, Mix, Feedback amount + frequency, Dynamics (right = compression, left = gating/expansion), Style, Drive with pan ring (and automatic level compensation), Tone (bass, mid, treble, presence), Level with pan ring, Remove. *Codex: same.*
- Styles: Tube, Tape, Amplifier, Transformer, Smudge, Breakdown, Foldback, Rectify, Destroy. *Codex: 20 original styles in matching categories (Clip and FX cover foldback, rectify, smear, sub fuzz and a "Wreck" bit-crush/sample-rate/clip style).*
- Input/output: High Quality oversampling, Channel Mode Stereo or Mid/Side, input and output level, mix, restricted feedback (feedback reduced when the input is silent). *Codex: oversampling up to 8x, everything else included.*
- Not included: linear-phase crossovers, "Superb" 32x oversampling, the modulation system (XLFO, envelopes, MIDI sources, XY controllers), band presets.

## Distortion engine: synth-style workflow (Serum reference)
Feedback from testing was that the distortion "isn't comparable to Serum 2 yet". Xfer's own manual could not be
fetched from this build environment, so the comparison is based on the well-known layout of Serum's distortion
module: a set of curve modes (tube, clip, diode, folds, zero-square, downsample, asymmetric, rectify, a drawable
X-Shaper), a resonant LP / BP / HP filter that can sit before or after the curve, Drive and Mix. What Codex added:

| Area | Before | Now |
|---|---|---|
| Filter | fixed 12 dB low-pass before / high-pass after | resonant **Pre / Post** filter: LP 12/24, BP, HP 12/24, Notch (zero-delay SVF, Simper / Cytomic topology, safe under fast modulation; 24 dB modes use one resonant stage like an analog cascade; resonance gain compensation) |
| Curves | 12 fixed curves | + Diode 2, Zero-Square, Downsample, **X-Shaper** and **X-Shaper (Asym)** (8 points per half, Catmull-Rom, drawn in the transfer display, preset curves) |
| Aliasing | oversampling only (up to 8x) | **first-order antiderivative anti-aliasing** (Parker, Zambon, Välimäki, Bilbao: "Reducing the aliasing of nonlinear waveshaping using continuous-time convolution", DAFx-16) on every curve, plus 16x |

Measured with `tools/Render.cpp` (5 kHz sine, hard clip at +24 dB, aliasing that lands below 20 kHz, relative to the
harmonics; Blackman-Harris window, double-precision test tone):

| Oversampling | without ADAA | with ADAA |
|---|---|---|
| Off | -15.5 dB | -27.9 dB |
| 2x | -30.6 dB | -51.2 dB |
| 4x | -39.8 dB | -63.7 dB |
| 8x | -60.6 dB | -89.1 dB |
| 16x | -67.6 dB | -91.1 dB |

ADAA is implemented with a tabulated antiderivative per curve (cubic Hermite with the curve itself as the derivative,
linear extension beyond the table for saturating curves, wrapped for the periodic folds), so it works for every
mode including user-drawn X-Shaper curves.

## Compressor: FabFilter Pro-C 3 and the Teletronix / UAD LA-3A as references
From the Pro-C 3 manual (provided): the style list (Clean, Versatile, Smooth, Punch, Upward, TTM, Op-El, Vari-Mu,
Classic, Opto, Vocal, Mastering, Bus, Pumping), Threshold / Ratio / Knee (0-72 dB) / Range, Attack / Release /
Lookahead / Hold, Auto Gain, wet/dry. Codex implements the styles that matter for kicks (Clean, Punch, Smooth, Bus,
Mastering, Classic, Vari-Mu, Opto, Pumping, TTM) and, as requested, **downward, upward and two-way** compression per
band with up to 4 bands. From the LA-3A: a T4 optical cell style with a fixed ~10 ms attack, a two-stage
program-dependent release (fast first half, then 0.5-5 s depending on how long the cell was lit) and the
Compress / Limit switch. Static curves are verified to 0.1 dB in the test suite.

## Reverb: ValhallaRoom as a reference
Controls follow ValhallaRoom's layout (Mix, Predelay, Decay, High Cut, Depth, Early: Size / Cross / Send / Diffusion /
Mod, Late: Size / Cross / Diffusion / Mod, Decay EQ: Bass / High multipliers and crossovers, modes). The late field is
an 8-line Hadamard feedback delay network with modulated delays and a 3-band decay filter per line; measured RT60
matches the Decay setting within 6 %. Kick-specific additions: Low Cut (keeps the sub out of the reverb), Width and
Ducking. Sources: valhalladsp.com (ValhallaRoom manual pages), MusicRadar's ValhallaRoom review.

## EQ processing modes: Pro-Q 4 as a reference
Zero Latency (minimum phase), Natural Phase (here: the same filters at 2x with half-band IIR resampling, so cramping
near Nyquist disappears) and Linear Phase (FIR built from the exact IIR magnitude, Blackman-windowed, with a 2x2
matrix so Mid / Side / Left / Right bands work too; resolution Low to Maximum). Tests confirm identical magnitude in
all three modes and a symmetric impulse response in Linear Phase.

## Premium workflow features (overnight round)
Modelled on what dedicated kick synthesizers offer: breakpoint (draw) envelopes for pitch and amp, a clean sub layer
with automatic phase alignment, drag-the-kick-into-the-DAW export, macros, A/B compare, a categorised preset browser
with favourites, an oversampled output clipper and a CPU readout. Typical presets use 2-3 % of one CPU core; the
absolute worst case (every module on at maximum quality, two 24-band linear-phase EQs, 16x oversampling) about 50 %.
