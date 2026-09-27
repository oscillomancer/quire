// A float32 WAV file written incrementally (recording, engine.md §4): the
// header's sizes are rewritten on updateHeader() and close(), so a crash
// leaves a readable file up to the last update. Not real-time: a writer
// thread owns it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "supple/base/result.h"

namespace supple::audio {

class WavStreamWriter {
  public:
    WavStreamWriter() = default;
    ~WavStreamWriter();
    WavStreamWriter(const WavStreamWriter&) = delete;
    WavStreamWriter& operator=(const WavStreamWriter&) = delete;

    Status open(const std::filesystem::path& path, double sampleRate, std::uint32_t channels);
    // Interleaved frames.
    Status append(const float* frames, std::size_t count);
    Status updateHeader();  // sizes of what is written so far, flushed to the file
    Status close();
    bool isOpen() const { return file_ != nullptr; }
    std::uint64_t frames() const { return frames_; }
    std::uint32_t channels() const { return channels_; }

  private:
    std::FILE* file_ = nullptr;
    std::uint32_t channels_ = 0;
    std::uint64_t frames_ = 0;
};

}  // namespace supple::audio
