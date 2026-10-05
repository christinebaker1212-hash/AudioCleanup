# Capabilities: implemented, verified, and known limits

Status key:
- **Verified (automated):** covered by `tests/` with measured numbers. Raw output is in
  [`VALIDATION-REPORT.txt`](VALIDATION-REPORT.txt).
- **Verified (real material):** checked by rendering CC/public-domain speech, music and effects
  and inspecting measurements, plots and screenshots. This does not replace listening.
- **Implemented:** working code path that is exercised, without a dedicated pass/fail metric.

Nothing advertised in the UI is a placeholder. Every stage listed in the advanced panel runs
the algorithm named here.

## Signal integrity

| Capability | Implementation | Status / result |
|---|---|---|
| Floating-point processing | 32-bit float buffers, double-precision filter states and accumulators, no internal clipping | Verified: a 32-bit float export round trip is bit-exact |
| Bypass transparency | Every neutral processor is bit-exact; all sections off = exact pass-through | Verified: max diff 0 (stereo M/S 1e-7) |
| Latency compensation | Each stage reports latency; renderer removes it per stage | Verified: an impulse stays on its sample through limiter, compressor lookahead, saturator, de-esser, plosive; 0-sample lag for the full voice chain incl. 48→44.1 kHz |
| Block-boundary safety | Per-sample processing, absolute-time parameter updates | Verified: bit-identical output with block sizes 1, 777 and 4096 for 10 processors |
| Tails | `tail()` per processor, rendered and trimmed below −120 dBFS | Implemented; length bounded (tests) |
| Parameter smoothing | TPT SVF filters modulated per sample (dynamic EQ, de-esser); smoothed gain envelopes | Verified via block invariance and EQ tests |
| Channel layout | Mono, stereo, N-channel preserved (5.1 loudness weighting) | Verified: 1/2/5-channel in = out |
| Silence / very short clips | Silent input passes untouched; < 400 ms clips fall back from undefined LUFS to energy/peak metrics | Verified: 80 ms clip delivers under the ceiling in every family |
| Originals preserved | Sources never written; export refuses to overwrite the source | Implemented |

## Metering and delivery

| Capability | Implementation | Status / result |
|---|---|---|
| Loudness (BS.1770-4 / EBU R128) | libebur128: integrated, LRA, momentary, short-term | Verified: EBU Tech 3341 stereo sine −23/−20 dBFS read −22.99/−19.99 LUFS |
| True peak | 4× oversampled (libebur128 meter + own 4× polyphase detector for the limiter) | Verified: fs/4 45° sine reads within 0.11 dB of the true peak; the detector never under-reads |
| True-peak limiter | 4× oversampled detection, sliding-minimum hold + matched box filter (the gain is down before the peak), program-dependent release, linked | Verified: ≤ 0.008 dB overshoot on music, fs/4 sines, 19 kHz bursts, squares and impacts at −1 and −0.1 dBTP; bit-exact below the ceiling; THD+N −152 dB on a sustained over |
| Soft clipper (Loud/Dense only) | 4× oversampled tanh-knee clipper ahead of the limiter. Engages only when the limiter bound alone cannot reach the target; clip and limiter gain reduction are bounded separately (P99) and found by bracketed search | Verified (real material): a jazz track reaches −9.05 LUFS with clip 2.9 dB and limiter 3.5 dB P99; a very dynamic drum & bass track stops at −11.2 LUFS at the 3 dB clip bound and is reported as not reached |
| Loudness targeting loop | Render → measure → correct gain/ceiling (≤ 6 passes); compressor feedback when the limiter bound would be exceeded; honest "not reached" reporting | Verified: all 15 presets on synthetic material and real speech/music |
| Resampling | r8brain-free-src, 24-bit profile (180 dB), linear phase | Verified: 44.1↔48↔96 kHz level ±0.0000 dB, THD+N ≤ −150 dB, timing exact |
| Dither / export | TPDF (2 LSB p-p) and 3-tap F-weighted noise shaping (44.1/48 kHz); WAV 16/24/32f, FLAC 16/24; Ogg Vorbis and MP3 (LAME 3.100, CBR/VBR, Xing/LAME tag) from float | Verified: TPDF level −96.35 dBFS (theory −96.33); shaping lowers 3 kHz error 4.9 dB; 24-bit WAV/FLAC round trip identical to the engine quantiser |
| Import | Container identified from content (RIFF/RF64, FLAC, AIFF, Ogg, MPEG); MP3/MP2 via minimp3 decoding every frame (no length estimate), following mono/stereo switches, honouring LAME/Xing gapless delay and padding; Ogg Opus via opusfile (48 kHz, header gain); clear messages for M4A/AAC, WMA, Ogg FLAC/Speex | Verified: MPEG-2 22.05 kHz VBR without a Xing header and with ID3v1 opens complete (it failed before); a VBR file without a Xing header no longer truncates (0.99 s → 5.04 s); LAME-tagged MP3 is sample-exact (88200 frames for 2 s); Opus fixture exactly 48000 frames, level within 0.03 dB; WAV named .mp3 bit-exact |
| MP3 export | LAME 3.100, quality 2, joint stereo, Xing/LAME tag written after encoding; rates above 48 kHz resampled first with r8brain | Verified: round trip at 44.1/48/96 kHz is sample-count exact, 0-sample lag, level within 0.01 dB at 320 kbps/V0 |
| Preview = export | Preview plays the same rendered float buffer; export quantises it with the same deterministic dither | Verified (bit-exact) |

## Restoration

| Processor | Algorithm | Status / result |
|---|---|---|
| DC removal | Exact subtraction of the measured per-channel offset | Verified (residual < 1e-6) |
| High/low-pass | Butterworth cascades, orders 1–8 | Verified: −3.01 dB at fc; response matches theory to 0.01 dB (order 8 down to −96 dB) |
| Hum removal | Long-window Welch spectrum; 50/60 Hz family; f0 refined from low harmonics; only peaks at exact multiples (±0.5 Hz + 0.03 Hz·k) notched, depth = measured prominence + 3 dB | Verified: 60 Hz and drifting 50.3 Hz found to ±0.07 Hz, 21–52 dB reduction, 1 kHz programme tone untouched (±0.05 dB), no false detection on clean speech; real hum found in a LibriSpeech file and confirmed independently |
| Noise profile | Quietest frames ranked by mean log-spectrum, per-bin median/ln 2 (unbiased), optional user-selected region | Verified: white −55.05 / pink −55.7 dBFS for a −55 dBFS floor |
| Spectral denoiser | STFT (≈43 ms, 75 % overlap, sqrt-Hann), decision-directed a-priori SNR, MMSE log-spectral amplitude gain, gain floor = attenuation bound, constant-Q frequency smoothing, release hold only while programme is present, linked stereo gains, optional minimum-statistics tracking | Verified: 12–13 dB reduction in gaps against a 15 dB bound (never exceeds it); active-level change ≤ 0.14 dB; speech leakage into the removed signal ≤ 2 %; residual spectral roughness equal to the original noise (no musical noise); adaptive mode follows a +12 dB noise step |
| Denoise artifact control | Measures loudest-frame energy loss after denoise and re-renders gentler if it exceeds the preset bound | Implemented, logged in the decision log |
| Neural speech enhancer | RNNoise 0.2 (bundled weights), 48 kHz internal via r8brain, attenuation-bounded wet/dry | Verified: 0-sample alignment at 48 and 44.1 kHz; 27 dB gap-noise reduction. Speech only (not offered for SFX/music) |
| De-click | AR(p) forward/backward residual detection, periodicity test (rejects glottal/periodic excitation), robust onset protection, edge refinement, least-squares AR interpolation; category-specific criteria (music: ≤ 0.5 ms, strong, non-onset) | Verified: 29/30 inserted clicks restored to −68…−150 dB error (the 30th sits 2 ms after a drum onset and is deliberately left); 0–1 repairs on clean speech/impacts; on real music, false detections cut from 42/min to 0–2.6/min on 5 of 6 tracks (the remaining one has genuine glitches) |
| De-clip | Detection: flat runs at each polarity's extreme, judged separately (asymmetric clipping) and down to −50 dBFS (clipped takes turned down later). Reconstruction: A-SPADE sparse declipping (Kitić et al. 2015; Záviška et al. 2019): Hann-windowed 23 ms frames, 75 % overlap, 2×-redundant DFT, hard-thresholding with growing sparsity alternated with projection onto the consistent set (reliable samples exact, clipped samples beyond the clip level); runs up to 10 ms, peaks bounded to +12 dB; multithreaded. Runs first in the chain, on the raw samples | Verified: two-sine test −29.7 → −53.1 dB error. SDR on real recordings clipped 6 / 10 / 14 dB below peak: solo trumpet 15.6 / 9.5 / 6.0 → **50.4 / 34.0 / 25.3 dB** (previous AR method 24.9 / 13.6 / 8.6); string orchestra 28.5 / 18.2 / 11.3 → 38.6 / 28.3 / 20.6; ragtime piano 24.7 / 14.7 / 9.1 → 40.1 / 27.0 / 20.3; drum & bass 23.4 / 14.0 / 8.7 → 24.6 / 18.5 / 14.9; speech 18.1 / 11.1 / 6.9 → 22.2 / 19.9 / 14.8. At 3 dB both methods exceed 28 dB SDR. A positive-only clip at −26 dBFS is detected at the exact level with the negative half untouched |
| Plosive control | Sub-F0 / voice-band (300 Hz–3 kHz) ratio and LF onset both relative to running baselines; complementary low-band attenuation only during bursts (bit-transparent when idle) | Verified: synthetic pops detected 5/5, LF reduced 7 dB; 0 events on clean speech |
| De-esser | Sibilance-to-voice ratio detector (level-independent), thresholds from the measured ratio distribution, per-sample-modulated SVF shelf or bell with lookahead | Verified: sibilant bursts −6 dB, voiced passages ±0.02 dB |
| De-reverb | Late-reverb PSD prediction (Lebart / Habets) from blind RT60, bounded LSA gain | Verified: tail in gaps −8 dB, active level −1 dB. RT60 estimate is coarse (0.8 s room read as 1.28 s); treat as approximate |

## Tone and dynamics

| Processor | Algorithm | Status / result |
|---|---|---|
| Parametric EQ | Cytomic TPT SVF bell / shelf | Verified: measured = analytic within 0.0000 dB, 20 Hz–22 kHz |
| Corrective EQ (adaptive) | 1/3-octave LTAS vs category target + preset intent, partial correction, bounded boost/cut, no boosts near the noise floor or into absent content, voice fundamental region untouched, intent guards (e.g. warm presets never brighten), no opposing overlapping bands, resonance cuts from 1/12-octave LTAS | Verified (real material): broad moves within bounds; 0.7–1.6 dB rms change after loudness matching on 3 commercial-style tracks |
| Dynamic EQ | Per-band SVF detector, threshold from the band's measured P75 level, bounded range | Verified: 6.00 dB cut at a 6 dB range; transparent below threshold |
| Compressor | Feed-forward log-domain, soft knee, smooth-branching attack/release, peak/RMS detector, stereo link 0–100 %, sidechain HPF, lookahead, parallel mix; threshold derived from detector statistics for a target GR | Verified: static curve exact; steady GR −7.500 dB; attack/release time constants within 0.01 dB of theory; linked = identical gains, unlinked leaves the quiet channel at 0.000 dB |
| Multiband | 3 bands, Linkwitz-Riley 24 dB/oct crossovers; the low band passes the upper crossover's allpass so the sum is a flat-magnitude allpass | Verified: ≤ 0.0000 dB deviation with all bands at unity |
| Resonance control (dynamic) | STFT, linked channels. Each bin is compared with the peaks around it: a moving maximum over ±1/12 octave (at least ±300 Hz), with the reference taken as the louder of the mean peak level to the left and right, out to ±1/2 octave. Only bins with context on both sides are judged. The excess above a threshold is reduced 3:1, bounded by depth, spread over the main lobe, with 5 ms attack and 70–100 ms release. A closed-loop guard halves depth if mean attenuation exceeds 1 dB. On in voice presets, Detailed Foley, Game Asset and the Warm/Punchy/Loud music presets; optional elsewhere | Verified: an intermittent resonance 12 dB above its neighbours is cut 4.0 dB (depth 4); a 30-harmonic series changes 0.00 dB; resonance-free passages 0.00 dB; block-size invariant. Finished commercial master: mean 0.04 dB, acting only around 2–4 kHz (≤ 0.6 dB per 1/3 octave) |
| Level rider | K-weighted 400 ms level, bounded correction, zero-phase (forward/backward) smoothing, frozen in pauses | Implemented; exercised in all voice presets |
| Expander | Linked downward expander with hold, bounded range (never a hard gate) | Verified: transparent above threshold |
| Transient shaper | Level-independent fast/slow envelope differences | Verified: transparent at 0 dB, block invariant |
| Saturation | 4× linear-phase polyphase FIR oversampling (Kaiser, 64 taps/phase, integer latency), unity-small-signal-gain tanh with even-harmonic asymmetry, DC blocker | Verified: passband 0.0001 dB at 15 kHz; aliased 5th harmonic at −122 dB |
| Tempo detection (music) | Log-magnitude spectral flux at 200 frames/s, mean-removed autocorrelation, 4-harmonic comb plus eighth-note support, broad prior at 120 BPM; confidence = normalised autocovariance at the beat lag | Verified: 92 and 128 BPM patterns exact; 174 BPM reported as 87 (half time, the same note grid); white noise confidence 0.01 (threshold 0.2). Real tracks are not independently cross-checked |
| Tempo-synced release (music) | Compressor, multiband band and limiter sustained releases snap to the nearest note length (1/32–1/2 note) when within ×1.6 of the preset value; skipped without a clear pulse; `--no-tempo-sync` disables | Verified: at 128 BPM compressor 250 → 234.4 ms (1/8 note), limiter 480 → 468.8 ms (1/4 note) |
| Stereo | M/S width, LR4 bass mono-isation (only when LF correlation < 0.2), voice balance correction, side-only EQ (width by frequency) and side low-cut (12 dB/oct; Warm 40 Hz, Punchy 60 Hz, Loud 80 Hz). Polarity fix: channels in opposite polarity (correlation < −0.5, also below 150 Hz) get the right channel inverted. Narrow-mix widening (music, no reference): side/mid above 300 Hz below −16 dB (finished mixes measure −2 to −18) lifts the side above 300 Hz by half the shortfall to −12 dB, at most 3 dB (1.5 dB Transparent/Dynamic); the mid, and so the mono fold-down, is untouched | Verified: transparent at width 1; side low-cut −17.1 dB at 30 Hz (theory 17.1) with the mid unchanged (max error 3e-8); anti-phase mix mono fold-down −16.1 → −0.1 dB, never triggered on a normal mix; narrow mix −30.1 → −27.2 dB side/mid, ordinary stereo left alone |

## Decision engine, presets, workflow

| Capability | Status |
|---|---|
| Analysis: loudness, true peak, crest, PLR, DC, clipping, noise profile, SNR, hum, click rate, spectrum, tilt, rumble/subsonic, stereo correlation/width/balance/dual-mono, transients (attack/decay/onsets/peak-to-loudness), speech activity (RNNoise VAD), F0, sibilance distribution, plosives, RT60 | Implemented; core detectors verified above |
| Stage selection and settings derived from measurements, bounded by preset constraints, scaled by Cleanup/Tone/Dynamics macros; reasons and actual settings exposed | Implemented; visible in the GUI advanced panel and `--plan` |
| Separate control of cleanup / tone / dynamics / delivery | Implemented (section switches, macros, per-stage Auto/On/Bypass) |
| 15 presets (5 voice, 5 SFX, 5 music) with editable targets/ceilings; user presets (JSON) | Verified: delivery test over all presets; JSON round trip |
| Batch processing and Game Asset batch consistency (keeps a chosen fraction of each asset's deviation from the group median) | Verified (function); implemented in GUI and CLI |
| Aligned-stem mode: roles, gain, constant-power pan, 3 buses with measured glue compression, per-stem treatment, master via Music preset, A/B vs static mix | Verified: 0-sample alignment vs static mix; master under the ceiling |
| Loudness-matched, latency-aligned A/B; removed-noise audition | Verified: removed + output = input (< 1e-6) |
| Background processing, progress, cancel | Verified (cancellation test); GUI uses a worker thread |
| Reference comparison (`af_cli compare`) and blind listening sets (`tools/make_listening_set.py`) | Implemented |
| Reference matching (GUI **Match a reference...**, CLI `--match`): 1/3-octave level-neutral balance fit (≤ 5 bells, bounded), closed-loop residual correction after dynamics, loudness target taken from the reference, bounded M/S width match; amount 0–100 % | Verified: synthetic test 2.72 → 1.28 dB rms deviation, loudness within 0.11 LU, width moves toward the reference within bounds; real track 3.39 → 0.88 dB rms (generic preset: 2.87) |
| Reference width by frequency | Per-band side/mid of the reference vs the source: broadband width from the band average, then a bounded side EQ (+4/−6 dB, ≤ 4 bands, 100 Hz–12 kHz) for the remaining shape | Verified: wide-lows/narrow-highs source vs the opposite reference, 4.84 → 0.88 dB rms per-band deviation; earlier width test now −16.3 dB vs reference −16.5 |
| Reference dynamics (**Match its dynamics**, CLI `--no-match-dynamics` to disable): probes the delivered result (limiter at delivery rate and bounds) and closes the loop on LRA (slow zero-phase level riding, 3 s window / ~1 s glide, then bus compression) and short-term crest (transient emphasis, −3…+6 dB); keeps the best of ≤ 4 passes. A reference denser than the preset widens the limiter bound (hard cap 6 dB) and soft-clip bound (≤ 2.5 dB), logged | Verified: synthetic verse/chorus source LRA 5.8 → 2.1 LU vs reference 1.7 (5.8 without), loudness within 0.34 LU (2.8 LU short without); real track to a −8.7 LUFS master: −12.6 → −10.3 LUFS, LRA 3.1 → 2.4 vs 2.0. Cannot restore dynamics already limited away (stated in the log) |

## Known limits (stated plainly)

- **Listening validation is still to be done.** The objective tests above are necessary but not
  sufficient. Commercial-grade sound is an acceptance target that must be confirmed through
  blind, loudness-matched listening with [`LISTENING.md`](LISTENING.md). No engineer-approved
  reference outputs were supplied, so none were compared.
- **Loud/Dense and very dynamic material:** limiting (7 dB P99) and soft clipping (3 dB P99) are
  both bounded, so a very dynamic drum & bass track stops near −11 LUFS rather than −9. The report
  says so. Raise `maxClipDb` / `maxLimiterGrDb` in a user preset to trade density for loudness.
- **Reference dynamics** can only remove dynamics, never restore them: a source that was already
  limited harder than the reference stays denser. Matching a very loud reference is capped at
  6 dB of limiter gain reduction, so the result may land 1–2 dB quieter than the reference; the
  log reports the gap.
- **RT60 estimation** is blind and coarse; de-reverb strength is bounded to compensate.
- **Hum** harmonics masked by programme are left in place by design: depth never exceeds the
  measured prominence.
- **Neural enhancer** is RNNoise (small, fast, speech-only). DeepFilterNet3 (via ONNX Runtime) was
  integrated and measured on the same degraded-speech set (stationary noise, room, hum, phone band,
  clipping, babble, clatter, café): in this chain it matched RNNoise on DNSMOS (e.g. babble 1.74 vs
  1.75, café 2.02 vs 1.96) but cost intelligibility (babble STOI 0.627 vs 0.707), so it was not adopted.
- **Automatic EQ cannot tell a defect from a choice.** Finished commercial tracks deviate 10-50 dB from
  any single target curve at the extremes (solo trumpet, dark piano recordings), and stronger or
  trend-relative correction moved finished masters 1.5-2.6 dB rms away from themselves for a
  0.2-0.7 dB gain on deliberately damaged mixes. Tonal correction therefore stays deliberately
  partial; use **Match a reference** to steer it toward a sound you choose.
- **Hiss in music** is not reduced by default: the quietest frames of music are music, so the noise
  floor cannot be measured reliably without a dedicated high-band hiss detector.
- **Windows build:** produced by GitHub Actions (MSVC, windows-2022). The full validation suite
  passes there. The Windows GUI has not been operated interactively; the GUI was exercised
  headless on Linux (Xvfb) through its scripting options, with screenshots in `docs/screenshots`.
- **Stem alignment** assumes stems share a common start; there is no automatic offset detection.
- **Monitoring** resamples to the device rate with r8brain once per render. Changing the device
  rate re-prepares the monitor buffers.
- JUCE is AGPLv3/commercial; review before distributing binaries.
