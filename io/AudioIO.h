#pragma once
// Reading (all decoders bundled; the container is identified from the file's
// content, so a misnamed file still opens):
//   WAV, FLAC, AIFF   JUCE
//   MP3 (and MP2)     minimp3: MPEG-1/2/2.5, CBR/VBR with or without a Xing
//                     header, ID3v1/v2/APE tags, gapless (LAME encoder delay
//                     and padding removed)
//   Ogg Vorbis        JUCE (libvorbis)
//   Ogg Opus / .opus  opusfile (decodes at 48 kHz, header gain applied)
// Writing: WAV / FLAC (integer formats quantised by the engine's own dither, so
// preview and export match exactly), Ogg Vorbis and MP3 (LAME; lossy, from float).

#include <ac/AudioBuffer.h>
#include <ac/Dither.h>

#include <string>
#include <vector>

namespace af {

enum class FileFormat { Wav, Flac, Ogg, Mp3 };

struct ExportOptions
{
    FileFormat format = FileFormat::Wav;
    int bitDepth = 24;            ///< 16, 24 or 32 (32 = IEEE float, WAV only); ignored for Ogg
    ac::DitherType dither = ac::DitherType::Tpdf; ///< ignored for Ogg (lossy encoder works from float)
    int oggQualityIndex = 8;      ///< index into Ogg Vorbis quality options (8 = ~256 kbps)
    int mp3QualityIndex = 0;      ///< index into mp3QualityOptions() (0 = 320 kbps CBR)
};

/** Extension (with dot) for an export format. */
const char* extensionFor(FileFormat f);
/** Ogg Vorbis quality option labels (e.g. "256 kbps"). */
std::vector<std::string> oggQualityOptions();
/** MP3 quality option labels, best first ("320 kbps CBR", "V0 VBR (~245 kbps)", ...). */
std::vector<std::string> mp3QualityOptions();
/** True for lossy export formats (bit depth and dither do not apply). */
inline bool isLossy(FileFormat f) { return f == FileFormat::Ogg || f == FileFormat::Mp3; }

struct SourceInfo
{
    double sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    std::string formatName;
};

/** Load any supported file into float. Returns false with message on failure. */
bool loadAudio(const std::string& path, ac::AudioBuffer& out, std::string& error, SourceInfo* info = nullptr);

/** Write the buffer; integer formats get engine dither. MP3 accepts mono or
    stereo; rates LAME cannot encode (above 48 kHz) are resampled to 44.1 or
    48 kHz first. */
bool saveAudio(const std::string& path, const ac::AudioBuffer& b, const ExportOptions& opt, std::string& error);

/** File extensions accepted for import, as a wildcard ("*.wav;*.flac;..."). */
std::string supportedWildcard();
/** True if the file extension is one we can import. */
bool isSupportedExtension(const std::string& path);

} // namespace af
