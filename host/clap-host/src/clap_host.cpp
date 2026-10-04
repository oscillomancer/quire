#include "supple/host/clap_host.h"

#include "bridge_client.h"
#include "pending_edits.h"
#include "scan.h"
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
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(path, ec);
        if (ec)
            return Error("host.load", "cannot resolve " + path.string() + ": " + ec.message(), {},
                         "check the plugin path and its symlinks");
        const std::string key = canonical.string();
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
    std::atomic<bool> rescanned{false}, sleeping{false};
    std::vector<Notifications::ParamClear> paramsClear;  // main thread only

    struct Timer {
        clap_id id;
        std::uint32_t periodMs;
        std::chrono::steady_clock::time_point next;
    };
    std::vector<Timer> timers;  // main thread only
    clap_id nextTimer = 1;

    // clap.gui requests ([thread-safe]: any thread may call them).
    std::atomic<bool> guiResize{false}, guiClosed{false}, guiShow{false}, guiHide{false};
    std::atomic<std::uint64_t> guiSize{0};
    std::optional<Error> restartError;  // main thread only
    // clap.posix-fd-support (main thread).
    std::vector<std::pair<int, std::uint32_t>> fds;

    SpscRing<LogLine, 256> audioLog;  // plugin audio thread -> main
    std::mutex logMutex;              // any other thread
    std::vector<std::string> log;

    PendingEdits paramQueue;  // main -> audio

    // Event conversion arena, sized at activation (never resized on the audio thread).
    std::vector<ClapEvent> events;
    std::uint32_t eventCount = 0;
    OutputEvents* outSink = nullptr;
    // Only one flush producer runs at a time. Inactive flushes produce on
    // main; activation/deactivation hand production to/from the audio thread.
    // The main thread is always the consumer; it never resets the ring.
    SpscRing<OutputEvent, IdleOutputs::kCapacity> idleOutputs;
    std::atomic<std::uint64_t> idleDropped{0};
    bool idleFlush = false;
    clap_input_events_t inEvents{};
    clap_output_events_t outEvents{};
    clap_event_transport_t transport{};
    bool preferMidiNotes = false;

    // Every declared audio port (ADR-0018), in declaration order: the main
    // ports carry the block's audio, the key port its sidechain, the rest
    // silence (inputs) or scratch (outputs). Pointer arrays and spare
    // channels are allocated at activation; empty = no audio-ports extension.
    std::vector<std::uint32_t> inPorts, outPorts;  // channel counts
    int mainIn = -1, keyIn = -1, mainOut = -1;
    std::vector<clap_audio_buffer_t> inBuffers, outBuffers;
    std::vector<std::vector<float>> spare;       // zeroed or scratch channels
    std::vector<float*> pointers;                // per port, per channel
    std::vector<std::size_t> inFirst, outFirst;  // each port's first pointer
    std::vector<std::size_t> inSpare, outSpare;  // each port's first spare channel
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
        S(h)->paramsClear.clear();  // a later rescan re-enables earlier clears
        S(h)->rescanFlags.fetch_or(flags);
        S(h)->rescanned = true;
    }
    static void CLAP_ABI paramsClear(const clap_host_t* h, clap_id id,
                                     clap_param_clear_flags flags) {
        S(h)->paramsClear.push_back({id, flags});
    }
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

    // clap.gui (the editor lives in a window the host owns)
    static void CLAP_ABI guiResizeHintsChanged(const clap_host_t*) {}
    static bool CLAP_ABI guiRequestResize(const clap_host_t* h, uint32_t width, uint32_t height) {
        auto* s = S(h);
        s->guiSize.store((std::uint64_t(width) << 32) | height, std::memory_order_release);
        s->guiResize = true;
        return true;  // applied by the window owner on its next loop pass
    }
    static bool CLAP_ABI guiRequestShow(const clap_host_t* h) {
        S(h)->guiShow = true;
        return true;
    }
    static bool CLAP_ABI guiRequestHide(const clap_host_t* h) {
        S(h)->guiHide = true;
        return true;
    }
    static void CLAP_ABI guiClosed(const clap_host_t* h, bool) { S(h)->guiClosed = true; }

    // clap.posix-fd-support (main thread)
    static bool CLAP_ABI registerFd(const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        auto& fds = S(h)->fds;
        for (auto& [f, fl] : fds)
            if (f == fd) return false;
        fds.emplace_back(fd, flags);
        return true;
    }
    static bool CLAP_ABI modifyFd(const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        for (auto& [f, fl] : S(h)->fds)
            if (f == fd) {
                fl = flags;
                return true;
            }
        return false;
    }
    static bool CLAP_ABI unregisterFd(const clap_host_t* h, int fd) {
        auto& fds = S(h)->fds;
        const auto before = fds.size();
        std::erase_if(fds, [&](const auto& x) { return x.first == fd; });
        return fds.size() != before;
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
        if ((!sink && !s->idleFlush) || e->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
        OutputEvent out{};
        out.time = s->idleFlush ? 0 : e->time;
        out.flags = e->flags;
        switch (e->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* p = reinterpret_cast<const clap_event_param_value_t*>(e);
                out.kind = OutputEvent::Kind::ParamValue;
                out.paramId = p->param_id;
                out.value = p->value;
                out.noteId = p->note_id;
                out.port = p->port_index;
                out.channel = p->channel;
                out.key = p->key;
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
                out.port = n->port_index;
                out.channel = n->channel;
                out.key = n->key;
                break;
            }
            default:
                return true;
        }
        if (s->idleFlush) {
            if (s->idleOutputs.push(out)) return true;
            s->idleDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
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
const clap_host_gui_t kHostGui = {HostCallbacks::guiResizeHintsChanged,
                                  HostCallbacks::guiRequestResize, HostCallbacks::guiRequestShow,
                                  HostCallbacks::guiRequestHide, HostCallbacks::guiClosed};
const clap_host_posix_fd_support_t kHostFd = {HostCallbacks::registerFd, HostCallbacks::modifyFd,
                                              HostCallbacks::unregisterFd};
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
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kHostGui;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kHostFd;
    if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS)) return &kHostSemantic;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

std::optional<fs::path> stockPluginDir() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
    const fs::path dir = exe.parent_path().parent_path().parent_path() / "plugins" / "clap";
    if (!fs::is_directory(dir, ec)) return std::nullopt;
    return fs::weakly_canonical(dir, ec);
}

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

void orderCatalogue(std::vector<PluginInfo>& list) {
    // Stable: scan() returns plugins in search-path order, which is the priority.
    std::stable_sort(list.begin(), list.end(),
                     [](const PluginInfo& a, const PluginInfo& b) { return a.id < b.id; });
}

bool shadowed(std::span<const PluginInfo> catalogue, std::size_t index) {
    return index > 0 && index < catalogue.size() && catalogue[index - 1].id == catalogue[index].id;
}

Result<std::vector<PluginInfo>> scanModuleInProcess(const fs::path& file) {
    std::error_code ec;
    if (fs::is_directory(file, ec)) return scanVst3Module(file);
    std::vector<PluginInfo> out;
    {
        auto lib = Library::open(file);
        if (!lib) return lib.error();
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
        self->keyChannels_ = ports.keyChannels;
        self->acceptsNotes_ = ports.acceptsNotes;
        self->remoteEditor_ = ports.editor;
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
        self->keyChannels_ = (*backing)->sidechainChannels();
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
    self->extGui_ = static_cast<const clap_plugin_gui_t*>(ext(CLAP_EXT_GUI));
    self->extFd_ =
        static_cast<const clap_plugin_posix_fd_support_t*>(ext(CLAP_EXT_POSIX_FD_SUPPORT));
    self->extSemantic_ = static_cast<const clap_plugin_supple_semantic_params_t*>(
        ext(CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (self->extSemantic_ && self->extSemantic_->struct_size <
                                  offsetof(clap_plugin_supple_semantic_params_t, get_telemetry))
        self->extSemantic_ = nullptr;  // too old to call safely
    self->info_.semantic = self->extSemantic_ != nullptr;
    if (self->extSemantic_) (void)self->semanticSchema();

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
        // Every port, and which is the key: the first input that is not the main one.
        for (bool input : {true, false}) {
            auto& list = input ? h.inPorts : h.outPorts;
            int& main = input ? h.mainIn : h.mainOut;
            const uint32_t n = ports->count(self->plugin_, input);
            if (n > 16)
                return Error("host.layout", "CLAP layout exceeds 16 audio ports", {},
                             "select a layout with at most 16 ports per direction");
            for (uint32_t i = 0; i < n; ++i) {
                clap_audio_port_info_t p{};
                const bool ok = ports->get(self->plugin_, i, input, &p);
                if (ok && p.channel_count > 64)
                    return Error("host.layout", "CLAP port exceeds 64 channels", {},
                                 "select a layout with at most 64 channels per port");
                list.push_back(ok ? p.channel_count : 0);
                if (ok && main < 0 && (p.flags & CLAP_AUDIO_PORT_IS_MAIN))
                    main = static_cast<int>(i);
            }
            if (main < 0 && !list.empty()) main = 0;
        }
        for (std::size_t i = 0; i < h.inPorts.size(); ++i)
            if (static_cast<int>(i) != h.mainIn && h.inPorts[i] > 0) {
                h.keyIn = static_cast<int>(i);
                self->keyChannels_ = h.inPorts[i];
                break;
            }
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
    closeEditor();  // the editor goes before the plugin
    // Destroying a processing instance is a caller bug (the audio thread must
    // stop processing first); recover by stopping on this thread.
    if (processing_) stopProcessing();
    if (active_) deactivate();
    plugin_->destroy(plugin_);
    plugin_ = nullptr;
}

Status Instance::setSidechainActive(bool on) {
    if (active_)
        return Error("host.sidechain", "the key can change only while the plug-in is inactive");
    keyActive_ = on && keyChannels_ > 0;
    return okStatus();
}

Status Instance::activate(double sampleRate, std::uint32_t minFrames, std::uint32_t maxFrames) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || minFrames == 0 || minFrames > maxFrames)
        return Error("host.activate", "invalid sample rate or block-size range", {},
                     "use a finite positive rate and 0 < minFrames <= maxFrames");
    if (vst3_) {
        SUPPLE_TRY(vst3_->activate(sampleRate, maxFrames, keyActive_));
        // What the plug-in settled on (a bus it adapted): the engine sizes its
        // channels from this and refuses a plan it cannot serve.
        inChannels_ = vst3_->inputChannels();
        outChannels_ = vst3_->outputChannels();
        keyChannels_ = vst3_->sidechainChannels();
        active_ = true;
        sampleRate_ = sampleRate;
        maxFrames_ = maxFrames;
        return okStatus();
    }
    if (remote_) {
        if (active_) return okStatus();
        SUPPLE_TRY(remote_->activate(sampleRate, minFrames, maxFrames, inChannels_, outChannels_,
                                     keyChannels_, keyActive_));
        remote_->refreshLatency();
        active_ = true;
        sampleRate_ = sampleRate;
        maxFrames_ = maxFrames;
        return okStatus();
    }
    if (active_) return okStatus();
    hostState_->events.assign(kMaxInputEvents, ClapEvent{});
    {
        HostState& h = *hostState_;
        h.inBuffers.assign(h.inPorts.size(), clap_audio_buffer_t{});
        h.outBuffers.assign(h.outPorts.size(), clap_audio_buffer_t{});
        std::size_t channels = 0;
        for (auto c : h.inPorts) channels += c;
        for (auto c : h.outPorts) channels += c;
        h.spare.assign(channels, std::vector<float>(maxFrames, 0.0f));
        h.pointers.assign(channels, nullptr);
        h.inFirst.clear();
        h.outFirst.clear();
        std::size_t at = 0;
        for (auto c : h.inPorts) {
            h.inFirst.push_back(at);
            at += c;
        }
        for (auto c : h.outPorts) {
            h.outFirst.push_back(at);
            at += c;
        }
    }
    if (!plugin_->activate(plugin_, sampleRate, minFrames, maxFrames))
        return Error(
            "host.activate",
            "\"" + info_.id + "\" refused to activate at " + std::to_string(sampleRate) + " Hz", {},
            "try another sample rate or block size");
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

namespace {
void flushIdleOutputs(Instance::HostState& h, const clap_plugin_t* plugin,
                      const clap_plugin_params_t* params) {
    h.outSink = nullptr;
    h.idleFlush = true;
    params->flush(plugin, &h.inEvents, &h.outEvents);
    h.idleFlush = false;
}

void flushInactiveEdits(Instance::HostState& h, const clap_plugin_t* plugin,
                        const clap_plugin_params_t* params) {
    const auto edits = h.paramQueue.pending();
    if (edits.empty() || !params) return;
    if (h.events.size() < edits.size()) h.events.resize(edits.size());
    h.eventCount = 0;
    for (const auto& edit : edits) {
        auto& e = h.events[h.eventCount++].param;
        e = {};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = edit.id;
        e.value = edit.value;
        e.note_id = e.port_index = e.channel = e.key = -1;
    }
    flushIdleOutputs(h, plugin, params);
    h.eventCount = 0;
    h.paramQueue.barrier();
}
}  // namespace

Status Instance::setParam(std::uint32_t id, double clapValue) {
    if (vst3_) return vst3_->setParam(id, clapValue);
    if (remote_) return remote_->setParam(id, clapValue);
    if (!extParams_) return Error("host.param", "the plugin has no parameters");
    if (!active_) {
        // Inactive: flush on the main thread.
        flushInactiveEdits(*hostState_, plugin_, extParams_);
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
        flushIdleOutputs(h, plugin_, extParams_);
        h.eventCount = 0;
        return okStatus();
    }
    if (!hostState_->paramQueue.push(id, clapValue))
        return Error("host.param_queue", "too many queued parameter changes", {},
                     "render or flush first");
    hostState_->processRequested.store(true, std::memory_order_release);
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
    PendingEdits::Change pc{};
    while (h.eventCount < limit && h.paramQueue.pop(pc)) {
        appendParam(h, 0, pc.id, pc.value);
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
        flushIdleOutputs(h, plugin_, extParams_);
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
    if (b.frames == 0 || b.frames > maxFrames_) return false;
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
    // Declared ports: each gets a full pointer array (block channels where the
    // block has them, else the spare channels: zeroed inputs, scratch outputs).
    const bool allPorts = !h.inPorts.empty() || !h.outPorts.empty();
    if (allPorts) {
        for (std::size_t i = 0; i < h.inPorts.size(); ++i) {
            const float* const* src = nullptr;
            std::uint32_t have = 0;
            if (static_cast<int>(i) == h.mainIn) {
                src = b.inputs;
                have = b.inputChannels;
            } else if (static_cast<int>(i) == h.keyIn) {
                src = b.sidechain;
                have = b.sidechainChannels;
            }
            std::uint64_t constant = 0;
            float** ptrs = h.pointers.data() + h.inFirst[i];
            for (std::uint32_t c = 0; c < h.inPorts[i]; ++c) {
                if (src && c < have) {
                    ptrs[c] = const_cast<float*>(src[c]);
                } else {
                    ptrs[c] = h.spare[h.inFirst[i] + c].data();
                    constant |= std::uint64_t{1} << (c & 63);
                }
            }
            h.inBuffers[i] = {};
            h.inBuffers[i].data32 = ptrs;
            h.inBuffers[i].channel_count = h.inPorts[i];
            h.inBuffers[i].constant_mask = constant;
        }
        for (std::size_t i = 0; i < h.outPorts.size(); ++i) {
            const bool main = static_cast<int>(i) == h.mainOut;
            float** ptrs = h.pointers.data() + h.outFirst[i];
            for (std::uint32_t c = 0; c < h.outPorts[i]; ++c)
                ptrs[c] =
                    main && c < b.outputChannels ? b.outputs[c] : h.spare[h.outFirst[i] + c].data();
            h.outBuffers[i] = {};
            h.outBuffers[i].data32 = ptrs;
            h.outBuffers[i].channel_count = h.outPorts[i];
        }
    }

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
    if (allPorts) {
        p.audio_inputs = h.inBuffers.empty() ? nullptr : h.inBuffers.data();
        p.audio_inputs_count = static_cast<uint32_t>(h.inBuffers.size());
        p.audio_outputs = h.outBuffers.empty() ? nullptr : h.outBuffers.data();
        p.audio_outputs_count = static_cast<uint32_t>(h.outBuffers.size());
    } else {  // no audio-ports extension: one stereo pair each way
        p.audio_inputs = b.inputChannels ? &in : nullptr;
        p.audio_inputs_count = b.inputChannels ? 1 : 0;
        p.audio_outputs = b.outputChannels ? &outBuf : nullptr;
        p.audio_outputs_count = b.outputChannels ? 1 : 0;
    }
    p.in_events = &h.inEvents;
    p.out_events = &h.outEvents;
    h.outSink = out;
    const clap_process_status status = plugin_->process(plugin_, &p);
    h.outSink = nullptr;
    h.eventCount = 0;
    h.sleeping.store(status == CLAP_PROCESS_SLEEP, std::memory_order_relaxed);
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
    auto schema = json::parse(sink.text);
    auto valid = [](const json::Value& document) {
        const auto* entries = document.find("params");
        if (!document.isObject() || !entries || !entries->isObject()) return false;
        std::set<std::uint32_t> ids;
        for (const auto& [key, entry] : entries->asObject()) {
            const auto* id = entry.find("id");
            const auto* name = entry.find("name");
            const auto* type = entry.find("type");
            if (key.empty() || !entry.isObject() || !id || !id->isNumber() ||
                !std::isfinite(id->asDouble()) || id->asDouble() < 0 ||
                id->asDouble() > UINT32_MAX || std::trunc(id->asDouble()) != id->asDouble() ||
                !ids.insert(static_cast<std::uint32_t>(id->asDouble())).second || !name ||
                !name->isString() || name->asString().empty() || !type || !type->isString())
                return false;
            const auto& kind = type->asString();
            if (kind == "float" || kind == "int") {
                const auto* min = entry.find("min");
                const auto* max = entry.find("max");
                if (!min || !max || !min->isNumber() || !max->isNumber() ||
                    !std::isfinite(min->asDouble()) || !std::isfinite(max->asDouble()) ||
                    min->asDouble() > max->asDouble())
                    return false;
                if (kind == "int" && (std::trunc(min->asDouble()) != min->asDouble() ||
                                      std::trunc(max->asDouble()) != max->asDouble()))
                    return false;
            } else if (kind == "enum") {
                const auto* choices = entry.find("choices");
                if (!choices || !choices->isArray() || choices->size() == 0) return false;
                std::set<std::string> seen;
                for (const auto& choice : choices->asArray())
                    if (!choice.isString() || choice.asString().empty() ||
                        !seen.insert(choice.asString()).second)
                        return false;
            } else if (kind != "bool")
                return false;
        }
        return true;
    };
    if (schema && valid(*schema)) return schema;
    info_.semantic = false;
    extSemantic_ = nullptr;
    std::lock_guard logLock(hostState_->logMutex);
    hostState_->log.push_back("warning: invalid semantic schema for " + info_.id +
                              "; using plain CLAP parameters");
    return Error("host.semantic.unsupported", "invalid semantic schema for " + info_.id, {},
                 "use plain CLAP parameters");
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
    if (!active_) flushInactiveEdits(*hostState_, plugin_, extParams_);
    ByteSink sink;
    if (!extState_->save(plugin_, &sink.stream))
        return Error("host.state", info_.id + " failed to save its state");
    return std::move(sink.bytes);
}

Status Instance::loadState(std::span<const std::uint8_t> bytes) {
    if (vst3_) return vst3_->loadState(bytes);
    if (remote_) return remote_->loadState(bytes);
    if (!extState_) return Error("host.state", info_.id + " has no state extension");
    hostState_->paramQueue.barrier();
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
        auto result = activate(sr, 1, maxFrames);
        if (!result) h.restartError = result.error();
        did = true;
    }
    // While active, a requested flush is served by the next process() or
    // flushParams() on the audio thread (needsAudioFlush()).
    if (!active_ && extParams_ && h.flushRequested.exchange(false)) {
        h.eventCount = 0;
        flushIdleOutputs(h, plugin_, extParams_);
        did = true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (extTimer_) {
        const auto timers = h.timers;
        for (const auto& t : timers) {
            if (now >= t.next) {
                auto live = std::find_if(h.timers.begin(), h.timers.end(),
                                         [&](const auto& timer) { return timer.id == t.id; });
                if (live == h.timers.end()) continue;
                live->next = now + std::chrono::milliseconds(t.periodMs);
                extTimer_->on_timer(plugin_, t.id);
                did = true;
            }
        }
    }
    return did;
}

namespace {
// A bridged editor's requests are asked for this often (a round trip each).
constexpr int kRemoteEditorPollMs = 30;
std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

bool Instance::hasEditor() const {
    if (vst3_) return vst3_->hasEditor();
    if (remote_) return remoteEditor_ && !remote_->failed();
    return plugin_ && !remote_ && extGui_ &&
           extGui_->is_api_supported(plugin_, CLAP_WINDOW_API_X11, false);
}

Result<Instance::EditorSize> Instance::openEditor(unsigned long x11Parent, double scale) {
    if (vst3_) {
        auto size = vst3_->openEditor(x11Parent, scale);
        if (size) editorOpen_ = true;
        return size;
    }
    if (!hasEditor()) return Error("host.editor", "this plugin has no embeddable X11 editor");
    if (remote_) {
        if (editorOpen_) closeEditor();
        auto size = remote_->openEditor(x11Parent, scale);
        if (size) {
            editorOpen_ = true;
            remotePolledMs_ = steadyMs();
        }
        return size;
    }
    if (editorOpen_) closeEditor();
    if (!extGui_->create(plugin_, CLAP_WINDOW_API_X11, false))
        return Error("host.editor", "the plugin could not create its editor");
    (void)extGui_->set_scale(plugin_, scale);  // X11 plugins may handle scale themselves
    clap_window_t window{};
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = x11Parent;
    if (!extGui_->set_parent(plugin_, &window)) {
        extGui_->destroy(plugin_);
        return Error("host.editor", "the plugin refused the parent window");
    }
    EditorSize size;
    if (!extGui_->get_size(plugin_, &size.width, &size.height)) {
        extGui_->destroy(plugin_);
        return Error("host.editor", "the plugin could not report its editor size");
    }
    size.resizable = extGui_->can_resize(plugin_);
    // Cleared before show: a resize the plug-in asks for while showing stands.
    hostState_->guiClosed = false;
    hostState_->guiResize = false;
    if (!extGui_->show(plugin_)) {
        extGui_->destroy(plugin_);
        hostState_->guiResize = false;
        return Error("host.editor", "the plugin could not show its editor");
    }
    editorOpen_ = true;
    return size;
}

Result<Instance::EditorSize> Instance::resizeEditor(std::uint32_t width, std::uint32_t height) {
    if (!editorOpen_) return Error("host.editor", "no editor is open");
    if (vst3_) return vst3_->resizeEditor(width, height);
    if (remote_) return remote_->resizeEditor(width, height);
    EditorSize size{width, height, extGui_->can_resize(plugin_)};
    if (size.resizable) {
        (void)extGui_->adjust_size(plugin_, &size.width, &size.height);
        (void)extGui_->set_size(plugin_, size.width, size.height);
    }
    (void)extGui_->get_size(plugin_, &size.width, &size.height);
    return size;
}

void Instance::closeEditor() {
    if (!editorOpen_) return;
    if (vst3_) {
        vst3_->closeEditor();
        editorOpen_ = false;
        return;
    }
    if (remote_) {
        remote_->closeEditor();
        editorOpen_ = false;
        return;
    }
    (void)extGui_->hide(plugin_);
    extGui_->destroy(plugin_);
    editorOpen_ = false;
}

Instance::EditorRequests Instance::takeEditorRequests() {
    if (vst3_) return vst3_->takeEditorRequests();
    if (remote_) {
        if (!editorOpen_) return {};
        // A failed bridge answers at once (closed); otherwise not every pass.
        if (!remote_->failed() && steadyMs() - remotePolledMs_ < kRemoteEditorPollMs) return {};
        remotePolledMs_ = steadyMs();
        EditorRequests q = remote_->editorRequests();
        if (q.closed) editorOpen_ = false;  // the bridge released it
        return q;
    }
    EditorRequests r;
    if (!hostState_) return r;
    HostState& h = *hostState_;
    if (h.guiResize.exchange(false)) {
        r.resize = true;
        const auto size = h.guiSize.load(std::memory_order_acquire);
        r.width = static_cast<std::uint32_t>(size >> 32);
        r.height = static_cast<std::uint32_t>(size);
    }
    r.show = h.guiShow.exchange(false);
    r.hide = h.guiHide.exchange(false);
    if (h.guiClosed.exchange(false)) {
        r.closed = true;
        if (editorOpen_) {  // the plugin closed its own window: release it
            extGui_->destroy(plugin_);
            editorOpen_ = false;
        }
    }
    return r;
}

std::vector<std::pair<int, std::uint32_t>> Instance::watchedFds() const {
    if (vst3_) return vst3_->watchedFds();
    return hostState_ ? hostState_->fds : std::vector<std::pair<int, std::uint32_t>>{};
}

void Instance::onFd(int fd, std::uint32_t flags) {
    if (vst3_) return vst3_->onFd(fd);
    if (extFd_ && plugin_) extFd_->on_fd(plugin_, fd, flags);
}

int Instance::nextTimerMs() const {
    if (vst3_) return vst3_->nextTimerMs();
    if (remote_) {  // the bridge runs the plug-in's timers; this paces the request polls
        if (!editorOpen_) return -1;
        return static_cast<int>(
            std::max<std::int64_t>(0, kRemoteEditorPollMs - (steadyMs() - remotePolledMs_)));
    }
    if (!hostState_ || !extTimer_ || hostState_->timers.empty()) return -1;
    const auto now = std::chrono::steady_clock::now();
    auto next = hostState_->timers.front().next;
    for (const auto& t : hostState_->timers) next = std::min(next, t.next);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
    return static_cast<int>(std::max<std::int64_t>(0, ms));
}

bool Instance::processRequested() const SUPPLE_RT {
    return hostState_ && hostState_->processRequested.load(std::memory_order_acquire);
}

bool Instance::takeProcessRequest() SUPPLE_RT {
    return hostState_ && hostState_->processRequested.exchange(false, std::memory_order_acq_rel);
}

bool Instance::sleeping() const SUPPLE_RT {
    return hostState_ && hostState_->sleeping.load(std::memory_order_relaxed);
}

Instance::Notifications Instance::takeNotifications() {
    if (vst3_) return vst3_->takeNotifications();
    if (remote_) return std::exchange(remoteNotes_, Notifications{});
    HostState& h = *hostState_;
    Notifications n;
    n.restartError = std::exchange(h.restartError, {});
    n.stateDirty = h.stateDirty.exchange(false);
    n.latencyChanged = h.latencyChanged.exchange(false);
    n.semanticChanged = h.semanticChanged.exchange(false);
    n.tailChanged = h.tailChanged.exchange(false);
    n.droppedEvents = h.droppedEvents.exchange(0);
    n.paramsRescan = h.rescanned.exchange(false);
    n.paramsRescanFlags = h.rescanFlags.exchange(0);
    n.paramsClear = std::exchange(h.paramsClear, {});
    return n;
}

Instance::IdleOutputs Instance::takeIdleOutputs() {
    if (remote_) return remote_->takeIdleOutputs();
    IdleOutputs out;
    if (!hostState_) return out;
    auto& h = *hostState_;
    const auto count = h.idleOutputs.size();
    out.events.reserve(count);
    OutputEvent event;
    for (std::size_t i = 0; i < count && h.idleOutputs.pop(event); ++i) out.events.push_back(event);
    out.dropped = h.idleDropped.exchange(0);
    return out;
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
