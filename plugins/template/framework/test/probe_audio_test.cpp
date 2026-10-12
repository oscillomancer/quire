// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "Check.h"
#include "tools/ProbeAudio.h"
#include <limits>

namespace {
struct Ports {
    clap_plugin_t plugin{};
    std::vector<uint32_t> inputs, outputs;
    bool failGet = false;
    Ports(std::vector<uint32_t> in, std::vector<uint32_t> out) : inputs(in), outputs(out) {
        plugin.plugin_data = this;
        plugin.get_extension = [](const clap_plugin_t*, const char*) -> const void* {
            static const clap_plugin_audio_ports_t ports{
                [](const clap_plugin_t* p, bool input) -> uint32_t {
                    const auto& self = *static_cast<Ports*>(p->plugin_data);
                    return static_cast<uint32_t>((input ? self.inputs : self.outputs).size());
                },
                [](const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info) {
                    const auto& self = *static_cast<Ports*>(p->plugin_data);
                    const auto& buses = input ? self.inputs : self.outputs;
                    if (self.failGet || index >= buses.size()) return false;
                    info->channel_count = buses[index];
                    return true;
                }};
            return &ports;
        };
    }
};
}

TEST(probe_audio_uses_every_advertised_bus_and_channel) {
    Ports ports({8, 1}, {8, 2});
    supple::probe::Audio audio;
    CHECK(audio.configure(&ports.plugin, 17));
    clap_process_t process{};
    audio.bind(process);
    CHECK(process.frames_count == 17);
    CHECK(process.audio_inputs_count == 2 && process.audio_outputs_count == 2);
    CHECK(process.audio_inputs[0].channel_count == 8 && process.audio_inputs[1].channel_count == 1);
    CHECK(process.audio_outputs[0].channel_count == 8 && process.audio_outputs[1].channel_count == 2);
    double phase = 0;
    audio.fillInputs(phase);
    CHECK(process.audio_inputs[0].data32[7][16] != 0.0f);
    CHECK(process.audio_inputs[0].data32[0] != process.audio_outputs[0].data32[0]);
    process.audio_outputs[1].data32[1][16] = std::numeric_limits<float>::infinity();
    CHECK(!audio.finiteOutputs());
}

TEST(probe_audio_handles_mono_and_instruments_without_an_input_bus) {
    for (uint32_t channels : {1u, 2u, 8u}) {
        Ports ports({}, {channels});
        supple::probe::Audio audio;
        CHECK(audio.configure(&ports.plugin, 512));
        clap_process_t process{};
        audio.bind(process);
        CHECK(process.audio_inputs_count == 0 && process.audio_inputs == nullptr);
        CHECK(process.audio_outputs_count == 1 && process.audio_outputs[0].channel_count == channels);
        process.audio_outputs[0].data32[channels - 1][511] = 0.5f;
        CHECK(audio.finiteOutputs());
    }
}

TEST(probe_audio_takes_a_note_effect_without_audio) {
    Ports none({}, {});
    supple::probe::Audio audio;
    CHECK(audio.configure(&none.plugin, 512));
    CHECK(!audio.hasOutput());
    clap_process_t process{};
    audio.bind(process);
    CHECK(process.frames_count == 512);
    CHECK(process.audio_inputs == nullptr && process.audio_inputs_count == 0);
    CHECK(process.audio_outputs == nullptr && process.audio_outputs_count == 0);
    double phase = 0;
    audio.fillInputs(phase);
    CHECK(audio.finiteOutputs() && audio.outputPeak() == 0.0);
}

TEST(probe_audio_reports_the_output_peak) {
    Ports ports({2}, {2, 1});
    supple::probe::Audio audio;
    CHECK(audio.configure(&ports.plugin, 64));
    clap_process_t process{};
    audio.bind(process);
    CHECK(audio.hasOutput() && audio.outputPeak() == 0.0);
    process.audio_outputs[1].data32[0][63] = -0.5f;
    CHECK(audio.outputPeak() == 0.5);
}

TEST(probe_audio_refuses_missing_or_invalid_port_descriptions) {
    supple::probe::Audio audio;
    Ports zero({}, {0}), bad({2}, {2});
    CHECK(!audio.configure(&zero.plugin, 512));
    bad.failGet = true;
    CHECK(!audio.configure(&bad.plugin, 512));
    CHECK(!audio.configure(&bad.plugin, 0));
}

SUPPLE_TEST_MAIN
