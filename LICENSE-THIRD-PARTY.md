# Third-party components

AudioFinisher is built on the following libraries, fetched at configure time
at pinned versions (see `cmake/Dependencies.cmake`).

| Component | Version | Use | License |
|---|---|---|---|
| JUCE | 8.0.15 | GUI, audio device I/O, WAV/FLAC/AIFF codecs | AGPLv3 or commercial JUCE licence |
| r8brain-free-src | 6.5 | Sample-rate conversion; its bundled PFFFT is used for STFT processing | MIT (PFFFT: BSD-like) |
| libebur128 | 1.2.6 | ITU-R BS.1770-4 / EBU R128 loudness, LRA and true-peak metering | MIT |
| RNNoise | 0.2 (release tarball incl. model) | Optional neural speech enhancement + voice-activity detection | BSD-3-Clause |

JUCE is dual-licensed. Distributing binaries built from this repository
requires either complying with the AGPLv3 (publishing the source) or holding
a JUCE commercial licence. The DSP engine (`engine/`) does not depend on JUCE.

Test material used during development (not redistributed) came from the
librosa example-data repository: LibriSpeech excerpts (CC BY 4.0) and
Creative Commons / public-domain music recordings.
