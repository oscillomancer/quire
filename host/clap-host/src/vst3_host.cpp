#include "vst3_host.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "supple/base/spsc.h"
#include "supple/json/json.h"

namespace supple::host {
namespace fs = std::filesystem;
namespace vst = Steinberg::Vst;
using Steinberg::FUnknownPtr;
using Steinberg::IPtr;
using Steinberg::kResultOk;

namespace {
constexpr std::size_t kMaxEvents = 2048;
constexpr std::size_t kMaxParams = 256;
struct Change {
    std::uint32_t id;
    double value;
};

// The SDK's ParameterChanges grows vectors during addPoint. These bounded
// queues allocate once at construction, before any audio-thread call.
class FixedQueue : public Steinberg::U::Implements<Steinberg::U::Directly<vst::IParamValueQueue>> {
  public:
    FixedQueue() { points.reserve(kMaxEvents); }
    void clear(vst::ParamID id) {
        id_ = id;
        points.clear();
    }
    vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API getPointCount() override {
        return static_cast<Steinberg::int32>(points.size());
    }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 i, Steinberg::int32& offset,
                                           vst::ParamValue& value) override {
        if (i < 0 || static_cast<std::size_t>(i) >= points.size()) return Steinberg::kResultFalse;
        offset = points[i].first;
        value = points[i].second;
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 offset, vst::ParamValue value,
                                           Steinberg::int32& index) override {
        if (points.size() == points.capacity()) return Steinberg::kResultFalse;
        index = static_cast<Steinberg::int32>(points.size());
        points.emplace_back(offset, value);
        return kResultOk;
    }

  private:
    vst::ParamID id_ = vst::kNoParamId;
    std::vector<std::pair<Steinberg::int32, vst::ParamValue>> points;
};

class FixedChanges
    : public Steinberg::U::Implements<Steinberg::U::Directly<vst::IParameterChanges>> {
  public:
    void prepare(std::size_t count) {
        queues_.reserve(std::min(count, kMaxParams));
        for (std::size_t i = 0; i < std::min(count, kMaxParams); ++i)
            queues_.push_back(Steinberg::owned(new FixedQueue));
    }
    void clear() { used_ = 0; }
    Steinberg::int32 PLUGIN_API getParameterCount() override { return used_; }
    vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 i) override {
        return i >= 0 && i < used_ ? queues_[i].get() : nullptr;
    }
    vst::IParamValueQueue* PLUGIN_API addParameterData(const vst::ParamID& id,
                                                       Steinberg::int32& index) override {
        for (Steinberg::int32 i = 0; i < used_; ++i)
            if (queues_[i]->getParameterId() == id) {
                index = i;
                return queues_[i].get();
            }
        if (used_ == static_cast<Steinberg::int32>(queues_.size())) return nullptr;
        index = used_++;
        queues_[index]->clear(id);
        return queues_[index].get();
    }

  private:
    std::vector<IPtr<FixedQueue>> queues_;
    Steinberg::int32 used_ = 0;
};

class Handler : public Steinberg::U::Implements<Steinberg::U::Directly<vst::IComponentHandler>> {
  public:
    std::atomic<Steinberg::int32> restart{0};
    Steinberg::tresult PLUGIN_API beginEdit(vst::ParamID) override { return kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit(vst::ParamID, vst::ParamValue) override {
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(vst::ParamID) override { return kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        restart.fetch_or(flags, std::memory_order_relaxed);
        return kResultOk;
    }
};

// A module stays loaded for the life of the process once opened (scan or
// instance), and is never unloaded, not even at exit: modules can bind to
// each other's GNU-unique symbols (two plugins built with the same fmt), so
// unloading one leaves the other calling unmapped code — a crash at exit
// seen with two locally built plugins. Hosts commonly keep VST3 modules
// loaded for the same reason.
std::shared_ptr<VST3::Hosting::Module> moduleFor(const fs::path& path) {
    static auto& mutex = *new std::mutex;
    static auto& cache = *new std::map<std::string, std::shared_ptr<VST3::Hosting::Module>>;
    std::lock_guard lock(mutex);
    const auto key = path.lexically_normal().string();
    if (auto found = cache.find(key); found != cache.end()) return found->second;
    std::string error;
    auto module = VST3::Hosting::Module::create(key, error);
    if (module) cache[key] = module;
    return module;
}

// The host context plugins get in initialize(): our name, and IMessage /
// IAttributeList creation for component-controller messages.
class HostContext : public vst::HostApplication {
  public:
    explicit HostContext(std::string name) : name_(std::move(name)) {}
    Steinberg::tresult PLUGIN_API getName(vst::String128 name) override {
        return vst::StringConvert::convert(name_, name) ? Steinberg::kResultTrue
                                                        : Steinberg::kInternalError;
    }

  private:
    std::string name_;
};

bool isVst3(const fs::path& p) { return p.extension() == ".vst3"; }
std::string classId(const VST3::Hosting::ClassInfo& info) { return "vst3:" + info.ID().toString(); }

// Steinberg's moduleinfo writer emits JSON5 comments and trailing commas.
// Normalize those two forms before handing the metadata to core-json.
std::string moduleJson(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool quoted = false, escaped = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (quoted) {
            out.push_back(c);
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            out.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i += 2;
            while (i < input.size() && input[i] != '\n') ++i;
            out.push_back('\n');
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
            ++i;
            out.push_back(' ');
            continue;
        }
        if (c == ',') {
            std::size_t next = i + 1;
            while (next < input.size() && std::isspace(static_cast<unsigned char>(input[next])))
                ++next;
            if (next < input.size() && (input[next] == '}' || input[next] == ']')) continue;
        }
        out.push_back(c);
    }
    return out;
}

std::optional<std::vector<PluginInfo>> scanModuleInfo(const fs::path& path) {
    std::ifstream file(path / "Contents/Resources/moduleinfo.json");
    if (!file) return std::nullopt;
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    auto parsed = json::parse(moduleJson(source));
    if (!parsed || !parsed->isObject()) return std::nullopt;
    const auto* classes = parsed->find("Classes");
    if (!classes || !classes->isArray()) return std::nullopt;
    std::string factoryVendor;
    if (const auto* factory = parsed->find("Factory Info"); factory && factory->isObject()) {
        if (const auto* vendor = factory->find("Vendor"); vendor && vendor->isString())
            factoryVendor = vendor->asString();
    }
    std::vector<PluginInfo> found;
    for (const auto& cls : classes->asArray()) {
        if (!cls.isObject()) return std::nullopt;
        const auto* category = cls.find("Category");
        if (!category || !category->isString() || category->asString() != "Audio Module Class")
            continue;
        const auto* cid = cls.find("CID");
        const auto* name = cls.find("Name");
        if (!cid || !cid->isString() || !name || !name->isString()) return std::nullopt;
        auto uid = VST3::UID::fromString(cid->asString());
        if (!uid) return std::nullopt;
        PluginInfo info;
        info.format = "vst3";
        info.id = "vst3:" + uid->toString();
        info.path = path;
        info.name = name->asString();
        info.vendor = factoryVendor;
        if (const auto* vendor = cls.find("Vendor"); vendor && vendor->isString())
            info.vendor = vendor->asString();
        if (const auto* version = cls.find("Version"); version && version->isString())
            info.version = version->asString();
        if (const auto* categories = cls.find("Sub Categories");
            categories && categories->isArray()) {
            for (const auto& value : categories->asArray()) {
                if (!value.isString()) return std::nullopt;
                info.features.push_back(value.asString());
            }
        }
        info.instrument = std::find(info.features.begin(), info.features.end(), "Instrument") !=
                          info.features.end();
        found.push_back(std::move(info));
    }
    return found;
}

std::vector<std::uint8_t> encodeState(const std::vector<std::uint8_t>& component,
                                      const std::vector<std::uint8_t>& controller) {
    std::vector<std::uint8_t> out;
    out.reserve(16 + component.size() + controller.size());
    auto u32 = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    u32(0x33545356);
    u32(1);
    u32(static_cast<std::uint32_t>(component.size()));
    u32(static_cast<std::uint32_t>(controller.size()));
    out.insert(out.end(), component.begin(), component.end());
    out.insert(out.end(), controller.begin(), controller.end());
    return out;
}
std::uint32_t read32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) |
           (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}
}  // namespace

struct Vst3Instance::State {
    std::shared_ptr<VST3::Hosting::Module> module;
    // Not reference counted (the SDK's HostApplication ignores addRef/release):
    // owned here and declared before the plug-in objects, so it outlives them.
    std::unique_ptr<HostContext> context;
    IPtr<vst::IComponent> component;
    FUnknownPtr<vst::IAudioProcessor> processor;
    IPtr<vst::IEditController> controller;
    IPtr<Handler> handler = Steinberg::owned(new Handler);
    FUnknownPtr<vst::IConnectionPoint> componentPoint, controllerPoint;  // when connected
    std::vector<ParamInfo> parameters;
    std::uint32_t inputs = 0, outputs = 0;  // channels of the main buses
    // Every audio bus gets buffers (the VST3 contract): the main buses carry
    // the track's audio, auxiliary ones (sidechains, extra outputs) silence
    // and scratch space, preallocated in activate().
    std::vector<std::uint32_t> inBusChannels, outBusChannels;
    std::vector<vst::AudioBusBuffers> inBuses, outBuses;
    std::vector<std::vector<float>> auxStorage;
    std::vector<float*> auxPointers;
    std::vector<float**> silentIn;  // each auxiliary input bus's silent channels
    int keyBus = -1;                // the first auxiliary input bus (the key), ADR-0018
    bool keyActive = false;
    std::vector<float*> keyPointers;  // the block's key, padded with silence
    bool notes = false, active = false;
    bool separateController = false;
    std::array<std::array<vst::ParamID, 130>, 16> midiParams{};
    double rate = 0;
    FixedChanges inputChanges, outputChanges;
    vst::EventList inputEvents{static_cast<Steinberg::int32>(kMaxEvents)};
    vst::EventList outputEvents{static_cast<Steinberg::int32>(kMaxEvents)};
    SpscRing<Change, 1024> changes;
    std::atomic<std::uint32_t> dropped{0};
    std::atomic<bool> latencyChanged{false};
};

Vst3Instance::Vst3Instance() : state_(std::make_unique<State>()) {}
Vst3Instance::~Vst3Instance() {
    auto& s = *state_;
    if (s.active) deactivate();
    if (s.componentPoint && s.controllerPoint) {
        s.componentPoint->disconnect(s.controllerPoint);
        s.controllerPoint->disconnect(s.componentPoint);
    }
    if (s.controller) {
        s.controller->setComponentHandler(nullptr);
        if (s.separateController) s.controller->terminate();
    }
    if (s.component) s.component->terminate();
}

Result<std::unique_ptr<Vst3Instance>> Vst3Instance::create(const PluginInfo& info,
                                                           const HostIdentity& host) {
    auto module = moduleFor(info.path);
    if (!module) return Error("host.load", "cannot load VST3 module " + info.path.string());
    if (info.id.rfind("vst3:", 0) != 0 || info.id.size() != 37)
        return Error("host.create", "invalid VST3 class id " + info.id);
    auto id = VST3::UID::fromString(info.id.substr(5));
    if (!id) return Error("host.create", "invalid VST3 class id " + info.id);
    std::unique_ptr<Vst3Instance> self(new Vst3Instance);
    auto& s = *self->state_;
    s.module = std::move(module);
    s.context = std::make_unique<HostContext>(host.name);
    s.component = s.module->getFactory().createInstance<vst::IComponent>(*id);
    if (!s.component || s.component->initialize(s.context.get()) != kResultOk)
        return Error("host.create", "VST3 component could not initialize");
    s.processor = FUnknownPtr<vst::IAudioProcessor>(s.component);
    if (!s.processor) return Error("host.create", "VST3 class has no audio processor");
    Steinberg::TUID controllerId{};
    if (s.component->getControllerClassId(controllerId) == kResultOk)
        s.controller =
            s.module->getFactory().createInstance<vst::IEditController>(VST3::UID(controllerId));
    s.separateController = static_cast<bool>(s.controller);
    if (!s.controller) s.controller = FUnknownPtr<vst::IEditController>(s.component);
    if (s.separateController) {
        if (s.controller->initialize(s.context.get()) != kResultOk)
            return Error("host.create", "VST3 controller could not initialize");
        // The SDK's host sequence: connect the two halves, then give the
        // controller the component's state (its parameter values).
        s.componentPoint = FUnknownPtr<vst::IConnectionPoint>(s.component);
        s.controllerPoint = FUnknownPtr<vst::IConnectionPoint>(s.controller);
        if (s.componentPoint && s.controllerPoint) {
            s.componentPoint->connect(s.controllerPoint);
            s.controllerPoint->connect(s.componentPoint);
        } else {
            s.componentPoint = nullptr;
            s.controllerPoint = nullptr;
        }
        Steinberg::ResizableMemoryIBStream state;
        if (s.component->getState(&state) == kResultOk) {
            state.rewind();
            s.controller->setComponentState(&state);
        }
    }
    if (s.controller) s.controller->setComponentHandler(s.handler);
    for (auto& channel : s.midiParams) channel.fill(vst::kNoParamId);
    if (s.controller) {
        FUnknownPtr<vst::IMidiMapping> mapping(s.controller);
        if (mapping) {
            for (Steinberg::int16 channel = 0; channel < 16; ++channel)
                for (Steinberg::int16 control = 0; control < 130; ++control) {
                    vst::ParamID target = vst::kNoParamId;
                    if (mapping->getMidiControllerAssignment(0, channel, control, target) ==
                        kResultOk)
                        s.midiParams[channel][control] = target;
                }
        }
    }
    for (auto [dir, channels] :
         {std::pair{vst::kInput, &s.inBusChannels}, std::pair{vst::kOutput, &s.outBusChannels}}) {
        const auto count = s.component->getBusCount(vst::kAudio, dir);
        for (Steinberg::int32 b = 0; b < count; ++b) {
            vst::BusInfo bus{};
            const bool ok = s.component->getBusInfo(vst::kAudio, dir, b, bus) == kResultOk;
            channels->push_back(ok ? static_cast<std::uint32_t>(std::max(bus.channelCount, 0)) : 0);
            if (ok && dir == vst::kInput && b > 0 && bus.busType == vst::kAux && s.keyBus < 0 &&
                bus.channelCount > 0)
                s.keyBus = b;
        }
    }
    s.inputs = s.inBusChannels.empty() ? 0 : std::min<std::uint32_t>(s.inBusChannels[0], 2);
    s.outputs = s.outBusChannels.empty() ? 0 : std::min<std::uint32_t>(s.outBusChannels[0], 2);
    s.notes = s.component->getBusCount(vst::kEvent, vst::kInput) > 0;
    if (s.controller) {
        const auto count = s.controller->getParameterCount();
        for (Steinberg::int32 i = 0; i < count; ++i) {
            vst::ParameterInfo p{};
            if (s.controller->getParameterInfo(i, p) != kResultOk) continue;
            std::uint32_t flags = 0;
            if (p.stepCount > 0) flags |= CLAP_PARAM_IS_STEPPED;
            if (p.flags & vst::ParameterInfo::kIsList) flags |= CLAP_PARAM_IS_ENUM;
            if (p.flags & vst::ParameterInfo::kIsReadOnly) flags |= CLAP_PARAM_IS_READONLY;
            if (p.flags & vst::ParameterInfo::kIsHidden) flags |= CLAP_PARAM_IS_HIDDEN;
            if (p.flags & vst::ParameterInfo::kIsBypass) flags |= CLAP_PARAM_IS_BYPASS;
            s.parameters.push_back(
                {p.id, vst::StringConvert::convert(p.title), "",
                 s.controller->normalizedParamToPlain(p.id, 0),
                 s.controller->normalizedParamToPlain(p.id, 1),
                 s.controller->normalizedParamToPlain(p.id, p.defaultNormalizedValue), flags});
        }
    }
    s.inputChanges.prepare(std::max<std::size_t>(1, s.parameters.size()));
    s.outputChanges.prepare(std::max<std::size_t>(1, s.parameters.size()));
    return self;
}

std::uint32_t Vst3Instance::inputChannels() const { return state_->inputs; }
std::uint32_t Vst3Instance::outputChannels() const { return state_->outputs; }
bool Vst3Instance::acceptsNotes() const { return state_->notes; }
std::uint32_t Vst3Instance::sidechainChannels() const {
    const auto& s = *state_;
    return s.keyBus < 0 ? 0 : s.inBusChannels[static_cast<std::size_t>(s.keyBus)];
}

Status Vst3Instance::activate(double rate, std::uint32_t maxFrames, bool sidechainActive) {
    auto& s = *state_;
    if (s.active) return okStatus();
    // One arrangement per bus: the main buses stereo or mono, auxiliary buses
    // as the plugin has them (a partial list is refused, and the plugin then
    // outputs silence). Only the main buses are activated.
    auto arrangements = [&](vst::BusDirection dir, const std::vector<std::uint32_t>& channels,
                            std::uint32_t main) {
        std::vector<vst::SpeakerArrangement> out;
        for (std::size_t b = 0; b < channels.size(); ++b) {
            vst::SpeakerArrangement a =
                channels[b] >= 2 ? vst::SpeakerArr::kStereo : vst::SpeakerArr::kMono;
            if (b == 0)
                a = main == 2 ? vst::SpeakerArr::kStereo : vst::SpeakerArr::kMono;
            else
                s.processor->getBusArrangement(dir, static_cast<Steinberg::int32>(b), a);
            out.push_back(a);
        }
        return out;
    };
    auto ins = arrangements(vst::kInput, s.inBusChannels, s.inputs);
    auto outs = arrangements(vst::kOutput, s.outBusChannels, s.outputs);
    s.processor->setBusArrangements(ins.data(), static_cast<Steinberg::int32>(ins.size()),
                                    outs.data(), static_cast<Steinberg::int32>(outs.size()));
    // What the plugin settled on (it may adapt a refused layout).
    for (auto [dir, channels] :
         {std::pair{vst::kInput, &s.inBusChannels}, std::pair{vst::kOutput, &s.outBusChannels}})
        for (std::size_t b = 0; b < channels->size(); ++b) {
            vst::SpeakerArrangement a = 0;
            if (s.processor->getBusArrangement(dir, static_cast<Steinberg::int32>(b), a) ==
                kResultOk)
                (*channels)[b] = static_cast<std::uint32_t>(vst::SpeakerArr::getChannelCount(a));
        }
    if (s.inputs) s.component->activateBus(vst::kAudio, vst::kInput, 0, true);
    if (s.outputs) s.component->activateBus(vst::kAudio, vst::kOutput, 0, true);
    s.inBuses.assign(s.inBusChannels.size(), {});
    s.outBuses.assign(s.outBusChannels.size(), {});
    s.auxStorage.clear();
    s.auxPointers.clear();
    std::size_t auxChannels = 0;
    for (auto* channels : {&s.inBusChannels, &s.outBusChannels})
        for (std::size_t b = 1; b < channels->size(); ++b) auxChannels += (*channels)[b];
    s.auxPointers.reserve(auxChannels);
    for (auto [channels, buses] :
         {std::pair{&s.inBusChannels, &s.inBuses}, std::pair{&s.outBusChannels, &s.outBuses}})
        for (std::size_t b = 1; b < channels->size(); ++b) {
            (*buses)[b].numChannels = static_cast<Steinberg::int32>((*channels)[b]);
            (*buses)[b].channelBuffers32 = s.auxPointers.data() + s.auxPointers.size();
            for (std::uint32_t c = 0; c < (*channels)[b]; ++c) {
                s.auxStorage.emplace_back(maxFrames, 0.0f);
                s.auxPointers.push_back(s.auxStorage.back().data());
            }
        }
    s.silentIn.assign(s.inBuses.size(), nullptr);
    for (std::size_t b = 1; b < s.inBuses.size(); ++b)
        s.silentIn[b] = s.inBuses[b].channelBuffers32;
    // The key bus only while a route feeds it: some plug-ins take an active
    // auxiliary bus as "key from it" and would otherwise key from silence.
    s.keyActive = sidechainActive && s.keyBus >= 0;
    if (s.keyBus >= 0) s.component->activateBus(vst::kAudio, vst::kInput, s.keyBus, s.keyActive);
    s.keyPointers.assign(s.keyBus >= 0 ? s.inBusChannels[static_cast<std::size_t>(s.keyBus)] : 0,
                         nullptr);
    if (s.notes) s.component->activateBus(vst::kEvent, vst::kInput, 0, true);
    vst::ProcessSetup setup{vst::kRealtime, vst::kSample32,
                            static_cast<Steinberg::int32>(maxFrames), rate};
    if (s.processor->setupProcessing(setup) != kResultOk ||
        s.component->setActive(true) != kResultOk)
        return Error("host.activate", "VST3 component refused processing setup");
    s.active = true;
    s.rate = rate;
    return okStatus();
}
void Vst3Instance::deactivate() {
    state_->component->setActive(false);
    state_->active = false;
}
std::uint32_t Vst3Instance::latency() const {
    return state_->active ? state_->processor->getLatencySamples() : 0;
}
std::uint32_t Vst3Instance::tail() const {
    const auto samples = state_->processor->getTailSamples();
    return samples == vst::kInfiniteTail ? UINT32_MAX : samples;
}
std::vector<ParamInfo> Vst3Instance::params() const { return state_->parameters; }

Result<double> Vst3Instance::paramValue(std::uint32_t id) const {
    if (!state_->controller) return Error("host.param", "VST3 has no controller");
    for (const auto& p : state_->parameters)
        if (p.id == id)
            return state_->controller->normalizedParamToPlain(
                id, state_->controller->getParamNormalized(id));
    return Error("host.param", "no parameter with id " + std::to_string(id));
}
Result<std::string> Vst3Instance::paramText(std::uint32_t id, double plain) const {
    if (!state_->controller) return Error("host.param", "VST3 has no controller");
    vst::String128 text{};
    const double normalized = state_->controller->plainParamToNormalized(id, plain);
    if (state_->controller->getParamStringByValue(id, normalized, text) != kResultOk)
        return Error("host.param", "VST3 has no text for parameter " + std::to_string(id));
    return vst::StringConvert::convert(text);
}
Result<double> Vst3Instance::paramFromText(std::uint32_t id, std::string_view text) const {
    if (!state_->controller) return Error("host.param_text", "VST3 has no controller");
    vst::String128 utf16{};
    if (!vst::StringConvert::convert(std::string(text), utf16))
        return Error("host.param_text", "invalid parameter text");
    vst::ParamValue normalized = 0;
    if (state_->controller->getParamValueByString(id, utf16, normalized) != kResultOk)
        return Error("host.param_text", "VST3 could not read parameter text");
    return state_->controller->normalizedParamToPlain(id, normalized);
}
Status Vst3Instance::setParam(std::uint32_t id, double plain) {
    auto& s = *state_;
    const auto found = std::find_if(s.parameters.begin(), s.parameters.end(),
                                    [&](const ParamInfo& p) { return p.id == id; });
    if (found == s.parameters.end())
        return Error("host.param", "no parameter with id " + std::to_string(id));
    if (!std::isfinite(plain) || plain < found->min || plain > found->max)
        return Error("host.param", "parameter value is outside its range");
    const double normalized = s.controller->plainParamToNormalized(id, plain);
    s.controller->setParamNormalized(id, normalized);
    if (!s.changes.push({id, plain}))
        return Error("host.param_queue", "too many queued parameter changes");
    return okStatus();
}

Result<std::vector<std::uint8_t>> Vst3Instance::saveState() {
    Steinberg::ResizableMemoryIBStream component, controller;
    if (state_->component->getState(&component) != kResultOk)
        return Error("host.state", "VST3 component could not save state");
    if (state_->separateController && state_->controller->getState(&controller) != kResultOk)
        return Error("host.state", "VST3 controller could not save state");
    return encodeState(component.take(), controller.take());
}
Status Vst3Instance::loadState(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 16 || read32(bytes, 0) != 0x33545356 || read32(bytes, 4) != 1)
        return Error("host.state", "invalid VST3 state header");
    const auto c = read32(bytes, 8), e = read32(bytes, 12);
    if (std::uint64_t(c) + e != bytes.size() - 16)
        return Error("host.state", "invalid VST3 state lengths");
    if (c > static_cast<std::uint32_t>(std::numeric_limits<Steinberg::int32>::max()) ||
        e > static_cast<std::uint32_t>(std::numeric_limits<Steinberg::int32>::max()))
        return Error("host.state", "VST3 state is too large");
    Steinberg::ResizableMemoryIBStream component, controller;
    component.write(const_cast<std::uint8_t*>(bytes.data() + 16), static_cast<Steinberg::int32>(c),
                    nullptr);
    component.rewind();
    if (state_->component->setState(&component) != kResultOk)
        return Error("host.state", "VST3 component rejected state");
    if (state_->separateController) {
        component.rewind();
        if (state_->controller->setComponentState(&component) != kResultOk)
            return Error("host.state", "VST3 controller rejected component state");
        if (e) {
            controller.write(const_cast<std::uint8_t*>(bytes.data() + 16 + c),
                             static_cast<Steinberg::int32>(e), nullptr);
            controller.rewind();
            if (state_->controller->setState(&controller) != kResultOk)
                return Error("host.state", "VST3 controller rejected state");
        }
    }
    Change obsolete{};
    while (state_->changes.pop(obsolete)) {
    }
    return okStatus();
}

bool Vst3Instance::startProcessing() SUPPLE_RT {
    // kNotImplemented means "nothing to do" (the SDK documents it as OK; sfizz returns it).
    const auto result = state_->processor->setProcessing(true);
    return result == kResultOk || result == Steinberg::kNotImplemented;
}
void Vst3Instance::stopProcessing() SUPPLE_RT { state_->processor->setProcessing(false); }
void Vst3Instance::reset() SUPPLE_RT {}
void Vst3Instance::flushParams() SUPPLE_RT {}

bool Vst3Instance::process(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT {
    auto& s = *state_;
    s.inputChanges.clear();
    s.outputChanges.clear();
    s.inputEvents.clear();
    s.outputEvents.clear();
    auto addNormalized = [&](std::uint32_t id, double value, std::uint32_t time) {
        Steinberg::int32 queueIndex = 0;
        auto* q = s.inputChanges.addParameterData(id, queueIndex);
        if (!q) {
            s.dropped.fetch_add(1);
            return;
        }
        Steinberg::int32 index = 0;
        if (q->addPoint(static_cast<Steinberg::int32>(time), value, index) != kResultOk)
            s.dropped.fetch_add(1);
    };
    auto addParam = [&](std::uint32_t id, double plain, std::uint32_t time) {
        if (s.controller) addNormalized(id, s.controller->plainParamToNormalized(id, plain), time);
    };
    Change change{};
    while (s.changes.pop(change)) addParam(change.id, change.value, 0);
    for (const auto& event : block.events) {
        const auto time = std::min(event.time, block.frames ? block.frames - 1 : 0u);
        if (event.kind == Event::Kind::Param) {
            addParam(event.paramId, event.value, time);
            continue;
        }
        if (event.kind == Event::Kind::Midi) {
            const auto channel = event.midi[0] & 15;
            const auto type = event.midi[0] & 0xf0;
            int control = -1;
            double normalized = 0;
            if (type == 0xb0) {
                control = event.midi[1];
                normalized = event.midi[2] / 127.0;
            } else if (type == 0xe0) {
                control = vst::kPitchBend;
                normalized = (event.midi[1] | (event.midi[2] << 7)) / 16383.0;
            } else if (type == 0xd0) {
                control = vst::kAfterTouch;
                normalized = event.midi[1] / 127.0;
            }
            if (control >= 0) {
                const auto target = s.midiParams[channel][static_cast<std::size_t>(control)];
                if (target != vst::kNoParamId) addNormalized(target, normalized, time);
            }
            continue;
        }
        vst::Event e{};
        e.busIndex = 0;
        e.sampleOffset = static_cast<Steinberg::int32>(time);
        e.type = event.kind == Event::Kind::NoteOn ? vst::Event::kNoteOnEvent
                                                   : vst::Event::kNoteOffEvent;
        if (e.type == vst::Event::kNoteOnEvent)
            e.noteOn = {event.channel, event.key, 0, static_cast<float>(event.velocity), 0,
                        event.noteId};
        else
            e.noteOff = {event.channel, event.key, static_cast<float>(event.velocity), event.noteId,
                         0};
        if (s.inputEvents.addEvent(e) != kResultOk) s.dropped.fetch_add(1);
    }
    if (!s.inBuses.empty()) {
        s.inBuses[0].numChannels = static_cast<Steinberg::int32>(block.inputChannels);
        s.inBuses[0].channelBuffers32 = const_cast<float**>(block.inputs);
        s.inBuses[0].silenceFlags = 0;
    }
    if (!s.outBuses.empty()) {
        s.outBuses[0].numChannels = static_cast<Steinberg::int32>(block.outputChannels);
        s.outBuses[0].channelBuffers32 = const_cast<float**>(block.outputs);
    }
    for (std::size_t b = 1; b < s.inBuses.size(); ++b) {  // silent auxiliary inputs
        s.inBuses[b].channelBuffers32 = s.silentIn[b];
        s.inBuses[b].silenceFlags = ~0ull;
    }
    if (s.keyActive && block.sidechain && block.sidechainChannels > 0) {
        auto& key = s.inBuses[static_cast<std::size_t>(s.keyBus)];
        for (std::size_t c = 0; c < s.keyPointers.size(); ++c)
            s.keyPointers[c] = c < block.sidechainChannels ? const_cast<float*>(block.sidechain[c])
                                                           : key.channelBuffers32[c];
        key.channelBuffers32 = s.keyPointers.data();
        key.silenceFlags = 0;
    }
    vst::ProcessContext context{};
    context.sampleRate = s.rate;
    if (block.transport) {
        const auto& t = *block.transport;
        context.state = vst::ProcessContext::kTempoValid | vst::ProcessContext::kTimeSigValid |
                        vst::ProcessContext::kProjectTimeMusicValid |
                        vst::ProcessContext::kBarPositionValid;
        if (t.playing) context.state |= vst::ProcessContext::kPlaying;
        if (t.loopActive)
            context.state |= vst::ProcessContext::kCycleActive | vst::ProcessContext::kCycleValid;
        context.tempo = t.tempo;
        context.timeSigNumerator = t.numerator;
        context.timeSigDenominator = t.denominator;
        context.projectTimeMusic = t.beat;
        context.barPositionMusic = t.barStartBeat;
        context.cycleStartMusic = t.loopStartBeat;
        context.cycleEndMusic = t.loopEndBeat;
        context.projectTimeSamples = static_cast<Steinberg::int64>(t.seconds * s.rate);
    } else
        context.projectTimeSamples = block.steadyTime;
    vst::ProcessData data;
    data.processMode = vst::kRealtime;
    data.numSamples = static_cast<Steinberg::int32>(block.frames);
    data.numInputs = static_cast<Steinberg::int32>(s.inBuses.size());
    data.numOutputs = static_cast<Steinberg::int32>(s.outBuses.size());
    data.inputs = s.inBuses.empty() ? nullptr : s.inBuses.data();
    data.outputs = s.outBuses.empty() ? nullptr : s.outBuses.data();
    data.inputParameterChanges = &s.inputChanges;
    data.outputParameterChanges = &s.outputChanges;
    data.inputEvents = &s.inputEvents;
    data.outputEvents = &s.outputEvents;
    data.processContext = &context;
    const bool ok = s.processor->process(data) == kResultOk;
    if (out) {
        for (Steinberg::int32 i = 0; i < s.outputChanges.getParameterCount(); ++i) {
            auto* q = s.outputChanges.getParameterData(i);
            for (Steinberg::int32 j = 0; j < q->getPointCount(); ++j) {
                Steinberg::int32 offset;
                double value;
                if (q->getPoint(j, offset, value) != kResultOk) continue;
                if (out->count == out->storage.size()) {
                    ++out->dropped;
                    continue;
                }
                out->storage[out->count++] = {
                    OutputEvent::Kind::ParamValue, static_cast<std::uint32_t>(offset),
                    q->getParameterId(),
                    s.controller ? s.controller->normalizedParamToPlain(q->getParameterId(), value)
                                 : value,
                    -1};
            }
        }
    }
    return ok;
}

bool Vst3Instance::serviceMainThread() {
    const auto flags = state_->handler->restart.exchange(0);
    if (flags & vst::kLatencyChanged) state_->latencyChanged = true;
    return flags != 0;
}
Instance::Notifications Vst3Instance::takeNotifications() {
    Instance::Notifications n;
    n.latencyChanged = state_->latencyChanged.exchange(false);
    n.droppedEvents = state_->dropped.exchange(0);
    return n;
}

Result<std::vector<PluginInfo>> scanVst3(std::span<const fs::path> paths) {
    std::vector<fs::path> bundles;
    for (const auto& root : paths) {
        std::error_code ec;
        if (isVst3(root) && fs::is_directory(root, ec)) {
            bundles.push_back(root);
            continue;
        }
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec) && isVst3(it->path())) {
                bundles.push_back(it->path());
                it.disable_recursion_pending();
            }
        }
    }
    std::sort(bundles.begin(), bundles.end());
    std::vector<PluginInfo> result;
    for (const auto& path : bundles) {
        if (auto listed = scanModuleInfo(path)) {
            result.insert(result.end(), std::make_move_iterator(listed->begin()),
                          std::make_move_iterator(listed->end()));
            continue;
        }
        auto module = moduleFor(path);
        if (!module) continue;
        auto factory = module->getFactory();
        const auto vendor = factory.info().vendor();
        for (const auto& cls : factory.classInfos()) {
            if (cls.category() != "Audio Module Class") continue;
            PluginInfo info;
            info.format = "vst3";
            info.id = classId(cls);
            info.path = path;
            info.name = cls.name();
            info.vendor = cls.vendor().empty() ? vendor : cls.vendor();
            info.version = cls.version();
            info.features = cls.subCategories();
            info.instrument = std::find(info.features.begin(), info.features.end(), "Instrument") !=
                              info.features.end();
            result.push_back(std::move(info));
        }
    }
    return result;
}

std::vector<fs::path> defaultVst3SearchPath() {
    std::vector<fs::path> paths;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        if (fs::exists(p, ec)) paths.push_back(p);
    };
    if (const char* env = std::getenv("VST3_PATH")) {
        std::string text(env);
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find(':', start);
            if (end > start) add(text.substr(start, end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (const char* home = std::getenv("HOME")) add(fs::path(home) / ".vst3");
    add("/usr/lib/vst3");
    add("/usr/local/lib/vst3");
    return paths;
}
}  // namespace supple::host
