#include "supple/audio/audio_file.h"

#include "supple/base/files.h"

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
            const unsigned channels = wav.channels;
            const double rate = wav.sampleRate;
            // dr_wav clamps a data chunk that runs past the end of the file
            // (interrupted recordings are common); keep what is there but say so.
            // RIFF/RIFX store the declared size just before the data.
            bool truncated = false;
            if ((wav.container == drwav_container_riff || wav.container == drwav_container_rifx) &&
                wav.dataChunkDataPos >= 4 && wav.dataChunkDataPos <= bytes.size()) {
                const std::size_t at = static_cast<std::size_t>(wav.dataChunkDataPos) - 4;
                const bool be = wav.container == drwav_container_rifx;
                std::uint32_t declared = 0;
                for (int b = 0; b < 4; ++b)
                    declared |= std::uint32_t(bytes[at + static_cast<std::size_t>(be ? 3 - b : b)])
                                << (8 * b);
                truncated = declared != 0xFFFFFFFFu && wav.dataChunkDataPos + declared >
                                                           static_cast<drwav_uint64>(bytes.size());
            }
            // No encoding stores more than two frames per byte (4-bit ADPCM,
            // mono): anything larger is a lying header; this also bounds the allocation.
            if (channels == 0 ||
                wav.totalPCMFrameCount > 2 * static_cast<drwav_uint64>(bytes.size())) {
                drwav_uninit(&wav);
                return decodeError("WAV");
            }
            const std::size_t frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
            std::vector<float> interleaved(frames * channels);
            const auto got = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
            drwav_uninit(&wav);
            auto out =
                deinterleave(interleaved.data(), static_cast<std::size_t>(got), channels, rate);
            out.truncated = truncated || got < frames;
            return out;
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
                    if (!std::isfinite(x))
                        x = std::isnan(x) ? 0.0 : x;  // NaN -> 0; inf clamps below
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
    auto st = writeFileAtomic(path, *bytes);
    if (!st) {
        Error e = std::move(st).error();
        e.code = "audio.write";
        return e;
    }
    return okStatus();
}

}  // namespace supple::audio
