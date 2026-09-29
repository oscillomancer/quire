// Original MIT test module for the VST3 host. Four classes exercise the
// effect, note, latency and sidechain paths without any external installed
// plug-in. The sidechain gain behaves like strict real plug-ins: it refuses
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
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
const FUID kGainId(0x11000101, 0x22000202, 0x33000303, 0x44000404);
const FUID kSineId(0x11000102, 0x22000202, 0x33000303, 0x44000404);
const FUID kDelayId(0x11000103, 0x22000202, 0x33000303, 0x44000404);
const FUID kSidechainId(0x11000104, 0x22000202, 0x33000303, 0x44000404);
constexpr double kTau = 6.28318530717958647692;

// The host must call the edit controller on the thread that initialized the
// plug-in (the SDK marks the parameter conversions [UI-thread & Connected]) and
// never from the audio thread while processing. This counts the violations so a
// test can assert there are none.
std::atomic<int> gControllerCallsOffThread{0};

class Fixture final : public SingleComponentEffect {
  public:
    enum Kind { Gain, Sine, Delay, Sidechain };
    explicit Fixture(Kind kind) : kind_(kind), value_(kind == Delay ? 0.0 : 0.5) {}
    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (SingleComponentEffect::initialize(context) != kResultOk) return kResultFalse;
        owner_ = std::this_thread::get_id();
        if (kind_ != Sine) addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        if (kind_ == Sidechain)
            addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, BusTypes::kAux, 0);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        if (kind_ == Sine) addEventInput(STR16("Notes"));
        parameters.addParameter(kind_ == Gain || kind_ == Sidechain ? STR16("Gain")
                                : kind_ == Delay                    ? STR16("Latency")
                                                                    : STR16("Level"),
                                nullptr, kind_ == Delay ? 1 : 0, kind_ == Delay ? 0 : 0.5,
                                ParameterInfo::kCanAutomate, 1);
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
        int32 n = 0;
        double value = value_.load();
        return stream->write(&value, sizeof(value), &n) == kResultOk && n == sizeof(value)
                   ? kResultOk
                   : kResultFalse;
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) override { return setState(stream); }
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
                        : input[i] * static_cast<float>(kind_ == Delay ? 1.0 : value_.load());
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
    int32 PLUGIN_API countClasses() override { return 4; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (!info || index < 0 || index >= 4) return kInvalidArgument;
        const FUID ids[] = {kGainId, kSineId, kDelayId, kSidechainId};
        const char* names[] = {"Test VST3 Gain", "Test VST3 Sine", "Test VST3 Latency",
                               "Test VST3 Sidechain Gain"};
        *info =
            PClassInfo(ids[index], PClassInfo::kManyInstances, kVstAudioEffectClass, names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (!info || index < 0 || index >= 4) return kInvalidArgument;
        const FUID ids[] = {kGainId, kSineId, kDelayId, kSidechainId};
        const char* names[] = {"Test VST3 Gain", "Test VST3 Sine", "Test VST3 Latency",
                               "Test VST3 Sidechain Gain"};
        *info = PClassInfo2(ids[index], PClassInfo::kManyInstances, kVstAudioEffectClass,
                            names[index], 0, index == 1 ? "Instrument" : "Fx", "Supple Audio",
                            "1.0", kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        const FUID ids[] = {kGainId, kSineId, kDelayId, kSidechainId};
        for (int i = 0; i < 4; ++i) {
            if (std::memcmp(cid, ids[i], 16) != 0) continue;
            auto* plugin = new Fixture(static_cast<Fixture::Kind>(i));
            const auto result = plugin->queryInterface(iid, obj);
            plugin->release();
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
