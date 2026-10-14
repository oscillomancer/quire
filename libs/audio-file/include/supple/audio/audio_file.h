// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Audio file I/O (spec: docs/specs/core-analysis.md §9). Main/I-O threads only.
//
// Reading: WAV (PCM 8/16/24/32, float 32/64, RF64/BW64), FLAC and MP3, detected
// by content (not extension), decoded to planar float at the file's own rate.
// Writing: RIFF WAV or RF64 beyond 4 GiB as 16/24-bit PCM with seeded TPDF
// dither, or 32-bit float; FLAC at 16 or 24 bits.
// Writes are atomic (temporary file + rename, see writeFileAtomic). PCM
// encoding maps NaN to 0 and clamps infinities to full scale; float32 stores
// samples unchanged. A WAV whose data is shorter than its header declares
// decodes to the audio present with AudioData::truncated set.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "supple/base/result.h"

namespace supple::audio {

struct AudioData {
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;  // planar; all the same length
    // Set by readAudio when the file holds less audio than its header declares
    // (e.g. an interrupted recording): the audio is what was there.
    bool truncated = false;

    std::size_t channelCount() const { return channels.size(); }
    std::size_t frames() const { return channels.empty() ? 0 : channels.front().size(); }
    double seconds() const {
        return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0;
    }
    // Pointers for APIs that take `const float* const*`.
    std::vector<const float*> pointers() const;
};

enum class Container { Wav, Flac, Mp3, Unknown };
Container detect(std::span<const std::uint8_t> head);  // from the first bytes

// What a file's header says, without decoding it: the media model's
// MediaSource needs exactly this (rate, channels, frames). The container is
// detected by content, like readAudio. `truncated` mirrors AudioData: the data
// chunk holds less audio than it declares. Error codes: "audio.open",
// "audio.read", "audio.format".
struct FileInfo {
    Container container = Container::Unknown;
    double sampleRate = 0;
    int channels = 0;
    std::int64_t frames = 0;
    bool truncated = false;
};
Result<FileInfo> inspect(const std::filesystem::path& path);

Result<AudioData> resample(const AudioData& data, double targetRate);

Result<AudioData> readAudio(const std::filesystem::path& path);
Result<AudioData> readAudio(std::span<const std::uint8_t> bytes);

// Tests: how many whole-file decodes readAudio() has performed (the path
// overload goes through the byte overload, which counts once). The media model
// must read a file's header — inspect() — never decode it to learn metadata.
std::uint64_t decodeCountForTests();
void resetDecodeCountForTests();

enum class SampleFormat { Pcm16, Pcm24, Float32 };
struct WavOptions {
    SampleFormat format = SampleFormat::Float32;
    bool dither = true;            // TPDF dither for PCM formats
    std::uint64_t ditherSeed = 0;  // deterministic
};

// Builds a canonical RIFF or RF64 header for the given payload size. RF64
// includes ds64 with the 64-bit sizes and sample count; no audio allocation.
Result<std::vector<std::uint8_t>> wavHeader(std::uint64_t frames, std::uint32_t channels,
                                            std::uint32_t sampleRate, SampleFormat format);

Result<std::vector<std::uint8_t>> encodeWav(const AudioData& data, const WavOptions& options = {});
Status writeWav(const std::filesystem::path& path, const AudioData& data,
                const WavOptions& options = {});

// FLAC (an original encoder: fixed predictors, Rice coding, stereo
// decorrelation, MD5 in STREAMINFO). 16- or 24-bit, quantized and dithered
// exactly as encodeWav does for the same options, so the two decode alike.
struct FlacOptions {
    int bitsPerSample = 24;  // 16 or 24
    bool dither = true;
    std::uint64_t ditherSeed = 0;
    int blockSize = 4096;
};
Result<std::vector<std::uint8_t>> encodeFlac(const AudioData& data,
                                             const FlacOptions& options = {});
Status writeFlac(const std::filesystem::path& path, const AudioData& data,
                 const FlacOptions& options = {});

}  // namespace supple::audio
