// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The VST3 a host sees, against the CLAP it was built from. clap-wrapper
// turns the CLAP into the VST3, and it decides alone how buses, MIDI and
// speaker layouts cross over; this loads both built modules and checks:
//
//   - the VST3 buses are the product's: the main bus, the sidechain as an
//     aux input, every aux output, note input and output;
//   - every declared layout can be chosen through setBusArrangements, with
//     the right speaker arrangement for surround and ambisonics;
//   - with note input, MIDI controllers and pitch bend are mapped (VST3
//     carries them as parameters) and reach the processor: the same notes,
//     pitch bend, mod wheel, automation and audio rendered through both
//     formats give the same output on every bus, and the same notes and
//     MIDI out;
//   - and so do 96 notes held at once -- three times clap-wrapper's stock
//     room for 32 -- across a stop and start of processing.
//
// Every VST3 process call is a real-time context (core/Realtime.h): under
// the rtsan preset, an allocation or lock anywhere in it -- the wrapper's
// own work around the CLAP call included -- fails the test.
//
// Linux only, like vst3-unload: it opens the modules with dlopen.
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include "core/ParamTable.h"
#include "core/Realtime.h"
#include "host_contract.h"
#include "Check.h"
#include "ProductInfo.h"

using namespace Steinberg;
namespace info = product::info;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// --- the host side of VST3's interfaces, as little as a host needs -------------------

#define SUPPLE_UNKNOWN(Interface)                                                         \
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {              \
        if (FUnknownPrivate::iidEqual(iid, Interface::iid) ||                             \
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {                              \
            *obj = this;                                                                  \
            return kResultOk;                                                             \
        }                                                                                 \
        *obj = nullptr;                                                                   \
        return kNoInterface;                                                              \
    }                                                                                     \
    uint32 PLUGIN_API addRef() override { return 1; }                                     \
    uint32 PLUGIN_API release() override { return 1; }

struct HostApplication final : Vst::IHostApplication {
    SUPPLE_UNKNOWN(Vst::IHostApplication)
    tresult PLUGIN_API getName(Vst::String128 name) override {
        const char* n = "vst3-host-tests";
        for (int i = 0; i < 128; ++i) {
            name[i] = static_cast<Vst::TChar>(n[i]);
            if (!n[i]) break;
        }
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
};

// The plugin adds to these from inside its process call, where nothing may
// allocate (the rtsan preset checks): room is made beforehand.
struct EventList final : Vst::IEventList {
    SUPPLE_UNKNOWN(Vst::IEventList)
    std::vector<Vst::Event> events;
    EventList() { events.reserve(1024); }
    int32 PLUGIN_API getEventCount() override { return static_cast<int32>(events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Vst::Event& e) override {
        if (index < 0 || index >= getEventCount()) return kResultFalse;
        e = events[static_cast<size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Vst::Event& e) override {
        if (events.size() == events.capacity()) return kResultFalse;
        events.push_back(e);
        return kResultOk;
    }
};

struct ValueQueue final : Vst::IParamValueQueue {
    SUPPLE_UNKNOWN(Vst::IParamValueQueue)
    Vst::ParamID id = 0;
    std::vector<std::pair<int32, Vst::ParamValue>> points;
    ValueQueue() { points.reserve(64); }
    Vst::ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, Vst::ParamValue& value) override {
        if (index < 0 || index >= getPointCount()) return kResultFalse;
        offset = points[static_cast<size_t>(index)].first;
        value = points[static_cast<size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, Vst::ParamValue value, int32& index) override {
        if (points.size() == points.capacity()) return kResultFalse;
        index = getPointCount();
        points.push_back({offset, value});
        return kResultOk;
    }
};

// Every queue exists beforehand: the plugin adds output parameter changes
// from inside its process call.
struct ParameterChanges final : Vst::IParameterChanges {
    SUPPLE_UNKNOWN(Vst::IParameterChanges)
    std::vector<ValueQueue> queues = std::vector<ValueQueue>(64);
    size_t used = 0;
    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(used); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < getParameterCount() ? &queues[static_cast<size_t>(index)] : nullptr;
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override {
        for (size_t k = 0; k < used; ++k)
            if (queues[k].id == id) {
                index = static_cast<int32>(k);
                return &queues[k];
            }
        if (used == queues.size()) return nullptr;
        index = static_cast<int32>(used);
        queues[used].id = id;
        return &queues[used++];
    }
};

#undef SUPPLE_UNKNOWN

// --- what each layout must look like in VST3 ------------------------------------------

Vst::SpeakerArrangement arrangement(const info::LayoutInfo& l) {
    using namespace Vst::SpeakerArr;
    switch (l.layout) {
        case supple::Layout::Mono: return kMono;
        case supple::Layout::Stereo: return kStereo;
        case supple::Layout::Surround51: return k51;
        case supple::Layout::Surround71: return k71Music;
        case supple::Layout::Surround714: return k71_4;
        case supple::Layout::Ambisonic1: return kAmbi1stOrderACN;
        case supple::Layout::Ambisonic2: return kAmbi2cdOrderACN;
        case supple::Layout::Ambisonic3: return kAmbi3rdOrderACN;
        default: return l.channels >= 64 ? ~Vst::SpeakerArrangement{0} : (Vst::SpeakerArrangement{1} << l.channels) - 1;
    }
}

const char* layoutName(const info::LayoutInfo& l) {
    const char* names[] = {"generic", "mono", "stereo", "5.1", "7.1", "7.1.4", "ambi1", "ambi2", "ambi3"};
    return names[static_cast<int>(l.layout)];
}

// --- the scenario, played identically through both formats ------------------------------

constexpr int kBlock = 256;
constexpr int kBlocks = 48;
constexpr int kNoteOnBlock = 2, kBendBlock = 10, kWheelBlock = 14, kParamBlock = 18, kBendBackBlock = 26,
              kNoteOffBlock = 34;
constexpr int kKey = 69, kNoteId = 5;
constexpr float kVelocity = 0.75f;

// The dense performance: 48 notes struck in one block and 48 more in
// another, every one held, processing stopped and started in between (which
// clap-wrapper turns into a reset), then all released together.
constexpr int kDenseBlocks = 20, kStrikeBlock = 2, kRestartBlock = 5, kStrikeAgainBlock = 8, kReleaseBlock = 12;
constexpr int kDenseNotes = 48;
struct DenseNote {
    bool on;
    int key, id, offset;
};
// This block's notes, in time order.
std::vector<DenseNote> denseNotes(int block) {
    std::vector<DenseNote> notes;
    for (int k = 0; k < kDenseNotes; ++k) {
        if (block == kStrikeBlock) notes.push_back({true, 30 + k, 100 + k, k * 5});
        if (block == kStrikeAgainBlock) notes.push_back({true, 30 + kDenseNotes + k, 200 + k, k * 5});
    }
    for (int k = 0; block == kReleaseBlock && k < 2 * kDenseNotes; ++k)
        notes.push_back({false, 30 + k, k < kDenseNotes ? 100 + k : 200 + k - kDenseNotes, k * 2});
    return notes;
}

// The parameter both formats automate: the one host-smoke uses.
int automatedParam() {
    for (size_t i = 0; i < supple::params::kCount; ++i)
        if (!std::strcmp(supple::params::at(i).name, VST3_HOST_PLAIN_PARAM)) return static_cast<int>(i);
    return -1;
}
constexpr double kAutomated = 0.25;  // normalized

float signal(int port, int channel, int frame) {
    if (port == 0) return 0.2f * std::sin(0.031f * static_cast<float>(frame) + static_cast<float>(channel));
    // A sidechain that comes and goes.
    return (frame / 2000) % 2 ? 0.0f : 0.6f * std::sin(0.007f * static_cast<float>(frame) + static_cast<float>(channel));
}

// One note or MIDI event sent, in a form both formats reduce to.
struct Sent {
    int frame;
    const char* kind;  // on, off, bend, cc
    int a, b;          // key and channel; bend value and channel; controller and value
    bool operator==(const Sent& o) const {
        return frame == o.frame && !std::strcmp(kind, o.kind) && a == o.a && b == o.b;
    }
};

std::string describe(const Sent& s) {
    char text[64];
    std::snprintf(text, sizeof(text), "%d:%s:%d:%d", s.frame, s.kind, s.a, s.b);
    return text;
}

// What came out: every output channel, then what was sent.
struct Result {
    bool ok = true;
    std::vector<float> audio;
    std::vector<Sent> sent;
    Result() { sent.reserve(4096); }  // CLAP fills it from inside process()
    Result(bool good) : Result() { ok = good; }
};

// --- VST3 --------------------------------------------------------------------------------

struct Vst3Module {
    void* handle = nullptr;
    IPluginFactory* factory = nullptr;
    bool (*exitModule)() = nullptr;
    TUID cid{};

    explicit Vst3Module(const char* path) {
        handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            std::printf("  dlopen: %s\n", dlerror());
            return;
        }
        auto entry = reinterpret_cast<bool (*)(void*)>(dlsym(handle, "ModuleEntry"));
        exitModule = reinterpret_cast<bool (*)()>(dlsym(handle, "ModuleExit"));
        auto get = reinterpret_cast<IPluginFactory* (*)()>(dlsym(handle, "GetPluginFactory"));
        if (!entry || !exitModule || !get || !entry(handle)) return;
        factory = get();
        for (int32 i = 0; factory && i < factory->countClasses(); ++i) {
            PClassInfo c{};
            if (factory->getClassInfo(i, &c) == kResultOk && !std::strcmp(c.category, kVstAudioEffectClass)) {
                std::memcpy(cid, c.cid, sizeof(TUID));
                return;
            }
        }
        factory = nullptr;
    }
    ~Vst3Module() {
        if (factory) factory->release();
        if (exitModule) exitModule();
        if (handle) dlclose(handle);
    }
};

struct Vst3Instance {
    HostApplication host;
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    Vst::IMidiMapping* midi = nullptr;

    explicit Vst3Instance(Vst3Module& m) {
        if (!m.factory || m.factory->createInstance(m.cid, Vst::IComponent::iid, reinterpret_cast<void**>(&component)) != kResultOk)
            return;
        if (component->initialize(&host) != kResultOk) return;
        component->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&processor));
        component->queryInterface(Vst::IEditController::iid, reinterpret_cast<void**>(&controller));
        component->queryInterface(Vst::IMidiMapping::iid, reinterpret_cast<void**>(&midi));
    }
    ~Vst3Instance() {
        if (midi) midi->release();
        if (controller) controller->release();
        if (processor) processor->release();
        if (component) {
            component->terminate();
            component->release();
        }
    }
    int32 buses(Vst::MediaType type, Vst::BusDirection dir) { return component->getBusCount(type, dir); }
    Vst::BusInfo bus(Vst::MediaType type, Vst::BusDirection dir, int32 index) {
        Vst::BusInfo b{};
        component->getBusInfo(type, dir, index, b);
        return b;
    }
    // Every audio bus in one arrangement.
    tresult arrange(Vst::SpeakerArrangement a) {
        std::vector<Vst::SpeakerArrangement> ins(static_cast<size_t>(buses(Vst::kAudio, Vst::kInput)), a),
            outs(static_cast<size_t>(buses(Vst::kAudio, Vst::kOutput)), a);
        return processor->setBusArrangements(ins.data(), static_cast<int32>(ins.size()), outs.data(),
                                             static_cast<int32>(outs.size()));
    }
};

std::string narrow(const Vst::String128 s) {
    std::string out;
    for (int i = 0; i < 128 && s[i]; ++i) out.push_back(static_cast<char>(s[i]));
    return out;
}

// The host's call into the VST3, as a real-time context.
tresult processRealtime(Vst::IAudioProcessor* processor, Vst::ProcessData& data) SUPPLE_REALTIME {
    return processor->process(data);
}

Result renderVst3(Vst3Module& m, const info::LayoutInfo& layout, bool dense = false) {
    Result r;
    Vst3Instance v(m);
    if (!v.processor) return Result(false);
    if (info::kAudioOutput && v.arrange(arrangement(layout)) != kResultTrue) return Result(false);
    Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, kBlock, 48000.0};
    if (v.processor->setupProcessing(setup) != kResultOk || v.component->setActive(true) != kResultOk)
        return Result(false);
    v.processor->setProcessing(true);

    // Buses: one storage per channel.
    const int32 ins = v.buses(Vst::kAudio, Vst::kInput), outs = v.buses(Vst::kAudio, Vst::kOutput);
    const size_t width = static_cast<size_t>(layout.channels);
    std::vector<std::vector<std::vector<float>>> in(static_cast<size_t>(ins), std::vector<std::vector<float>>(width, std::vector<float>(kBlock))),
        out(static_cast<size_t>(outs), std::vector<std::vector<float>>(width, std::vector<float>(kBlock)));
    std::vector<std::vector<float*>> inPtr(in.size()), outPtr(out.size());
    std::vector<Vst::AudioBusBuffers> inBus(in.size()), outBus(out.size());
    for (size_t b = 0; b < in.size(); ++b) {
        for (auto& c : in[b]) inPtr[b].push_back(c.data());
        inBus[b].numChannels = static_cast<int32>(width);
        inBus[b].channelBuffers32 = inPtr[b].data();
    }
    for (size_t b = 0; b < out.size(); ++b) {
        for (auto& c : out[b]) outPtr[b].push_back(c.data());
        outBus[b].numChannels = static_cast<int32>(width);
        outBus[b].channelBuffers32 = outPtr[b].data();
    }

    Vst::ParamID bendId = 0, wheelId = 0;
    const bool mapped = v.midi && v.midi->getMidiControllerAssignment(0, 0, Vst::kPitchBend, bendId) == kResultOk &&
                        v.midi->getMidiControllerAssignment(0, 0, 1, wheelId) == kResultOk;
    if (info::kNoteInput && !mapped) return Result(false);
    const int param = dense ? -1 : automatedParam();

    for (int block = 0; block < (dense ? kDenseBlocks : kBlocks); ++block) {
        if (dense && block == kRestartBlock) {
            v.processor->setProcessing(false);
            v.processor->setProcessing(true);
        }
        // The sidechain follows the main input among the inputs.
        for (size_t b = 0; b < in.size(); ++b)
            for (size_t c = 0; c < width; ++c)
                for (int i = 0; i < kBlock; ++i)
                    in[b][c][static_cast<size_t>(i)] =
                        signal(info::kAudioInput && b == 0 ? 0 : 1, static_cast<int>(c), block * kBlock + i);
        EventList events, sent;
        ParameterChanges changes, outChanges;
        auto note = [&](bool on) {
            Vst::Event e{};
            e.busIndex = 0;
            e.sampleOffset = 10;
            if (on) {
                e.type = Vst::Event::kNoteOnEvent;
                e.noteOn = {0, kKey, 0.0f, kVelocity, 0, kNoteId};
            } else {
                e.type = Vst::Event::kNoteOffEvent;
                e.noteOff = {0, kKey, kVelocity, kNoteId, 0.0f};
            }
            events.addEvent(e);
        };
        auto point = [&](Vst::ParamID id, double value) {
            int32 index = 0, at = 0;
            changes.addParameterData(id, index)->addPoint(0, value, at);
        };
        const bool performance = info::kNoteInput && !dense;
        if (performance && block == kNoteOnBlock) note(true);
        if (performance && block == kNoteOffBlock) note(false);
        if (performance && block == kBendBlock) point(bendId, 1.0);
        if (performance && block == kBendBackBlock) point(bendId, 0.0);
        if (performance && block == kWheelBlock) point(wheelId, 1.0);
        for (const DenseNote& n : dense ? denseNotes(block) : std::vector<DenseNote>{}) {
            Vst::Event e{};
            e.sampleOffset = n.offset;
            e.type = n.on ? Vst::Event::kNoteOnEvent : Vst::Event::kNoteOffEvent;
            if (n.on) e.noteOn = {0, static_cast<int16>(n.key), 0.0f, kVelocity, 0, n.id};
            else e.noteOff = {0, static_cast<int16>(n.key), kVelocity, n.id, 0.0f};
            events.addEvent(e);
        }
        if (param >= 0 && block == kParamBlock) point(supple::params::at(static_cast<size_t>(param)).id, kAutomated);

        Vst::ProcessData data{};
        data.processMode = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples = kBlock;
        data.numInputs = ins;
        data.numOutputs = outs;
        data.inputs = inBus.empty() ? nullptr : inBus.data();
        data.outputs = outBus.empty() ? nullptr : outBus.data();
        data.inputEvents = &events;
        data.outputEvents = &sent;
        data.inputParameterChanges = &changes;
        data.outputParameterChanges = &outChanges;
        if (processRealtime(v.processor, data) != kResultOk) r.ok = false;
        for (auto& bus : out)
            for (auto& c : bus) r.audio.insert(r.audio.end(), c.begin(), c.end());
        for (const Vst::Event& e : sent.events) {
            const int frame = block * kBlock + e.sampleOffset;
            if (e.type == Vst::Event::kNoteOnEvent) r.sent.push_back({frame, "on", e.noteOn.pitch, e.noteOn.channel});
            if (e.type == Vst::Event::kNoteOffEvent) r.sent.push_back({frame, "off", e.noteOff.pitch, e.noteOff.channel});
            if (e.type == Vst::Event::kLegacyMIDICCOutEvent) {
                const auto& cc = e.midiCCOut;
                if (cc.controlNumber == Vst::kPitchBend)
                    r.sent.push_back({frame, "bend", cc.value2 << 7 | cc.value, cc.channel});
                else if (cc.controlNumber < 128)
                    r.sent.push_back({frame, "cc", cc.controlNumber, cc.value});
            }
        }
    }
    v.processor->setProcessing(false);
    v.component->setActive(false);
    return r;
}

// --- CLAP --------------------------------------------------------------------------------

Result renderClap(const clap_plugin_factory_t* factory, const info::LayoutInfo& layout, bool dense = false) {
    Result r;
    host_contract::RenderInstance instance(factory, info::kId);
    const clap_plugin_t* plug = instance.plug;
    if (!plug || !host_contract::selectLayout(plug, static_cast<uint32_t>(layout.channels))) return Result(false);
    if (!plug->activate(plug, 48000.0, 1, kBlock)) return Result(false);
    instance.active = true;
    plug->start_processing(plug);
    instance.processing = true;

    host_contract::Buses<float> buses;
    buses.build(plug, kBlock, false);
    clap_process_t proc{};
    buses.bind(proc);
    proc.frames_count = kBlock;

    struct Out {
        Result* r;
        int block;
        static bool CLAP_ABI push(const clap_output_events_t* list, const clap_event_header_t* h) {
            auto* self = static_cast<Out*>(list->ctx);
            const int frame = self->block * kBlock + static_cast<int>(h->time);
            auto& sent = self->r->sent;
            if (sent.size() == sent.capacity()) return false;
            if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
                const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
                sent.push_back({frame, h->type == CLAP_EVENT_NOTE_ON ? "on" : "off", n->key, n->channel});
            } else if (h->type == CLAP_EVENT_MIDI) {
                const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                if ((m->data[0] & 0xF0) == 0xE0)
                    sent.push_back({frame, "bend", m->data[2] << 7 | m->data[1], m->data[0] & 0x0F});
                else if ((m->data[0] & 0xF0) == 0xB0)
                    sent.push_back({frame, "cc", m->data[1], m->data[2]});
            }
            return true;
        }
    } outEvents{&r, 0};
    clap_output_events_t outList{&outEvents, Out::push};
    const int param = dense ? -1 : automatedParam();
    std::vector<clap_event_note_t> notes;

    for (int block = 0; block < (dense ? kDenseBlocks : kBlocks); ++block) {
        if (dense && block == kRestartBlock) {  // what clap-wrapper does for setProcessing(false), then (true)
            plug->stop_processing(plug);
            plug->reset(plug);
            plug->start_processing(plug);
        }
        outEvents.block = block;
        for (size_t b = 0; b < buses.inPointers.size(); ++b)
            for (size_t c = 0; c < buses.inPointers[b].size(); ++c)
                for (int i = 0; i < kBlock; ++i)
                    buses.inPointers[b][c][i] =
                        signal(static_cast<int>(b) == buses.mainIn ? 0 : 1, static_cast<int>(c), block * kBlock + i);
        host_contract::InputEvents events;
        clap_event_note_t note{};
        clap_event_midi_t midi{};
        clap_event_param_value_t value{};
        auto midiAt = [&](uint8_t status, uint8_t a, uint8_t b) {
            midi.header = {sizeof(midi), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            midi.port_index = 0;
            midi.data[0] = status;
            midi.data[1] = a;
            midi.data[2] = b;
            events.events.push_back(&midi.header);
        };
        const bool performance = info::kNoteInput && !dense;
        if (performance && (block == kNoteOnBlock || block == kNoteOffBlock)) {
            note.header = {sizeof(note), 10, CLAP_CORE_EVENT_SPACE_ID,
                           static_cast<uint16_t>(block == kNoteOnBlock ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            note.note_id = kNoteId;
            note.port_index = 0;
            note.channel = 0;
            note.key = kKey;
            note.velocity = kVelocity;
            events.events.push_back(&note.header);
        }
        // What clap-wrapper makes of the VST3 values above: 16383, 0 and 127.
        if (performance && block == kBendBlock) midiAt(0xE0, 0x7F, 0x7F);
        if (performance && block == kBendBackBlock) midiAt(0xE0, 0, 0);
        if (performance && block == kWheelBlock) midiAt(0xB0, 1, 127);
        notes.clear();
        for (const DenseNote& n : dense ? denseNotes(block) : std::vector<DenseNote>{}) {
            clap_event_note_t e{};
            e.header = {sizeof(e), static_cast<uint32_t>(n.offset), CLAP_CORE_EVENT_SPACE_ID,
                        static_cast<uint16_t>(n.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e.note_id = n.id;
            e.port_index = 0;
            e.channel = 0;
            e.key = static_cast<int16_t>(n.key);
            e.velocity = kVelocity;
            notes.push_back(e);
        }
        for (const clap_event_note_t& e : notes) events.events.push_back(&e.header);
        if (param >= 0 && block == kParamBlock) {
            const supple::ParamDef& p = supple::params::at(static_cast<size_t>(param));
            value.header = {sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            value.param_id = p.id;
            value.note_id = -1;
            value.port_index = value.channel = value.key = -1;
            value.value = kAutomated * (p.hostMax() - p.hostMin()) + p.hostMin();
            events.events.push_back(&value.header);
        }
        proc.in_events = &events.input;
        proc.out_events = &outList;
        if (plug->process(plug, &proc) == CLAP_PROCESS_ERROR) r.ok = false;
        buses.forEachOutput([&](float* c) { r.audio.insert(r.audio.end(), c, c + kBlock); });
        instance.pump();
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: vst3-host-tests <Product.so in the .vst3> <Product.clap>\n");
        return 2;
    }
    std::printf("vst3-host-tests: %s\n", info::kName);
    Vst3Module vst3(argv[1]);
    check(vst3.factory != nullptr, "the VST3 module loads and has an audio effect class");
    void* clapModule = dlopen(argv[2], RTLD_LAZY | RTLD_LOCAL);
    auto* entry = clapModule ? static_cast<const clap_plugin_entry_t*>(dlsym(clapModule, "clap_entry")) : nullptr;
    check(entry && entry->init(argv[2]), "the CLAP module loads");
    if (!vst3.factory || !entry) return 1;
    const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));

    {
        Vst3Instance v(vst3);
        check(v.component && v.processor && v.controller, "it instantiates as component, processor and controller");
        if (!v.processor) return 1;

        // Buses.
        const int32 audioIns = (info::kAudioInput ? 1 : 0) + (info::kSidechain ? 1 : 0);
        const int32 audioOuts = info::kAudioOutput ? 1 + info::kAuxOutputs : 0;
        check(v.buses(Vst::kAudio, Vst::kInput) == audioIns && v.buses(Vst::kAudio, Vst::kOutput) == audioOuts,
              "audio buses: the main bus, the sidechain, each aux output");
        bool kinds = true;
        for (int32 b = 0; b < audioIns; ++b) {
            const Vst::BusInfo bus = v.bus(Vst::kAudio, Vst::kInput, b);
            const bool main = info::kAudioInput && b == 0;
            kinds = kinds && bus.busType == (main ? Vst::kMain : Vst::kAux) && (main || narrow(bus.name) == "Sidechain");
        }
        for (int32 b = 1; b < audioOuts; ++b)
            kinds = kinds && narrow(v.bus(Vst::kAudio, Vst::kOutput, b).name) == info::kAuxOutputNames[b - 1];
        check(kinds, "the sidechain is an aux input, and every bus has its name");
        check(v.buses(Vst::kEvent, Vst::kInput) == (info::kNoteInput ? 1 : 0) &&
                  v.buses(Vst::kEvent, Vst::kOutput) == (info::kNoteOutput ? 1 : 0),
              "event buses: notes in and out as declared");

        // Layouts, through the host's own call for them.
        bool chosen = true, speakers = true;
        for (const auto& l : info::kLayouts) {
            if (!info::kAudioOutput) break;
            const Vst::SpeakerArrangement want = arrangement(l);
            if (v.arrange(want) != kResultTrue) {
                std::printf("     %s: setBusArrangements refused\n", layoutName(l));
                chosen = false;
                continue;
            }
            Vst::SpeakerArrangement got = 0;
            v.processor->getBusArrangement(Vst::kOutput, 0, got);
            const bool named = l.layout != supple::Layout::Generic;
            if ((named && got != want) || Vst::SpeakerArr::getChannelCount(got) != l.channels) {
                std::printf("     %s: arrangement %llx, wanted %llx\n", layoutName(l),
                            static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
                speakers = false;
            }
            for (int32 b = 0; b < audioIns; ++b)
                speakers = speakers && v.bus(Vst::kAudio, Vst::kInput, b).channelCount == l.channels;
        }
        check(chosen, "every declared layout can be chosen");
        check(speakers, "each with its speaker arrangement on every bus");
        if (info::kAudioOutput) {
            Vst::SpeakerArrangement odd = Vst::SpeakerArr::kStereo | Vst::kSpeakerC;  // 3 channels, undeclared
            bool declared = false;
            for (const auto& l : info::kLayouts) declared |= l.channels == 3;
            if (!declared) check(v.arrange(odd) != kResultTrue, "an undeclared layout is refused");
        }

        if (info::kNoteInput) {
            Vst::ParamID id = 0;
            const bool bend = v.midi && v.midi->getMidiControllerAssignment(0, 0, Vst::kPitchBend, id) == kResultOk;
            const bool wheel = v.midi && v.midi->getMidiControllerAssignment(0, 15, 1, id) == kResultOk;
            const bool pressure = v.midi && v.midi->getMidiControllerAssignment(0, 3, Vst::kAfterTouch, id) == kResultOk;
            check(bend && wheel && pressure, "pitch bend, controllers and pressure are mapped on every channel");
        }
    }

    // The same performance through both formats, in every layout.
    for (const auto& l : info::kLayouts) {
        const Result a = renderVst3(vst3, l), b = renderClap(factory, l);
        char what[96];
        std::snprintf(what, sizeof(what), "%s: VST3 and CLAP render alike", info::kAudioOutput ? layoutName(l) : "notes");
        double worst = a.audio.size() == b.audio.size() ? 0.0 : INFINITY;
        for (size_t k = 0; k < a.audio.size() && k < b.audio.size(); ++k)
            worst = std::max(worst, supple::test::finiteDifference(a.audio[k], b.audio[k]));
        if (!(worst <= 1e-6)) std::printf("     largest difference %g\n", worst);
        check(a.ok && b.ok && worst <= 1e-6 && (!info::kAudioOutput || !a.audio.empty()), what);
        if (info::kNoteOutput) {
            if (a.sent != b.sent) {
                std::printf("     VST3 sent %zu, CLAP sent %zu:\n", a.sent.size(), b.sent.size());
                for (size_t k = 0; k < std::max(a.sent.size(), b.sent.size()); ++k)
                    std::printf("       %-24s %s\n", k < a.sent.size() ? describe(a.sent[k]).c_str() : "-",
                                k < b.sent.size() ? describe(b.sent[k]).c_str() : "-");
            }
            check(!a.sent.empty() && a.sent == b.sent, "and send the same notes and MIDI");
        }
    }

    // Many notes at once, across a stop and start.
    if (info::kNoteInput) {
        const info::LayoutInfo& l = info::kLayouts[0];
        const Result a = renderVst3(vst3, l, true), b = renderClap(factory, l, true);
        double worst = a.audio.size() == b.audio.size() ? 0.0 : INFINITY;
        for (size_t k = 0; k < a.audio.size() && k < b.audio.size(); ++k)
            worst = std::max(worst, supple::test::finiteDifference(a.audio[k], b.audio[k]));
        if (!(worst <= 1e-6)) std::printf("     largest difference %g\n", worst);
        check(a.ok && b.ok && worst <= 1e-6 && a.sent == b.sent,
              "96 notes held across a restart: VST3 and CLAP alike");
    } else {
        std::printf("      (no note input in this product)\n");
    }

    entry->deinit();
    dlclose(clapModule);
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
