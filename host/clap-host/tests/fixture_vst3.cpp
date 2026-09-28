// Original MIT test module for the VST3 host. Three classes exercise the
// effect, note and latency paths without any external installed plug-in.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

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
constexpr double kTau = 6.28318530717958647692;

class Fixture final : public SingleComponentEffect {
  public:
    enum Kind { Gain, Sine, Delay };
    explicit Fixture(Kind kind) : kind_(kind), value_(kind == Delay ? 0.0 : 0.5) {}
    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (SingleComponentEffect::initialize(context) != kResultOk) return kResultFalse;
        if (kind_ != Sine) addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        if (kind_ == Sine) addEventInput(STR16("Notes"));
        parameters.addParameter(kind_ == Gain    ? STR16("Gain")
                                : kind_ == Delay ? STR16("Latency")
                                                 : STR16("Level"),
                                nullptr, kind_ == Delay ? 1 : 0, kind_ == Delay ? 0 : 0.5,
                                ParameterInfo::kCanAutomate, 1);
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
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
                        : input[i] * static_cast<float>(kind_ == Gain ? value_.load() : 1.0);
            }
        }
        return kResultOk;
    }
    OBJ_METHODS(Fixture, SingleComponentEffect)
  private:
    Kind kind_;
    std::atomic<double> value_;
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
    int32 PLUGIN_API countClasses() override { return 3; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (!info || index < 0 || index >= 3) return kInvalidArgument;
        const FUID ids[] = {kGainId, kSineId, kDelayId};
        const char* names[] = {"Test VST3 Gain", "Test VST3 Sine", "Test VST3 Latency"};
        *info =
            PClassInfo(ids[index], PClassInfo::kManyInstances, kVstAudioEffectClass, names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (!info || index < 0 || index >= 3) return kInvalidArgument;
        const FUID ids[] = {kGainId, kSineId, kDelayId};
        const char* names[] = {"Test VST3 Gain", "Test VST3 Sine", "Test VST3 Latency"};
        *info = PClassInfo2(ids[index], PClassInfo::kManyInstances, kVstAudioEffectClass,
                            names[index], 0, index == 1 ? "Instrument" : "Fx", "Supple Audio",
                            "1.0", kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        const FUID ids[] = {kGainId, kSineId, kDelayId};
        for (int i = 0; i < 3; ++i) {
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

extern "C" bool ModuleEntry(void*) { return true; }
extern "C" bool ModuleExit() { return true; }
extern "C" IPluginFactory* GetPluginFactory() { return new Factory; }
