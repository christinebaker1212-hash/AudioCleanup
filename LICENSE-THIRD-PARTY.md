# Third-party components

AudioFinisher is built on the following libraries, fetched at configure time
at pinned versions (see `cmake/Dependencies.cmake`).

| Component | Version | Use | License |
|---|---|---|---|
| JUCE | 8.0.15 | GUI, audio device I/O, WAV/FLAC/AIFF codecs | AGPLv3 or commercial JUCE licence |
| r8brain-free-src | 6.5 | Sample-rate conversion; its bundled PFFFT is used for STFT processing | MIT (PFFFT: BSD-like) |
| libebur128 | 1.2.6 | ITU-R BS.1770-4 / EBU R128 loudness, LRA and true-peak metering | MIT |
| RNNoise | 0.2 (release tarball incl. model) | Optional neural speech enhancement + voice-activity detection | BSD-3-Clause |
| minimp3 | commit ea99364 | MP3/MP2 decoding (MPEG-1/2/2.5, gapless) | CC0 1.0 |
| libogg | 1.3.6 | Ogg container for Opus decoding | BSD-3-Clause |
| Opus | 1.5.2 | Opus audio decoding | BSD-3-Clause |
| opusfile | 0.12 | Ogg Opus file decoding (HTTP support disabled) | BSD-3-Clause |
| LAME | 3.100 | MP3 encoding (libmp3lame, statically linked) | LGPL 2.0 or later |

JUCE is dual-licensed. Distributing binaries built from this repository
requires either complying with the AGPLv3 (publishing the source) or holding
a JUCE commercial licence. The DSP engine (`engine/`) does not depend on JUCE.

LAME is LGPL: libmp3lame is linked statically, and its unmodified source is
fetched from the pinned release by `cmake/Dependencies.cmake` (built with
`cmake/lame/config.h`), so users can rebuild and relink it with this
repository's source.

Test material used during development (not redistributed) came from the
librosa example-data repository: LibriSpeech excerpts (CC BY 4.0) and
Creative Commons / public-domain music recordings.
