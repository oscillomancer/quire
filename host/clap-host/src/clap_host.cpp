#include "supple/host/clap_host.h"

#include "bridge_client.h"
#include "vst3_host.h"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "supple/clap/semantic_params.h"
#include "supple/json/json.h"

namespace supple::host {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Library: one dlopen'ed .clap binary, shared by every instance created from it.
// ---------------------------------------------------------------------------

class Library {
  public:
    static Result<std::shared_ptr<Library>> open(const fs::path& path) {
        static std::mutex mutex;
        static std::map<std::string, std::weak_ptr<Library>> cache;
        std::lock_guard lock(mutex);
        const std::string key = fs::weakly_canonical(path).string();
        if (auto existing = cache[key].lock()) return existing;

        void* handle = ::dlopen(key.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            return Error("host.load", "cannot load " + key + ": " + std::string(::dlerror()), {},
                         "check that the file is a CLAP plugin built for this platform");
        auto* entry = static_cast<const clap_plugin_entry_t*>(::dlsym(handle, "clap_entry"));
        if (!entry) {
            ::dlclose(handle);
            return Error("host.load", key + " has no clap_entry symbol", {},
                         "this is not a CLAP plugin");
        }
        if (!clap_version_is_compatible(entry->clap_version)) {
            ::dlclose(handle);
            return Error("host.load", key + " uses an incompatible CLAP version");
        }
        if (!entry->init(key.c_str())) {
            ::dlclose(handle);
            return Error("host.load", key + ": clap_entry.init failed");
        }
        auto* factory =
            static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory) {
            entry->deinit();
            ::dlclose(handle);
            return Error("host.load", key + " provides no plugin factory");
        }
        auto lib = std::shared_ptr<Library>(new Library(handle, entry, factory, key));
        cache[key] = lib;
        return lib;
    }

    ~Library() {
        entry_->deinit();
        ::dlclose(handle_);
    }

    const clap_plugin_factory_t* factory() const { return factory_; }
    const std::string& path() const { return path_; }

  private:
    Library(void* h, const clap_plugin_entry_t* e, const clap_plugin_factory_t* f, std::string p)
        : handle_(h), entry_(e), factory_(f), path_(std::move(p)) {}
    void* handle_;
    const clap_plugin_entry_t* entry_;
    const clap_plugin_factory_t* factory_;
    std::string path_;
};

// ---------------------------------------------------------------------------
// Host state and callbacks
// ---------------------------------------------------------------------------

namespace {

thread_local bool tAudioThread = false;

struct AudioThreadScope {
    bool previous;
    AudioThreadScope() SUPPLE_RT : previous(tAudioThread) { tAudioThread = true; }
    ~AudioThreadScope() SUPPLE_RT { tAudioThread = previous; }
};

union ClapEvent {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_param_value_t param;
    clap_event_midi_t midi;
};

struct LogLine {
    char text[240];
};

struct ParamChange {
    std::uint32_t id;
    double value;
};

constexpr std::size_t kMaxInputEvents = 8192;

}  // namespace

struct Instance::HostState {
    Instance* owner = nullptr;
    clap_host_t host{};
    std::string name, vendor, url, version;
    std::thread::id mainThread = std::this_thread::get_id();

    std::atomic<bool> callbackRequested{false}, restartRequested{false}, processRequested{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> stateDirty{false}, latencyChanged{false}, semanticChanged{false};
    std::atomic<bool> tailChanged{false};
    std::atomic<std::uint32_t> droppedEvents{0};  // input events that did not fit the arena
    std::atomic<std::uint32_t> rescanFlags{0};
    std::atomic<bool> rescanned{false};

    struct Timer {
        clap_id id;
        std::uint32_t periodMs;
        std::chrono::steady_clock::time_point next;
    };
    std::vector<Timer> timers;  // main thread only
    clap_id nextTimer = 1;

    SpscRing<LogLine, 256> audioLog;  // plugin audio thread -> main
    std::mutex logMutex;              // any other thread
    std::vector<std::string> log;

    SpscRing<ParamChange, 1024> paramQueue;  // main -> audio

    // Event conversion arena, sized at activation (never resized on the audio thread).
    std::vector<ClapEvent> events;
    std::uint32_t eventCount = 0;
    OutputEvents* outSink = nullptr;
    clap_input_events_t inEvents{};
    clap_output_events_t outEvents{};
    clap_event_transport_t transport{};
    bool preferMidiNotes = false;
};

struct HostCallbacks {
    static Instance::HostState* S(const clap_host_t* h) {
        return static_cast<Instance::HostState*>(h->host_data);
    }

    // clap_host
    static const void* CLAP_ABI getExtension(const clap_host_t*, const char* id);
    static void CLAP_ABI requestRestart(const clap_host_t* h) { S(h)->restartRequested = true; }
    static void CLAP_ABI requestProcess(const clap_host_t* h) { S(h)->processRequested = true; }
    static void CLAP_ABI requestCallback(const clap_host_t* h) { S(h)->callbackRequested = true; }

    // clap.log
    static void CLAP_ABI log(const clap_host_t* h, clap_log_severity severity, const char* msg) {
        static constexpr const char* kSev[] = {
            "debug", "info", "warning", "error", "fatal", "host-misbehaving", "plugin-misbehaving"};
        const char* sev = severity >= 0 && severity <= 6 ? kSev[severity] : "log";
        if (tAudioThread) {
            // No formatting library calls on the audio thread: copy by hand.
            LogLine line{};
            std::size_t n = 0;
            auto put = [&](const char* text) {
                for (; text && *text && n + 1 < sizeof(line.text); ++text) line.text[n++] = *text;
            };
            put("[");
            put(sev);
            put("] ");
            put(msg);
            line.text[n] = 0;
            S(h)->audioLog.push(line);  // dropped when full
            return;
        }
        std::lock_guard lock(S(h)->logMutex);
        S(h)->log.push_back(std::string("[") + sev + "] " + (msg ? msg : ""));
    }

    // clap.thread-check
    static bool CLAP_ABI isMainThread(const clap_host_t* h) {
        return std::this_thread::get_id() == S(h)->mainThread && !tAudioThread;
    }
    static bool CLAP_ABI isAudioThread(const clap_host_t*) { return tAudioThread; }

    // clap.params
    static void CLAP_ABI paramsRescan(const clap_host_t* h, clap_param_rescan_flags flags) {
        S(h)->rescanFlags.fetch_or(flags);
        S(h)->rescanned = true;
    }
    static void CLAP_ABI paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
    static void CLAP_ABI paramsRequestFlush(const clap_host_t* h) { S(h)->flushRequested = true; }

    // clap.state
    static void CLAP_ABI stateMarkDirty(const clap_host_t* h) { S(h)->stateDirty = true; }

    // clap.latency, clap.tail
    static void CLAP_ABI latencyChanged(const clap_host_t* h) { S(h)->latencyChanged = true; }
    static void CLAP_ABI tailChanged(const clap_host_t* h) { S(h)->tailChanged = true; }

    // clap.audio-ports, clap.note-ports
    static bool CLAP_ABI audioPortsRescanSupported(const clap_host_t*, uint32_t) { return false; }
    static void CLAP_ABI audioPortsRescan(const clap_host_t* h, uint32_t) {
        S(h)->restartRequested = true;
    }
    static uint32_t CLAP_ABI noteDialects(const clap_host_t*) {
        return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    }
    static void CLAP_ABI noteRescan(const clap_host_t*, uint32_t) {}

    // clap.timer-support (main thread)
    static bool CLAP_ABI registerTimer(const clap_host_t* h, uint32_t periodMs, clap_id* id) {
        auto* s = S(h);
        const clap_id newId = s->nextTimer++;
        s->timers.push_back(
            {newId, std::max<std::uint32_t>(periodMs, 1),
             std::chrono::steady_clock::now() + std::chrono::milliseconds(periodMs)});
        *id = newId;
        return true;
    }
    static bool CLAP_ABI unregisterTimer(const clap_host_t* h, clap_id id) {
        auto& t = S(h)->timers;
        const auto before = t.size();
        t.erase(std::remove_if(t.begin(), t.end(), [&](const auto& x) { return x.id == id; }),
                t.end());
        return t.size() != before;
    }

    // com.suppleaudio.semantic-params
    static void CLAP_ABI semanticChanged(const clap_host_t* h, uint32_t) {
        S(h)->semanticChanged = true;
    }

    // Event lists
    static uint32_t CLAP_ABI inSize(const clap_input_events_t* list) {
        return static_cast<Instance::HostState*>(list->ctx)->eventCount;
    }
    static const clap_event_header_t* CLAP_ABI inGet(const clap_input_events_t* list,
                                                     uint32_t index) {
        auto* s = static_cast<Instance::HostState*>(list->ctx);
        return index < s->eventCount ? &s->events[index].header : nullptr;
    }
    static bool CLAP_ABI outTryPush(const clap_output_events_t* list,
                                    const clap_event_header_t* e) {
        auto* s = static_cast<Instance::HostState*>(list->ctx);
        OutputEvents* sink = s->outSink;
        if (!sink || e->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;  // accepted and ignored
        OutputEvent out{};
        out.time = e->time;
        switch (e->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* p = reinterpret_cast<const clap_event_param_value_t*>(e);
                out.kind = OutputEvent::Kind::ParamValue;
                out.paramId = p->param_id;
                out.value = p->value;
                break;
            }
            case CLAP_EVENT_PARAM_GESTURE_BEGIN:
            case CLAP_EVENT_PARAM_GESTURE_END: {
                const auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(e);
                out.kind = e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                               ? OutputEvent::Kind::GestureBegin
                               : OutputEvent::Kind::GestureEnd;
                out.paramId = g->param_id;
                break;
            }
            case CLAP_EVENT_NOTE_END: {
                const auto* n = reinterpret_cast<const clap_event_note_t*>(e);
                out.kind = OutputEvent::Kind::NoteEnd;
                out.noteId = n->note_id;
                break;
            }
            default:
                return true;
        }
        if (sink->count < sink->storage.size()) {
            sink->storage[sink->count++] = out;
            return true;
        }
        ++sink->dropped;
        return false;
    }
};

namespace {

const clap_host_log_t kHostLog = {HostCallbacks::log};
const clap_host_thread_check_t kHostThreadCheck = {HostCallbacks::isMainThread,
                                                   HostCallbacks::isAudioThread};
const clap_host_params_t kHostParams = {HostCallbacks::paramsRescan, HostCallbacks::paramsClear,
                                        HostCallbacks::paramsRequestFlush};
const clap_host_state_t kHostState = {HostCallbacks::stateMarkDirty};
const clap_host_latency_t kHostLatency = {HostCallbacks::latencyChanged};
const clap_host_tail_t kHostTail = {HostCallbacks::tailChanged};
const clap_host_audio_ports_t kHostAudioPorts = {HostCallbacks::audioPortsRescanSupported,
                                                 HostCallbacks::audioPortsRescan};
const clap_host_note_ports_t kHostNotePorts = {HostCallbacks::noteDialects,
                                               HostCallbacks::noteRescan};
const clap_host_timer_support_t kHostTimer = {HostCallbacks::registerTimer,
                                              HostCallbacks::unregisterTimer};
const clap_host_supple_semantic_params_t kHostSemantic = {
    sizeof(clap_host_supple_semantic_params_t), HostCallbacks::semanticChanged};

// clap_ostream / clap_istream adapters (main thread).
struct StringSink {
    std::string text;
    clap_ostream_t stream{this,
                          [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                              static_cast<StringSink*>(s->ctx)->text.append(
                                  static_cast<const char*>(buf), static_cast<std::size_t>(size));
                              return static_cast<int64_t>(size);
                          }};
};
struct ByteSink {
    std::vector<std::uint8_t> bytes;
    clap_ostream_t stream{this,
                          [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                              auto& v = static_cast<ByteSink*>(s->ctx)->bytes;
                              const auto* b = static_cast<const std::uint8_t*>(buf);
                              v.insert(v.end(), b, b + size);
                              return static_cast<int64_t>(size);
                          }};
};
struct ByteSource {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    clap_istream_t stream{this, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
                              auto* self = static_cast<ByteSource*>(s->ctx);
                              const std::size_t n =
                                  std::min<std::size_t>(size, self->bytes.size() - self->offset);
                              std::memcpy(buf, self->bytes.data() + self->offset, n);
                              self->offset += n;
                              return static_cast<int64_t>(n);
                          }};
};

Error semanticErrors(const std::string& json, std::string_view what) {
    Error e("host.semantic.rejected", std::string(what) + " was rejected by the plugin");
    auto parsed = json::parse(json);
    if (parsed && parsed->isArray()) {
        for (const auto& item : parsed->asArray()) {
            Error d;
            if (const auto* c = item.find("code"); c && c->isString()) d.code = c->asString();
            if (const auto* m = item.find("message"); m && m->isString()) d.message = m->asString();
            if (const auto* p = item.find("path"); p && p->isString()) d.path = p->asString();
            if (const auto* h = item.find("hint"); h && h->isString()) d.hint = h->asString();
            e.details.push_back(std::move(d));
        }
    }
    if (e.details.size() == 1) return e.details.front();
    return e;
}

bool hasFeature(const clap_plugin_descriptor_t* d, const char* feature) {
    if (!d->features) return false;
    for (const char* const* f = d->features; *f; ++f)
        if (std::strcmp(*f, feature) == 0) return true;
    return false;
}

}  // namespace

const void* CLAP_ABI HostCallbacks::getExtension(const clap_host_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &kHostLog;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &kHostThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kHostState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kHostLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kHostTail;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kHostAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kHostNotePorts;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kHostTimer;
    if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS)) return &kHostSemantic;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

std::vector<fs::path> defaultSearchPath() {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        if (!p.empty() && fs::exists(p, ec)) out.push_back(p);
    };
    if (const char* env = std::getenv("CLAP_PATH")) {
        std::string s(env);
        std::size_t start = 0;
        while (start <= s.size()) {
            const std::size_t end = s.find(':', start);
            add(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (const char* home = std::getenv("HOME")) add(fs::path(home) / ".clap");
    add("/usr/lib/clap");
    add("/usr/local/lib/clap");
    for (const auto& p : defaultVst3SearchPath()) add(p);
    return out;
}

Result<std::vector<PluginInfo>> scan(std::span<const fs::path> dirsOrFiles) {
    std::vector<fs::path> files;
    for (const auto& root : dirsOrFiles) {
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) {
            files.push_back(root);
        } else if (fs::is_directory(root, ec) && root.extension() != ".vst3") {
            for (auto it = fs::recursive_directory_iterator(
                     root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->path().extension() == ".clap" && it->is_regular_file(ec))
                    files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<PluginInfo> out;
    for (const auto& file : files) {
        auto lib = Library::open(file);
        if (!lib)
            continue;  // not loadable: skipped (the sandbox reports load errors on explicit load)
        const auto* factory = (*lib)->factory();
        const uint32_t n = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < n; ++i) {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (!d || !d->id) continue;
            PluginInfo info;
            info.id = d->id;
            info.name = d->name ? d->name : "";
            info.vendor = d->vendor ? d->vendor : "";
            info.version = d->version ? d->version : "";
            info.description = d->description ? d->description : "";
            info.path = file;
            if (d->features)
                for (const char* const* f = d->features; *f; ++f) info.features.emplace_back(*f);
            info.instrument = hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT);
            // Semantic support needs an instance to ask.
            auto probe = Instance::create(info, HostIdentity{});
            if (probe) info.semantic = (*probe)->info().semantic;
            out.push_back(std::move(info));
        }
    }
    auto vst3 = scanVst3(dirsOrFiles);
    if (vst3)
        out.insert(out.end(), std::make_move_iterator(vst3->begin()),
                   std::make_move_iterator(vst3->end()));
    return out;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Instance>> Instance::create(const PluginInfo& info,
                                                   const HostIdentity& identity, bool isolated,
                                                   std::shared_ptr<BridgeProcess> bridge) {
    if (isolated) {
        RemotePorts ports;
        auto remote = RemoteInstance::spawn(info, identity, ports, std::move(bridge));
        if (!remote) return std::move(remote).error();
        std::unique_ptr<Instance> self(new Instance());
        self->info_ = info;
        self->info_.semantic = ports.semantic;
        self->inChannels_ = ports.inChannels;
        self->outChannels_ = ports.outChannels;
        self->acceptsNotes_ = ports.acceptsNotes;
        self->hostState_ = std::make_unique<HostState>();
        self->remote_ = std::move(*remote);
        return self;
    }
    if (info.format == "vst3" || info.id.rfind("vst3:", 0) == 0) {
        auto backing = Vst3Instance::create(info, identity);
        if (!backing) return std::move(backing).error();
        std::unique_ptr<Instance> self(new Instance());
        self->info_ = info;
        self->info_.format = "vst3";
        self->info_.semantic = false;
        self->inChannels_ = (*backing)->inputChannels();
        self->outChannels_ = (*backing)->outputChannels();
        self->acceptsNotes_ = (*backing)->acceptsNotes();
        self->vst3_ = std::move(*backing);
        return self;
    }
    auto lib = Library::open(info.path);
    if (!lib) return std::move(lib).error();

    std::unique_ptr<Instance> self(new Instance());
    self->info_ = info;
    self->library_ = *lib;
    self->hostState_ = std::make_unique<HostState>();
    HostState& h = *self->hostState_;
    h.owner = self.get();
    h.name = identity.name;
    h.vendor = identity.vendor;
    h.url = identity.url;
    h.version = identity.version;
    h.host.clap_version = CLAP_VERSION;
    h.host.host_data = &h;
    h.host.name = h.name.c_str();
    h.host.vendor = h.vendor.c_str();
    h.host.url = h.url.c_str();
    h.host.version = h.version.c_str();
    h.host.get_extension = HostCallbacks::getExtension;
    h.host.request_restart = HostCallbacks::requestRestart;
    h.host.request_process = HostCallbacks::requestProcess;
    h.host.request_callback = HostCallbacks::requestCallback;
    h.inEvents = {&h, HostCallbacks::inSize, HostCallbacks::inGet};
    h.outEvents = {&h, HostCallbacks::outTryPush};

    const auto* factory = self->library_->factory();
    self->plugin_ = factory->create_plugin(factory, &h.host, info.id.c_str());
    if (!self->plugin_)
        return Error("host.create", "the factory could not create \"" + info.id + "\"", {},
                     "check the plugin id with plugin.list");
    if (!self->plugin_->init(self->plugin_)) {
        self->plugin_->destroy(self->plugin_);
        self->plugin_ = nullptr;
        return Error("host.create", "\"" + info.id + "\" failed to initialize");
    }
    auto ext = [&](const char* id) { return self->plugin_->get_extension(self->plugin_, id); };
    self->extParams_ = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
    self->extState_ = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
    self->extLatency_ = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
    self->extTail_ = static_cast<const clap_plugin_tail_t*>(ext(CLAP_EXT_TAIL));
    self->extTimer_ = static_cast<const clap_plugin_timer_support_t*>(ext(CLAP_EXT_TIMER_SUPPORT));
    self->extSemantic_ = static_cast<const clap_plugin_supple_semantic_params_t*>(
        ext(CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (self->extSemantic_ &&
        self->extSemantic_->struct_size < sizeof(clap_plugin_supple_semantic_params_t))
        self->extSemantic_ = nullptr;  // too old to call safely
    self->info_.semantic = self->extSemantic_ != nullptr;

    // Port configuration: prefer stereo main in/out (or stereo out for instruments).
    if (auto* cfg = static_cast<const clap_plugin_audio_ports_config_t*>(
            ext(CLAP_EXT_AUDIO_PORTS_CONFIG))) {
        const uint32_t n = cfg->count(self->plugin_);
        for (uint32_t i = 0; i < n; ++i) {
            clap_audio_ports_config_t c{};
            if (!cfg->get(self->plugin_, i, &c)) continue;
            const bool stereoOut = c.has_main_output && c.main_output_channel_count == 2;
            const bool stereoIn = !c.has_main_input || c.main_input_channel_count == 2;
            if (stereoOut && stereoIn) {
                cfg->select(self->plugin_, c.id);
                break;
            }
        }
    }
    if (auto* ports = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS))) {
        auto mainChannels = [&](bool input) -> uint32_t {
            const uint32_t n = ports->count(self->plugin_, input);
            for (uint32_t i = 0; i < n; ++i) {
                clap_audio_port_info_t p{};
                if (ports->get(self->plugin_, i, input, &p) && (p.flags & CLAP_AUDIO_PORT_IS_MAIN))
                    return p.channel_count;
            }
            clap_audio_port_info_t p{};
            return n > 0 && ports->get(self->plugin_, 0, input, &p) ? p.channel_count : 0;
        };
        self->inChannels_ = mainChannels(true);
        self->outChannels_ = mainChannels(false);
    }
    if (auto* notes = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS))) {
        if (notes->count(self->plugin_, true) > 0) {
            self->acceptsNotes_ = true;
            clap_note_port_info_t np{};
            if (notes->get(self->plugin_, 0, true, &np))
                h.preferMidiNotes = !(np.supported_dialects & CLAP_NOTE_DIALECT_CLAP) &&
                                    (np.supported_dialects & CLAP_NOTE_DIALECT_MIDI);
        }
    }
    return self;
}

Instance::~Instance() {
    if (vst3_) {
        if (processing_) stopProcessing();
        if (active_) deactivate();
        return;
    }
    if (remote_) {
        if (processing_) stopProcessing();
        if (active_) deactivate();
        remote_.reset();
        return;
    }
    if (!plugin_) return;
    // Destroying a processing instance is a caller bug (the audio thread must
    // stop processing first); recover by stopping on this thread.
    if (processing_) stopProcessing();
    if (active_) deactivate();
    plugin_->destroy(plugin_);
    plugin_ = nullptr;
}

Status Instance::activate(double sampleRate, std::uint32_t minFrames, std::uint32_t maxFrames) {
    if (vst3_) {
        SUPPLE_TRY(vst3_->activate(sampleRate, maxFrames));
        // What the plug-in settled on (a bus it adapted): the engine sizes its
        // channels from this and refuses a plan it cannot serve.
        inChannels_ = vst3_->inputChannels();
        outChannels_ = vst3_->outputChannels();
        active_ = true;
        sampleRate_ = sampleRate;
        maxFrames_ = maxFrames;
        return okStatus();
    }
    if (remote_) {
        if (active_) return okStatus();
        SUPPLE_TRY(remote_->activate(sampleRate, minFrames, maxFrames, inChannels_, outChannels_));
        remote_->refreshLatency();
        active_ = true;
        sampleRate_ = sampleRate;
        maxFrames_ = maxFrames;
        return okStatus();
    }
    if (active_) return okStatus();
    hostState_->events.assign(kMaxInputEvents, ClapEvent{});
    if (!plugin_->activate(plugin_, sampleRate, minFrames, maxFrames))
        return Error("host.activate",
                     "\"" + info_.id + "\" refused to activate at " +
                         std::to_string(static_cast<int>(sampleRate)) + " Hz",
                     {}, "try another sample rate or block size");
    active_ = true;
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    return okStatus();
}

void Instance::deactivate() {
    if (vst3_) {
        if (active_ && !processing_) {
            vst3_->deactivate();
            active_ = false;
        }
        return;
    }
    if (remote_ && active_ && !processing_) {
        remote_->deactivate();
        active_ = false;
        return;
    }
    if (!active_) return;
    if (processing_) {
        // CLAP forbids deactivating while processing: the audio thread must
        // call stopProcessing() (and be joined) first.
        std::lock_guard lock(hostState_->logMutex);
        hostState_->log.emplace_back("host: deactivate() ignored while processing");
        return;
    }
    plugin_->deactivate(plugin_);
    active_ = false;
}

std::uint32_t Instance::latency() const {
    if (vst3_) return vst3_->latency();
    if (remote_) return active_ ? remote_->latency() : 0;
    return active_ && extLatency_ ? extLatency_->get(plugin_) : 0;
}

std::uint32_t Instance::tail() const {
    if (vst3_) return vst3_->tail();
    if (remote_) return remote_->tail();
    return extTail_ ? extTail_->get(plugin_) : 0;
}

bool Instance::reportsTail() const {
    if (vst3_) return true;
    return remote_ ? remote_->reportsTail() : extTail_ != nullptr;
}

bool Instance::failed() const { return remote_ && remote_->failed(); }

int Instance::bridgePid() const { return remote_ ? remote_->bridgePid() : -1; }

std::string Instance::failure() const { return remote_ ? remote_->failure() : std::string(); }

std::vector<ParamInfo> Instance::params() const {
    if (vst3_) return vst3_->params();
    if (remote_) return remote_->params();
    std::vector<ParamInfo> out;
    if (!extParams_) return out;
    const uint32_t n = extParams_->count(plugin_);
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t p{};
        if (!extParams_->get_info(plugin_, i, &p)) continue;
        out.push_back({p.id, p.name, p.module, p.min_value, p.max_value, p.default_value, p.flags});
    }
    return out;
}

Result<double> Instance::paramValue(std::uint32_t id) const {
    if (vst3_) return vst3_->paramValue(id);
    if (remote_) return remote_->paramValue(id);
    double v = 0;
    if (!extParams_ || !extParams_->get_value(plugin_, id, &v))
        return Error("host.param", "no parameter with id " + std::to_string(id));
    return v;
}

Result<std::string> Instance::paramText(std::uint32_t id, double clapValue) const {
    if (vst3_) return vst3_->paramText(id, clapValue);
    if (remote_) return remote_->paramText(id, clapValue);
    char buf[256] = {};
    if (!extParams_ || !extParams_->value_to_text(plugin_, id, clapValue, buf, sizeof(buf)))
        return Error("host.param", "the plugin has no text for parameter " + std::to_string(id));
    return std::string(buf);
}

Result<double> Instance::paramFromText(std::uint32_t id, std::string_view text) const {
    if (vst3_) return vst3_->paramFromText(id, text);
    if (remote_) return remote_->paramFromText(id, text);
    double v = 0;
    const std::string s(text);
    if (!extParams_ || !extParams_->text_to_value(plugin_, id, s.c_str(), &v))
        return Error("host.param_text",
                     "the plugin could not read \"" + s + "\" for parameter " + std::to_string(id),
                     {}, "use the plugin's own value text (param.get shows it)");
    return v;
}

bool Instance::paramsNormalized() const { return info_.format == "vst3"; }

Result<double> Instance::normalizedParam(std::uint32_t id, double plain) const {
    if (vst3_) return vst3_->normalizedParam(id, plain);
    if (remote_) return remote_->normalizedParam(id, plain);
    return plain;  // CLAP: the host value is the value the plug-in is given
}

Result<double> Instance::plainParam(std::uint32_t id, double normalized) const {
    if (vst3_) return vst3_->plainParam(id, normalized);
    if (remote_) return remote_->plainParam(id, normalized);
    return normalized;
}

Status Instance::setParam(std::uint32_t id, double clapValue) {
    if (vst3_) return vst3_->setParam(id, clapValue);
    if (remote_) return remote_->setParam(id, clapValue);
    if (!extParams_) return Error("host.param", "the plugin has no parameters");
    if (!active_) {
        // Inactive: flush on the main thread.
        HostState& h = *hostState_;
        h.eventCount = 1;
        auto& e = h.events.empty() ? (h.events.assign(1, ClapEvent{}), h.events[0]) : h.events[0];
        e.param = {};
        e.param.header = {sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                          CLAP_EVENT_PARAM_VALUE, 0};
        e.param.param_id = id;
        e.param.value = clapValue;
        e.param.note_id = -1;
        e.param.port_index = -1;
        e.param.channel = -1;
        e.param.key = -1;
        h.outSink = nullptr;
        extParams_->flush(plugin_, &h.inEvents, &h.outEvents);
        h.eventCount = 0;
        return okStatus();
    }
    if (!hostState_->paramQueue.push({id, clapValue}))
        return Error("host.param_queue", "too many queued parameter changes", {},
                     "render or flush first");
    return okStatus();
}

namespace {
bool appendParam(Instance::HostState& h, std::uint32_t time, std::uint32_t id,
                 double value) SUPPLE_RT {
    if (h.eventCount >= h.events.size()) return false;
    auto& e = h.events[h.eventCount++].param;
    e = {};
    e.header = {sizeof(clap_event_param_value_t), time, CLAP_CORE_EVENT_SPACE_ID,
                CLAP_EVENT_PARAM_VALUE, 0};
    e.param_id = id;
    e.value = value;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    return true;
}

// Moves queued setParam() changes into the arena, leaving `reserve` slots
// free; what does not fit stays queued for the next block/flush.
void drainParamQueue(Instance::HostState& h, std::size_t reserve) SUPPLE_RT {
    const std::size_t limit = h.events.size() > reserve ? h.events.size() - reserve : 0;
    ParamChange pc{};
    while (h.eventCount < limit && h.paramQueue.peek(pc)) {
        appendParam(h, 0, pc.id, pc.value);
        h.paramQueue.consume();
    }
}
}  // namespace

void Instance::flushParams() SUPPLE_RT {
    if (vst3_) {
        vst3_->flushParams();
        return;
    }
    if (remote_) {
        if (active_ && !processing_) remote_->flush();
        return;
    }
    AudioThreadScope scope;
    HostState& h = *hostState_;
    h.eventCount = 0;
    drainParamQueue(h, 0);
    const bool requested = h.flushRequested.exchange(false);
    if ((h.eventCount || requested) && extParams_) {
        h.outSink = nullptr;
        extParams_->flush(plugin_, &h.inEvents, &h.outEvents);
    }
    h.eventCount = 0;
}

bool Instance::startProcessing() SUPPLE_RT {
    if (vst3_) {
        if (processing_) return true;
        processing_ = active_ && vst3_->startProcessing();
        return processing_;
    }
    if (remote_) {
        if (processing_) return true;
        if (!active_) return false;
        processing_ = remote_->startProcessing();
        return processing_;
    }
    if (processing_) return true;
    if (!active_) return false;
    AudioThreadScope scope;
    processing_ = plugin_->start_processing(plugin_);
    return processing_;
}

void Instance::stopProcessing() SUPPLE_RT {
    if (vst3_) {
        if (processing_) vst3_->stopProcessing();
        processing_ = false;
        return;
    }
    if (remote_) {
        if (processing_) remote_->stopProcessing();
        processing_ = false;
        return;
    }
    if (!processing_) return;
    AudioThreadScope scope;
    plugin_->stop_processing(plugin_);
    processing_ = false;
}

void Instance::reset() SUPPLE_RT {
    if (vst3_) {
        vst3_->reset();
        return;
    }
    if (remote_) {
        if (active_) remote_->reset();
        return;
    }
    AudioThreadScope scope;  // CLAP: [audio-thread & active]
    if (active_) plugin_->reset(plugin_);
}

bool Instance::process(const ProcessBlock& b, OutputEvents* out) SUPPLE_RT {
    if (vst3_) return active_ && processing_ && vst3_->process(b, out);
    if (remote_) return active_ && processing_ && remote_->process(b, out);
    if (!active_ || !processing_) return false;
    AudioThreadScope scope;
    HostState& h = *hostState_;
    h.eventCount = 0;
    h.flushRequested.store(false);  // a process call is a flush
    drainParamQueue(h, b.events.size());
    for (const Event& e : b.events) {
        if (h.eventCount >= h.events.size()) {
            h.droppedEvents.fetch_add(1);
            continue;
        }
        const std::uint32_t t = std::min(e.time, b.frames ? b.frames - 1 : 0u);
        switch (e.kind) {
            case Event::Kind::Param:
                appendParam(h, t, e.paramId, e.value);
                break;
            case Event::Kind::NoteOn:
            case Event::Kind::NoteOff:
            case Event::Kind::NoteChoke: {
                ClapEvent& ce = h.events[h.eventCount++];
                if (h.preferMidiNotes) {  // MIDI-only port: a choke becomes a note-off
                    ce.midi = {};
                    ce.midi.header = {sizeof(clap_event_midi_t), t, CLAP_CORE_EVENT_SPACE_ID,
                                      CLAP_EVENT_MIDI, 0};
                    ce.midi.port_index = static_cast<uint16_t>(e.port);
                    const bool on = e.kind == Event::Kind::NoteOn;
                    const auto status =
                        static_cast<std::uint8_t>((on ? 0x90 : 0x80) | (e.channel & 0x0f));
                    ce.midi.data[0] = status;
                    ce.midi.data[1] = static_cast<std::uint8_t>(e.key & 0x7f);
                    ce.midi.data[2] = e.kind == Event::Kind::NoteChoke
                                          ? 0
                                          : static_cast<std::uint8_t>(
                                                std::clamp(e.velocity * 127.0 + 0.5, 0.0, 127.0));
                } else {
                    ce.note = {};
                    const uint16_t type = e.kind == Event::Kind::NoteOn    ? CLAP_EVENT_NOTE_ON
                                          : e.kind == Event::Kind::NoteOff ? CLAP_EVENT_NOTE_OFF
                                                                           : CLAP_EVENT_NOTE_CHOKE;
                    ce.note.header = {sizeof(clap_event_note_t), t, CLAP_CORE_EVENT_SPACE_ID, type,
                                      0};
                    ce.note.note_id = e.noteId;
                    ce.note.port_index = e.port;
                    ce.note.channel = e.channel;
                    ce.note.key = e.key;
                    ce.note.velocity = e.velocity;
                }
                break;
            }
            case Event::Kind::Midi: {
                ClapEvent& ce = h.events[h.eventCount++];
                ce.midi = {};
                ce.midi.header = {sizeof(clap_event_midi_t), t, CLAP_CORE_EVENT_SPACE_ID,
                                  CLAP_EVENT_MIDI, 0};
                ce.midi.port_index = static_cast<uint16_t>(e.port);
                std::memcpy(ce.midi.data, e.midi, 3);
                break;
            }
        }
    }

    clap_audio_buffer_t in{}, outBuf{};
    in.data32 = const_cast<float**>(b.inputs);
    in.channel_count = b.inputChannels;
    outBuf.data32 = const_cast<float**>(b.outputs);
    outBuf.channel_count = b.outputChannels;

    clap_process_t p{};
    p.steady_time = b.steadyTime;
    p.frames_count = b.frames;
    if (b.transport) {
        clap_event_transport_t& tr = h.transport;
        tr = {};
        tr.header = {sizeof(clap_event_transport_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                     CLAP_EVENT_TRANSPORT, 0};
        tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                   CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE |
                   (b.transport->playing ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u);
        tr.song_pos_beats =
            static_cast<clap_beattime>(std::llround(b.transport->beat * CLAP_BEATTIME_FACTOR));
        tr.song_pos_seconds =
            static_cast<clap_sectime>(std::llround(b.transport->seconds * CLAP_SECTIME_FACTOR));
        tr.tempo = b.transport->tempo;
        tr.bar_start = static_cast<clap_beattime>(
            std::llround(b.transport->barStartBeat * CLAP_BEATTIME_FACTOR));
        tr.bar_number = b.transport->bar;
        tr.tsig_num = b.transport->numerator;
        tr.tsig_denom = b.transport->denominator;
        if (b.transport->loopActive) {
            tr.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
            tr.loop_start_beats = static_cast<clap_beattime>(
                std::llround(b.transport->loopStartBeat * CLAP_BEATTIME_FACTOR));
            tr.loop_end_beats = static_cast<clap_beattime>(
                std::llround(b.transport->loopEndBeat * CLAP_BEATTIME_FACTOR));
            tr.loop_start_seconds = static_cast<clap_sectime>(
                std::llround(b.transport->loopStartSeconds * CLAP_SECTIME_FACTOR));
            tr.loop_end_seconds = static_cast<clap_sectime>(
                std::llround(b.transport->loopEndSeconds * CLAP_SECTIME_FACTOR));
        }
        p.transport = &tr;
    }
    p.audio_inputs = b.inputChannels ? &in : nullptr;
    p.audio_inputs_count = b.inputChannels ? 1 : 0;
    p.audio_outputs = b.outputChannels ? &outBuf : nullptr;
    p.audio_outputs_count = b.outputChannels ? 1 : 0;
    p.in_events = &h.inEvents;
    p.out_events = &h.outEvents;
    h.outSink = out;
    const clap_process_status status = plugin_->process(plugin_, &p);
    h.outSink = nullptr;
    h.eventCount = 0;
    return status != CLAP_PROCESS_ERROR;
}

Result<json::Value> Instance::semanticSchema() {
    if (remote_) {
        auto text = remote_->text(bridge::Op::SemanticSchema);
        if (!text) return std::move(text).error();
        return json::parse(*text);
    }
    if (!extSemantic_)
        return Error("host.semantic.unsupported", info_.id + " has no semantic-params extension");
    StringSink sink;
    if (!extSemantic_->get_schema(plugin_, &sink.stream))
        return Error("host.semantic", "get_schema failed");
    return json::parse(sink.text);
}

Result<std::string> Instance::getPatch(bool changedOnly) {
    if (remote_) return remote_->text(bridge::Op::GetPatch, changedOnly);
    if (!extSemantic_)
        return Error("host.semantic.unsupported", info_.id + " has no semantic-params extension");
    StringSink sink;
    if (!extSemantic_->get_patch(
            plugin_, changedOnly ? CLAP_SUPPLE_PATCH_DETAIL_CHANGED : CLAP_SUPPLE_PATCH_DETAIL_FULL,
            &sink.stream))
        return Error("host.semantic", "get_patch failed");
    return sink.text;
}

Status Instance::setPatch(std::string_view text, std::uint32_t mode) {
    if (remote_) return remote_->setPatch(text, mode);
    if (!extSemantic_)
        return Error("host.semantic.unsupported", info_.id + " has no semantic-params extension");
    StringSink errors;
    if (!extSemantic_->set_patch(plugin_, mode, text.data(), static_cast<uint32_t>(text.size()),
                                 &errors.stream))
        return semanticErrors(errors.text,
                              mode == CLAP_SUPPLE_SET_VALIDATE_ONLY ? "the patch" : "the patch");
    return okStatus();
}

Result<std::string> Instance::describe(bool changedOnly) {
    if (remote_) return remote_->text(bridge::Op::Describe, changedOnly);
    if (!extSemantic_)
        return Error("host.semantic.unsupported", info_.id + " has no semantic-params extension");
    StringSink sink;
    if (!extSemantic_->describe(
            plugin_, changedOnly ? CLAP_SUPPLE_PATCH_DETAIL_CHANGED : CLAP_SUPPLE_PATCH_DETAIL_FULL,
            &sink.stream))
        return Error("host.semantic", "describe failed");
    return sink.text;
}

Result<std::vector<std::uint8_t>> Instance::saveState() {
    if (vst3_) return vst3_->saveState();
    if (remote_) return remote_->saveState();
    if (!extState_) return Error("host.state", info_.id + " has no state extension");
    ByteSink sink;
    if (!extState_->save(plugin_, &sink.stream))
        return Error("host.state", info_.id + " failed to save its state");
    return std::move(sink.bytes);
}

Status Instance::loadState(std::span<const std::uint8_t> bytes) {
    if (vst3_) return vst3_->loadState(bytes);
    if (remote_) return remote_->loadState(bytes);
    if (!extState_) return Error("host.state", info_.id + " has no state extension");
    ByteSource source{bytes};
    if (!extState_->load(plugin_, &source.stream))
        return Error("host.state", info_.id + " rejected the state", {},
                     "the state may belong to another version");
    return okStatus();
}

bool Instance::needsAudioFlush() const {
    if (vst3_) return false;
    if (remote_) return active_ && remote_->needsAudioFlush();
    return active_ && hostState_->flushRequested.load();
}

bool Instance::serviceMainThread() {
    if (vst3_) return vst3_->serviceMainThread();
    if (remote_) return remote_->service(remoteNotes_, remoteLog_);
    HostState& h = *hostState_;
    bool did = false;
    if (h.callbackRequested.exchange(false)) {
        plugin_->on_main_thread(plugin_);
        did = true;
    }
    // A restart raised while processing stays pending until processing stops.
    if (!active_) {
        h.restartRequested = false;  // the next activation picks up the change
    } else if (!processing_ && h.restartRequested.exchange(false)) {
        const double sr = sampleRate_;
        const std::uint32_t maxFrames = maxFrames_;
        deactivate();
        (void)activate(sr, 1, maxFrames);
        did = true;
    }
    // While active, a requested flush is served by the next process() or
    // flushParams() on the audio thread (needsAudioFlush()).
    if (!active_ && extParams_ && h.flushRequested.exchange(false)) {
        h.eventCount = 0;
        h.outSink = nullptr;
        extParams_->flush(plugin_, &h.inEvents, &h.outEvents);
        did = true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (extTimer_) {
        for (auto& t : h.timers) {
            if (now >= t.next) {
                extTimer_->on_timer(plugin_, t.id);
                t.next = now + std::chrono::milliseconds(t.periodMs);
                did = true;
            }
        }
    }
    return did;
}

Instance::Notifications Instance::takeNotifications() {
    if (vst3_) return vst3_->takeNotifications();
    if (remote_) return std::exchange(remoteNotes_, Notifications{});
    HostState& h = *hostState_;
    Notifications n;
    n.stateDirty = h.stateDirty.exchange(false);
    n.latencyChanged = h.latencyChanged.exchange(false);
    n.semanticChanged = h.semanticChanged.exchange(false);
    n.tailChanged = h.tailChanged.exchange(false);
    n.droppedEvents = h.droppedEvents.exchange(0);
    n.paramsRescan = h.rescanned.exchange(false);
    n.paramsRescanFlags = h.rescanFlags.exchange(0);
    return n;
}

std::vector<std::string> Instance::takeLog() {
    if (vst3_) return {};
    if (remote_) return std::exchange(remoteLog_, {});
    HostState& h = *hostState_;
    std::vector<std::string> out;
    LogLine line{};
    while (h.audioLog.pop(line)) out.emplace_back(line.text);
    std::lock_guard lock(h.logMutex);
    out.insert(out.end(), h.log.begin(), h.log.end());
    h.log.clear();
    return out;
}

}  // namespace supple::host
