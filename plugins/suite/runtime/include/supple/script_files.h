// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a script's AudioFile or Convolver reads: a file property's contents,
// made off the audio thread by the glue's loader (the framework's file
// slots run it) and bound into the script's member at the start of a block.
// The runtime and the glue share it; the script compiler does not (it needs
// the framework's DSP headers), and scripts never see it.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "dsp/AudioFile.h"
#include "dsp/Convolution.h"

namespace supple::script::detail {

struct FileContents {
    // The file's audio, at the session's rate.
    dsp::AudioData audio;
    // Each channel's level by blocks, for AudioFile's peak() and rms(): the
    // largest |sample| of each kBlock frames, of each kBlock of those, and so
    // on up to a level of kBlock or fewer; and the sum of squares before each
    // block's first frame. measure() makes them from the audio, off the
    // audio thread, so a range of any length costs a few hundred steps.
    static constexpr size_t kBlock = 64;
    struct Levels {
        std::vector<std::vector<float>> peak;
        std::vector<double> energy;
    };
    std::vector<Levels> levels;
    void measure();
    double peak(int channel, int from, int to) const;
    double rms(int channel, int from, int to) const;
    // An impulse response's convolution, prepared for the session's
    // channels (impulseResponse() members only).
    std::unique_ptr<dsp::Convolution> convolution;
    int length = 0;  // the response's frames

    // Back to silence, as reset() leaves every other object.
    void reset() {
        if (convolution) convolution->reset();
    }
};

}  // namespace supple::script::detail
