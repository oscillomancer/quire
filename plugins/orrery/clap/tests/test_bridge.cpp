// The editor bridge (orrery/editor_bridge.h) against a minimal in-process host:
// gestures reach the host as CLAP events and the engine as values, both while
// processing and through params.flush; structural merges; the extension hook.
#include <doctest/doctest.h>

#include <clap/clap.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "orrery/clap_plugin.h"
#include "orrery/params.h"
#include "supple/json/json.h"

using namespace supple;
using namespace supple::orrery;

namespace {

struct TestHost {
    clap_host_t host{};
    clap_host_params_t params{};
    int flushRequests = 0, callbacks = 0;

    TestHost() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "bridge-test";
        host.vendor = "";
        host.url = "";
        host.version = "1";
        host.get_extension = [](const clap_host_t* h, const char* id) -> const void* {
            auto* t = static_cast<TestHost*>(h->host_data);
            return std::string(id) == CLAP_EXT_PARAMS ? &t->params : nullptr;
        };
        host.request_restart = [](const clap_host_t*) {};
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t* h) {
            ++static_cast<TestHost*>(h->host_data)->callbacks;
        };
        params.rescan = [](const clap_host_t*, clap_param_rescan_flags) {};
        params.clear = [](const clap_host_t*, clap_id, clap_param_clear_flags) {};
        params.request_flush = [](const clap_host_t* h) {
            ++static_cast<TestHost*>(h->host_data)->flushRequests;
        };
    }
};

// Output events the plugin pushes; refuses beyond `capacity`.
struct OutEvents {
    clap_output_events_t out{};
    struct Rec {
        std::uint16_t type;
        clap_id id;
        double value;
    };
    std::vector<Rec> got;
    std::size_t capacity = 1000;
    OutEvents() {
        got.reserve(capacity);  // try_push runs on the audio thread: never allocate there
        out.ctx = this;
        out.try_push = [](const clap_output_events_t* o, const clap_event_header_t* h) {
            auto* self = static_cast<OutEvents*>(o->ctx);
            if (self->got.size() >= self->capacity) return false;
            if (h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
                self->got.push_back({h->type, e->param_id, e->value});
            } else {
                const auto* e = reinterpret_cast<const clap_event_param_gesture_t*>(h);
                self->got.push_back({h->type, e->param_id, 0.0});
            }
            return true;
        };
    }
};

struct InEvents {
    clap_input_events_t in{};
    std::vector<clap_event_note_t> notes;
    const clap_event_header_t* single = nullptr;
    InEvents() {
        in.ctx = this;
        in.size = [](const clap_input_events_t* i) {
            auto* self = static_cast<InEvents*>(i->ctx);
            return self->single ? 1u : static_cast<std::uint32_t>(self->notes.size());
        };
        in.get = [](const clap_input_events_t* i, std::uint32_t k) -> const clap_event_header_t* {
            auto* self = static_cast<InEvents*>(i->ctx);
            return self->single ? self->single : &self->notes[k].header;
        };
    }
    void noteOn(int key) {
        clap_event_note_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
        e.note_id = -1;
        e.port_index = 0;
        e.channel = 0;
        e.key = static_cast<std::int16_t>(key);
        e.velocity = 1.0;
        notes.push_back(e);
    }
};

struct ModuleData {
    EditorBridge* bridge = nullptr;
    int mainThreadCalls = 0;
};
int destroyed = 0;
const int kProbeExtension = 42;

const ClapExtensions kExtensions = {
    [](const clap_plugin_t*, const clap_host_t*, EditorBridge& b) -> void* {
        return new ModuleData{&b, 0};
    },
    [](void* d) {
        delete static_cast<ModuleData*>(d);
        ++destroyed;
    },
    [](const char* id) -> const void* {
        return std::string(id) == "test.probe" ? &kProbeExtension : nullptr;
    },
    [](void* d) { ++static_cast<ModuleData*>(d)->mainThreadCalls; },
};

struct Instance {
    TestHost th;
    const clap_plugin_t* p = nullptr;
    ModuleData* data = nullptr;
    std::vector<float> l = std::vector<float>(256), r = std::vector<float>(256);
    Instance() {
        p = createClapPlugin(&th.host, &kExtensions);
        REQUIRE(p);
        REQUIRE(p->init(p));
        data = static_cast<ModuleData*>(extensionData(p));
        REQUIRE(data);
    }
    ~Instance() { p->destroy(p); }
    EditorBridge& bridge() { return *data->bridge; }
    void start() {
        REQUIRE(p->activate(p, 48000.0, 16, 256));
        REQUIRE(p->start_processing(p));
    }
    // One 256-frame block; returns the block's RMS.
    double block(InEvents& in, OutEvents& out) {
        float* chans[2] = {l.data(), r.data()};
        clap_audio_buffer_t buf{};
        buf.data32 = chans;
        buf.channel_count = 2;
        clap_process_t pr{};
        pr.frames_count = 256;
        pr.audio_outputs = &buf;
        pr.audio_outputs_count = 1;
        pr.in_events = &in.in;
        pr.out_events = &out.out;
        p->process(p, &pr);
        double s = 0.0;
        for (float x : l) s += static_cast<double>(x) * static_cast<double>(x);
        return std::sqrt(s / 256.0);
    }
    const clap_plugin_params_t* params() const {
        return static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
    }
};

std::size_t indexOf(std::string_view key) {
    return params::indexOf(table(), *params::findByKey(table(), key));
}

}  // namespace

TEST_CASE("short CLAP input payloads cannot change parameters or start voices") {
    Instance instance;
    InEvents in;
    OutEvents out;
    const auto id = table()[indexOf("voice.drift")].id;
    double before = 0;
    REQUIRE(instance.params()->get_value(instance.p, id, &before));
    clap_event_param_value_t param{};
    param.header = {sizeof(clap_event_header_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                    CLAP_EVENT_PARAM_VALUE, 0};
    param.param_id = id;
    param.value = 1;
    param.note_id = param.port_index = param.channel = param.key = -1;
    in.single = &param.header;
    instance.params()->flush(instance.p, &in.in, &out.out);
    double after = 0;
    REQUIRE(instance.params()->get_value(instance.p, id, &after));
    CHECK(after == before);

    instance.start();
    instance.block(in, out);
    REQUIRE(instance.params()->get_value(instance.p, id, &after));
    CHECK(after == before);
    param.header.size = sizeof(param);
    instance.block(in, out);
    REQUIRE(instance.params()->get_value(instance.p, id, &after));
    CHECK(after == 1);

    clap_event_note_t note{};
    note.header = {sizeof(clap_event_header_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
    note.note_id = 1;
    note.key = 60;
    note.velocity = .8;
    clap_event_midi_t midi{};
    midi.header = {sizeof(clap_event_header_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    midi.data[0] = 0x90;
    midi.data[1] = 60;
    midi.data[2] = 100;
    for (auto* header : {&note.header, &midi.header}) {
        instance.p->reset(instance.p);
        in.single = header;
        CHECK(instance.block(in, out) == 0);
        header->size = header == &note.header ? sizeof(note) : sizeof(midi);
        CHECK(instance.block(in, out) > 0);
    }
    for (int invalid = 0; invalid < 7; ++invalid) {
        instance.p->reset(instance.p);
        note.velocity = .8;
        note.port_index = 0;
        note.key = 60;
        note.channel = 0;
        note.note_id = 1;
        switch (invalid) {
            case 0:
                note.velocity = std::numeric_limits<double>::quiet_NaN();
                break;
            case 1:
                note.velocity = std::numeric_limits<double>::infinity();
                break;
            case 2:
                note.velocity = 1.1;
                break;
            case 3:
                note.port_index = 1;
                break;
            case 4:
                note.key = -1;
                break;
            case 5:
                note.channel = 16;
                break;
            case 6:
                note.note_id = -2;
                break;
        }
        in.single = &note.header;
        CHECK(instance.block(in, out) == 0);
    }
    instance.p->reset(instance.p);
    midi.port_index = 1;
    in.single = &midi.header;
    CHECK(instance.block(in, out) == 0);
    midi.port_index = 0;
    for (int byte : {1, 2}) {
        midi.data[byte] = 128;
        CHECK(instance.block(in, out) == 0);
        midi.data[byte] = byte == 1 ? 60 : 100;
    }
    instance.p->stop_processing(instance.p);
    instance.p->deactivate(instance.p);
}

TEST_CASE("the extension hook creates, forwards and destroys module data") {
    const int before = destroyed;
    {
        Instance in;
        CHECK(in.p->get_extension(in.p, "test.probe") == &kProbeExtension);
        CHECK(in.p->get_extension(in.p, "nope") == nullptr);
        CHECK(in.p->get_extension(in.p, CLAP_EXT_PARAMS) != nullptr);  // core ids first
        in.p->on_main_thread(in.p);
        CHECK(in.data->mainThreadCalls == 1);
    }
    CHECK(destroyed == before + 1);
}

TEST_CASE("an editor gesture while processing reaches the host and the engine") {
    Instance in;
    in.start();
    InEvents notes;
    notes.noteOn(57);
    OutEvents out;
    CHECK(in.block(notes, out) > 0.01);
    InEvents none;
    const std::size_t level = indexOf("output.level");
    const clap_id id = table()[level].id;
    in.bridge().beginEdit(level);
    in.bridge().edit(level, -30.0);
    in.bridge().edit(level, -60.0);
    in.bridge().endEdit(level);
    CHECK(in.bridge().value(level) == doctest::Approx(-60.0));  // the knob reads back at once
    CHECK(in.th.flushRequests >= 1);
    for (int i = 0; i < 20; ++i) in.block(none, out);  // the output gain glides
    REQUIRE(out.got.size() == 5);
    CHECK(out.got.front().type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
    CHECK(out.got.back().type == CLAP_EVENT_PARAM_GESTURE_END);
    for (const auto& e : out.got) CHECK(e.id == id);
    CHECK(out.got[3].value == doctest::Approx(params::toClap(table()[level], -60.0)));
    double loud = 0.0;
    OutEvents out2;
    InEvents more;
    more.noteOn(60);
    loud = in.block(more, out2);
    CHECK(loud < 0.01);  // -60 dB reached the engine
    CHECK(out2.got.empty());
}

TEST_CASE("edits while the host is not processing go out through params.flush") {
    Instance in;
    REQUIRE(in.p->activate(in.p, 48000.0, 16, 256));  // active, not processing
    auto filter = json::parse(R"({"filters":{"filter1":{"type":"ladder"}}})");
    REQUIRE(filter);
    REQUIRE(in.bridge().merge(*filter));
    const std::size_t cutoff = indexOf("filter1.cutoff");
    in.bridge().edit(cutoff, 2000.0);  // outside a gesture: sent as a whole gesture
    InEvents none;
    OutEvents out;
    in.params()->flush(in.p, &none.in, &out.out);
    REQUIRE(out.got.size() == 3);
    CHECK(out.got[1].type == CLAP_EVENT_PARAM_VALUE);
    double v = 0.0;
    REQUIRE(in.params()->get_value(in.p, table()[cutoff].id, &v));
    CHECK(params::fromClap(table()[cutoff], v) == doctest::Approx(2000.0));
}

TEST_CASE("a host that refuses events gets them again after a main-thread retry") {
    Instance in;
    in.start();
    const std::size_t level = indexOf("output.level");
    in.bridge().beginEdit(level);
    in.bridge().edit(level, -12.0);
    in.bridge().endEdit(level);
    InEvents none;
    OutEvents out;
    out.capacity = 2;
    in.block(none, out);
    CHECK(out.got.size() == 2);
    CHECK(in.th.callbacks == 1);
    const int flushes = in.th.flushRequests;
    in.p->on_main_thread(in.p);
    CHECK(in.th.flushRequests == flushes + 1);
    out.capacity = 100;
    in.block(none, out);
    REQUIRE(out.got.size() == 4);
    CHECK(out.got.back().type == CLAP_EVENT_PARAM_GESTURE_END);
}

TEST_CASE("structural merges go through the codec; revision and telemetry move") {
    Instance in;
    in.start();
    const std::uint64_t r0 = in.bridge().revision();
    CHECK_FALSE(in.bridge().structure().osc[1].has_value());
    REQUIRE(in.bridge().merge(json::parse(R"({"oscillators": {"osc2": {"type": "fm"}}})").value()));
    CHECK(in.bridge().structure().osc[1] == OscType::Fm);
    CHECK(in.bridge().revision() > r0);
    auto bad =
        in.bridge().merge(json::parse(R"({"oscillators": {"osc2": {"type": "piano"}}})").value());
    REQUIRE_FALSE(bad);
    CHECK(in.bridge().structure().osc[1] == OscType::Fm);  // unchanged on error
    CHECK(in.bridge().patchText(true).find("\"osc2\"") != std::string::npos);
    InEvents notes;
    notes.noteOn(57);
    OutEvents out;
    in.block(notes, out);
    CHECK(in.bridge().telemetry().peakL == 0.0f);  // off until an editor asks
    in.bridge().setTelemetryEnabled(true);
    InEvents none;
    in.block(none, out);
    const Telemetry t = in.bridge().telemetry();
    CHECK(t.peakL > 0.0f);
    CHECK(t.voices >= 1);
    CHECK(t.processing);
    CHECK(in.bridge().telemetry().peakL == 0.0f);  // peaks reset on read
}

TEST_CASE("CLAP note expressions validate payloads and match conjunctive wildcard addresses") {
    for (auto expression : {CLAP_NOTE_EXPRESSION_PRESSURE, CLAP_NOTE_EXPRESSION_BRIGHTNESS,
                            CLAP_NOTE_EXPRESSION_EXPRESSION}) {
        const std::string source = expression == CLAP_NOTE_EXPRESSION_PRESSURE     ? "pressure"
                                   : expression == CLAP_NOTE_EXPRESSION_BRIGHTNESS ? "slide"
                                                                                   : "expression";
        auto run = [&](int key, int channel, int id, std::uint32_t size, double value) {
            Instance instance;
            const auto patch = json::parse(std::string(R"({"oscillators":{"osc1":{"type":"va"}},
                "filters":{"filter1":{"type":"svf","cutoff":200}},"routes":{")") +
                                           source + R"(>filter1.cutoff":{"amount":"+5 oct"}}})");
            REQUIRE(patch);
            REQUIRE(instance.bridge().merge(*patch));
            instance.start();
            InEvents input;
            input.noteOn(60);
            input.notes[0].channel = 1;
            input.notes[0].note_id = 12;
            OutEvents output;
            instance.block(input, output);
            clap_event_note_expression_t event{};
            event.header = {size, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
            event.expression_id = expression;
            event.port_index = 0;
            event.key = static_cast<std::int16_t>(key);
            event.channel = static_cast<std::int16_t>(channel);
            event.note_id = id;
            event.value = value;
            input.single = &event.header;
            instance.block(input, output);
            input.single = nullptr;
            input.notes.clear();
            std::vector<float> audio;
            for (int n = 0; n < 8; ++n) {
                instance.block(input, output);
                audio.insert(audio.end(), instance.l.begin(), instance.l.end());
            }
            instance.p->stop_processing(instance.p);
            instance.p->deactivate(instance.p);
            return audio;
        };
        const auto size = static_cast<std::uint32_t>(sizeof(clap_event_note_expression_t));
        const auto dry = run(60, 1, 12, sizeof(clap_event_header_t), 1);
        const auto wet = run(60, 1, 12, size, 1);
        CHECK(bool(dry != wet));
        CHECK(bool(run(-1, -1, -1, size, 1) == wet));
        CHECK(bool(run(-1, 1, 12, size, 1) == wet));
        CHECK(bool(run(60, -1, 12, size, 1) == wet));
        CHECK(bool(run(60, 1, -1, size, 1) == wet));
        CHECK(bool(run(61, 1, 12, size, 1) == dry));
        CHECK(bool(run(60, 2, 12, size, 1) == dry));
        CHECK(bool(run(60, 1, 13, size, 1) == dry));
        CHECK(bool(run(60, 1, 12, size, std::numeric_limits<double>::quiet_NaN()) == dry));
        CHECK(bool(run(60, 1, 12, size, 2) == dry));
    }
}
