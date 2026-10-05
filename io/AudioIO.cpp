#include "AudioIO.h"

#include <ac/Resampler.h>

#include <juce_audio_formats/juce_audio_formats.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include <minimp3_ex.h>

#include <lame.h>
#include <opusfile.h>

#include <algorithm>
#include <cstring>

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
        m.registerFormat(new juce::OggVorbisAudioFormat(), false);
        init = true;
    }
    return m;
}

constexpr const char* kImportExtensions = "wav;wave;flac;aif;aiff;aifc;ogg;oga;opus;mp3;mp2;mpga";

// ---------------------------------------------------------------- sniffing

enum class Container { Unknown, Riff, Flac, Aiff, Ogg, Mpeg, Mp4, Adts, Asf };

bool startsWith(const uint8_t* d, size_t n, const char* tag, size_t off = 0)
{
    const size_t len = std::strlen(tag);
    return n >= off + len && std::memcmp(d + off, tag, len) == 0;
}

Container sniff(const uint8_t* d, size_t n)
{
    if (startsWith(d, n, "RIFF") || startsWith(d, n, "RF64") || startsWith(d, n, "BW64")) return Container::Riff;
    if (startsWith(d, n, "fLaC")) return Container::Flac;
    if (startsWith(d, n, "FORM")) return Container::Aiff;
    if (startsWith(d, n, "OggS")) return Container::Ogg;
    if (startsWith(d, n, "ftyp", 4)) return Container::Mp4;
    if (n >= 16 && std::memcmp(d, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8) == 0) return Container::Asf;
    if (startsWith(d, n, "ID3")) return Container::Mpeg;
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) return Container::Adts; // AAC ADTS (layer bits 00)
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return Container::Mpeg;
    return Container::Unknown;
}

/** Codecs of the logical streams that start in the first pages of an Ogg file. */
struct OggCodecs { bool opus = false, vorbis = false, flac = false, speex = false; };

OggCodecs oggCodecs(const uint8_t* d, size_t n)
{
    OggCodecs c;
    size_t pos = 0;
    for (int page = 0; page < 64 && pos + 27 <= n; ++page)
    {
        if (std::memcmp(d + pos, "OggS", 4) != 0) break;
        const bool bos = (d[pos + 5] & 0x02) != 0;
        const size_t segs = d[pos + 26];
        if (pos + 27 + segs > n) break;
        size_t body = 0;
        for (size_t i = 0; i < segs; ++i) body += d[pos + 27 + i];
        const uint8_t* pkt = d + pos + 27 + segs;
        const size_t avail = std::min(body, n - std::min(n, pos + 27 + segs));
        if (!bos) break; // all beginning-of-stream pages come first
        if (startsWith(pkt, avail, "OpusHead")) c.opus = true;
        else if (startsWith(pkt, avail, "\x01vorbis")) c.vorbis = true;
        else if (startsWith(pkt, avail, "\x7F" "FLAC")) c.flac = true;
        else if (startsWith(pkt, avail, "Speex   ")) c.speex = true;
        pos += 27 + segs + body;
    }
    return c;
}

void fillInfo(SourceInfo* info, double rate, int ch, int bits, bool isFloat, std::string name)
{
    if (!info) return;
    info->sampleRate = rate;
    info->channels = ch;
    info->bitsPerSample = bits;
    info->floatingPoint = isFloat;
    info->formatName = std::move(name);
}

// --------------------------------------------------------------------- MP3

/** Decodes a whole MPEG audio (layer I/II/III) stream. Unlike a sequential
    reader this never trusts an up-front length estimate: every frame is
    decoded, mono/stereo switches are followed (mono frames are duplicated),
    frames at a different sample rate (a spliced file) are dropped and
    reported, and a LAME/Xing/Info tag supplies the gapless delay/padding. */
bool decodeMpeg(const uint8_t* data, size_t size, ac::AudioBuffer& out, std::string& error, SourceInfo* info,
                std::string* warning)
{
    const uint8_t* p = data;
    size_t n = size;
    mp3dec_skip_id3(&p, &n);

    int freeFormatBytes = 0, frameBytes = 0;
    const int first = mp3d_find_frame(p, int(std::min<size_t>(n, INT_MAX)), &freeFormatBytes, &frameBytes);
    if (!frameBytes)
    {
        error = "No MPEG audio frames found (the file may be damaged or not an MP3)";
        return false;
    }
    p += first;
    n -= size_t(first);

    // Encoder delay and padding from a LAME / Xing / Info tag in the first frame.
    uint32_t tagFrames = 0;
    int delay = 0, padding = 0;
    bool haveTag = false;
    const int layer = 4 - HDR_GET_LAYER(p);
    const int samplesPerFrame = int(hdr_frame_samples(p));
    if (layer == 3)
    {
        const int r = mp3dec_check_vbrtag(p, frameBytes, &tagFrames, &delay, &padding);
        if (r != 0)
        {
            haveTag = r > 0;
            p += frameBytes; // the tag frame carries no audio
            n -= size_t(frameBytes);
        }
    }

    mp3dec_t dec;
    mp3dec_init(&dec);
    std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    std::vector<float> left, right;
    left.reserve(haveTag ? size_t(tagFrames) * size_t(samplesPerFrame) : n * 3);
    bool stereo = false;
    int hz = 0;
    size_t droppedFrames = 0, frames = 0;
    double kbpsSum = 0;
    while (n > 0)
    {
        mp3dec_frame_info_t fi {};
        const int s = mp3dec_decode_frame(&dec, p, int(std::min<size_t>(n, INT_MAX)), pcm.data(), &fi);
        if (fi.frame_bytes <= 0) break;
        p += fi.frame_bytes;
        n -= size_t(fi.frame_bytes);
        if (s <= 0) continue; // junk skipped, or the bit reservoir is still filling
        if (hz == 0) hz = fi.hz;
        if (fi.hz != hz)
        {
            ++droppedFrames;
            continue;
        }
        ++frames;
        kbpsSum += fi.bitrate_kbps;
        if (fi.channels == 2)
        {
            if (!stereo)
            {
                right = left;
                stereo = true;
            }
            for (int i = 0; i < s; ++i)
            {
                left.push_back(pcm[size_t(2 * i)]);
                right.push_back(pcm[size_t(2 * i + 1)]);
            }
        }
        else
        {
            left.insert(left.end(), pcm.begin(), pcm.begin() + s);
            if (stereo) right.insert(right.end(), pcm.begin(), pcm.begin() + s);
        }
    }
    if (frames == 0 || hz <= 0)
    {
        error = "The MP3 contains no decodable audio";
        return false;
    }

    size_t start = 0, len = left.size();
    if (haveTag)
    {
        start = std::min(len, size_t(std::max(0, delay)));
        len -= start;
        if (tagFrames > 0)
        {
            const long long expected = (long long)tagFrames * samplesPerFrame - std::max(0, delay) - std::max(0, padding);
            if (expected > 0 && size_t(expected) < len) len = size_t(expected);
        }
    }
    const int nch = stereo ? 2 : 1;
    out = ac::AudioBuffer(nch, len, double(hz));
    std::copy(left.begin() + std::ptrdiff_t(start), left.begin() + std::ptrdiff_t(start + len), out.channel(0));
    if (stereo) std::copy(right.begin() + std::ptrdiff_t(start), right.begin() + std::ptrdiff_t(start + len), out.channel(1));

    const int kbps = int(kbpsSum / double(frames) + 0.5);
    const char* layerName = layer == 3 ? "MP3" : layer == 2 ? "MP2" : "MP1";
    fillInfo(info, hz, nch, 32, true, std::string(layerName) + " file, " + std::to_string(kbps) + " kbps" + (haveTag ? ", gapless" : ""));
    if (droppedFrames > 0 && warning)
        *warning = std::to_string(droppedFrames) + " MP3 frames at a different sample rate were skipped (spliced file)";
    return true;
}

// -------------------------------------------------------------------- Opus

bool decodeOpus(const uint8_t* data, size_t size, ac::AudioBuffer& out, std::string& error, SourceInfo* info)
{
    int err = 0;
    OggOpusFile* of = op_open_memory(data, size, &err);
    if (!of)
    {
        error = "Cannot open the Ogg Opus stream (error " + std::to_string(err) + ")";
        return false;
    }
    // A chained file may change channel count between links; downmix to stereo then.
    int nch = op_channel_count(of, 0);
    for (int li = 1; li < op_link_count(of); ++li)
        if (op_channel_count(of, li) != nch) nch = -1;
    const bool forceStereo = nch < 0;
    if (forceStereo) nch = 2;

    const ogg_int64_t total = op_pcm_total(of, -1);
    std::vector<std::vector<float>> ch(static_cast<size_t>(nch));
    for (auto& c : ch) c.reserve(total > 0 ? size_t(total) : 0);
    std::vector<float> buf(size_t(5760 * nch * 2));
    for (;;)
    {
        const int got = forceStereo ? op_read_float_stereo(of, buf.data(), int(buf.size()))
                                    : op_read_float(of, buf.data(), int(buf.size()), nullptr);
        if (got == OP_HOLE) continue; // skip a gap in the data, keep decoding
        if (got < 0)
        {
            op_free(of);
            error = "Ogg Opus decode error " + std::to_string(got);
            return false;
        }
        if (got == 0) break;
        for (int i = 0; i < got; ++i)
            for (int c = 0; c < nch; ++c) ch[size_t(c)].push_back(buf[size_t(i * nch + c)]);
    }
    const OpusHead* head = op_head(of, 0);
    const int inputRate = head ? int(head->input_sample_rate) : 0;
    op_free(of);
    if (ch.empty() || ch[0].empty())
    {
        error = "The Ogg Opus stream contains no audio";
        return false;
    }
    out = ac::AudioBuffer(nch, ch[0].size(), 48000.0);
    for (int c = 0; c < nch; ++c) std::copy(ch[size_t(c)].begin(), ch[size_t(c)].end(), out.channel(c));
    fillInfo(info, 48000.0, nch, 32, true,
             "Ogg Opus" + (inputRate > 0 && inputRate != 48000 ? " (recorded at " + std::to_string(inputRate) + " Hz, decoded at 48 kHz)" : std::string()));
    return true;
}

// -------------------------------------------------------------------- JUCE

bool decodeWithJuce(const juce::MemoryBlock& mem, ac::AudioBuffer& out, std::string& error, SourceInfo* info)
{
    // Content-based: every registered format tries the stream, whatever the extension.
    std::unique_ptr<juce::AudioFormatReader> r(
        manager().createReaderFor(std::make_unique<juce::MemoryInputStream>(mem, false)));
    if (!r)
    {
        error = "Unsupported or unreadable audio file";
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
    fillInfo(info, r->sampleRate, nch, int(r->bitsPerSample), r->usesFloatingPointData, r->getFormatName().toStdString());
    return true;
}

// -------------------------------------------------------------------- MP3 out

struct Mp3Quality { const char* label; int kbps; int vbrQuality; }; // vbrQuality < 0 = CBR
constexpr Mp3Quality kMp3Qualities[] = {
    { "320 kbps CBR", 320, -1 },
    { "V0 VBR (~245 kbps)", 0, 0 },
    { "256 kbps CBR", 256, -1 },
    { "V2 VBR (~190 kbps)", 0, 2 },
    { "192 kbps CBR", 192, -1 },
    { "128 kbps CBR", 128, -1 },
};

bool writeMp3(const juce::File& f, const ac::AudioBuffer& in, int qualityIndex, std::string& error)
{
    const int nch = in.numChannels();
    if (nch < 1 || nch > 2)
    {
        error = "MP3 export supports mono or stereo only (this file has " + std::to_string(nch) + " channels); use WAV or FLAC";
        return false;
    }
    // LAME encodes 8-48 kHz. Higher rates go to the nearest family rate first,
    // through the engine's linear-phase resampler (better than LAME's own).
    static const int lameRates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    const ac::AudioBuffer* src = &in;
    ac::AudioBuffer resampled;
    const int rate = int(in.sampleRate + 0.5);
    if (std::find(std::begin(lameRates), std::end(lameRates), rate) == std::end(lameRates))
    {
        const double dst = (rate % 11025 == 0) ? 44100.0 : 48000.0;
        resampled = ac::resample(in, dst);
        src = &resampled;
    }
    const auto& q = kMp3Qualities[std::clamp(qualityIndex, 0, int(std::size(kMp3Qualities)) - 1)];

    struct Lame
    {
        lame_global_flags* g = lame_init();
        ~Lame() { if (g) lame_close(g); }
    } lame;
    if (!lame.g)
    {
        error = "MP3 encoder initialisation failed";
        return false;
    }
    const int outRate = int(src->sampleRate + 0.5);
    lame_set_num_channels(lame.g, nch);
    lame_set_in_samplerate(lame.g, outRate);
    lame_set_out_samplerate(lame.g, outRate);
    lame_set_mode(lame.g, nch == 1 ? MONO : JOINT_STEREO);
    lame_set_quality(lame.g, 2); // LAME's "high quality" algorithm set
    lame_set_bWriteVbrTag(lame.g, 1); // Xing/LAME tag: exact length, gapless playback
    if (q.vbrQuality >= 0)
    {
        lame_set_VBR(lame.g, vbr_mtrh);
        lame_set_VBR_quality(lame.g, float(q.vbrQuality));
    }
    else
    {
        lame_set_VBR(lame.g, vbr_off);
        lame_set_brate(lame.g, q.kbps);
    }
    if (lame_init_params(lame.g) < 0)
    {
        error = "MP3 encoder rejected the settings (" + std::to_string(outRate) + " Hz, " + q.label + ")";
        return false;
    }

    juce::FileOutputStream os(f);
    if (!os.openedOk())
    {
        error = "Cannot open for writing: " + f.getFullPathName().toStdString();
        return false;
    }
    const size_t n = src->numFrames();
    const size_t blk = 8192;
    std::vector<unsigned char> mp3(size_t(1.25 * blk) + 7200);
    for (size_t s = 0; s < n; s += blk)
    {
        const int m = int(std::min(blk, n - s));
        const float* l = src->channel(0) + s;
        const float* r = nch > 1 ? src->channel(1) + s : l;
        const int bytes = lame_encode_buffer_ieee_float(lame.g, l, r, m, mp3.data(), int(mp3.size()));
        if (bytes < 0 || !os.write(mp3.data(), size_t(bytes)))
        {
            error = "MP3 encoding failed";
            return false;
        }
    }
    const int tail = lame_encode_flush(lame.g, mp3.data(), int(mp3.size()));
    if (tail < 0 || !os.write(mp3.data(), size_t(tail)))
    {
        error = "MP3 encoding failed";
        return false;
    }
    // Fill in the tag frame LAME reserved at the start of the stream.
    std::vector<unsigned char> tag(2880);
    const size_t tagBytes = lame_get_lametag_frame(lame.g, tag.data(), tag.size());
    if (tagBytes > 0 && tagBytes <= tag.size())
    {
        os.flush();
        if (!os.setPosition(0) || !os.write(tag.data(), tagBytes))
        {
            error = "Cannot write the MP3 header";
            return false;
        }
    }
    os.flush();
    if (os.getStatus().failed())
    {
        error = "Write failed: " + os.getStatus().getErrorMessage().toStdString();
        return false;
    }
    return true;
}
} // namespace

std::string supportedWildcard() { return "*.wav;*.wave;*.flac;*.aif;*.aiff;*.aifc;*.ogg;*.oga;*.opus;*.mp3;*.mp2;*.mpga"; }

bool isSupportedExtension(const std::string& path)
{
    return juce::File(juce::String::fromUTF8(path.c_str())).hasFileExtension(kImportExtensions);
}

const char* extensionFor(FileFormat f)
{
    switch (f)
    {
        case FileFormat::Flac: return ".flac";
        case FileFormat::Ogg: return ".ogg";
        case FileFormat::Mp3: return ".mp3";
        case FileFormat::Wav: break;
    }
    return ".wav";
}

std::vector<std::string> mp3QualityOptions()
{
    std::vector<std::string> out;
    for (const auto& q : kMp3Qualities) out.emplace_back(q.label);
    return out;
}

std::vector<std::string> oggQualityOptions()
{
    std::vector<std::string> out;
    for (auto& q : juce::OggVorbisAudioFormat().getQualityOptions()) out.push_back(q.toStdString());
    return out;
}

bool loadAudio(const std::string& path, ac::AudioBuffer& out, std::string& error, SourceInfo* info)
{
    juce::File f(juce::String::fromUTF8(path.c_str()));
    if (!f.existsAsFile())
    {
        error = "File not found: " + path;
        return false;
    }
    juce::MemoryBlock mem;
    if (!f.loadFileAsData(mem))
    {
        error = "Cannot read " + path + " (file locked or no permission?)";
        return false;
    }
    if (mem.getSize() == 0)
    {
        error = "The file is empty: " + path;
        return false;
    }
    const auto* d = static_cast<const uint8_t*>(mem.getData());
    const size_t n = mem.getSize();
    const auto name = f.getFileName().toStdString();
    auto fail = [&](const std::string& why) {
        error = name + ": " + why;
        return false;
    };

    bool ok = false;
    std::string why, warning;
    switch (sniff(d, n))
    {
        case Container::Ogg:
        {
            const auto c = oggCodecs(d, std::min<size_t>(n, 1 << 20));
            if (c.opus) ok = decodeOpus(d, n, out, why, info);
            else if (c.vorbis) ok = decodeWithJuce(mem, out, why, info);
            else if (c.flac) return fail("FLAC inside an Ogg container is not supported; save it as a plain .flac file");
            else if (c.speex) return fail("Ogg Speex is not supported; convert it to WAV, FLAC, MP3 or Opus");
            else return fail("this Ogg file contains no Vorbis or Opus audio stream");
            break;
        }
        case Container::Mp4:
            return fail("this is an MP4/M4A (AAC or ALAC) file, which is not supported yet; convert it to WAV, FLAC or MP3"
                        + std::string(f.hasFileExtension("mp3;ogg") ? " (it only has a ." + f.getFileExtension().substring(1).toStdString() + " name)" : ""));
        case Container::Adts:
            return fail("this is raw AAC audio, which is not supported yet; convert it to WAV, FLAC or MP3");
        case Container::Asf:
            return fail("this is a Windows Media (WMA) file, which is not supported; convert it to WAV, FLAC or MP3");
        case Container::Mpeg:
            ok = decodeMpeg(d, n, out, why, info, &warning);
            break;
        case Container::Riff:
        case Container::Flac:
        case Container::Aiff:
            ok = decodeWithJuce(mem, out, why, info);
            break;
        case Container::Unknown:
            // No recognisable header: an MP3 with leading junk, or something JUCE recognises.
            ok = decodeWithJuce(mem, out, why, info);
            if (!ok && mp3dec_detect_buf(d, n) == 0) ok = decodeMpeg(d, n, out, why, info, &warning);
            break;
    }
    if (!ok) return fail(why);
    if (out.numFrames() == 0) return fail("the file contains no audio samples");
    if (info && !warning.empty()) info->formatName += " (" + warning + ")";
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
    if (opt.format == FileFormat::Mp3)
    {
        if (!writeMp3(f, b, opt.mp3QualityIndex, error))
        {
            f.deleteFile();
            return false;
        }
        return true;
    }
    int bits = opt.bitDepth;
    if (opt.format == FileFormat::Flac && bits > 24) bits = 24;
    if (opt.format == FileFormat::Ogg) bits = 32; // Vorbis encodes from float
    const bool isFloat = bits == 32;
    std::unique_ptr<juce::AudioFormat> fmt;
    if (opt.format == FileFormat::Flac) fmt = std::make_unique<juce::FlacAudioFormat>();
    else if (opt.format == FileFormat::Ogg) fmt = std::make_unique<juce::OggVorbisAudioFormat>();
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
                             .withSampleFormat(opt.format == FileFormat::Ogg ? juce::AudioFormatWriterOptions::SampleFormat::automatic
                                               : isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                         : juce::AudioFormatWriterOptions::SampleFormat::integral)
                             .withQualityOptionIndex(opt.format == FileFormat::Flac ? 5
                                                     : opt.format == FileFormat::Ogg ? juce::jlimit(0, 10, opt.oggQualityIndex) : 0);
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
