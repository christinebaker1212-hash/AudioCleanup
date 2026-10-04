#pragma once
// Reading: WAV, FLAC, AIFF, Ogg Vorbis, MP3 (JUCE decoders, all bundled).
// Writing: WAV / FLAC (integer formats quantised by the engine's own dither, so
// preview and export match exactly) and Ogg Vorbis (lossy, from float).

#include <ac/AudioBuffer.h>
#include <ac/Dither.h>

#include <string>
#include <vector>

namespace af {

enum class FileFormat { Wav, Flac, Ogg };

struct ExportOptions
{
    FileFormat format = FileFormat::Wav;
    int bitDepth = 24;            ///< 16, 24 or 32 (32 = IEEE float, WAV only); ignored for Ogg
    ac::DitherType dither = ac::DitherType::Tpdf; ///< ignored for Ogg (lossy encoder works from float)
    int oggQualityIndex = 8;      ///< index into Ogg Vorbis quality options (8 = ~256 kbps)
};

/** Extension (with dot) for an export format. */
const char* extensionFor(FileFormat f);
/** Ogg Vorbis quality option labels (e.g. "256 kbps"). */
std::vector<std::string> oggQualityOptions();

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

/** Write the buffer; integer formats get engine dither. */
bool saveAudio(const std::string& path, const ac::AudioBuffer& b, const ExportOptions& opt, std::string& error);

/** File extensions accepted for import, as a wildcard ("*.wav;*.flac;..."). */
std::string supportedWildcard();
/** True if the file extension is one we can import. */
bool isSupportedExtension(const std::string& path);

} // namespace af
