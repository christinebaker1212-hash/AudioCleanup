# AudioFinisher v0.1.1

## Changes since v0.1.0

- **Display scaling:** the window now fits the screen at any Windows scaling
  (125 %, 150 %...). The right-hand panel scrolls when the window is short,
  the toolbar wraps on narrow windows, and a new **Interface size** setting
  (70-120 %, bottom of the right panel) shrinks or enlarges the whole UI on
  top of Windows scaling.
- **More formats:** imports Ogg Vorbis and MP3 (bundled decoders, nothing to
  install) and exports Ogg Vorbis (~256 kbps). MP3 decoding keeps the
  encoder's start padding (a few ms of leading silence).

---

v0.1.0 was the first release. Windows x64 desktop app for one-click restoration, mixing and
mastering of voice, sound effects and music. All processing is built in:
no plug-ins, DAW or cloud needed.

## Download

`AudioFinisher-win64.zip` contains:

- `AudioFinisher.exe`: the desktop application
- `af_cli.exe`: the command-line renderer (analyze / process / batch / compare)
- `af_tests.exe`: the DSP validation suite (run it to check your machine)
- README, capability list, listening procedure, validation report

The executables use a static runtime, so no Visual C++ redistributable is
needed. They are not code-signed, so Windows SmartScreen may warn on first
launch ("More info" → "Run anyway").

## What's in it

- Workflow: drag in audio → Voice Clip / Sound Effect / Music → preset →
  Process → loudness-matched, latency-aligned A/B (processed / original /
  removed noise) → export WAV 16/24/32-float or FLAC 16/24 with dither.
- Adaptive engine: analyse → select treatment → render → measure → bounded
  corrections. Every stage's decision, reason and actual settings are shown in
  the advanced panel.
- 15 presets (5 voice, 5 sound effect, 5 music), user presets, batch
  processing with game-asset consistency, aligned-stem mixer.
- Restoration: DC/high-pass, hum, MMSE-LSA spectral denoise, RNNoise speech
  enhancer, de-click, de-clip, plosive control, adaptive de-esser, de-reverb.
- Tone and dynamics: corrective and dynamic EQ, compressor, LR4 multiband,
  level rider, expander, transient shaper, oversampled saturation, bounded
  soft clipper (Loud/Dense), 4x oversampled lookahead true-peak limiter,
  BS.1770 loudness targeting, r8brain resampling.

## Verification

The validation suite (39 cases) passes on Windows (MSVC) and Linux (GCC) in
CI. Measured results are in `docs/CAPABILITIES.md` and
`docs/VALIDATION-REPORT.txt`.

## Known limits

- Sound quality has been measured but not yet validated by blind listening.
  Use `tools/make_listening_set.py` and `docs/LISTENING.md` to do that.
- The Windows GUI was built and tested in CI but not operated interactively
  before release.
- Very dynamic material may stop short of the Loud/Dense target, because
  limiting and clipping are bounded. The result reports this.
- Stems must be time-aligned; the room-reverb estimate is approximate.
- JUCE is AGPLv3 or commercial. See `LICENSE-THIRD-PARTY.md` before
  redistributing.
