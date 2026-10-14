// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Original MIT test module for the VST3 host. Its processor classes exercise the
// effect, note, latency, sidechain, layout and state paths without any external
// installed plug-in. The sidechain gain behaves like strict real plug-ins: it refuses
// a bus layout that leaves out its sidechain, outputs silence unless given
// every bus, and answers setProcessing with kNotImplemented.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
const FUID kGainId(0x11000101, 0x22000202, 0x33000303, 0x44000404);
const FUID kSineId(0x11000102, 0x22000202, 0x33000303, 0x44000404);
const FUID kDelayId(0x11000103, 0x22000202, 0x33000303, 0x44000404);
const FUID kSidechainId(0x11000104, 0x22000202, 0x33000303, 0x44000404);
const FUID kSeparateId(0x11000105, 0x22000202, 0x33000303, 0x44000404);
const FUID kControllerId(0x11000106, 0x22000202, 0x33000303, 0x44000404);
const FUID kMultiOutId(0x11000107, 0x22000202, 0x33000303, 0x44000404);
const FUID kAdapterId(0x11000108, 0x22000202, 0x33000303, 0x44000404);
// A separate controller with no state of its own: like sfizz, it keeps the
// SDK's getState, which answers kNotImplemented.
const FUID kStatelessId(0x11000109, 0x22000202, 0x33000303, 0x44000404);
const FUID kStatelessControllerId(0x1100010A, 0x22000202, 0x33000303, 0x44000404);
constexpr double kTau = 6.28318530717958647692;

// The host must call the edit controller on the thread that initialized the
// plug-in (the SDK marks the parameter conversions [UI-thread & Connected]) and
// never from the audio thread while processing. This counts the violations so a
// test can assert there are none.
std::atomic<int> gControllerCallsOffThread{0};
std::atomic<int> gLayoutMode{0};

std::atomic<int> gRunLoopRemovedCalls{0}, gRunLoopTimerCalls{0}, gRunLoopFdCalls{0};
std::atomic<int> gNoteOffs{0}, gWildcardOffs{0};
// A view attached before the host asked its size (clap-wrapper's views size a
// resizable editor in attached() from the last size reported).
std::atomic<int> gAttachedUnsized{0};
std::array<std::atomic<int>, 64> gNoteIdOffs{};

#if defined(__linux__)
class TestHandler final
    : public U::Implements<U::Directly<Linux::ITimerHandler, Linux::IEventHandler>> {
  public:
    Linux::IRunLoop* loop = nullptr;
    TestHandler* other = nullptr;
    bool first = false;
    void PLUGIN_API onTimer() override {
        if (!first) {
            ++gRunLoopRemovedCalls;
            return;
        }
        loop->unregisterTimer(other);
        loop->unregisterTimer(this);
        ++gRunLoopTimerCalls;
    }
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) override {
        if (!first) {
            ++gRunLoopRemovedCalls;
            return;
        }
        loop->unregisterEventHandler(other);
        loop->unregisterEventHandler(this);
        ++gRunLoopFdCalls;
    }
};
class TestView final : public CPluginView {
  public:
    TestView() { rect = {0, 0, 300, 200}; }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0 ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getSize(ViewRect* size) override {
        sized_ = true;
        return CPluginView::getSize(size);
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (!sized_) ++gAttachedUnsized;
        return CPluginView::attached(parent, type);
    }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override {
        if (loop_) {
            loop_->unregisterTimer(&first_);
            loop_->unregisterTimer(&second_);
            loop_->unregisterEventHandler(&first_);
            loop_->unregisterEventHandler(&second_);
        }
        CPluginView::setFrame(frame);
        loop_ = FUnknownPtr<Linux::IRunLoop>(frame);
        if (loop_) {
            first_.loop = second_.loop = loop_;
            first_.first = true;
            first_.other = &second_;
            loop_->registerEventHandler(&first_, 123);
            loop_->registerEventHandler(&second_, 123);
            loop_->registerTimer(&first_, 1);
            loop_->registerTimer(&second_, 1);
        }
        return kResultOk;
    }

  private:
    bool sized_ = false;
    FUnknownPtr<Linux::IRunLoop> loop_;
    TestHandler first_, second_;
};

#endif

class Fixture final : public SingleComponentEffect {
  public:
    // MultiOut (ADR-0021): an instrument with a main output (0.125) and an
    // auxiliary "Kick" output bus (0.25), constant levels. Adapter: a gain that
    // offers stereo but refuses it at setBusArrangements and settles on mono.
    enum Kind { Gain, Sine, Delay, Sidechain, Separate, MultiOut, Adapter, Stateless };
    explicit Fixture(Kind kind, bool controller = false)
        : controller_(controller), kind_(kind), value_(kind == Delay ? 0.0 : 0.5) {}
    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (SingleComponentEffect::initialize(context) != kResultOk) return kResultFalse;
        owner_ = std::this_thread::get_id();
        if (kind_ != Sine && kind_ != MultiOut) addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        if (kind_ == Sidechain)
            addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, BusTypes::kAux, 0);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        if (kind_ == MultiOut)
            addAudioOutput(STR16("Kick"), SpeakerArr::kStereo, BusTypes::kAux, 0);
        if (kind_ == Sine) addEventInput(STR16("Notes"));
        parameters.addParameter(kind_ == Gain || kind_ == Sidechain ? STR16("Gain")
                                : kind_ == Delay                    ? STR16("Latency")
                                                                    : STR16("Level"),
                                nullptr, kind_ == Delay ? 1 : 0, kind_ == Delay ? 0 : 0.5,
                                ParameterInfo::kCanAutomate, 1);
        return kResultOk;
    }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        return type == kAudio && gLayoutMode.load() == 2
                   ? 129
                   : SingleComponentEffect::getBusCount(type, dir);
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index,
                                  BusInfo& bus) override {
        auto result = SingleComponentEffect::getBusInfo(type, dir, index, bus);
        if (result == kResultOk && type == kAudio && gLayoutMode.load() == 1) bus.channelCount = 65;
        return result;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
#if defined(__linux__)
        return controller_ && std::strcmp(name, ViewType::kEditor) == 0 ? new TestView : nullptr;
#else
        (void)name;
        return nullptr;
#endif
    }
    tresult PLUGIN_API getControllerClassId(TUID id) override {
        if (kind_ != Separate && kind_ != Stateless) return kResultFalse;
        (kind_ == Stateless ? kStatelessControllerId : kControllerId).toTUID(id);
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        processing_.store(state != 0, std::memory_order_relaxed);
        return kind_ == Sidechain ? kNotImplemented : kResultOk;
    }
    // Watched conversions: a call from another thread while processing is a
    // host bug (the count is read by the test).
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue value) override {
        watchControllerCall();
        return EditControllerEx1::normalizedParamToPlain(id, value);
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue value) override {
        watchControllerCall();
        return EditControllerEx1::plainParamToNormalized(id, value);
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs, int32 numOuts) override {
        if (kind_ == Sidechain && (numIns != 2 || numOuts != 1)) return kResultFalse;
        if (kind_ == Adapter) {
            for (auto* list : {&audioInputs, &audioOutputs})
                for (auto& bus : *list)
                    static_cast<AudioBus*>(bus.get())->setArrangement(SpeakerArr::kMono);
            return kResultFalse;
        }
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }
    tresult PLUGIN_API setState(IBStream* stream) override {
        int32 n = 0;
        double value = 0;
        if (stream->read(&value, sizeof(value), &n) != kResultOk || n != sizeof(value))
            return kResultFalse;
        value_ = std::clamp(value, 0.0, 1.0);
        parameters.getParameter(1)->setNormalized(value_);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* stream) override {
        if (controller_ && kind_ == Stateless) return kNotImplemented;
        int32 n = 0;
        double value = value_.load();
        return stream->write(&value, sizeof(value), &n) == kResultOk && n == sizeof(value)
                   ? kResultOk
                   : kResultFalse;
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) override { return setState(stream); }
    tresult PLUGIN_API getEditorState(IBStream* stream) override { return getState(stream); }
    tresult PLUGIN_API setEditorState(IBStream* stream) override { return setState(stream); }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        if (EditControllerEx1::setParamNormalized(id, value) != kResultOk) return kResultFalse;
        value_ = value;
        if (kind_ == Delay && componentHandler) componentHandler->restartComponent(kLatencyChanged);
        return kResultOk;
    }
    uint32 PLUGIN_API getLatencySamples() override {
        return kind_ == Delay && value_ >= 0.5 ? 64 : 0;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.inputEvents)
            for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
                Event event{};
                data.inputEvents->getEvent(i, event);
                if (event.type == Event::kNoteOffEvent) {
                    ++gNoteOffs;
                    if (event.noteOff.channel < 0 || event.noteOff.pitch < 0) ++gWildcardOffs;
                    if (event.noteOff.noteId >= 0 && event.noteOff.noteId < 64)
                        ++gNoteIdOffs[event.noteOff.noteId];
                }
            }
        if (data.numSamples == 0) {
            if (data.inputParameterChanges) {
                for (int32 q = 0; q < data.inputParameterChanges->getParameterCount(); ++q) {
                    auto* changes = data.inputParameterChanges->getParameterData(q);
                    for (int32 point = 0; point < changes->getPointCount(); ++point) {
                        int32 offset = 0;
                        double value = 0;
                        changes->getPoint(point, offset, value);
                        value_ = value;
                    }
                }
            }
            return kResultOk;
        }
        if (kind_ == MultiOut) {
            for (int32 bus = 0; bus < data.numOutputs; ++bus)
                for (int32 c = 0; c < data.outputs[bus].numChannels; ++c)
                    std::fill_n(data.outputs[bus].channelBuffers32[c], data.numSamples,
                                bus == 0 ? 0.125f : 0.25f);
            return kResultOk;
        }
        if (kind_ == Sidechain && data.numInputs != 2) {
            for (int32 c = 0; c < data.outputs[0].numChannels; ++c)
                std::fill_n(data.outputs[0].channelBuffers32[c], data.numSamples, 0.0f);
            return kResultOk;
        }
        auto* queue = data.inputParameterChanges && data.inputParameterChanges->getParameterCount()
                          ? data.inputParameterChanges->getParameterData(0)
                          : nullptr;
        int32 nextPoint = 0, nextNote = 0;
        for (int32 i = 0; i < data.numSamples; ++i) {
            while (queue && nextPoint < queue->getPointCount()) {
                int32 offset = 0;
                double value = 0;
                if (queue->getPoint(nextPoint, offset, value) != kResultOk || offset > i) break;
                value_ = value;
                if (data.outputParameterChanges) {
                    int32 queueIndex = 0, pointIndex = 0;
                    auto* output = data.outputParameterChanges->addParameterData(
                        queue->getParameterId(), queueIndex);
                    if (output) output->addPoint(offset, value, pointIndex);
                }
                ++nextPoint;
            }
            while (kind_ == Sine && data.inputEvents &&
                   nextNote < data.inputEvents->getEventCount()) {
                Event event{};
                if (data.inputEvents->getEvent(nextNote, event) != kResultOk ||
                    event.sampleOffset > i)
                    break;
                if (event.type == Event::kNoteOnEvent && event.noteOn.pitch >= 0 &&
                    event.noteOn.pitch < 128)
                    voices_[event.noteOn.pitch] = true;
                if (event.type == Event::kNoteOffEvent && event.noteOff.pitch >= 0 &&
                    event.noteOff.pitch < 128)
                    voices_[event.noteOff.pitch] = false;
                ++nextNote;
            }
            double sample = 0;
            if (kind_ == Sine) {
                for (int note = 0; note < 128; ++note) {
                    if (!voices_[note]) continue;
                    const double hz = 440.0 * std::exp2((note - 69) / 12.0);
                    sample += std::sin(kTau * phase_[note]) * value_;
                    phase_[note] += hz / processSetup.sampleRate;
                    if (phase_[note] >= 1) phase_[note] -= 1;
                }
            }
            for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
                auto* output = data.outputs[0].channelBuffers32[channel];
                const auto* input =
                    kind_ == Sine ? nullptr : data.inputs[0].channelBuffers32[channel];
                output[i] =
                    kind_ == Sine
                        ? static_cast<float>(sample)
                        : input[i] * static_cast<float>(kind_ == Delay ? 1.0 : value_.load()) +
                              (kind_ == Sidechain && data.inputs[1].silenceFlags == 0
                                   ? data.inputs[1].channelBuffers32[channel][i]
                                   : 0.0f);
            }
        }
        return kResultOk;
    }
    OBJ_METHODS(Fixture, SingleComponentEffect)
  private:
    void watchControllerCall() {
        if (processing_.load(std::memory_order_relaxed) && std::this_thread::get_id() != owner_)
            gControllerCallsOffThread.fetch_add(1, std::memory_order_relaxed);
    }
    bool controller_ = false;
    Kind kind_;
    std::atomic<double> value_;
    std::atomic<bool> processing_{false};
    std::thread::id owner_{};
    std::array<bool, 128> voices_{};
    std::array<double, 128> phase_{};
};

class Factory final
    : public U::Implements<U::Directly<IPluginFactory2>, U::Indirectly<IPluginFactory>> {
  public:
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kInvalidArgument;
        *info = PFactoryInfo("Supple Audio", "https://supple.audio", "dev@supple.audio", 0);
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 10; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (!info || index < 0 || index >= 10) return kInvalidArgument;
        const FUID ids[] = {
            kGainId,     kSineId,       kDelayId,   kSidechainId, kSeparateId,
            kMultiOutId, kControllerId, kAdapterId, kStatelessId, kStatelessControllerId};
        const char* names[] = {"Test VST3 Gain",
                               "Test VST3 Sine",
                               "Test VST3 Latency",
                               "Test VST3 Sidechain Gain",
                               "Test VST3 Separate Gain",
                               "Test VST3 Multi-Out",
                               "Test VST3 Gain Controller",
                               "Test VST3 Mono Adapter",
                               "Test VST3 Stateless Controller Gain",
                               "Test VST3 Stateless Controller"};
        *info = PClassInfo(
            ids[index], PClassInfo::kManyInstances,
            index == 6 || index == 9 ? kVstComponentControllerClass : kVstAudioEffectClass,
            names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (!info || index < 0 || index >= 10) return kInvalidArgument;
        const FUID ids[] = {
            kGainId,     kSineId,       kDelayId,   kSidechainId, kSeparateId,
            kMultiOutId, kControllerId, kAdapterId, kStatelessId, kStatelessControllerId};
        const char* names[] = {"Test VST3 Gain",
                               "Test VST3 Sine",
                               "Test VST3 Latency",
                               "Test VST3 Sidechain Gain",
                               "Test VST3 Separate Gain",
                               "Test VST3 Multi-Out",
                               "Test VST3 Gain Controller",
                               "Test VST3 Mono Adapter",
                               "Test VST3 Stateless Controller Gain",
                               "Test VST3 Stateless Controller"};
        *info = PClassInfo2(
            ids[index], PClassInfo::kManyInstances,
            index == 6 || index == 9 ? kVstComponentControllerClass : kVstAudioEffectClass,
            names[index], 0, index == 1 || index == 5 ? "Instrument" : "Fx", "Supple Audio", "1.0",
            kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        const FUID ids[] = {
            kGainId,     kSineId,       kDelayId,   kSidechainId, kSeparateId,
            kMultiOutId, kControllerId, kAdapterId, kStatelessId, kStatelessControllerId};
        for (int i = 0; i < 6; ++i) {
            if (std::memcmp(cid, ids[i], 16) != 0) continue;
            auto* plugin = new Fixture(static_cast<Fixture::Kind>(i));
            const auto result = plugin->queryInterface(iid, obj);
            plugin->release();
            return result;
        }
        if (std::memcmp(cid, kAdapterId, 16) == 0) {
            auto* plugin = new Fixture(Fixture::Adapter);
            const auto result = plugin->queryInterface(iid, obj);
            plugin->release();
            return result;
        }
        if (std::memcmp(cid, kStatelessId, 16) == 0) {
            auto* plugin = new Fixture(Fixture::Stateless);
            const auto result = plugin->queryInterface(iid, obj);
            plugin->release();
            return result;
        }
        if (std::memcmp(cid, kControllerId, 16) == 0 ||
            std::memcmp(cid, kStatelessControllerId, 16) == 0) {
            const bool stateless = std::memcmp(cid, kStatelessControllerId, 16) == 0;
            auto* controller = new Fixture(stateless ? Fixture::Stateless : Fixture::Gain, true);
            const auto result = controller->queryInterface(iid, obj);
            controller->release();
            return result;
        }
        return kNoInterface;
    }
};
}  // namespace

// The controller-call watch, for the host tests (the module is dlopen'd).
extern "C" int fixtureOffThreadControllerCalls() { return gControllerCallsOffThread.load(); }
extern "C" void fixtureResetOffThreadControllerCalls() { gControllerCallsOffThread.store(0); }

extern "C" bool ModuleEntry(void*) { return true; }
extern "C" bool ModuleExit() { return true; }
extern "C" IPluginFactory* GetPluginFactory() { return new Factory; }

extern "C" int fixtureRemovedHandlerCalls() { return gRunLoopRemovedCalls.load(); }
extern "C" int fixtureTimerCalls() { return gRunLoopTimerCalls.load(); }
extern "C" int fixtureAttachedUnsized() { return gAttachedUnsized.load(); }
extern "C" int fixtureFdCalls() { return gRunLoopFdCalls.load(); }
extern "C" int fixtureNoteOffs(int id) {
    return id == -1 ? gNoteOffs.load() : id == -2 ? gWildcardOffs.load() : gNoteIdOffs[id].load();
}

extern "C" void fixtureLayoutMode(int mode) { gLayoutMode = mode; }

#if defined(__APPLE__)
extern "C" bool bundleEntry(void*) { return true; }
extern "C" bool bundleExit() { return true; }
#elif defined(_WIN32)
extern "C" __declspec(dllexport) bool InitDll() { return true; }
extern "C" __declspec(dllexport) bool ExitDll() { return true; }
#endif
