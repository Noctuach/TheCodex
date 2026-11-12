# Release checklist

Things to settle before publishing Codex.

## 1. Name and branding
- [ ] **Check the name.** "Codex" is a common name, including OpenAI's Codex. Search the plugin stores (KVR,
      Plugin Boutique) and your country's trademark register for existing audio products called Codex.
- [ ] **Set your developer name.** It's in `CMakeLists.txt` (or pass it with `-D...`):
  - `CODEX_COMPANY`: shown in DAWs, e.g. "Nocturnal Audio".
  - `CODEX_BUNDLE_ID`: reverse-DNS, no spaces, e.g. `com.yourname.codex`.
  - `CODEX_MANUFACTURER_CODE`: 4 characters, first letter upper case, unique to you.
  - Do not change these or `PLUGIN_CODE` after release, or DAWs will treat it as a different plugin.
- [ ] Version: `project(Codex VERSION x.y.z)` in `CMakeLists.txt`. The About box picks it up automatically.

## 2. Licences
- [ ] **JUCE.** JUCE 8 is dual-licensed. A free plugin can either:
  - be released as open source under AGPLv3, or
  - use JUCE's free "Starter" commercial licence (for small developers; check the revenue limit and terms on juce.com).
- [ ] **VST3 SDK.** Steinberg's VST3 SDK is bundled with JUCE. Check its current licence terms. To use the VST logo,
      follow Steinberg's usage guidelines.
- [ ] **Fonts.** Barlow is OFL: ship `OFL-Barlow.txt` with the plugin (CI already does this).
- [ ] Choose a licence for your own code and add a `LICENSE` file.

## 3. Builds
- [ ] Push to GitHub. Actions builds Windows / macOS / Linux, runs the tests and pluginval at strictness 7.
- [ ] Test the CI artefacts in at least two DAWs per platform (for example Ableton, FL Studio, Cubase, Reaper, Logic).
- [ ] **macOS.** Unsigned plugins are blocked by Gatekeeper. For a smooth install, sign with a Developer ID certificate
      and notarize (Apple Developer Program). Otherwise users have to allow the plugin in System Settings → Privacy & Security.
- [ ] **Windows.** A zip with `Codex.vst3` is enough (it goes in `C:\Program Files\Common Files\VST3`). An installer is
      optional (for example Inno Setup).

## 4. Publishing
- [ ] Release page with download links, screenshots (`docs/`), system requirements and the quick-start from the README.
- [ ] Factory presets: audition them all on studio monitors and adjust levels if needed.
