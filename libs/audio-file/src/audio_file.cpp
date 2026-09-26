#include "supple/audio/audio_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>

#include "supple/base/rng.h"

#define DR_WAV_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_MP3_NO_STDIO
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

namespace supple::audio {

namespace fs = std::filesystem;

std::vector<const float*> AudioData::pointers() const {
    std::vector<const float*> p;
    for (const auto& c : channels) p.push_back(c.data());
    return p;
}

Container detect(std::span<const std::uint8_t> h) {
    auto is = [&](const char* tag, std::size_t at = 0) {
        const std::size_t n = std::strlen(tag);
        return h.size() >= at + n && std::memcmp(h.data() + at, tag, n) == 0;
    };
    if ((is("RIFF") || is("RF64") || is("BW64")) && is("WAVE", 8)) return Container::Wav;
    if (is("fLaC")) return Container::Flac;
    if (is("ID3")) return Container::Mp3;
    if (h.size() >= 2 && h[0] == 0xff && (h[1] & 0xe0) == 0xe0) return Container::Mp3;
    return Container::Unknown;
}

namespace {

AudioData deinterleave(const float* interleaved, std::size_t frames, unsigned channels,
                       double rate) {
    AudioData out;
    out.sampleRate = rate;
    out.channels.assign(channels, std::vector<float>(frames));
    for (std::size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < channels; ++c) out.channels[c][i] = interleaved[i * channels + c];
    return out;
}

Error decodeError(const char* what) {
    return Error("audio.decode", std::string("could not decode the ") + what + " data", {},
                 "the file may be truncated or use an unsupported variant");
}

}  // namespace

Result<AudioData> readAudio(std::span<const std::uint8_t> bytes) {
    switch (detect(bytes)) {
        case Container::Wav: {
            drwav wav;
            if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr))
                return decodeError("WAV");
            const std::size_t frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
            std::vector<float> interleaved(frames * wav.channels);
            const auto got = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
            const unsigned channels = wav.channels;
            const double rate = wav.sampleRate;
            drwav_uninit(&wav);
            if (channels == 0) return decodeError("WAV");
            return deinterleave(interleaved.data(), static_cast<std::size_t>(got), channels, rate);
        }
        case Container::Flac: {
            unsigned int channels = 0, rate = 0;
            drflac_uint64 frames = 0;
            float* data = drflac_open_memory_and_read_pcm_frames_f32(
                bytes.data(), bytes.size(), &channels, &rate, &frames, nullptr);
            if (!data) return decodeError("FLAC");
            auto out = deinterleave(data, static_cast<std::size_t>(frames), channels, rate);
            drflac_free(data, nullptr);
            return out;
        }
        case Container::Mp3: {
            drmp3_config config{};
            drmp3_uint64 frames = 0;
            float* data = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(),
                                                                    &config, &frames, nullptr);
            if (!data) return decodeError("MP3");
            auto out = deinterleave(data, static_cast<std::size_t>(frames), config.channels,
                                    config.sampleRate);
            drmp3_free(data, nullptr);
            return out;
        }
        case Container::Unknown:
            break;
    }
    return Error("audio.format", "unrecognised audio format", {}, "supported: WAV, FLAC, MP3");
}

Result<AudioData> readAudio(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error("audio.open", "cannot open " + path.string(), {}, "check the path");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    auto r = readAudio(bytes);
    if (!r) {
        Error e = std::move(r).error();
        e.message = path.filename().string() + ": " + e.message;
        return e;
    }
    return r;
}

Result<std::vector<std::uint8_t>> encodeWav(const AudioData& data, const WavOptions& options) {
    const std::size_t channels = data.channelCount();
    const std::size_t frames = data.frames();
    if (channels == 0 || channels > 64)
        return Error("audio.encode", "WAV needs 1..64 channels", {},
                     "got " + std::to_string(channels));
    for (const auto& c : data.channels)
        if (c.size() != frames) return Error("audio.encode", "channels have different lengths");
    if (!(data.sampleRate > 0) || data.sampleRate > 768000)
        return Error("audio.encode", "invalid sample rate", {}, "use 1 .. 768000 Hz");

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.channels = static_cast<drwav_uint32>(channels);
    format.sampleRate = static_cast<drwav_uint32>(std::lround(data.sampleRate));
    std::vector<std::uint8_t> payload;
    switch (options.format) {
        case SampleFormat::Float32: {
            format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
            format.bitsPerSample = 32;
            payload.resize(frames * channels * 4);
            auto* out = reinterpret_cast<float*>(payload.data());
            for (std::size_t i = 0; i < frames; ++i)
                for (std::size_t c = 0; c < channels; ++c)
                    out[i * channels + c] = data.channels[c][i];
            break;
        }
        case SampleFormat::Pcm16:
        case SampleFormat::Pcm24: {
            const bool is16 = options.format == SampleFormat::Pcm16;
            format.format = DR_WAVE_FORMAT_PCM;
            format.bitsPerSample = is16 ? 16 : 24;
            const double scale = is16 ? 32768.0 : 8388608.0;
            const std::int32_t maxV = is16 ? 32767 : 8388607, minV = is16 ? -32768 : -8388608;
            const std::size_t bytesPer = is16 ? 2 : 3;
            payload.resize(frames * channels * bytesPer);
            Rng rng(options.ditherSeed);
            std::size_t w = 0;
            for (std::size_t i = 0; i < frames; ++i)
                for (std::size_t c = 0; c < channels; ++c) {
                    double x = static_cast<double>(data.channels[c][i]) * scale;
                    if (options.dither) x += rng.uniform() - rng.uniform();  // TPDF, +-1 LSB
                    auto q = static_cast<std::int32_t>(std::clamp(
                        std::floor(x + 0.5), static_cast<double>(minV), static_cast<double>(maxV)));
                    for (std::size_t b = 0; b < bytesPer; ++b)
                        payload[w++] = static_cast<std::uint8_t>(q >> (8 * b));
                }
            break;
        }
    }
    drwav wav;
    void* mem = nullptr;
    size_t size = 0;
    if (!drwav_init_memory_write(&wav, &mem, &size, &format, nullptr))
        return Error("audio.encode", "WAV writer failed");
    const auto written = drwav_write_raw(&wav, payload.size(), payload.data());
    drwav_uninit(&wav);
    if (written != payload.size()) {
        drwav_free(mem, nullptr);
        return Error("audio.encode", "WAV writer wrote fewer bytes than expected");
    }
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(mem),
                                  static_cast<std::uint8_t*>(mem) + size);
    drwav_free(mem, nullptr);
    return out;
}

Status writeWav(const fs::path& path, const AudioData& data, const WavOptions& options) {
    auto bytes = encodeWav(data, options);
    if (!bytes) return std::move(bytes).error();
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return Error("audio.write", "cannot write " + tmp.string(), {}, "check the directory");
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
        out.flush();
        if (!out)
            return Error("audio.write", "short write to " + tmp.string(), {},
                         "check free disk space");
    }
    fs::rename(tmp, path, ec);
    if (ec)
        return Error("audio.write", "cannot move " + tmp.string() + " into place: " + ec.message());
    return okStatus();
}

}  // namespace supple::audio
