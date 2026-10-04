# Listening evaluation procedure

The automated suite proves signal integrity and measured behaviour. Sound quality has to be
judged by ears. This procedure keeps the judgement fair.

## 1. Build a blind, loudness-matched set

```sh
python3 tools/make_listening_set.py --cli build/cli/af_cli_artefacts/Release/af_cli \
    --out listening/ \
    voice.natural:dialog_room.wav voice.rescue:noisy_phone.wav \
    sfx.punchy:impact.wav sfx.ambience:roomtone.wav \
    music.transparent:mix.wav music.loud:edm.wav
```

For each item this writes `NN_A.wav` / `NN_B.wav`: the processed render and the original,
latency-aligned and gain-matched to the processed integrated loudness, in random order. It also
writes `NN_removed.wav` (what cleanup took out), `NN_plan.txt` (every decision), `index.html`
(players plus preference and notes fields) and `key.json` (the answers, kept closed until
scoring).

In the GUI the same comparison is **Processed / Original** with *Loudness-matched A/B* on. The
louder signal is attenuated, so neither one clips.

## 2. What to listen for

| Family | Must preserve | Typical failure to flag |
|---|---|---|
| Voice | breaths, consonants (t/k/s), vocal identity (low end), room continuity between phrases | musical noise, "underwater" vowels, lisping from de-ess, pumping floor, dull top |
| Sound effects | attack shape, tail and decay, stereo image, LF weight of impacts | softened transients, truncated tails, gating chatter |
| Music | stereo image, groove and micro-dynamics, low-end balance, cymbal detail | pumping, flattened transients, harshness, smeared bass |

Also audition `NN_removed.wav`. It should contain noise, hum and clicks, not words, notes or
transients.

## 3. Scoring

- Rate each pair as A, B or no difference, with a short note. Score at least 3 listeners, or one
  engineer in 2 sessions.
- A preset passes when the processed version is preferred or tied in at least 80 % of items of
  its family, and no item shows a "must preserve" failure.

## 4. Reference outputs

When an engineer-approved reference render exists for a source:

```sh
af_cli compare our_render.wav approved_reference.wav
```

This aligns the two files, reports loudness/LRA/true-peak/PLR deltas, the 1/3-octave balance
difference after loudness matching, stereo correlation, and the residual level. Treat > 1 dB rms
balance deviation, > 1 LU loudness error or > 2 LU LRA difference as a mismatch to investigate.
