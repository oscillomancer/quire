// Test fixtures: two small, deterministic CLAP plugins built in-tree so the
// host tests are hermetic.
//
//   com.suppleaudio.test.gain  — stereo effect; params "gain" (dB, ADR-0010
//     normalized CLAP value) and "latency" (0/64 samples, stepped; changing it
//     while active requests a restart); versioned state; the semantic-params
//     extension implemented with core-params + core-patch.
//   com.suppleaudio.test.sine  — instrument; 0 inputs, stereo output, CLAP and
//     MIDI note dialects; one sine voice per key at velocity * 0.25, stops at
//     note off and reports NOTE_END.
//   com.suppleaudio.test.edge  — stereo pass-through for host edge cases. Its
//     4-byte state is a configuration: byte 0 = refuse start_processing,
//     bytes 1..3 = reported tail in samples (little endian). Loading state
//     calls request_flush; param 7 ("Flushes") reads back how many times
//     params.flush has been called.
#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "supple/clap/semantic_params.h"
#include "supple/json/json.h"
#include "supple/params/params.h"
#include "supple/patch/patch.h"

namespace {

using namespace supple;

// --- gain -----------------------------------------------------------------------

constexpr params::Choice kLatencyChoices[] = {{"none", "0 samples"}, {"short", "64 samples"}};
constexpr params::ParamSpec kGainTable[] = {
    {.id = 0,
     .key = "gain",
     .name = "Gain",
     .type = params::Type::Float,
     .unit = units::Unit::dB,
     .min = -60,
     .max = 12,
     .def = 0,
     .digits = 2,
     .description = "Output gain.",
     .flags = params::kAutomatable | params::kModulatable | params::kOffAtMin},
    {.id = 1,
     .key = "latency",
     .name = "Latency",
     .type = params::Type::Enum,
     .max = 1,
     .def = 0,
     .choices = kLatencyChoices,
     .description = "Adds a fixed delay (restart required).",
     .flags = params::kAutomatable},
};
constexpr params::Table kGainT{kGainTable};

const patch::FlatPatchType& gainPatchType() {
    static const patch::FlatPatchType t("test-gain", 1, 0, kGainT, false);
    return t;
}

const clap_plugin_descriptor_t* gainDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.gain",
                                               "Test Gain",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Gain fixture",
                                               features};
    return &d;
}

const clap_plugin_descriptor_t* checkedGainDescriptor(bool reject) {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t checked = {CLAP_VERSION_INIT,
                                                     "com.suppleaudio.test.threaded-gain",
                                                     "Thread Checked Gain",
                                                     "Supple Audio",
                                                     "",
                                                     "",
                                                     "",
                                                     "0.1.0",
                                                     "Thread contract fixture",
                                                     features};
    static const clap_plugin_descriptor_t refusing = {CLAP_VERSION_INIT,
                                                      "com.suppleaudio.test.reject-gain",
                                                      "Reject Gain",
                                                      "Supple Audio",
                                                      "",
                                                      "",
                                                      "",
                                                      "0.1.0",
                                                      "Patch refusal fixture",
                                                      features};
    return reject ? &refusing : &checked;
}

const clap_plugin_descriptor_t* slowGainDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.slow-gain",
                                               "Slow Gain",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Slow patch fixture",
                                               features};
    return &d;
}

const clap_plugin_descriptor_t* badCurveDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.bad-curve",
                                               "Bad Curve",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Invalid semantic curve fixture",
                                               features};
    return &d;
}

std::atomic<int> gThreadFailures{0};
std::atomic<int> gPatchStarts{0};

struct Gain {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    std::atomic<double> gainDb{0.0};
    std::atomic<int> latencyChoice{0};
    int activeLatency = 0;  // latency in samples at activation
    double gain = 1.0;      // audio thread
    float delay[2][64] = {};
    int delayPos = 0;
    bool enforceThread = false;
    bool rejectPatch = false;
    bool slowPatch = false;
    bool badCurve = false;

    void applyParam(clap_id id, double clapValue) {
        if (const auto* p = params::findById(kGainT, id)) {
            const double v = params::fromClap(*p, clapValue);
            if (id == 0) gainDb = v;
            if (id == 1 && static_cast<int>(v) != latencyChoice.load()) {
                latencyChoice = static_cast<int>(v);
                if (host && host->request_restart) host->request_restart(host);
            }
        }
    }
    static double dbToGain(double db) { return db <= -60 ? 0.0 : std::pow(10.0, db / 20.0); }

    patch::Patch<patch::FlatBody> current() const {
        auto p = gainPatchType().defaults();
        p.body.values[0] = gainDb.load();
        p.body.values[1] = latencyChoice.load();
        return p;
    }
};

Gain* G(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

bool checkGainMain(const clap_plugin_t* p) {
    if (!G(p)->enforceThread) return true;
    const auto* h = G(p)->host;
    const auto* check =
        static_cast<const clap_host_thread_check_t*>(h->get_extension(h, CLAP_EXT_THREAD_CHECK));
    const bool ok = check && check->is_main_thread(h);
    if (!ok) gThreadFailures.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

uint32_t CLAP_ABI gainParamCount(const clap_plugin_t*) { return 2; }
bool CLAP_ABI gainParamInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index >= 2) return false;
    const auto& p = kGainTable[index];
    std::memset(info, 0, sizeof(*info));
    info->id = p.id;
    info->flags = params::clapFlags(p);
    std::snprintf(info->name, sizeof(info->name), "%.*s", static_cast<int>(p.name.size()),
                  p.name.data());
    const auto r = params::clapRange(p);
    info->min_value = r.min;
    info->max_value = r.max;
    info->default_value = r.def;
    return true;
}
bool CLAP_ABI gainParamValue(const clap_plugin_t* pl, clap_id id, double* out) {
    auto* g = G(pl);
    if (id == 0)
        *out = params::toClap(kGainTable[0], g->gainDb);
    else if (id == 1)
        *out = g->latencyChoice;
    else
        return false;
    return true;
}
bool CLAP_ABI gainValueToText(const clap_plugin_t*, clap_id id, double v, char* out,
                              uint32_t size) {
    const auto* p = params::findById(kGainT, id);
    if (!p) return false;
    std::snprintf(out, size, "%s", params::format(*p, params::fromClap(*p, v)).c_str());
    return true;
}
bool CLAP_ABI gainTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
    const auto* p = params::findById(kGainT, id);
    if (!p) return false;
    auto v = params::parse(*p, text);
    if (!v) return false;
    *out = params::toClap(*p, *v);
    return true;
}
void CLAP_ABI gainFlush(const clap_plugin_t* pl, const clap_input_events_t* in,
                        const clap_output_events_t*) {
    for (uint32_t i = 0; i < in->size(in); ++i) {
        const auto* h = in->get(in, i);
        if (h->type == CLAP_EVENT_PARAM_VALUE)
            G(pl)->applyParam(reinterpret_cast<const clap_event_param_value_t*>(h)->param_id,
                              reinterpret_cast<const clap_event_param_value_t*>(h)->value);
    }
}
const clap_plugin_params_t kGainParams = {gainParamCount,  gainParamInfo,   gainParamValue,
                                          gainValueToText, gainTextToValue, gainFlush};

uint32_t CLAP_ABI gainLatency(const clap_plugin_t* pl) {
    return static_cast<uint32_t>(G(pl)->activeLatency);
}
const clap_plugin_latency_t kGainLatency = {gainLatency};

bool CLAP_ABI gainStateSave(const clap_plugin_t* pl, const clap_ostream_t* os) {
    const std::string doc = gainPatchType().serialize(G(pl)->current());
    auto bytes = patch::encodeState(doc, 0x54474e31);  // "TGN1"
    if (!bytes) return false;
    return os->write(os, bytes->data(), bytes->size()) == static_cast<int64_t>(bytes->size());
}
bool CLAP_ABI gainStateLoad(const clap_plugin_t* pl, const clap_istream_t* is) {
    std::vector<std::uint8_t> buf;
    std::uint8_t tmp[512];
    for (;;) {
        const int64_t n = is->read(is, tmp, sizeof(tmp));
        if (n < 0) return false;
        if (n == 0) break;
        buf.insert(buf.end(), tmp, tmp + n);
    }
    auto text = patch::decodeState(buf, 0x54474e31);
    if (!text) return false;
    auto p = gainPatchType().parse(*text);
    if (!p) return false;
    G(pl)->gainDb = p->body.values[0];
    G(pl)->latencyChoice = static_cast<int>(p->body.values[1]);
    return true;
}
const clap_plugin_state_t kGainState = {gainStateSave, gainStateLoad};

bool checkedGain(const clap_plugin_t* p) {
    return !std::strcmp(p->desc->id, checkedGainDescriptor(false)->id) ||
           !std::strcmp(p->desc->id, checkedGainDescriptor(true)->id) ||
           !std::strcmp(p->desc->id, slowGainDescriptor()->id) ||
           !std::strcmp(p->desc->id, badCurveDescriptor()->id);
}
uint32_t CLAP_ABI gainPortCount(const clap_plugin_t* p, bool input) {
    return input && checkedGain(p) ? 0 : 1;
}
bool CLAP_ABI gainPortGet(const clap_plugin_t*, uint32_t index, bool isInput,
                          clap_audio_port_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kGainPorts = {gainPortCount, gainPortGet};
extern const clap_plugin_note_ports_t kSineNotes;

// semantic extension
bool writeString(const clap_ostream_t* os, const std::string& s) {
    return os->write(os, s.data(), s.size()) == static_cast<int64_t>(s.size());
}
std::string errorsJson(const Error& e) {
    json::Value arr = json::Value::array();
    auto one = [](const Error& x) {
        json::Value v = json::Value::object();
        v["code"] = x.code;
        v["message"] = x.message;
        v["path"] = x.path;
        v["hint"] = x.hint;
        return v;
    };
    if (e.details.empty()) arr.push_back(one(e));
    for (const auto& d : e.details) arr.push_back(one(d));
    return json::writeCompact(arr);
}
bool CLAP_ABI semSchema(const clap_plugin_t* pl, const clap_ostream_t* out) {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.semantic-schema/1.0";
    doc["plugin"]["id"] = pl->desc->id;
    doc["patch_type"] = gainPatchType().schemaId();
    doc["params"] = *params::describeTable(kGainT).find("params");
    if (G(pl)->badCurve) doc["params"]["gain"]["curve"] = "pow:invalid";
    doc["patch"] = gainPatchType().jsonSchema();
    return writeString(out, json::writeCompact(doc));
}
bool CLAP_ABI semGetPatch(const clap_plugin_t* pl, uint32_t detail, const clap_ostream_t* out) {
    return writeString(
        out, gainPatchType().serialize(G(pl)->current(), detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED
                                                             ? patch::Detail::Changed
                                                             : patch::Detail::Full));
}
bool CLAP_ABI semSetPatch(const clap_plugin_t* pl, uint32_t mode, const char* text, uint32_t size,
                          const clap_ostream_t* errors) {
    if (!checkGainMain(pl)) return false;
    if (G(pl)->slowPatch) {
        gPatchStarts.fetch_add(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    if (G(pl)->rejectPatch) {
        if (errors) writeString(errors, errorsJson(Error("fixture.reject", "patch rejected")));
        return false;
    }
    auto doc = json::parse(std::string_view(text, size));
    if (!doc) {
        if (errors) writeString(errors, errorsJson(doc.error()));
        return false;
    }
    auto result = mode == CLAP_SUPPLE_SET_REPLACE ? gainPatchType().fromJson(*doc)
                                                  : gainPatchType().merge(G(pl)->current(), *doc);
    if (!result) {
        if (errors) writeString(errors, errorsJson(result.error()));
        return false;
    }
    if (mode == CLAP_SUPPLE_SET_VALIDATE_ONLY) return true;
    G(pl)->gainDb = result->body.values[0];
    const int latency = static_cast<int>(result->body.values[1]);
    if (latency != G(pl)->latencyChoice.exchange(latency) && G(pl)->host->request_restart)
        G(pl)->host->request_restart(G(pl)->host);
    return true;
}
bool CLAP_ABI semDescribe(const clap_plugin_t* pl, uint32_t detail, const clap_ostream_t* out) {
    return writeString(out,
                       "Gain: " + patch::describeFlat(gainPatchType(), G(pl)->current(),
                                                      detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED
                                                          ? patch::Detail::Changed
                                                          : patch::Detail::Full));
}
const clap_plugin_supple_semantic_params_t kGainSemantic = {
    sizeof(clap_plugin_supple_semantic_params_t), semSchema, semGetPatch, semSetPatch, semDescribe};

const void* CLAP_ABI gainExt(const clap_plugin_t* pl, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kGainParams;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kGainLatency;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kGainState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
    if (checkedGain(pl) && !std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kSineNotes;
    if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS)) return &kGainSemantic;
    return nullptr;
}

bool CLAP_ABI gainActivate(const clap_plugin_t* pl, double, uint32_t, uint32_t) {
    if (!checkGainMain(pl)) return false;
    auto* g = G(pl);
    g->activeLatency = g->latencyChoice ? 64 : 0;
    std::memset(g->delay, 0, sizeof(g->delay));
    g->delayPos = 0;
    g->gain = Gain::dbToGain(g->gainDb);
    return true;
}

clap_process_status CLAP_ABI gainProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    auto* g = G(pl);
    const uint32_t n = p->frames_count;
    const uint32_t nev = p->in_events->size(p->in_events);
    uint32_t ev = 0;
    for (uint32_t i = 0; i < n; ++i) {
        while (ev < nev) {
            const auto* h = p->in_events->get(p->in_events, ev);
            if (h->time > i) break;
            if (h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(h);
                g->applyParam(pv->param_id, pv->value);
            }
            ++ev;
        }
        g->gain = Gain::dbToGain(g->gainDb);  // no smoothing: tests check sample-exact changes
        for (int c = 0; c < 2; ++c) {
            float x = p->audio_inputs_count > 0 ? p->audio_inputs[0].data32[c][i] : 0.0f;
            if (g->activeLatency) {
                float& slot = g->delay[c][g->delayPos];
                const float delayed = slot;
                slot = x;
                x = delayed;
            }
            p->audio_outputs[0].data32[c][i] = static_cast<float>(static_cast<double>(x) * g->gain);
        }
        if (g->activeLatency) g->delayPos = (g->delayPos + 1) % 64;
    }
    return CLAP_PROCESS_CONTINUE;
}

// --- sine -------------------------------------------------------------------------

const clap_plugin_descriptor_t* sineDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                     CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.sine",
                                               "Test Sine",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Sine fixture",
                                               features};
    return &d;
}

struct Voice {
    bool on = false;
    int key = 0, channel = 0;
    int32_t noteId = -1;
    double phase = 0, inc = 0, amp = 0;
};
struct Sine {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    double sampleRate = 48000;
    Voice voices[8];
};
Sine* S(const clap_plugin_t* p) { return static_cast<Sine*>(p->plugin_data); }

uint32_t CLAP_ABI sinePortCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }
bool CLAP_ABI sinePortGet(const clap_plugin_t* p, uint32_t index, bool isInput,
                          clap_audio_port_info_t* info) {
    if (isInput) return false;
    return gainPortGet(p, index, false, info);
}
const clap_plugin_audio_ports_t kSinePorts = {sinePortCount, sinePortGet};
uint32_t CLAP_ABI sineNoteCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool CLAP_ABI sineNoteGet(const clap_plugin_t*, uint32_t index, bool isInput,
                          clap_note_port_info_t* info) {
    if (!isInput || index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}
const clap_plugin_note_ports_t kSineNotes = {sineNoteCount, sineNoteGet};
const void* CLAP_ABI sineExt(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kSinePorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kSineNotes;
    return nullptr;
}
bool CLAP_ABI sineActivate(const clap_plugin_t* pl, double sr, uint32_t, uint32_t) {
    S(pl)->sampleRate = sr;
    for (auto& v : S(pl)->voices) v = {};
    return true;
}
void noteOn(Sine* s, int channel, int key, int32_t id, double velocity) {
    for (auto& v : s->voices) {
        if (v.on) continue;
        v = {true,
             key,
             channel,
             id,
             0.0,
             440.0 * std::pow(2.0, (key - 69) / 12.0) / s->sampleRate,
             0.25 * velocity};
        return;
    }
}
void noteOff(Sine* s, const clap_output_events_t* out, uint32_t time, int channel, int key) {
    for (auto& v : s->voices) {
        if (!v.on || v.key != key || v.channel != channel) continue;
        v.on = false;
        clap_event_note_t end{};
        end.header = {sizeof(end), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0};
        end.note_id = v.noteId;
        end.key = static_cast<int16_t>(key);
        end.channel = static_cast<int16_t>(channel);
        out->try_push(out, &end.header);
    }
}
clap_process_status CLAP_ABI sineProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    auto* s = S(pl);
    const uint32_t n = p->frames_count;
    const uint32_t nev = p->in_events->size(p->in_events);
    uint32_t ev = 0;
    float* L = p->audio_outputs[0].data32[0];
    float* R = p->audio_outputs[0].data32[1];
    for (uint32_t i = 0; i < n; ++i) {
        while (ev < nev) {
            const auto* h = p->in_events->get(p->in_events, ev);
            if (h->time > i) break;
            if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
                const auto* ne = reinterpret_cast<const clap_event_note_t*>(h);
                if (h->type == CLAP_EVENT_NOTE_ON)
                    noteOn(s, ne->channel, ne->key, ne->note_id, ne->velocity);
                else
                    noteOff(s, p->out_events, i, ne->channel, ne->key);
            } else if (h->type == CLAP_EVENT_MIDI) {
                const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                const int status = m->data[0] & 0xf0, ch = m->data[0] & 0x0f;
                if (status == 0x90 && m->data[2] > 0)
                    noteOn(s, ch, m->data[1], -1, m->data[2] / 127.0);
                else if (status == 0x80 || (status == 0x90 && m->data[2] == 0))
                    noteOff(s, p->out_events, i, ch, m->data[1]);
            }
            ++ev;
        }
        double y = 0;
        for (auto& v : s->voices) {
            if (!v.on) continue;
            y += v.amp * std::sin(2.0 * M_PI * v.phase);
            v.phase += v.inc;
            v.phase -= std::floor(v.phase);
        }
        L[i] = R[i] = static_cast<float>(y);
    }
    return CLAP_PROCESS_CONTINUE;
}

// --- edge -------------------------------------------------------------------------

const clap_plugin_descriptor_t* edgeDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
    static const clap_plugin_descriptor_t d = {
        CLAP_VERSION_INIT, "com.suppleaudio.test.edge", "Test Edge", "Supple Audio", "", "", "",
        "0.1.0",           "Host edge-case fixture",    features};
    return &d;
}

struct Edge {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    std::uint8_t config[4] = {0, 0, 0, 0};
    std::atomic<int> flushes{0};
};
Edge* E(const clap_plugin_t* p) { return static_cast<Edge*>(p->plugin_data); }

uint32_t CLAP_ABI edgeParamCount(const clap_plugin_t*) { return 1; }
bool CLAP_ABI edgeParamInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 7;
    info->flags = CLAP_PARAM_IS_READONLY;
    std::snprintf(info->name, sizeof(info->name), "Flushes");
    info->max_value = 1e9;
    return true;
}
bool CLAP_ABI edgeParamValue(const clap_plugin_t* pl, clap_id id, double* out) {
    if (id != 7) return false;
    *out = E(pl)->flushes.load();
    return true;
}
bool CLAP_ABI edgeValueToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
    std::snprintf(out, size, "%d", static_cast<int>(v));
    return true;
}
bool CLAP_ABI edgeTextToValue(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void CLAP_ABI edgeFlush(const clap_plugin_t* pl, const clap_input_events_t*,
                        const clap_output_events_t*) {
    ++E(pl)->flushes;
}
const clap_plugin_params_t kEdgeParams = {edgeParamCount,  edgeParamInfo,   edgeParamValue,
                                          edgeValueToText, edgeTextToValue, edgeFlush};
uint32_t CLAP_ABI edgeTail(const clap_plugin_t* pl) {
    const auto* c = E(pl)->config;
    return static_cast<uint32_t>(c[1]) | (static_cast<uint32_t>(c[2]) << 8) |
           (static_cast<uint32_t>(c[3]) << 16);
}
const clap_plugin_tail_t kEdgeTail = {edgeTail};
bool CLAP_ABI edgeStateSave(const clap_plugin_t* pl, const clap_ostream_t* os) {
    return os->write(os, E(pl)->config, 4) == 4;
}
bool CLAP_ABI edgeStateLoad(const clap_plugin_t* pl, const clap_istream_t* is) {
    std::uint8_t buf[4];
    if (is->read(is, buf, 4) != 4) return false;
    std::memcpy(E(pl)->config, buf, 4);
    const auto* hp = static_cast<const clap_host_params_t*>(
        E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_PARAMS));
    if (hp) hp->request_flush(E(pl)->host);
    return true;
}
const clap_plugin_state_t kEdgeState = {edgeStateSave, edgeStateLoad};
const void* CLAP_ABI edgeExt(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kEdgeParams;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kEdgeTail;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kEdgeState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
    return nullptr;
}
bool CLAP_ABI edgeActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
bool CLAP_ABI edgeStart(const clap_plugin_t* pl) { return E(pl)->config[0] == 0; }
clap_process_status CLAP_ABI edgeProcess(const clap_plugin_t*, const clap_process_t* p) {
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < p->frames_count; ++i)
            p->audio_outputs[0].data32[c][i] = p->audio_inputs[0].data32[c][i];
    return CLAP_PROCESS_CONTINUE;
}

// --- crash (the isolation fixture) -------------------------------------------------------
// An effect that passes audio through until its Mode parameter says otherwise:
// 1 crashes the process in process() (SIGSEGV), 2 hangs it there.

const clap_plugin_descriptor_t* crashDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
    static const clap_plugin_descriptor_t d = {
        CLAP_VERSION_INIT,
        "com.suppleaudio.test.crash",
        "Test Crash",
        "Supple Audio",
        "",
        "",
        "",
        "0.1.0",
        "Crashes or hangs on request (plugin isolation tests)",
        features};
    return &d;
}

struct Crash {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    std::atomic<int> mode{0};
};
Crash* C(const clap_plugin_t* p) { return static_cast<Crash*>(p->plugin_data); }

uint32_t CLAP_ABI crashParamCount(const clap_plugin_t*) { return 1; }
bool CLAP_ABI crashParamInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 1;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "Mode");
    info->max_value = 2;
    return true;
}
bool CLAP_ABI crashParamValue(const clap_plugin_t* pl, clap_id id, double* out) {
    if (id != 1) return false;
    *out = C(pl)->mode.load();
    return true;
}
bool CLAP_ABI crashValueToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
    static const char* kNames[] = {"pass", "crash", "hang"};
    std::snprintf(out, size, "%s", kNames[std::clamp(static_cast<int>(v), 0, 2)]);
    return true;
}
bool CLAP_ABI crashTextToValue(const clap_plugin_t*, clap_id, const char*, double*) {
    return false;
}
void crashEvents(const clap_plugin_t* pl, const clap_input_events_t* in) {
    for (uint32_t i = 0, n = in->size(in); i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (ev->param_id == 1) C(pl)->mode = static_cast<int>(ev->value + 0.5);
        }
    }
}
void CLAP_ABI crashFlush(const clap_plugin_t* pl, const clap_input_events_t* in,
                         const clap_output_events_t*) {
    crashEvents(pl, in);
}
const clap_plugin_params_t kCrashParams = {crashParamCount,  crashParamInfo,   crashParamValue,
                                           crashValueToText, crashTextToValue, crashFlush};
const void* CLAP_ABI crashExt(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kCrashParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
    return nullptr;
}
bool CLAP_ABI crashActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
clap_process_status CLAP_ABI crashProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    crashEvents(pl, p->in_events);
    if (C(pl)->mode.load() == 1) std::raise(SIGSEGV);
    while (C(pl)->mode.load() == 2) {  // spin (no system call: sanitizers see a hang, not a call)
    }
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < p->frames_count; ++i)
            p->audio_outputs[0].data32[c][i] = p->audio_inputs[0].data32[c][i];
    return CLAP_PROCESS_CONTINUE;
}

// --- shared lifecycle ------------------------------------------------------------------

bool CLAP_ABI checkedGainInit(const clap_plugin_t* p) { return checkGainMain(p); }
void CLAP_ABI checkedGainDeactivate(const clap_plugin_t* p) { (void)checkGainMain(p); }
void CLAP_ABI checkedGainDestroy(const clap_plugin_t* p) {
    (void)checkGainMain(p);
    delete G(p);
}

bool CLAP_ABI initOk(const clap_plugin_t*) { return true; }
void CLAP_ABI deactivateNop(const clap_plugin_t*) {}
bool CLAP_ABI startOk(const clap_plugin_t*) { return true; }
void CLAP_ABI stopNop(const clap_plugin_t*) {}
void CLAP_ABI resetNop(const clap_plugin_t*) {}
void CLAP_ABI mainNop(const clap_plugin_t*) {}

template <class T>
void CLAP_ABI destroyT(const clap_plugin_t* p) {
    delete static_cast<T*>(p->plugin_data);
}

template <class T>
const clap_plugin_t* make(const clap_host_t* host, const clap_plugin_descriptor_t* desc,
                          bool(CLAP_ABI* activate)(const clap_plugin_t*, double, uint32_t,
                                                   uint32_t),
                          clap_process_status(CLAP_ABI* process)(const clap_plugin_t*,
                                                                 const clap_process_t*),
                          const void*(CLAP_ABI* ext)(const clap_plugin_t*, const char*)) {
    auto* self = new T();
    self->host = host;
    self->plugin = {desc,    self,    initOk,   destroyT<T>, activate, deactivateNop,
                    startOk, stopNop, resetNop, process,     ext,      mainNop};
    return &self->plugin;
}

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) { return 8; }
const clap_plugin_descriptor_t* CLAP_ABI factoryDesc(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0   ? gainDescriptor()
           : i == 1 ? sineDescriptor()
           : i == 2 ? edgeDescriptor()
           : i == 3 ? crashDescriptor()
           : i == 4 ? checkedGainDescriptor(false)
           : i == 5 ? checkedGainDescriptor(true)
           : i == 6 ? slowGainDescriptor()
           : i == 7 ? badCurveDescriptor()
                    : nullptr;
}
const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                            const char* id) {
    if (!std::strcmp(id, gainDescriptor()->id))
        return make<Gain>(host, gainDescriptor(), gainActivate, gainProcess, gainExt);
    if (!std::strcmp(id, checkedGainDescriptor(false)->id) ||
        !std::strcmp(id, checkedGainDescriptor(true)->id) ||
        !std::strcmp(id, slowGainDescriptor()->id) || !std::strcmp(id, badCurveDescriptor()->id)) {
        const bool reject = !std::strcmp(id, checkedGainDescriptor(true)->id);
        const bool slow = !std::strcmp(id, slowGainDescriptor()->id);
        const bool badCurve = !std::strcmp(id, badCurveDescriptor()->id);
        const clap_plugin_t* p = make<Gain>(host,
                                            badCurve ? badCurveDescriptor()
                                            : slow   ? slowGainDescriptor()
                                                     : checkedGainDescriptor(reject),
                                            gainActivate, gainProcess, gainExt);
        G(p)->enforceThread = true;
        G(p)->rejectPatch = reject;
        G(p)->slowPatch = slow;
        G(p)->badCurve = badCurve;
        auto* mutablePlugin = const_cast<clap_plugin_t*>(p);
        mutablePlugin->init = checkedGainInit;
        mutablePlugin->deactivate = checkedGainDeactivate;
        mutablePlugin->destroy = checkedGainDestroy;
        return p;
    }
    if (!std::strcmp(id, sineDescriptor()->id)) {
        const clap_plugin_t* p =
            make<Sine>(host, sineDescriptor(), sineActivate, sineProcess, sineExt);
        // clap_plugin.reset: silence every voice (a reused instance must not
        // carry a hanging note into the next render).
        const_cast<clap_plugin_t*>(p)->reset = [](const clap_plugin_t* pl) {
            for (auto& v : S(pl)->voices) v = {};
        };
        return p;
    }
    if (!std::strcmp(id, edgeDescriptor()->id)) {
        const clap_plugin_t* p =
            make<Edge>(host, edgeDescriptor(), edgeActivate, edgeProcess, edgeExt);
        const_cast<clap_plugin_t*>(p)->start_processing = edgeStart;
        return p;
    }
    if (!std::strcmp(id, crashDescriptor()->id))
        return make<Crash>(host, crashDescriptor(), crashActivate, crashProcess, crashExt);
    return nullptr;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDesc, factoryCreate};

bool CLAP_ABI entryInit(const char*) { return true; }
void CLAP_ABI entryDeinit() {}
const void* CLAP_ABI entryFactory(const char* id) {
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT int supple_fixture_thread_failures() {
    return gThreadFailures.load(std::memory_order_relaxed);
}
extern "C" CLAP_EXPORT int supple_fixture_patch_starts() {
    return gPatchStarts.load(std::memory_order_acquire);
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit,
                                                               entryDeinit, entryFactory};
