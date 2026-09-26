// A stock-plugin-shaped fixture built only from plugin_support: shows the
// intended wiring and feeds the semantic conformance test.
#include <clap/clap.h>

#include <cstring>

#include "supple/clap/plugin_support.h"

namespace {

using namespace supple;

constexpr params::Choice kModes[] = {{"gentle", "Gentle"}, {"steep", "Steep"}};
constexpr std::string_view kTiltTags[] = {"role:brightness"};
constexpr params::ParamSpec kTable[] = {
    {.id = 0,
     .key = "tilt",
     .name = "Tilt",
     .unit = units::Unit::dB,
     .min = -6,
     .max = 6,
     .def = 0,
     .curve = {params::CurveKind::Bipolar, 1.5},
     .digits = 2,
     .tags = kTiltTags,
     .description = "Spectral tilt around the pivot.",
     .flags = params::kAutomatable | params::kModulatable},
    {.id = 1,
     .key = "pivot",
     .name = "Pivot",
     .unit = units::Unit::Hz,
     .min = 100,
     .max = 10000,
     .def = 1000,
     .curve = {params::CurveKind::Log},
     .description = "Pivot frequency.",
     .flags = params::kAutomatable},
    {.id = 2,
     .key = "mode",
     .name = "Mode",
     .type = params::Type::Enum,
     .max = 1,
     .def = 0,
     .choices = kModes,
     .description = "Slope family.",
     .flags = params::kAutomatable},
    {.id = 3,
     .key = "mix",
     .name = "Mix",
     .unit = units::Unit::percent,
     .min = 0,
     .max = 100,
     .def = 100,
     .description = "Dry/wet.",
     .flags = params::kAutomatable},
    {.id = 4,
     .key = "meter",
     .name = "Meter",
     .unit = units::Unit::dB,
     .min = -120,
     .max = 6,
     .def = -120,
     .flags = params::kReadonly},
};

const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType t("test-tilt", 1, 0, kTable, false);
    return t;
}

struct Tilt {
    clap_plugin_t plugin{};
    clapx::ParamStore store{kTable};
    clapx::FlatSemantic semantic{patchType(),          store,    "com.suppleaudio.test.tilt",
                                 "A tilt EQ fixture.", "Tilt: ", 0x54494c54};
};
Tilt* T(const clap_plugin_t* p) { return static_cast<Tilt*>(p->plugin_data); }
clapx::FlatSemantic* resolve(const clap_plugin_t* p) { return &T(p)->semantic; }

const clap_plugin_params_t kParams = {
    [](const clap_plugin_t*) { return clapx::ClapParams::count(kTable); },
    [](const clap_plugin_t*, uint32_t i, clap_param_info_t* o) {
        return clapx::ClapParams::info(kTable, i, o);
    },
    [](const clap_plugin_t* p, clap_id id, double* v) { return T(p)->store.getClap(id, *v); },
    [](const clap_plugin_t*, clap_id id, double v, char* o, uint32_t n) {
        return clapx::ClapParams::valueToText(kTable, id, v, o, n);
    },
    [](const clap_plugin_t*, clap_id id, const char* t, double* v) {
        return clapx::ClapParams::textToValue(kTable, id, t, v);
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
        clapx::ClapParams::applyEvents(T(p)->store, in);
    }};
const clap_plugin_state_t kState = {
    [](const clap_plugin_t* p, const clap_ostream_t* o) { return T(p)->semantic.saveState(o); },
    [](const clap_plugin_t* p, const clap_istream_t* i) { return T(p)->semantic.loadState(i); }};
const clap_plugin_audio_ports_t kPorts = {
    [](const clap_plugin_t*, bool) -> uint32_t { return 1; },
    [](const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) {
        if (index) return false;
        std::memset(info, 0, sizeof(*info));
        std::strcpy(info->name, input ? "In" : "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }};

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT, "com.suppleaudio.test.tilt", "Test Tilt", "Supple Audio", "", "", "",
    "0.1.0",           "plugin_support fixture",    kFeatures};

const clap_plugin_t* create(const clap_host_t*) {
    auto* self = new Tilt();
    self->plugin = {&kDesc,
                    self,
                    [](const clap_plugin_t*) { return true; },
                    [](const clap_plugin_t* p) { delete T(p); },
                    [](const clap_plugin_t*, double, uint32_t, uint32_t) { return true; },
                    [](const clap_plugin_t*) {},
                    [](const clap_plugin_t*) { return true; },
                    [](const clap_plugin_t*) {},
                    [](const clap_plugin_t*) {},
                    [](const clap_plugin_t* p, const clap_process_t* pr) -> clap_process_status {
                        clapx::ClapParams::applyEvents(T(p)->store, pr->in_events);
                        const float mix = static_cast<float>(T(p)->store.get(3) / 100.0);
                        for (uint32_t c = 0; c < 2; ++c)
                            for (uint32_t i = 0; i < pr->frames_count; ++i)
                                pr->audio_outputs[0].data32[c][i] =
                                    pr->audio_inputs[0].data32[c][i] * mix;
                        return CLAP_PROCESS_CONTINUE;
                    },
                    [](const clap_plugin_t*, const char* id) -> const void* {
                        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
                        if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
                        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
                        if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS))
                            return clapx::FlatSemantic::extension<resolve>();
                        return nullptr;
                    },
                    [](const clap_plugin_t*) {}};
    return &self->plugin;
}

const clap_plugin_factory_t kFactory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDesc : nullptr; },
    [](const clap_plugin_factory_t*, const clap_host_t* h, const char* id) {
        return std::strcmp(id, kDesc.id) ? nullptr : create(h);
    }};

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, [](const char*) { return true; }, [] {},
    [](const char* id) -> const void* {
        return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &kFactory;
    }};
