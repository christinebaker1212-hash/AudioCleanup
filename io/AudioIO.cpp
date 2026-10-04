#include "AudioIO.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace af {

namespace {
juce::AudioFormatManager& manager()
{
    static juce::AudioFormatManager m;
    static bool init = false;
    if (!init)
    {
        m.registerFormat(new juce::WavAudioFormat(), true);
        m.registerFormat(new juce::FlacAudioFormat(), false);
        m.registerFormat(new juce::AiffAudioFormat(), false);
        init = true;
    }
    return m;
}
} // namespace

std::string supportedWildcard() { return "*.wav;*.wave;*.flac;*.aif;*.aiff"; }

bool loadAudio(const std::string& path, ac::AudioBuffer& out, std::string& error, SourceInfo* info)
{
    juce::File f(juce::String::fromUTF8(path.c_str()));
    if (!f.existsAsFile())
    {
        error = "File not found: " + path;
        return false;
    }
    std::unique_ptr<juce::AudioFormatReader> r(manager().createReaderFor(f));
    if (!r)
    {
        error = "Unsupported or unreadable audio file: " + path;
        return false;
    }
    const int nch = int(r->numChannels);
    const auto len = r->lengthInSamples;
    if (nch <= 0 || len < 0 || len > (juce::int64(1) << 31))
    {
        error = "File too large or has no channels";
        return false;
    }
    juce::AudioBuffer<float> tmp(nch, int(len));
    if (len > 0 && !r->read(&tmp, 0, int(len), 0, true, true))
    {
        error = "Read error";
        return false;
    }
    out = ac::AudioBuffer(nch, size_t(len), r->sampleRate);
    for (int c = 0; c < nch; ++c) std::copy(tmp.getReadPointer(c), tmp.getReadPointer(c) + len, out.channel(c));
    if (info)
    {
        info->sampleRate = r->sampleRate;
        info->channels = nch;
        info->bitsPerSample = int(r->bitsPerSample);
        info->floatingPoint = r->usesFloatingPointData;
        info->formatName = r->getFormatName().toStdString();
    }
    return true;
}

bool saveAudio(const std::string& path, const ac::AudioBuffer& b, const ExportOptions& opt, std::string& error)
{
    juce::File f(juce::String::fromUTF8(path.c_str()));
    f.getParentDirectory().createDirectory();
    if (f.existsAsFile() && !f.deleteFile())
    {
        error = "Cannot overwrite " + path;
        return false;
    }
    int bits = opt.bitDepth;
    if (opt.format == FileFormat::Flac && bits > 24) bits = 24;
    const bool isFloat = bits == 32;
    std::unique_ptr<juce::AudioFormat> fmt;
    if (opt.format == FileFormat::Flac) fmt = std::make_unique<juce::FlacAudioFormat>();
    else fmt = std::make_unique<juce::WavAudioFormat>();

    auto fileStream = std::make_unique<juce::FileOutputStream>(f);
    if (!fileStream->openedOk())
    {
        error = "Cannot open for writing: " + path;
        return false;
    }
    std::unique_ptr<juce::OutputStream> stream(fileStream.release());
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(b.sampleRate)
                             .withNumChannels(b.numChannels())
                             .withBitsPerSample(bits)
                             .withSampleFormat(isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                       : juce::AudioFormatWriterOptions::SampleFormat::integral)
                             .withQualityOptionIndex(opt.format == FileFormat::Flac ? 5 : 0);
    std::unique_ptr<juce::AudioFormatWriter> w = fmt->createWriterFor(stream, options);
    if (!w)
    {
        error = "Format does not support this sample rate / bit depth / channel count";
        return false;
    }
    const int nch = b.numChannels();
    const size_t n = b.numFrames();
    if (isFloat)
    {
        std::vector<const float*> ptrs(static_cast<size_t>(nch));
        for (int c = 0; c < nch; ++c) ptrs[size_t(c)] = b.channel(c);
        const size_t blk = 65536;
        std::vector<const float*> p2(static_cast<size_t>(nch));
        for (size_t s = 0; s < n; s += blk)
        {
            for (int c = 0; c < nch; ++c) p2[size_t(c)] = ptrs[size_t(c)] + s;
            if (!w->writeFromFloatArrays(p2.data(), nch, int(std::min(blk, n - s))))
            {
                error = "Write failed";
                return false;
            }
        }
    }
    else
    {
        const auto q = ac::quantise(b, bits, opt.dither);
        // JUCE expects left-justified 32-bit integers for integer formats.
        const int shift = 32 - bits;
        const size_t blk = 65536;
        std::vector<std::vector<int>> tmp(static_cast<size_t>(nch), std::vector<int>(blk));
        std::vector<const int*> ptrs(static_cast<size_t>(nch));
        for (size_t s = 0; s < n; s += blk)
        {
            const size_t m = std::min(blk, n - s);
            for (int c = 0; c < nch; ++c)
            {
                for (size_t i = 0; i < m; ++i) tmp[size_t(c)][i] = int(uint32_t(q[size_t(c)][s + i]) << shift);
                ptrs[size_t(c)] = tmp[size_t(c)].data();
            }
            if (!w->write(ptrs.data(), int(m)))
            {
                error = "Write failed";
                return false;
            }
        }
    }
    w.reset();
    return true;
}

} // namespace af
