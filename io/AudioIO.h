#pragma once
// WAV / FLAC (and AIFF) reading and writing. Integer formats are quantised by
// the engine's own dither (ac::quantise) so preview and export match exactly.

#include <ac/AudioBuffer.h>
#include <ac/Dither.h>

#include <string>

namespace af {

enum class FileFormat { Wav, Flac };

struct ExportOptions
{
    FileFormat format = FileFormat::Wav;
    int bitDepth = 24;            ///< 16, 24 or 32 (32 = IEEE float, WAV only)
    ac::DitherType dither = ac::DitherType::Tpdf;
};

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

/** File extensions accepted for import (e.g. "*.wav;*.flac;*.aif;*.aiff"). */
std::string supportedWildcard();

} // namespace af
