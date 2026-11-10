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
