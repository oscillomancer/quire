#include "supple/audio/audio_file.h"

#include <cmath>

#include "supple/dsp/dsp.h"

namespace supple::audio {
Result<AudioData> resample(const AudioData& data, double targetRate) {
    if (!std::isfinite(targetRate) || targetRate <= 0 || !std::isfinite(data.sampleRate) ||
        data.sampleRate <= 0 || data.channels.empty())
        return Error{"audio.resample", "Invalid audio data or sample rate", "",
                     "Provide planar audio and positive finite source and target rates"};
    const auto frames = data.frames();
    for (const auto& channel : data.channels)
        if (channel.size() != frames)
            return Error{"audio.resample", "Channel lengths differ", "",
                         "Use the same frame count for each channel"};
    AudioData out;
    out.sampleRate = targetRate;
    out.truncated = data.truncated;
    out.channels.reserve(data.channelCount());
    for (const auto& channel : data.channels)
        out.channels.push_back(dsp::resample(channel, data.sampleRate, targetRate));
    return out;
}
}  // namespace supple::audio
