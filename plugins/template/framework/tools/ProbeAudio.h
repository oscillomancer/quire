// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Audio storage for an embedding probe, sized from the plugin's current
// ports before activation. Input and output buses never alias; an instrument
// has no input buffers, a note effect none at all. No allocation occurs while
// processing.
#pragma once

#include <clap/clap.h>
#include <cmath>
#include <vector>

namespace supple::probe {
class Audio {
  public:
    Audio() = default;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool configure(const clap_plugin_t* plugin, uint32_t frames) {
        const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
            plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        frames_ = frames;
        return ports && frames && configureBuses(plugin, ports, true, input_) &&
               configureBuses(plugin, ports, false, output_);
    }
    bool hasOutput() const { return !output_.buffers.empty(); }
    void bind(clap_process_t& process) {
        process.frames_count = frames_;
        process.audio_inputs = input_.buffers.empty() ? nullptr : input_.buffers.data();
        process.audio_outputs = output_.buffers.empty() ? nullptr : output_.buffers.data();
        process.audio_inputs_count = static_cast<uint32_t>(input_.buffers.size());
        process.audio_outputs_count = static_cast<uint32_t>(output_.buffers.size());
    }
    void fillInputs(double& phase) {
        for (uint32_t f = 0; f < frames_; ++f) {
            for (auto& bus : input_.samples)
                for (size_t c = 0; c < bus.size(); ++c)
                    bus[c][f] = 0.2f * static_cast<float>(std::sin(phase + 0.37 * static_cast<double>(c)));
            phase += 0.0576;
        }
    }
    bool finiteOutputs() const {
        for (const auto& bus : output_.samples)
            for (const auto& channel : bus)
                for (float value : channel)
                    if (!std::isfinite(value)) return false;
        return true;
    }
    // The largest magnitude on any output channel in the last block.
    double outputPeak() const {
        double peak = 0.0;
        for (const auto& bus : output_.samples)
            for (const auto& channel : bus)
                for (float value : channel) peak = std::fmax(peak, std::fabs(static_cast<double>(value)));
        return peak;
    }

  private:
    struct Buses {
        std::vector<std::vector<std::vector<float>>> samples;
        std::vector<std::vector<float*>> pointers;
        std::vector<clap_audio_buffer_t> buffers;
    };
    bool configureBuses(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* ports,
                        bool input, Buses& buses) {
        const uint32_t count = ports->count(plugin, input);
        buses.samples.resize(count);
        buses.pointers.resize(count);
        buses.buffers.assign(count, clap_audio_buffer_t{});
        for (uint32_t b = 0; b < count; ++b) {
            clap_audio_port_info_t info{};
            if (!ports->get(plugin, b, input, &info) || !info.channel_count) return false;
            auto& samples = buses.samples[b];
            auto& pointers = buses.pointers[b];
            samples.assign(info.channel_count, std::vector<float>(frames_, 0.0f));
            pointers.resize(info.channel_count);
            for (uint32_t c = 0; c < info.channel_count; ++c) pointers[c] = samples[c].data();
            buses.buffers[b].data32 = pointers.data();
            buses.buffers[b].channel_count = info.channel_count;
        }
        return true;
    }
    uint32_t frames_ = 0;
    Buses input_, output_;
};
}  // namespace supple::probe
