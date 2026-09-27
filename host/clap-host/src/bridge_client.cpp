// The bridged backing of host::Instance (ADR-0013): the plugin lives in a
// bridge process; main-thread calls are control-plane requests, audio-thread
// calls are commands in shared memory with a futex wake-up and a deadline.
#include "bridge_client.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <mutex>

#include "supple/base/rt.h"

extern char** environ;

namespace supple::host {

using namespace bridge;

namespace {

std::mutex gBridgeMutex;
std::filesystem::path gBridgeExecutable;

constexpr int kCreateTimeoutMs = 20000;

}  // namespace

void setBridgeExecutable(std::filesystem::path path) {
    std::lock_guard lock(gBridgeMutex);
    gBridgeExecutable = std::move(path);
}

std::filesystem::path bridgeExecutable() {
    std::lock_guard lock(gBridgeMutex);
    return gBridgeExecutable;
}

// --- lifetime -------------------------------------------------------------------------

Result<std::shared_ptr<BridgeProcess>> BridgeProcess::spawn() {
    const std::filesystem::path exe = bridgeExecutable();
    if (exe.empty())
        return Error("host.bridge", "no plugin bridge is configured", {},
                     "setBridgeExecutable() at startup, or use in-process isolation");
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return Error("host.bridge", "socketpair failed");
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], kControlFd);  // dup2 clears CLOEXEC
    const std::string exeText = exe.string();
    const std::string fdText = std::to_string(kControlFd);
    char* argv[] = {const_cast<char*>(exeText.c_str()), const_cast<char*>("--control-fd"),
                    const_cast<char*>(fdText.c_str()), nullptr};
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, exeText.c_str(), &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(sv[1]);
    if (rc != 0) {
        ::close(sv[0]);
        return Error("host.bridge",
                     "could not start the plugin bridge " + exeText + ": " + std::strerror(rc));
    }
    std::shared_ptr<BridgeProcess> self(new BridgeProcess());
    self->pid_ = pid;
    self->sock_ = sv[0];
    self->pidfd_ = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    return self;
}

Result<std::shared_ptr<BridgeProcess>> createBridgeProcess() { return BridgeProcess::spawn(); }
bool bridgeProcessFailed(const std::shared_ptr<BridgeProcess>& process) {
    return process && (process->failed() || !process->alive());
}

Result<std::unique_ptr<RemoteInstance>> RemoteInstance::spawn(
    const PluginInfo& info, const HostIdentity& identity, RemotePorts& ports,
    std::shared_ptr<BridgeProcess> process) {
    if (!process) {
        auto made = createBridgeProcess();
        if (!made) return std::move(made).error();
        process = std::move(*made);
    }
    std::unique_ptr<RemoteInstance> self(new RemoteInstance());
    self->process_ = std::move(process);
    Writer w;
    w.str(info.path.string());
    w.str(info.id);
    w.str(identity.name);
    w.str(identity.vendor);
    w.str(identity.url);
    w.str(identity.version);
    auto reply = self->call(Op::Create, w, -1, kCreateTimeoutMs);
    if (!reply) return std::move(reply).error();
    Reader r(reply->data(), reply->size());
    self->instanceId_ = r.u32();
    ports.inChannels = r.u32();
    ports.outChannels = r.u32();
    ports.acceptsNotes = r.u8() != 0;
    ports.semantic = r.u8() != 0;
    if (!r.ok() || !self->instanceId_)
        return Error("host.bridge", "malformed reply from the plugin bridge");
    return self;
}

RemoteInstance::~RemoteInstance() {
    if (process_ && instanceId_ && !failed()) (void)call(Op::Destroy, Writer{}, -1, 2000);
    unmap();
}

BridgeProcess::~BridgeProcess() {
    if (sock_ >= 0) ::close(sock_);
    if (pid_ > 0) {
        // The bridge exits after its final Destroy or when the socket closes.
        int status = 0;
        for (int i = 0; i < 200 && ::waitpid(pid_, &status, WNOHANG) == 0; ++i) ::usleep(10000);
        if (::waitpid(pid_, &status, WNOHANG) == 0) {
            killBridge();
            ::waitpid(pid_, &status, 0);
        }
    }
    if (pidfd_ >= 0) ::close(pidfd_);
}

void RemoteInstance::unmap() {
    if (map_) ::munmap(map_, layout_.size);
    map_ = nullptr;
    hdr_ = nullptr;
}

void BridgeProcess::killBridge() {
    // Through the pidfd: never a recycled pid.
    if (pidfd_ >= 0)
        ::syscall(SYS_pidfd_send_signal, pidfd_, SIGKILL, nullptr, 0);
    else if (pid_ > 0)
        ::kill(pid_, SIGKILL);
}

// Main thread: collects the exit status of a bridge that has died.
void BridgeProcess::reap() {
    if (pid_ <= 0) return;
    int st = 0;
    if (::waitpid(pid_, &st, WNOHANG) != pid_) return;
    pid_ = -1;
    const int why = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    if (!failed_.exchange(true) || failSignal_.load() == -1) failSignal_.store(why);
}

bool BridgeProcess::alive() const {
    if (pid_ <= 0) return false;
    if (pidfd_ < 0) return ::kill(pid_, 0) == 0;
    pollfd p{pidfd_, POLLIN, 0};
    return ::poll(&p, 1, 0) == 0;  // readable once the process has exited
}

void BridgeProcess::fail(int why) {
    if (!failed_.exchange(true)) failSignal_.store(why);
}

std::string BridgeProcess::failure() const {
    if (!failed()) return {};
    const int s = failSignal_.load();
    if (s == kHung) return "the plugin stopped responding and its bridge was killed";
    if (s > 0) return std::string("the plugin bridge died (") + ::strsignal(s) + ")";
    return "the plugin bridge exited";
}

std::string RemoteInstance::failure() const { return process_->failure(); }

// --- control plane --------------------------------------------------------------------

Result<std::vector<std::uint8_t>> BridgeProcess::call(Op op, std::uint32_t instance,
                                                      const Writer& w, int fd, int timeoutMs) {
    std::lock_guard lock(requests_);
    if (failed()) {
        reap();
        return Error("host.bridge_failed", failure(), {}, "fx.restart re-creates it");
    }
    std::uint32_t status = 0;
    std::vector<std::uint8_t> reply;
    std::uint32_t returnedId = 0;
    if (!sendFrame(sock_, static_cast<std::uint32_t>(op), instance, w.bytes, fd) ||
        !recvFrame(sock_, status, returnedId, reply, nullptr, timeoutMs) ||
        returnedId != instance) {
        // Gone, or not answering: either way the instance is lost.
        if (alive()) {
            killBridge();
            fail(kHung);
        } else {
            fail(-1);
            for (int i = 0; i < 100 && pid_ > 0; ++i) {
                reap();
                if (pid_ > 0) ::usleep(1000);
            }
        }
        return Error("host.bridge_failed", failure(), {}, "fx.restart re-creates it");
    }
    if (static_cast<Reply>(status) != Reply::Ok) {
        Reader r(reply.data(), reply.size());
        return r.error();
    }
    return reply;
}

Result<std::vector<std::uint8_t>> RemoteInstance::call(Op op, const Writer& w, int fd,
                                                       int timeoutMs) {
    return process_->call(op, instanceId_, w, fd, timeoutMs);
}

Status RemoteInstance::activate(double rate, std::uint32_t minFrames, std::uint32_t maxFrames,
                                std::uint32_t in, std::uint32_t out) {
    unmap();
    layout_ = Layout(maxFrames, std::min(in, kMaxChannels), std::min(out, kMaxChannels));
    const int fd = ::memfd_create("supple-bridge", MFD_CLOEXEC);
    if (fd < 0) return Error("host.bridge", "memfd_create failed");
    if (::ftruncate(fd, static_cast<off_t>(layout_.size)) != 0) {
        ::close(fd);
        return Error("host.bridge", "could not size the shared memory");
    }
    map_ = ::mmap(nullptr, layout_.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_ == MAP_FAILED) {
        map_ = nullptr;
        ::close(fd);
        return Error("host.bridge", "could not map the shared memory");
    }
    hdr_ = new (map_) Header{};
    hdr_->magic = kMagic;
    hdr_->version = kVersion;
    hdr_->maxFrames = maxFrames;
    hdr_->inChannels = layout_.inChannels;
    hdr_->outChannels = layout_.outChannels;
    seq_ = 0;
    Writer w;
    w.f64(rate);
    w.u32(minFrames);
    w.u32(maxFrames);
    w.u32(static_cast<std::uint32_t>(layout_.size));
    auto reply = call(Op::Activate, w, fd);
    ::close(fd);  // the mapping (and the bridge's copy) keep it
    if (!reply) {
        unmap();
        return std::move(reply).error();
    }
    return okStatus();
}

void RemoteInstance::deactivate() {
    (void)call(Op::Deactivate, Writer{});
    unmap();
}

std::vector<ParamInfo> RemoteInstance::params() {
    std::vector<ParamInfo> out;
    auto reply = call(Op::Params, Writer{});
    if (!reply) return out;
    Reader r(reply->data(), reply->size());
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        ParamInfo p{};
        p.id = r.u32();
        p.name = r.str();
        p.module = r.str();
        p.min = r.f64();
        p.max = r.f64();
        p.def = r.f64();
        p.flags = r.u32();
        out.push_back(std::move(p));
    }
    return out;
}

Result<double> RemoteInstance::paramValue(std::uint32_t id) {
    Writer w;
    w.u32(id);
    auto reply = call(Op::ParamValue, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).f64();
}

Result<std::string> RemoteInstance::paramText(std::uint32_t id, double value) {
    Writer w;
    w.u32(id);
    w.f64(value);
    auto reply = call(Op::ParamText, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).str();
}

Result<double> RemoteInstance::paramFromText(std::uint32_t id, std::string_view text) {
    Writer w;
    w.u32(id);
    w.str(text);
    auto reply = call(Op::ParamFromText, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).f64();
}

Status RemoteInstance::setParam(std::uint32_t id, double value) {
    Writer w;
    w.u32(id);
    w.f64(value);
    auto reply = call(Op::SetParam, w);
    if (!reply) return std::move(reply).error();
    return okStatus();
}

void RemoteInstance::refreshLatency() {
    auto reply = call(Op::Latency, Writer{});
    if (!reply) return;
    Reader r(reply->data(), reply->size());
    latency_ = r.u32();
    tail_ = r.u32();
    reportsTail_ = r.u8() != 0;
}

Result<std::string> RemoteInstance::text(Op op, std::optional<bool> flag) {
    Writer w;
    if (flag) w.u8(*flag ? 1 : 0);
    auto reply = call(op, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).str();
}

Status RemoteInstance::setPatch(std::string_view json, std::uint32_t mode) {
    Writer w;
    w.str(json);
    w.u32(mode);
    auto reply = call(Op::SetPatch, w);
    if (!reply) return std::move(reply).error();
    return okStatus();
}

Result<std::vector<std::uint8_t>> RemoteInstance::saveState() {
    auto reply = call(Op::SaveState, Writer{});
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).blob();
}

Status RemoteInstance::loadState(std::span<const std::uint8_t> bytes) {
    Writer w;
    w.blob(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    auto reply = call(Op::LoadState, w);
    if (!reply) return std::move(reply).error();
    return okStatus();
}

bool RemoteInstance::service(Instance::Notifications& notes, std::vector<std::string>& log) {
    if (failed()) {
        process_->reap();
        return false;
    }
    auto reply = call(Op::Service, Writer{});
    if (!reply) return false;
    Reader r(reply->data(), reply->size());
    const bool didWork = r.u8() != 0;
    Instance::Notifications n;
    n.stateDirty = r.u8() != 0;
    n.paramsRescan = r.u8() != 0;
    n.latencyChanged = r.u8() != 0;
    n.semanticChanged = r.u8() != 0;
    n.tailChanged = r.u8() != 0;
    n.paramsRescanFlags = r.u32();
    n.droppedEvents = r.u32();
    needsAudioFlush_ = r.u8() != 0;
    const std::uint32_t lines = r.u32();
    for (std::uint32_t i = 0; i < lines && r.ok(); ++i) log.push_back(r.str());
    latency_ = r.u32();
    tail_ = r.u32();
    reportsTail_ = r.u8() != 0;
    notes.stateDirty |= n.stateDirty;
    notes.paramsRescan |= n.paramsRescan;
    notes.latencyChanged |= n.latencyChanged;
    notes.semanticChanged |= n.semanticChanged;
    notes.tailChanged |= n.tailChanged;
    notes.paramsRescanFlags |= n.paramsRescanFlags;
    notes.droppedEvents += n.droppedEvents;
    return didWork;
}

// --- data plane (audio thread) ----------------------------------------------------------

bool RemoteInstance::command(Cmd cmd) SUPPLE_RT {
    if (!hdr_ || process_->failed()) return false;
    // The sanctioned exception to the audio thread's no-syscall rule
    // (ADR-0013): waking the bridge and waiting for it, with a deadline.
    rt::ScopedSanctionedSyscall syscalls;
    hdr_->cmd = static_cast<std::uint32_t>(cmd);
    const std::uint32_t req = ++seq_;
    hdr_->request.store(req, std::memory_order_release);
    futexWake(hdr_->request);
    for (int spin = 0; spin < 4000; ++spin)
        if (hdr_->done.load(std::memory_order_acquire) == req) return true;
    // Lifecycle commands may page in a plugin's code; a block must not stall
    // the renderer long, but a loaded machine must not get a healthy plugin
    // killed either.
    const int limitMs = cmd == Cmd::Process ? deadlineMs_ : 5 * deadlineMs_ / 2 + 2000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limitMs);
    for (;;) {
        const std::uint32_t seen = hdr_->done.load(std::memory_order_acquire);
        if (seen == req) return true;
        futexWait(hdr_->done, seen, 10);
        if (hdr_->done.load(std::memory_order_acquire) == req) return true;
        if (!process_->alive()) {
            process_->fail(-1);  // the signal is read when the main thread reaps it
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            process_->killBridge();
            process_->fail(-2);
            return false;
        }
    }
}

bool RemoteInstance::startProcessing() SUPPLE_RT {
    if (!command(Cmd::StartProcessing)) return false;
    return hdr_->processing != 0;
}

void RemoteInstance::stopProcessing() SUPPLE_RT { command(Cmd::StopProcessing); }
void RemoteInstance::reset() SUPPLE_RT { command(Cmd::Reset); }
void RemoteInstance::flush() SUPPLE_RT { command(Cmd::Flush); }

bool RemoteInstance::process(const ProcessBlock& b, OutputEvents* out) SUPPLE_RT {
    const std::uint32_t frames = std::min(b.frames, layout_.maxFrames);
    auto silence = [&] {
        for (std::uint32_t c = 0; c < b.outputChannels; ++c)
            std::fill_n(b.outputs[c], b.frames, 0.0f);
    };
    if (!hdr_ || process_->failed()) {
        silence();  // a failed node is silent; the session plays on
        return true;
    }
    auto* base = static_cast<std::uint8_t*>(map_);
    auto* inAudio = reinterpret_cast<float*>(base + layout_.inAudio);
    auto* outAudio = reinterpret_cast<float*>(base + layout_.outAudio);
    for (std::uint32_t c = 0; c < layout_.inChannels; ++c) {
        float* dst = inAudio + static_cast<std::size_t>(c) * layout_.maxFrames;
        if (b.inputs && c < b.inputChannels)
            std::copy_n(b.inputs[c], frames, dst);
        else
            std::fill_n(dst, frames, 0.0f);
    }
    auto* events = reinterpret_cast<Event*>(base + layout_.events);
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(b.events.size(), kMaxEvents));
    std::copy_n(b.events.begin(), n, events);
    hdr_->eventCount = n;
    hdr_->frames = frames;
    hdr_->steadyTime = b.steadyTime;
    hdr_->hasTransport = b.transport ? 1 : 0;
    if (b.transport) hdr_->transport = *b.transport;
    if (!command(Cmd::Process)) {
        silence();
        return true;
    }
    for (std::uint32_t c = 0; c < b.outputChannels; ++c) {
        if (c < layout_.outChannels)
            std::copy_n(outAudio + static_cast<std::size_t>(c) * layout_.maxFrames, frames,
                        b.outputs[c]);
        else
            std::fill_n(b.outputs[c], b.frames, 0.0f);
    }
    if (out) {
        const auto* oe = reinterpret_cast<const OutputEvent*>(base + layout_.outEvents);
        for (std::uint32_t i = 0; i < hdr_->outEventCount; ++i) {
            if (out->count < out->storage.size())
                out->storage[out->count++] = oe[i];
            else
                ++out->dropped;
        }
        out->dropped += hdr_->outEventsDropped;
    }
    return hdr_->result != 0;
}

}  // namespace supple::host
