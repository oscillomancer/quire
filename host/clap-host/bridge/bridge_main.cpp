// <slug>-bridge: hosts isolated fx instances (ADR-0013).
// Started by the host with its end of the control socket on fd 3 (or
// --control-fd N). Main-thread requests arrive on the socket; audio-thread
// commands arrive in per-instance shared memory sent with Activate and run on
// that instance's audio thread. Exits when the host closes the control socket.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/bridge_protocol.h"
#include "../src/scan.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"

using namespace supple;
using namespace supple::host;
using namespace supple::host::bridge;

namespace {

struct BridgeInstance {
    std::unique_ptr<Instance> instance;
    void* map = nullptr;
    std::size_t mapSize = 0;
    Header* hdr = nullptr;
    Layout layout{0, 0, 0};
    std::thread audio;
    std::atomic<bool> quit{false};
    // Serializes the plugin's lifecycle: the main thread's servicing (which
    // may restart the plugin: deactivate + activate) against the audio
    // thread's start/stop/reset/flush commands. process() takes no lock: it
    // only runs between start and stop, when no restart can happen.
    std::mutex lifecycle;

    // --- the audio thread ---------------------------------------------------------------

    // `last`: the request sequence when Activate was handled — taken on the
    // main thread before its reply, so a command the host sends right after
    // is never mistaken for one already served, however late this starts.
    void audioLoop(std::uint32_t last) {
        auto* base = static_cast<std::uint8_t*>(map);
        auto* events = reinterpret_cast<const Event*>(base + layout.events);
        auto* outEvents = reinterpret_cast<OutputEvent*>(base + layout.outEvents);
        auto* inAudio = reinterpret_cast<float*>(base + layout.inAudio);
        auto* outAudio = reinterpret_cast<float*>(base + layout.outAudio);
        auto* keyAudio = reinterpret_cast<float*>(base + layout.keyAudio);
        const float* in[kMaxChannels];
        const float* key[kMaxChannels];
        float* out[kMaxChannels];
        for (std::uint32_t c = 0; c < layout.keyChannels; ++c)
            key[c] = keyAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        for (std::uint32_t c = 0; c < layout.inChannels; ++c)
            in[c] = inAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        for (std::uint32_t c = 0; c < layout.outChannels; ++c)
            out[c] = outAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        while (!quit.load()) {
            const std::uint32_t req = hdr->request.load(std::memory_order_acquire);
            if (req == last) {
                futexWait(hdr->request, last, 100);
                continue;
            }
            last = req;
            std::uint32_t result = 1;
            switch (static_cast<Cmd>(hdr->cmd)) {
                case Cmd::Process: {
                    ProcessBlock b;
                    b.inputs = layout.inChannels ? in : nullptr;
                    b.inputChannels = layout.inChannels;
                    b.outputs = out;
                    b.outputChannels = layout.outChannels;
                    if (hdr->hasKey && layout.keyChannels) {
                        b.sidechain = key;
                        b.sidechainChannels = layout.keyChannels;
                    }
                    b.frames = std::min(hdr->frames, layout.maxFrames);
                    b.events =
                        std::span<const Event>(events, std::min(hdr->eventCount, kMaxEvents));
                    b.transport = hdr->hasTransport ? &hdr->transport : nullptr;
                    b.steadyTime = hdr->steadyTime;
                    OutputEvents sink{std::span<OutputEvent>(outEvents, kMaxOutEvents)};
                    result = instance->process(b, &sink) ? 1 : 0;
                    hdr->outEventCount = static_cast<std::uint32_t>(sink.count);
                    hdr->outEventsDropped = static_cast<std::uint32_t>(sink.dropped);
                    break;
                }
                case Cmd::StartProcessing: {
                    std::lock_guard lock(lifecycle);
                    result = instance->startProcessing() ? 1 : 0;
                    break;
                }
                case Cmd::StopProcessing: {
                    std::lock_guard lock(lifecycle);
                    instance->stopProcessing();
                    break;
                }
                case Cmd::Reset: {
                    std::lock_guard lock(lifecycle);
                    instance->reset();
                    break;
                }
                case Cmd::Flush: {
                    std::lock_guard lock(lifecycle);
                    instance->flushParams();
                    break;
                }
                case Cmd::Quit:
                    quit = true;
                    break;
            }
            hdr->result = result;
            hdr->processing = instance->processing() ? 1 : 0;
            hdr->done.store(req, std::memory_order_release);
            futexWake(hdr->done);
        }
    }

    void stopAudio() {
        if (!audio.joinable()) return;
        quit = true;
        audio.join();
        quit = false;
    }

    void unmap() {
        if (map) ::munmap(map, mapSize);
        map = nullptr;
        hdr = nullptr;
    }
};

struct Bridge {
    int sock = kControlFd;
    std::map<std::uint32_t, std::unique_ptr<BridgeInstance>> instances;
    std::uint32_t nextId = 1;
    std::uint32_t replyId = 0;

    // --- the control plane ---------------------------------------------------------------

    bool reply(const Writer& w) {
        return sendFrame(sock, static_cast<std::uint32_t>(Reply::Ok), replyId, w.bytes);
    }
    bool fail(const Error& e) {
        Writer w;
        w.error(e);
        return sendFrame(sock, static_cast<std::uint32_t>(Reply::Error), replyId, w.bytes);
    }
    template <class T>
    bool replyOr(const Result<T>& r, auto&& write) {
        if (!r) return fail(r.error());
        Writer w;
        write(w, *r);
        return reply(w);
    }
    bool replyStatus(const supple::Status& st) { return st ? reply(Writer{}) : fail(st.error()); }
    static void writeSize(Writer& w, const Instance::EditorSize& size) {
        w.u32(size.width);
        w.u32(size.height);
        w.u8(size.resizable ? 1 : 0);
    }

    // Returns false to exit.
    bool handle(Op op, std::uint32_t instanceId, Reader& r, int fd) {
        replyId = instanceId;
        if (!validRequest(op, r)) {
            if (fd >= 0) ::close(fd);
            return fail(Error("bridge.malformed", "malformed request to the plugin bridge"));
        }
        if (fd >= 0 && op != Op::Activate) {
            ::close(fd);
            return fail(Error("bridge.fd", "unexpected descriptor for this operation"));
        }
        if (op == Op::Create) {
            if (instanceId != 0)
                return fail(Error("bridge.instance", "Create requires instance id 0"));
            PluginInfo info;
            info.path = r.str();
            info.id = r.str();
            info.format = r.str();
            HostIdentity identity;
            identity.name = r.str();
            identity.vendor = r.str();
            identity.url = r.str();
            identity.version = r.str();
            if (!r.ok()) return fail(Error("bridge.create", "malformed Create request"));
            auto made = Instance::create(info, identity);
            if (!made) return fail(made.error());
            auto slot = std::make_unique<BridgeInstance>();
            slot->instance = std::move(*made);
            const std::uint32_t newId = nextId++;
            Writer w;
            w.u32(newId);
            w.u32(slot->instance->audioInputChannels());
            w.u32(slot->instance->audioOutputChannels());
            w.u8(slot->instance->acceptsNotes() ? 1 : 0);
            w.u8(slot->instance->info().semantic ? 1 : 0);
            w.u32(slot->instance->sidechainChannels());
            w.u8(slot->instance->hasEditor() ? 1 : 0);
            instances.emplace(newId, std::move(slot));
            return reply(w);
        }
        auto it = instances.find(instanceId);
        if (it == instances.end()) {
            if (fd >= 0) ::close(fd);
            return fail(Error("bridge.no_instance", "unknown instance id"));
        }
        BridgeInstance& slot = *it->second;
        auto& instance = slot.instance;
        auto& map = slot.map;
        auto& mapSize = slot.mapSize;
        auto& hdr = slot.hdr;
        auto& layout = slot.layout;
        auto& audio = slot.audio;
        auto& lifecycle = slot.lifecycle;
        auto stopAudio = [&] { slot.stopAudio(); };
        auto unmap = [&] { slot.unmap(); };
        switch (op) {
            case Op::Create:
                break;
            case Op::Activate: {
                const double rate = r.f64();
                const std::uint32_t minFrames = r.u32(), maxFrames = r.u32(), size = r.u32();
                if (fd < 0) return fail(Error("bridge.activate", "no shared memory"));
                struct stat backing{};
                if (!r.ok() || size < sizeof(Header) || ::fstat(fd, &backing) != 0 ||
                    backing.st_size < static_cast<off_t>(size)) {
                    ::close(fd);
                    return fail(Error("bridge.activate", "invalid shared memory size or request"));
                }
                stopAudio();
                unmap();
                map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                ::close(fd);
                if (map == MAP_FAILED) {
                    map = nullptr;
                    return fail(Error("bridge.activate", "could not map the shared memory"));
                }
                mapSize = size;
                hdr = static_cast<Header*>(map);
                layout =
                    Layout(hdr->maxFrames, hdr->inChannels, hdr->outChannels, hdr->keyChannels);
                if (hdr->magic != kMagic || hdr->version != kVersion || layout.size > size ||
                    hdr->maxFrames != maxFrames || maxFrames == 0 ||
                    hdr->inChannels > kMaxChannels || hdr->outChannels > kMaxChannels ||
                    hdr->keyChannels > kMaxChannels) {
                    unmap();
                    return fail(Error("bridge.activate", "shared memory layout mismatch"));
                }
                (void)instance->setSidechainActive(hdr->keyActive != 0);
                auto st = instance->activate(rate, minFrames, maxFrames);
                if (!st) {
                    unmap();
                    return fail(st.error());
                }
                const std::uint32_t first = hdr->request.load(std::memory_order_acquire);
                audio = std::thread([&slot, first] { slot.audioLoop(first); });
                return reply(Writer{});
            }
            case Op::Deactivate:
                stopAudio();
                instance->deactivate();
                unmap();
                return reply(Writer{});
            case Op::Params: {
                const auto params = instance->params();
                Writer w;
                w.u32(static_cast<std::uint32_t>(params.size()));
                for (const ParamInfo& p : params) {
                    w.u32(p.id);
                    w.str(p.name);
                    w.str(p.module);
                    w.f64(p.min);
                    w.f64(p.max);
                    w.f64(p.def);
                    w.u32(p.flags);
                }
                return reply(w);
            }
            case Op::ParamValue:
                return replyOr(instance->paramValue(r.u32()),
                               [](Writer& w, double v) { w.f64(v); });
            case Op::ParamText: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->paramText(id, r.f64()),
                               [](Writer& w, const std::string& s) { w.str(s); });
            }
            case Op::ParamFromText: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->paramFromText(id, r.str()),
                               [](Writer& w, double v) { w.f64(v); });
            }
            case Op::SetParam: {
                const std::uint32_t id = r.u32();
                return replyStatus(instance->setParam(id, r.f64()));
            }
            case Op::NormalizedParam: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->normalizedParam(id, r.f64()),
                               [](Writer& w, double v) { w.f64(v); });
            }
            case Op::PlainParam: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->plainParam(id, r.f64()),
                               [](Writer& w, double v) { w.f64(v); });
            }
            case Op::Latency: {
                Writer w;
                w.u32(instance->latency());
                w.u32(instance->tail());
                w.u8(instance->reportsTail() ? 1 : 0);
                return reply(w);
            }
            case Op::SemanticSchema:
                return replyOr(instance->semanticSchema(), [](Writer& w, const json::Value& v) {
                    w.str(json::writeCompact(v));
                });
            case Op::GetPatch:
                return replyOr(instance->getPatch(r.u8() != 0),
                               [](Writer& w, const std::string& s) { w.str(s); });
            case Op::SetPatch: {
                const std::string text = r.str();
                return replyStatus(instance->setPatch(text, r.u32()));
            }
            case Op::Describe:
                return replyOr(instance->describe(r.u8() != 0),
                               [](Writer& w, const std::string& s) { w.str(s); });
            case Op::SaveState:
                return replyOr(instance->saveState(),
                               [](Writer& w, const std::vector<std::uint8_t>& b) { w.blob(b); });
            case Op::TakeIdleOutputs: {
                const auto out = instance->takeIdleOutputs();
                Writer w;
                w.u64(out.dropped);
                w.u32(static_cast<std::uint32_t>(out.events.size()));
                for (const auto& event : out.events) writeOutputEvent(w, event);
                return reply(w);
            }
            case Op::LoadState: {
                const auto bytes = r.blob();
                return replyStatus(instance->loadState(bytes));
            }
            case Op::Service: {
                bool did = false;
                {
                    std::lock_guard lock(lifecycle);
                    did = instance->serviceMainThread();
                }
                const auto n = instance->takeNotifications();
                const auto log = instance->takeLog();
                Writer w;
                w.u8(did ? 1 : 0);
                w.u8(n.stateDirty ? 1 : 0);
                w.u8(n.paramsRescan ? 1 : 0);
                w.u8(n.latencyChanged ? 1 : 0);
                w.u8(n.semanticChanged ? 1 : 0);
                w.u8(n.tailChanged ? 1 : 0);
                w.u32(n.paramsRescanFlags);
                w.u8(instance->takeProcessRequest() ? 1 : 0);
                if (n.paramsClear.size() > kMaxParamClears)
                    return fail(Error("bridge.notifications_limit", "too many parameter clears"));
                w.u32(static_cast<std::uint32_t>(n.paramsClear.size()));
                for (const auto& clear : n.paramsClear) {
                    w.u32(clear.id);
                    w.u32(clear.flags);
                }
                w.u32(n.droppedEvents);
                w.u8(instance->needsAudioFlush() ? 1 : 0);
                w.u32(static_cast<std::uint32_t>(log.size()));
                for (const auto& line : log) w.str(line);
                w.u32(instance->latency());  // a restart may have changed them silently
                w.u32(instance->tail());
                w.u8(instance->reportsTail() ? 1 : 0);
                w.u8(n.restartError ? 1 : 0);
                if (n.restartError) w.error(*n.restartError);
                return reply(w);
            }
            case Op::EditorOpen: {
                const auto parent = static_cast<unsigned long>(r.u64());
                const double scale = r.f64();
                if (!r.ok()) return fail(Error("bridge.editor", "malformed EditorOpen request"));
                return replyOr(instance->openEditor(parent, scale), writeSize);
            }
            case Op::EditorResize: {
                const std::uint32_t width = r.u32(), height = r.u32();
                return replyOr(instance->resizeEditor(width, height), writeSize);
            }
            case Op::EditorClose:
                instance->closeEditor();
                return reply(Writer{});
            case Op::EditorRequests: {
                const auto q = instance->takeEditorRequests();
                Writer w;
                w.u8(q.resize ? 1 : 0);
                w.u8(q.closed ? 1 : 0);
                w.u32(q.width);
                w.u32(q.height);
                w.u8(q.show ? 1 : 0);
                w.u8(q.hide ? 1 : 0);
                return reply(w);
            }
            case Op::Destroy:
                stopAudio();
                instance->closeEditor();
                if (instance->processing()) instance->stopProcessing();
                instance.reset();
                unmap();
                reply(Writer{});
                instances.erase(it);
                return true;  // an idle pool-owned bridge must accept the next Create
        }
        return fail(Error("bridge.op", "unknown request"));
    }
};

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    if (argc == 3 && std::string_view(argv[1]) == "--scan") {
        // Keep plugin printf/cout (including background threads and unload
        // destructors) away from the protocol stream. Only our saved fd writes it.
        const int output = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (output < 0 || null < 0 || ::dup2(null, STDOUT_FILENO) < 0) {
            if (output >= 0) ::close(output);
            if (null >= 0) ::close(null);
            return 1;
        }
        ::close(null);
        std::error_code ec;
        const auto path = std::filesystem::absolute(argv[2], ec).lexically_normal();
        const Result<std::vector<PluginInfo>> result =
            ec ? Result<std::vector<PluginInfo>>(
                     Error("host.scan.load", "cannot resolve plugin path"))
               : scanModuleInProcess(path);
        const auto text = json::writeCanonical(scanReply(result));
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto n = ::write(output, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ::close(output);
                return 1;
            }
            offset += static_cast<std::size_t>(n);
        }
        ::close(output);
        return result ? 0 : 1;
    }
    Bridge b;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--control-fd") b.sock = std::atoi(argv[i + 1]);
    std::vector<pollfd> polled;
    std::vector<std::uint32_t> owners;  // the instance of each editor descriptor
    for (;;) {
        // The control socket, and the descriptors open editors watch
        // (clap.posix-fd-support / the VST3 run loop) with their timers.
        polled.assign(1, pollfd{b.sock, POLLIN, 0});
        owners.assign(1, 0);
        int timeout = 10;
        for (auto& [id, slot] : b.instances) {
            if (!slot->instance->editorOpen()) continue;
            for (const auto& [fd, flags] : slot->instance->watchedFds()) {
                short events = 0;
                if (flags & CLAP_POSIX_FD_READ) events |= POLLIN;
                if (flags & CLAP_POSIX_FD_WRITE) events |= POLLOUT;
                polled.push_back(pollfd{fd, events, 0});
                owners.push_back(id);
            }
            const int due = slot->instance->nextTimerMs();
            if (due >= 0) timeout = std::min(timeout, due);
        }
        const int ready = ::poll(polled.data(), polled.size(), timeout);
        for (std::size_t i = 1; ready > 0 && i < polled.size(); ++i) {
            if (!polled[i].revents) continue;
            const auto it = b.instances.find(owners[i]);
            if (it == b.instances.end()) continue;
            std::uint32_t flags = 0;
            if (polled[i].revents & POLLIN) flags |= CLAP_POSIX_FD_READ;
            if (polled[i].revents & POLLOUT) flags |= CLAP_POSIX_FD_WRITE;
            if (polled[i].revents & (POLLERR | POLLHUP)) flags |= CLAP_POSIX_FD_ERROR;
            std::lock_guard lock(it->second->lifecycle);
            it->second->instance->onFd(polled[i].fd, flags);
        }
        if (!(polled[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            // idle: the plugin's own main-thread work (timers, callbacks)
            for (auto& [id, slot] : b.instances) {
                (void)id;
                std::lock_guard lock(slot->lifecycle);
                (void)slot->instance->serviceMainThread();
            }
            continue;
        }
        std::uint32_t op = 0;
        std::uint32_t id = 0;
        std::vector<std::uint8_t> payload;
        int fd = -1;
        if (!recvFrame(b.sock, op, id, payload, &fd)) break;  // the host is gone
        Reader r(payload.data(), payload.size());
        if (!b.handle(static_cast<Op>(op), id, r, fd)) break;
    }
    for (auto& [id, slot] : b.instances) {
        (void)id;
        slot->stopAudio();
        if (slot->instance) slot->instance->closeEditor();
        if (slot->instance && slot->instance->processing()) slot->instance->stopProcessing();
        slot->instance.reset();
        slot->unmap();
    }
    return 0;
}
