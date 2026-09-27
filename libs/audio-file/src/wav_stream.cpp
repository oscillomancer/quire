#include "supple/audio/wav_stream.h"

#include <cmath>
#include <cstring>

namespace supple::audio {

namespace {

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

constexpr long kHeaderBytes = 44;
// The RIFF container caps sizes at 32 bits; longer recordings need RF64.
constexpr std::uint64_t kMaxData = 0xFFFFFFFFull - (kHeaderBytes - 8);

}  // namespace

WavStreamWriter::~WavStreamWriter() { (void)close(); }

Status WavStreamWriter::open(const std::filesystem::path& path, double sampleRate,
                             std::uint32_t channels) {
    if (file_) return Error("audio.stream", "the writer is already open");
    if (channels == 0 || channels > 64 || !(sampleRate > 0))
        return Error("audio.stream", "bad channel count or rate");
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return Error("file.write", "cannot create " + path.string());
    channels_ = channels;
    frames_ = 0;
    std::uint8_t h[kHeaderBytes] = {};
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, kHeaderBytes - 8);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 3);  // IEEE float
    put16(h + 22, static_cast<std::uint16_t>(channels));
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 4);
    put16(h + 32, static_cast<std::uint16_t>(channels * 4));
    put16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, 0);
    if (std::fwrite(h, 1, sizeof h, file_) != sizeof h) return Error("file.write", "header");
    return okStatus();
}

Status WavStreamWriter::append(const float* frames, std::size_t count) {
    if (!file_) return Error("audio.stream", "the writer is closed");
    if ((frames_ + count) * channels_ * 4 > kMaxData)
        return Error("audio.stream_full", "a recording over 4 GiB needs RF64 (not yet)");
    if (count && std::fwrite(frames, sizeof(float) * channels_, count, file_) != count)
        return Error("file.write", "short write");
    frames_ += count;
    return okStatus();
}

Status WavStreamWriter::updateHeader() {
    if (!file_) return okStatus();
    const auto data = static_cast<std::uint32_t>(frames_ * channels_ * 4);
    std::uint8_t b[4];
    const long end = std::ftell(file_);
    put32(b, data + kHeaderBytes - 8);
    if (std::fseek(file_, 4, SEEK_SET) != 0 || std::fwrite(b, 1, 4, file_) != 4)
        return Error("file.write", "header");
    put32(b, data);
    if (std::fseek(file_, 40, SEEK_SET) != 0 || std::fwrite(b, 1, 4, file_) != 4)
        return Error("file.write", "header");
    if (std::fseek(file_, end, SEEK_SET) != 0) return Error("file.write", "seek");
    std::fflush(file_);
    return okStatus();
}

Status WavStreamWriter::close() {
    if (!file_) return okStatus();
    auto st = updateHeader();
    std::fclose(file_);
    file_ = nullptr;
    return st;
}

}  // namespace supple::audio
